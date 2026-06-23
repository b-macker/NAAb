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
    CHECK_FAILED,       // governance check failed
    TOOL_CALL,          // LLM requested tool invocation
    TOOL_RESULT,        // tool executed, result returned
    TOOL_ERROR,         // tool invocation failed
    TOOL_BLOCKED,       // tool call blocked by governance
    CODEGEN_EXEC,       // codegen.run() dynamic code execution
    CODEGEN_BLOCKED,    // codegen.run() blocked by governance
    PULSE_DEGRADED,     // governance pulse entered DEGRADED state
    PULSE_IMPAIRED,     // governance pulse entered IMPAIRED state
    REFUSAL_ATTESTATION // governance enforcement refused an action
};

// --- Single Runtime Event ---
struct RuntimeEvent {
    RuntimeEventType type;
    std::string detail;         // e.g., "env.get('AWS_KEY')", "file.read('/etc/shadow')"
    std::string file;           // source file where event occurred
    int line = 0;               // source line
    int turn = 0;               // agent turn (0 if not in agent context)
    int agent_handle = 0;       // agent handle ID (0 if not in agent context)
    std::string agent_config;   // agent config name ("risk_assessor", etc.)
    size_t sequence_id = 0;     // global monotonic counter
    std::chrono::steady_clock::time_point timestamp;
    std::string content_fingerprint;  // hash of response content (agent events only)
    int output_tokens = 0;            // total output tokens (agent events only)
    int thinking_tokens = 0;          // thinking tokens consumed (agent events only)
    std::unordered_set<std::string> content_keywords;  // keywords from response (agent events only)
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
    std::deque<std::unordered_set<std::string>> per_turn_types;  // action types per turn (sliding window)
    int vocabulary_contraction_count = 0;  // narrowing action diversity
    double initial_entropy = -1.0;        // F2: baseline Shannon entropy for contraction detection
    std::unordered_set<std::string> prev_turn_blocked_caps;  // capabilities blocked in previous turn

    // Coherence dynamics (velocity/acceleration)
    std::deque<double> coherence_history;      // recent coherence values for derivative computation
    double coherence_velocity = 0.0;           // d(coherence)/d(turn)
    double coherence_acceleration = 0.0;       // d²(coherence)/d(turn)²

    // Rate normalization
    int turns_analyzed = 0;

    // Pipeline pressure inheritance
    double inherited_pressure = 0.0;  // pressure inherited from prior pipeline stage
    int pipeline_depth = 0;           // F3: current nesting depth in pipeline chain

    // F9/F18: Capability tracking (granted vs exercised)
    std::unordered_set<std::string> granted_capabilities;   // capabilities granted to this agent
    std::unordered_set<std::string> exercised_capabilities; // capabilities actually used
    int first_event_turn = -1;                              // turn of first event

    // F19: Semantic stability (keyword overlap between consecutive responses)
    std::unordered_set<std::string> prev_response_keywords;
    int semantic_stability_count = 0;     // turns where topic shifted significantly

    // Mandate alignment — continuous system_prompt keyword adherence
    std::unordered_set<std::string> mandate_keywords;  // extracted from system_prompt
    std::deque<double> mandate_alignment_history;       // rolling window of alignment scores
    int mandate_drift_count = 0;                        // turns where rolling mean dropped below threshold

    // Temporal trust decay — trust erodes when not actively reinforced
    std::chrono::steady_clock::time_point last_activity_time = std::chrono::steady_clock::now();

    // Adaptive baseline — observe "normal" behavior before penalizing deviations
    bool baseline_complete = false;
    int baseline_turns_counted = 0;
    int baseline_completed_turn = -1;  // wall-clock turn when baseline finished (-1 = not yet)
    struct BaselineStats {
        double mean_failures = 0.0;
        double mean_circular = 0.0;
        double mean_scope_creep = 0.0;
        double mean_contradictions = 0.0;
        double stddev_failures = 0.0;
        // Running sums for incremental computation
        double sum_failures = 0.0;
        double sum_sq_failures = 0.0;
        double sum_circular = 0.0;
        double sum_scope_creep = 0.0;
        double sum_contradictions = 0.0;
    } baseline;

    // Reality checkpoint state
    double last_pressure_score = 0.0;
    int consecutive_high_pressure_turns = 0;
    int last_checkpoint_turn = -100;   // init negative for no initial cooldown
    int signals_fired_this_turn = 0;

    // Recovery tracking (Phase 4a)
    int last_recovery_turn = -1;              // turn of last coherence recovery (-1 = never)

    // Response quality tracking (Phase 1b/1c)
    int response_quality_count = 0;           // turns where content ratio was below threshold
    int thinking_collapse_count = 0;          // turns where thinking dropped >50% from baseline
    std::deque<int> thinking_history;          // rolling window of thinking token counts
    double thinking_baseline_mean = -1.0;     // established after baseline window completes
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

    // Re-bind config without clearing behavioral state (threshold-only reloads)
    void updateConfig(const BehavioralSequenceConfig& config);

    // Check if enabled
    bool isEnabled() const;

    // Telemetry: total events processed, evicted, and patterns matched
    size_t totalEventsProcessed() const;
    size_t totalEventsEvicted() const;
    size_t totalPatternsMatched() const;

    // Max partial progress across all active patterns (0.0–1.0)
    double getMaxPartialProgress() const;

private:
    const BehavioralSequenceConfig* config_ = nullptr;
    std::deque<RuntimeEvent> event_buffer_;
    size_t sequence_counter_ = 0;
    size_t evicted_event_count_ = 0;
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

    // Re-bind config without clearing drift state (threshold-only reloads)
    void updateConfig(const ContextDriftConfig& config);

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

    // Update checkpoint state fields on DriftState (thread-safe)
    void updateCheckpointState(int handle_id, double pressure, int consecutive, int checkpoint_turn);

    // Set inherited pressure from prior pipeline stage (thread-safe)
    void setInheritedPressure(int handle_id, double pressure);

    // F15: Recover coherence (e.g., at pipeline stage transitions)
    void resetCoherence(int handle_id, double amount);

    // Initialize mandate keywords from system_prompt (for mandate alignment signal)
    void initializeMandateKeywords(int handle_id, const std::unordered_set<std::string>& keywords);

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
