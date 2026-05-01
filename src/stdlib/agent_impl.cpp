// NAAb Agent Module — Governed LLM conversation management
// Provides agent.create(), agent.send(), agent.run(), agent.messages(), agent.usage()
// Requires "agents" section in govern.json for configuration

#include "naab/stdlib_new_modules.h"
#include "naab/naab_val.h"
#include "naab/governance.h"
#include "naab/sandbox.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <stdexcept>
#include <cstdlib>
#include <unordered_set>
#include <mutex>

namespace naab {
namespace stdlib {

using interpreter::NaabVal;
using json = nlohmann::json;

// V-DOS-010: Maximum API response size (25 MB, same as http module)
static constexpr size_t MAX_RESPONSE_BYTES = 25 * 1024 * 1024;

// Bounded write sink for curl
struct BoundedSink {
    std::string* buffer;
    size_t max_size;
};

static size_t AgentWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* sink = static_cast<BoundedSink*>(userp);
    if (sink->buffer->size() + total > sink->max_size) return 0;
    sink->buffer->append(static_cast<char*>(contents), total);
    return total;
}

// Counter for unique handle IDs
static int s_handle_counter = 0;

// Server-side governance tracking — immune to handle mutation
struct AgentTracker {
    int turns = 0;
    int input_tokens = 0;
    int output_tokens = 0;
};
static std::unordered_map<int, AgentTracker> s_trackers;
static std::mutex s_agent_mutex;

// ============================================================================
// Helper: Find agent config by name
// ============================================================================

static const governance::AgentConfig* findAgentConfig(const std::string& name) {
    auto* engine = governance::GovernanceEngine::getCurrent();
    if (!engine || !engine->isActive()) return nullptr;
    for (const auto& agent : engine->getRules().agents) {
        if (agent.name == name) return &agent;
    }
    return nullptr;
}

// ============================================================================
// Helper: HTTP POST with curl
// ============================================================================

static json httpPost(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& header_lines) {

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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AgentWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
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
        // Anthropic format
        if (response.contains("error") && response["error"].contains("message")) {
            error_msg = response["error"]["message"].get<std::string>();
        }
        // Gemini format
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
// Helper: Call Anthropic Messages API
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
        });
}

// ============================================================================
// Helper: Call Google Gemini API
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

    // Generation config
    json gen_config;
    gen_config["maxOutputTokens"] = config.max_tokens;
    if (config.temperature != 1.0)
        gen_config["temperature"] = config.temperature;
    request_body["generationConfig"] = gen_config;

    // System instruction
    if (!config.system_prompt.empty()) {
        json sys;
        sys["parts"] = json::array();
        json sys_part;
        sys_part["text"] = config.system_prompt;
        sys["parts"].push_back(sys_part);
        request_body["systemInstruction"] = sys;
    }

    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/"
                    + config.model + ":generateContent?key=" + api_key;

    return httpPost(url, request_body.dump(),
        {"content-type: application/json", "User-Agent: NAAb/1.0"});
}

// ============================================================================
// Helper: Normalize response to common format {content, stop_reason, usage}
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
        // Gemini response format
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
        // Estimate output tokens if API didn't report them but content exists
        // (Gemma models may omit candidatesTokenCount from usageMetadata)
        if (result.output_tokens == 0 && !result.content.empty()) {
            result.output_tokens = static_cast<int>(result.content.size() / 4);
        }
    } else {
        // Anthropic response format
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
// Helper: Route to correct provider
// ============================================================================

static json callProvider(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const json& messages) {

    // Sandbox check
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox) {
        std::string host = (config.provider == "gemini" || config.provider == "google")
            ? "generativelanguage.googleapis.com" : "api.anthropic.com";
        if (!sandbox->canConnect(host, 443)) {
            throw std::runtime_error(
                "Agent error: Network access to API endpoint is blocked by sandbox\n\n"
                "  Help:\n"
                "  - Agent API calls require network access\n"
                "  - Check sandbox configuration\n");
        }
    }

    if (config.provider == "gemini" || config.provider == "google") {
        return callGemini(config, api_key, messages);
    }
    return callAnthropic(config, api_key, messages);
}

// ============================================================================
// agent.create(config_name) → handle dict
// ============================================================================

static NaabVal agentCreate(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error(
            "Agent error: agent.create requires a config name\n\n"
            "  Got: " + std::string(args.empty() ? "no arguments" : "non-string argument") + "\n"
            "  Expected: agent.create(\"agent_name\")\n\n"
            "  Help:\n"
            "  - The name must match an entry in govern.json \"agents\" section\n\n"
            "  Example:\n"
            "    x Wrong: agent.create()\n"
            "    v Right: agent.create(\"researcher\")\n");
    }

    std::string config_name = args[0].asString();

    // Check governance engine is available
    auto* engine = governance::GovernanceEngine::getCurrent();
    if (!engine || !engine->isActive()) {
        throw std::runtime_error(
            "Agent error: agent.create requires governance configuration\n\n"
            "  Help:\n"
            "  - Create a govern.json with an \"agents\" section\n"
            "  - Agents must be defined in governance config before use\n");
    }

    // Find agent config
    const auto* config = findAgentConfig(config_name);
    if (!config) {
        // Build list of available agents
        std::string available;
        for (const auto& a : engine->getRules().agents) {
            if (!available.empty()) available += ", ";
            available += "\"" + a.name + "\"";
        }
        if (available.empty()) available = "(none defined)";

        throw std::runtime_error(fmt::format(
            "Agent error: Agent '{}' not defined in govern.json\n\n"
            "  Available agents: {}\n\n"
            "  Help:\n"
            "  - Add an \"agents\" section to govern.json with this agent name\n\n"
            "  Example:\n"
            "    \"agents\": {{\n"
            "      \"{}\": {{ \"model\": \"claude-sonnet-4-20250514\", \"max_tokens\": 4096 }}\n"
            "    }}\n",
            config_name, available, config_name));
    }

    // Validate model is set
    if (config->model.empty()) {
        throw std::runtime_error(fmt::format(
            "Agent error: Agent '{}' has no model configured\n\n"
            "  Help:\n"
            "  - Set \"model\" in the agent config in govern.json\n\n"
            "  Example:\n"
            "    \"{}\": {{ \"model\": \"claude-sonnet-4-20250514\", \"max_tokens\": 4096 }}\n",
            config_name, config_name));
    }

    // Check API key availability
    const char* api_key = std::getenv(config->api_key_env.c_str());
    if (!api_key || std::string(api_key).empty()) {
        throw std::runtime_error(
            "Agent error: API key not available\n\n"
            "  Help:\n"
            "  - Set the required API key environment variable\n"
            "  - The key must be set before running the script\n");
    }

    // Build handle dict — mutex protects s_handle_counter and s_trackers
    int handle_id;
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        handle_id = ++s_handle_counter;
        s_trackers[handle_id] = AgentTracker{};
    }

    std::unordered_map<std::string, NaabVal> handle;
    handle["__agent_handle"] = NaabVal::makeBool(true);
    handle["id"] = NaabVal::makeInt(handle_id);
    handle["config_name"] = NaabVal::makeString(config_name);
    handle["messages"] = NaabVal::makeList({});
    handle["input_tokens"] = NaabVal::makeInt(0);
    handle["output_tokens"] = NaabVal::makeInt(0);
    handle["turns"] = NaabVal::makeInt(0);

    return NaabVal::makeDict(std::move(handle));
}

// ============================================================================
// agent.send(handle, message) → response dict
// ============================================================================

static NaabVal agentSend(std::vector<NaabVal>& args) {
    if (args.size() < 2) {
        throw std::runtime_error(
            "Agent error: agent.send requires a handle and message\n\n"
            "  Expected: agent.send(handle, \"your message\")\n\n"
            "  Example:\n"
            "    let h = agent.create(\"researcher\")\n"
            "    let response = agent.send(h, \"What is 2+2?\")\n");
    }

    if (!args[0].isDict()) {
        throw std::runtime_error(
            "Agent error: First argument must be an agent handle from agent.create()\n\n"
            "  Help:\n"
            "  - Create a handle first with agent.create(\"name\")\n");
    }

    auto& handle = args[0].asDict();

    // Validate handle
    auto it = handle.find("__agent_handle");
    if (it == handle.end() || !it->second.isBool() || !it->second.asBool()) {
        throw std::runtime_error(
            "Agent error: Invalid agent handle\n\n"
            "  Help:\n"
            "  - Use the handle returned by agent.create()\n");
    }

    std::string config_name = handle["config_name"].asString();
    std::string message;
    if (args[1].isString()) {
        message = args[1].asString();
    } else {
        throw std::runtime_error(
            "Agent error: Message must be a string\n\n"
            "  Expected: agent.send(handle, \"your message\")\n");
    }

    // Get config
    const auto* config = findAgentConfig(config_name);
    if (!config) {
        throw std::runtime_error(fmt::format(
            "Agent error: Agent config '{}' no longer available\n\n"
            "  Help:\n"
            "  - The governance configuration may have changed\n",
            config_name));
    }

    // Server-side governance enforcement (immune to handle mutation)
    // Lock to validate tracker state; released before slow API call
    int handle_id = handle["id"].asInt();
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto tracker_it = s_trackers.find(handle_id);
        if (tracker_it == s_trackers.end()) {
            throw std::runtime_error(
                "Agent error: Invalid or expired agent handle\n\n"
                "  Help:\n"
                "  - Use a handle returned by agent.create()\n"
                "  - Forged or corrupted handles are rejected\n");
        }
        auto& tracker = tracker_it->second;

        // Enforce turn limit from tracker (not handle dict)
        if (tracker.turns >= config->max_turns) {
            throw std::runtime_error(fmt::format(
                "Agent error: Conversation exceeded max_turns limit ({})\n\n"
                "  Got: {} turns used\n"
                "  Expected: max {} turns\n\n"
                "  Help:\n"
                "  - Create a new agent handle for a fresh conversation\n"
                "  - Or increase max_turns in govern.json agents config\n",
                config->max_turns, tracker.turns, config->max_turns));
        }

        // Enforce token budget from tracker (not handle dict)
        int total_used = tracker.input_tokens + tracker.output_tokens;
        if (total_used >= config->max_total_tokens) {
            throw std::runtime_error(fmt::format(
                "Agent error: Token budget exhausted ({}/{} tokens used)\n\n"
                "  Help:\n"
                "  - Create a new agent handle for a fresh conversation\n"
                "  - Or increase max_total_tokens in govern.json agents config\n",
                total_used, config->max_total_tokens));
        }
    } // unlock before API call

    // Build messages array from handle history + new message
    json messages_json = json::array();
    auto& msg_list = handle["messages"].asList();
    for (auto& msg : msg_list) {
        auto& msg_dict = msg.asDict();
        json msg_obj;
        msg_obj["role"] = msg_dict["role"].asString();
        msg_obj["content"] = msg_dict["content"].asString();
        messages_json.push_back(msg_obj);
    }
    // Append new user message
    json user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = message;
    messages_json.push_back(user_msg);

    // Get API key
    const char* api_key = std::getenv(config->api_key_env.c_str());
    if (!api_key || std::string(api_key).empty()) {
        throw std::runtime_error(
            "Agent error: API key not available\n\n"
            "  Help:\n"
            "  - Set the required API key environment variable\n");
    }

    // Call provider-specific API
    json response = callProvider(*config, std::string(api_key), messages_json);

    // Normalize response across providers
    auto normalized = normalizeResponse(config->provider, response);
    std::string content = normalized.content;
    std::string stop_reason = normalized.stop_reason;
    int resp_input_tokens = normalized.input_tokens;
    int resp_output_tokens = normalized.output_tokens;

    // ── Gap 1: Block tool_use responses (no tool execution loop yet) ──
    if (stop_reason == "tool_use" || stop_reason == "FUNCTION_CALL" ||
        stop_reason == "tool_calls") {
        throw std::runtime_error(fmt::format(
            "Agent error: Agent '{}' returned a tool_use response\n\n"
            "  Got: stop_reason={}\n\n"
            "  Help:\n"
            "  - Agent tool execution is not yet supported\n"
            "  - The agent attempted to call a function instead of responding with text\n"
            "  - Rephrase your prompt to request a text response\n",
            config_name, stop_reason));
    }

    // ── Gap 2: Output content filtering (secrets + PII) ──
    auto* gov_engine = governance::GovernanceEngine::getCurrent();
    if (gov_engine && gov_engine->isActive() && !content.empty()) {
        // Check for secrets in LLM response (reuses existing 18-pattern scanner)
        std::string secret_err = gov_engine->checkSecrets(content, 0);
        if (!secret_err.empty()) {
            throw std::runtime_error(fmt::format(
                "Agent error: Response from '{}' contains a potential secret\n\n"
                "  Help:\n"
                "  - The LLM response was blocked because it contains a pattern\n"
                "    matching a known secret format (API key, token, etc.)\n"
                "  - This is enforced by governance code_quality.no_secrets\n",
                config_name));
        }
        // Check for PII in LLM response (reuses existing SSN/CC/email/phone/IP scanner)
        std::string pii_err = gov_engine->checkPii(content, 0);
        if (!pii_err.empty()) {
            throw std::runtime_error(fmt::format(
                "Agent error: Response from '{}' contains potential PII\n\n"
                "  Help:\n"
                "  - The LLM response was blocked because it contains a pattern\n"
                "    matching personally identifiable information\n"
                "  - This is enforced by governance code_quality.no_pii\n",
                config_name));
        }
    }

    // ── Gap 3: Advisory for per-agent path/shell restrictions (once per config) ──
    if (gov_engine && gov_engine->isActive()) {
        bool has_restrictions = config->shell_allowed_set ||
                                !config->allowed_paths.empty() ||
                                !config->blocked_paths.empty();
        if (has_restrictions) {
            static std::unordered_set<std::string> warned_agents;
            if (warned_agents.find(config_name) == warned_agents.end()) {
                warned_agents.insert(config_name);
                fprintf(stderr,
                    "[governance] Advisory: Agent '%s' has path/shell restrictions "
                    "configured — enforcement pending tool execution support\n",
                    config_name.c_str());
            }
        }
    }

    // Update handle: append messages, update counters
    // Add user message to history
    std::unordered_map<std::string, NaabVal> user_msg_val;
    user_msg_val["role"] = NaabVal::makeString("user");
    user_msg_val["content"] = NaabVal::makeString(message);
    msg_list.push_back(NaabVal::makeDict(std::move(user_msg_val)));

    // Add assistant message to history
    std::unordered_map<std::string, NaabVal> asst_msg_val;
    asst_msg_val["role"] = NaabVal::makeString("assistant");
    asst_msg_val["content"] = NaabVal::makeString(content);
    msg_list.push_back(NaabVal::makeDict(std::move(asst_msg_val)));

    // Update server-side tracker (authoritative) and handle dict (informational)
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto& tracker = s_trackers[handle_id];
        tracker.turns++;
        tracker.input_tokens += resp_input_tokens;
        tracker.output_tokens += resp_output_tokens;
        handle["turns"] = NaabVal::makeInt(tracker.turns);
        handle["input_tokens"] = NaabVal::makeInt(tracker.input_tokens);
        handle["output_tokens"] = NaabVal::makeInt(tracker.output_tokens);
    }

    // Build response dict
    std::unordered_map<std::string, NaabVal> result;
    result["content"] = NaabVal::makeString(content);
    result["role"] = NaabVal::makeString("assistant");
    result["stop_reason"] = NaabVal::makeString(stop_reason);

    std::unordered_map<std::string, NaabVal> usage_val;
    usage_val["input_tokens"] = NaabVal::makeInt(resp_input_tokens);
    usage_val["output_tokens"] = NaabVal::makeInt(resp_output_tokens);
    result["usage"] = NaabVal::makeDict(std::move(usage_val));

    return NaabVal::makeDict(std::move(result));
}

// ============================================================================
// agent.run(config_name, prompt) → content string
// ============================================================================

static NaabVal agentRun(std::vector<NaabVal>& args) {
    if (args.size() < 2 || !args[0].isString() || !args[1].isString()) {
        throw std::runtime_error(
            "Agent error: agent.run requires a config name and prompt\n\n"
            "  Expected: agent.run(\"agent_name\", \"your prompt\")\n\n"
            "  Help:\n"
            "  - agent.run is a one-shot convenience function\n"
            "  - For multi-turn conversations, use agent.create() + agent.send()\n\n"
            "  Example:\n"
            "    let answer = agent.run(\"researcher\", \"What is 2+2?\")\n");
    }

    // Create handle, send, return content
    std::vector<NaabVal> create_args = {args[0]};
    NaabVal handle = agentCreate(create_args);

    std::vector<NaabVal> send_args = {handle, args[1]};
    NaabVal response = agentSend(send_args);

    return response.asDict()["content"];
}

// ============================================================================
// agent.messages(handle) → array
// ============================================================================

static NaabVal agentMessages(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isDict()) {
        throw std::runtime_error(
            "Agent error: agent.messages requires an agent handle\n\n"
            "  Expected: agent.messages(handle)\n");
    }

    auto& handle = args[0].asDict();
    auto it = handle.find("messages");
    if (it == handle.end()) {
        return NaabVal::makeList({});
    }
    return it->second;
}

// ============================================================================
// agent.usage(handle) → usage dict
// ============================================================================

static NaabVal agentUsage(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isDict()) {
        throw std::runtime_error(
            "Agent error: agent.usage requires an agent handle\n\n"
            "  Expected: agent.usage(handle)\n");
    }

    auto& handle = args[0].asDict();

    // Validate handle
    auto it = handle.find("__agent_handle");
    if (it == handle.end() || !it->second.isBool() || !it->second.asBool()) {
        throw std::runtime_error(
            "Agent error: Invalid agent handle\n\n"
            "  Help:\n  - Use the handle returned by agent.create()\n");
    }

    // Read from server-side tracker (immune to handle mutation)
    int handle_id = handle["id"].asInt();
    std::lock_guard<std::mutex> lock(s_agent_mutex);
    auto tracker_it = s_trackers.find(handle_id);
    if (tracker_it == s_trackers.end()) {
        throw std::runtime_error(
            "Agent error: Invalid or expired agent handle\n\n"
            "  Help:\n  - Use a handle returned by agent.create()\n");
    }
    auto& tracker = tracker_it->second;

    std::unordered_map<std::string, NaabVal> result;
    result["input_tokens"] = NaabVal::makeInt(tracker.input_tokens);
    result["output_tokens"] = NaabVal::makeInt(tracker.output_tokens);
    result["total_tokens"] = NaabVal::makeInt(tracker.input_tokens + tracker.output_tokens);
    result["turns"] = NaabVal::makeInt(tracker.turns);

    return NaabVal::makeDict(std::move(result));
}

// ============================================================================
// Module Interface
// ============================================================================

bool AgentModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "create", "send", "run", "messages", "usage"
    };
    return functions.count(name) > 0;
}

NaabVal AgentModule::call(
    const std::string& function_name,
    std::vector<NaabVal>& args) {

    if (function_name == "create") return agentCreate(args);
    if (function_name == "send") return agentSend(args);
    if (function_name == "run") return agentRun(args);
    if (function_name == "messages") return agentMessages(args);
    if (function_name == "usage") return agentUsage(args);

    throw std::runtime_error(fmt::format(
        "Agent error: Unknown function 'agent.{}'\n\n"
        "  Available functions: create, send, run, messages, usage\n\n"
        "  Help:\n"
        "  - agent.create(name) — create agent handle from govern.json config\n"
        "  - agent.send(handle, msg) — send message, get response\n"
        "  - agent.run(name, prompt) — one-shot conversation\n"
        "  - agent.messages(handle) — get conversation history\n"
        "  - agent.usage(handle) — get token usage stats\n",
        function_name));
}

} // namespace stdlib
} // namespace naab
