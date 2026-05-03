// NAAb Agent Module — Governed LLM conversation management
// Provides agent.create(), agent.send(), agent.run(), agent.messages(), agent.usage()
// Requires "agents" section in govern.json for configuration

#include "naab/stdlib_new_modules.h"
#include "naab/naab_val.h"
#include "naab/governance.h"
#include "naab/agent_provider.h"
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

    // Call provider via shared layer
    auto agent_resp = runtime::callAgentMultiTurn(*config, std::string(api_key), messages_json.dump());
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
