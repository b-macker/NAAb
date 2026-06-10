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
#include <atomic>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include "naab/crypto_utils.h"

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

// Process-level secret for HMAC nonce generation (anti-forge)
// Generated once from /dev/urandom; never exposed to scripts
static std::string s_process_secret;
static std::once_flag s_secret_init;
static void ensureProcessSecret() {
    std::call_once(s_secret_init, []() {
        unsigned char buf[32];
        FILE* f = fopen("/dev/urandom", "rb");
        if (f) {
            size_t n = fread(buf, 1, 32, f);
            fclose(f);
            if (n == 32) {
                s_process_secret = security::CryptoUtils::toHex(buf, 32);
                return;
            }
        }
        // Fallback: timestamp + pid (weaker but functional)
        fprintf(stderr, "[governance] Warning: /dev/urandom unavailable — agent handle security is degraded\n");
        s_process_secret = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
            ":" + std::to_string(getpid());
    });
}

// Server-side governance tracking — immune to handle mutation
struct AgentTracker {
    int turns = 0;
    int input_tokens = 0;
    int output_tokens = 0;
    int retries = 0;           // total retry attempts across all sends
    int fallbacks = 0;         // times a fallback model was used
    int64_t total_latency_ms = 0;  // cumulative API call time
    std::string nonce;         // HMAC nonce — verified on every handle use
    int last_challenge_turn = -100;  // step-up challenge cooldown tracking
    int challenges_passed = 0;
    int challenges_failed = 0;
    // Tool execution counters (Phase 4 — L5 telemetry)
    int tool_calls_total = 0;      // cumulative tool invocations across all sends
    int tool_calls_blocked = 0;    // cumulative blocked tool calls
    int64_t tool_total_latency_ms = 0; // cumulative tool execution time
    // Standing Lease — TTL on agent authorization (Kerberos TGT / OAuth access token analog)
    int lease_granted_turn = 0;    // turn when lease was last granted/renewed
    int lease_expires_turn = 0;    // turn after which agent must re-authorize (0 = no lease)
    std::chrono::steady_clock::time_point lease_granted_time;  // wall-clock lease start
    int key_offset = 0;  // round-robin position across agent.send() calls
};
static std::unordered_map<int, AgentTracker> s_trackers;
static std::mutex s_agent_mutex;

// Run-level dispatch counters — shared across all agent calls in this process
struct DispatchCounters {
    std::atomic<int> total_calls{0};
    std::atomic<int> total_retries{0};
    std::atomic<int> total_tokens{0};
    std::atomic<int64_t> total_agent_time_ms{0};
    std::atomic<int> consecutive_failures{0};
    std::atomic<bool> hard_stopped{false};
    std::string stop_reason;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> dead_keys;
    std::mutex dead_keys_mutex;
    std::mutex stop_reason_mutex;
};
static DispatchCounters s_dispatch;

// Tool registration — script-side allowlist for agent tool execution
struct ToolRegistration {
    std::string name;
    NaabVal function;           // NaabVal holding the FunctionValue
    std::string description;    // from schema
    json input_schema;          // JSON Schema for parameters
};
static std::mutex s_tools_mutex;
static std::unordered_map<std::string, ToolRegistration> s_registered_tools;

// Thread-local tool execution context
static thread_local const governance::AgentConfig* t_tool_agent_context = nullptr;
static thread_local int t_in_tool_execution_for_handle = -1;

// RAII guard for scoped tool agent context
struct ScopedToolContext {
    const governance::AgentConfig* previous;
    ScopedToolContext(const governance::AgentConfig* config)
        : previous(t_tool_agent_context) { t_tool_agent_context = config; }
    ~ScopedToolContext() { t_tool_agent_context = previous; }
};

// Validate tool name: alphanumeric + underscore, reasonable length
static bool isValidToolName(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    if (std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    return true;
}

// Builtin names that cannot be used as tool names
static bool isReservedToolName(const std::string& name) {
    static const std::unordered_set<std::string> reserved = {
        "print", "len", "type", "typeof", "int", "float", "string", "bool",
        "range", "null", "true", "false", "main", "return", "if", "else",
        "for", "while", "break", "continue", "fn", "let", "const", "use",
        "import", "export", "struct", "enum", "match", "try", "catch", "throw",
        "new", "in", "config"
    };
    return reserved.count(name) > 0;
}

// Get current tool agent context (for stdlib functions to check per-agent restrictions)
const governance::AgentConfig* getToolAgentContext() {
    return t_tool_agent_context;
}

// Check if a key is dead (with optional cooldown-based revival).
// Caller must hold s_dispatch.dead_keys_mutex.
// If revived is non-null and a key is revived, *revived is set to true.
static bool isKeyDead(const std::string& key_env, int cooldown_seconds,
                      bool* revived = nullptr) {
    auto it = s_dispatch.dead_keys.find(key_env);
    if (it == s_dispatch.dead_keys.end()) return false;
    if (cooldown_seconds <= 0) return true;  // never revive (backward compat)
    auto elapsed = std::chrono::steady_clock::now() - it->second;
    if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= cooldown_seconds) {
        s_dispatch.dead_keys.erase(it);  // cooldown elapsed — revive
        if (revived) *revived = true;
        return false;
    }
    return true;
}

// Score a step-up challenge response: word count + keyword overlap with system prompt
static bool scoreStepUpChallenge(const std::string& response,
                                  const std::string& system_prompt,
                                  int min_words, double keyword_threshold) {
    // 1. Word count
    int words = 0;
    bool in_word = false;
    for (char c : response) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    if (words < min_words) return false;

    // 2. Extract meaningful keywords from system prompt (lowercase, >3 chars)
    std::unordered_set<std::string> prompt_keywords;
    std::string current;
    for (char c : system_prompt) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (current.size() > 3) prompt_keywords.insert(current);
            current.clear();
        }
    }
    if (current.size() > 3) prompt_keywords.insert(current);
    if (prompt_keywords.empty()) return true;  // no keywords to check

    // 3. Check overlap with response
    std::string response_lower;
    response_lower.reserve(response.size());
    for (char c : response)
        response_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    int found = 0;
    for (const auto& kw : prompt_keywords) {
        if (response_lower.find(kw) != std::string::npos) found++;
    }
    return static_cast<double>(found) / prompt_keywords.size() >= keyword_threshold;
}

// Pipeline upstream provenance — keyed by downstream handle_id
// Set by agentPipeline() after each stage; read by buildEnvironmentDict()
static std::mutex s_provenance_mutex;
static std::unordered_map<int, NaabVal> s_upstream_provenance;

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

// Forward declaration — validates handle and verifies HMAC nonce
static std::pair<std::string, int> validateHandle(NaabVal& handle_val);

// ============================================================================
// Helper: Build environment dict for agent self-awareness
// Returns static config (limits, model chain, permissions) + dynamic state
// (turns/tokens remaining, coherence, key health, dispatch proximity)
// ============================================================================

static NaabVal buildEnvironmentDict(int handle_id, const std::string& config_name) {
    std::unordered_map<std::string, NaabVal> env;

    // --- Static config (from govern.json AgentConfig) ---
    const auto* config = findAgentConfig(config_name);
    if (config) {
        // Limits
        std::unordered_map<std::string, NaabVal> limits;
        limits["max_turns"] = NaabVal::makeInt(config->max_turns);
        limits["max_tokens"] = NaabVal::makeInt(config->max_tokens);
        limits["max_total_tokens"] = NaabVal::makeInt(config->max_total_tokens);
        limits["timeout_seconds"] = NaabVal::makeInt(config->timeout_seconds);
        limits["risk_budget"] = NaabVal::makeInt(config->risk_budget);
        env["limits"] = NaabVal::makeDict(std::move(limits));

        // Model chain
        std::vector<NaabVal> model_list;
        for (const auto& m : config->model_chain) {
            model_list.push_back(NaabVal::makeString(m));
        }
        if (model_list.empty()) {
            model_list.push_back(NaabVal::makeString(config->model));
        }
        env["model_chain"] = NaabVal::makeList(std::move(model_list));
        env["provider"] = NaabVal::makeString(config->provider);
        env["temperature"] = NaabVal::makeDouble(config->temperature);
        env["response_format"] = NaabVal::makeString(config->response_format);

        // Key pool size (don't expose key names — security)
        int key_count = config->api_key_envs.empty() ? 1 : static_cast<int>(config->api_key_envs.size());
        env["key_pool_size"] = NaabVal::makeInt(key_count);

        // Retry config
        std::unordered_map<std::string, NaabVal> retry;
        retry["max_attempts"] = NaabVal::makeInt(config->retry.max_attempts);
        retry["backoff_ms"] = NaabVal::makeInt(config->retry.backoff_ms);
        retry["jitter"] = NaabVal::makeBool(config->retry.jitter);
        retry["key_retry_after_seconds"] = NaabVal::makeInt(config->retry.key_retry_after_seconds);
        env["retry"] = NaabVal::makeDict(std::move(retry));

        // Permissions
        std::unordered_map<std::string, NaabVal> permissions;
        permissions["shell_allowed"] = NaabVal::makeBool(
            !config->shell_allowed_set || config->shell_allowed);
        std::vector<NaabVal> allowed_langs;
        for (const auto& l : config->allowed_languages)
            allowed_langs.push_back(NaabVal::makeString(l));
        permissions["allowed_languages"] = NaabVal::makeList(std::move(allowed_langs));
        permissions["network_allowed"] = NaabVal::makeBool(
            !config->network_allowed_set || config->network_allowed);
        if (!config->allowed_actions.empty()) {
            std::vector<NaabVal> actions;
            for (const auto& a : config->allowed_actions)
                actions.push_back(NaabVal::makeString(a));
            permissions["allowed_actions"] = NaabVal::makeList(std::move(actions));
        }
        // Tool execution permissions
        permissions["tools_enabled"] = NaabVal::makeBool(config->tools_enabled);
        if (config->tools_enabled) {
            // Count registered tools that match config allowlist
            int registered_count = 0;
            std::vector<NaabVal> tool_names;
            {
                std::lock_guard<std::mutex> tlock(s_tools_mutex);
                for (const auto& t : config->tools) {
                    if (s_registered_tools.count(t)) {
                        registered_count++;
                        tool_names.push_back(NaabVal::makeString(t));
                    }
                }
            }
            permissions["tools_registered"] = NaabVal::makeInt(registered_count);
            permissions["tools_available"] = NaabVal::makeList(std::move(tool_names));
        }
        env["permissions"] = NaabVal::makeDict(std::move(permissions));
    }

    // --- Dynamic state (computed at call time) ---
    std::unordered_map<std::string, NaabVal> state;

    // Per-agent usage from tracker
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto it = s_trackers.find(handle_id);
        if (it != s_trackers.end()) {
            auto& t = it->second;
            state["turns_used"] = NaabVal::makeInt(t.turns);
            state["tokens_used"] = NaabVal::makeInt(t.input_tokens + t.output_tokens);
            if (config) {
                state["turns_remaining"] = NaabVal::makeInt(
                    std::max(0, config->max_turns - t.turns));
                state["tokens_remaining"] = NaabVal::makeInt(
                    config->max_total_tokens > 0
                        ? std::max(0, config->max_total_tokens - t.input_tokens - t.output_tokens)
                        : -1);
            }
            state["challenges_passed"] = NaabVal::makeInt(t.challenges_passed);
            state["challenges_failed"] = NaabVal::makeInt(t.challenges_failed);
            // Standing Lease: remaining turns before re-authorization required
            if (t.lease_expires_turn > 0) {
                state["lease_remaining"] = NaabVal::makeInt(
                    std::max(0, t.lease_expires_turn - t.turns));
            }
            // Wall-clock lease: remaining seconds
            if (config && config->standing_lease_seconds > 0) {
                auto elapsed = std::chrono::steady_clock::now() - t.lease_granted_time;
                int elapsed_sec = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
                state["lease_remaining_seconds"] = NaabVal::makeInt(
                    std::max(0, config->standing_lease_seconds - elapsed_sec));
            }
            // Tool execution state
            if (t.tool_calls_total > 0 || (config && config->tools_enabled)) {
                state["tool_calls_total"] = NaabVal::makeInt(t.tool_calls_total);
                state["tool_calls_blocked"] = NaabVal::makeInt(t.tool_calls_blocked);
                state["tool_total_latency_ms"] = NaabVal::makeInt(static_cast<int>(t.tool_total_latency_ms));
            }
        }
    }
    // s_agent_mutex released before acquiring dead_keys_mutex (lock ordering)

    // Key health (count only — no key env var names exposed)
    {
        std::lock_guard<std::mutex> lock(s_dispatch.dead_keys_mutex);
        int dead_count = 0;
        std::vector<std::string> keys = config ? config->api_key_envs : std::vector<std::string>{};
        if (keys.empty() && config) keys.push_back(config->api_key_env);
        int cooldown = config ? config->retry.key_retry_after_seconds : 0;
        for (const auto& k : keys) {
            if (isKeyDead(k, cooldown)) dead_count++;
        }
        state["keys_active"] = NaabVal::makeInt(static_cast<int>(keys.size()) - dead_count);
        state["keys_dead"] = NaabVal::makeInt(dead_count);
    }

    // Coherence + governance state (from governance engine)
    auto* ge = governance::GovernanceEngine::getCurrent();
    if (ge && ge->isActive()) {
        // getDriftState() returns std::optional<DriftState>, is const and thread-safe
        auto drift_opt = ge->getDriftState(handle_id);
        if (drift_opt) {
            state["coherence"] = NaabVal::makeDouble(drift_opt->coherence_score);
            state["coherence_velocity"] = NaabVal::makeDouble(drift_opt->coherence_velocity);
            state["contradictions"] = NaabVal::makeInt(drift_opt->contradictions);
            state["circular_actions"] = NaabVal::makeInt(drift_opt->circular_action_count);
            state["repeated_failures"] = NaabVal::makeInt(drift_opt->repeated_failures);
            state["scope_creep"] = NaabVal::makeInt(drift_opt->scope_creep_count);
            state["pipeline_depth"] = NaabVal::makeInt(drift_opt->pipeline_depth);
            state["inherited_pressure"] = NaabVal::makeDouble(drift_opt->inherited_pressure);

            // Capability utilization: what agent has vs what it's used
            std::vector<NaabVal> granted, exercised;
            for (const auto& c : drift_opt->granted_capabilities)
                granted.push_back(NaabVal::makeString(c));
            for (const auto& c : drift_opt->exercised_capabilities)
                exercised.push_back(NaabVal::makeString(c));
            state["capabilities_granted"] = NaabVal::makeList(std::move(granted));
            state["capabilities_exercised"] = NaabVal::makeList(std::move(exercised));
        } else {
            state["coherence"] = NaabVal::makeDouble(1.0);  // fresh agent
            state["pipeline_depth"] = NaabVal::makeInt(0);
        }

        // Risk budget remaining (getRemainingBudget() is public, const)
        if (config && config->risk_budget > 0) {
            state["risk_budget_remaining"] = NaabVal::makeInt(
                ge->getRemainingBudget(config_name));
        }

        // Governance level — enum is NORMAL(0), ELEVATED(1), HIGH(2), CRITICAL(3)
        auto level = ge->getGovernanceLevel();
        const char* level_str = "normal";
        switch (level) {
            case governance::GovernanceLevel::NORMAL:   level_str = "normal";   break;
            case governance::GovernanceLevel::ELEVATED: level_str = "elevated"; break;
            case governance::GovernanceLevel::HIGH:     level_str = "high";     break;
            case governance::GovernanceLevel::CRITICAL: level_str = "critical"; break;
        }
        state["governance_level"] = NaabVal::makeString(level_str);

        // Governance pulse — verdict only (no breakdown, prevent gaming)
        auto pulse_verdict = ge->getPulseVerdict();
        const char* pulse_str = "healthy";
        if (pulse_verdict == governance::PulseVerdict::DEGRADED) pulse_str = "degraded";
        else if (pulse_verdict == governance::PulseVerdict::IMPAIRED) pulse_str = "impaired";
        state["governance_health"] = NaabVal::makeString(pulse_str);
        state["refusal_count"] = NaabVal::makeInt(ge->getPulse().refusal_count);

        // Evidence epoch — monotonic counter for evidence freshness
        state["governance_epoch"] = NaabVal::makeInt(ge->getGovernanceEpoch());
    }

    // Upstream provenance (pipeline stages only — trust calibration for input)
    {
        std::lock_guard<std::mutex> lock(s_provenance_mutex);
        auto prov_it = s_upstream_provenance.find(handle_id);
        if (prov_it != s_upstream_provenance.end()) {
            state["upstream_provenance"] = prov_it->second;
        }
    }

    // Dispatch (run-level hard stop proximity)
    std::unordered_map<std::string, NaabVal> dispatch;
    if (ge) {
        const auto& hs = ge->getRules().agent_dispatch.hard_stop;
        dispatch["calls_remaining"] = NaabVal::makeInt(
            hs.max_calls_per_run > 0
                ? std::max(0, hs.max_calls_per_run - s_dispatch.total_calls.load()) : -1);
        dispatch["tokens_remaining"] = NaabVal::makeInt(
            hs.max_tokens_per_run > 0
                ? std::max(0, hs.max_tokens_per_run - s_dispatch.total_tokens.load()) : -1);
        dispatch["time_remaining_ms"] = NaabVal::makeInt(
            hs.max_agent_time_ms > 0
                ? std::max(0, static_cast<int>(hs.max_agent_time_ms - s_dispatch.total_agent_time_ms.load())) : -1);
        dispatch["hard_stopped"] = NaabVal::makeBool(s_dispatch.hard_stopped.load());
    }
    state["dispatch"] = NaabVal::makeDict(std::move(dispatch));

    env["state"] = NaabVal::makeDict(std::move(state));
    return NaabVal::makeDict(std::move(env));
}

// ============================================================================
// agent.register_tool(name, function, schema) — register NAAb function as LLM tool
// ============================================================================

static NaabVal agentRegisterTool(std::vector<NaabVal>& args) {
    if (args.size() < 3 || !args[0].isString()) {
        throw std::runtime_error(
            "Agent error: agent.register_tool requires (name, function, schema)\n\n"
            "  Help:\n"
            "  - name: string name for the tool\n"
            "  - function: the NAAb function to call\n"
            "  - schema: dict with 'description' and 'parameters' keys\n\n"
            "  Example:\n"
            "    agent.register_tool(\"search\", search_fn, {\n"
            "      \"description\": \"Search the database\",\n"
            "      \"parameters\": {\"query\": {\"type\": \"string\"}}\n"
            "    })\n");
    }

    std::string name = args[0].asString();

    // Validate tool name (Gap I)
    if (!isValidToolName(name)) {
        throw std::runtime_error(
            "Agent error: Invalid tool name\n\n"
            "  Help:\n"
            "  - Tool names must be 1-128 characters\n"
            "  - Only letters, digits, and underscores allowed\n"
            "  - Must start with a letter or underscore\n");
    }
    if (isReservedToolName(name)) {
        throw std::runtime_error(
            "Agent error: Tool name conflicts with a reserved keyword\n\n"
            "  Help:\n"
            "  - Choose a different name that doesn't conflict with NAAb builtins\n");
    }

    // Validate function argument (accept both tree-walker FunctionValue and VM closures)
    if (!args[1].isFunction() && !args[1].isVMClosure()) {
        throw std::runtime_error(
            "Agent error: Second argument to register_tool must be a function\n\n"
            "  Help:\n"
            "  - Pass a function reference, not a string or other value\n");
    }

    // Parse schema from dict
    std::string description;
    json input_schema;

    if (args[2].isDict()) {
        auto& schema_dict = args[2].asDict();
        auto desc_it = schema_dict.find("description");
        if (desc_it != schema_dict.end() && desc_it->second.isString()) {
            description = desc_it->second.asString();
        }
        auto params_it = schema_dict.find("parameters");
        if (params_it != schema_dict.end() && params_it->second.isDict()) {
            // Convert NaabVal dict to JSON schema
            auto& params = params_it->second.asDict();
            input_schema["type"] = "object";
            json properties = json::object();
            for (auto& [key, val] : params) {
                if (val.isDict()) {
                    auto& pdict = val.asDict();
                    json prop;
                    auto type_it = pdict.find("type");
                    if (type_it != pdict.end() && type_it->second.isString())
                        prop["type"] = type_it->second.asString();
                    auto pdesc_it = pdict.find("description");
                    if (pdesc_it != pdict.end() && pdesc_it->second.isString())
                        prop["description"] = pdesc_it->second.asString();
                    properties[key] = prop;
                }
            }
            input_schema["properties"] = properties;
        }
    }

    if (description.empty()) {
        throw std::runtime_error(
            "Agent error: Tool schema must include a 'description' field\n\n"
            "  Help:\n"
            "  - Provide a description so the LLM knows when to use this tool\n");
    }

    // Register (or warn on re-registration)
    {
        std::lock_guard<std::mutex> lock(s_tools_mutex);
        auto it = s_registered_tools.find(name);
        if (it != s_registered_tools.end()) {
            // Re-registration: warn but allow (existing agents keep their snapshots)
            auto* gov_engine = governance::GovernanceEngine::getCurrent();
            if (gov_engine) {
                gov_engine->emitEvent(governance::RuntimeEventType::AGENT_SEND,
                    "register_tool_reregistration('" + name + "')", "", 0);
            }
        }
        s_registered_tools[name] = ToolRegistration{
            name, args[1], description, input_schema
        };
    }

    // Emit telemetry
    auto* gov_engine = governance::GovernanceEngine::getCurrent();
    if (gov_engine && gov_engine->isActive()) {
        gov_engine->writeAgentTelemetry("AGENT_TOOL_REGISTERED", {
            {"tool_name", name},
            {"description", description}
        });
    }

    return NaabVal::makeBool(true);
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
    // Generate HMAC nonce to prevent handle forgery/replay
    ensureProcessSecret();
    int handle_id;
    std::string nonce;
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        handle_id = ++s_handle_counter;
        std::string nonce_input = std::to_string(handle_id) + ":" + config_name + ":" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        nonce = security::CryptoUtils::hmacSha256(nonce_input, s_process_secret);
        auto& tracker = s_trackers[handle_id];
        tracker.nonce = nonce;
        // Standing Lease: grant initial lease if configured
        if (config->standing_lease_turns > 0) {
            tracker.lease_granted_turn = 0;
            tracker.lease_expires_turn = config->standing_lease_turns;
        }
        if (config->standing_lease_seconds > 0) {
            tracker.lease_granted_time = std::chrono::steady_clock::now();
        }
    }

    std::unordered_map<std::string, NaabVal> handle;
    handle["__agent_handle"] = NaabVal::makeBool(true);
    handle["__nonce"] = NaabVal::makeString(nonce);
    handle["id"] = NaabVal::makeInt(handle_id);
    handle["config_name"] = NaabVal::makeString(config_name);
    handle["messages"] = NaabVal::makeList({});
    handle["input_tokens"] = NaabVal::makeInt(0);
    handle["output_tokens"] = NaabVal::makeInt(0);
    handle["turns"] = NaabVal::makeInt(0);

    // Environment self-awareness: agent knows its limits from birth
    handle["environment"] = buildEnvironmentDict(handle_id, config_name);

    return NaabVal::makeDict(std::move(handle));
}

// ============================================================================
// agent.extract_code(response, lang) → extracted code string
// ============================================================================

static NaabVal agentExtractCode(std::vector<NaabVal>& args) {
    if (args.size() < 1) {
        throw std::runtime_error(
            "Agent error: agent.extract_code requires a response string\n\n"
            "  Expected: agent.extract_code(response, lang='')\n\n"
            "  Example:\n"
            "    let code = agent.extract_code(response, \"python\")\n");
    }

    if (!args[0].isString()) {
        throw std::runtime_error(
            "Agent error: agent.extract_code response must be a string\n\n"
            "  Got: " + std::string(args[0].isNull() ? "null" : "non-string") + "\n\n"
            "  Example:\n"
            "    agent.extract_code(\"```python\\nprint('hi')\\n```\", \"python\")\n");
    }

    std::string response = args[0].asString();
    std::string lang_hint = args.size() > 1 && args[1].isString() ? args[1].asString() : "";

    if (response.empty()) {
        return NaabVal::makeString("");
    }

    // Search for code fences (```language ... ```)
    auto fence_start = response.find("```");
    if (fence_start == std::string::npos) {
        // No fence found — return response as-is
        return NaabVal::makeString(response);
    }

    // Find the end of the opening fence line (language tag)
    auto line_end = response.find('\n', fence_start);
    if (line_end == std::string::npos || line_end <= fence_start + 3) {
        // No newline after fence or invalid fence format
        return NaabVal::makeString(response);
    }

    // Extract the language tag (if any)
    std::string fence_line = response.substr(fence_start + 3, line_end - fence_start - 3);
    // Trim whitespace from fence line to get language
    while (!fence_line.empty() && (fence_line.back() == ' ' || fence_line.back() == '\t' ||
           fence_line.back() == '\r' || fence_line.back() == '\n')) {
        fence_line.pop_back();
    }
    while (!fence_line.empty() && (fence_line.front() == ' ' || fence_line.front() == '\t')) {
        fence_line.erase(fence_line.begin());
    }

    // If lang_hint is provided and fence has a language, prefer matching
    // If no lang_hint, use the fence's language (or empty for no language)
    bool lang_matches = true;
    if (!lang_hint.empty() && !fence_line.empty()) {
        // Check if the fence language matches the hint (case-insensitive, substring)
        std::string hint_lower = lang_hint;
        std::string fence_lower = fence_line;
        std::transform(hint_lower.begin(), hint_lower.end(), hint_lower.begin(), ::tolower);
        std::transform(fence_lower.begin(), fence_lower.end(), fence_lower.begin(), ::tolower);
        lang_matches = (fence_lower.find(hint_lower) != std::string::npos ||
                        hint_lower.find(fence_lower) != std::string::npos);
    }

    if (!lang_matches && !lang_hint.empty()) {
        // Fence language doesn't match hint — skip this fence and look for next
        // This is a more sophisticated version that can handle multiple fences
        // For now, fall back to returning the response as-is if lang doesn't match
        return NaabVal::makeString(response);
    }

    // Find the closing fence
    auto fence_end = response.find("```", line_end);
    if (fence_end == std::string::npos || fence_end <= line_end) {
        // No closing fence or malformed
        return NaabVal::makeString(response);
    }

    // Extract code between opening and closing fence lines
    std::string extracted = response.substr(line_end + 1, fence_end - line_end - 1);

    // Strip trailing whitespace/newlines from extracted code
    while (!extracted.empty() && (extracted.back() == '\n' || extracted.back() == '\r' ||
           extracted.back() == ' ' || extracted.back() == '\t')) {
        extracted.pop_back();
    }

    // Strip leading whitespace/newlines from extracted code
    while (!extracted.empty() && (extracted.front() == '\n' || extracted.front() == '\r' ||
           extracted.front() == ' ' || extracted.front() == '\t')) {
        extracted.erase(extracted.begin());
    }

    return NaabVal::makeString(extracted);
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

    // Validate handle (checks __agent_handle marker + HMAC nonce)
    auto [config_name, validated_id] = validateHandle(args[0]);
    auto& handle = args[0].asDict();

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
    int handle_id = validated_id;
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

        // Standing Lease: check if authorization has expired
        if (tracker.lease_expires_turn > 0 && tracker.turns >= tracker.lease_expires_turn) {
            // Lease expired — agent must re-authorize via step-up challenge
            // The step-up challenge below will handle re-authorization if enabled.
            // If step-up is not enabled, we throw — the agent cannot continue.
            auto* gov_engine = governance::GovernanceEngine::getCurrent();
            const auto& cb_cfg = gov_engine ? gov_engine->getRules().circuit_breaker
                                             : governance::CircuitBreakerConfig{};
            if (!cb_cfg.step_up_enabled) {
                throw std::runtime_error(fmt::format(
                    "Agent error: Standing lease expired at turn {}\n\n"
                    "  Got: {} turns used, lease granted for {} turns\n\n"
                    "  Help:\n"
                    "  - Create a new agent handle for a fresh conversation\n"
                    "  - Or enable step_up challenges to allow lease renewal\n",
                    tracker.lease_expires_turn, tracker.turns,
                    config->standing_lease_turns));
            }
            // Mark that a forced step-up is needed (handled below in step-up section)
        }

        // Wall-clock lease: check if time-based authorization has expired
        if (config->standing_lease_seconds > 0) {
            auto elapsed = std::chrono::steady_clock::now() - tracker.lease_granted_time;
            int elapsed_sec = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
            if (elapsed_sec >= config->standing_lease_seconds) {
                auto* gov_engine = governance::GovernanceEngine::getCurrent();
                const auto& cb_cfg = gov_engine ? gov_engine->getRules().circuit_breaker
                                                 : governance::CircuitBreakerConfig{};
                if (!cb_cfg.step_up_enabled) {
                    throw std::runtime_error(fmt::format(
                        "Agent error: Wall-clock lease expired after {} seconds\n\n"
                        "  Help:\n"
                        "  - Create a new agent handle for a fresh conversation\n"
                        "  - Or enable step_up challenges to allow lease renewal\n",
                        elapsed_sec));
                }
                // Falls through to step-up challenge handling below
            }
        }

        // Enforce token budget from tracker (not handle dict)
        int total_used = tracker.input_tokens + tracker.output_tokens;
        if (config->max_total_tokens > 0 && total_used >= config->max_total_tokens) {
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

    // Hard stop check
    if (s_dispatch.hard_stopped.load()) {
        std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
        throw std::runtime_error(
            "Agent error: Hard stop active\n\n"
            "  Got: " + s_dispatch.stop_reason + "\n\n"
            "  Help:\n"
            "  - All agent API calls are blocked for the remainder of this run\n"
            "  - Review agent_dispatch.hard_stop configuration\n");
    }

    // Build key list and model chain (normalize string → vector)
    std::vector<std::string> keys = config->api_key_envs;
    if (keys.empty()) keys.push_back(config->api_key_env);
    std::vector<std::string> models = config->model_chain;
    if (models.empty()) models.push_back(config->model);

    // Validate at least one key is resolvable
    bool any_key = false;
    for (const auto& k : keys) {
        if (!runtime::resolveApiKey(k).empty()) { any_key = true; break; }
    }
    if (!any_key) {
        throw std::runtime_error(
            "Agent error: API key not available\n\n"
            "  Help:\n"
            "  - Set the required API key environment variable, or\n"
            "  - Place the key in ~/.naab/keys/" + keys[0] + "\n");
    }

    // Mid-run governance reload check (Governance Under Survivability)
    auto* gov_engine = governance::GovernanceEngine::getCurrent();
    std::vector<std::string> gov_notices;
    if (gov_engine && gov_engine->isActive()) {
        gov_engine->reloadIfChanged();
        gov_notices = gov_engine->getAndClearNotices();
        // Re-lookup config: reloadIfChanged() may have swapped rules_.agents,
        // invalidating the pointer obtained before reload.
        config = findAgentConfig(config_name);
        if (!config) {
            throw std::runtime_error(fmt::format(
                "Agent error: Agent config '{}' removed during governance reload\n\n"
                "  Help:\n"
                "  - The governance configuration was reloaded mid-run\n"
                "  - The agent config is no longer available\n",
                config_name));
        }
    }

    // Behavioral sequence: emit agent.send event (once, before retry loop)
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().behavioral_sequences.enabled) {
        gov_engine->setAgentContext(handle_id, current_turn, config_name);
        std::string bsd_block = gov_engine->emitEvent(governance::RuntimeEventType::AGENT_SEND,
            "agent.send('" + config_name + "')", "", 0);
        if (!bsd_block.empty()) {
            throw std::runtime_error(bsd_block);
        }
    }

    // ── Prompt-side governance: scan outbound message before API call ──
    if (gov_engine && gov_engine->isActive() && !message.empty()) {
        std::string secret_err = gov_engine->checkSecrets(message, 0);
        if (!secret_err.empty()) {
            throw std::runtime_error(fmt::format(
                "Agent error: Prompt for '{}' contains a potential secret\n\n"
                "  Help:\n"
                "  - The outbound message was blocked because it contains a pattern\n"
                "    matching a known secret format (API key, token, etc.)\n"
                "  - Sanitize sensitive data before sending to external LLM APIs\n",
                config_name));
        }
        std::string pii_err = gov_engine->checkPii(message, 0);
        if (!pii_err.empty()) {
            throw std::runtime_error(fmt::format(
                "Agent error: Prompt for '{}' contains potential PII\n\n"
                "  Help:\n"
                "  - The outbound message was blocked because it contains a pattern\n"
                "    matching personally identifiable information\n"
                "  - Remove or mask PII before sending to external LLM APIs\n",
                config_name));
        }
        std::string info_err = gov_engine->checkInfoDisclosure("naab", message, 0);
        if (!info_err.empty()) {
            throw std::runtime_error(fmt::format(
                "Agent error: Prompt for '{}' contains information disclosure patterns\n\n"
                "  Help:\n"
                "  - The outbound message was blocked because it contains patterns\n"
                "    matching environment dumps, process listings, or system info\n"
                "  - Remove sensitive system information before sending to external LLM APIs\n",
                config_name));
        }
    }

    // Telemetry: prompt scan passed — proof that pre-send governance ran this turn
    if (gov_engine && gov_engine->isActive()) {
        gov_engine->writeAgentTelemetry("PROMPT_SCAN", {
            {"handle_id",      std::to_string(handle_id)},
            {"config_name",    config_name},
            {"turn",           std::to_string(current_turn)},
            {"message_length", std::to_string(message.size())},
            {"checks",         "no_secrets,no_pii,no_info_disclosure"},
            {"result",         "pass"}
        });
    }

    // Pre-call admission check
    if (gov_engine && gov_engine->isActive()) {
        std::string admission = gov_engine->checkAdmission(config_name);
        if (!admission.empty()) {
            throw std::runtime_error(admission);
        }
        // Per-agent action matrix: check AGENT_SEND
        if (config && !config->allowed_actions.empty()) {
            bool allowed = false;
            for (const auto& a : config->allowed_actions) {
                if (a == "AGENT_SEND") { allowed = true; break; }
            }
            if (!allowed) {
                throw std::runtime_error(
                    "Agent error: Action matrix does not include AGENT_SEND\n\n"
                    "  Help:\n"
                    "  - This agent's allowed_actions list does not permit sending messages\n"
                    "  - Add AGENT_SEND to the agent's allowed_actions configuration\n");
            }
        }
        // Telemetry: admission gate passed — exposes exposure tracking state for audit
        {
            const char* adm_level_names[] = {"normal", "elevated", "high", "critical"};
            auto adm_lvl = static_cast<int>(gov_engine->getGovernanceLevel());
            const char* adm_level_str = (adm_lvl >= 0 && adm_lvl <= 3)
                ? adm_level_names[adm_lvl] : "unknown";
            gov_engine->writeAgentTelemetry("ADMISSION_EVAL", {
                {"handle_id",             std::to_string(handle_id)},
                {"config_name",           config_name},
                {"turn",                  std::to_string(current_turn)},
                {"result",                "pass"},
                {"risk_budget_remaining", std::to_string(gov_engine->getRemainingBudget(config_name))},
                {"autonomous_actions",    std::to_string(gov_engine->getAutonomousActionCount())},
                {"unique_agents",         std::to_string(gov_engine->getUniqueAgentCount())},
                {"governance_level",      adm_level_str}
            });
        }
    }

    // ── Step-up challenge: at elevated governance or expired lease ──
    if (gov_engine && gov_engine->isActive()) {
        const auto& cb = gov_engine->getRules().circuit_breaker;
        if (cb.step_up_enabled) {
            auto level = gov_engine->getGovernanceLevel();
            int required_level = (cb.step_up_at_level == "high") ? 2 : 1;
            // Standing Lease: force step-up when lease expired (regardless of gov level)
            bool lease_expired = false;
            {
                std::lock_guard<std::mutex> lock(s_agent_mutex);
                auto it = s_trackers.find(handle_id);
                if (it != s_trackers.end()) {
                    if (it->second.lease_expires_turn > 0) {
                        lease_expired = (it->second.turns >= it->second.lease_expires_turn);
                    }
                    // Wall-clock lease check
                    if (!lease_expired && config->standing_lease_seconds > 0) {
                        auto elapsed = std::chrono::steady_clock::now() - it->second.lease_granted_time;
                        int elapsed_sec = static_cast<int>(
                            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
                        lease_expired = (elapsed_sec >= config->standing_lease_seconds);
                    }
                }
            }
            if (static_cast<int>(level) >= required_level || lease_expired) {
                // Check cooldown from tracker (server-side, immune to handle mutation)
                bool should_challenge = false;
                {
                    std::lock_guard<std::mutex> lock(s_agent_mutex);
                    auto it = s_trackers.find(handle_id);
                    if (it != s_trackers.end()) {
                        should_challenge = lease_expired ||  // always challenge on lease expiry
                            (current_turn - it->second.last_challenge_turn) >= cb.step_up_cooldown_turns;
                    }
                }
                if (should_challenge) {
                    // Find a usable key for the challenge call
                    std::string challenge_key;
                    for (const auto& k : keys) {
                        std::string resolved = runtime::resolveApiKey(k);
                        if (!resolved.empty()) {
                            std::lock_guard<std::mutex> lock(s_dispatch.dead_keys_mutex);
                            if (!isKeyDead(k, config->retry.key_retry_after_seconds)) {
                                challenge_key = resolved;
                                break;
                            }
                        }
                    }
                    if (!challenge_key.empty()) {
                        // Build challenge message
                        json challenge_msgs = json::array();
                        if (!config->system_prompt.empty()) {
                            json sys_msg;
                            sys_msg["role"] = "system";
                            sys_msg["content"] = config->system_prompt;
                            challenge_msgs.push_back(sys_msg);
                        }
                        json challenge_user;
                        challenge_user["role"] = "user";
                        challenge_user["content"] = cb.step_up_challenge;
                        challenge_msgs.push_back(challenge_user);

                        governance::AgentConfig challenge_config = *config;
                        challenge_config.model = models[0];

                        s_dispatch.total_calls++;
                        auto challenge_result = runtime::callAgentWithStatus(
                            challenge_config, challenge_key, challenge_msgs.dump());

                        bool passed = false;
                        if (challenge_result.response.success) {
                            passed = scoreStepUpChallenge(
                                challenge_result.response.content,
                                config->system_prompt,
                                cb.step_up_min_words,
                                cb.step_up_keyword_threshold);
                        }

                        // Update tracker state (server-side)
                        {
                            std::lock_guard<std::mutex> lock(s_agent_mutex);
                            auto it = s_trackers.find(handle_id);
                            if (it != s_trackers.end()) {
                                it->second.last_challenge_turn = current_turn;
                                if (passed) {
                                    it->second.challenges_passed++;
                                    // Standing Lease: renew lease on successful challenge
                                    if (config->standing_lease_turns > 0) {
                                        it->second.lease_granted_turn = current_turn;
                                        it->second.lease_expires_turn = current_turn + config->standing_lease_turns;
                                    }
                                    if (config->standing_lease_seconds > 0) {
                                        it->second.lease_granted_time = std::chrono::steady_clock::now();
                                    }
                                } else {
                                    it->second.challenges_failed++;
                                }
                            }
                        }

                        if (passed) {
                            gov_engine->recoverCoherence(handle_id);
                            gov_engine->writeAgentTelemetry("AGENT_CHALLENGE_PASS", {
                                {"agent", config_name},
                                {"turn", std::to_string(current_turn)}
                            });
                        } else {
                            gov_engine->writeAgentTelemetry("AGENT_CHALLENGE_FAIL", {
                                {"agent", config_name},
                                {"turn", std::to_string(current_turn)}
                            });
                            throw std::runtime_error(
                                "Agent error: Step-up challenge failed\n\n"
                                "  Help:\n"
                                "  - The agent could not demonstrate coherence with its task\n"
                                "  - This check triggers at elevated governance levels\n"
                                "  - Review the agent's behavior and configuration\n");
                        }
                    }
                }
            }
        }
    }

    // ── Build tool definitions for initial API call (if tools enabled) ──
    std::vector<runtime::ToolDefinition> initial_tool_defs;
    if (config && config->tools_enabled && !config->tools.empty()) {
        std::lock_guard<std::mutex> lock(s_tools_mutex);
        for (const auto& tool_name : config->tools) {
            auto it = s_registered_tools.find(tool_name);
            if (it != s_registered_tools.end()) {
                initial_tool_defs.push_back({
                    it->second.name,
                    it->second.description,
                    it->second.input_schema.dump()
                });
            }
        }
    }

    // ── Retry loop with key rotation, model fallback, backoff + jitter ──
    runtime::AgentResponse agent_resp;
    int max_attempts = config->retry.max_attempts;
    int model_idx = 0;
    int key_offset = 0;
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto koff_it = s_trackers.find(handle_id);
        if (koff_it != s_trackers.end()) key_offset = koff_it->second.key_offset;
    }
    int attempts_made = 0;
    std::string last_error;
    std::string messages_str = messages_json.dump();

    // Get hard stop config
    const auto& hs = gov_engine && gov_engine->isActive()
        ? gov_engine->getRules().agent_dispatch.hard_stop
        : governance::GovernanceRules::AgentDispatchConfig::HardStopConfig{};

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        // Pick key (round-robin, skip dead)
        std::string key_env;
        std::string api_key;
        bool found_key = false;
        int key_offset_before = key_offset;  // save for empty-response restore
        for (size_t k = 0; k < keys.size(); k++) {
            size_t idx = (key_offset + k) % keys.size();
            {
                bool was_revived = false;
                std::lock_guard<std::mutex> lock(s_dispatch.dead_keys_mutex);
                if (isKeyDead(keys[idx], config->retry.key_retry_after_seconds, &was_revived)) continue;
                if (was_revived && gov_engine && gov_engine->isActive()) {
                    gov_engine->writeAgentTelemetry("AGENT_KEY_REVIVED", {
                        {"api_key_env", keys[idx]},
                        {"cooldown_seconds", std::to_string(config->retry.key_retry_after_seconds)}
                    });
                }
            }
            api_key = runtime::resolveApiKey(keys[idx]);
            if (!api_key.empty()) {
                key_env = keys[idx];
                key_offset = static_cast<int>(idx) + 1;
                found_key = true;
                break;
            }
        }
        if (!found_key) {
            last_error = "All API keys exhausted or dead";
            break;
        }

        // Pick model from chain
        std::string current_model = models[model_idx % models.size()];

        // Build temporary config with current model
        governance::AgentConfig call_config = *config;
        call_config.model = current_model;

        // Rate limit: inter-call delay
        if (config->rate_limit.delay_between_calls_ms > 0 && attempt > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config->rate_limit.delay_between_calls_ms));
        }

        attempts_made++;
        s_dispatch.total_calls++;

        // Run-level call budget check
        if (hs.max_calls_per_run > 0 && s_dispatch.total_calls.load() > hs.max_calls_per_run) {
            {
                std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
                s_dispatch.hard_stopped = true;
                s_dispatch.stop_reason = "max_calls_per_run (" + std::to_string(hs.max_calls_per_run) + ") exceeded";
            }
            throw std::runtime_error("Agent error: Hard stop — " + s_dispatch.stop_reason);
        }

        auto attempt_start = std::chrono::steady_clock::now();
        // Use callAgentWithTools when tool definitions are available (sends tool schema to LLM)
        auto result = initial_tool_defs.empty()
            ? runtime::callAgentWithStatus(call_config, api_key, messages_str)
            : runtime::callAgentWithTools(call_config, api_key, messages_str, initial_tool_defs);
        auto attempt_end = std::chrono::steady_clock::now();
        int64_t attempt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            attempt_end - attempt_start).count();

        s_dispatch.total_agent_time_ms += attempt_ms;

        // Time budget check
        if (hs.max_agent_time_ms > 0 && s_dispatch.total_agent_time_ms.load() > hs.max_agent_time_ms) {
            std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
            s_dispatch.hard_stopped = true;
            s_dispatch.stop_reason = "max_agent_time_ms (" + std::to_string(hs.max_agent_time_ms) + ") exceeded";
            if (!result.response.success) {
                throw std::runtime_error("Agent error: Hard stop — " + s_dispatch.stop_reason);
            }
        }

        if (result.response.success) {
            // Treat zero-token empty responses as a soft failure — retry if budget allows.
            // Free-tier providers occasionally return HTTP 200 with no content.
            // Guard is BEFORE consecutive_failures reset to preserve real failure history.
            if (result.response.output_tokens == 0 && result.response.content.empty()
                    && attempt + 1 < max_attempts) {
                last_error = "empty response (0 output tokens)";
                s_dispatch.total_retries++;
                key_offset = key_offset_before;  // don't rotate key for empty response
                if (config->retry.backoff_ms > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(config->retry.backoff_ms));
                continue;
            }

            // Success — reset consecutive failures, populate trace
            s_dispatch.consecutive_failures = 0;
            s_dispatch.total_tokens += result.response.input_tokens + result.response.output_tokens;

            // Token budget check (don't throw — this call succeeded, next will be blocked)
            if (hs.max_tokens_per_run > 0 && s_dispatch.total_tokens.load() > hs.max_tokens_per_run) {
                std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
                s_dispatch.hard_stopped = true;
                s_dispatch.stop_reason = "max_tokens_per_run (" + std::to_string(hs.max_tokens_per_run) + ") exceeded";
            }

            agent_resp = result.response;
            agent_resp.actual_model = current_model;
            agent_resp.actual_provider = config->provider;
            agent_resp.actual_api_key_env = key_env;
            agent_resp.latency_ms = attempt_ms;
            agent_resp.attempts = attempts_made;
            agent_resp.fallback_used = (model_idx > 0);
            agent_resp.original_model = models[0];

            // Telemetry: successful response
            if (gov_engine && gov_engine->isActive()) {
                gov_engine->writeAgentTelemetry("AGENT_RESPONSE", {
                    {"handle_id", std::to_string(handle_id)},
                    {"model", current_model},
                    {"api_key_env", key_env},
                    {"latency_ms", std::to_string(attempt_ms)},
                    {"input_tokens", std::to_string(agent_resp.input_tokens)},
                    {"output_tokens", std::to_string(agent_resp.output_tokens)},
                    {"attempts", std::to_string(attempts_made)},
                    {"fallback_used", agent_resp.fallback_used ? "true" : "false"}
                });
            }
            break;
        }

        // Failure — classify error by HTTP status
        int status = result.http_status;
        last_error = result.response.error;
        s_dispatch.total_retries++;

        // Classify error before incrementing consecutive failures.
        // Key rotation errors (Gemini 400 "API key not valid") are expected
        // during rotation — they shouldn't count toward consecutive failures
        // or trigger hard stop (especially in parallel dispatch where multiple
        // threads rotate keys simultaneously).
        bool should_never_retry = false;
        bool is_key_error_400 = false;
        for (int code : config->retry.never_retry) {
            if (status == code) { should_never_retry = true; break; }
        }
        if (should_never_retry && status == 400 &&
            (last_error.find("API key not valid") != std::string::npos ||
             last_error.find("API_KEY_INVALID") != std::string::npos)) {
            should_never_retry = false;
            is_key_error_400 = true;
        }

        // Key skip errors (401 or 400-key-error) don't count as consecutive failures
        bool is_key_skip = is_key_error_400;
        if (!is_key_skip) {
            for (int code : config->retry.skip_key_on) {
                if (status == code) { is_key_skip = true; break; }
            }
        }
        if (!is_key_skip) {
            s_dispatch.consecutive_failures++;
        }

        // Telemetry: retry event
        if (gov_engine && gov_engine->isActive()) {
            gov_engine->writeAgentTelemetry("AGENT_RETRY", {
                {"handle_id", std::to_string(handle_id)},
                {"attempt", std::to_string(attempt + 1)},
                {"http_status", std::to_string(status)},
                {"api_key_env", key_env},
                {"model", current_model}
            });
        }

        // Consecutive failure hard stop
        if (hs.consecutive_failure_limit > 0 &&
            s_dispatch.consecutive_failures.load() >= hs.consecutive_failure_limit) {
            {
                std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
                s_dispatch.hard_stopped = true;
                s_dispatch.stop_reason = "consecutive_failure_limit (" +
                    std::to_string(hs.consecutive_failure_limit) + ") reached";
            }
            if (gov_engine && gov_engine->isActive() &&
                gov_engine->getRules().context_drift.enabled) {
                gov_engine->checkContextDrift(handle_id, current_turn, "api_call_failed");
            }
            if (gov_engine && gov_engine->isActive()) {
                gov_engine->writeAgentTelemetry("AGENT_HARD_STOP", {
                    {"reason", s_dispatch.stop_reason},
                    {"total_calls", std::to_string(s_dispatch.total_calls.load())},
                    {"consecutive_failures", std::to_string(s_dispatch.consecutive_failures.load())}
                });
            }
            throw std::runtime_error("Agent error: Hard stop — " + s_dispatch.stop_reason +
                "\n\n  Last error: " + last_error);
        }

        // Never retry: abort immediately (real 400 bad request, not key error)
        if (should_never_retry) {
            if (gov_engine && gov_engine->isActive() &&
                gov_engine->getRules().context_drift.enabled) {
                gov_engine->checkContextDrift(handle_id, current_turn, last_error);
            }
            throw std::runtime_error(last_error);
        }

        // Mark dead key for rest of run (already classified above as is_key_skip)
        if (is_key_skip) {
            {
                std::lock_guard<std::mutex> lock(s_dispatch.dead_keys_mutex);
                s_dispatch.dead_keys[key_env] = std::chrono::steady_clock::now();
            }
            if (gov_engine && gov_engine->isActive()) {
                gov_engine->writeAgentTelemetry("AGENT_KEY_DISABLED", {
                    {"api_key_env", key_env},
                    {"http_status", std::to_string(status)}
                });
            }
        }

        // Fallback model on 404/503
        for (int code : config->retry.fallback_model_on) {
            if (status == code && model_idx + 1 < static_cast<int>(models.size())) {
                if (gov_engine && gov_engine->isActive()) {
                    gov_engine->writeAgentTelemetry("AGENT_FALLBACK", {
                        {"handle_id", std::to_string(handle_id)},
                        {"from_model", models[model_idx]},
                        {"to_model", models[model_idx + 1]},
                        {"http_status", std::to_string(status)}
                    });
                }
                model_idx++;
                break;
            }
        }

        // Backoff with jitter before next attempt
        if (attempt + 1 < max_attempts) {
            int delay = config->retry.backoff_ms;
            for (int b = 0; b < attempt; b++) {
                delay = static_cast<int>(delay * config->retry.backoff_multiplier);
                if (delay > 60000) { delay = 60000; break; }  // cap at 60s
            }
            if (config->retry.jitter && delay > 0) {
                int jitter_range = delay / 4;
                if (jitter_range > 0) {
                    delay += (std::rand() % (2 * jitter_range + 1)) - jitter_range;
                }
            }
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
    }

    // All attempts exhausted
    if (!agent_resp.success) {
        if (gov_engine && gov_engine->isActive() &&
            gov_engine->getRules().context_drift.enabled) {
            gov_engine->checkContextDrift(handle_id, current_turn, "api_call_failed");
        }
        throw std::runtime_error(fmt::format(
            "Agent error: All {} attempts exhausted\n\n"
            "  Last error: {}\n"
            "  Models tried: {}\n\n"
            "  Help:\n"
            "  - Check API key validity and quota\n"
            "  - Review retry configuration in govern.json agents section\n",
            attempts_made, last_error, model_idx + 1));
    }

    std::string content = agent_resp.content;

    // Strip markdown code fences from LLM responses.
    // LLMs frequently wrap JSON/code in ```json ... ``` or ``` ... ```
    // which breaks json.parse() downstream. Strip them transparently.
    if (!content.empty()) {
        // Find leading fence: ``` optionally followed by a language tag
        auto fence_start = content.find("```");
        if (fence_start != std::string::npos) {
            // Check if there's a closing fence
            auto fence_end = content.rfind("```");
            if (fence_end != std::string::npos && fence_end > fence_start + 3) {
                // Skip past the opening fence line (```json\n or ```\n)
                auto line_end = content.find('\n', fence_start);
                if (line_end != std::string::npos && line_end < fence_end) {
                    // Only strip if fence is near the start (allow leading whitespace/newlines)
                    bool only_whitespace_before = true;
                    for (size_t ci = 0; ci < fence_start; ++ci) {
                        if (content[ci] != ' ' && content[ci] != '\t' &&
                            content[ci] != '\n' && content[ci] != '\r') {
                            only_whitespace_before = false;
                            break;
                        }
                    }
                    // Only strip if fence is near the end too
                    bool only_whitespace_after = true;
                    for (size_t ci = fence_end + 3; ci < content.size(); ++ci) {
                        if (content[ci] != ' ' && content[ci] != '\t' &&
                            content[ci] != '\n' && content[ci] != '\r') {
                            only_whitespace_after = false;
                            break;
                        }
                    }
                    if (only_whitespace_before && only_whitespace_after) {
                        // Extract content between opening fence line and closing fence
                        content = content.substr(line_end + 1, fence_end - line_end - 1);
                        // Trim trailing whitespace/newlines
                        while (!content.empty() && (content.back() == '\n' ||
                               content.back() == '\r' || content.back() == ' ')) {
                            content.pop_back();
                        }
                    }
                }
            }
        }
    }

    std::string stop_reason = agent_resp.stop_reason;
    int resp_input_tokens = agent_resp.input_tokens;
    int resp_output_tokens = agent_resp.output_tokens;

    // ── Governed Tool Execution Loop ──
    // When the LLM returns tool_use/FUNCTION_CALL, execute registered tool functions
    // and send results back. Every step passes through 7 defense layers.
    int tool_calls_this_send = 0;
    int tool_blocked_this_send = 0;
    int tool_loop_turns = 0;
    int tool_total_result_chars = 0;
    std::vector<NaabVal> tool_results_summary;  // for response dict
    std::string tool_loop_exit_reason = "text_response";

    // Check if this is a tool_use response AND tools are enabled
    bool is_tool_response = (stop_reason == "tool_use" || stop_reason == "FUNCTION_CALL" ||
                             stop_reason == "tool_calls" || !agent_resp.tool_calls.empty());

    if (is_tool_response && config && !config->tools_enabled) {
        // Tools not enabled — hard block as before
        throw std::runtime_error(fmt::format(
            "Agent error: Agent '{}' returned a tool_use response but tools are not enabled\n\n"
            "  Help:\n"
            "  - Tool execution must be enabled in the agent configuration\n"
            "  - Register tool functions before sending messages\n",
            config_name));
    }

    if (is_tool_response && config && config->tools_enabled) {
        // Layer 1D: Check TOOL_EXEC in action matrix
        if (!config->allowed_actions.empty()) {
            bool tool_exec_allowed = false;
            for (const auto& a : config->allowed_actions) {
                if (a == "TOOL_EXEC") { tool_exec_allowed = true; break; }
            }
            if (!tool_exec_allowed) {
                if (gov_engine) {
                    gov_engine->emitEvent(governance::RuntimeEventType::TOOL_BLOCKED,
                        "tool_exec_not_in_action_matrix(" + config_name + ")", "", 0);
                }
                throw std::runtime_error(fmt::format(
                    "Agent error: Agent '{}' attempted tool execution but TOOL_EXEC is not in allowed_actions\n\n"
                    "  Help:\n"
                    "  - Add TOOL_EXEC to the agent's allowed_actions in govern.json\n",
                    config_name));
            }
        }

        // Get the tool registration snapshot for this handle
        std::unordered_map<std::string, ToolRegistration> tool_snapshot;
        {
            std::lock_guard<std::mutex> lock(s_tools_mutex);
            // Only include tools that are in BOTH the govern.json allowlist AND registered
            for (const auto& tool_name : config->tools) {
                auto it = s_registered_tools.find(tool_name);
                if (it != s_registered_tools.end()) {
                    tool_snapshot[tool_name] = it->second;
                }
            }
        }

        // Build tool definitions for the provider
        std::vector<runtime::ToolDefinition> tool_defs;
        for (const auto& [name, reg] : tool_snapshot) {
            tool_defs.push_back({name, reg.description, reg.input_schema.dump()});
        }

        // Emit TOOL_LOOP_START telemetry
        if (gov_engine && gov_engine->isActive()) {
            gov_engine->writeAgentTelemetry("AGENT_TOOL_LOOP_START", {
                {"handle_id", std::to_string(handle_id)},
                {"tool_count", std::to_string(tool_snapshot.size())}
            });
        }

        // ── TOOL LOOP ──
        while (is_tool_response && tool_loop_turns < config->max_tool_loop_turns) {
            tool_loop_turns++;

            // E1: Empty tool_calls with tool_use stop_reason
            if (agent_resp.tool_calls.empty()) {
                tool_loop_exit_reason = "empty_tool_calls";
                break;
            }

            // Process each tool call in this response
            // Build Anthropic-style content array for assistant turn
            json assistant_content = json::array();
            // Preserve any text content alongside tool calls (Gap N)
            if (!content.empty()) {
                assistant_content.push_back({{"type", "text"}, {"text", content}});
            }

            json tool_result_blocks = json::array();

            for (const auto& tc : agent_resp.tool_calls) {
                // Budget check: max_tool_calls_per_turn
                if (tool_calls_this_send >= config->max_tool_calls_per_turn) {
                    tool_loop_exit_reason = "max_tool_calls_per_turn";
                    break;
                }

                // Step 0: Tool name validation (Gap I)
                if (!isValidToolName(tc.name)) {
                    if (gov_engine) {
                        gov_engine->emitEvent(governance::RuntimeEventType::TOOL_BLOCKED,
                            "tool_name_invalid", "", 0);
                    }
                    json error_result;
                    error_result["type"] = "tool_result";
                    error_result["tool_use_id"] = tc.id;
                    error_result["content"] = "Error: invalid tool name";
                    error_result["is_error"] = true;
                    tool_result_blocks.push_back(error_result);
                    tool_calls_this_send++;
                    tool_blocked_this_send++;
                    continue;
                }

                // Step 1: Dual-gate allowlist check
                auto snap_it = tool_snapshot.find(tc.name);
                if (snap_it == tool_snapshot.end()) {
                    // Tool not in allowlist or not registered
                    if (gov_engine) {
                        gov_engine->emitEvent(governance::RuntimeEventType::TOOL_BLOCKED,
                            "tool_not_allowed(" + tc.name + ")", "", 0);
                        gov_engine->writeAgentTelemetry("AGENT_TOOL_BLOCKED", {
                            {"tool_name", tc.name},
                            {"reason", "not_in_allowlist"},
                            {"handle_id", std::to_string(handle_id)}
                        });
                    }
                    json error_result;
                    error_result["type"] = "tool_result";
                    error_result["tool_use_id"] = tc.id;
                    error_result["content"] = "Error: tool not available";
                    error_result["is_error"] = true;
                    tool_result_blocks.push_back(error_result);
                    tool_calls_this_send++;
                    tool_blocked_this_send++;

                    // Track for response dict
                    std::unordered_map<std::string, NaabVal> ts;
                    ts["name"] = NaabVal::makeString(tc.name);
                    ts["success"] = NaabVal::makeBool(false);
                    ts["error"] = NaabVal::makeString("not_in_allowlist");
                    tool_results_summary.push_back(NaabVal::makeDict(std::move(ts)));
                    continue;
                }

                // Step 2: Parse arguments with depth guard (Gap J)
                json tool_args;
                try {
                    tool_args = json::parse(tc.arguments);
                } catch (const std::exception&) {
                    if (gov_engine) {
                        gov_engine->emitEvent(governance::RuntimeEventType::TOOL_ERROR,
                            "tool_args_parse_failed(" + tc.name + ")", "", 0);
                    }
                    json error_result;
                    error_result["type"] = "tool_result";
                    error_result["tool_use_id"] = tc.id;
                    error_result["content"] = "Error: invalid arguments";
                    error_result["is_error"] = true;
                    tool_result_blocks.push_back(error_result);
                    tool_calls_this_send++;
                    tool_blocked_this_send++;
                    continue;
                }

                // Step 3: Scan arguments for secrets/PII (Gap B)
                if (gov_engine && gov_engine->isActive()) {
                    std::string secret_err = gov_engine->checkSecrets(tc.arguments, 0);
                    if (!secret_err.empty()) {
                        gov_engine->emitEvent(governance::RuntimeEventType::TOOL_BLOCKED,
                            "tool_args_secrets(" + tc.name + ")", "", 0);
                        json error_result;
                        error_result["type"] = "tool_result";
                        error_result["tool_use_id"] = tc.id;
                        error_result["content"] = "Error: arguments contain potential secrets";
                        error_result["is_error"] = true;
                        tool_result_blocks.push_back(error_result);
                        tool_calls_this_send++;
                        tool_blocked_this_send++;
                        continue;
                    }
                }

                // Emit TOOL_CALL BSD event (Gap F)
                if (gov_engine) {
                    gov_engine->emitEvent(governance::RuntimeEventType::TOOL_CALL,
                        "tool_call('" + tc.name + "')", "", 0);
                    gov_engine->writeAgentTelemetry("AGENT_TOOL_CALL", {
                        {"tool_name", tc.name},
                        {"handle_id", std::to_string(handle_id)},
                        {"tool_loop_turn", std::to_string(tool_loop_turns)},
                        {"args_size", std::to_string(tc.arguments.size())}
                    });
                }

                // Step 4-5: Execute tool function under scoped agent restrictions
                auto tool_start = std::chrono::steady_clock::now();
                std::string tool_result_str;
                bool tool_success = true;

                // Convert JSON args to NaabVal vector
                std::vector<NaabVal> naab_args;
                if (tool_args.is_object()) {
                    // Pass properties as positional args in schema order
                    if (snap_it->second.input_schema.contains("properties")) {
                        for (auto& [key, schema] : snap_it->second.input_schema["properties"].items()) {
                            if (tool_args.contains(key)) {
                                auto& val = tool_args[key];
                                if (val.is_string()) naab_args.push_back(NaabVal::makeString(val.get<std::string>()));
                                else if (val.is_number_integer()) naab_args.push_back(NaabVal::makeInt(val.get<int>()));
                                else if (val.is_number_float()) naab_args.push_back(NaabVal::makeDouble(val.get<double>()));
                                else if (val.is_boolean()) naab_args.push_back(NaabVal::makeBool(val.get<bool>()));
                                else if (val.is_null()) naab_args.push_back(NaabVal::makeNull());
                                else naab_args.push_back(NaabVal::makeString(val.dump()));
                            } else {
                                naab_args.push_back(NaabVal::makeNull());
                            }
                        }
                    }
                }

                {
                    // Gap A: Scoped agent context — per-agent restrictions apply during tool execution
                    ScopedToolContext tool_ctx(config);

                    // Gap O: Guard against recursive agent.send() on same handle
                    int prev_handle = t_in_tool_execution_for_handle;
                    t_in_tool_execution_for_handle = handle_id;

                    try {
                        // Call tool function via the VM callback
                        NaabVal result_val;
                        if (gov_engine && gov_engine->hasVMCallbacks()) {
                            result_val = gov_engine->callVMFunction(
                                snap_it->second.function, naab_args);
                        } else {
                            throw std::runtime_error("Agent error: no VM callback for tool execution");
                        }

                        // Step 6: Validate return type (E4: reject function/closure)
                        if (result_val.isFunction() || result_val.isVMClosure()) {
                            tool_result_str = "Error: tool returned a function (non-serializable)";
                            tool_success = false;
                        } else if (result_val.isNull()) {
                            tool_result_str = "null";
                        } else if (result_val.isString()) {
                            tool_result_str = result_val.asString();
                        } else if (result_val.isInt()) {
                            tool_result_str = std::to_string(result_val.asInt());
                        } else if (result_val.isBool()) {
                            tool_result_str = result_val.asBool() ? "true" : "false";
                        } else if (result_val.isDouble()) {
                            tool_result_str = std::to_string(result_val.asDouble());
                        } else {
                            // Dict/list — serialize to JSON
                            // Use a simple recursive serialization
                            try {
                                std::function<json(const NaabVal&)> toJson;
                                toJson = [&toJson](const NaabVal& v) -> json {
                                    if (v.isNull()) return nullptr;
                                    if (v.isInt()) return v.asInt();
                                    if (v.isDouble()) return v.asDouble();
                                    if (v.isBool()) return v.asBool();
                                    if (v.isString()) return v.asString();
                                    if (v.isList()) {
                                        json arr = json::array();
                                        for (const auto& item : v.asListConst())
                                            arr.push_back(toJson(item));
                                        return arr;
                                    }
                                    if (v.isDict()) {
                                        json obj = json::object();
                                        for (const auto& [k, val] : v.asDictConst())
                                            obj[k] = toJson(val);
                                        return obj;
                                    }
                                    return "<unsupported>";
                                };
                                tool_result_str = toJson(result_val).dump();
                            } catch (...) {
                                tool_result_str = "Error: failed to serialize tool result";
                                tool_success = false;
                            }
                        }
                    } catch (const governance::GovernanceHardError&) {
                        t_in_tool_execution_for_handle = prev_handle;
                        throw;  // uncatchable — propagate to main
                    } catch (const std::exception& e) {
                        // Step 9: Exception sanitization (Gap M)
                        std::string err_msg = e.what();
                        // Sanitize: strip file paths
                        std::regex path_re("/[^ \\n]+\\.(cpp|h|naab)");
                        err_msg = std::regex_replace(err_msg, path_re, "[internal]");
                        // Truncate
                        if (err_msg.size() > 256) err_msg = err_msg.substr(0, 256) + "...";
                        tool_result_str = "Error: " + err_msg;
                        tool_success = false;

                        if (gov_engine) {
                            gov_engine->emitEvent(governance::RuntimeEventType::TOOL_ERROR,
                                "tool_error('" + tc.name + "')", "", 0);
                        }
                    }

                    t_in_tool_execution_for_handle = prev_handle;
                }

                auto tool_end = std::chrono::steady_clock::now();
                int tool_latency_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(tool_end - tool_start).count());
                // Ensure non-zero latency when tool actually executed (sub-ms functions round to 0)
                if (tool_latency_ms == 0) tool_latency_ms = 1;

                // Step 7: Truncation (Gap L)
                bool truncated = false;
                if (static_cast<int>(tool_result_str.size()) > config->tool_result_max_chars) {
                    tool_result_str = tool_result_str.substr(0, static_cast<size_t>(config->tool_result_max_chars))
                        + "\n[truncated]";
                    truncated = true;
                }
                tool_total_result_chars += static_cast<int>(tool_result_str.size());
                if (tool_total_result_chars > config->tool_result_max_total_chars) {
                    tool_result_str = "[TRUNCATED: cumulative tool result limit reached]";
                    truncated = true;
                }

                // Step 8: Result content scanning (Gap B)
                bool redacted = false;
                if (gov_engine && gov_engine->isActive() && tool_success) {
                    std::string secret_err = gov_engine->checkSecrets(tool_result_str, 0);
                    if (!secret_err.empty()) {
                        tool_result_str = "[REDACTED: tool result contained potential secrets]";
                        redacted = true;
                        gov_engine->writeAgentTelemetry("AGENT_TOOL_SCAN_HIT", {
                            {"tool_name", tc.name},
                            {"scan_type", "secrets"},
                            {"direction", "result"},
                            {"action", "redact"}
                        });
                    }
                    std::string pii_err = gov_engine->checkPii(tool_result_str, 0);
                    if (!pii_err.empty()) {
                        tool_result_str = "[REDACTED: tool result contained potential PII]";
                        redacted = true;
                    }
                }

                // Step 10: Taint marking (Gap E)
                if (gov_engine && gov_engine->isActive() &&
                    gov_engine->getRules().taint_tracking.enabled) {
                    gov_engine->setLastReturnTainted(true, "tool_result:" + tc.name);
                }

                // Step 11: BSD event (Gap F)
                if (gov_engine) {
                    gov_engine->emitEvent(governance::RuntimeEventType::TOOL_RESULT,
                        "tool_result('" + tc.name + "')", "", 0);
                    gov_engine->writeAgentTelemetry("AGENT_TOOL_RESULT", {
                        {"tool_name", tc.name},
                        {"handle_id", std::to_string(handle_id)},
                        {"success", tool_success ? "true" : "false"},
                        {"latency_ms", std::to_string(tool_latency_ms)},
                        {"result_chars", std::to_string(tool_result_str.size())},
                        {"truncated", truncated ? "true" : "false"},
                        {"redacted", redacted ? "true" : "false"}
                    });
                }

                // Build tool_use content block for assistant message
                json tool_use_block;
                tool_use_block["type"] = "tool_use";
                tool_use_block["id"] = tc.id;
                tool_use_block["name"] = tc.name;
                tool_use_block["input"] = tool_args;
                assistant_content.push_back(tool_use_block);

                // Build tool_result block for user message
                json result_block;
                result_block["type"] = "tool_result";
                result_block["tool_use_id"] = tc.id;
                result_block["content"] = tool_result_str;
                if (!tool_success) result_block["is_error"] = true;
                tool_result_blocks.push_back(result_block);

                tool_calls_this_send++;

                // Track for response dict
                std::unordered_map<std::string, NaabVal> ts;
                ts["name"] = NaabVal::makeString(tc.name);
                ts["success"] = NaabVal::makeBool(tool_success);
                ts["latency_ms"] = NaabVal::makeInt(tool_latency_ms);
                tool_results_summary.push_back(NaabVal::makeDict(std::move(ts)));

                // D3: Hard stop check after each tool function
                if (s_dispatch.hard_stopped.load()) {
                    tool_loop_exit_reason = "hard_stop";
                    break;
                }
            } // end for each tool_call

            if (tool_loop_exit_reason != "text_response") break;

            // Append assistant (tool_use) message to history
            json asst_msg;
            asst_msg["role"] = "assistant";
            asst_msg["content"] = assistant_content;
            messages_json.push_back(asst_msg);

            // Append user (tool_result) message to history
            json user_tool_msg;
            user_tool_msg["role"] = "user";
            user_tool_msg["content"] = tool_result_blocks;
            messages_json.push_back(user_tool_msg);

            // Budget enforcement per round-trip (Gap C) + tracker updates (L5)
            {
                std::lock_guard<std::mutex> lock(s_agent_mutex);
                auto& tracker = s_trackers[handle_id];
                tracker.turns++;
                tracker.input_tokens += resp_input_tokens;
                tracker.output_tokens += resp_output_tokens;
                if (tracker.turns >= config->max_turns) {
                    tool_loop_exit_reason = "max_turns";
                    break;
                }
                int total_tokens_used = tracker.input_tokens + tracker.output_tokens;
                if (config->max_total_tokens > 0 && total_tokens_used >= config->max_total_tokens) {
                    tool_loop_exit_reason = "max_tokens";
                    break;
                }
            }

            // Check tool-specific limits
            if (tool_calls_this_send >= config->max_tool_calls_per_turn) {
                tool_loop_exit_reason = "max_tool_calls_per_turn";
                break;
            }
            if (s_dispatch.hard_stopped.load()) {
                tool_loop_exit_reason = "hard_stop";
                break;
            }

            // D8: Governance level check before next LLM call
            if (gov_engine && gov_engine->isActive()) {
                auto level = gov_engine->getGovernanceLevel();
                if (level == governance::GovernanceLevel::CRITICAL) {
                    tool_loop_exit_reason = "governance_critical";
                    break;
                }
            }

            // Reload governance config before next LLM call (S1)
            if (gov_engine) {
                gov_engine->reloadIfChanged();
                config = findAgentConfig(config_name);
                if (!config) {
                    throw std::runtime_error(fmt::format(
                        "Agent error: Agent config '{}' removed during governance reload\n\n"
                        "  Help:\n"
                        "  - The governance configuration was reloaded mid-run\n"
                        "  - The agent config is no longer available\n",
                        config_name));
                }
                // Check if tools were disabled mid-loop
                if (!config->tools_enabled) {
                    tool_loop_exit_reason = "tools_disabled";
                    gov_notices.push_back("Tools disabled during tool loop (governance reload)");
                    break;
                }
            }

            // Make next LLM API call (with tool definitions)
            messages_str = messages_json.dump();

            // Retry loop for the tool-loop's LLM call (simplified — single attempt)
            std::string key_env_loop;
            std::string api_key_loop;
            {
                bool found_key = false;
                for (size_t k = 0; k < keys.size(); k++) {
                    std::lock_guard<std::mutex> lock(s_dispatch.dead_keys_mutex);
                    if (isKeyDead(keys[k], config->retry.key_retry_after_seconds)) continue;
                    api_key_loop = runtime::resolveApiKey(keys[k]);
                    if (!api_key_loop.empty()) {
                        key_env_loop = keys[k];
                        found_key = true;
                        break;
                    }
                }
                if (!found_key) {
                    tool_loop_exit_reason = "no_api_keys";
                    break;
                }
            }

            governance::AgentConfig loop_config = *config;
            loop_config.model = models[0];  // Use primary model for tool loop

            auto loop_result = runtime::callAgentWithTools(
                loop_config, api_key_loop, messages_str, tool_defs);

            if (!loop_result.response.success) {
                tool_loop_exit_reason = "api_error";
                break;
            }

            // Update response data
            agent_resp = loop_result.response;
            content = agent_resp.content;
            stop_reason = agent_resp.stop_reason;
            resp_input_tokens = agent_resp.input_tokens;
            resp_output_tokens = agent_resp.output_tokens;

            // Check if this is still a tool response
            is_tool_response = (stop_reason == "tool_use" || stop_reason == "FUNCTION_CALL" ||
                                stop_reason == "tool_calls" || !agent_resp.tool_calls.empty());
            if (!is_tool_response) {
                tool_loop_exit_reason = "text_response";
            }
        } // end while tool loop

        // Update tracker tool counters (L5)
        {
            std::lock_guard<std::mutex> lock(s_agent_mutex);
            auto& tracker = s_trackers[handle_id];
            tracker.tool_calls_total += tool_calls_this_send;
            tracker.tool_calls_blocked += tool_blocked_this_send;
            // Sum latency from tool_results_summary
            for (const auto& ts_entry : tool_results_summary) {
                if (ts_entry.isDict()) {
                    auto lat_it = ts_entry.asDictConst().find("latency_ms");
                    if (lat_it != ts_entry.asDictConst().end() && lat_it->second.isInt())
                        tracker.tool_total_latency_ms += lat_it->second.asInt();
                }
            }
        }

        // Emit TOOL_LOOP_END telemetry
        if (gov_engine && gov_engine->isActive()) {
            gov_engine->writeAgentTelemetry("AGENT_TOOL_LOOP_END", {
                {"handle_id", std::to_string(handle_id)},
                {"tool_calls_made", std::to_string(tool_calls_this_send)},
                {"tool_loop_turns", std::to_string(tool_loop_turns)},
                {"exit_reason", tool_loop_exit_reason}
            });
        }
    } // end if tools_enabled

    // Block tool_use if still unresolved (tools not enabled or tool loop exhausted without text)
    if ((stop_reason == "tool_use" || stop_reason == "FUNCTION_CALL" ||
         stop_reason == "tool_calls") && !config->tools_enabled) {
        throw std::runtime_error(fmt::format(
            "Agent error: Agent '{}' returned a tool_use response\n\n"
            "  Help:\n"
            "  - Tool execution must be enabled in the agent configuration\n"
            "  - Register tool functions before sending messages\n",
            config_name));
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
                        gov_engine->emitEvent(governance::RuntimeEventType::CHECK_FAILED,
                            "agent_restriction:shell_blocked(" + config_name + ")", "", 0);
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
                            gov_engine->emitEvent(governance::RuntimeEventType::CHECK_FAILED,
                                "agent_restriction:blocked_path(" + config_name + ":" + blocked + ")", "", 0);
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
                        gov_engine->emitEvent(governance::RuntimeEventType::CHECK_FAILED,
                            "agent_restriction:blocked_path(" + config_name + ":" + blocked + ")", "", 0);
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
                    gov_engine->emitEvent(governance::RuntimeEventType::CHECK_FAILED,
                        "agent_restriction:path_not_allowed(" + config_name + ":" + path + ")", "", 0);
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

    // Telemetry: response scan passed — proof that post-receive governance ran this turn
    if (gov_engine && gov_engine->isActive() && !content.empty()) {
        gov_engine->writeAgentTelemetry("RESPONSE_SCAN", {
            {"handle_id",      std::to_string(handle_id)},
            {"config_name",    config_name},
            {"turn",           std::to_string(current_turn)},
            {"content_length", std::to_string(content.size())},
            {"output_tokens",  std::to_string(resp_output_tokens)},
            {"checks",         "no_secrets,no_pii,path_restrictions"},
            {"result",         "pass"}
        });
    }

    // Telemetry: response suppressed — content was empty after all retries
    if (gov_engine && gov_engine->isActive() && content.empty()) {
        gov_engine->writeAgentTelemetry("RESPONSE_SUPPRESSED", {
            {"handle_id",     std::to_string(handle_id)},
            {"config_name",   config_name},
            {"turn",          std::to_string(current_turn)},
            {"output_tokens", std::to_string(resp_output_tokens)},
            {"reason",        !agent_resp.error.empty() ? agent_resp.error : "empty response"},
            {"retries_used",  std::to_string(agent_resp.attempts - 1)}
        });
    }

    // response_format: check JSON validity BEFORE CDD so parse failures feed coherence
    bool json_valid_result = true;
    std::string json_error_signal;
    if (config && config->response_format == "json" && !content.empty()) {
        try {
            (void)nlohmann::json::parse(content);
        } catch (...) {
            json_valid_result = false;
            json_error_signal = "response_format:json parse failed";
            fmt::print(stderr,
                "[hint] agent '{}' has response_format: \"json\" but response is not valid JSON\n",
                config_name);
        }
    }

    // Output Contract checking (Phase 7) — validate response against schema
    if (config && !content.empty() && !config->output_contract.format.empty()) {
        std::string contract_error;

        if (config->output_contract.format == "json") {
            try {
                auto parsed = nlohmann::json::parse(content);

                // Check required fields
                for (const auto& req_field : config->output_contract.required_fields) {
                    if (!parsed.contains(req_field)) {
                        contract_error = fmt::format("missing required field '{}'", req_field);
                        break;
                    }
                }

                // Check field types (if provided)
                if (contract_error.empty()) {
                    for (const auto& [field, expected_type] : config->output_contract.field_types) {
                        if (!parsed.contains(field)) continue;  // type check only for present fields

                        const auto& field_val = parsed[field];
                        bool type_matches = false;

                        if (expected_type == "string") type_matches = field_val.is_string();
                        else if (expected_type == "number") type_matches = field_val.is_number();
                        else if (expected_type == "boolean") type_matches = field_val.is_boolean();
                        else if (expected_type == "array") type_matches = field_val.is_array();
                        else if (expected_type == "object") type_matches = field_val.is_object();

                        if (!type_matches) {
                            contract_error = fmt::format(
                                "field '{}' type mismatch (expected {}, got {})",
                                field, expected_type, field_val.type_name());
                            break;
                        }
                    }
                }

                // Check regex patterns (if provided)
                if (contract_error.empty()) {
                    for (const auto& [field, pattern_str] : config->output_contract.regex_checks) {
                        if (!parsed.contains(field)) continue;
                        if (!parsed[field].is_string()) continue;

                        std::string field_str = parsed[field].get<std::string>();
                        try {
                            std::regex pattern(pattern_str);
                            if (!std::regex_search(field_str, pattern)) {
                                contract_error = fmt::format(
                                    "field '{}' does not match pattern: {}", field, pattern_str);
                                break;
                            }
                        } catch (const std::regex_error& e) {
                            contract_error = fmt::format("invalid regex in contract: {}", e.what());
                            break;
                        }
                    }
                }

            } catch (const nlohmann::json::exception& e) {
                contract_error = fmt::format("JSON parse failed: {}", e.what());
            }
        }

        // Emit CONTRACT_VIOLATION telemetry if contract failed
        if (!contract_error.empty()) {
            if (gov_engine && gov_engine->isActive()) {
                gov_engine->writeAgentTelemetry("CONTRACT_VIOLATION", {
                    {"handle_id",     std::to_string(handle_id)},
                    {"config_name",   config_name},
                    {"turn",          std::to_string(current_turn)},
                    {"format",        config->output_contract.format},
                    {"violation",     contract_error},
                    {"content_length", std::to_string(content.size())}
                });
            }
            // Block the response by throwing an error
            throw std::runtime_error(
                fmt::format("Agent error: output contract violation\n\n"
                    "  Agent: {}\n"
                    "  Format: {}\n"
                    "  Violation: {}\n\n"
                    "  The LLM response does not match the expected output schema.\n",
                    config_name, config->output_contract.format, contract_error));
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
        tracker.retries += agent_resp.attempts - 1;
        if (agent_resp.fallback_used) tracker.fallbacks++;
        tracker.total_latency_ms += agent_resp.latency_ms;
        tracker.key_offset = key_offset;
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
        // Context drift check — include json parse failures as coherence signals
        std::string cdd_error = agent_resp.success ? json_error_signal : agent_resp.error;
        // Read governance level before CDD (for change detection)
        int level_before = static_cast<int>(gov_engine->getGovernanceLevel());
        std::string drift_err = gov_engine->checkContextDrift(
            handle_id, current_turn, cdd_error);
        // CDD_TURN: emit BEFORE potential throw so it's always in the audit trail
        {
            auto drift_state = gov_engine->getDriftState(handle_id);
            int level_after = static_cast<int>(gov_engine->getGovernanceLevel());
            const char* level_names[] = {"normal", "elevated", "high", "critical"};
            const char* level_str = (level_after >= 0 && level_after <= 3)
                ? level_names[level_after] : "unknown";
            if (drift_state) {
                char coh[32], vel[32], acc[32], pres[32];
                snprintf(coh,  sizeof(coh),  "%.4f", drift_state->coherence_score);
                snprintf(vel,  sizeof(vel),  "%.4f", drift_state->coherence_velocity);
                snprintf(acc,  sizeof(acc),  "%.4f", drift_state->coherence_acceleration);
                snprintf(pres, sizeof(pres), "%.4f", drift_state->last_pressure_score);
                gov_engine->writeAgentTelemetry("CDD_TURN", {
                    {"handle_id",        std::to_string(handle_id)},
                    {"turn",             std::to_string(current_turn)},
                    {"coherence",        coh},
                    {"velocity",         vel},
                    {"acceleration",     acc},
                    {"pressure",         pres},
                    {"signals_fired",    std::to_string(drift_state->signals_fired_this_turn)},
                    {"governance_level", level_str},
                    {"drift_detected",   drift_err.empty() ? "false" : "true"}
                });
            } else {
                // CDD ran but drift analyzer has no state yet (before first check_interval_turns)
                gov_engine->writeAgentTelemetry("CDD_TURN", {
                    {"handle_id",        std::to_string(handle_id)},
                    {"turn",             std::to_string(current_turn)},
                    {"governance_level", level_str},
                    {"drift_detected",   "false"},
                    {"state",            "no_data_yet"}
                });
            }
            // GOVERNANCE_LEVEL_CHANGE: only when level transitions
            if (level_after != level_before) {
                const char* from_str = (level_before >= 0 && level_before <= 3)
                    ? level_names[level_before] : "unknown";
                gov_engine->writeAgentTelemetry("GOVERNANCE_LEVEL_CHANGE", {
                    {"handle_id",  std::to_string(handle_id)},
                    {"turn",       std::to_string(current_turn)},
                    {"from_level", from_str},
                    {"to_level",   level_str}
                });
            }
        }
        if (!drift_err.empty()) {
            // Hard/soft enforcement: abort the agent call immediately
            // (advisory returns "" from enforce(), so only real blocks reach here)
            throw std::runtime_error(drift_err);
        }
    }

    // Governance health check — verify instrumentation is operational
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().governance_health.enabled) {
        std::string health_warnings = gov_engine->checkGovernanceHealth(current_turn);
        if (!health_warnings.empty()) {
            // Surface to operator via stderr (NOT to agent — verdict only)
            fprintf(stderr, "%s", health_warnings.c_str());
            // Telemetry: health warning — subsystem degradation is now auditable
            std::string w = health_warnings;
            if (w.size() > 300) w = w.substr(0, 300) + "...";
            gov_engine->writeAgentTelemetry("GOVERNANCE_HEALTH_WARNING", {
                {"handle_id", std::to_string(handle_id)},
                {"turn",      std::to_string(current_turn)},
                {"warning",   w}
            });
        }
    }

    // Temporal coupling — inter-agent timing correlation
    if (gov_engine && gov_engine->isActive()) {
        std::string coupling_warnings = gov_engine->checkTemporalCoupling();
        if (!coupling_warnings.empty()) {
            fprintf(stderr, "%s", coupling_warnings.c_str());
        }
    }

    // Build response dict
    std::unordered_map<std::string, NaabVal> result;
    result["success"] = NaabVal::makeBool(true);
    result["content"] = NaabVal::makeString(content);
    result["role"] = NaabVal::makeString("assistant");
    result["stop_reason"] = NaabVal::makeString(stop_reason);

    std::unordered_map<std::string, NaabVal> usage_val;
    usage_val["input_tokens"] = NaabVal::makeInt(resp_input_tokens);
    usage_val["output_tokens"] = NaabVal::makeInt(resp_output_tokens);
    result["usage"] = NaabVal::makeDict(std::move(usage_val));

    // Trace dict (traceability for key rotation, retry, fallback)
    std::unordered_map<std::string, NaabVal> trace;
    trace["model"] = NaabVal::makeString(agent_resp.actual_model);
    trace["provider"] = NaabVal::makeString(agent_resp.actual_provider);
    trace["api_key_env"] = NaabVal::makeString(agent_resp.actual_api_key_env);
    trace["attempts"] = NaabVal::makeInt(agent_resp.attempts);
    trace["latency_ms"] = NaabVal::makeInt(static_cast<int>(agent_resp.latency_ms));
    trace["fallback_used"] = NaabVal::makeBool(agent_resp.fallback_used);
    if (agent_resp.fallback_used) {
        trace["original_model"] = NaabVal::makeString(agent_resp.original_model);
    }
    trace["turn"] = NaabVal::makeInt(current_turn);
    trace["handle_id"] = NaabVal::makeInt(handle_id);
    result["trace"] = NaabVal::makeDict(std::move(trace));

    // Tool execution summary (L4)
    if (tool_calls_this_send > 0) {
        result["tool_calls_made"] = NaabVal::makeInt(tool_calls_this_send);
        result["tool_loop_turns"] = NaabVal::makeInt(tool_loop_turns);
        result["tool_results"] = NaabVal::makeList(std::move(tool_results_summary));
        result["tool_budget_remaining"] = NaabVal::makeInt(
            std::max(0, config->max_tool_calls_per_turn - tool_calls_this_send));
        result["tool_loop_exit_reason"] = NaabVal::makeString(tool_loop_exit_reason);
    }

    // Reality checkpoint: add pressure data if checkpoint fired at ADVISORY
    if (gov_engine && gov_engine->isActive()) {
        auto cp = gov_engine->getCheckpointData(handle_id, current_turn);
        if (cp.fired) {
            std::unordered_map<std::string, NaabVal> cp_dict;
            cp_dict["pressure"] = NaabVal::makeDouble(cp.pressure);
            cp_dict["sustained_turns"] = NaabVal::makeInt(cp.sustained_turns);
            cp_dict["recommendation"] = NaabVal::makeString(
                "Multiple governance signals trending toward thresholds. "
                "Review agent direction.");
            result["reality_checkpoint"] = NaabVal::makeDict(std::move(cp_dict));

            gov_notices.push_back(fmt::format(
                "Reality Checkpoint: pressure {:.2f} sustained {} turns",
                cp.pressure, cp.sustained_turns));
        }
    }

    // Environment self-awareness: updated state after each interaction
    result["environment"] = buildEnvironmentDict(handle_id, config_name);

    // Attach governance reload notices (Governance Under Survivability)
    if (!gov_notices.empty()) {
        std::vector<NaabVal> notice_vals;
        notice_vals.reserve(gov_notices.size());
        for (const auto& n : gov_notices) {
            notice_vals.push_back(NaabVal::makeString(n));
        }
        result["governance_notices"] = NaabVal::makeList(std::move(notice_vals));
    }

    // response_format: use pre-CDD json_valid_result (checked before CDD for coherence signal)
    if (config && config->response_format == "json" && !content.empty()) {
        result["json_valid"] = NaabVal::makeBool(json_valid_result);
    }

    // Emit signed execution attestation if provenance is enabled
    if (gov_engine && gov_engine->isActive()) {
        auto cp = gov_engine->getCheckpointData(handle_id, current_turn);
        gov_engine->emitAttestation("send", config_name, current_turn, cp.pressure);

        // Track aggregate autonomous exposure
        std::string exposure_block = gov_engine->recordAutonomousAction(config_name);
        if (!exposure_block.empty()) {
            throw std::runtime_error(exposure_block);
        }
    }

    // Mark agent response as tainted (LLM output is untrusted external data)
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().taint_tracking.enabled) {
        gov_engine->setLastReturnTainted(true, "agent.send");
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

    // Mark agent response as tainted (LLM output is untrusted external data)
    if (auto* ge = governance::GovernanceEngine::getCurrent();
        ge && ge->isActive() && ge->getRules().taint_tracking.enabled) {
        ge->setLastReturnTainted(true, "agent.run");
    }

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
    result["retries"] = NaabVal::makeInt(tracker.retries);
    result["fallbacks"] = NaabVal::makeInt(tracker.fallbacks);
    result["total_latency_ms"] = NaabVal::makeInt(static_cast<int>(tracker.total_latency_ms));
    // Tool execution counters (L5)
    if (tracker.tool_calls_total > 0) {
        result["tool_calls_total"] = NaabVal::makeInt(tracker.tool_calls_total);
        result["tool_calls_blocked"] = NaabVal::makeInt(tracker.tool_calls_blocked);
        result["tool_total_latency_ms"] = NaabVal::makeInt(static_cast<int>(tracker.tool_total_latency_ms));
    }

    return NaabVal::makeDict(std::move(result));
}

// ============================================================================
// agent.key_health(config_name) → dict {available, active, dead}
// ============================================================================

static NaabVal agentKeyHealth(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error(
            "Agent error: agent.key_health requires a config name\n\n"
            "  Expected: agent.key_health(\"agent_name\")\n\n"
            "  Help:\n"
            "  - Returns key rotation status for the named agent config\n"
            "  - Shows which keys are active vs dead (401 responses)\n");
    }

    const auto* config = findAgentConfig(args[0].asString());
    if (!config) {
        throw std::runtime_error(
            "Agent error: Unknown agent config '" + args[0].asString() + "'\n\n"
            "  Help:\n  - Check agent names in govern.json agents section\n");
    }

    std::vector<std::string> keys = config->api_key_envs;
    if (keys.empty()) keys.push_back(config->api_key_env);

    std::vector<NaabVal> active_list, dead_list;
    for (const auto& k : keys) {
        bool is_dead = false;
        {
            std::lock_guard<std::mutex> lock(s_dispatch.dead_keys_mutex);
            is_dead = isKeyDead(k, config->retry.key_retry_after_seconds);
        }
        if (is_dead) {
            dead_list.push_back(NaabVal::makeString(k));
        } else if (!runtime::resolveApiKey(k).empty()) {
            active_list.push_back(NaabVal::makeString(k));
        }
    }

    std::unordered_map<std::string, NaabVal> result;
    result["available"] = NaabVal::makeInt(static_cast<int>(active_list.size()));
    result["active"] = NaabVal::makeList(std::move(active_list));
    result["dead"] = NaabVal::makeList(std::move(dead_list));
    return NaabVal::makeDict(std::move(result));
}

// ============================================================================
// agent.dispatch_status() → dict {calls_made, tokens_used, hard_stopped, ...}
// ============================================================================

static NaabVal agentDispatchStatus(std::vector<NaabVal>& /*args*/) {
    auto* ge = governance::GovernanceEngine::getCurrent();

    int max_calls = 0, max_tokens = 0, max_time_ms = 0;
    if (ge) {
        const auto& hs = ge->getRules().agent_dispatch.hard_stop;
        max_calls = hs.max_calls_per_run;
        max_tokens = hs.max_tokens_per_run;
        max_time_ms = hs.max_agent_time_ms;
    }

    std::unordered_map<std::string, NaabVal> result;
    result["calls_made"] = NaabVal::makeInt(s_dispatch.total_calls.load());
    result["calls_remaining"] = NaabVal::makeInt(
        max_calls > 0 ? std::max(0, max_calls - s_dispatch.total_calls.load()) : -1);
    result["tokens_used"] = NaabVal::makeInt(s_dispatch.total_tokens.load());
    result["tokens_remaining"] = NaabVal::makeInt(
        max_tokens > 0 ? std::max(0, max_tokens - s_dispatch.total_tokens.load()) : -1);
    result["agent_time_ms"] = NaabVal::makeInt(
        static_cast<int>(s_dispatch.total_agent_time_ms.load()));
    result["time_remaining_ms"] = NaabVal::makeInt(
        max_time_ms > 0 ? std::max(0, static_cast<int>(max_time_ms - s_dispatch.total_agent_time_ms.load())) : -1);
    result["consecutive_failures"] = NaabVal::makeInt(s_dispatch.consecutive_failures.load());
    result["hard_stopped"] = NaabVal::makeBool(s_dispatch.hard_stopped.load());
    {
        std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
        result["stop_reason"] = NaabVal::makeString(s_dispatch.stop_reason);
    }
    return NaabVal::makeDict(std::move(result));
}

// ============================================================================
// agent.environment(handle) → environment dict
// On-demand query for current environment snapshot without making an API call.
// ============================================================================

static NaabVal agentEnvironment(std::vector<NaabVal>& args) {
    if (args.empty()) {
        throw std::runtime_error(
            "Agent error: agent.environment requires an agent handle\n\n"
            "  Expected: agent.environment(handle)\n\n"
            "  Help:\n"
            "  - Returns the current environment snapshot for the agent\n"
            "  - Includes config limits, remaining capacity, coherence state\n");
    }

    auto [config_name, handle_id] = validateHandle(args[0]);
    return buildEnvironmentDict(handle_id, config_name);
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

    int handle_id = handle.at("id").asInt();
    std::string config_name = handle.at("config_name").asString();

    // Verify HMAC nonce — prevents handle forgery and replay
    auto nonce_it = handle.find("__nonce");
    if (nonce_it == handle.end() || !nonce_it->second.isString()) {
        throw std::runtime_error(
            "Agent error: Invalid agent handle\n\n"
            "  Help:\n  - Use the handle returned by agent.create()\n"
            "  - Forged or modified handles are rejected\n");
    }
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto tracker_it = s_trackers.find(handle_id);
        if (tracker_it == s_trackers.end() ||
            !security::CryptoUtils::constantTimeCompare(
                nonce_it->second.asString(), tracker_it->second.nonce)) {
            throw std::runtime_error(
                "Agent error: Invalid or tampered agent handle\n\n"
                "  Help:\n  - Use the handle returned by agent.create()\n"
                "  - Forged or modified handles are rejected\n");
        }
    }

    return {config_name, handle_id};
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
                    } catch (const governance::GovernanceHardError&) {
                        throw;  // uncatchable — propagate even from batch workers
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

    // Mark agent batch response as tainted (LLM output is untrusted external data)
    if (auto* ge = governance::GovernanceEngine::getCurrent();
        ge && ge->isActive() && ge->getRules().taint_tracking.enabled) {
        ge->setLastReturnTainted(true, "agent.batch");
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

    // F3: Track pipeline depth with thread_local counter + RAII guard
    // Authoritative pipeline depth counter. Synced to governance engine via
    // setPipelineDepth() below. RAII DepthGuard ensures consistency.
    static thread_local int t_pipeline_depth = 0;
    t_pipeline_depth++;
    struct DepthGuard { ~DepthGuard() { t_pipeline_depth--; } } depth_guard;

    // Set pipeline_depth on governance engine (thread_local, read by checkAdmission)
    {
        auto* ge = governance::GovernanceEngine::getCurrent();
        if (ge && ge->isActive()) {
            ge->setPipelineDepth(0, t_pipeline_depth);

            // F7: Pipeline separation of duties — no adjacent stages may share config
            const auto& sep = ge->getRules().pipeline_separation;
            if (sep.enabled && handles.size() >= 2) {
                for (size_t i = 1; i < handles.size(); i++) {
                    if (handles[i-1].isDict() && handles[i].isDict()) {
                        auto& prev = handles[i-1].asDict();
                        auto& curr = handles[i].asDict();
                        auto pc = prev.find("config_name");
                        auto cc = curr.find("config_name");
                        if (pc != prev.end() && cc != curr.end() &&
                            pc->second.isString() && cc->second.isString() &&
                            pc->second.asString() == cc->second.asString()) {
                            std::string msg = fmt::format(
                                "Pipeline separation violation — adjacent stages {} and {} "
                                "share agent config '{}'\n\n"
                                "  Pipeline stages should use distinct agent configurations\n"
                                "  to ensure separation of duties.\n",
                                i-1, i, pc->second.asString());
                            if (sep.level == governance::EnforcementLevel::HARD ||
                                sep.level == governance::EnforcementLevel::SOFT) {
                                throw std::runtime_error(msg);
                            } else {
                                fprintf(stderr, "[governance] ADVISORY: %s\n", msg.c_str());
                            }
                        }
                    }
                }
            }
        }
    }

    // Chain: send to each agent sequentially, pass output as input to next
    std::string current_message = args[1].asString();
    NaabVal last_response = NaabVal::makeNull();
    std::vector<NaabVal> stage_traces;

    for (size_t i = 0; i < handles.size(); i++) {
        std::vector<NaabVal> send_args = {handles[i], NaabVal::makeString(current_message)};
        try {
            last_response = agentSend(send_args);
        } catch (...) {
            // Recover coherence for the failed stage to prevent floor-grinding
            auto* ge = governance::GovernanceEngine::getCurrent();
            if (ge && ge->isActive() && handles[i].isDict()) {
                auto& d = handles[i].asDict();
                auto hid = d.find("id");
                if (hid != d.end()) {
                    ge->recoverCoherence(hid->second.asInt());
                }
            }
            throw;
        }

        // Collect trace from this stage's response
        if (last_response.isDict()) {
            auto& rd = last_response.asDict();
            auto tr_it = rd.find("trace");
            if (tr_it != rd.end()) {
                stage_traces.push_back(tr_it->second);
            }
        }

        // Extract content for next stage, prepend governance metadata
        if (i < handles.size() - 1) {
            bool got_content = false;
            if (last_response.isDict()) {
                auto& resp_dict = last_response.asDict();
                auto it = resp_dict.find("content");
                if (it != resp_dict.end() && it->second.isString()) {
                    std::string content = it->second.asString();
                    got_content = !content.empty();

                    if (got_content) {
                        // Build metadata header from previous stage
                        std::string meta = "[Pipeline Stage " + std::to_string(i) + " Output]\n";

                        // Token usage
                        auto usage_it = resp_dict.find("usage");
                        if (usage_it != resp_dict.end() && usage_it->second.isDict()) {
                            auto& u = usage_it->second.asDict();
                            auto out_it = u.find("output_tokens");
                            if (out_it != u.end()) {
                                meta += "Tokens: " + std::to_string(out_it->second.asInt()) + "\n";
                            }
                        }

                        // Reality checkpoint pressure
                        double stage_pressure = 0.0;
                        auto cp_it = resp_dict.find("reality_checkpoint");
                        if (cp_it != resp_dict.end() && cp_it->second.isDict()) {
                            auto& cp = cp_it->second.asDict();
                            auto p_it = cp.find("pressure");
                            if (p_it != cp.end()) {
                                stage_pressure = p_it->second.asDouble();
                                meta += "Governance pressure: " +
                                    std::to_string(stage_pressure) + "\n";
                            }
                        }

                        // Inherit pressure to next pipeline stage's CDD
                        if (stage_pressure > 0.0) {
                            auto* ge = governance::GovernanceEngine::getCurrent();
                            if (ge && ge->isActive() && handles[i + 1].isDict()) {
                                auto& next_dict = handles[i + 1].asDict();
                                auto hid_it = next_dict.find("id");
                                if (hid_it != next_dict.end()) {
                                    ge->setInheritedPressure(hid_it->second.asInt(), stage_pressure);
                                }
                            }
                        }

                        // F15: Recover coherence at pipeline stage transitions
                        // New agent = fresh direction; partial recovery prevents floor-grinding
                        {
                            auto* ge = governance::GovernanceEngine::getCurrent();
                            if (ge && ge->isActive() && handles[i + 1].isDict()) {
                                auto& next_dict = handles[i + 1].asDict();
                                auto hid_it = next_dict.find("id");
                                if (hid_it != next_dict.end()) {
                                    ge->recoverCoherence(hid_it->second.asInt());
                                }
                            }
                        }

                        // Upstream provenance: trust calibration for downstream stage
                        // Captures what matters for output trustworthiness, not full env
                        {
                            std::unordered_map<std::string, NaabVal> prov;
                            prov["stage"] = NaabVal::makeInt(static_cast<int>(i));

                            // From trace: what model/infra produced this output
                            auto tr_it = resp_dict.find("trace");
                            if (tr_it != resp_dict.end() && tr_it->second.isDict()) {
                                auto& tr = tr_it->second.asDict();
                                auto m_it = tr.find("model");
                                if (m_it != tr.end()) prov["model_used"] = m_it->second;
                                auto f_it = tr.find("fallback_used");
                                if (f_it != tr.end()) prov["was_fallback"] = f_it->second;
                                auto a_it = tr.find("attempts");
                                if (a_it != tr.end())
                                    prov["retries"] = NaabVal::makeInt(
                                        std::max(0, a_it->second.asInt() - 1));
                                auto l_it = tr.find("latency_ms");
                                if (l_it != tr.end()) prov["latency_ms"] = l_it->second;
                            }

                            // From environment: reasoning quality + infra health at output time
                            auto env_it = resp_dict.find("environment");
                            if (env_it != resp_dict.end() && env_it->second.isDict()) {
                                auto& env_d = env_it->second.asDict();
                                auto st_it = env_d.find("state");
                                if (st_it != env_d.end() && st_it->second.isDict()) {
                                    auto& st = st_it->second.asDict();
                                    auto c_it = st.find("coherence");
                                    if (c_it != st.end())
                                        prov["coherence_at_output"] = c_it->second;
                                    auto kd_it = st.find("keys_dead");
                                    if (kd_it != st.end())
                                        prov["keys_dead"] = kd_it->second;
                                    auto ka_it = st.find("keys_active");
                                    if (ka_it != st.end())
                                        prov["keys_active"] = ka_it->second;
                                }
                            }

                            // Tool execution data for provenance (5D)
                            auto tc_it = resp_dict.find("tool_calls_made");
                            if (tc_it != resp_dict.end() && tc_it->second.isInt()) {
                                prov["tool_calls_made"] = tc_it->second;
                            }

                            // Pressure from this stage
                            if (stage_pressure > 0.0) {
                                prov["pressure"] = NaabVal::makeDouble(stage_pressure);
                            }

                            // Store for downstream handle
                            if (handles[i + 1].isDict()) {
                                auto& nd = handles[i + 1].asDict();
                                auto nid = nd.find("id");
                                if (nid != nd.end()) {
                                    std::lock_guard<std::mutex> lock(s_provenance_mutex);
                                    s_upstream_provenance[nid->second.asInt()] =
                                        NaabVal::makeDict(std::move(prov));
                                }
                            }
                        }

                        // Governance notices
                        auto gn_it = resp_dict.find("governance_notices");
                        if (gn_it != resp_dict.end() && gn_it->second.isList()) {
                            auto& notices = gn_it->second.asList();
                            if (!notices.empty()) {
                                meta += "Notices: " + std::to_string(notices.size()) +
                                    " governance signals\n";
                            }
                        }

                        meta += "\n[Content]\n";
                        current_message = meta + content;
                    }
                }
            }
            if (!got_content) {
                throw std::runtime_error(fmt::format(
                    "Agent error: pipeline stage {} returned empty content — "
                    "cannot chain to next stage.\n", i));
            }
        }
    }

    // Attach stage_traces to the final pipeline response
    if (!stage_traces.empty() && last_response.isDict()) {
        auto& final_dict = last_response.asDict();
        final_dict["stage_traces"] = NaabVal::makeList(std::move(stage_traces));
    }

    // Mark pipeline response as tainted (LLM output is untrusted external data)
    if (auto* ge = governance::GovernanceEngine::getCurrent();
        ge && ge->isActive() && ge->getRules().taint_tracking.enabled) {
        ge->setLastReturnTainted(true, "agent.pipeline");
    }

    return last_response;
}

// ============================================================================
// Module Interface
// ============================================================================

bool AgentModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "create", "send", "run", "messages", "usage",
        "batch", "fan_out", "pipeline", "check",
        "key_health", "dispatch_status", "environment",
        "register_tool", "extract_code"
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
    if (function_name == "key_health") return agentKeyHealth(args);
    if (function_name == "dispatch_status") return agentDispatchStatus(args);
    if (function_name == "environment") return agentEnvironment(args);
    if (function_name == "register_tool") return agentRegisterTool(args);
    if (function_name == "extract_code") return agentExtractCode(args);

    throw std::runtime_error(fmt::format(
        "Agent error: Unknown function 'agent.{}'\n\n"
        "  Available functions: create, send, run, messages, usage, batch, fan_out, pipeline, check, key_health, dispatch_status, environment, register_tool\n\n"
        "  Help:\n"
        "  - agent.create(name) — create agent handle from govern.json config\n"
        "  - agent.send(handle, msg) — send message, get response\n"
        "  - agent.run(name, prompt) — one-shot conversation\n"
        "  - agent.messages(handle) — get conversation history\n"
        "  - agent.usage(handle) — get token usage stats\n"
        "  - agent.batch(handles, msgs) — parallel: send msgs[i] to handles[i]\n"
        "  - agent.fan_out(handles, msg) — parallel: send same msg to all handles\n"
        "  - agent.pipeline(handles, msg) — sequential: chain agents, output->input\n"
        "  - agent.check(name) — pre-flight: validate config + API key, returns {valid, error}\n"
        "  - agent.key_health(name) — key rotation status: active vs dead keys\n"
        "  - agent.dispatch_status() — run-level dispatch counters and hard stop status\n"
        "  - agent.environment(handle) — current environment snapshot: limits, state, coherence\n"
        "  - agent.register_tool(name, fn, schema) — register function as LLM-callable tool\n",
        function_name));
}

AgentDispatchStats getAgentDispatchStats() {
    AgentDispatchStats stats;
    stats.total_calls = s_dispatch.total_calls.load();
    stats.total_retries = s_dispatch.total_retries.load();
    stats.total_tokens = s_dispatch.total_tokens.load();
    stats.total_agent_time_ms = s_dispatch.total_agent_time_ms.load();
    stats.consecutive_failures = s_dispatch.consecutive_failures.load();
    stats.hard_stopped = s_dispatch.hard_stopped.load();
    {
        std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
        stats.stop_reason = s_dispatch.stop_reason;
    }
    {
        std::lock_guard<std::mutex> lock(s_dispatch.dead_keys_mutex);
        for (const auto& [key, _] : s_dispatch.dead_keys) {
            stats.dead_keys.push_back(key);
        }
    }
    // Aggregate tool stats from all trackers
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        for (const auto& [_, tracker] : s_trackers) {
            stats.total_tool_calls += tracker.tool_calls_total;
            stats.total_tool_calls_blocked += tracker.tool_calls_blocked;
            stats.total_tool_latency_ms += tracker.tool_total_latency_ms;
        }
    }
    return stats;
}

} // namespace stdlib
} // namespace naab
