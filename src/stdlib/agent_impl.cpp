// NAAb Agent Module — Governed LLM conversation management
// Provides agent.create(), agent.send(), agent.run(), agent.messages(), agent.usage(),
// agent.batch(), agent.fan_out(), agent.pipeline()
// Requires "agents" section in govern.json for configuration

#include "naab/stdlib_new_modules.h"
#include "naab/naab_val.h"
#include "naab/governance.h"
#include "naab/agent_provider.h"
#include "naab/thread_pool.h"
#include "naab/sandbox.h"
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <stdexcept>
#include <cstdlib>
#include <unordered_set>
#include <mutex>
#include <future>
#include <regex>

namespace naab {
namespace stdlib {

using interpreter::NaabVal;
using json = nlohmann::json;

// RAII guard: sets GovernanceEngine on worker threads, clears on scope exit.
// Prevents stale thread_local pointers after pool tasks complete.
struct GovernanceGuard {
    GovernanceGuard(governance::GovernanceEngine* e) { governance::GovernanceEngine::setCurrent(e); }
    ~GovernanceGuard() { governance::GovernanceEngine::setCurrent(nullptr); }
    GovernanceGuard(const GovernanceGuard&) = delete;
    GovernanceGuard& operator=(const GovernanceGuard&) = delete;
};

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

    // Check API key availability (env var, then ~/.naab/keys/ fallback)
    std::string api_key_str = runtime::resolveApiKey(config->api_key_env);
    if (api_key_str.empty()) {
        throw std::runtime_error(
            "Agent error: API key not available\n\n"
            "  Help:\n"
            "  - Set the required API key environment variable, or\n"
            "  - Place the key in ~/.naab/keys/" + config->api_key_env + "\n");
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
    int current_turn = 0;
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
        current_turn = tracker.turns;
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

    // Get API key (env var, then ~/.naab/keys/ fallback)
    std::string api_key = runtime::resolveApiKey(config->api_key_env);
    if (api_key.empty()) {
        throw std::runtime_error(
            "Agent error: API key not available\n\n"
            "  Help:\n"
            "  - Set the required API key environment variable, or\n"
            "  - Place the key in ~/.naab/keys/" + config->api_key_env + "\n");
    }

    // Mid-run governance reload check (Governance Under Survivability)
    // Detect if operator tightened govern.json since last turn
    auto* gov_engine = governance::GovernanceEngine::getCurrent();
    std::vector<std::string> gov_notices;
    if (gov_engine && gov_engine->isActive()) {
        gov_engine->reloadIfChanged();
        gov_notices = gov_engine->getAndClearNotices();
    }

    // Behavioral sequence: emit agent.send event and block if it completes a hard pattern
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().behavioral_sequences.enabled) {
        gov_engine->setAgentTurn(handle_id, current_turn);
        std::string bsd_block = gov_engine->emitEvent(governance::RuntimeEventType::AGENT_SEND,
            "agent.send('" + config_name + "')", "", 0);
        if (!bsd_block.empty()) {
            throw std::runtime_error(bsd_block);
        }
    }

    // Call provider via shared layer
    auto agent_resp = runtime::callAgentMultiTurn(*config, api_key, messages_json.dump());
    if (!agent_resp.success) {
        throw std::runtime_error(agent_resp.error);
    }

    std::string content = agent_resp.content;
    std::string stop_reason = agent_resp.stop_reason;
    int resp_input_tokens = agent_resp.input_tokens;
    int resp_output_tokens = agent_resp.output_tokens;

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
    // (gov_engine already obtained above for reload check)
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

    // ── Gap 3: Per-agent path/shell restriction enforcement ──
    if (gov_engine && gov_engine->isActive() && !content.empty()) {
        // Shell restriction: if shell is explicitly blocked for this agent,
        // scan response for shell/bash/terminal command patterns
        if (config->shell_allowed_set && !config->shell_allowed) {
            // Detect code blocks with shell commands
            static const std::vector<std::string> shell_patterns = {
                "```(?:bash|sh|shell|zsh|terminal)",
                "\\$\\s+(?:sudo|rm|chmod|curl|wget|apt|pip|npm|cd|ls|mkdir|cat|echo)",
                "(?:^|\\n)\\s*(?:sudo|rm\\s|chmod\\s|chown\\s|kill\\s|pkill\\s)",
            };
            for (const auto& pat : shell_patterns) {
                try {
                    std::regex re(pat, std::regex::ECMAScript | std::regex::icase);
                    if (std::regex_search(content, re)) {
                        throw std::runtime_error(fmt::format(
                            "Agent error: Response from '{}' contains shell commands\n\n"
                            "  Help:\n"
                            "  - Shell execution is blocked for this agent role\n"
                            "  - Configure capabilities.shell.enabled or agent shell_allowed in govern.json\n",
                            config_name));
                    }
                } catch (const std::regex_error&) {}
            }
        }

        // Blocked paths: scan response for references to blocked path patterns
        if (!config->blocked_paths.empty()) {
            for (const auto& blocked : config->blocked_paths) {
                // Simple substring/glob match on response content
                if (blocked.find('*') != std::string::npos) {
                    // Glob pattern — convert to regex
                    std::string re_str;
                    for (char c : blocked) {
                        if (c == '*') re_str += ".*";
                        else if (c == '?') re_str += ".";
                        else if (c == '.' || c == '/' || c == '\\') re_str += std::string("\\") + c;
                        else re_str += c;
                    }
                    try {
                        std::regex re(re_str, std::regex::ECMAScript);
                        if (std::regex_search(content, re)) {
                            throw std::runtime_error(fmt::format(
                                "Agent error: Response from '{}' references a blocked path\n\n"
                                "  Help:\n"
                                "  - The agent's response references a path matching blocked pattern '{}'\n"
                                "  - Configure agent blocked_paths in govern.json\n",
                                config_name, blocked));
                        }
                    } catch (const std::regex_error&) {}
                } else {
                    // Exact substring match
                    if (content.find(blocked) != std::string::npos) {
                        throw std::runtime_error(fmt::format(
                            "Agent error: Response from '{}' references a blocked path\n\n"
                            "  Help:\n"
                            "  - The agent's response references blocked path '{}'\n"
                            "  - Configure agent blocked_paths in govern.json\n",
                            config_name, blocked));
                    }
                }
            }
        }

        // Allowed paths: if allowlist is set, any path-like reference not matching
        // an allowed pattern triggers a violation
        if (!config->allowed_paths.empty()) {
            // Extract path-like strings from response (starts with / or ./)
            static const std::regex path_re("(?:^|\\s|[\"'`])(/[a-zA-Z0-9_./-]+|\\./[a-zA-Z0-9_./-]+)",
                                            std::regex::ECMAScript);
            std::sregex_iterator it(content.begin(), content.end(), path_re);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                std::string path = (*it)[1].str();
                bool allowed = false;
                for (const auto& ap : config->allowed_paths) {
                    if (path.find(ap) == 0) { allowed = true; break; }
                }
                if (!allowed) {
                    throw std::runtime_error(fmt::format(
                        "Agent error: Response from '{}' references path '{}' not in allowed list\n\n"
                        "  Help:\n"
                        "  - Only paths matching allowed_paths are permitted for this agent\n"
                        "  - Configure agent allowed_paths in govern.json\n",
                        config_name, path));
                }
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
        current_turn = tracker.turns;  // updated turn count
    }

    // Behavioral sequence: emit agent.response event + context drift check
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().behavioral_sequences.enabled) {
        gov_engine->emitEvent(governance::RuntimeEventType::AGENT_RESPONSE,
            "agent.response('" + config_name + "', tokens=" +
            std::to_string(resp_output_tokens) + ")", "", 0);
        // Context drift check
        std::string drift_err = gov_engine->checkContextDrift(
            handle_id, current_turn,
            agent_resp.success ? "" : agent_resp.error);
        if (!drift_err.empty()) {
            // Hard/soft enforcement: abort the agent call immediately
            // (advisory returns "" from enforce(), so only real blocks reach here)
            throw std::runtime_error(drift_err);
        }
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

    // Attach governance reload notices (Governance Under Survivability)
    if (!gov_notices.empty()) {
        std::vector<NaabVal> notice_vals;
        notice_vals.reserve(gov_notices.size());
        for (const auto& n : gov_notices) {
            notice_vals.push_back(NaabVal::makeString(n));
        }
        result["governance_notices"] = NaabVal::makeList(std::move(notice_vals));
    }

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
// agent.check(config_name) → dict {valid, config_name, error}
// Pre-flight validation: config exists, API key env var is set and non-empty.
// Does NOT make an API call — just checks local config and env.
// ============================================================================

static NaabVal agentCheck(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error(
            "Agent error: agent.check requires a config name\n\n"
            "  Expected: agent.check(\"agent_name\")\n\n"
            "  Example:\n"
            "    let status = agent.check(\"judge_1\")\n"
            "    if status.get(\"valid\") { ... }\n");
    }

    std::string config_name = args[0].asString();
    std::unordered_map<std::string, NaabVal> result;
    result["config_name"] = NaabVal::makeString(config_name);

    // Check governance engine
    auto* engine = governance::GovernanceEngine::getCurrent();
    if (!engine || !engine->isActive()) {
        result["valid"] = NaabVal::makeBool(false);
        result["error"] = NaabVal::makeString("governance not active");
        return NaabVal::makeDict(std::move(result));
    }

    // Check config exists
    const auto* config = findAgentConfig(config_name);
    if (!config) {
        result["valid"] = NaabVal::makeBool(false);
        result["error"] = NaabVal::makeString("agent '" + config_name + "' not defined in govern.json");
        return NaabVal::makeDict(std::move(result));
    }

    // Check model is set
    if (config->model.empty()) {
        result["valid"] = NaabVal::makeBool(false);
        result["error"] = NaabVal::makeString("no model configured for '" + config_name + "'");
        return NaabVal::makeDict(std::move(result));
    }

    // Check API key availability (env var, then ~/.naab/keys/ fallback)
    std::string api_key = runtime::resolveApiKey(config->api_key_env);
    if (api_key.empty()) {
        result["valid"] = NaabVal::makeBool(false);
        result["error"] = NaabVal::makeString("API key '" + config->api_key_env + "' not found in env or ~/.naab/keys/");
        return NaabVal::makeDict(std::move(result));
    }

    result["valid"] = NaabVal::makeBool(true);
    result["error"] = NaabVal::makeString("");
    result["provider"] = NaabVal::makeString(config->provider);
    result["model"] = NaabVal::makeString(config->model);
    result["api_key_env"] = NaabVal::makeString(config->api_key_env);
    return NaabVal::makeDict(std::move(result));
}

// ============================================================================
// Agent thread pool — for batch/fan_out parallel dispatch.
// Separate from polyglot pool: agent calls are I/O-bound (HTTP wait).
// ============================================================================

static runtime::ThreadPool& getUserAgentThreadPool() {
    // Read pool config from governance if available
    int pool_size = 6;
    int queue_max = 50;
    auto* engine = governance::GovernanceEngine::getCurrent();
    if (engine && engine->isActive()) {
        pool_size = engine->getRules().agent_dispatch.pool_size;
        queue_max = engine->getRules().agent_dispatch.pool_queue_max;
    }
    static runtime::ThreadPool* pool = nullptr;
    static std::once_flag init_flag;
    static int init_pool_size = 0;
    static int init_queue_max = 0;
    std::call_once(init_flag, [pool_size, queue_max]() {
        init_pool_size = pool_size;
        init_queue_max = queue_max;
        pool = new runtime::ThreadPool(
            static_cast<size_t>(pool_size),
            static_cast<size_t>(queue_max));
    });
    // Warn if governance config differs from initialized pool
    if (pool_size != init_pool_size || queue_max != init_queue_max) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "[governance] Warning: agent_dispatch pool config changed "
                "(init: %d/%d, now: %d/%d) — restart required to apply.\n",
                init_pool_size, init_queue_max, pool_size, queue_max);
        }
    }
    return *pool;
}

// ============================================================================
// Helper: validate handle and return (config_name, handle_id)
// ============================================================================

static std::pair<std::string, int> validateHandle(NaabVal& handle_val) {
    if (!handle_val.isDict()) {
        throw std::runtime_error(
            "Agent error: Expected an agent handle (dict from agent.create())\n\n"
            "  Help:\n  - Use the handle returned by agent.create()\n");
    }
    auto& handle = handle_val.asDict();
    auto it = handle.find("__agent_handle");
    if (it == handle.end() || !it->second.isBool() || !it->second.asBool()) {
        throw std::runtime_error(
            "Agent error: Invalid agent handle\n\n"
            "  Help:\n  - Use the handle returned by agent.create()\n");
    }
    return {handle.at("config_name").asString(), handle.at("id").asInt()};
}

// ============================================================================
// agent.batch(handles_array, messages_array) → array of response dicts
// Sends messages[i] to handles[i] concurrently. Returns ordered results.
// ============================================================================

static NaabVal agentBatch(std::vector<NaabVal>& args) {
    if (args.size() < 2) {
        throw std::runtime_error(
            "Agent error: agent.batch requires handles array and messages array\n\n"
            "  Expected: agent.batch(handles, messages)\n\n"
            "  Example:\n"
            "    let h1 = agent.create(\"judge_1\")\n"
            "    let h2 = agent.create(\"judge_2\")\n"
            "    let results = agent.batch([h1, h2], [\"prompt1\", \"prompt2\"])\n");
    }

    if (!args[0].isList() || !args[1].isList()) {
        throw std::runtime_error(
            "Agent error: agent.batch requires two arrays\n\n"
            "  Got: " + std::string(args[0].isList() ? "array" : "non-array") +
            ", " + std::string(args[1].isList() ? "array" : "non-array") + "\n"
            "  Expected: agent.batch([handle1, handle2, ...], [msg1, msg2, ...])\n");
    }

    auto& handles = args[0].asList();
    auto& messages = args[1].asList();

    if (handles.size() != messages.size()) {
        throw std::runtime_error(fmt::format(
            "Agent error: handles and messages arrays must have same length\n\n"
            "  Got: {} handles, {} messages\n",
            handles.size(), messages.size()));
    }

    if (handles.empty()) {
        return NaabVal::makeList({});
    }

    // Validate all handles and messages upfront (fail fast)
    std::vector<std::pair<std::string, int>> validated;
    for (size_t i = 0; i < handles.size(); i++) {
        validated.push_back(validateHandle(handles[i]));
        if (!messages[i].isString()) {
            throw std::runtime_error(fmt::format(
                "Agent error: messages[{}] must be a string\n\n"
                "  Got: non-string value at index {}\n",
                i, i));
        }
    }

    // Check concurrency limit from governance
    auto* engine = governance::GovernanceEngine::getCurrent();
    int max_concurrent = 6;
    if (engine && engine->isActive()) {
        // Mid-run governance reload before dispatching batch workers
        engine->reloadIfChanged();
        max_concurrent = engine->getRules().agent_dispatch.max_concurrent;
    }

    // For single handle, just use regular send
    if (handles.size() == 1) {
        std::vector<NaabVal> send_args = {handles[0], messages[0]};
        NaabVal resp = agentSend(send_args);
        return NaabVal::makeList({resp});
    }

    // Dispatch all sends via thread pool
    auto& pool = getUserAgentThreadPool();

    // Each task captures handle+message by value to avoid cross-thread races.
    // Consequence: the caller's handle dicts are NOT updated with turn counts
    // or message history after batch completes. The server-side s_trackers map
    // remains authoritative — use agent.usage(handle) to read actual state.
    struct BatchTask {
        NaabVal handle;
        NaabVal message;
    };
    std::vector<BatchTask> tasks;
    for (size_t i = 0; i < handles.size(); i++) {
        tasks.push_back({handles[i], messages[i]});
    }

    // Capture governance engine pointer for worker threads (thread_local)
    auto* gov_engine_ptr = governance::GovernanceEngine::getCurrent();
    // Capture sandbox config for worker thread propagation (fail-closed: GAP 7)
    auto sandbox_config = security::SandboxManager::instance().getDefaultConfig();

    // Submit in batches respecting max_concurrent
    std::vector<NaabVal> results(tasks.size());
    size_t idx = 0;
    while (idx < tasks.size()) {
        size_t batch_end = std::min(idx + static_cast<size_t>(max_concurrent), tasks.size());

        std::vector<std::future<NaabVal>> futures;
        for (size_t i = idx; i < batch_end; i++) {
            auto& task = tasks[i];
            futures.push_back(pool.enqueue(
                [handle = task.handle, message = task.message, gov_engine_ptr, sandbox_config]() mutable -> NaabVal {
                    GovernanceGuard guard(gov_engine_ptr);
                    // Activate sandbox in worker thread (propagate from main thread)
                    security::ScopedSandbox worker_sandbox(sandbox_config);
                    try {
                        std::vector<NaabVal> send_args = {handle, message};
                        return agentSend(send_args);
                    } catch (const std::exception& e) {
                        // Return error dict instead of crashing the batch
                        std::unordered_map<std::string, NaabVal> err;
                        err["error"] = NaabVal::makeString(e.what());
                        err["success"] = NaabVal::makeBool(false);
                        err["content"] = NaabVal::makeString("");
                        return NaabVal::makeDict(std::move(err));
                    }
                }));
        }

        for (size_t i = 0; i < futures.size(); i++) {
            results[idx + i] = futures[i].get();
        }
        idx = batch_end;
    }

    return NaabVal::makeList(std::move(results));
}

// ============================================================================
// agent.fan_out(handles_array, message) → array of response dicts
// Sends the SAME message to all handles concurrently. Returns ordered results.
// ============================================================================

static NaabVal agentFanOut(std::vector<NaabVal>& args) {
    if (args.size() < 2) {
        throw std::runtime_error(
            "Agent error: agent.fan_out requires handles array and a message\n\n"
            "  Expected: agent.fan_out(handles, message)\n\n"
            "  Example:\n"
            "    let judges = [agent.create(\"j1\"), agent.create(\"j2\")]\n"
            "    let results = agent.fan_out(judges, \"evaluate this action\")\n");
    }

    if (!args[0].isList()) {
        throw std::runtime_error(
            "Agent error: First argument must be an array of handles\n\n"
            "  Expected: agent.fan_out([handle1, handle2, ...], message)\n");
    }

    if (!args[1].isString()) {
        throw std::runtime_error(
            "Agent error: Second argument must be a message string\n\n"
            "  Expected: agent.fan_out(handles, \"your message\")\n");
    }

    auto& handles = args[0].asList();
    std::string message = args[1].asString();

    if (handles.empty()) {
        return NaabVal::makeList({});
    }

    // Build messages array with same message for all (agentBatch validates handles)
    std::vector<NaabVal> messages_list;
    for (size_t i = 0; i < handles.size(); i++) {
        messages_list.push_back(NaabVal::makeString(message));
    }

    // Reuse batch logic
    std::vector<NaabVal> batch_args = {args[0], NaabVal::makeList(std::move(messages_list))};
    return agentBatch(batch_args);
}

// ============================================================================
// agent.pipeline(handles_array, initial_message) → final response dict
// Chains agents sequentially: output of N becomes input to N+1.
// Returns the final response. Intermediate responses stored in handle history.
// ============================================================================

static NaabVal agentPipeline(std::vector<NaabVal>& args) {
    if (args.size() < 2) {
        throw std::runtime_error(
            "Agent error: agent.pipeline requires handles array and initial message\n\n"
            "  Expected: agent.pipeline(handles, initial_message)\n\n"
            "  Example:\n"
            "    let a = agent.create(\"analyst\")\n"
            "    let r = agent.create(\"reviewer\")\n"
            "    let result = agent.pipeline([a, r], raw_data)\n");
    }

    if (!args[0].isList()) {
        throw std::runtime_error(
            "Agent error: First argument must be an array of handles\n\n"
            "  Expected: agent.pipeline([handle1, handle2, ...], message)\n");
    }

    if (!args[1].isString()) {
        throw std::runtime_error(
            "Agent error: Second argument must be a message string\n\n"
            "  Expected: agent.pipeline(handles, \"initial input\")\n");
    }

    auto& handles = args[0].asList();
    if (handles.empty()) {
        throw std::runtime_error(
            "Agent error: Pipeline requires at least one agent handle\n\n"
            "  Got: empty handles array\n");
    }

    // Validate all handles upfront
    for (size_t i = 0; i < handles.size(); i++) {
        validateHandle(handles[i]);
    }

    // Chain: send to each agent sequentially, pass output as input to next
    std::string current_message = args[1].asString();
    NaabVal last_response = NaabVal::makeNull();

    for (size_t i = 0; i < handles.size(); i++) {
        std::vector<NaabVal> send_args = {handles[i], NaabVal::makeString(current_message)};
        last_response = agentSend(send_args);

        // Extract content for next stage
        if (i < handles.size() - 1) {
            bool got_content = false;
            if (last_response.isDict()) {
                auto it = last_response.asDict().find("content");
                if (it != last_response.asDict().end() && it->second.isString()) {
                    current_message = it->second.asString();
                    got_content = !current_message.empty();
                }
            }
            if (!got_content) {
                throw std::runtime_error(fmt::format(
                    "Agent error: pipeline stage {} returned empty content — "
                    "cannot chain to next stage.\n", i));
            }
        }
    }

    return last_response;
}

// ============================================================================
// Module Interface
// ============================================================================

bool AgentModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "create", "send", "run", "messages", "usage",
        "batch", "fan_out", "pipeline", "check"
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
    if (function_name == "batch") return agentBatch(args);
    if (function_name == "fan_out") return agentFanOut(args);
    if (function_name == "pipeline") return agentPipeline(args);
    if (function_name == "check") return agentCheck(args);

    throw std::runtime_error(fmt::format(
        "Agent error: Unknown function 'agent.{}'\n\n"
        "  Available functions: create, send, run, messages, usage, batch, fan_out, pipeline\n\n"
        "  Help:\n"
        "  - agent.create(name) — create agent handle from govern.json config\n"
        "  - agent.send(handle, msg) — send message, get response\n"
        "  - agent.run(name, prompt) — one-shot conversation\n"
        "  - agent.messages(handle) — get conversation history\n"
        "  - agent.usage(handle) — get token usage stats\n"
        "  - agent.batch(handles, msgs) — parallel: send msgs[i] to handles[i]\n"
        "  - agent.fan_out(handles, msg) — parallel: send same msg to all handles\n"
        "  - agent.pipeline(handles, msg) — sequential: chain agents, output->input\n"
        "  - agent.check(name) — pre-flight: validate config + API key, returns {valid, error}\n",
        function_name));
}

} // namespace stdlib
} // namespace naab
