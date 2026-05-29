#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <optional>

namespace naab {
namespace governance {

// Forward declarations — full definitions in governance.h
struct BehavioralSequenceConfig;
struct ContextDriftConfig;
struct SequencePattern;
struct SequenceStep;
enum class EnforcementLevel;

// --- Runtime Event Types ---
enum class RuntimeEventType {
    ENV_READ,           // env.get("KEY")
    ENV_WRITE,          // env.set_var("KEY", val)
    FILE_READ,          // file.read(path)
    FILE_WRITE,         // file.write(path, data)
    NET_CONNECT,        // http.get/post
    SHELL_EXEC,         // shell block execution
    AGENT_SEND,         // agent.send() call
    AGENT_RESPONSE,     // agent response received
    TAINT_VIOLATION,    // taint sink blocked
    TAINT_SANITIZED,    // value sanitized
    ENCODE,             // crypto.base64_encode, crypto.hex_encode
    DECODE,             // crypto.base64_decode
    PROCESS_EXEC,       // process.exec
    CONFIG_RELOAD,      // govern.json reloaded mid-run
    CHECK_FAILED        // governance check failed
};

// --- Single Runtime Event ---
struct RuntimeEvent {
    RuntimeEventType type;
    std::string detail;         // e.g., "env.get('AWS_KEY')", "file.read('/etc/shadow')"
    std::string file;           // source file where event occurred
    int line = 0;               // source line
    int turn = 0;               // agent turn (0 if not in agent context)
    size_t sequence_id = 0;     // global monotonic counter
    std::chrono::steady_clock::time_point timestamp;
};

// --- FSM State Per Pattern ---
struct PatternMatchState {
    size_t current_step = 0;
    size_t last_match_seq_id = 0;
    int last_match_turn = 0;
    std::chrono::steady_clock::time_point last_match_time;
    std::vector<RuntimeEvent> matched_events;  // one per completed step
};

// --- Sequence Match Result (returned by recordEvent) ---
struct SequenceMatchResult {
    std::string pattern_name;                    // empty = no match
    const SequencePattern* pattern = nullptr;    // valid only if pattern_name non-empty
    std::vector<RuntimeEvent> matched_events;    // copy of matched events
};

// --- Context Drift State (per agent handle) ---
struct DriftState {
    int handle_id = 0;
    double coherence_score = 1.0;         // starts at 1.0, decays
    int contradictions = 0;
    int repeated_failures = 0;
    int circular_action_count = 0;
    int scope_creep_count = 0;
    std::deque<std::string> turn_fingerprints;  // hash of actions per turn
    std::deque<std::string> recent_errors;      // last N error messages
    int last_checked_turn = 0;
    std::unordered_set<std::string> seen_event_types;  // historical unique type abbreviations
    std::unordered_set<std::string> prev_turn_blocked_caps;  // capabilities blocked in previous turn
};

// --- Event Ring Buffer + Sequence Pattern Matcher ---
class BehavioralSequenceDetector {
public:
    BehavioralSequenceDetector() = default;

    // Configure from parsed govern.json
    void configure(const BehavioralSequenceConfig& config);

    // Record a runtime event; returns match result (thread-safe, no TOCTOU)
    SequenceMatchResult recordEvent(const RuntimeEvent& event);

    // Check if adding this event would complete a pattern (read-only, no state mutation)
    SequenceMatchResult wouldMatch(const RuntimeEvent& event) const;

    // Get events from the current turn (for drift analysis)
    std::vector<RuntimeEvent> getEventsForTurn(int turn) const;

    // Reset all state
    void reset();

    // Check if enabled
    bool isEnabled() const;

    // Telemetry: total events processed and patterns matched
    size_t totalEventsProcessed() const;
    size_t totalPatternsMatched() const;

private:
    const BehavioralSequenceConfig* config_ = nullptr;
    std::deque<RuntimeEvent> event_buffer_;
    size_t sequence_counter_ = 0;
    size_t match_count_ = 0;
    std::unordered_map<std::string, PatternMatchState> pattern_states_;
    std::vector<SequencePattern> default_patterns_;  // built-in patterns when user provides none

    const std::vector<SequencePattern>& getActivePatterns() const;
    void buildDefaultPatterns();
    bool matchesStep(const RuntimeEvent& event, const SequenceStep& step) const;
    bool globMatch(const std::string& text, const std::string& pattern) const;
    std::string eventTypeToString(RuntimeEventType type) const;

    mutable std::mutex mutex_;
};

// --- Context Drift Analyzer ---
class ContextDriftAnalyzer {
public:
    ContextDriftAnalyzer() = default;

    void configure(const ContextDriftConfig& config);

    // Called after each agent turn; returns true if drift detected
    bool recordTurn(int handle_id, int turn_number,
                    const std::vector<RuntimeEvent>& turn_events,
                    const std::string& error_if_any);

    // Get current coherence score for an agent
    double getCoherence(int handle_id) const;

    // Get drift state for decision_trace (returns copy — safe across threads)
    std::optional<DriftState> getDriftState(int handle_id) const;

    bool isEnabled() const;

    // Telemetry: total turns analyzed
    size_t totalTurnsAnalyzed() const;

    void reset();

private:
    const ContextDriftConfig* config_ = nullptr;
    std::unordered_map<int, DriftState> drift_states_;
    size_t turns_analyzed_ = 0;
    mutable std::mutex mutex_;

    std::string computeFingerprint(const std::vector<RuntimeEvent>& events) const;
    bool isCircular(const DriftState& state, const std::string& fingerprint) const;
};

} // namespace governance
} // namespace naab
