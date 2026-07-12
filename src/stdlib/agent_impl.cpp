// NAAb Agent Module — Governed LLM conversation management
// Provides agent.create(), agent.send(), agent.run(), agent.messages(), agent.usage(),
// agent.batch(), agent.fan_out(), agent.pipeline()
// Requires "agents" section in govern.json for configuration

#include "naab/stdlib_new_modules.h"
#include "naab/naab_val.h"
#include "naab/governance.h"
#include "naab/agent_provider.h"
#include "naab/keyword_extract.h"
#include "naab/thread_pool.h"
#include "naab/sandbox.h"
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <stdexcept>
#include <cstdlib>
#include <random>
#include <unordered_set>
#include <mutex>
#include <future>
#include <regex>
#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
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
    std::string config_name;   // bound at create time — reject if handle dict diverges
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
    size_t key_offset = 0;  // round-robin position across agent.send() calls
    int truncation_count = 0;  // times response was truncated (MAX_TOKENS)
    int last_reinforcement_turn = -100;  // mandate reinforcement cooldown
    int last_correction_turn = -100;     // coherence correction cooldown
};
static std::unordered_map<int, AgentTracker> s_trackers;
static std::mutex s_agent_mutex;

// Pending proposals from agent.propose() — server-side authority for
// agent.commit(). Keyed by handle_id; the whole set is invalidated by any
// subsequent send/propose/commit on that handle (single outstanding proposal
// set, no replay). Guarded by s_agent_mutex.
struct PendingProposal {
    std::string nonce;         // HMAC anti-forge (same secret as handles)
    std::string content_hash;  // sha256 of candidate content
    std::string content;       // authoritative content (commit uses this, not the dict)
    std::string user_message;  // prompt that produced the candidates (history pairing)
    std::string model;
    int input_tokens = 0;
    int output_tokens = 0;
    bool scan_ok = true;       // response scan + contract passed during propose
    double score = 1.0;        // read-only admissibility score at propose time
    bool admissible = true;
};
static std::unordered_map<int, std::vector<PendingProposal>> s_pending_proposals;

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

// Keyword extraction shared with CDD (include/naab/keyword_extract.h) —
// mandate/instruction/response keywords must come from the same tokenizer.
using naab::keywords::extractKeywords;

// Effective per-agent CDD signal toggle: the agent's context_drift_signals
// override wins over the global context_drift.signals config. Pre-CDD gates
// must use this so a per-agent-enabled signal still gets its inputs when the
// global toggle is off (and vice versa).
static bool effectiveSignal(const governance::AgentConfig* config,
                            bool global_enabled, const char* key) {
    if (config) {
        auto it = config->context_drift_signals.find(key);
        if (it != config->context_drift_signals.end()) return it->second;
    }
    return global_enabled;
}

// Strip a single markdown code fence wrapping the entire response.
// LLMs frequently wrap JSON/code in ```json ... ``` or ``` ... ```
// which breaks json.parse() downstream. Only strips when the fence spans
// the whole response (optional surrounding whitespace); partial fences and
// multiple fence blocks are left untouched. Shared by send and propose paths.
static std::string stripMarkdownFences(const std::string& input) {
    std::string content = input;
    if (content.empty()) return content;
    auto fence_start = content.find("```");
    if (fence_start == std::string::npos) return content;
    auto fence_end = content.rfind("```");
    if (fence_end == std::string::npos || fence_end <= fence_start + 3) return content;
    auto line_end = content.find('\n', fence_start);
    if (line_end == std::string::npos || line_end >= fence_end) return content;
    bool only_whitespace_before = true;
    for (size_t ci = 0; ci < fence_start; ++ci) {
        if (content[ci] != ' ' && content[ci] != '\t' &&
            content[ci] != '\n' && content[ci] != '\r') {
            only_whitespace_before = false;
            break;
        }
    }
    bool only_whitespace_after = true;
    for (size_t ci = fence_end + 3; ci < content.size(); ++ci) {
        if (content[ci] != ' ' && content[ci] != '\t' &&
            content[ci] != '\n' && content[ci] != '\r') {
            only_whitespace_after = false;
            break;
        }
    }
    if (only_whitespace_before && only_whitespace_after) {
        content = content.substr(line_end + 1, fence_end - line_end - 1);
        while (!content.empty() && (content.back() == '\n' ||
               content.back() == '\r' || content.back() == ' ')) {
            content.pop_back();
        }
    }
    return content;
}

// Score a step-up challenge response: word count + keyword overlap with system prompt
// and recent user prompts (context-aware). Returns overlap ratio for telemetry.
static double scoreStepUpChallengeRatio(const std::string& response,
                                         const std::string& system_prompt,
                                         int min_words) {
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
    if (words < min_words) return -1.0;  // word count failure

    // 2. Extract keywords from system prompt only.
    // Mandate challenges ask the agent to restate its objective, which is defined
    // by the system prompt. User prompt keywords dilute the denominator without
    // contributing to the numerator, creating a structural ceiling that drops
    // below any fixed threshold as conversations grow longer.
    std::unordered_set<std::string> prompt_keywords;
    extractKeywords(system_prompt, prompt_keywords);
    if (prompt_keywords.empty()) return 1.0;  // no keywords to check

    // 3. Check overlap with response
    std::string response_lower;
    response_lower.reserve(response.size());
    for (char c : response)
        response_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Strip common English suffixes for fuzzy matching.
    // "building"→"build", "coding"→"cod", "helpful"→"help", "assistant"→"assist"
    // Stems are matched as substrings too, so "cod" finds "code" in response.
    // Min word size = suffix_len + 3 (keep 3-char stems for substring matching).
    auto stem = [](const std::string& w) -> std::string {
        if (w.size() >= 6 && w.compare(w.size()-3, 3, "ing") == 0) return w.substr(0, w.size()-3);
        if (w.size() >= 6 && w.compare(w.size()-3, 3, "ful") == 0) return w.substr(0, w.size()-3);
        if (w.size() >= 6 && w.compare(w.size()-3, 3, "ant") == 0) return w.substr(0, w.size()-3);
        if (w.size() >= 5 && w.compare(w.size()-2, 2, "ed") == 0) return w.substr(0, w.size()-2);
        if (w.size() >= 5 && w.compare(w.size()-2, 2, "er") == 0) return w.substr(0, w.size()-2);
        if (w.size() >= 5 && w.compare(w.size()-2, 2, "ly") == 0) return w.substr(0, w.size()-2);
        if (w.size() >= 5 && w.back() == 's' && (w.size() < 2 || w[w.size()-2] != 's'))
            return w.substr(0, w.size()-1);
        return w;
    };

    int found = 0;
    for (const auto& kw : prompt_keywords) {
        if (response_lower.find(kw) != std::string::npos) {
            found++;
        } else {
            std::string s = stem(kw);
            if (s != kw && response_lower.find(s) != std::string::npos) found++;
        }
    }
    return static_cast<double>(found) / static_cast<double>(prompt_keywords.size());
}

// Score a contextual challenge response: same approach as scoreStepUpChallengeRatio
// but accepts pre-computed expected keywords directly rather than extracting from
// system_prompt + prompts.
static double scoreContextualChallengeRatio(const std::string& response,
                                             const std::unordered_set<std::string>& expected_keywords,
                                             int min_words) {
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
    if (words < min_words) return -1.0;  // word count failure
    if (expected_keywords.empty()) return 1.0;  // no keywords to check

    // 2. Check overlap with response
    std::string response_lower;
    response_lower.reserve(response.size());
    for (char c : response)
        response_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    int found = 0;
    for (const auto& kw : expected_keywords) {
        if (response_lower.find(kw) != std::string::npos) found++;
    }
    return static_cast<double>(found) / static_cast<double>(expected_keywords.size());
}

// Union of an entity's recent per-sighting context sets (DriftState.entity_context
// stores a bounded deque of sightings; consumers that need "all recent context
// keywords" flatten it through here).
static std::unordered_set<std::string> entityContextUnion(
    const std::deque<std::unordered_set<std::string>>& sightings) {
    std::unordered_set<std::string> u;
    for (const auto& s : sightings) u.insert(s.begin(), s.end());
    return u;
}

// Build a mechanical summary preamble from DriftState metadata for challenge context.
// Returns empty string if no metadata is available.
static std::string buildChallengeSummary(const governance::DriftState& ds, int current_turn) {
    // If no metadata, return empty (fall through to recent-only behavior)
    if (ds.instruction_keywords.empty() && ds.plan_step_keywords.empty() &&
        ds.entity_context.empty() && ds.tool_result_keywords.empty() &&
        ds.mandate_keywords.empty()) {
        return "";
    }

    std::string summary = "[Context: Turn " + std::to_string(current_turn) + " of ongoing session.";

    // Mandate (system prompt keywords) — reinforces objective for challenge
    if (!ds.mandate_keywords.empty()) {
        summary += " Your role:";
        int count = 0;
        for (const auto& kw : ds.mandate_keywords) {
            if (count >= 20) { summary += " ..."; break; }
            summary += (count == 0 ? " " : ", ") + kw;
            count++;
        }
        summary += ".";
    }

    // Topics from accumulated instruction keywords (cap at 30 terms)
    if (!ds.instruction_keywords.empty()) {
        summary += " Topics discussed:";
        int count = 0;
        for (const auto& kw : ds.instruction_keywords) {
            if (count >= 30) { summary += " ..."; break; }
            summary += (count == 0 ? " " : ", ") + kw;
            count++;
        }
        summary += ".";
    }

    // Plan steps
    if (!ds.plan_step_keywords.empty()) {
        summary += " Plan: " + std::to_string(ds.plan_step_keywords.size()) + " steps";
        for (size_t i = 0; i < ds.plan_step_keywords.size() && i < 8; i++) {
            summary += " (" + std::to_string(i + 1) + ")";
            int kcount = 0;
            for (const auto& kw : ds.plan_step_keywords[i]) {
                if (kcount >= 5) break;
                summary += (kcount == 0 ? " " : ", ") + kw;
                kcount++;
            }
        }
        summary += ".";
    }

    // Key entities (top 5 by windowed context keyword count)
    if (!ds.entity_context.empty()) {
        std::vector<std::pair<std::string, std::unordered_set<std::string>>> entities;
        for (const auto& [ent, sightings] : ds.entity_context) {
            entities.push_back({ent, entityContextUnion(sightings)});
        }
        std::sort(entities.begin(), entities.end(),
                  [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
        summary += " Key entities:";
        for (size_t i = 0; i < entities.size() && i < 5; i++) {
            summary += (i == 0 ? " " : ", ") + entities[i].first;
            if (!entities[i].second.empty()) {
                summary += " (";
                int kcount = 0;
                for (const auto& kw : entities[i].second) {
                    if (kcount >= 4) break;
                    summary += (kcount == 0 ? "" : ", ") + kw;
                    kcount++;
                }
                summary += ")";
            }
        }
        summary += ".";
    }

    // Tools used (names only)
    if (!ds.tool_result_keywords.empty()) {
        summary += " Tools used:";
        int count = 0;
        for (const auto& [name, _] : ds.tool_result_keywords) {
            summary += (count == 0 ? " " : ", ") + name;
            count++;
        }
        summary += ".";
    }

    summary += "]\n\n";
    return summary;
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
        limits["min_tokens"] = NaabVal::makeInt(config->min_tokens);
        limits["max_total_tokens"] = NaabVal::makeInt(config->max_total_tokens);
        limits["timeout_seconds"] = NaabVal::makeInt(config->timeout_seconds);
        limits["risk_budget"] = NaabVal::makeInt(config->risk_budget);
        limits["thinking_budget"] = NaabVal::makeInt(config->thinking_budget);
        limits["context_window"] = NaabVal::makeInt(config->context_window);
        env["limits"] = NaabVal::makeDict(std::move(limits));

        env["context_strategy"] = NaabVal::makeString(config->context_strategy);

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
            state["truncation_count"] = NaabVal::makeInt(t.truncation_count);
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
            state["min_coherence"] = NaabVal::makeDouble(drift_opt->min_coherence_lifetime);
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

            // Semantic governance fields
            state["semantic_stability_count"] = NaabVal::makeInt(drift_opt->semantic_stability_count);
            state["mandate_drift_count"] = NaabVal::makeInt(drift_opt->mandate_drift_count);
            state["context_growth_count"] = NaabVal::makeInt(drift_opt->context_growth_count);
            state["instruction_recall_count"] = NaabVal::makeInt(drift_opt->instruction_recall_count);
            state["plan_drift_count"] = NaabVal::makeInt(drift_opt->plan_drift_count);
            state["entity_consistency_count"] = NaabVal::makeInt(drift_opt->entity_consistency_count);
            state["instruction_conflict_count"] = NaabVal::makeInt(drift_opt->instruction_conflict_count);
            state["persona_drift_count"] = NaabVal::makeInt(drift_opt->persona_drift_count);
            state["tool_integrity_count"] = NaabVal::makeInt(drift_opt->tool_integrity_count);
            state["claim_mismatch_count"] = NaabVal::makeInt(drift_opt->claim_result_mismatch_count);
            state["prompt_compliance_count"] = NaabVal::makeInt(drift_opt->prompt_compliance_count);
            state["response_repetition_count"] = NaabVal::makeInt(drift_opt->response_repetition_count);
            state["consecutive_quarantines"] = NaabVal::makeInt(drift_opt->consecutive_quarantines);
            // Per-agent CDD signal overrides (context_drift_signals), when any
            if (drift_opt->signal_override_mask != 0) {
                std::unordered_map<std::string, NaabVal> sig_ov;
                for (int si = 0; si < governance::NUM_CDD_SIGNALS; si++) {
                    if (drift_opt->signal_override_mask & (1u << si)) {
                        sig_ov[governance::kCddSignalKeys[si]] = NaabVal::makeBool(
                            ((drift_opt->signal_override_values >> si) & 1u) != 0);
                    }
                }
                state["cdd_signal_overrides"] = NaabVal::makeDict(std::move(sig_ov));
            }
            if (!drift_opt->claim_accuracy_history.empty()) {
                double ca_sum = 0;
                for (double v : drift_opt->claim_accuracy_history) ca_sum += v;
                state["claim_accuracy"] = NaabVal::makeDouble(
                    ca_sum / static_cast<double>(drift_opt->claim_accuracy_history.size()));
            } else {
                state["claim_accuracy"] = NaabVal::makeDouble(1.0);
            }
            if (!drift_opt->mandate_alignment_history.empty()) {
                double ma_sum = 0;
                for (double v : drift_opt->mandate_alignment_history) ma_sum += v;
                double ma_mean = ma_sum / static_cast<double>(drift_opt->mandate_alignment_history.size());
                state["mandate_alignment"] = NaabVal::makeDouble(ma_mean);
            } else {
                state["mandate_alignment"] = NaabVal::makeDouble(0.0);
            }
            // Escalation effectiveness tracking
            state["escalation_turn"] = NaabVal::makeInt(drift_opt->escalation_turn);
            state["escalation_from_level"] = NaabVal::makeInt(drift_opt->escalation_from_level);
            state["escalation_to_level"] = NaabVal::makeInt(drift_opt->escalation_to_level);
            if (drift_opt->escalation_turn >= 0) {
                int eff_window = ge->getRules().context_drift.escalation_effectiveness_window;
                if (eff_window > 0 && drift_opt->post_escalation_turns_counted >= eff_window) {
                    double mean = drift_opt->post_escalation_coherence_sum /
                                  drift_opt->post_escalation_turns_counted;
                    state["escalation_effectiveness"] = NaabVal::makeDouble(
                        mean - drift_opt->escalation_coherence_at);
                }
            }
        } else {
            state["coherence"] = NaabVal::makeDouble(1.0);  // fresh agent
            state["pipeline_depth"] = NaabVal::makeInt(0);
            state["semantic_stability_count"] = NaabVal::makeInt(0);
            state["mandate_drift_count"] = NaabVal::makeInt(0);
            state["context_growth_count"] = NaabVal::makeInt(0);
            state["instruction_recall_count"] = NaabVal::makeInt(0);
            state["plan_drift_count"] = NaabVal::makeInt(0);
            state["entity_consistency_count"] = NaabVal::makeInt(0);
            state["instruction_conflict_count"] = NaabVal::makeInt(0);
            state["persona_drift_count"] = NaabVal::makeInt(0);
            state["tool_integrity_count"] = NaabVal::makeInt(0);
            state["claim_mismatch_count"] = NaabVal::makeInt(0);
            state["prompt_compliance_count"] = NaabVal::makeInt(0);
            state["response_repetition_count"] = NaabVal::makeInt(0);
            state["consecutive_quarantines"] = NaabVal::makeInt(0);
            state["claim_accuracy"] = NaabVal::makeDouble(1.0);
            state["mandate_alignment"] = NaabVal::makeDouble(0.0);
            state["escalation_turn"] = NaabVal::makeInt(-1);
            state["escalation_from_level"] = NaabVal::makeInt(0);
            state["escalation_to_level"] = NaabVal::makeInt(0);
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

    // Fix 3B: mark agent phase active so Pulse consecutive_passes counter
    // only tracks agent-phase checks, not static source scanning.
    engine->setAgentGovernanceActive(true);

    // Tool execution context — agent creation from tool callbacks
    // requires AGENT_SEND in the parent agent's allowed_actions
    if (t_in_tool_execution_for_handle >= 0 && t_tool_agent_context) {
        if (!t_tool_agent_context->allowed_actions.empty()) {
            bool can_delegate = false;
            for (const auto& a : t_tool_agent_context->allowed_actions) {
                if (a == "AGENT_SEND") { can_delegate = true; break; }
            }
            if (!can_delegate) {
                throw std::runtime_error(
                    "Agent error: agent.create denied — parent agent lacks AGENT_SEND permission\n\n"
                    "  Help:\n"
                    "  - Tool functions cannot create agents unless the parent agent's\n"
                    "    allowed_actions includes AGENT_SEND\n");
            }
        }
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

        // Global agent count limit (existing config field, now enforced)
        int max_agents = engine->getRules().exposure_tracking.max_unique_agents;
        if (max_agents > 0 && static_cast<int>(s_trackers.size()) >= max_agents) {
            throw std::runtime_error(
                "Agent error: agent.create denied — max agent count reached\n\n"
                "  Active agents: " + std::to_string(s_trackers.size()) + "\n"
                "  Limit: " + std::to_string(max_agents) + "\n\n"
                "  Help:\n"
                "  - Reduce the number of concurrent agents\n");
        }

        handle_id = ++s_handle_counter;
        std::string nonce_input = std::to_string(handle_id) + ":" + config_name + ":" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        nonce = security::CryptoUtils::hmacSha256(nonce_input, s_process_secret);
        auto& tracker = s_trackers[handle_id];
        tracker.nonce = nonce;
        tracker.config_name = config_name;
        // Standing Lease: grant initial lease if configured
        if (config->standing_lease_turns > 0) {
            tracker.lease_granted_turn = 0;
            tracker.lease_expires_turn = config->standing_lease_turns;
        }
        if (config->standing_lease_seconds > 0) {
            tracker.lease_granted_time = std::chrono::steady_clock::now();
        }
    }

    // Bind per-agent CDD signal overrides at creation so the propose path
    // (which never calls setAgentContext-driven init) is covered too
    if (engine && engine->isActive() && !config->context_drift_signals.empty()) {
        engine->setSignalOverrides(handle_id, config->context_drift_signals);
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

    // Agent interaction transcript: creation entry with config snapshot
    if (engine && engine->isActive() && engine->isTranscriptAgent(config_name)) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char ts_buf[32];
        std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

        nlohmann::json te;
        te["type"] = "agent_create";
        te["run_id"] = engine->getRunId();
        te["timestamp"] = std::string(ts_buf);
        te["handle_id"] = handle_id;
        te["agent"] = config_name;

        nlohmann::json cfg;
        cfg["provider"] = config->provider;
        cfg["model"] = config->model;
        if (!config->model_chain.empty()) cfg["model_chain"] = config->model_chain;
        cfg["max_tokens"] = config->max_tokens;
        if (config->min_tokens > 0) cfg["min_tokens"] = config->min_tokens;
        cfg["thinking_budget"] = config->thinking_budget;
        cfg["temperature"] = config->temperature;
        cfg["max_turns"] = config->max_turns;
        cfg["max_total_tokens"] = config->max_total_tokens;
        cfg["system_prompt"] = config->system_prompt;
        cfg["tools_enabled"] = config->tools_enabled;
        if (!config->tools.empty()) cfg["tools"] = config->tools;
        if (!config->response_format.empty()) cfg["response_format"] = config->response_format;
        cfg["risk_budget"] = config->risk_budget;
        cfg["standing_lease_turns"] = config->standing_lease_turns;
        cfg["standing_lease_seconds"] = config->standing_lease_seconds;
        cfg["retry_max_attempts"] = config->retry.max_attempts;
        if (!config->allowed_actions.empty()) cfg["allowed_actions"] = config->allowed_actions;
        if (!config->output_contract.format.empty()) cfg["output_contract_format"] = config->output_contract.format;
        if (!config->context_drift_signals.empty()) cfg["context_drift_signals"] = config->context_drift_signals;
        te["config"] = cfg;

        engine->writeAgentTranscript(te.dump());
    }

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
        throw std::runtime_error(
            "Agent error: Agent config no longer available\n\n"
            "  Help:\n"
            "  - The governance configuration may have changed\n");
    }

    // Gap O: block recursive self-send from tool execution
    // t_in_tool_execution_for_handle is SET when a tool callback is running
    // but was NEVER CHECKED — a tool could agent.send() its own invoking handle
    if (t_in_tool_execution_for_handle == validated_id) {
        throw std::runtime_error(
            "Agent error: agent.send denied — recursive self-send from tool execution\n\n"
            "  Help:\n"
            "  - A tool function cannot call agent.send() on the agent that invoked it\n"
            "  - Use a different agent handle for delegation\n");
    }

    // Server-side governance enforcement (immune to handle mutation)
    // Lock to validate tracker state; released before slow API call
    int handle_id = validated_id;
    int current_turn = 0;

    // Transcript: accumulate per-turn data, write once at end
    auto* gov_for_transcript = governance::GovernanceEngine::getCurrent();
    bool transcript_active = gov_for_transcript && gov_for_transcript->isActive()
        && gov_for_transcript->isTranscriptAgent(config_name);
    nlohmann::json transcript_entry;
    auto transcript_start = std::chrono::steady_clock::now();
    if (transcript_active) {
        transcript_entry["type"] = "agent_send";
        transcript_entry["handle_id"] = handle_id;
        transcript_entry["agent"] = config_name;
        transcript_entry["prompt"] = message;
    }

    // Error-path transcript helper: writes partial entry with error details
    auto write_error_transcript = [&](const std::string& error_msg) {
        if (!transcript_active || !gov_for_transcript) return;
        try {
            transcript_entry["error"] = true;
            transcript_entry["error_message"] = error_msg;
            auto tend = std::chrono::steady_clock::now();
            transcript_entry["wall_time_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                tend - transcript_start).count();
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf;
#ifdef _WIN32
            localtime_s(&tm_buf, &t);
#else
            localtime_r(&t, &tm_buf);
#endif
            char ts_buf[32];
            std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
            transcript_entry["timestamp"] = std::string(ts_buf);
            transcript_entry["run_id"] = gov_for_transcript->getRunId();
            gov_for_transcript->writeAgentTranscript(transcript_entry.dump());
        } catch (...) {} // never mask the real error
    };

    try { // transcript error-path wrapper

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

        // Stale-proposal invalidation: a new send supersedes any outstanding
        // agent.propose() candidates for this handle (no replay after state moves)
        s_pending_proposals.erase(handle_id);

        // Enforce turn limit from tracker (not handle dict)
        if (tracker.turns >= config->max_turns) {
            throw std::runtime_error(fmt::format(
                "Agent error: Conversation exceeded max_turns limit ({})\n\n"
                "  Got: {} turns used\n"
                "  Expected: max {} turns\n\n"
                "  Help:\n"
                "  - Create a new agent handle for a fresh conversation\n"
                "  - Or create a new agent handle for a longer conversation\n",
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
                "  - Or create a new agent handle with a larger token budget\n",
                total_used, config->max_total_tokens));
        }
        current_turn = tracker.turns;
        if (transcript_active) {
            transcript_entry["turn"] = current_turn;
            transcript_entry["turns_remaining"] = config->max_turns - current_turn;
        }
    } // unlock before API call

    // Build messages array from handle history + new message
    // V-AG-003: Guard messages key access
    json messages_json = json::array();
    auto msgs_it = handle.find("messages");
    if (msgs_it == handle.end() || !msgs_it->second.isList()) {
        throw std::runtime_error(
            "Agent error: handle missing 'messages' list\n\n"
            "  Help:\n  - Use a handle returned by agent.create()\n");
    }
    auto& msg_list = msgs_it->second.asList();

    // Context windowing: limit history sent per API call
    size_t start_idx = 0;
    bool windowing_active = config && config->context_strategy != "full" &&
                             config->context_window > 0 &&
                             msg_list.size() > static_cast<size_t>(config->context_window);

    if (windowing_active) {
        start_idx = msg_list.size() - static_cast<size_t>(config->context_window);
        // Even alignment: msg_list is always even (paired user/assistant appends).
        // Positions 0,2,4... are "user", 1,3,5... are "assistant".
        // Starting on odd index would make first message "assistant" → Gemini HTTP 400.
        if (start_idx % 2 != 0) start_idx++;
    }

    for (size_t i = start_idx; i < msg_list.size(); i++) {
        auto& msg = msg_list[i];
        auto& msg_dict = msg.asDict();
        // V-AG-002: Guard message dict key access
        auto role_it = msg_dict.find("role");
        auto content_it = msg_dict.find("content");
        if (role_it == msg_dict.end() || !role_it->second.isString() ||
            content_it == msg_dict.end() || !content_it->second.isString()) {
            throw std::runtime_error(
                "Agent error: each message must be a dict with 'role' and 'content' string keys\n\n"
                "  Help:\n  - Use {role: \"user\", content: \"message\"}\n");
        }
        json msg_obj;
        msg_obj["role"] = role_it->second.asString();
        msg_obj["content"] = content_it->second.asString();
        messages_json.push_back(msg_obj);
    }
    // Append new user message
    json user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = message;
    messages_json.push_back(user_msg);

    if (transcript_active) {
        // Prepend system prompt to messages for complete transcript
        nlohmann::json full_messages = nlohmann::json::array();
        if (config && !config->system_prompt.empty()) {
            full_messages.push_back({{"role", "system"}, {"content", config->system_prompt}});
        }
        for (const auto& m : messages_json) full_messages.push_back(m);
        transcript_entry["messages"] = full_messages;
        if (windowing_active) {
            transcript_entry["context_windowing"] = {
                {"strategy", config->context_strategy},
                {"window_size", config->context_window},
                {"total_messages", static_cast<int>(msg_list.size())},
                {"messages_sent", static_cast<int>(messages_json.size())},
                {"messages_dropped", static_cast<int>(start_idx)},
                {"summary_prepended", config->context_strategy == "summary"}
            };
        }
    }

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

    if (transcript_active) {
        nlohmann::json api_params;
        api_params["model"] = models[0];
        if (models.size() > 1) api_params["model_chain"] = models;
        api_params["max_tokens"] = config->max_tokens;
        if (config->min_tokens > 0) api_params["min_tokens"] = config->min_tokens;
        api_params["thinking_budget"] = config->thinking_budget;
        api_params["temperature"] = config->temperature;
        api_params["keys_available"] = static_cast<int>(keys.size());
        transcript_entry["api_params"] = api_params;
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
            throw std::runtime_error(
                "Agent error: Agent config removed during governance reload\n\n"
                "  Help:\n"
                "  - The governance configuration was reloaded mid-run\n"
                "  - The agent config is no longer available\n");
        }
    }

    // Context windowing summary mode: prepend DriftState preamble to first windowed message.
    // Deferred to here because gov_engine is declared above (after messages are built).
    if (windowing_active && config && config->context_strategy == "summary" &&
        gov_engine && gov_engine->isActive() && !messages_json.empty()) {
        auto ds = gov_engine->getDriftState(handle_id);
        if (ds) {
            std::string window_summary = buildChallengeSummary(*ds, current_turn);
            if (!window_summary.empty()) {
                // First message in messages_json (always "user" due to even alignment)
                std::string existing = messages_json[0]["content"].get<std::string>();
                messages_json[0]["content"] = window_summary + existing;
            }
        }
    }

    // Initialize mandate keywords on first send (for mandate alignment + prompt compliance signals)
    if (gov_engine && gov_engine->isActive() && current_turn == 0 &&
        !config->system_prompt.empty() &&
        (effectiveSignal(config, gov_engine->getRules().context_drift.signals.mandate_alignment, "mandate_alignment") ||
         effectiveSignal(config, gov_engine->getRules().context_drift.signals.prompt_compliance, "prompt_compliance"))) {
        std::unordered_set<std::string> mandate_kw;
        extractKeywords(config->system_prompt, mandate_kw);
        if (!mandate_kw.empty()) {
            gov_engine->initializeMandateKeywords(handle_id, mandate_kw);
            if (transcript_active) {
                transcript_entry["mandate_keywords_count"] = static_cast<int>(mandate_kw.size());
            }
        }
    }

    // NOTE: instruction/prompt keyword bookkeeping happens AFTER the admission
    // and step-up gates (below) — recording the pending prompt here would let
    // contextual challenges quiz the agent on a message it never received, and
    // would leak the undelivered prompt into summary-mode preambles.

    // Transcript: first-turn config snapshot
    if (transcript_active && current_turn == 0 && config) {
        nlohmann::json cfg;
        cfg["provider"] = config->provider;
        cfg["model"] = config->model;
        if (!config->model_chain.empty()) cfg["model_chain"] = config->model_chain;
        cfg["max_tokens"] = config->max_tokens;
        if (config->min_tokens > 0) cfg["min_tokens"] = config->min_tokens;
        cfg["thinking_budget"] = config->thinking_budget;
        cfg["temperature"] = config->temperature;
        cfg["max_turns"] = config->max_turns;
        cfg["max_total_tokens"] = config->max_total_tokens;
        if (!config->system_prompt.empty())
            cfg["system_prompt"] = config->system_prompt.substr(0, 500);
        cfg["tools_enabled"] = config->tools_enabled;
        if (!config->tools.empty()) cfg["tools"] = config->tools;
        if (!config->response_format.empty()) cfg["response_format"] = config->response_format;
        cfg["risk_budget"] = config->risk_budget;
        cfg["standing_lease_turns"] = config->standing_lease_turns;
        cfg["standing_lease_seconds"] = config->standing_lease_seconds;
        if (!config->allowed_actions.empty()) cfg["allowed_actions"] = config->allowed_actions;
        cfg["retry_max_attempts"] = config->retry.max_attempts;
        if (!config->output_contract.format.empty()) {
            cfg["output_contract"] = {{"format", config->output_contract.format}};
        }
        transcript_entry["config"] = cfg;
    }

    // Behavioral sequence: emit agent.send event (once, before retry loop)
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().behavioral_sequences.enabled) {
        gov_engine->setAgentContext(handle_id, current_turn, config_name);
        std::string bsd_block = gov_engine->emitEvent(governance::RuntimeEventType::AGENT_SEND,
            "agent.send('" + config_name + "')", "", 0);
        if (transcript_active) {
            transcript_entry["bsd_pre_send"] = bsd_block.empty() ? "pass" : "block";
        }
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

    if (transcript_active) {
        transcript_entry["prompt_scan"] = {
            {"result", "pass"}, {"message_length", message.size()}
        };
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
            if (transcript_active) {
                transcript_entry["admission"] = {
                    {"result", "pass"},
                    {"governance_level", adm_level_str},
                    {"risk_budget_remaining", gov_engine->getRemainingBudget(config_name)},
                    {"autonomous_actions", gov_engine->getAutonomousActionCount()}
                };
            }
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
                        should_challenge = lease_expired ||
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
                        // Select challenge type based on DriftState data
                        std::string challenge_type = "mandate";  // fallback
                        std::string challenge_text = cb.step_up_challenge;
                        std::unordered_set<std::string> contextual_expected_keywords;

                        // Get DriftState for contextual selection and summary history
                        std::optional<governance::DriftState> challenge_ds;
                        if (gov_engine) {
                            challenge_ds = gov_engine->getDriftState(handle_id);
                        }

                        if (cb.step_up_contextual && challenge_ds) {
                                // Priority 1: tool_result — test recall of actual tool output
                                if (challenge_type == "mandate" && !challenge_ds->tool_result_keywords.empty()) {
                                    // Pick most recently recorded tool (last in iteration order)
                                    std::string tool_name;
                                    for (const auto& [name, kw] : challenge_ds->tool_result_keywords) {
                                        if (!kw.empty()) tool_name = name;
                                    }
                                    if (!tool_name.empty()) {
                                        challenge_type = "tool_result";
                                        challenge_text = "Your last call to " + tool_name +
                                            " returned a result. In one sentence, describe what "
                                            "that result contained, then proceed.";
                                        contextual_expected_keywords = challenge_ds->tool_result_keywords.at(tool_name);
                                    }
                                }
                                // Priority 2: plan_step — test plan awareness
                                if (challenge_type == "mandate" && !challenge_ds->plan_step_keywords.empty() &&
                                    challenge_ds->plan_last_step_matched >= 0) {
                                    int next = challenge_ds->plan_last_step_matched;
                                    if (next < static_cast<int>(challenge_ds->plan_step_keywords.size())) {
                                        challenge_type = "plan_step";
                                        challenge_text = "What is step " + std::to_string(next + 1) +
                                            " of your current plan? State it in one sentence, then proceed.";
                                        contextual_expected_keywords = challenge_ds->plan_step_keywords[static_cast<size_t>(next)];
                                    }
                                }
                                // Priority 3: instruction — test instruction memory
                                if (challenge_type == "mandate" && !challenge_ds->instruction_history.empty()) {
                                    challenge_type = "instruction";
                                    challenge_text = "In one sentence, summarize the most recent "
                                        "user instruction you received, then proceed.";
                                    contextual_expected_keywords = challenge_ds->instruction_history.back();
                                }
                                // Priority 4: entity — test entity awareness
                                if (challenge_type == "mandate" && !challenge_ds->entity_context.empty()) {
                                    // Pick entity with most windowed context keywords
                                    std::string best_entity;
                                    std::unordered_set<std::string> best_ctx;
                                    for (const auto& [ent, sightings] : challenge_ds->entity_context) {
                                        auto ctx = entityContextUnion(sightings);
                                        if (ctx.size() > best_ctx.size()) {
                                            best_entity = ent;
                                            best_ctx = std::move(ctx);
                                        }
                                    }
                                    if (!best_entity.empty()) {
                                        challenge_type = "entity";
                                        challenge_text = "What is " + best_entity +
                                            " and how does it relate to your current task? "
                                            "Answer in one sentence, then proceed.";
                                        contextual_expected_keywords = std::move(best_ctx);
                                    }
                                }
                        }

                        // Build challenge message with conversation history.
                        // History mode controls context sent to the model:
                        //   "full"    — all messages (no cap)
                        //   "recent"  — last N messages (default, backward compatible)
                        //   "summary" — DriftState summary preamble + last N messages
                        // System prompt is NOT included here — callAgentWithStatus
                        // sends it via config (Gemini: systemInstruction, Anthropic:
                        // "system" field). Including it would double-send.
                        json challenge_msgs = json::array();
                        size_t history_message_count = 0;
                        {
                            auto hist_it = handle.find("messages");
                            if (hist_it != handle.end() && hist_it->second.isList()) {
                                auto& hlist = hist_it->second.asList();
                                std::vector<json> all_msgs;
                                for (auto& hmsg : hlist) {
                                    if (hmsg.isDict()) {
                                        auto& hd = hmsg.asDict();
                                        auto hr_it = hd.find("role");
                                        auto hc_it = hd.find("content");
                                        if (hr_it != hd.end() && hr_it->second.isString() &&
                                            hc_it != hd.end() && hc_it->second.isString()) {
                                            json hm;
                                            hm["role"] = hr_it->second.asString();
                                            hm["content"] = hc_it->second.asString();
                                            all_msgs.push_back(std::move(hm));
                                        }
                                    }
                                }
                                if (cb.step_up_challenge_history == "full") {
                                    // All messages, no cap
                                    for (auto& m : all_msgs) {
                                        challenge_msgs.push_back(std::move(m));
                                    }
                                } else {
                                    // "recent" and "summary" both use configurable recent count
                                    size_t recent = static_cast<size_t>(cb.step_up_history_recent_count);
                                    size_t start = all_msgs.size() > recent
                                        ? all_msgs.size() - recent : 0;
                                    for (size_t i = start; i < all_msgs.size(); i++) {
                                        challenge_msgs.push_back(std::move(all_msgs[i]));
                                    }
                                }
                                history_message_count = challenge_msgs.size();
                            }
                        }

                        // Summary mode: prepend DriftState summary to challenge text
                        std::string effective_challenge = challenge_text;
                        if (cb.step_up_challenge_history == "summary" && challenge_ds) {
                            std::string summary = buildChallengeSummary(*challenge_ds, current_turn);
                            if (!summary.empty()) {
                                effective_challenge = summary + challenge_text;
                            }
                        }

                        json challenge_user;
                        challenge_user["role"] = "user";
                        challenge_user["content"] = effective_challenge;
                        challenge_msgs.push_back(challenge_user);

                        governance::AgentConfig challenge_config = *config;
                        challenge_config.model = models[0];

                        s_dispatch.total_calls++;
                        auto challenge_result = runtime::callAgentWithStatus(
                            challenge_config, challenge_key, challenge_msgs.dump());

                        // Extract recent user messages for context-aware scoring
                        std::vector<std::string> recent_prompts;
                        {
                            auto msgs_it2 = handle.find("messages");
                            if (msgs_it2 != handle.end() && msgs_it2->second.isList()) {
                                auto& mlist = msgs_it2->second.asList();
                                int count = 0;
                                for (auto rit = mlist.rbegin(); rit != mlist.rend() && count < 5; ++rit) {
                                    if (rit->isDict()) {
                                        auto& d = rit->asDict();
                                        auto r_it = d.find("role");
                                        auto c_it = d.find("content");
                                        if (r_it != d.end() && r_it->second.isString() &&
                                            r_it->second.asString() == "user" &&
                                            c_it != d.end() && c_it->second.isString()) {
                                            recent_prompts.push_back(c_it->second.asString());
                                            count++;
                                        }
                                    }
                                }
                            }
                        }

                        bool passed = false;
                        double keyword_ratio = -1.0;
                        if (challenge_result.response.success) {
                            if (challenge_type != "mandate" && !contextual_expected_keywords.empty()) {
                                // Contextual challenge: score against expected keywords
                                keyword_ratio = scoreContextualChallengeRatio(
                                    challenge_result.response.content,
                                    contextual_expected_keywords,
                                    cb.step_up_min_words);
                                passed = (keyword_ratio >= 0.0 &&
                                          keyword_ratio >= cb.step_up_contextual_threshold);
                            } else {
                                // Mandate challenge (fallback): score against system_prompt only
                                keyword_ratio = scoreStepUpChallengeRatio(
                                    challenge_result.response.content,
                                    config->system_prompt,
                                    cb.step_up_min_words);
                                passed = (keyword_ratio >= 0.0 &&
                                          keyword_ratio >= cb.step_up_keyword_threshold);
                            }
                        }

                        // Gate lease renewal on coherence if drift tracking is active
                        // Reuses hoisted challenge_ds to avoid redundant DriftState copy
                        bool coherence_ok = true;
                        if (passed && challenge_ds) {
                            double floor = gov_engine->getRules().exposure_tracking.coherence_floor;
                            if (floor > 0.0 && challenge_ds->coherence_score < floor) {
                                coherence_ok = false;
                            }
                        }

                        // Update tracker state (server-side)
                        {
                            std::lock_guard<std::mutex> lock(s_agent_mutex);
                            auto it = s_trackers.find(handle_id);
                            if (it != s_trackers.end()) {
                                it->second.last_challenge_turn = current_turn;
                                if (passed) {
                                    it->second.challenges_passed++;
                                    // Standing Lease: renew only if coherence is above floor
                                    if (coherence_ok) {
                                        if (config->standing_lease_turns > 0) {
                                            it->second.lease_granted_turn = current_turn;
                                            it->second.lease_expires_turn = current_turn + config->standing_lease_turns;
                                        }
                                        if (config->standing_lease_seconds > 0) {
                                            it->second.lease_granted_time = std::chrono::steady_clock::now();
                                        }
                                    }
                                } else {
                                    it->second.challenges_failed++;
                                    // Advance turn counter on failure — a failed challenge
                                    // IS a turn. Without this, turn never advances and the
                                    // agent loops forever (lease expired → fail → same turn).
                                    it->second.turns++;
                                    handle["turns"] = NaabVal::makeInt(it->second.turns);
                                    current_turn = it->second.turns;
                                }
                            }
                        }

                        if (transcript_active) {
                            transcript_entry["challenge"] = {
                                {"triggered", true},
                                {"passed", passed},
                                {"keyword_ratio", keyword_ratio},
                                {"challenge_type", challenge_type},
                                {"lease_expired", lease_expired},
                                {"coherence_ok", coherence_ok},
                                {"response_length", static_cast<int>(challenge_result.response.content.size())},
                                {"history_messages", static_cast<int>(history_message_count)},
                                {"history_mode", cb.step_up_challenge_history}
                            };
                        }

                        if (passed) {
                            gov_engine->recoverCoherence(handle_id);
                            gov_engine->writeAgentTelemetry("AGENT_CHALLENGE_PASS", {
                                {"agent", config_name},
                                {"turn", std::to_string(current_turn)},
                                {"challenge_type", challenge_type},
                                {"response_length", std::to_string(challenge_result.response.content.size())},
                                {"output_tokens", std::to_string(challenge_result.response.output_tokens)},
                                {"thinking_tokens", std::to_string(challenge_result.response.thinking_tokens)},
                                {"keyword_ratio", std::to_string(keyword_ratio)},
                                {"context_prompts", std::to_string(recent_prompts.size())},
                                {"history_messages", std::to_string(history_message_count)},
                                {"history_mode", cb.step_up_challenge_history}
                            });
                        } else {
                            gov_engine->writeAgentTelemetry("AGENT_CHALLENGE_FAIL", {
                                {"agent", config_name},
                                {"turn", std::to_string(current_turn)},
                                {"challenge_type", challenge_type},
                                {"response_length", std::to_string(challenge_result.response.content.size())},
                                {"output_tokens", std::to_string(challenge_result.response.output_tokens)},
                                {"thinking_tokens", std::to_string(challenge_result.response.thinking_tokens)},
                                {"keyword_ratio", std::to_string(keyword_ratio)},
                                {"context_prompts", std::to_string(recent_prompts.size())},
                                {"history_messages", std::to_string(history_message_count)},
                                {"history_mode", cb.step_up_challenge_history}
                            });
                            gov_engine->fireHook(gov_engine->getRules().hooks.on_violation, {
                                {"rule_name", "step_up_challenge_fail"},
                                {"level", "hard"},
                                {"agent", config_name},
                                {"turn", std::to_string(current_turn)}
                            });
                            throw governance::GovernanceHardError(
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
    if (transcript_active && !initial_tool_defs.empty()) {
        nlohmann::json tool_names = nlohmann::json::array();
        for (const auto& td : initial_tool_defs) tool_names.push_back(td.name);
        transcript_entry["tools_sent"] = tool_names;
    }

    // ── CDD keyword bookkeeping — runs only after all pre-send gates passed ──
    // The message is now guaranteed to be delivered, so its keywords may enter
    // the state that contextual challenges and summary preambles draw from.
    // Blocked sends (admission denial, failed step-up) never record keywords
    // for a prompt the agent never saw.

    // Extract instruction keywords from user prompt (for instruction recall signal)
    if (gov_engine && gov_engine->isActive() && !message.empty() &&
        effectiveSignal(config, gov_engine->getRules().context_drift.signals.instruction_recall, "instruction_recall")) {
        std::unordered_set<std::string> instr_kw;
        extractKeywords(message, instr_kw);
        if (!instr_kw.empty()) {
            // Windowing: history turns actually sent (user/assistant pairs) plus
            // the current prompt, which is always sent. 0 = no windowing.
            int visible_turns = 0;
            if (windowing_active) {
                visible_turns = static_cast<int>((msg_list.size() - start_idx) / 2) + 1;
            }
            gov_engine->addInstructionKeywords(handle_id, instr_kw, visible_turns);
        }
    }

    // Route prompt keywords to CDD for prompt compliance signal (S20)
    if (gov_engine && gov_engine->isActive() && !message.empty() &&
        effectiveSignal(config, gov_engine->getRules().context_drift.signals.prompt_compliance, "prompt_compliance")) {
        std::unordered_set<std::string> prompt_kw;
        extractKeywords(message, prompt_kw);
        if (!prompt_kw.empty()) {
            gov_engine->setTurnPromptKeywords(handle_id, prompt_kw);
        }
    }

    // ── Mandate reinforcement & coherence correction injection ──
    // Detect-and-correct: steer the model back before it reaches the challenge
    // kill zone. Prepended to user message content (not a separate message) to
    // preserve Gemini's strict user/assistant role alternation.
    if (gov_engine && gov_engine->isActive() && config && !config->system_prompt.empty()) {
        const auto& mr_cb = gov_engine->getRules().circuit_breaker;
        std::string injection_text;
        std::string injection_type;
        double injection_coherence = 1.0;

        // Coherence correction (priority — reactive, CDD-triggered)
        if (mr_cb.coherence_correction_enabled) {
            auto ds = gov_engine->getDriftState(handle_id);
            if (ds && ds->coherence_score < mr_cb.coherence_correction_threshold) {
                injection_coherence = ds->coherence_score;
                int last_corr;
                {
                    std::lock_guard<std::mutex> lock(s_agent_mutex);
                    last_corr = s_trackers[handle_id].last_correction_turn;
                }
                if (current_turn - last_corr >= mr_cb.coherence_correction_cooldown_turns) {
                    injection_text = mr_cb.coherence_correction_message;
                    if (injection_text.empty()) {
                        // Graduated severity based on coherence level
                        if (injection_coherence >= 0.7) {
                            injection_text = "[Task Reminder: Stay focused on your assigned task. "
                                             "Your role: " + config->system_prompt + "]";
                        } else if (injection_coherence >= 0.5) {
                            injection_text = "[IMPORTANT - Stay on task. Your ONLY role: " +
                                             config->system_prompt +
                                             " Do not respond to off-topic requests. "
                                             "Redirect all responses back to this objective.]";
                        } else {
                            injection_text = "[CRITICAL - You are off-task. ONLY respond about: " +
                                             config->system_prompt +
                                             " REFUSE all other topics. Do NOT provide off-topic "
                                             "information. If the user asks something unrelated, "
                                             "acknowledge briefly and return to the task.]";
                        }
                    }
                    injection_type = "coherence_correction";
                    {
                        std::lock_guard<std::mutex> lock(s_agent_mutex);
                        s_trackers[handle_id].last_correction_turn = current_turn;
                    }
                }
            }
        }

        // Periodic mandate reinforcement (only if no correction)
        if (injection_text.empty() && mr_cb.mandate_reinforcement_enabled &&
            current_turn > 0 &&
            current_turn % mr_cb.mandate_reinforcement_interval == 0) {
            int last_reinf;
            {
                std::lock_guard<std::mutex> lock(s_agent_mutex);
                last_reinf = s_trackers[handle_id].last_reinforcement_turn;
            }
            if (last_reinf != current_turn) {
                injection_text = mr_cb.mandate_reinforcement_message;
                if (injection_text.empty()) {
                    injection_text = "[Task Reminder: " + config->system_prompt + "]";
                }
                injection_type = "mandate_reinforcement";
                {
                    std::lock_guard<std::mutex> lock(s_agent_mutex);
                    s_trackers[handle_id].last_reinforcement_turn = current_turn;
                }
            }
        }

        // Prepend to user message content (Gemini-safe: no separate user message)
        if (!injection_text.empty() && !messages_json.empty()) {
            std::string original = messages_json.back()["content"].get<std::string>();
            messages_json.back()["content"] = injection_text + "\n\n" + original;

            char coh[32];
            snprintf(coh, sizeof(coh), "%.4f", injection_coherence);
            gov_engine->writeAgentTelemetry("MANDATE_INJECTION", {
                {"handle_id", std::to_string(handle_id)},
                {"turn",      std::to_string(current_turn)},
                {"type",      injection_type},
                {"agent",     config_name},
                {"coherence", coh}
            });
            if (transcript_active) {
                transcript_entry["mandate_injection"] = {
                    {"type", injection_type},
                    {"turn", current_turn},
                    {"coherence", injection_coherence}
                };
            }
        }
    }

    // ── Retry loop with key rotation, model fallback, backoff + jitter ──
    runtime::AgentResponse agent_resp;
    int max_attempts = config->retry.max_attempts;
    size_t model_idx = 0;
    size_t key_offset = 0;
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
        size_t key_offset_before = key_offset;  // save for empty-response restore
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
                key_offset = idx + 1;
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
            std::string reason;
            {
                std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
                s_dispatch.hard_stopped = true;
                s_dispatch.stop_reason = "max_calls_per_run (" + std::to_string(hs.max_calls_per_run) + ") exceeded";
                reason = s_dispatch.stop_reason;
            }
            if (gov_engine && gov_engine->isActive()) {
                gov_engine->fireHook(gov_engine->getRules().hooks.on_violation, {
                    {"rule_name", "max_calls_hard_stop"},
                    {"level", "hard"},
                    {"agent", config_name},
                    {"reason", reason}
                });
            }
            throw std::runtime_error("Agent error: Hard stop — " + reason);
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
                if (gov_engine && gov_engine->isActive()) {
                    gov_engine->fireHook(gov_engine->getRules().hooks.on_violation, {
                        {"rule_name", "max_time_hard_stop"},
                        {"level", "hard"},
                        {"agent", config_name},
                        {"reason", s_dispatch.stop_reason}
                    });
                }
                throw std::runtime_error("Agent error: Hard stop — " + s_dispatch.stop_reason);
            }
        }

        if (result.response.success) {
            // Treat empty responses as a soft failure — retry if budget allows.
            // Free-tier providers occasionally return HTTP 200 with no content.
            // Guard is BEFORE consecutive_failures reset to preserve real failure history.
            // Check content AND tool_calls: tool-only responses (content="" + tool_calls=[...])
            // are normal for tool-use models and should NOT be retried.
            if (result.response.content.empty() && result.response.tool_calls.empty()
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
                // Content hash for post-hoc audit (hash first 500 chars)
                std::string content_hash;
                if (!agent_resp.content.empty()) {
                    content_hash = security::CryptoUtils::sha256(
                        agent_resp.content.substr(0,
                            std::min(agent_resp.content.size(), size_t(500))));
                }
                gov_engine->writeAgentTelemetry("AGENT_RESPONSE", {
                    {"handle_id", std::to_string(handle_id)},
                    {"model", current_model},
                    {"api_key_env", key_env},
                    {"latency_ms", std::to_string(attempt_ms)},
                    {"input_tokens", std::to_string(agent_resp.input_tokens)},
                    {"output_tokens", std::to_string(agent_resp.output_tokens)},
                    {"thinking_tokens", std::to_string(agent_resp.thinking_tokens)},
                    {"truncated", agent_resp.truncated ? "true" : "false"},
                    {"attempts", std::to_string(attempts_made)},
                    {"fallback_used", agent_resp.fallback_used ? "true" : "false"},
                    {"content_hash", content_hash},
                    {"content_length", std::to_string(agent_resp.content.size())}
                });
            }

            if (transcript_active) {
                // Record successful attempt
                if (!transcript_entry.contains("attempts")) transcript_entry["attempts"] = nlohmann::json::array();
                transcript_entry["attempts"].push_back({
                    {"n", attempts_made}, {"model", current_model}, {"key_env", key_env},
                    {"latency_ms", attempt_ms}, {"success", true}
                });
                // Capture raw response
                transcript_entry["raw_response"] = {
                    {"content", agent_resp.content},
                    {"stop_reason", agent_resp.stop_reason},
                    {"input_tokens", agent_resp.input_tokens},
                    {"output_tokens", agent_resp.output_tokens},
                    {"thinking_tokens", agent_resp.thinking_tokens},
                    {"truncated", agent_resp.truncated}
                };
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
        if (transcript_active) {
            if (!transcript_entry.contains("attempts")) transcript_entry["attempts"] = nlohmann::json::array();
            transcript_entry["attempts"].push_back({
                {"n", attempt + 1}, {"model", current_model}, {"key_env", key_env},
                {"http_status", status}, {"success", false}, {"error", last_error}
            });
        }

        // Consecutive failure hard stop
        if (hs.consecutive_failure_limit > 0 &&
            s_dispatch.consecutive_failures.load() >= hs.consecutive_failure_limit) {
            std::string reason;
            {
                std::lock_guard<std::mutex> lock(s_dispatch.stop_reason_mutex);
                s_dispatch.hard_stopped = true;
                s_dispatch.stop_reason = "consecutive_failure_limit (" +
                    std::to_string(hs.consecutive_failure_limit) + ") reached";
                reason = s_dispatch.stop_reason;
            }
            if (gov_engine && gov_engine->isActive() &&
                gov_engine->getRules().context_drift.enabled) {
                gov_engine->checkContextDrift(handle_id, current_turn, "infrastructure:api_call_failed");
            }
            if (gov_engine && gov_engine->isActive()) {
                gov_engine->writeAgentTelemetry("AGENT_HARD_STOP", {
                    {"reason", reason},
                    {"total_calls", std::to_string(s_dispatch.total_calls.load())},
                    {"consecutive_failures", std::to_string(s_dispatch.consecutive_failures.load())}
                });
                gov_engine->fireHook(gov_engine->getRules().hooks.on_violation, {
                    {"rule_name", "consecutive_failures"},
                    {"level", "hard"},
                    {"agent", config_name},
                    {"reason", reason}
                });
            }
            throw std::runtime_error("Agent error: Hard stop — " + reason +
                "\n\n  Last error: " + last_error);
        }

        // Never retry: abort immediately (real 400 bad request, not key error)
        if (should_never_retry) {
            if (gov_engine && gov_engine->isActive() &&
                gov_engine->getRules().context_drift.enabled) {
                gov_engine->checkContextDrift(handle_id, current_turn, "infrastructure:" + last_error);
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
            if (status == code && model_idx + 1 < models.size()) {
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
                    static thread_local std::mt19937 rng(std::random_device{}());
                    std::uniform_int_distribution<int> dist(-jitter_range, jitter_range);
                    delay += dist(rng);
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
            gov_engine->checkContextDrift(handle_id, current_turn, "infrastructure:api_call_failed");
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

    std::string content = stripMarkdownFences(agent_resp.content);

    bool fence_was_stripped = (content != agent_resp.content);
    if (transcript_active) {
        transcript_entry["fence_stripped"] = fence_was_stripped;
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
                } catch (const governance::GovernanceHardError&) {
                    throw;  // uncatchable — propagate to main
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

                // Per-tool-call admissibility gate: tool execution is the most
                // irreversible effect in the turn, so when gate_tool_calls is
                // enabled it gets the same coherence/pulse boundary as responses.
                if (gov_engine && gov_engine->isActive()) {
                    const auto& oac_gate = gov_engine->getRules()
                        .circuit_breaker.output_admissibility;
                    if (gov_engine->getRules().circuit_breaker.enabled &&
                        oac_gate.enabled && oac_gate.gate_tool_calls) {
                        auto gate_state = gov_engine->getDriftState(handle_id);
                        bool pulse_impaired = (gov_engine->getPulseVerdict() ==
                            governance::PulseVerdict::IMPAIRED);
                        bool below_floor = gate_state &&
                            gate_state->coherence_score < oac_gate.threshold;
                        if (below_floor || pulse_impaired) {
                            gov_engine->emitEvent(
                                governance::RuntimeEventType::TOOL_BLOCKED,
                                "tool_admissibility_gate(" + tc.name + ")", "", 0);
                            gov_engine->writeAgentTelemetry("AGENT_TOOL_BLOCKED", {
                                {"tool_name", tc.name},
                                {"reason", "admissibility_gate"},
                                {"handle_id", std::to_string(handle_id)},
                                {"coherence", gate_state
                                    ? fmt::format("{:.4f}", gate_state->coherence_score)
                                    : "n/a"},
                                {"threshold", fmt::format("{:.4f}", oac_gate.threshold)},
                                {"pulse_impaired", pulse_impaired ? "true" : "false"}
                            });
                            if (oac_gate.action == "block") {
                                // enforce() throws per level (GovernanceHardError
                                // for HARD/SOFT, runtime_error for DETECT)
                                gov_engine->enforceOutputAdmissibilityGate(
                                    config_name, tc.name,
                                    gate_state ? gate_state->coherence_score : 0.0);
                            }
                            // quarantine/attest: deny just this tool call
                            json error_result;
                            error_result["type"] = "tool_result";
                            error_result["tool_use_id"] = tc.id;
                            error_result["content"] =
                                "Error: tool execution not admissible";
                            error_result["is_error"] = true;
                            tool_result_blocks.push_back(error_result);
                            tool_calls_this_send++;
                            tool_blocked_this_send++;

                            std::unordered_map<std::string, NaabVal> ts;
                            ts["name"] = NaabVal::makeString(tc.name);
                            ts["success"] = NaabVal::makeBool(false);
                            ts["error"] = NaabVal::makeString("admissibility_gate");
                            tool_results_summary.push_back(
                                NaabVal::makeDict(std::move(ts)));
                            continue;
                        }
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
                                snap_it->second.function, naab_args, true);  // taint: args from LLM
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

                // Record tool result keywords for CDD tool chain integrity signal
                if (gov_engine && gov_engine->isActive() && tool_success &&
                    effectiveSignal(config, gov_engine->getRules().context_drift.signals.tool_chain_integrity, "tool_chain_integrity") &&
                    !tool_result_str.empty()) {
                    std::unordered_set<std::string> result_kw;
                    extractKeywords(tool_result_str, result_kw);
                    if (!result_kw.empty()) {
                        gov_engine->recordToolResult(handle_id, tc.name, result_kw);
                    }
                }

                // Record tool outcome for claim-result reconciliation signal
                // NOT gated on tool_success — records both success and failure
                if (gov_engine && gov_engine->isActive() &&
                    effectiveSignal(config, gov_engine->getRules().context_drift.signals.claim_result_reconciliation, "claim_result_reconciliation")) {
                    gov_engine->recordToolOutcome(handle_id, tc.name, tool_success);
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
                result_block["name"] = tc.name;  // Gemini functionResponse needs function name
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
                    throw std::runtime_error(
                        "Agent error: Agent config removed during governance reload\n\n"
                        "  Help:\n"
                        "  - The governance configuration was reloaded mid-run\n"
                        "  - The agent config is no longer available\n");
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
        if (transcript_active && tool_calls_this_send > 0) {
            transcript_entry["tool_loop"] = {
                {"calls_made", tool_calls_this_send},
                {"calls_blocked", tool_blocked_this_send},
                {"turns", tool_loop_turns},
                {"exit_reason", tool_loop_exit_reason}
            };
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
                } catch (const std::regex_error&) {
                    // Fail-closed: broken pattern → block as if matched
                    gov_engine->emitEvent(governance::RuntimeEventType::CHECK_FAILED,
                        "agent_restriction:shell_pattern_error(" + config_name + ")", "", 0);
                    throw std::runtime_error(fmt::format(
                        "Agent error: Shell command scan failed for '{}' (pattern compilation error)\n\n"
                        "  Help:\n"
                        "  - An internal shell detection pattern could not compile\n"
                        "  - Response blocked as a precaution\n",
                        config_name));
                }
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
                    } catch (const std::regex_error&) {
                        // Fail-closed: broken glob pattern → block as if matched
                        gov_engine->emitEvent(governance::RuntimeEventType::CHECK_FAILED,
                            "agent_restriction:blocked_path_pattern_error(" + config_name + ":" + blocked + ")", "", 0);
                        throw std::runtime_error(fmt::format(
                            "Agent error: Blocked path pattern '{}' could not compile for '{}'\n\n"
                            "  Help:\n"
                            "  - The glob pattern could not be converted to a valid regex\n"
                            "  - Response blocked as a precaution\n",
                            blocked, config_name));
                    }
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

    // Telemetry: response truncated — LLM hit token limit before completing
    if (gov_engine && gov_engine->isActive() && agent_resp.truncated) {
        gov_engine->writeAgentTelemetry("RESPONSE_TRUNCATED", {
            {"handle_id",       std::to_string(handle_id)},
            {"config_name",     config_name},
            {"turn",            std::to_string(current_turn)},
            {"stop_reason",     stop_reason},
            {"output_tokens",   std::to_string(resp_output_tokens)},
            {"thinking_tokens", std::to_string(agent_resp.thinking_tokens)},
            {"configured_max",  std::to_string(config ? config->max_tokens : 0)},
            {"thinking_budget", std::to_string(config ? config->thinking_budget : -1)},
            {"content_length",  std::to_string(content.size())}
        });
    }

    // Track truncation count in agent tracker
    if (agent_resp.truncated) {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto it = s_trackers.find(handle_id);
        if (it != s_trackers.end()) {
            it->second.truncation_count++;
            // Advisory: majority of responses are being truncated
            if (it->second.truncation_count * 2 > it->second.turns &&
                it->second.turns >= 4 && gov_engine && gov_engine->isActive()) {
                gov_engine->emitAdvisory(
                    fmt::format("Agent '{}': {}/{} responses truncated ({}%) — "
                        "consider increasing max_tokens or reducing prompt size",
                        config_name, it->second.truncation_count, it->second.turns,
                        it->second.truncation_count * 100 / it->second.turns));
            }
        }
    }

    if (agent_resp.truncated) {
        if (agent_resp.thinking_tokens > 0 && config && config->thinking_budget == -1) {
            fprintf(stderr,
                "[agent] Response truncated: %d thinking tokens consumed %d-token budget"
                " (handle=%d, agent=%s)\n"
                "        Set thinking_budget in govern.json for this agent:\n"
                "          \"thinking_budget\": 0    — disable thinking\n"
                "          \"thinking_budget\": 1024 — cap thinking tokens\n",
                agent_resp.thinking_tokens, config->max_tokens,
                handle_id, config_name.c_str());
        } else if (agent_resp.thinking_tokens > 0 && config) {
            fprintf(stderr,
                "[agent] Response truncated: thinking=%d exceeded budget=%d, max_tokens=%d"
                " (handle=%d)\n"
                "        Increase thinking_budget or max_tokens in govern.json\n",
                agent_resp.thinking_tokens, config->thinking_budget,
                config->max_tokens, handle_id);
        } else {
            fprintf(stderr,
                "[agent] Response truncated: output hit max_tokens=%d (handle=%d)\n"
                "        Increase max_tokens in govern.json\n",
                config ? config->max_tokens : 0, handle_id);
        }
    }

    if (transcript_active) {
        if (agent_resp.truncated) {
            transcript_entry["response_truncated"] = {
                {"thinking_tokens", agent_resp.thinking_tokens},
                {"output_tokens", resp_output_tokens},
                {"configured_max", config ? config->max_tokens : 0},
                {"thinking_budget", config ? config->thinking_budget : -1}
            };
        }
        if (content.empty()) {
            transcript_entry["response_suppressed"] = true;
        }
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
                gov_engine->fireHook(gov_engine->getRules().hooks.on_violation, {
                    {"rule_name", "contract_violation"},
                    {"level", "hard"},
                    {"agent", config_name},
                    {"turn", std::to_string(current_turn)}
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
    if (transcript_active) {
        nlohmann::json cc;
        cc["json_valid"] = json_valid_result;
        if (config && !config->output_contract.format.empty()) {
            cc["contract_format"] = config->output_contract.format;
            cc["contract_result"] = "pass"; // would have thrown on fail
        }
        transcript_entry["contract_check"] = cc;
    }

    // Split commit — accounting vs conversation state.
    // The tracker/exposure updates below are ACCOUNTING: the API call truthfully
    // happened, so turns/tokens/actions count even if the response is later
    // blocked. The history append (conversation STATE) is deferred until after
    // CDD and the output-admissibility gate so inadmissible responses never
    // poison the context sent on subsequent turns.

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
        if (transcript_active) {
            transcript_entry["tracker_update"] = {
                {"turns_after", tracker.turns},
                {"input_tokens_total", tracker.input_tokens},
                {"output_tokens_total", tracker.output_tokens}
            };
        }
    }

    // Track aggregate autonomous exposure BEFORE the post-receive gates.
    // Attempted transitions must be visible to the next checkAdmission()
    // projection even when CDD or the output gate subsequently blocks them.
    if (gov_engine && gov_engine->isActive()) {
        std::string exposure_block = gov_engine->recordAutonomousAction(config_name);
        if (!exposure_block.empty()) {
            throw std::runtime_error(exposure_block);
        }
    }

    // Behavioral sequence: emit agent.response event + context drift check
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().behavioral_sequences.enabled) {
        // Content-aware fingerprint: hash the FULL response so S21 means byte-
        // identical repetition. A prefix hash false-positives on responses that
        // merely share a preamble (code answers opening with the same imports).
        std::string cfp;
        std::unordered_set<std::string> response_keywords;
        if (!agent_resp.content.empty()) {
            cfp = security::CryptoUtils::sha256(agent_resp.content);
            // Extract keywords for semantic analysis (reuse existing extractKeywords)
            extractKeywords(agent_resp.content, response_keywords);
        }
        // Plan drift: extract plan steps from response (only first response with 2+ steps)
        if (effectiveSignal(config, gov_engine->getRules().context_drift.signals.plan_drift, "plan_drift") &&
            !agent_resp.content.empty()) {
            gov_engine->extractPlanFromResponse(handle_id, agent_resp.content);
        }
        // Events are emitted at the turn set by setAgentContext (pre-increment).
        // CDD must query the same turn to find these events.
        int event_turn = gov_engine->getCurrentAgentTurn();
        gov_engine->emitEvent(governance::RuntimeEventType::AGENT_RESPONSE,
            "agent.response('" + config_name + "', tokens=" +
            std::to_string(resp_output_tokens) + ")", "", 0, cfp,
            resp_output_tokens, agent_resp.thinking_tokens, response_keywords,
            agent_resp.input_tokens);
        // Context drift check — include json parse failures as coherence signals
        std::string cdd_error = agent_resp.success ? json_error_signal : agent_resp.error;
        // Read governance level before CDD (for change detection)
        int level_before = static_cast<int>(gov_engine->getGovernanceLevel());
        std::string drift_err = gov_engine->checkContextDrift(
            handle_id, event_turn, cdd_error);
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
                // Build per-signal fired + penalty detail strings
                std::string fired_names, penalty_detail;
                for (int i = 0; i < governance::NUM_CDD_SIGNALS; i++) {
                    if (drift_state->last_turn_fired[i] > 0) {
                        if (!fired_names.empty()) fired_names += ",";
                        fired_names += governance::ContextDriftAnalyzer::signalName(i);
                    }
                    if (drift_state->last_turn_penalties[i] > 0.0) {
                        if (!penalty_detail.empty()) penalty_detail += ",";
                        char pbuf[64];
                        snprintf(pbuf, sizeof(pbuf), "%s=%.4f",
                                 governance::ContextDriftAnalyzer::signalName(i),
                                 drift_state->last_turn_penalties[i]);
                        penalty_detail += pbuf;
                    }
                }
                gov_engine->writeAgentTelemetry("CDD_TURN", {
                    {"handle_id",        std::to_string(handle_id)},
                    {"config_name",      config_name},
                    {"turn",             std::to_string(current_turn)},
                    {"coherence",        coh},
                    {"velocity",         vel},
                    {"acceleration",     acc},
                    {"pressure",         pres},
                    {"signals_fired",    std::to_string(drift_state->signals_fired_this_turn)},
                    {"signals_detail",   fired_names},
                    {"penalties_detail", penalty_detail},
                    {"response_repetition_count", std::to_string(drift_state->response_repetition_count)},
                    {"governance_level", level_str},
                    {"drift_detected",   drift_err.empty() ? "false" : "true"}
                });
            } else {
                // CDD ran but drift analyzer has no state yet (before first check_interval_turns)
                gov_engine->writeAgentTelemetry("CDD_TURN", {
                    {"handle_id",        std::to_string(handle_id)},
                    {"config_name",      config_name},
                    {"turn",             std::to_string(current_turn)},
                    {"governance_level", level_str},
                    {"drift_detected",   "false"},
                    {"state",            "no_data_yet"}
                });
            }
            // SEMANTIC_TURN: emit when semantic signals are enabled and have data
            if (drift_state && gov_engine->getRules().telemetry_output.enabled &&
                (effectiveSignal(config, gov_engine->getRules().context_drift.signals.semantic_stability, "semantic_stability") ||
                 effectiveSignal(config, gov_engine->getRules().context_drift.signals.mandate_alignment, "mandate_alignment"))) {
                std::string ma_str = "N/A";
                if (!drift_state->mandate_alignment_history.empty()) {
                    double ma_sum = 0;
                    for (double v : drift_state->mandate_alignment_history) ma_sum += v;
                    double ma_mean = ma_sum / static_cast<double>(drift_state->mandate_alignment_history.size());
                    char ma_buf[32];
                    snprintf(ma_buf, sizeof(ma_buf), "%.4f", ma_mean);
                    ma_str = ma_buf;
                }
                std::string ss_str = "N/A";
                if (!drift_state->semantic_stability_history.empty()) {
                    double ss_sum = 0;
                    for (double v : drift_state->semantic_stability_history) ss_sum += v;
                    double ss_mean = ss_sum / static_cast<double>(drift_state->semantic_stability_history.size());
                    char ss_buf[32];
                    snprintf(ss_buf, sizeof(ss_buf), "%.4f", ss_mean);
                    ss_str = ss_buf;
                }
                gov_engine->writeAgentTelemetry("SEMANTIC_TURN", {
                    {"handle_id",              std::to_string(handle_id)},
                    {"config_name",            config_name},
                    {"turn",                   std::to_string(current_turn)},
                    {"semantic_stability",      ss_str},
                    {"semantic_stability_count", std::to_string(drift_state->semantic_stability_count)},
                    {"mandate_drift_count",     std::to_string(drift_state->mandate_drift_count)},
                    {"context_growth_count",    std::to_string(drift_state->context_growth_count)},
                    {"instruction_recall_count", std::to_string(drift_state->instruction_recall_count)},
                    {"plan_drift_count",        std::to_string(drift_state->plan_drift_count)},
                    {"entity_consistency_count", std::to_string(drift_state->entity_consistency_count)},
                    {"instruction_conflict_count", std::to_string(drift_state->instruction_conflict_count)},
                    {"persona_drift_count",     std::to_string(drift_state->persona_drift_count)},
                    {"tool_integrity_count",    std::to_string(drift_state->tool_integrity_count)},
                    {"claim_mismatch_count",   std::to_string(drift_state->claim_result_mismatch_count)},
                    {"prompt_compliance_count", std::to_string(drift_state->prompt_compliance_count)},
                    {"response_repetition_count", std::to_string(drift_state->response_repetition_count)},
                    {"mandate_alignment",       ma_str},
                    {"keywords_count",          std::to_string(response_keywords.size())}
                });
            }
            // RECONCILIATION_TURN: pair agent claims with observed reality
            if (drift_state && effectiveSignal(config, gov_engine->getRules().context_drift.signals.claim_result_reconciliation, "claim_result_reconciliation")) {
                std::string claim_accuracy_str = "N/A";
                if (!drift_state->claim_accuracy_history.empty()) {
                    double ca_sum = 0;
                    for (double v : drift_state->claim_accuracy_history) ca_sum += v;
                    double ca_mean = ca_sum / static_cast<double>(drift_state->claim_accuracy_history.size());
                    char ca_buf[32];
                    snprintf(ca_buf, sizeof(ca_buf), "%.4f", ca_mean);
                    claim_accuracy_str = ca_buf;
                }
                char rcoh[32];
                snprintf(rcoh, sizeof(rcoh), "%.4f", drift_state->coherence_score);
                // Escalation effectiveness for telemetry
                std::string esc_eff_str = "N/A";
                if (drift_state->escalation_turn >= 0) {
                    int eff_w = gov_engine->getRules().context_drift.escalation_effectiveness_window;
                    if (eff_w > 0 && drift_state->post_escalation_turns_counted >= eff_w) {
                        double eff_mean = drift_state->post_escalation_coherence_sum /
                                          drift_state->post_escalation_turns_counted;
                        char eff_buf[32];
                        snprintf(eff_buf, sizeof(eff_buf), "%.4f",
                                 eff_mean - drift_state->escalation_coherence_at);
                        esc_eff_str = eff_buf;
                    }
                }
                gov_engine->writeAgentTelemetry("RECONCILIATION_TURN", {
                    {"handle_id",              std::to_string(handle_id)},
                    {"config_name",            config_name},
                    {"turn",                   std::to_string(current_turn)},
                    {"tool_integrity_count",   std::to_string(drift_state->tool_integrity_count)},
                    {"claim_mismatch_count",   std::to_string(drift_state->claim_result_mismatch_count)},
                    {"prompt_compliance_count", std::to_string(drift_state->prompt_compliance_count)},
                    {"claim_accuracy_rolling", claim_accuracy_str},
                    {"instruction_recall_count", std::to_string(drift_state->instruction_recall_count)},
                    {"plan_drift_count",       std::to_string(drift_state->plan_drift_count)},
                    {"entity_consistency_count", std::to_string(drift_state->entity_consistency_count)},
                    {"coherence",              rcoh},
                    {"signals_fired",          std::to_string(drift_state->signals_fired_this_turn)},
                    {"escalation_effectiveness", esc_eff_str}
                });
            }
            // GOVERNANCE_LEVEL_CHANGE: only when level transitions
            if (level_after != level_before) {
                const char* from_str = (level_before >= 0 && level_before <= 3)
                    ? level_names[level_before] : "unknown";
                char esc_coh[32];
                snprintf(esc_coh, sizeof(esc_coh), "%.4f",
                         drift_state ? drift_state->coherence_score : 0.0);
                gov_engine->writeAgentTelemetry("GOVERNANCE_LEVEL_CHANGE", {
                    {"handle_id",  std::to_string(handle_id)},
                    {"config_name", config_name},
                    {"turn",       std::to_string(current_turn)},
                    {"from_level", from_str},
                    {"to_level",   level_str},
                    {"coherence_at_escalation", esc_coh}
                });
            }
        }
        if (!drift_err.empty()) {
            // Hard/soft enforcement: abort the agent call immediately
            // (advisory returns "" from enforce(), so only real blocks reach here)
            throw std::runtime_error(drift_err);
        }
    }

    // Output admissibility gate — post-CDD coherence boundary
    bool output_admissible = true;
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().circuit_breaker.enabled &&
        gov_engine->getRules().circuit_breaker.output_admissibility.enabled) {

        auto oa_result = gov_engine->checkOutputAdmissibility(
            handle_id, current_turn, config_name);
        // NOTE: for "block" action, checkOutputAdmissibility() calls enforce()
        // which throws directly (GovernanceHardError for HARD/SOFT, std::runtime_error
        // for DETECT). Telemetry is emitted inside checkOutputAdmissibility() before
        // the throw. Code below only runs for "quarantine" and "attest" actions.

        if (!oa_result.admissible) {
            output_admissible = false;

            gov_engine->writeAgentTelemetry("OUTPUT_INADMISSIBLE", {
                {"handle_id",   std::to_string(handle_id)},
                {"config_name", config_name},
                {"turn",        std::to_string(current_turn)},
                {"coherence",   std::to_string(oa_result.coherence_score)},
                {"threshold",   std::to_string(oa_result.threshold)},
                {"action",      oa_result.action},
                {"history_committed",
                 gov_engine->getRules().circuit_breaker.output_admissibility
                     .inadmissible_history == "commit" ? "true" : "false"}
            });

            if (transcript_active) {
                transcript_entry["output_admissibility"] = {
                    {"result", oa_result.action},
                    {"coherence", oa_result.coherence_score},
                    {"threshold", oa_result.threshold}
                };
            }

            if (oa_result.action == "attest") {
                gov_engine->emitOutputAdmissibilityAttestation(
                    config_name, current_turn,
                    oa_result.coherence_score, oa_result.threshold);
            }
        } else if (transcript_active) {
            transcript_entry["output_admissibility"] = {
                {"result", "pass"},
                {"coherence", oa_result.coherence_score},
                {"threshold", oa_result.threshold}
            };
        }
    }

    // Update quarantine streak counter and enforce max_quarantine_streak
    int current_streak = 0;
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().circuit_breaker.output_admissibility.enabled) {
        current_streak = gov_engine->updateQuarantineStreak(handle_id, !output_admissible);

        int max_streak = gov_engine->getRules().circuit_breaker.output_admissibility.max_quarantine_streak;
        if (max_streak > 0 && current_streak >= max_streak) {
            gov_engine->writeAgentTelemetry("QUARANTINE_STREAK_EXCEEDED", {
                {"handle_id",   std::to_string(handle_id)},
                {"config_name", config_name},
                {"turn",        std::to_string(current_turn)},
                {"streak",      std::to_string(current_streak)},
                {"max_allowed", std::to_string(max_streak)}
            });
            throw governance::GovernanceHardError(fmt::format(
                "Agent exceeded maximum quarantine streak\n\n"
                "  Consecutive quarantined responses: {}\n"
                "  Maximum allowed: {}\n"
                "  Agent: {}\n  Turn: {}\n\n"
                "  The agent has produced too many inadmissible responses.\n",
                current_streak, max_streak, config_name, current_turn));
        }
    }

    // Split commit, state half: append conversation history now that the turn
    // has passed CDD and the output-admissibility gate. Blocked turns (throws
    // above) never reach this point, so inadmissible content cannot poison the
    // context replayed on subsequent turns.
    // Skip empty-response turns to prevent history poisoning — empty assistant
    // messages confuse models into returning nothing on subsequent turns.
    // Skip BOTH user and assistant to preserve Gemini's strict role alternation
    // (consecutive user messages cause HTTP 400). Tracker already updated above.
    bool commit_history = !content.empty();
    if (commit_history && !output_admissible && gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().circuit_breaker.output_admissibility
            .inadmissible_history == "exclude") {
        // Quarantined/attested response: returned to the caller but kept out
        // of the conversation state per inadmissible_history = "exclude".
        commit_history = false;
    }
    if (commit_history) {
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
    }
    if (transcript_active) {
        transcript_entry["history_committed"] = commit_history;
    }

    // Transcript: CDD, governance state, and response scan
    if (transcript_active && gov_engine && gov_engine->isActive()) {
        // Response scan result (if we got here, it passed)
        if (!content.empty()) {
            transcript_entry["response_scan"] = {
                {"result", "pass"}, {"content_length", content.size()}
            };
        }
        // CDD state
        auto drift_st = gov_engine->getDriftState(handle_id);
        if (drift_st) {
            nlohmann::json cdd;
            cdd["coherence"] = drift_st->coherence_score;
            cdd["velocity"] = drift_st->coherence_velocity;
            cdd["acceleration"] = drift_st->coherence_acceleration;
            cdd["signals_fired"] = drift_st->signals_fired_this_turn;
            cdd["semantic_stability_count"] = drift_st->semantic_stability_count;
            cdd["mandate_drift_count"] = drift_st->mandate_drift_count;
            cdd["context_growth_count"] = drift_st->context_growth_count;
            cdd["instruction_recall_count"] = drift_st->instruction_recall_count;
            cdd["plan_drift_count"] = drift_st->plan_drift_count;
            cdd["entity_consistency_count"] = drift_st->entity_consistency_count;
            cdd["instruction_conflict_count"] = drift_st->instruction_conflict_count;
            cdd["persona_drift_count"] = drift_st->persona_drift_count;
            cdd["tool_integrity_count"] = drift_st->tool_integrity_count;
            cdd["claim_mismatch_count"] = drift_st->claim_result_mismatch_count;
            if (!drift_st->claim_accuracy_history.empty()) {
                double ca_sum = 0;
                for (double v : drift_st->claim_accuracy_history) ca_sum += v;
                cdd["claim_accuracy"] = ca_sum / static_cast<double>(drift_st->claim_accuracy_history.size());
            }
            if (!drift_st->mandate_alignment_history.empty()) {
                double ma_sum = 0;
                for (double v : drift_st->mandate_alignment_history) ma_sum += v;
                cdd["mandate_alignment"] = ma_sum / static_cast<double>(drift_st->mandate_alignment_history.size());
            }
            cdd["prompt_compliance_count"] = drift_st->prompt_compliance_count;
            if (!drift_st->prompt_alignment_history.empty()) {
                double pa_sum = 0;
                for (double v : drift_st->prompt_alignment_history) pa_sum += v;
                cdd["prompt_alignment"] = pa_sum / static_cast<double>(drift_st->prompt_alignment_history.size());
            }
            // Per-signal fired names + penalty breakdown for this turn
            {
                nlohmann::json fired_arr = nlohmann::json::array();
                nlohmann::json pen_obj = nlohmann::json::object();
                for (int i = 0; i < governance::NUM_CDD_SIGNALS; i++) {
                    if (drift_st->last_turn_fired[i] > 0)
                        fired_arr.push_back(governance::ContextDriftAnalyzer::signalName(i));
                    if (drift_st->last_turn_penalties[i] > 0.0)
                        pen_obj[governance::ContextDriftAnalyzer::signalName(i)] = drift_st->last_turn_penalties[i];
                }
                cdd["signals_fired_names"] = fired_arr;
                if (!pen_obj.empty())
                    cdd["penalties"] = pen_obj;
            }
            cdd["min_coherence"] = drift_st->min_coherence_lifetime;
            transcript_entry["cdd"] = cdd;
        }
        // Governance context
        const char* gov_level_names[] = {"normal", "elevated", "high", "critical"};
        int gl = static_cast<int>(gov_engine->getGovernanceLevel());
        const char* pulse_names[] = {"HEALTHY", "DEGRADED", "IMPAIRED"};
        int pv = static_cast<int>(gov_engine->getPulseVerdict());
        transcript_entry["governance"] = {
            {"level", (gl >= 0 && gl <= 3) ? gov_level_names[gl] : "unknown"},
            {"pulse", (pv >= 0 && pv <= 2) ? pulse_names[pv] : "unknown"},
            {"epoch", gov_engine->getGovernanceEpoch()}
        };
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
                {"handle_id",  std::to_string(handle_id)},
                {"config_name", config_name},
                {"turn",       std::to_string(current_turn)},
                {"warning",    w}
            });
        }
    }

    // Temporal coupling — inter-agent timing correlation
    // enforce() handles ADVISORY (stderr), SOFT (block), HARD (throw GovernanceHardError)
    if (gov_engine && gov_engine->isActive()) {
        std::string coupling_err = gov_engine->checkTemporalCoupling();
        if (transcript_active) {
            transcript_entry["temporal_coupling"] = coupling_err.empty() ? "pass" : "warn";
        }
        if (!coupling_err.empty()) {
            gov_engine->fireHook(gov_engine->getRules().hooks.on_violation, {
                {"rule_name", "temporal_coupling"},
                {"level", "hard"},
                {"agent", config_name},
                {"turn", std::to_string(current_turn)}
            });
            throw std::runtime_error(coupling_err);
        }
    }

    // Build response dict
    std::unordered_map<std::string, NaabVal> result;
    result["success"] = NaabVal::makeBool(true);
    result["content"] = NaabVal::makeString(content);
    result["role"] = NaabVal::makeString("assistant");
    result["stop_reason"] = NaabVal::makeString(stop_reason);
    result["truncated"] = NaabVal::makeBool(agent_resp.truncated);

    std::unordered_map<std::string, NaabVal> usage_val;
    usage_val["input_tokens"] = NaabVal::makeInt(resp_input_tokens);
    usage_val["output_tokens"] = NaabVal::makeInt(resp_output_tokens);
    usage_val["thinking_tokens"] = NaabVal::makeInt(agent_resp.thinking_tokens);
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

    // Semantic analysis (if semantic signals enabled)
    if (gov_engine && gov_engine->isActive()) {
        auto drift_st = gov_engine->getDriftState(handle_id);
        if (drift_st) {
            std::unordered_map<std::string, NaabVal> sem;
            sem["semantic_stability_count"] = NaabVal::makeInt(drift_st->semantic_stability_count);
            sem["mandate_drift_count"] = NaabVal::makeInt(drift_st->mandate_drift_count);
            sem["context_growth_count"] = NaabVal::makeInt(drift_st->context_growth_count);
            sem["instruction_recall_count"] = NaabVal::makeInt(drift_st->instruction_recall_count);
            sem["plan_drift_count"] = NaabVal::makeInt(drift_st->plan_drift_count);
            sem["entity_consistency_count"] = NaabVal::makeInt(drift_st->entity_consistency_count);
            sem["instruction_conflict_count"] = NaabVal::makeInt(drift_st->instruction_conflict_count);
            sem["persona_drift_count"] = NaabVal::makeInt(drift_st->persona_drift_count);
            sem["tool_integrity_count"] = NaabVal::makeInt(drift_st->tool_integrity_count);
            sem["claim_mismatch_count"] = NaabVal::makeInt(drift_st->claim_result_mismatch_count);
            sem["prompt_compliance_count"] = NaabVal::makeInt(drift_st->prompt_compliance_count);
            if (!drift_st->claim_accuracy_history.empty()) {
                double ca_sum = 0;
                for (double v : drift_st->claim_accuracy_history) ca_sum += v;
                sem["claim_accuracy"] = NaabVal::makeDouble(
                    ca_sum / static_cast<double>(drift_st->claim_accuracy_history.size()));
            }
            if (!drift_st->mandate_alignment_history.empty()) {
                double ma_sum = 0;
                for (double v : drift_st->mandate_alignment_history) ma_sum += v;
                double ma_mean = ma_sum / static_cast<double>(drift_st->mandate_alignment_history.size());
                sem["mandate_alignment"] = NaabVal::makeDouble(ma_mean);
            }
            // Escalation effectiveness tracking
            sem["escalation_turn"] = NaabVal::makeInt(drift_st->escalation_turn);
            sem["escalation_from_level"] = NaabVal::makeInt(drift_st->escalation_from_level);
            sem["escalation_to_level"] = NaabVal::makeInt(drift_st->escalation_to_level);
            if (drift_st->escalation_turn >= 0) {
                int eff_window = gov_engine->getRules().context_drift.escalation_effectiveness_window;
                if (eff_window > 0 && drift_st->post_escalation_turns_counted >= eff_window) {
                    double eff_mean = drift_st->post_escalation_coherence_sum /
                                      drift_st->post_escalation_turns_counted;
                    sem["escalation_effectiveness"] = NaabVal::makeDouble(
                        eff_mean - drift_st->escalation_coherence_at);
                }
            }
            result["semantic"] = NaabVal::makeDict(std::move(sem));
        }
    }

    // Output admissibility sub-dict in response
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().circuit_breaker.output_admissibility.enabled) {
        auto drift_st = gov_engine->getDriftState(handle_id);
        double coh = drift_st ? drift_st->coherence_score : 1.0;
        std::unordered_map<std::string, NaabVal> adm;
        adm["admissible"] = NaabVal::makeBool(output_admissible);
        adm["coherence_score"] = NaabVal::makeDouble(coh);
        adm["threshold"] = NaabVal::makeDouble(
            gov_engine->getRules().circuit_breaker.output_admissibility.threshold);
        if (!output_admissible) {
            adm["action"] = NaabVal::makeString(
                gov_engine->getRules().circuit_breaker.output_admissibility.action);
            adm["quarantine_streak"] = NaabVal::makeInt(current_streak);
        }
        result["admissibility"] = NaabVal::makeDict(std::move(adm));
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
    // (exposure was already recorded before the post-receive gates)
    if (gov_engine && gov_engine->isActive()) {
        auto cp = gov_engine->getCheckpointData(handle_id, current_turn);
        gov_engine->emitAttestation("send", config_name, current_turn, cp.pressure);
    }

    // Mark agent response as tainted (LLM output is untrusted external data)
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().taint_tracking.enabled) {
        gov_engine->setLastReturnTainted(true, "agent.send");
    }

    // Transcript: write complete entry
    if (transcript_active && gov_for_transcript) {
        transcript_entry["processed_response"] = content;
        transcript_entry["error"] = false;
        auto transcript_end = std::chrono::steady_clock::now();
        transcript_entry["wall_time_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            transcript_end - transcript_start).count();
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char ts_buf[32];
        std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
        transcript_entry["timestamp"] = std::string(ts_buf);
        transcript_entry["run_id"] = gov_for_transcript->getRunId();
        gov_for_transcript->writeAgentTranscript(transcript_entry.dump());
    }

    return NaabVal::makeDict(std::move(result));

    } catch (const governance::GovernanceHardError& e) {
        write_error_transcript(e.what());
        throw;
    } catch (const std::exception& e) {
        write_error_transcript(e.what());
        throw;
    } catch (...) {
        write_error_transcript("unknown error");
        throw;
    }
}

// ============================================================================
// agent.propose(handle, message [, n]) → {candidates, admissible_count, total}
//
// Generates N candidate responses for one PROPOSED transition without
// committing anything to conversation state: no history append, no turn
// increment, no CDD mutation, no tool execution. Candidates carry a
// read-only admissibility evaluation; agent.commit() runs the full
// post-receive pipeline for the selected candidate exactly once.
// ============================================================================

// Read-only admissibility score for a candidate against the CURRENT DriftState
// snapshot. Blends live coherence with mandate-alignment and semantic-stability
// components when history exists. Never mutates drift state — N candidates
// must not pollute the per-turn CDD signal history.
static double evaluateCandidateScore(governance::GovernanceEngine* gov,
                                     int handle_id, const std::string& content) {
    if (!gov || !gov->isActive()) return 1.0;
    auto state = gov->getDriftState(handle_id);
    if (!state) return 1.0;
    double score = state->coherence_score;

    std::unordered_set<std::string> content_kw;
    extractKeywords(content, content_kw);
    if (content_kw.empty()) return score;

    double align_component = -1.0;
    if (!state->mandate_keywords.empty()) {
        size_t hits = 0;
        for (const auto& kw : content_kw)
            if (state->mandate_keywords.count(kw)) hits++;
        double ratio = static_cast<double>(hits) / content_kw.size();
        const auto& cdd = gov->getRules().context_drift;
        double floor = cdd.thresholds.mandate_alignment_min > 0.0 ? cdd.thresholds.mandate_alignment_min : 0.15;
        align_component = std::min(1.0, ratio / floor);
    }
    double stab_component = -1.0;
    if (!state->prev_response_keywords.empty()) {
        size_t inter = 0;
        for (const auto& kw : content_kw)
            if (state->prev_response_keywords.count(kw)) inter++;
        size_t uni = content_kw.size() + state->prev_response_keywords.size() - inter;
        double jaccard = uni > 0 ? static_cast<double>(inter) / uni : 0.0;
        const auto& cdd = gov->getRules().context_drift;
        double floor = cdd.thresholds.semantic_stability_min_overlap > 0.0
            ? cdd.thresholds.semantic_stability_min_overlap : 0.25;
        stab_component = std::min(1.0, jaccard / floor);
    }

    double weight_sum = 0.6, blended = 0.6;
    if (align_component >= 0.0) { blended += 0.25 * align_component; weight_sum += 0.25; }
    if (stab_component >= 0.0)  { blended += 0.15 * stab_component;  weight_sum += 0.15; }
    return score * (blended / weight_sum);
}

static NaabVal agentPropose(std::vector<NaabVal>& args) {
    if (args.size() < 2 || !args[0].isDict() || !args[1].isString()) {
        throw std::runtime_error(
            "Agent error: agent.propose requires a handle and message\n\n"
            "  Expected: agent.propose(handle, \"your message\" [, n])\n\n"
            "  Help:\n"
            "  - agent.propose generates candidate responses WITHOUT committing\n"
            "    them to the conversation; commit one with agent.commit(handle, proposal)\n");
    }

    auto [config_name, handle_id] = validateHandle(args[0]);
    auto& handle = args[0].asDict();
    std::string message = args[1].asString();

    const auto* config = findAgentConfig(config_name);
    if (!config) {
        throw std::runtime_error(
            "Agent error: Agent config no longer available\n\n"
            "  Help:\n"
            "  - The governance configuration may have changed\n");
    }

    // Fail-closed: propose is disabled unless explicitly configured
    if (config->propose_candidates_max <= 0) {
        throw std::runtime_error(fmt::format(
            "Agent error: agent.propose is not enabled for '{}'\n\n"
            "  Help:\n"
            "  - Set \"propose_candidates_max\" (> 0) in this agent's govern.json config\n"
            "  - Proposals are capped at that count per call\n",
            config_name));
    }

    int n = 1;
    if (args.size() >= 3 && args[2].isInt()) n = static_cast<int>(args[2].asInt());
    if (n < 1) n = 1;
    if (n > config->propose_candidates_max) n = config->propose_candidates_max;

    // Gap O: tools cannot propose on their own invoking handle
    if (t_in_tool_execution_for_handle == handle_id) {
        throw std::runtime_error(
            "Agent error: agent.propose denied — recursive call from tool execution\n\n"
            "  Help:\n"
            "  - A tool function cannot call agent.propose() on the agent that invoked it\n");
    }

    int current_turn = 0;
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto tracker_it = s_trackers.find(handle_id);
        if (tracker_it == s_trackers.end()) {
            throw std::runtime_error(
                "Agent error: Invalid or expired agent handle\n\n"
                "  Help:\n"
                "  - Use a handle returned by agent.create()\n");
        }
        auto& tracker = tracker_it->second;
        // A new propose supersedes any outstanding proposal set
        s_pending_proposals.erase(handle_id);
        // Committing would add a turn — enforce the limit up front
        if (tracker.turns >= config->max_turns) {
            throw std::runtime_error(fmt::format(
                "Agent error: Conversation exceeded max_turns limit ({})\n\n"
                "  Got: {} turns used\n",
                config->max_turns, tracker.turns));
        }
        int total_used = tracker.input_tokens + tracker.output_tokens;
        if (config->max_total_tokens > 0 && total_used >= config->max_total_tokens) {
            throw std::runtime_error(fmt::format(
                "Agent error: Token budget exhausted ({}/{} tokens used)\n\n"
                "  Help:\n"
                "  - Create a new agent handle for a fresh conversation\n",
                total_used, config->max_total_tokens));
        }
        current_turn = tracker.turns;
    }

    // Hard stop check
    if (s_dispatch.hard_stopped) {
        throw std::runtime_error(
            "Agent error: Hard stop active\n\n"
            "  Got: " + s_dispatch.stop_reason + "\n\n"
            "  Help:\n"
            "  - All agent API calls are blocked for the remainder of this run\n");
    }

    // Mid-run governance reload + config re-lookup (same as send)
    auto* gov_engine = governance::GovernanceEngine::getCurrent();
    if (gov_engine && gov_engine->isActive()) {
        gov_engine->reloadIfChanged();
        config = findAgentConfig(config_name);
        if (!config || config->propose_candidates_max <= 0) {
            throw std::runtime_error(
                "Agent error: agent.propose no longer available after governance reload\n\n"
                "  Help:\n"
                "  - The governance configuration was reloaded mid-run\n");
        }
        if (n > config->propose_candidates_max) n = config->propose_candidates_max;
    }

    // Fail-closed: propose has no challenge machinery, so when a step-up
    // challenge or lease renewal is due the transition must go through
    // agent.send() (which can run the challenge).
    if (gov_engine && gov_engine->isActive()) {
        const auto& cb = gov_engine->getRules().circuit_breaker;
        if (cb.step_up_enabled) {
            int required_level = (cb.step_up_at_level == "high") ? 2 : 1;
            bool lease_expired = false;
            {
                std::lock_guard<std::mutex> lock(s_agent_mutex);
                auto it = s_trackers.find(handle_id);
                if (it != s_trackers.end() && it->second.lease_expires_turn > 0)
                    lease_expired = (it->second.turns >= it->second.lease_expires_turn);
            }
            if (static_cast<int>(gov_engine->getGovernanceLevel()) >= required_level ||
                lease_expired) {
                throw std::runtime_error(
                    "Agent error: agent.propose denied — step-up challenge required\n\n"
                    "  Help:\n"
                    "  - Governance level or lease state requires re-authorization\n"
                    "  - Call agent.send() first so the step-up challenge can run\n");
            }
        }
    }

    // Pre-send gates: BSD event, prompt scans, admission, action matrix
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().behavioral_sequences.enabled) {
        gov_engine->setAgentContext(handle_id, current_turn, config_name);
        std::string bsd_block = gov_engine->emitEvent(
            governance::RuntimeEventType::AGENT_SEND,
            "agent.propose('" + config_name + "')", "", 0);
        if (!bsd_block.empty()) throw std::runtime_error(bsd_block);
    }
    if (gov_engine && gov_engine->isActive() && !message.empty()) {
        std::string secret_err = gov_engine->checkSecrets(message, 0);
        if (!secret_err.empty()) {
            throw std::runtime_error(fmt::format(
                "Agent error: Prompt for '{}' contains a potential secret\n\n"
                "  Help:\n"
                "  - Sanitize sensitive data before sending to external LLM APIs\n",
                config_name));
        }
        std::string pii_err = gov_engine->checkPii(message, 0);
        if (!pii_err.empty()) {
            throw std::runtime_error(fmt::format(
                "Agent error: Prompt for '{}' contains potential PII\n\n"
                "  Help:\n"
                "  - Remove or mask PII before sending to external LLM APIs\n",
                config_name));
        }
    }
    if (gov_engine && gov_engine->isActive()) {
        std::string admission = gov_engine->checkAdmission(config_name);
        if (!admission.empty()) throw std::runtime_error(admission);
        if (!config->allowed_actions.empty()) {
            bool allowed = false;
            for (const auto& a : config->allowed_actions)
                if (a == "AGENT_SEND") { allowed = true; break; }
            if (!allowed) {
                throw std::runtime_error(
                    "Agent error: Action matrix does not include AGENT_SEND\n\n"
                    "  Help:\n"
                    "  - Add AGENT_SEND to the agent's allowed_actions configuration\n");
            }
        }
        // Accounting: one logical transition is being attempted (N candidates
        // for the SAME transition count once toward autonomous exposure)
        std::string exposure_block = gov_engine->recordAutonomousAction(config_name);
        if (!exposure_block.empty()) throw std::runtime_error(exposure_block);
    }

    // Build messages array (full history + new user message; no tools sent)
    json messages_json = json::array();
    auto msgs_it = handle.find("messages");
    if (msgs_it == handle.end() || !msgs_it->second.isList()) {
        throw std::runtime_error(
            "Agent error: handle missing 'messages' list\n\n"
            "  Help:\n  - Use a handle returned by agent.create()\n");
    }
    for (auto& msg : msgs_it->second.asList()) {
        if (!msg.isDict()) continue;
        auto& msg_dict = msg.asDict();
        auto role_it = msg_dict.find("role");
        auto content_it = msg_dict.find("content");
        if (role_it == msg_dict.end() || !role_it->second.isString() ||
            content_it == msg_dict.end() || !content_it->second.isString()) continue;
        json msg_obj;
        msg_obj["role"] = role_it->second.asString();
        msg_obj["content"] = content_it->second.asString();
        messages_json.push_back(msg_obj);
    }
    json user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = message;
    messages_json.push_back(user_msg);
    std::string messages_str = messages_json.dump();

    // Key / model resolution (first alive key, primary model)
    std::vector<std::string> keys = config->api_key_envs;
    if (keys.empty()) keys.push_back(config->api_key_env);
    std::vector<std::string> models = config->model_chain;
    if (models.empty()) models.push_back(config->model);

    const auto& oac = gov_engine && gov_engine->isActive()
        ? gov_engine->getRules().circuit_breaker.output_admissibility
        : governance::CircuitBreakerConfig::OutputAdmissibilityConfig{};
    bool oa_active = gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().circuit_breaker.enabled && oac.enabled;
    bool pulse_impaired = gov_engine && gov_engine->isActive() &&
        gov_engine->getPulseVerdict() == governance::PulseVerdict::IMPAIRED;

    bool transcript_active = gov_engine && gov_engine->isActive() &&
        gov_engine->isTranscriptAgent(config_name);

    ensureProcessSecret();
    governance::AgentConfig call_config = *config;
    call_config.model = models[0];

    std::vector<PendingProposal> pending;
    std::vector<NaabVal> candidate_dicts;
    int admissible_count = 0;
    int total_prop_tokens_in = 0, total_prop_tokens_out = 0;

    for (int i = 0; i < n; i++) {
        // Pick first alive resolvable key
        std::string api_key;
        for (const auto& k : keys) {
            {
                std::lock_guard<std::mutex> lock(s_dispatch.dead_keys_mutex);
                if (isKeyDead(k, config->retry.key_retry_after_seconds)) continue;
            }
            api_key = runtime::resolveApiKey(k);
            if (!api_key.empty()) break;
        }
        if (api_key.empty()) {
            throw std::runtime_error(
                "Agent error: API key not available\n\n"
                "  Help:\n"
                "  - Set the required API key environment variable\n");
        }

        runtime::AgentResponse resp;
        bool got_response = false;
        for (int attempt = 0; attempt < std::max(1, config->retry.max_attempts); attempt++) {
            s_dispatch.total_calls++;
            auto result = runtime::callAgentWithStatus(call_config, api_key, messages_str);
            if (result.response.success) {
                resp = result.response;
                got_response = true;
                break;
            }
            if (config->retry.backoff_ms > 0 && attempt + 1 < config->retry.max_attempts)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config->retry.backoff_ms));
        }

        std::unordered_map<std::string, NaabVal> cand;
        cand["index"] = NaabVal::makeInt(i);
        if (!got_response) {
            cand["success"] = NaabVal::makeBool(false);
            cand["content"] = NaabVal::makeString("");
            std::unordered_map<std::string, NaabVal> adm;
            adm["admissible"] = NaabVal::makeBool(false);
            adm["reason"] = NaabVal::makeString("api_error");
            cand["admissibility"] = NaabVal::makeDict(std::move(adm));
            candidate_dicts.push_back(NaabVal::makeDict(std::move(cand)));
            continue;
        }

        s_dispatch.total_tokens += resp.input_tokens + resp.output_tokens;
        total_prop_tokens_in += resp.input_tokens;
        total_prop_tokens_out += resp.output_tokens;

        std::string content = stripMarkdownFences(resp.content);

        // Stateless per-candidate governance: response scan + contract format.
        // Failures mark the candidate inadmissible instead of throwing —
        // candidates are for inspection; only commit executes consequences.
        bool scan_ok = true;
        std::string inadmissible_reason;
        if (gov_engine && gov_engine->isActive() && !content.empty()) {
            if (!gov_engine->checkSecrets(content, 0).empty() ||
                !gov_engine->checkPii(content, 0).empty()) {
                scan_ok = false;
                inadmissible_reason = "response_scan";
            }
        }
        if (scan_ok && config->response_format == "json" && !content.empty()) {
            try { (void)json::parse(content); }
            catch (const json::exception&) {
                scan_ok = false;
                inadmissible_reason = "contract";
            }
        }

        double score = evaluateCandidateScore(gov_engine, handle_id, content);
        bool admissible = scan_ok && !pulse_impaired &&
            (!oa_active || score >= oac.threshold);
        if (!admissible && inadmissible_reason.empty())
            inadmissible_reason = pulse_impaired ? "pulse_impaired" : "below_threshold";
        if (admissible) admissible_count++;

        // Anti-forge nonce ties the candidate to server-side state
        std::string content_hash = security::CryptoUtils::sha256(content);
        std::string nonce_input = "proposal:" + std::to_string(handle_id) + ":" +
            std::to_string(current_turn) + ":" + std::to_string(i) + ":" + content_hash;
        std::string nonce = security::CryptoUtils::hmacSha256(nonce_input, s_process_secret);

        PendingProposal pp;
        pp.nonce = nonce;
        pp.content_hash = content_hash;
        pp.content = content;
        pp.user_message = message;
        pp.model = models[0];
        pp.input_tokens = resp.input_tokens;
        pp.output_tokens = resp.output_tokens;
        pp.scan_ok = scan_ok;
        pp.score = score;
        pp.admissible = admissible;
        pending.push_back(std::move(pp));

        cand["success"] = NaabVal::makeBool(true);
        cand["content"] = NaabVal::makeString(content);
        cand["__proposal"] = NaabVal::makeString(nonce);
        std::unordered_map<std::string, NaabVal> adm;
        adm["admissible"] = NaabVal::makeBool(admissible);
        adm["score"] = NaabVal::makeDouble(score);
        adm["threshold"] = NaabVal::makeDouble(oa_active ? oac.threshold : 0.0);
        if (!admissible)
            adm["reason"] = NaabVal::makeString(inadmissible_reason);
        cand["admissibility"] = NaabVal::makeDict(std::move(adm));
        std::unordered_map<std::string, NaabVal> usage;
        usage["input_tokens"] = NaabVal::makeInt(resp.input_tokens);
        usage["output_tokens"] = NaabVal::makeInt(resp.output_tokens);
        cand["usage"] = NaabVal::makeDict(std::move(usage));
        std::unordered_map<std::string, NaabVal> trace;
        trace["model"] = NaabVal::makeString(models[0]);
        cand["trace"] = NaabVal::makeDict(std::move(trace));
        candidate_dicts.push_back(NaabVal::makeDict(std::move(cand)));
    }

    // Accounting commit: token spend is truth even though no turn advanced
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto& tracker = s_trackers[handle_id];
        tracker.input_tokens += total_prop_tokens_in;
        tracker.output_tokens += total_prop_tokens_out;
        handle["input_tokens"] = NaabVal::makeInt(tracker.input_tokens);
        handle["output_tokens"] = NaabVal::makeInt(tracker.output_tokens);
        s_pending_proposals[handle_id] = std::move(pending);
    }

    if (gov_engine && gov_engine->isActive()) {
        gov_engine->writeAgentTelemetry("AGENT_PROPOSE", {
            {"handle_id",        std::to_string(handle_id)},
            {"config_name",      config_name},
            {"turn",             std::to_string(current_turn)},
            {"candidates",       std::to_string(n)},
            {"admissible_count", std::to_string(admissible_count)},
            {"input_tokens",     std::to_string(total_prop_tokens_in)},
            {"output_tokens",    std::to_string(total_prop_tokens_out)}
        });
        if (gov_engine->getRules().taint_tracking.enabled)
            gov_engine->setLastReturnTainted(true, "agent.propose");
    }

    if (transcript_active && gov_engine) {
        nlohmann::json te;
        te["type"] = "agent_propose";
        te["handle_id"] = handle_id;
        te["agent"] = config_name;
        te["turn"] = current_turn;
        te["prompt"] = message;
        te["candidates"] = static_cast<int>(candidate_dicts.size());
        te["admissible_count"] = admissible_count;
        te["run_id"] = gov_engine->getRunId();
        gov_engine->writeAgentTranscript(te.dump());
    }

    std::unordered_map<std::string, NaabVal> result;
    result["success"] = NaabVal::makeBool(true);
    result["candidates"] = NaabVal::makeList(std::move(candidate_dicts));
    result["admissible_count"] = NaabVal::makeInt(admissible_count);
    result["total"] = NaabVal::makeInt(n);
    result["turn"] = NaabVal::makeInt(current_turn);
    return NaabVal::makeDict(std::move(result));
}

// ============================================================================
// agent.commit(handle, proposal) → response dict
//
// Commits ONE proposed candidate: runs the full post-receive pipeline
// (BSD response event, CDD, output-admissibility gate, split commit of
// history + turn) exactly once for the selected candidate.
// ============================================================================

static NaabVal agentCommit(std::vector<NaabVal>& args) {
    if (args.size() < 2 || !args[0].isDict() || !args[1].isDict()) {
        throw std::runtime_error(
            "Agent error: agent.commit requires a handle and a proposal\n\n"
            "  Expected: agent.commit(handle, proposal)\n\n"
            "  Help:\n"
            "  - Pass one candidate dict from agent.propose()'s candidates list\n");
    }

    auto [config_name, handle_id] = validateHandle(args[0]);
    auto& handle = args[0].asDict();
    auto& proposal = args[1].asDictConst();

    const auto* config = findAgentConfig(config_name);
    if (!config) {
        throw std::runtime_error(
            "Agent error: Agent config no longer available\n\n"
            "  Help:\n"
            "  - The governance configuration may have changed\n");
    }

    auto nonce_it = proposal.find("__proposal");
    if (nonce_it == proposal.end() || !nonce_it->second.isString()) {
        throw std::runtime_error(
            "Agent error: Not a proposal — missing proposal marker\n\n"
            "  Help:\n"
            "  - Pass a candidate dict returned by agent.propose()\n");
    }
    std::string nonce = nonce_it->second.asString();

    // Server-side verification: nonce membership + content authority.
    // The committed content comes from the registry, NOT the caller's dict —
    // mutating a proposal's content invalidates nothing but changes nothing.
    PendingProposal selected;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto pit = s_pending_proposals.find(handle_id);
        if (pit != s_pending_proposals.end()) {
            for (auto& pp : pit->second) {
                if (pp.nonce == nonce) { selected = pp; found = true; break; }
            }
        }
        if (found) s_pending_proposals.erase(handle_id);  // single use, no replay
    }
    if (!found) {
        throw std::runtime_error(
            "Agent error: Proposal not valid for this handle\n\n"
            "  Help:\n"
            "  - Proposals are single-use and expire when the conversation moves on\n"
            "  - Call agent.propose() again to generate fresh candidates\n");
    }
    if (!selected.scan_ok) {
        throw std::runtime_error(
            "Agent error: Proposal failed response scanning and cannot be committed\n\n"
            "  Help:\n"
            "  - Candidates that fail secret/PII/contract scans are inspection-only\n"
            "  - Select an admissible candidate instead\n");
    }

    // The user message that produced the candidates comes from the registry
    // (server-side authority), preserving user/assistant pairing in history.
    std::string user_message = selected.user_message;

    int current_turn = 0;
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto tracker_it = s_trackers.find(handle_id);
        if (tracker_it == s_trackers.end()) {
            throw std::runtime_error(
                "Agent error: Invalid or expired agent handle\n\n"
                "  Help:\n"
                "  - Use a handle returned by agent.create()\n");
        }
        if (tracker_it->second.turns >= config->max_turns) {
            throw std::runtime_error(fmt::format(
                "Agent error: Conversation exceeded max_turns limit ({})\n\n"
                "  Got: {} turns used\n",
                config->max_turns, tracker_it->second.turns));
        }
        current_turn = tracker_it->second.turns;
    }

    auto* gov_engine = governance::GovernanceEngine::getCurrent();

    // Post-receive pipeline: BSD response event + CDD at the pre-increment turn
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().behavioral_sequences.enabled) {
        gov_engine->setAgentContext(handle_id, current_turn, config_name);
        std::string cfp;
        std::unordered_set<std::string> response_keywords;
        if (!selected.content.empty()) {
            // Full-content fingerprint — same rationale as the send path: S21
            // detects byte-identical repetition, prefix hashes false-positive.
            cfp = security::CryptoUtils::sha256(selected.content);
            extractKeywords(selected.content, response_keywords);
        }
        int event_turn = gov_engine->getCurrentAgentTurn();
        gov_engine->emitEvent(governance::RuntimeEventType::AGENT_RESPONSE,
            "agent.response('" + config_name + "', tokens=" +
            std::to_string(selected.output_tokens) + ")", "", 0, cfp,
            selected.output_tokens, 0, response_keywords, selected.input_tokens);
        std::string drift_err = gov_engine->checkContextDrift(handle_id, event_turn, "");
        {
            auto drift_state = gov_engine->getDriftState(handle_id);
            if (drift_state) {
                // Build per-signal detail for commit-path CDD_TURN
                std::string c_fired, c_penalty;
                for (int i = 0; i < governance::NUM_CDD_SIGNALS; i++) {
                    if (drift_state->last_turn_fired[i] > 0) {
                        if (!c_fired.empty()) c_fired += ",";
                        c_fired += governance::ContextDriftAnalyzer::signalName(i);
                    }
                    if (drift_state->last_turn_penalties[i] > 0.0) {
                        if (!c_penalty.empty()) c_penalty += ",";
                        char pb[64];
                        snprintf(pb, sizeof(pb), "%s=%.4f",
                                 governance::ContextDriftAnalyzer::signalName(i),
                                 drift_state->last_turn_penalties[i]);
                        c_penalty += pb;
                    }
                }
                int c_level = static_cast<int>(gov_engine->getGovernanceLevel());
                const char* c_level_names[] = {"normal", "elevated", "high", "critical"};
                const char* c_level_str = (c_level >= 0 && c_level <= 3)
                    ? c_level_names[c_level] : "unknown";
                gov_engine->writeAgentTelemetry("CDD_TURN", {
                    {"handle_id",        std::to_string(handle_id)},
                    {"config_name",      config_name},
                    {"turn",             std::to_string(current_turn)},
                    {"coherence",        fmt::format("{:.4f}", drift_state->coherence_score)},
                    {"velocity",         fmt::format("{:.4f}", drift_state->coherence_velocity)},
                    {"acceleration",     fmt::format("{:.4f}", drift_state->coherence_acceleration)},
                    {"pressure",         fmt::format("{:.4f}", drift_state->last_pressure_score)},
                    {"signals_fired",    std::to_string(drift_state->signals_fired_this_turn)},
                    {"signals_detail",   c_fired},
                    {"penalties_detail", c_penalty},
                    {"response_repetition_count", std::to_string(drift_state->response_repetition_count)},
                    {"governance_level", c_level_str},
                    {"drift_detected",   drift_err.empty() ? "false" : "true"},
                    {"source",           "agent.commit"}
                });
            }
        }
        if (!drift_err.empty()) throw std::runtime_error(drift_err);
    }

    // Accounting commit: the turn advances now (tokens were counted at propose)
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto& tracker = s_trackers[handle_id];
        tracker.turns++;
        handle["turns"] = NaabVal::makeInt(tracker.turns);
        current_turn = tracker.turns;
    }

    // Output admissibility gate — full enforce-capable evaluation
    bool output_admissible = true;
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().circuit_breaker.enabled &&
        gov_engine->getRules().circuit_breaker.output_admissibility.enabled) {
        auto oa_result = gov_engine->checkOutputAdmissibility(
            handle_id, current_turn, config_name);
        if (!oa_result.admissible) {
            output_admissible = false;
            gov_engine->writeAgentTelemetry("OUTPUT_INADMISSIBLE", {
                {"handle_id",   std::to_string(handle_id)},
                {"config_name", config_name},
                {"turn",        std::to_string(current_turn)},
                {"coherence",   std::to_string(oa_result.coherence_score)},
                {"threshold",   std::to_string(oa_result.threshold)},
                {"action",      oa_result.action},
                {"source",      "agent.commit"}
            });
            if (oa_result.action == "attest") {
                gov_engine->emitOutputAdmissibilityAttestation(
                    config_name, current_turn,
                    oa_result.coherence_score, oa_result.threshold);
            }
        }
    }

    // Update quarantine streak counter (commit path)
    if (gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().circuit_breaker.output_admissibility.enabled) {
        int streak = gov_engine->updateQuarantineStreak(handle_id, !output_admissible);
        int max_streak = gov_engine->getRules().circuit_breaker.output_admissibility.max_quarantine_streak;
        if (max_streak > 0 && streak >= max_streak) {
            gov_engine->writeAgentTelemetry("QUARANTINE_STREAK_EXCEEDED", {
                {"handle_id",   std::to_string(handle_id)},
                {"config_name", config_name},
                {"turn",        std::to_string(current_turn)},
                {"streak",      std::to_string(streak)},
                {"max_allowed", std::to_string(max_streak)},
                {"source",      "agent.commit"}
            });
            throw governance::GovernanceHardError(fmt::format(
                "Agent exceeded maximum quarantine streak\n\n"
                "  Consecutive quarantined responses: {}\n"
                "  Maximum allowed: {}\n"
                "  Agent: {}\n  Turn: {}\n\n"
                "  The agent has produced too many inadmissible responses.\n",
                streak, max_streak, config_name, current_turn));
        }
    }

    // Split commit, state half — same rules as agent.send()
    bool commit_history = !selected.content.empty();
    if (commit_history && !output_admissible && gov_engine && gov_engine->isActive() &&
        gov_engine->getRules().circuit_breaker.output_admissibility
            .inadmissible_history == "exclude") {
        commit_history = false;
    }
    if (commit_history) {
        auto msgs_it = handle.find("messages");
        if (msgs_it != handle.end() && msgs_it->second.isList()) {
            auto& msg_list = msgs_it->second.asList();
            if (!user_message.empty()) {
                std::unordered_map<std::string, NaabVal> user_msg_val;
                user_msg_val["role"] = NaabVal::makeString("user");
                user_msg_val["content"] = NaabVal::makeString(user_message);
                msg_list.push_back(NaabVal::makeDict(std::move(user_msg_val)));
            }
            std::unordered_map<std::string, NaabVal> asst_msg_val;
            asst_msg_val["role"] = NaabVal::makeString("assistant");
            asst_msg_val["content"] = NaabVal::makeString(selected.content);
            msg_list.push_back(NaabVal::makeDict(std::move(asst_msg_val)));
        }
    }

    if (gov_engine && gov_engine->isActive()) {
        auto cp = gov_engine->getCheckpointData(handle_id, current_turn);
        gov_engine->emitAttestation("commit", config_name, current_turn, cp.pressure);
        gov_engine->writeAgentTelemetry("AGENT_PROPOSAL_COMMIT", {
            {"handle_id",    std::to_string(handle_id)},
            {"config_name",  config_name},
            {"turn",         std::to_string(current_turn)},
            {"content_hash", selected.content_hash},
            {"score",        fmt::format("{:.4f}", selected.score)},
            {"history_committed", commit_history ? "true" : "false"}
        });
        if (gov_engine->isTranscriptAgent(config_name)) {
            nlohmann::json te;
            te["type"] = "agent_commit";
            te["handle_id"] = handle_id;
            te["agent"] = config_name;
            te["turn"] = current_turn;
            te["content_hash"] = selected.content_hash;
            te["history_committed"] = commit_history;
            te["run_id"] = gov_engine->getRunId();
            gov_engine->writeAgentTranscript(te.dump());
        }
        if (gov_engine->getRules().taint_tracking.enabled)
            gov_engine->setLastReturnTainted(true, "agent.commit");
    }

    std::unordered_map<std::string, NaabVal> result;
    result["success"] = NaabVal::makeBool(true);
    result["content"] = NaabVal::makeString(selected.content);
    result["role"] = NaabVal::makeString("assistant");
    std::unordered_map<std::string, NaabVal> usage;
    usage["input_tokens"] = NaabVal::makeInt(selected.input_tokens);
    usage["output_tokens"] = NaabVal::makeInt(selected.output_tokens);
    result["usage"] = NaabVal::makeDict(std::move(usage));
    std::unordered_map<std::string, NaabVal> adm;
    adm["admissible"] = NaabVal::makeBool(output_admissible);
    adm["score"] = NaabVal::makeDouble(selected.score);
    result["admissibility"] = NaabVal::makeDict(std::move(adm));
    std::unordered_map<std::string, NaabVal> trace;
    trace["model"] = NaabVal::makeString(selected.model);
    trace["turn"] = NaabVal::makeInt(current_turn);
    trace["handle_id"] = NaabVal::makeInt(handle_id);
    result["trace"] = NaabVal::makeDict(std::move(trace));
    result["environment"] = buildEnvironmentDict(handle_id, config_name);
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

    // V-AG-004: Guard response content access
    if (!response.isDict()) return NaabVal::makeNull();
    auto& resp_dict = response.asDictConst();
    auto c_it = resp_dict.find("content");
    return (c_it != resp_dict.end()) ? c_it->second : NaabVal::makeNull();
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

    // V-AG-001: Read handle_id with type guard (same pattern as validateHandle)
    auto id_it = handle.find("id");
    if (id_it == handle.end() || !id_it->second.isInt()) {
        throw std::runtime_error(
            "Agent error: Invalid agent handle\n\n"
            "  Help:\n  - Use the handle returned by agent.create()\n");
    }
    int handle_id = id_it->second.asInt();
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
    result["truncation_count"] = NaabVal::makeInt(tracker.truncation_count);
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

    auto id_it = handle.find("id");
    auto cn_it = handle.find("config_name");
    if (id_it == handle.end() || !id_it->second.isInt() ||
        cn_it == handle.end() || !cn_it->second.isString()) {
        throw std::runtime_error(
            "Agent error: Invalid agent handle\n\n"
            "  Help:\n  - Use the handle returned by agent.create()\n");
    }
    int handle_id = id_it->second.asInt();
    std::string config_name = cn_it->second.asString();

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
        // Verify config_name matches what was bound at create time —
        // prevents swapping governance config on a live handle
        if (config_name != tracker_it->second.config_name) {
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

    // Validate all handles upfront (collect ids for per-handle depth tracking)
    std::vector<int> pipeline_handle_ids;
    pipeline_handle_ids.reserve(handles.size());
    for (size_t i = 0; i < handles.size(); i++) {
        pipeline_handle_ids.push_back(validateHandle(handles[i]).second);
    }

    // F3: Track pipeline depth with thread_local counter + RAII guard
    // Authoritative pipeline depth counter. Synced to governance engine via
    // setPipelineDepth() below (per-handle, so worker threads dispatched by a
    // stage still see the depth). RAII DepthGuard restores outer depth.
    static thread_local int t_pipeline_depth = 0;
    t_pipeline_depth++;
    struct DepthGuard {
        std::vector<int>* ids;
        ~DepthGuard() {
            t_pipeline_depth--;
            auto* ge = governance::GovernanceEngine::getCurrent();
            if (ge && ge->isActive() && ids) {
                for (int hid : *ids) ge->setPipelineDepth(hid, t_pipeline_depth);
            }
        }
    } depth_guard{&pipeline_handle_ids};

    // Set pipeline_depth on governance engine (per handle, read by checkAdmission)
    {
        auto* ge = governance::GovernanceEngine::getCurrent();
        if (ge && ge->isActive()) {
            for (int hid : pipeline_handle_ids) {
                ge->setPipelineDepth(hid, t_pipeline_depth);
            }

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
        } catch (const governance::GovernanceHardError&) {
            throw;  // HARD blocks propagate without coherence recovery
        } catch (...) {
            // Recover coherence for the failed stage to prevent floor-grinding
            auto* ge = governance::GovernanceEngine::getCurrent();
            if (ge && ge->isActive() && handles[i].isDict()) {
                auto& d = handles[i].asDict();
                auto hid = d.find("id");
                if (hid != d.end() && hid->second.isInt()) {
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
                                if (hid_it != next_dict.end() && hid_it->second.isInt()) {
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
                                if (hid_it != next_dict.end() && hid_it->second.isInt()) {
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
// agent.coherence(handle) — current coherence score (0.0-1.0)
// ============================================================================
static NaabVal agentCoherence(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isDict())
        throw std::runtime_error(
            "Agent error: agent.coherence requires an agent handle\n\n"
            "  Expected: agent.coherence(handle)\n\n"
            "  Help:\n"
            "  - Returns the current coherence score (0.0-1.0) for the agent\n"
            "  - Returns 1.0 if no drift state exists yet\n");
    auto [config_name, handle_id] = validateHandle(args[0]);
    auto* ge = governance::GovernanceEngine::getCurrent();
    if (ge && ge->isActive()) {
        auto drift_opt = ge->getDriftState(handle_id);
        if (drift_opt)
            return NaabVal::makeDouble(drift_opt->coherence_score);
    }
    return NaabVal::makeDouble(1.0);
}

// ============================================================================
// agent.reset(handle) — reset conversation and coherence state
// ============================================================================
static NaabVal agentReset(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isDict())
        throw std::runtime_error(
            "Agent error: agent.reset requires an agent handle\n\n"
            "  Expected: agent.reset(handle)\n\n"
            "  Help:\n"
            "  - Resets conversation history and coherence state\n"
            "  - The agent keeps its config but starts a fresh conversation\n");

    auto [config_name, handle_id] = validateHandle(args[0]);
    auto& handle = args[0].asDict();

    // Capture pre-reset state for telemetry
    int turn_at_reset = 0;
    double coherence_at_reset = 1.0;

    auto* gov_engine = governance::GovernanceEngine::getCurrent();
    if (gov_engine && gov_engine->isActive()) {
        auto drift_opt = gov_engine->getDriftState(handle_id);
        if (drift_opt)
            coherence_at_reset = drift_opt->coherence_score;
    }

    const auto* config = findAgentConfig(config_name);

    // Reset AgentTracker (preserve nonce, config_name, key_offset)
    // LOCK ORDERING: acquire s_agent_mutex first, release, THEN call
    // resetDriftState() which acquires ContextDriftAnalyzer::mutex_.
    {
        std::lock_guard<std::mutex> lock(s_agent_mutex);
        auto it = s_trackers.find(handle_id);
        if (it != s_trackers.end()) {
            auto& t = it->second;
            turn_at_reset = t.turns;
            t.turns = 0;
            t.input_tokens = 0;
            t.output_tokens = 0;
            t.truncation_count = 0;
            t.last_reinforcement_turn = -100;
            t.last_correction_turn = -100;
            t.last_challenge_turn = -100;
            t.challenges_passed = 0;
            t.challenges_failed = 0;
            t.tool_calls_total = 0;
            t.tool_calls_blocked = 0;
            t.tool_total_latency_ms = 0;
            if (config && config->standing_lease_turns > 0) {
                t.lease_granted_turn = 0;
                t.lease_expires_turn = config->standing_lease_turns;
            }
            if (config && config->standing_lease_seconds > 0) {
                t.lease_granted_time = std::chrono::steady_clock::now();
            }
        }
        s_pending_proposals.erase(handle_id);
    }
    // s_agent_mutex released — safe to call into governance engine

    // Reset DriftState (fresh coherence=1.0, all counters zero)
    if (gov_engine && gov_engine->isActive()) {
        gov_engine->resetDriftState(handle_id);

        // Re-initialize mandate keywords from system_prompt
        if (config && !config->system_prompt.empty() &&
            (effectiveSignal(config, gov_engine->getRules().context_drift.signals.mandate_alignment, "mandate_alignment") ||
             effectiveSignal(config, gov_engine->getRules().context_drift.signals.prompt_compliance, "prompt_compliance"))) {
            std::unordered_set<std::string> mandate_kw;
            extractKeywords(config->system_prompt, mandate_kw);
            if (!mandate_kw.empty()) {
                gov_engine->initializeMandateKeywords(handle_id, mandate_kw);
            }
        }
    }

    // Reset handle dict fields
    handle["messages"] = NaabVal::makeList({});
    handle["turns"] = NaabVal::makeInt(0);
    handle["input_tokens"] = NaabVal::makeInt(0);
    handle["output_tokens"] = NaabVal::makeInt(0);
    handle["environment"] = buildEnvironmentDict(handle_id, config_name);

    // Telemetry
    if (gov_engine && gov_engine->isActive()) {
        gov_engine->writeAgentTelemetry("AGENT_RESET", {
            {"handle_id",         std::to_string(handle_id)},
            {"config_name",       config_name},
            {"turn_at_reset",     std::to_string(turn_at_reset)},
            {"coherence_at_reset", fmt::format("{:.4f}", coherence_at_reset)},
        });
    }

    // Transcript
    if (gov_engine && gov_engine->isActive() && gov_engine->isTranscriptAgent(config_name)) {
        nlohmann::json te;
        te["type"] = "agent_reset";
        te["handle_id"] = handle_id;
        te["agent"] = config_name;
        te["turn_at_reset"] = turn_at_reset;
        te["coherence_at_reset"] = coherence_at_reset;
        auto now = std::chrono::system_clock::now();
        auto t_val = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &t_val);
#else
        localtime_r(&t_val, &tm_buf);
#endif
        char ts_buf[32];
        std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
        te["timestamp"] = std::string(ts_buf);
        te["run_id"] = gov_engine->getRunId();
        gov_engine->writeAgentTranscript(te.dump());
    }

    return args[0];
}

// ============================================================================
// Module Interface
// ============================================================================

bool AgentModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "create", "send", "run", "messages", "usage",
        "batch", "fan_out", "pipeline", "check",
        "key_health", "dispatch_status", "environment",
        "register_tool", "extract_code", "propose", "commit",
        "coherence", "reset"
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
    if (function_name == "propose") return agentPropose(args);
    if (function_name == "commit") return agentCommit(args);
    if (function_name == "coherence") return agentCoherence(args);
    if (function_name == "reset") return agentReset(args);

    throw std::runtime_error(fmt::format(
        "Agent error: Unknown function 'agent.{}'\n\n"
        "  Available functions: create, send, run, messages, usage, batch, fan_out, pipeline, check, key_health, dispatch_status, environment, register_tool, propose, commit, coherence, reset\n\n"
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
        "  - agent.register_tool(name, fn, schema) — register function as LLM-callable tool\n"
        "  - agent.coherence(handle) — current coherence score (0.0-1.0)\n"
        "  - agent.reset(handle) — reset conversation and coherence state\n",
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
