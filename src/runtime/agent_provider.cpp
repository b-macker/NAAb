// NAAb Agent Provider — Shared LLM API calling layer
// Extracted from stdlib/agent_impl.cpp for reuse by governance engine

#include "naab/agent_provider.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <stdexcept>
#include <cstdlib>
#include <fstream>

namespace naab {
namespace runtime {

using json = nlohmann::json;

// ============================================================================
// Resolve API key: env var first, then ~/.naab/keys/<varname> file fallback
// ============================================================================

std::string resolveApiKey(const std::string& env_var_name) {
    // Try environment variable first
    const char* val = std::getenv(env_var_name.c_str());
    if (val && val[0] != '\0') {
        return std::string(val);
    }

    // Fallback: read from ~/.naab/keys/<varname>
    const char* home = std::getenv("HOME");
    if (!home) return "";

    std::string key_path = std::string(home) + "/.naab/keys/" + env_var_name;
    std::ifstream ifs(key_path);
    if (!ifs.good()) return "";

    std::string key;
    std::getline(ifs, key);
    // Trim whitespace
    key.erase(0, key.find_first_not_of(" \t\r\n"));
    key.erase(key.find_last_not_of(" \t\r\n") + 1);
    return key;
}

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
// HTTP POST result (structured — does not throw)
// ============================================================================

struct HttpResult {
    json body;
    long status_code = 0;
    std::string error;       // non-empty on curl or parse failure
    std::string error_detail; // API error message (from response body)
};

static HttpResult httpPostRaw(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& header_lines,
    long timeout_seconds = 120L) {

    HttpResult result;

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "Failed to initialize HTTP client";
        return result;
    }

    std::string response_body;

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

    // On Termux (Android) the system curl CA bundle is not at the standard path.
    const char* ca_env = std::getenv("SSL_CERT_FILE");
    if (!ca_env) ca_env = std::getenv("CURL_CA_BUNDLE");
    if (ca_env && ca_env[0]) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_env);
    } else {
        curl_easy_setopt(curl, CURLOPT_CAINFO,
            "/data/data/com.termux/files/usr/etc/tls/cert.pem");
    }

    struct curl_slist* headers = nullptr;
    for (const auto& h : header_lines) {
        headers = curl_slist_append(headers, h.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status_code);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        result.error = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return result;
    }

    curl_easy_cleanup(curl);

    try {
        result.body = json::parse(response_body);
    } catch (const json::parse_error&) {
        result.error = "unparseable_response";
        return result;
    }

    // Extract error detail from non-2xx responses
    if (result.status_code < 200 || result.status_code >= 300) {
        std::string error_msg = "unknown error";
        if (result.body.contains("error") && result.body["error"].contains("message")) {
            error_msg = result.body["error"]["message"].get<std::string>();
        }
        if (result.body.contains("error") && result.body["error"].contains("status")) {
            error_msg = result.body["error"]["status"].get<std::string>();
            if (result.body["error"].contains("message"))
                error_msg += ": " + result.body["error"]["message"].get<std::string>();
        }
        if (error_msg.size() > 200) error_msg = error_msg.substr(0, 200) + "...";
        result.error_detail = error_msg;
    }

    return result;
}

// Throwing wrapper for backward compat — used by callProviderInternal
static json httpPost(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& header_lines,
    long timeout_seconds = 120L) {

    auto result = httpPostRaw(url, body, header_lines, timeout_seconds);

    if (!result.error.empty() && result.status_code == 0) {
        throw std::runtime_error(fmt::format(
            "Agent error: Network request failed ({})\n\n"
            "  Help:\n"
            "  - Check network connectivity\n"
            "  - The API endpoint may be unreachable\n",
            result.error));
    }
    if (result.error == "unparseable_response") {
        throw std::runtime_error(
            "Agent error: Invalid response from API\n\n"
            "  Help:\n"
            "  - The API returned an unparseable response\n");
    }
    if (result.status_code < 200 || result.status_code >= 300) {
        throw std::runtime_error(fmt::format(
            "Agent error: API returned status {}\n\n"
            "  Got: {}\n\n"
            "  Help:\n"
            "  - Check agent configuration in govern.json\n"
            "  - Verify the model name is valid\n",
            result.status_code, result.error_detail));
    }

    return result.body;
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

// ============================================================================
// Public API: Multi-turn with HTTP status (non-throwing, for retry loop)
// ============================================================================

ProviderResult callAgentWithStatus(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const std::string& messages_json_str) {

    ProviderResult pr;

    try {
        json messages = json::parse(messages_json_str);

        // Build request the same way as the provider-specific functions,
        // but use httpPostRaw to get structured status codes.
        json request_body;
        HttpResult http_result;

        if (config.provider == "gemini" || config.provider == "google") {
            // Convert to Gemini format
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
            request_body["contents"] = contents;

            json gen_config;
            gen_config["maxOutputTokens"] = config.max_tokens;
            if (config.temperature != 1.0)
                gen_config["temperature"] = config.temperature;
            request_body["generationConfig"] = gen_config;

            if (!config.system_prompt.empty()) {
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
            http_result = httpPostRaw(url, request_body.dump(),
                {"content-type: application/json", "User-Agent: NAAb/1.0"},
                static_cast<long>(config.timeout_seconds));
        } else {
            // Anthropic format
            request_body["model"] = config.model;
            request_body["max_tokens"] = config.max_tokens;
            request_body["messages"] = messages;
            if (config.temperature != 1.0)
                request_body["temperature"] = config.temperature;
            if (!config.system_prompt.empty())
                request_body["system"] = config.system_prompt;

            http_result = httpPostRaw(
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

        pr.http_status = static_cast<int>(http_result.status_code);

        // Network/parse error
        if (!http_result.error.empty() && http_result.status_code == 0) {
            pr.response.success = false;
            pr.response.error = fmt::format(
                "Agent error: Network request failed ({})\n\n"
                "  Help:\n"
                "  - Check network connectivity\n"
                "  - The API endpoint may be unreachable\n",
                http_result.error);
            return pr;
        }
        if (http_result.error == "unparseable_response") {
            pr.response.success = false;
            pr.response.error = "Agent error: Invalid response from API";
            return pr;
        }

        // Non-2xx — return structured error with status code
        if (http_result.status_code < 200 || http_result.status_code >= 300) {
            pr.response.success = false;
            pr.response.http_status = pr.http_status;
            pr.response.error = fmt::format(
                "Agent error: API returned status {}\n\n"
                "  Got: {}\n\n"
                "  Help:\n"
                "  - Check agent configuration in govern.json\n"
                "  - Verify the model name is valid\n",
                http_result.status_code, http_result.error_detail);
            return pr;
        }

        // Success — normalize response
        auto normalized = normalizeResponse(config.provider, http_result.body);
        pr.response.success = true;
        pr.response.content = normalized.content;
        pr.response.stop_reason = normalized.stop_reason;
        pr.response.input_tokens = normalized.input_tokens;
        pr.response.output_tokens = normalized.output_tokens;

    } catch (const std::exception& e) {
        pr.response.success = false;
        pr.response.error = e.what();
    }

    return pr;
}

} // namespace runtime
} // namespace naab
