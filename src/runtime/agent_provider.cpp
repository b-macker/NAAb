// NAAb Agent Provider — Shared LLM API calling layer
// Extracted from stdlib/agent_impl.cpp for reuse by governance engine

#include "naab/agent_provider.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <stdexcept>

namespace naab {
namespace runtime {

using json = nlohmann::json;

// V-DOS-010: Maximum API response size (25 MB)
static constexpr size_t MAX_RESPONSE_BYTES = 25 * 1024 * 1024;

// Bounded write sink for curl
struct BoundedSink {
    std::string* buffer;
    size_t max_size;
};

static size_t ProviderWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* sink = static_cast<BoundedSink*>(userp);
    if (sink->buffer->size() + total > sink->max_size) return 0;
    sink->buffer->append(static_cast<char*>(contents), total);
    return total;
}

// ============================================================================
// HTTP POST with curl
// ============================================================================

static json httpPost(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& header_lines,
    long timeout_seconds = 120L) {

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Agent error: Failed to initialize HTTP client");
    }

    std::string response_body;
    long response_code = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());

    BoundedSink sink{&response_body, MAX_RESPONSE_BYTES};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ProviderWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds > 0 ? timeout_seconds : 30L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    struct curl_slist* headers = nullptr;
    for (const auto& h : header_lines) {
        headers = curl_slist_append(headers, h.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        throw std::runtime_error(
            "Agent error: Network request failed\n\n"
            "  Help:\n"
            "  - Check network connectivity\n"
            "  - The API endpoint may be unreachable\n");
    }

    curl_easy_cleanup(curl);

    json response;
    try {
        response = json::parse(response_body);
    } catch (const json::parse_error&) {
        throw std::runtime_error(
            "Agent error: Invalid response from API\n\n"
            "  Help:\n"
            "  - The API returned an unparseable response\n");
    }

    if (response_code < 200 || response_code >= 300) {
        std::string error_msg = "unknown error";
        if (response.contains("error") && response["error"].contains("message")) {
            error_msg = response["error"]["message"].get<std::string>();
        }
        if (response.contains("error") && response["error"].contains("status")) {
            error_msg = response["error"]["status"].get<std::string>();
            if (response["error"].contains("message"))
                error_msg += ": " + response["error"]["message"].get<std::string>();
        }
        if (error_msg.size() > 200) error_msg = error_msg.substr(0, 200) + "...";
        throw std::runtime_error(fmt::format(
            "Agent error: API returned status {}\n\n"
            "  Got: {}\n\n"
            "  Help:\n"
            "  - Check agent configuration in govern.json\n"
            "  - Verify the model name is valid\n",
            response_code, error_msg));
    }

    return response;
}

// ============================================================================
// Call Anthropic Messages API
// ============================================================================

static json callAnthropic(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const json& messages) {

    json request_body;
    request_body["model"] = config.model;
    request_body["max_tokens"] = config.max_tokens;
    request_body["messages"] = messages;
    if (config.temperature != 1.0)
        request_body["temperature"] = config.temperature;
    if (!config.system_prompt.empty())
        request_body["system"] = config.system_prompt;

    return httpPost(
        "https://api.anthropic.com/v1/messages",
        request_body.dump(),
        {
            "x-api-key: " + api_key,
            "anthropic-version: 2023-06-01",
            "content-type: application/json",
            "User-Agent: NAAb/1.0"
        },
        static_cast<long>(config.timeout_seconds));
}

// ============================================================================
// Call Google Gemini API
// ============================================================================

static json callGemini(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const json& messages) {

    // Convert Anthropic-style messages to Gemini format
    json contents = json::array();
    for (const auto& msg : messages) {
        json part;
        part["role"] = (msg["role"] == "assistant") ? "model" : "user";
        part["parts"] = json::array();
        json text_part;
        text_part["text"] = msg["content"];
        part["parts"].push_back(text_part);
        contents.push_back(part);
    }

    json request_body;
    request_body["contents"] = contents;

    json gen_config;
    gen_config["maxOutputTokens"] = config.max_tokens;
    if (config.temperature != 1.0)
        gen_config["temperature"] = config.temperature;
    request_body["generationConfig"] = gen_config;

    if (!config.system_prompt.empty()) {
        // Gemma models don't support systemInstruction — prepend to first user message
        bool is_gemma = config.model.find("gemma") != std::string::npos;
        if (is_gemma) {
            if (!contents.empty() && contents[0]["role"] == "user") {
                std::string original = contents[0]["parts"][0]["text"];
                contents[0]["parts"][0]["text"] = config.system_prompt + "\n\n" + original;
                request_body["contents"] = contents;
            }
        } else {
            json sys;
            sys["parts"] = json::array();
            json sys_part;
            sys_part["text"] = config.system_prompt;
            sys["parts"].push_back(sys_part);
            request_body["systemInstruction"] = sys;
        }
    }

    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/"
                    + config.model + ":generateContent?key=" + api_key;

    return httpPost(url, request_body.dump(),
        {"content-type: application/json", "User-Agent: NAAb/1.0"},
        static_cast<long>(config.timeout_seconds));
}

// ============================================================================
// Normalize response across providers
// ============================================================================

struct NormalizedResponse {
    std::string content;
    std::string stop_reason;
    int input_tokens = 0;
    int output_tokens = 0;
};

static NormalizedResponse normalizeResponse(
    const std::string& provider,
    const json& response) {

    NormalizedResponse result;

    if (provider == "gemini" || provider == "google") {
        if (response.contains("candidates") && !response["candidates"].empty()) {
            auto& candidate = response["candidates"][0];
            if (candidate.contains("content") && candidate["content"].contains("parts")) {
                for (const auto& part : candidate["content"]["parts"]) {
                    if (part.contains("text")) {
                        if (!result.content.empty()) result.content += "\n";
                        result.content += part["text"].get<std::string>();
                    }
                }
            }
            if (candidate.contains("finishReason"))
                result.stop_reason = candidate["finishReason"].get<std::string>();
        }
        if (response.contains("usageMetadata")) {
            auto& usage = response["usageMetadata"];
            if (usage.contains("promptTokenCount"))
                result.input_tokens = usage["promptTokenCount"].get<int>();
            if (usage.contains("candidatesTokenCount"))
                result.output_tokens = usage["candidatesTokenCount"].get<int>();
        }
        if (result.output_tokens == 0 && !result.content.empty()) {
            result.output_tokens = static_cast<int>(result.content.size() / 4);
        }
    } else {
        if (response.contains("content") && response["content"].is_array()) {
            for (const auto& block : response["content"]) {
                if (block.contains("type") && block["type"] == "text" && block.contains("text")) {
                    if (!result.content.empty()) result.content += "\n";
                    result.content += block["text"].get<std::string>();
                }
            }
        }
        if (response.contains("stop_reason") && response["stop_reason"].is_string())
            result.stop_reason = response["stop_reason"].get<std::string>();
        if (response.contains("usage") && response["usage"].is_object()) {
            auto& usage = response["usage"];
            if (usage.contains("input_tokens"))
                result.input_tokens = usage["input_tokens"].get<int>();
            if (usage.contains("output_tokens"))
                result.output_tokens = usage["output_tokens"].get<int>();
        }
    }

    if (result.stop_reason.empty()) result.stop_reason = "end_turn";
    return result;
}

// ============================================================================
// Route to correct provider (internal)
// ============================================================================

static json callProviderInternal(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const json& messages) {

    // Agent API calls bypass sandbox network restrictions — they are
    // explicitly authorized by the govern.json agents section (which is signed).
    // The general network: false policy applies to user code (http module,
    // polyglot network calls), not to governed agent communication.

    if (config.provider == "gemini" || config.provider == "google") {
        return callGemini(config, api_key, messages);
    }
    return callAnthropic(config, api_key, messages);
}

// ============================================================================
// Public API: Single-shot call (for agent review and simple use)
// ============================================================================

AgentResponse callAgentSimple(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const std::string& user_prompt) {

    AgentResponse result;

    try {
        json messages = json::array();
        json user_msg;
        user_msg["role"] = "user";
        user_msg["content"] = user_prompt;
        messages.push_back(user_msg);

        json response = callProviderInternal(config, api_key, messages);
        auto normalized = normalizeResponse(config.provider, response);

        result.success = true;
        result.content = normalized.content;
        result.stop_reason = normalized.stop_reason;
        result.input_tokens = normalized.input_tokens;
        result.output_tokens = normalized.output_tokens;
    } catch (const std::exception& e) {
        result.success = false;
        result.error = e.what();
    }

    return result;
}

// ============================================================================
// Public API: Multi-turn call (for agent.send() conversation management)
// ============================================================================

AgentResponse callAgentMultiTurn(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const std::string& messages_json_str) {

    AgentResponse result;

    try {
        json messages = json::parse(messages_json_str);
        json response = callProviderInternal(config, api_key, messages);
        auto normalized = normalizeResponse(config.provider, response);

        result.success = true;
        result.content = normalized.content;
        result.stop_reason = normalized.stop_reason;
        result.input_tokens = normalized.input_tokens;
        result.output_tokens = normalized.output_tokens;
    } catch (const std::exception& e) {
        result.success = false;
        result.error = e.what();
    }

    return result;
}

} // namespace runtime
} // namespace naab
