#pragma once
#include <string>
#include <vector>
#include <array>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <optional>
#include <set>

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
    int input_tokens = 0;             // input/prompt tokens (agent events only)
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

// CDD signal indices for generic adaptive baseline
static constexpr int NUM_CDD_SIGNALS = 21;
enum CddSignalId : int {
    SIG_CIRCULAR = 0,              // S1
    SIG_REPEATED_FAILURES = 1,     // S2
    SIG_SCOPE_CREEP = 2,           // S3
    SIG_CONTRADICTIONS = 3,        // S4
    SIG_VOCAB_CONTRACTION = 4,     // S5
    SIG_COHERENCE_VELOCITY = 5,    // S6 — no cumulative counter, base_penalty only
    SIG_CAPABILITY_UNDERUTIL = 6,  // S7 — no cumulative counter, base_penalty only
    SIG_RESPONSE_QUALITY = 7,      // S8
    SIG_THINKING_COLLAPSE = 8,     // S9
    SIG_SEMANTIC_STABILITY = 9,    // S10
    SIG_MANDATE_ALIGNMENT = 10,    // S11
    SIG_CONTEXT_GROWTH = 11,       // S12
    SIG_INSTRUCTION_RECALL = 12,   // S13
    SIG_PLAN_DRIFT = 13,           // S14
    SIG_ENTITY_CONSISTENCY = 14,   // S15
    SIG_INSTRUCTION_CONFLICT = 15, // S16
    SIG_PERSONA_FINGERPRINT = 16,  // S17
    SIG_TOOL_CHAIN_INTEGRITY = 17, // S18
    SIG_CLAIM_RESULT = 18,         // S19
    SIG_PROMPT_COMPLIANCE = 19,    // S20 — off-topic prompt compliance
    SIG_RESPONSE_REPETITION = 20   // S21 — verbatim response repetition
};

struct SignalBaseline {
    int snapshot = 0;       // cumulative counter at baseline completion
    double mean = 0.0;      // per-turn firing rate during baseline
    double stddev = 0.0;
    double sum = 0.0;       // running sum during baseline
    double sum_sq = 0.0;    // running sum-of-squares
};

// --- Context Drift State (per agent handle) ---
struct DriftState {
    int handle_id = 0;
    std::string config_name;              // govern.json agent name this handle was created from
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
    std::deque<double> semantic_stability_history;  // rolling window of Jaccard similarity scores
    int semantic_stability_count = 0;     // turns where topic shifted significantly

    // Mandate alignment — continuous system_prompt keyword adherence
    std::unordered_set<std::string> mandate_keywords;  // extracted from system_prompt
    std::deque<double> mandate_alignment_history;       // rolling window of alignment scores
    int mandate_drift_count = 0;                        // turns where rolling mean dropped below threshold

    // Instruction recall — tracks mid-conversation user instruction keywords
    std::unordered_set<std::string> instruction_keywords;  // accumulated from user prompts
    int instruction_recall_count = 0;                      // turns where recall dropped below threshold

    // Plan drift — tracks execution against stated plan steps
    bool plan_extracted = false;                                          // whether plan has been parsed
    std::vector<std::unordered_set<std::string>> plan_step_keywords;     // keywords per plan step
    int plan_last_step_matched = -1;                                     // index of last matched step
    int plan_drift_count = 0;                                            // turns with drift detected

    // Entity consistency — tracks entity-context associations across turns
    std::unordered_map<std::string, std::unordered_set<std::string>> entity_context;  // entity → context keywords
    int entity_consistency_count = 0;                                    // turns with entity contradiction

    // Instruction conflict — detect contradictory user instructions
    std::deque<std::unordered_set<std::string>> instruction_history;    // keyword sets per user prompt (sliding window)
    int instruction_conflict_count = 0;                                 // turns with conflicting instructions detected

    // Persona fingerprint — detect linguistic style shifts
    std::deque<int> response_keyword_counts;     // rolling window of keyword counts per response
    double persona_baseline_mean = -1.0;         // established after adaptive baseline completes
    double persona_baseline_stddev = 0.0;        // stddev for deviation detection
    int persona_drift_count = 0;                 // turns where style deviated significantly

    // Tool chain integrity — detect tool result misrepresentation
    std::unordered_map<std::string, std::unordered_set<std::string>> tool_result_keywords;  // tool_name → result keywords
    int tool_integrity_count = 0;                // turns where agent misrepresented tool results

    // Claim-result reconciliation — detect tool outcome misrepresentation
    // Tracks actual success/failure per tool, compares against agent claims.
    // Unlike tool_chain_integrity (keyword recall), this detects STATUS misrepresentation:
    // agent claims "success" when tool returned error, or vice versa.
    std::unordered_map<std::string, bool> tool_last_outcome;  // tool_name → last success/failure
    int claim_result_mismatch_count = 0;                      // turns where agent misrepresented tool outcome
    std::deque<double> claim_accuracy_history;                 // rolling window of per-turn accuracy (size 20)

    // Prompt compliance — detect off-topic prompt compliance
    std::unordered_set<std::string> turn_prompt_keywords;  // per-turn (set before CDD, cleared after)
    std::deque<double> prompt_alignment_history;            // rolling window of prompt-to-mandate overlap
    int prompt_compliance_count = 0;                        // turns where off-topic prompt was complied with

    // Response repetition — detect verbatim duplicate responses
    std::deque<std::string> response_fingerprints;  // recent content_fingerprint values
    int response_repetition_count = 0;              // turns with exact duplicate response

    // Escalation effectiveness tracking — measure whether level changes improve behavior
    int escalation_turn = -1;                      // turn when last escalation occurred (-1 = never)
    int escalation_from_level = 0;                 // governance level before escalation
    int escalation_to_level = 0;                   // governance level after escalation
    double escalation_coherence_at = 0.0;          // coherence snapshot at escalation moment
    double post_escalation_coherence_sum = 0.0;    // sum of coherence readings in window
    int post_escalation_turns_counted = 0;         // turns counted in effectiveness window

    // Temporal trust decay — trust erodes when not actively reinforced
    std::chrono::steady_clock::time_point last_activity_time = std::chrono::steady_clock::now();

    // Adaptive baseline — observe "normal" behavior before penalizing deviations
    bool baseline_complete = false;
    int baseline_turns_counted = 0;
    int baseline_completed_turn = -1;  // wall-clock turn when baseline finished (-1 = not yet)
    int post_baseline_checks = 0;     // gate-passing turns after baseline complete
    std::array<SignalBaseline, NUM_CDD_SIGNALS> signal_baselines = {};

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

    // Context growth tracking — detect prompt bloat
    int context_growth_count = 0;             // turns where input tokens exceeded baseline by factor
    std::deque<int> input_tokens_history;      // rolling window of input token counts
    double input_tokens_baseline_mean = -1.0;  // established after baseline window completes
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

    // Get minimum coherence across all tracked agents
    double getMinCoherence() const;

    // Get per-agent coherence map (config_name → coherence_score)
    std::unordered_map<std::string, double> getAllAgentCoherences() const;

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

    // Add instruction keywords from user prompts (for instruction recall signal)
    void addInstructionKeywords(int handle_id, const std::unordered_set<std::string>& keywords);

    // Extract plan steps from agent response text (for plan drift signal)
    // Only extracts on first call per handle; subsequent calls are no-ops.
    void extractPlanFromResponse(int handle_id, const std::string& response_text);

    // Record tool result keywords (for tool chain integrity signal)
    void recordToolResult(int handle_id, const std::string& tool_name,
                          const std::unordered_set<std::string>& result_keywords);

    // Record tool execution outcome for claim-result reconciliation
    void recordToolOutcome(int handle_id, const std::string& tool_name, bool success);

    // Set per-turn prompt keywords (for prompt compliance signal)
    void setTurnPromptKeywords(int handle_id, const std::unordered_set<std::string>& keywords);

    // Record governance level escalation for effectiveness tracking
    void recordEscalation(int handle_id, int from_level, int to_level);

    // Bind the govern.json agent name to a handle's drift state (for scoped
    // per-agent resets on config reload)
    void setAgentConfigName(int handle_id, const std::string& name);

    // Scoped rate-window reset after an operator config change: for handles
    // belonging to a changed agent, re-snapshot cumulative signal counters and
    // restart the post-baseline rate window. Learned baseline statistics
    // (mean/stddev/baseline_complete) and coherence are preserved, so repeated
    // reloads cannot buy a grace period — with post_baseline_checks == 0 the
    // adaptive path is skipped and the full base penalty applies.
    // new_system_prompts maps agent name → new system_prompt; when the mandate
    // changed, mandate keywords are re-derived against the new prompt.
    void onAgentConfigChanged(
        const std::set<std::string>& changed_agents,
        const std::unordered_map<std::string, std::string>& new_system_prompts);

    void reset();

private:
    const ContextDriftConfig* config_ = nullptr;
    std::unordered_map<int, DriftState> drift_states_;
    size_t turns_analyzed_ = 0;
    mutable std::mutex mutex_;

    std::string computeFingerprint(const std::vector<RuntimeEvent>& events) const;
    bool isCircular(const DriftState& state, const std::string& fingerprint) const;
    static void snapshotSignalCounters(DriftState& state);
};

} // namespace governance
} // namespace naab
