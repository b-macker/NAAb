#include "naab/governance.h"
#include "naab/behavioral_sequence.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace naab {
namespace governance {

// ============================================================================
// Helper functions
// ============================================================================

// Normalize event type names from UPPERCASE_UNDERSCORE to lowercase.dot format
// Examples: "AGENT_SEND" → "agent.send", "FILE_WRITE" → "file.write"
static std::string normalizeEventTypeName(const std::string& raw) {
    if (raw.empty()) return raw;
    std::string s = raw;
    // Convert to lowercase
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    // Replace underscores with dots
    std::replace(s.begin(), s.end(), '_', '.');
    return s;
}

// ============================================================================
// BehavioralSequenceDetector
// ============================================================================

void BehavioralSequenceDetector::configure(const BehavioralSequenceConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = &config;
    event_buffer_.clear();
    sequence_counter_ = 0;
    pattern_states_.clear();
    // Build default patterns if user provides none
    if (config.patterns.empty()) {
        buildDefaultPatterns();
    }
}

void BehavioralSequenceDetector::updateConfig(const BehavioralSequenceConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = &config;
    if (config.patterns.empty()) {
        buildDefaultPatterns();
    }
    // Preserve: event_buffer_, sequence_counter_, evicted_event_count_,
    //           match_count_, pattern_states_
}

void BehavioralSequenceDetector::buildDefaultPatterns() {
    default_patterns_.clear();

    // Helper to create a step matching event types with optional detail globs
    auto makeStep = [](std::vector<std::string> types, std::vector<std::string> globs = {}) {
        SequenceStep step;
        step.match_any = std::move(types);
        step.detail_globs = std::move(globs);
        return step;
    };

    // BSD max_gap rationale: gap = max intervening events between consecutive steps.
    // Tight gaps (2-5) for direct cause-effect patterns where the steps should occur
    // close together (exfiltration, escape). Wider gaps (10-20) for multi-phase attack
    // patterns where an attacker may interleave benign actions between steps.
    // Overridable per-pattern via govern.json "behavioral_sequences.patterns".

    // 1. Credential harvesting: ENV_READ(secret/key/token) → NET_CONNECT
    //    gap=5: direct exfil — secret read should be followed closely by network send
    {
        SequencePattern p;
        p.name = "credential_harvesting";
        p.steps.push_back(makeStep({"ENV_READ"}, {"*secret*|*key*|*token*|*password*|*KEY*|*SECRET*|*TOKEN*"}));
        p.steps.push_back(makeStep({"NET_CONNECT"}));
        p.max_gap = 5;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 2. Sandbox probe-and-escape: FILE_READ(system file) → SHELL_EXEC
    //    gap=10: probe then exploit — attacker may inspect output before acting
    {
        SequencePattern p;
        p.name = "sandbox_probe_escape";
        p.steps.push_back(makeStep({"FILE_READ"}, {"*/etc/passwd*|*/proc/*|*/etc/shadow*"}));
        p.steps.push_back(makeStep({"SHELL_EXEC", "PROCESS_EXEC"}));
        p.max_gap = 10;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 3. Config tampering: FILE_READ(govern.json) → FILE_WRITE(govern.json)
    //    gap=5: tight — read-modify-write on the same file is typically immediate
    {
        SequencePattern p;
        p.name = "config_tampering";
        p.steps.push_back(makeStep({"FILE_READ"}, {"*govern.json*"}));
        p.steps.push_back(makeStep({"FILE_WRITE"}, {"*govern.json*"}));
        p.max_gap = 5;
        p.level = EnforcementLevel::HARD;
        default_patterns_.push_back(std::move(p));
    }

    // 4. Progressive escalation: 3 governance blocks then shell exec
    //    gap=15: wide — attacker may try many approaches between blocks
    {
        SequencePattern p;
        p.name = "progressive_escalation";
        p.steps.push_back(makeStep({"CHECK_FAILED"}));
        p.steps.push_back(makeStep({"CHECK_FAILED"}));
        p.steps.push_back(makeStep({"CHECK_FAILED"}));
        p.steps.push_back(makeStep({"SHELL_EXEC", "PROCESS_EXEC"}));
        p.max_gap = 15;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 5. Data staging: multiple FILE_READs → FILE_WRITE(/tmp) → NET_CONNECT
    //    gap=20: widest — multi-phase exfil with data collection, staging, and send
    {
        SequencePattern p;
        p.name = "data_staging";
        p.steps.push_back(makeStep({"FILE_READ"}));
        p.steps.push_back(makeStep({"FILE_READ"}));
        p.steps.push_back(makeStep({"FILE_WRITE"}, {"*/tmp/*|*/temp/*"}));
        p.steps.push_back(makeStep({"NET_CONNECT"}));
        p.max_gap = 20;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 6. Taint bypass via agent: repeated taint violations then agent send (data laundering)
    //    gap=10: moderate — violations may accumulate over several turns before send
    {
        SequencePattern p;
        p.name = "taint_bypass_via_agent";
        p.steps.push_back(makeStep({"TAINT_VIOLATION"}));
        p.steps.push_back(makeStep({"TAINT_VIOLATION"}));
        p.steps.push_back(makeStep({"AGENT_SEND"}));
        p.max_gap = 10;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 7. Repeated taint violations: sustained taint hygiene failure
    //    gap=15: wide — pattern tracks persistent issues across multiple turns
    {
        SequencePattern p;
        p.name = "repeated_taint_violations";
        p.steps.push_back(makeStep({"TAINT_VIOLATION"}));
        p.steps.push_back(makeStep({"TAINT_VIOLATION"}));
        p.steps.push_back(makeStep({"TAINT_VIOLATION"}));
        p.max_gap = 15;
        p.level = EnforcementLevel::ADVISORY;
        default_patterns_.push_back(std::move(p));
    }

    // 8. Tool data exfiltration: tool reads data → network connect
    //    gap=5: tight — direct read-to-send pipeline
    {
        SequencePattern p;
        p.name = "tool_data_exfil";
        p.steps.push_back(makeStep({"TOOL_CALL"}, {"*read*|*get*|*fetch*"}));
        p.steps.push_back(makeStep({"NET_CONNECT"}));
        p.max_gap = 5;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 9. Tool env harvest: env read → tool result → network
    //    gap=10: moderate — three-stage pattern with processing between stages
    {
        SequencePattern p;
        p.name = "tool_env_harvest";
        p.steps.push_back(makeStep({"ENV_READ"}));
        p.steps.push_back(makeStep({"TOOL_RESULT"}));
        p.steps.push_back(makeStep({"NET_CONNECT"}));
        p.max_gap = 10;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 10. Tool shell escape: tool execution leads to shell command
    //     gap=3: very tight — tool-to-shell should be nearly immediate
    {
        SequencePattern p;
        p.name = "tool_shell_escape";
        p.steps.push_back(makeStep({"TOOL_CALL"}));
        p.steps.push_back(makeStep({"SHELL_EXEC", "PROCESS_EXEC"}));
        p.max_gap = 3;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 11. Tool rapid fire: burst of tool calls (possible enumeration)
    //     gap=3: very tight — burst detection requires consecutive calls
    {
        SequencePattern p;
        p.name = "tool_rapid_fire";
        p.steps.push_back(makeStep({"TOOL_CALL"}));
        p.steps.push_back(makeStep({"TOOL_CALL"}));
        p.steps.push_back(makeStep({"TOOL_CALL"}));
        p.steps.push_back(makeStep({"TOOL_CALL"}));
        p.steps.push_back(makeStep({"TOOL_CALL"}));
        p.max_gap = 3;
        p.level = EnforcementLevel::ADVISORY;
        default_patterns_.push_back(std::move(p));
    }

    // 12. Codegen exfiltration: env read → codegen exec (exfil via dynamic code)
    //     gap=5: tight — env read feeding into dynamic execution
    {
        SequencePattern p;
        p.name = "codegen_exfil";
        p.steps.push_back(makeStep({"env.get"}));
        p.steps.push_back(makeStep({"codegen_exec"}));
        p.max_gap = 5;
        p.level = EnforcementLevel::SOFT;
        default_patterns_.push_back(std::move(p));
    }

    // 13. Codegen rapid fire: burst of dynamic code executions (enumeration/probing)
    //     gap=2: tightest — consecutive codegen calls with minimal intervening events
    {
        SequencePattern p;
        p.name = "codegen_rapid_fire";
        p.steps.push_back(makeStep({"codegen_exec"}));
        p.steps.push_back(makeStep({"codegen_exec"}));
        p.steps.push_back(makeStep({"codegen_exec"}));
        p.max_gap = 2;
        p.level = EnforcementLevel::ADVISORY;
        default_patterns_.push_back(std::move(p));
    }

    // 14. Tool result → codegen: tool output flows into generated code
    //     gap=3: tight — tool result should feed directly into codegen
    {
        SequencePattern p;
        p.name = "codegen_after_tool";
        p.steps.push_back(makeStep({"tool_result"}));
        p.steps.push_back(makeStep({"codegen_exec"}));
        p.max_gap = 3;
        p.level = EnforcementLevel::ADVISORY;
        default_patterns_.push_back(std::move(p));
    }

    // 15. Cross-agent file relay: agent A writes file, agent B reads file
    //     gap=5: moderate — relay may span a few intermediate events
    //     Only matches when both events originate from agent contexts
    {
        SequencePattern p;
        p.name = "cross_agent_file_relay";
        p.steps.push_back(makeStep({"FILE_WRITE"}));
        p.steps.push_back(makeStep({"FILE_READ"}));
        p.cross_agent = true;
        p.max_gap = 5;
        p.level = EnforcementLevel::ADVISORY;
        p.rationale = "Cross-agent file-based data transfer — verify content was validated";
        default_patterns_.push_back(std::move(p));
    }

    // 16. Cross-agent tool chain: agent A tool call leads to agent B tool call
    //     gap=10: wider — delegation may cross send/receive boundaries
    //     Only matches when both events originate from agent contexts
    {
        SequencePattern p;
        p.name = "cross_agent_tool_chain";
        p.steps.push_back(makeStep({"TOOL_CALL"}));
        p.steps.push_back(makeStep({"AGENT_SEND"}));
        p.steps.push_back(makeStep({"TOOL_CALL"}));
        p.cross_agent = true;
        p.max_gap = 10;
        p.level = EnforcementLevel::ADVISORY;
        p.rationale = "Cross-agent tool delegation chain — verify authorization propagation";
        default_patterns_.push_back(std::move(p));
    }
}

const std::vector<SequencePattern>& BehavioralSequenceDetector::getActivePatterns() const {
    if (config_ && !config_->patterns.empty()) return config_->patterns;
    return default_patterns_;
}

bool BehavioralSequenceDetector::isEnabled() const {
    return config_ && config_->enabled;
}

SequenceMatchResult BehavioralSequenceDetector::recordEvent(const RuntimeEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_ || !config_->enabled) return {};
    const auto& patterns = getActivePatterns();
    if (patterns.empty()) return {};

    // Assign sequence ID and add to buffer
    RuntimeEvent ev = event;
    ev.sequence_id = ++sequence_counter_;
    if (ev.timestamp == std::chrono::steady_clock::time_point{}) {
        ev.timestamp = std::chrono::steady_clock::now();
    }

    event_buffer_.push_back(ev);
    while (event_buffer_.size() > config_->window_size) {
        event_buffer_.pop_front();
        evicted_event_count_++;
    }

    // Check each pattern's FSM
    for (const auto& pattern : patterns) {
        auto& state = pattern_states_[pattern.name];

        // Check decay: reset if too old
        if (state.current_step > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                ev.timestamp - state.last_match_time).count();
            bool decayed = (elapsed > pattern.decay_seconds);
            if (!decayed && ev.turn > 0 && state.last_match_turn > 0) {
                decayed = (ev.turn - state.last_match_turn) > pattern.decay_turns;
            }
            if (decayed) {
                state.current_step = 0;
                state.matched_events.clear();
            }
        }

        // Check gap: reset if too many events between matches
        if (state.current_step > 0) {
            size_t gap = ev.sequence_id - state.last_match_seq_id - 1;
            if (gap > static_cast<size_t>(pattern.max_gap)) {
                state.current_step = 0;
                state.matched_events.clear();
            }
        }

        // Try to match current step
        if (state.current_step < pattern.steps.size()) {
            if (matchesStep(ev, pattern.steps[state.current_step])) {
                state.matched_events.push_back(ev);
                state.last_match_seq_id = ev.sequence_id;
                state.last_match_turn = ev.turn;
                state.last_match_time = ev.timestamp;
                state.current_step++;

                // Pattern complete?
                if (state.current_step >= pattern.steps.size()) {
                    // Cross-agent check: reject if all events came from same agent
                    if (pattern.cross_agent) {
                        std::unordered_set<std::string> agents;
                        for (const auto& me : state.matched_events) {
                            if (!me.agent_config.empty()) agents.insert(me.agent_config);
                        }
                        if (agents.size() < 2) {
                            state.current_step = 0;
                            state.matched_events.clear();
                            continue;
                        }
                    }
                    SequenceMatchResult result;
                    result.pattern_name = pattern.name;
                    result.pattern = &pattern;
                    result.matched_events = state.matched_events;
                    state.current_step = 0;
                    state.matched_events.clear();
                    match_count_++;
                    return result;
                }
            }
        }

        // If we reset above due to gap/decay, re-check step 0
        if (state.current_step == 0 && state.matched_events.empty()) {
            if (!pattern.steps.empty() && matchesStep(ev, pattern.steps[0])) {
                state.matched_events.push_back(ev);
                state.last_match_seq_id = ev.sequence_id;
                state.last_match_turn = ev.turn;
                state.last_match_time = ev.timestamp;
                state.current_step = 1;

                if (state.current_step >= pattern.steps.size()) {
                    // Cross-agent check: reject if all events came from same agent
                    if (pattern.cross_agent) {
                        std::unordered_set<std::string> agents;
                        for (const auto& me : state.matched_events) {
                            if (!me.agent_config.empty()) agents.insert(me.agent_config);
                        }
                        if (agents.size() < 2) {
                            state.current_step = 0;
                            state.matched_events.clear();
                            continue;
                        }
                    }
                    SequenceMatchResult result;
                    result.pattern_name = pattern.name;
                    result.pattern = &pattern;
                    result.matched_events = state.matched_events;
                    state.current_step = 0;
                    state.matched_events.clear();
                    match_count_++;
                    return result;
                }
            }
        }
    }

    return {};
}

bool BehavioralSequenceDetector::matchesStep(const RuntimeEvent& event,
                                              const SequenceStep& step) const {
    std::string type_str = eventTypeToString(event.type);

    for (size_t i = 0; i < step.match_any.size(); i++) {
        const auto& matcher = step.match_any[i];
        const std::string& glob = (i < step.detail_globs.size()) ? step.detail_globs[i] : "";
        bool type_match = false;

        // Check against event detail (includes method name like "env.get('HOME')")
        // If matcher contains wildcards, use glob matching; otherwise literal substring.
        if (matcher.find('*') != std::string::npos) {
            if (globMatch(event.detail, matcher)) {
                type_match = true;
            }
        } else if (event.detail.find(matcher) != std::string::npos) {
            type_match = true;
        }
        // Check against type string directly (with normalization for UPPERCASE_UNDERSCORE format)
        else if (type_str == matcher || type_str == normalizeEventTypeName(matcher)) {
            type_match = true;
        }
        // Special aliases
        else if (matcher == "encode" && event.type == RuntimeEventType::ENCODE) {
            type_match = true;
        }
        else if (matcher == "shell_exec" && event.type == RuntimeEventType::SHELL_EXEC) {
            type_match = true;
        }
        else if (matcher == "base64" && event.type == RuntimeEventType::ENCODE &&
                 event.detail.find("base64") != std::string::npos) {
            type_match = true;
        }
        else if (matcher == "compress" && event.type == RuntimeEventType::ENCODE &&
                 event.detail.find("compress") != std::string::npos) {
            type_match = true;
        }
        // Backward-compat aliases for NET_CONNECT
        else if ((matcher == "http.post" || matcher == "http.get" ||
                  matcher == "http" || matcher == "net_connect") &&
                 event.type == RuntimeEventType::NET_CONNECT) {
            type_match = true;
        }

        if (type_match) {
            // Per-matcher detail glob: apply glob[i] for match_any[i]
            if (!glob.empty() && !globMatch(event.detail, glob)) {
                continue;  // Glob didn't match, try next matcher
            }
            return true;
        }
    }
    return false;
}

bool BehavioralSequenceDetector::globMatch(const std::string& text,
                                            const std::string& pattern) const {
    if (pattern.empty() || pattern == "*") return true;

    // Split pattern by '*' and check each segment appears in order
    std::vector<std::string> segments;
    std::istringstream iss(pattern);
    std::string seg;
    while (std::getline(iss, seg, '*')) {
        if (!seg.empty()) segments.push_back(seg);
    }
    if (segments.empty()) return true;

    size_t pos = 0;
    bool starts_with_star = (pattern[0] == '*');
    bool ends_with_star = (pattern.back() == '*');

    for (size_t i = 0; i < segments.size(); i++) {
        size_t found;
        if (i == 0 && !starts_with_star) {
            if (text.find(segments[i]) != 0) return false;
            found = 0;
        } else {
            found = text.find(segments[i], pos);
        }
        if (found == std::string::npos) return false;
        pos = found + segments[i].size();
    }

    if (!ends_with_star) {
        return pos == text.size();
    }
    return true;
}

std::string BehavioralSequenceDetector::eventTypeToString(RuntimeEventType type) const {
    switch (type) {
        case RuntimeEventType::ENV_READ:        return "env.get";
        case RuntimeEventType::ENV_WRITE:       return "env.set_var";
        case RuntimeEventType::FILE_READ:       return "file.read";
        case RuntimeEventType::FILE_WRITE:      return "file.write";
        case RuntimeEventType::NET_CONNECT:     return "net_connect";
        case RuntimeEventType::SHELL_EXEC:      return "shell_exec";
        case RuntimeEventType::AGENT_SEND:      return "agent.send";
        case RuntimeEventType::AGENT_RESPONSE:  return "agent.response";
        case RuntimeEventType::TAINT_VIOLATION: return "taint_violation";
        case RuntimeEventType::TAINT_SANITIZED: return "taint_sanitized";
        case RuntimeEventType::ENCODE:          return "encode";
        case RuntimeEventType::DECODE:          return "decode";
        case RuntimeEventType::PROCESS_EXEC:    return "process.exec";
        case RuntimeEventType::CONFIG_RELOAD:   return "config_reload";
        case RuntimeEventType::CHECK_FAILED:    return "check_failed";
        case RuntimeEventType::TOOL_CALL:       return "tool_call";
        case RuntimeEventType::TOOL_RESULT:     return "tool_result";
        case RuntimeEventType::TOOL_ERROR:      return "tool_error";
        case RuntimeEventType::TOOL_BLOCKED:    return "tool_blocked";
        case RuntimeEventType::CODEGEN_EXEC:    return "codegen_exec";
        case RuntimeEventType::CODEGEN_BLOCKED: return "codegen_blocked";
        case RuntimeEventType::PULSE_DEGRADED:  return "pulse_degraded";
        case RuntimeEventType::PULSE_IMPAIRED:  return "pulse_impaired";
        case RuntimeEventType::REFUSAL_ATTESTATION: return "refusal_attestation";
    }
    return "unknown";
}

SequenceMatchResult BehavioralSequenceDetector::wouldMatch(const RuntimeEvent& event) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_ || !config_->enabled) return {};
    const auto& patterns = getActivePatterns();
    if (patterns.empty()) return {};

    for (const auto& pattern : patterns) {
        auto it = pattern_states_.find(pattern.name);
        if (it == pattern_states_.end()) continue;
        const auto& state = it->second;

        // Only fire if this event would be the FINAL step
        if (state.current_step == 0) continue;
        if (state.current_step + 1 != pattern.steps.size()) continue;

        // Gap check: would this event arrive too late?
        // sequence_counter_+1 is the would-be seq_id; -1 to match recordEvent formula
        if (pattern.max_gap > 0 && state.last_match_seq_id > 0) {
            size_t gap = sequence_counter_ + 1 - state.last_match_seq_id - 1;
            if (gap > static_cast<size_t>(pattern.max_gap)) continue;
        }

        // Decay check (time + turn — matches recordEvent logic)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - state.last_match_time).count();
        if (elapsed > pattern.decay_seconds) continue;
        if (event.turn > 0 && state.last_match_turn > 0 &&
            (event.turn - state.last_match_turn) > pattern.decay_turns) continue;

        // Does this event match the final step?
        if (matchesStep(event, pattern.steps[state.current_step])) {
            // Cross-agent check: reject if all events (including candidate) from same agent
            if (pattern.cross_agent) {
                std::unordered_set<std::string> agents;
                for (const auto& me : state.matched_events) {
                    if (!me.agent_config.empty()) agents.insert(me.agent_config);
                }
                if (!event.agent_config.empty()) agents.insert(event.agent_config);
                if (agents.size() < 2) continue;
            }
            SequenceMatchResult result;
            result.pattern_name = pattern.name;
            result.pattern = &pattern;
            result.matched_events = state.matched_events;  // steps so far (not including this one)
            return result;
        }
    }
    return {};
}

std::vector<RuntimeEvent> BehavioralSequenceDetector::getEventsForTurn(int turn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RuntimeEvent> result;
    for (const auto& ev : event_buffer_) {
        if (ev.turn == turn) {
            result.push_back(ev);
        }
    }
    return result;
}

void BehavioralSequenceDetector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    event_buffer_.clear();
    sequence_counter_ = 0;
    match_count_ = 0;
    pattern_states_.clear();
}

size_t BehavioralSequenceDetector::totalEventsProcessed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_counter_;
}

size_t BehavioralSequenceDetector::totalEventsEvicted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evicted_event_count_;
}

size_t BehavioralSequenceDetector::totalPatternsMatched() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return match_count_;
}

double BehavioralSequenceDetector::getMaxPartialProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_ || !config_->enabled) return 0.0;
    const auto& patterns = getActivePatterns();
    if (patterns.empty()) return 0.0;

    double max_progress = 0.0;
    for (const auto& pattern : patterns) {
        if (pattern.steps.empty()) continue;
        auto it = pattern_states_.find(pattern.name);
        if (it == pattern_states_.end()) continue;
        double progress = static_cast<double>(it->second.current_step) /
                          static_cast<double>(pattern.steps.size());
        if (progress > max_progress) max_progress = progress;
    }
    return max_progress;
}

// ============================================================================
// ContextDriftAnalyzer
// ============================================================================

void ContextDriftAnalyzer::configure(const ContextDriftConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = &config;
    drift_states_.clear();
}

void ContextDriftAnalyzer::updateConfig(const ContextDriftConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = &config;
    // Preserve: drift_states_, turns_analyzed_
}

bool ContextDriftAnalyzer::isEnabled() const {
    return config_ && config_->enabled;
}

bool ContextDriftAnalyzer::recordTurn(int handle_id, int turn_number,
                                       const std::vector<RuntimeEvent>& turn_events,
                                       const std::string& error_if_any) {
    if (!config_ || !config_->enabled) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    turns_analyzed_++;

    auto& state = drift_states_[handle_id];
    state.handle_id = handle_id;
    state.signals_fired_this_turn = 0;
    state.turns_analyzed++;

    // Temporal trust decay — coherence erodes over time even when idle
    if (config_->temporal_decay_enabled) {
        auto now = std::chrono::steady_clock::now();
        double elapsed_min = std::chrono::duration<double, std::ratio<60>>(
            now - state.last_activity_time).count();
        if (elapsed_min > config_->temporal_decay_grace_minutes) {
            double decay = config_->temporal_decay_per_minute *
                (elapsed_min - config_->temporal_decay_grace_minutes);
            state.coherence_score -= decay;
        }
        state.last_activity_time = now;
    }

    // Only check every N turns
    if (turn_number - state.last_checked_turn < config_->check_interval_turns) {
        return false;
    }
    state.last_checked_turn = turn_number;

    // Compute fingerprint for this turn's events
    std::string fp = computeFingerprint(turn_events);
    state.turn_fingerprints.push_back(fp);
    if (static_cast<int>(state.turn_fingerprints.size()) > config_->fingerprint_window) {
        state.turn_fingerprints.pop_front();
    }

    // Rate normalization helper: when enabled, scales penalty by event rate
    // (count / turns_analyzed) instead of flat weight. Makes early turns more
    // tolerant and sustained bad behavior more penalizing.
    auto penalty = [&](double weight, int count) -> double {
        if (config_->rate_normalized && state.turns_analyzed > 1) {
            double rate = static_cast<double>(count) / state.turns_analyzed;
            return rate * weight;
        }
        return weight;
    };

    // Adaptive baselining: during baseline window, count signals but suppress penalties
    bool in_baseline = config_->adaptive_baseline_enabled && !state.baseline_complete;
    // Per-turn signal counters (used for baseline accumulation)
    int turn_failures = 0, turn_circular = 0, turn_scope_creep = 0, turn_contradictions = 0;

    // Signal 1: Circular actions (same fingerprint repeats)
    if (config_->signals.circular_actions && isCircular(state, fp)) {
        state.circular_action_count++;
        turn_circular++;
        if (!in_baseline) {
            state.coherence_score -= penalty(config_->weights.circular, state.circular_action_count);
        }
        state.signals_fired_this_turn++;
    }

    // Signal 2: Repeated failures (same error string seen multiple times)
    if (config_->signals.repeated_failures && !error_if_any.empty()) {
        // Infrastructure errors (API failures, network errors) are categorically different
        // from behavioral drift — a flaky API is not the agent misbehaving.
        bool is_infrastructure = (error_if_any.size() >= 15 &&
            error_if_any.substr(0, 15) == "infrastructure:");
        if (!is_infrastructure || !config_->signals.exclude_infrastructure_errors) {
            state.recent_errors.push_back(error_if_any);
            if (state.recent_errors.size() > 10) state.recent_errors.pop_front();
            int same_count = 0;
            for (const auto& e : state.recent_errors) {
                if (e == error_if_any) same_count++;
            }
            if (same_count >= config_->thresholds.repeated_failure_count) {
                state.repeated_failures++;
                turn_failures++;
                if (!in_baseline) {
                    state.coherence_score -= penalty(config_->weights.repeated_failure, state.repeated_failures);
                }
                state.signals_fired_this_turn++;
            }
        }
    }

    // Compute per-turn action types (shared by signals 3 and 5)
    std::unordered_set<std::string> turn_types;
    if (!turn_events.empty()) {
        for (const auto& ev : turn_events) {
            std::string part;
            switch (ev.type) {
                case RuntimeEventType::ENV_READ:    part = "ER"; break;
                case RuntimeEventType::ENV_WRITE:   part = "EW"; break;
                case RuntimeEventType::FILE_READ:   part = "FR"; break;
                case RuntimeEventType::FILE_WRITE:  part = "FW"; break;
                case RuntimeEventType::NET_CONNECT: part = "NC"; break;
                case RuntimeEventType::SHELL_EXEC:  part = "SX"; break;
                case RuntimeEventType::AGENT_SEND:  part = "AS"; break;
                case RuntimeEventType::AGENT_RESPONSE: part = "AR"; break;
                case RuntimeEventType::ENCODE:      part = "EN"; break;
                case RuntimeEventType::DECODE:      part = "DE"; break;
                case RuntimeEventType::PROCESS_EXEC: part = "PX"; break;
                case RuntimeEventType::REFUSAL_ATTESTATION: part = "RA"; break;
                default: part = "XX"; break;
            }
            turn_types.insert(part);
        }
    }

    // Signal 3: Scope creep (new event types appearing that weren't seen before)
    if (config_->signals.scope_creep && !turn_types.empty()) {
        int new_types = 0;
        for (const auto& t : turn_types) {
            if (state.seen_event_types.find(t) == state.seen_event_types.end()) {
                new_types++;
            }
        }

        // Only fire after enough history and multiple new types at once
        if (static_cast<int>(state.turn_fingerprints.size()) >= config_->thresholds.scope_creep_min_history && new_types >= config_->thresholds.scope_creep_min_new_types) {
            state.scope_creep_count++;
            turn_scope_creep++;
            if (!in_baseline) {
                state.coherence_score -= penalty(config_->weights.scope_creep, state.scope_creep_count);
            }
            state.signals_fired_this_turn++;
        }

        for (const auto& t : turn_types) {
            state.seen_event_types.insert(t);
        }
    }

    // Signal 5: Vocabulary contraction (action diversity shrinking over time)
    // Detects "narrowing paths that still appear formally open"
    if (config_->signals.vocabulary_contraction && !turn_types.empty()) {
        state.per_turn_types.push_back(turn_types);
        if (static_cast<int>(state.per_turn_types.size()) > config_->fingerprint_window) {
            state.per_turn_types.pop_front();
        }

        // Need enough history to compare early vs recent
        int window = static_cast<int>(state.per_turn_types.size());
        if (window >= config_->thresholds.vocab_contraction_window) {
            int half = window / 2;

            // Compute Shannon entropy of action type distribution in a window half
            // H = -Σ p(type) * log2(p(type))
            auto computeEntropy = [](const std::deque<std::unordered_set<std::string>>& turns,
                                     size_t from, size_t to) -> double {
                std::unordered_map<std::string, int> freq;
                int total = 0;
                for (size_t i = from; i < to; i++) {
                    for (const auto& t : turns[i]) {
                        freq[t]++;
                        total++;
                    }
                }
                if (total == 0) return 0.0;
                double entropy = 0.0;
                for (const auto& [type, count] : freq) {
                    double p = static_cast<double>(count) / total;
                    if (p > 0.0) entropy -= p * std::log2(p);
                }
                return entropy;
            };

            double early_entropy = computeEntropy(state.per_turn_types,
                0, static_cast<size_t>(half));
            double recent_entropy = computeEntropy(state.per_turn_types,
                static_cast<size_t>(half), static_cast<size_t>(window));

            // Track initial entropy for baseline comparison
            if (state.initial_entropy < 0.0 && early_entropy > 0.0) {
                state.initial_entropy = early_entropy;
            }

            // Contraction: recent entropy dropped by 40%+ from initial baseline
            // Also fires on cardinality drop as fallback (early had 3+ types, recent lost 2+)
            if (state.initial_entropy > config_->thresholds.entropy_min_initial && recent_entropy < state.initial_entropy * config_->thresholds.entropy_contraction_ratio) {
                state.vocabulary_contraction_count++;
                if (!in_baseline) {
                    state.coherence_score -= penalty(config_->weights.vocabulary_contraction, state.vocabulary_contraction_count);
                }
                state.signals_fired_this_turn++;
            }
        }
    }

    // Signal 4: Intent contradictions (denied capability → alternative path)
    if (config_->signals.intent_contradictions && !turn_events.empty()) {
        // Map event types to capability categories
        auto capCategory = [](RuntimeEventType t) -> std::string {
            switch (t) {
                case RuntimeEventType::FILE_READ:
                case RuntimeEventType::FILE_WRITE:   return "filesystem";
                case RuntimeEventType::NET_CONNECT:  return "network";
                case RuntimeEventType::SHELL_EXEC:
                case RuntimeEventType::PROCESS_EXEC: return "execution";
                case RuntimeEventType::ENV_READ:
                case RuntimeEventType::ENV_WRITE:    return "environment";
                case RuntimeEventType::ENCODE:
                case RuntimeEventType::DECODE:       return "encoding";
                default: return "";
            }
        };

        // Collect blocked capabilities from this turn's CHECK_FAILED events
        std::unordered_set<std::string> this_turn_blocked;
        // Collect attempted capabilities from this turn's non-failure events
        std::unordered_set<std::string> this_turn_attempted;
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::CHECK_FAILED) {
                // Extract capability category from the detail string
                // Detail typically contains the check name (e.g., "filesystem", "network")
                std::string det = ev.detail;
                if (det.find("file") != std::string::npos || det.find("File") != std::string::npos ||
                    det.find("path") != std::string::npos) this_turn_blocked.insert("filesystem");
                if (det.find("network") != std::string::npos || det.find("Network") != std::string::npos ||
                    det.find("http") != std::string::npos) this_turn_blocked.insert("network");
                if (det.find("shell") != std::string::npos || det.find("Shell") != std::string::npos ||
                    det.find("exec") != std::string::npos) this_turn_blocked.insert("execution");
                if (det.find("env") != std::string::npos || det.find("Env") != std::string::npos)
                    this_turn_blocked.insert("environment");
            } else {
                std::string cap = capCategory(ev.type);
                if (!cap.empty()) this_turn_attempted.insert(cap);
            }
        }

        // Check if this turn attempts capabilities that were blocked in the previous turn
        for (const auto& cap : this_turn_attempted) {
            if (state.prev_turn_blocked_caps.count(cap)) {
                state.contradictions++;
                turn_contradictions++;
                if (!in_baseline) {
                    state.coherence_score -= penalty(config_->weights.contradiction, state.contradictions);
                }
                state.signals_fired_this_turn++;
                break;  // one contradiction per turn is enough
            }
        }

        // Update previous turn blocked capabilities for next check
        state.prev_turn_blocked_caps = this_turn_blocked;
    }

    // Signal 6: Coherence velocity (rapid decay detection)
    // Fires when coherence is dropping faster than -0.15 per turn
    if (config_->signals.coherence_velocity && state.coherence_history.size() >= 2) {
        size_t n = state.coherence_history.size();
        double prev_velocity = state.coherence_velocity;
        state.coherence_velocity = state.coherence_score - state.coherence_history[n - 1];
        state.coherence_acceleration = state.coherence_velocity - prev_velocity;

        if (state.coherence_velocity < config_->thresholds.velocity_drop) {
            if (!in_baseline) {
                state.coherence_score -= penalty(config_->weights.coherence_velocity, 1);
            }
            state.signals_fired_this_turn++;
        }
    }

    // Signal 7: Capability underutilization (F9)
    // Map events to capabilities and track exercised set
    if (config_->signals.capability_underutilization) {
        if (state.first_event_turn < 0) state.first_event_turn = turn_number;

        for (const auto& ev : turn_events) {
            std::string cap;
            switch (ev.type) {
                case RuntimeEventType::FILE_READ:
                case RuntimeEventType::FILE_WRITE: cap = "filesystem"; break;
                case RuntimeEventType::NET_CONNECT: cap = "network"; break;
                case RuntimeEventType::SHELL_EXEC:
                case RuntimeEventType::PROCESS_EXEC: cap = "execution"; break;
                case RuntimeEventType::ENV_READ:
                case RuntimeEventType::ENV_WRITE: cap = "environment"; break;
                default: break;
            }
            if (!cap.empty()) {
                bool was_new = state.exercised_capabilities.insert(cap).second;
                // Late first use (10+ turns after start) of a granted capability = suspicious
                if (was_new && state.granted_capabilities.count(cap) &&
                    turn_number - state.first_event_turn >= config_->thresholds.underutilization_delay) {
                    if (!in_baseline) {
                        state.coherence_score -= penalty(config_->weights.capability_underutilization, 1);
                    }
                    state.signals_fired_this_turn++;
                }
            }
        }
    }

    // Adaptive baseline: accumulate per-turn signal counts during window
    if (config_->adaptive_baseline_enabled) {
        if (!state.baseline_complete) {
            // Accumulate running sums for baseline computation
            state.baseline.sum_failures += turn_failures;
            state.baseline.sum_sq_failures += turn_failures * turn_failures;
            state.baseline.sum_circular += turn_circular;
            state.baseline.sum_scope_creep += turn_scope_creep;
            state.baseline.sum_contradictions += turn_contradictions;
            state.baseline_turns_counted++;

            if (state.baseline_turns_counted >= config_->adaptive_baseline_window) {
                // Compute means
                double n = static_cast<double>(state.baseline_turns_counted);
                state.baseline.mean_failures = state.baseline.sum_failures / n;
                state.baseline.mean_circular = state.baseline.sum_circular / n;
                state.baseline.mean_scope_creep = state.baseline.sum_scope_creep / n;
                state.baseline.mean_contradictions = state.baseline.sum_contradictions / n;

                // Compute stddev for failures (used as primary threshold signal)
                // Bessel's correction (n-1): unbiased sample variance for small samples
                double variance = (n > 1.0)
                    ? ((state.baseline.sum_sq_failures / n) -
                       (state.baseline.mean_failures * state.baseline.mean_failures)) * (n / (n - 1.0))
                    : 0.0;
                state.baseline.stddev_failures = (variance > 0.0) ? std::sqrt(variance) : 0.0;

                state.baseline_complete = true;
                state.baseline_completed_turn = turn_number;
            }
        }
    }

    // Signal 8: Response quality — content ratio too low (mostly thinking, little output)
    // Gemini: candidatesTokenCount = content only, thoughtsTokenCount = separate.
    // Ratio = content / (content + thinking) = proportion of generation that is useful.
    if (config_->signals.response_quality) {
        for (const auto& ev : turn_events) {
            double total_gen = static_cast<double>(ev.output_tokens + ev.thinking_tokens);
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && total_gen > 0) {
                double ratio = static_cast<double>(ev.output_tokens) / total_gen;
                if (ratio < config_->thresholds.response_quality_min_ratio) {
                    state.response_quality_count++;
                    if (!in_baseline) {
                        state.coherence_score -= penalty(config_->weights.response_quality,
                                                          state.response_quality_count);
                    }
                    state.signals_fired_this_turn++;
                }
            }
        }
    }

    // Signal 9: Thinking collapse — thinking tokens dropped >50% from baseline mean
    if (config_->signals.thinking_collapse) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE) {
                state.thinking_history.push_back(ev.thinking_tokens);
                if (static_cast<int>(state.thinking_history.size()) >
                    config_->thresholds.thinking_history_window) {
                    state.thinking_history.pop_front();
                }
            }
        }
        // Set baseline mean when adaptive baseline completes (or after enough history)
        if (state.thinking_baseline_mean < 0.0 &&
            (state.baseline_complete || static_cast<int>(state.thinking_history.size()) >=
             config_->thresholds.thinking_history_window / 2)) {
            if (!state.thinking_history.empty()) {
                double sum = 0.0;
                for (int t : state.thinking_history) sum += t;
                state.thinking_baseline_mean = sum / static_cast<double>(state.thinking_history.size());
            }
        }
        // Fire when current mean drops below threshold ratio of baseline
        if (state.thinking_baseline_mean > 0.0 &&
            static_cast<int>(state.thinking_history.size()) >= 3) {
            double current_sum = 0.0;
            for (int t : state.thinking_history) current_sum += t;
            double current_mean = current_sum / static_cast<double>(state.thinking_history.size());
            if (current_mean < state.thinking_baseline_mean * config_->thresholds.thinking_collapse_ratio) {
                state.thinking_collapse_count++;
                if (!in_baseline) {
                    state.coherence_score -= penalty(config_->weights.thinking_collapse,
                                                      state.thinking_collapse_count);
                }
                state.signals_fired_this_turn++;
            }
        }
    }

    // Signal 10: Semantic stability (F19) — topic shift detection via Jaccard similarity
    if (config_->signals.semantic_stability) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && !ev.content_keywords.empty()) {
                if (!state.prev_response_keywords.empty()) {
                    // Jaccard similarity: |A ∩ B| / |A ∪ B|
                    size_t intersection = 0;
                    for (const auto& kw : ev.content_keywords) {
                        if (state.prev_response_keywords.count(kw)) intersection++;
                    }
                    size_t union_size = state.prev_response_keywords.size() +
                                        ev.content_keywords.size() - intersection;
                    double similarity = union_size > 0 ?
                        static_cast<double>(intersection) / static_cast<double>(union_size) : 1.0;

                    if (similarity < config_->thresholds.semantic_stability_min_overlap) {
                        state.semantic_stability_count++;
                        if (!in_baseline) {
                            state.coherence_score -= penalty(config_->weights.semantic_stability,
                                                              state.semantic_stability_count);
                        }
                        state.signals_fired_this_turn++;
                    }
                }
                state.prev_response_keywords = ev.content_keywords;
                break;
            }
        }
    }

    // Signal 11: Mandate alignment — continuous system_prompt keyword adherence
    if (config_->signals.mandate_alignment && !state.mandate_keywords.empty()) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && !ev.content_keywords.empty()) {
                int found = 0;
                for (const auto& kw : state.mandate_keywords) {
                    if (ev.content_keywords.count(kw)) found++;
                }
                double alignment = static_cast<double>(found) / static_cast<double>(state.mandate_keywords.size());
                state.mandate_alignment_history.push_back(alignment);
                while (state.mandate_alignment_history.size() > 20)
                    state.mandate_alignment_history.pop_front();

                // Fire when rolling mean drops below threshold
                double mean = 0;
                for (double v : state.mandate_alignment_history) mean += v;
                mean /= static_cast<double>(state.mandate_alignment_history.size());

                if (mean < config_->thresholds.mandate_alignment_min) {
                    state.mandate_drift_count++;
                    if (!in_baseline) {
                        state.coherence_score -= penalty(config_->weights.mandate_alignment,
                                                          state.mandate_drift_count);
                    }
                    state.signals_fired_this_turn++;
                }
                break;
            }
        }
    }

    // F15: Natural healing — proportional to signal cleanliness.
    // Full healing at 0 signals, half at 1, third at 2, etc.
    if (config_->coherence_natural_healing > 0.0) {
        double heal_factor = 1.0 / (1.0 + state.signals_fired_this_turn);
        state.coherence_score += config_->coherence_natural_healing * heal_factor;
    }

    // Clamp coherence score to [0.0, 1.0]
    state.coherence_score = std::max(0.0, std::min(1.0, state.coherence_score));

    // Track coherence history for velocity/acceleration computation
    state.coherence_history.push_back(state.coherence_score);
    if (static_cast<int>(state.coherence_history.size()) > config_->thresholds.coherence_history_size) {
        state.coherence_history.pop_front();
    }

    return state.coherence_score < config_->coherence_threshold;
}

double ContextDriftAnalyzer::getCoherence(int handle_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = drift_states_.find(handle_id);
    if (it == drift_states_.end()) return 1.0;
    return it->second.coherence_score;
}

std::optional<DriftState> ContextDriftAnalyzer::getDriftState(int handle_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = drift_states_.find(handle_id);
    if (it == drift_states_.end()) return std::nullopt;
    return it->second;
}

std::string ContextDriftAnalyzer::computeFingerprint(
    const std::vector<RuntimeEvent>& events) const {
    // Fingerprint = sorted concatenation of event type abbreviations
    std::vector<std::string> parts;
    parts.reserve(events.size());
    for (const auto& ev : events) {
        std::string part;
        switch (ev.type) {
            case RuntimeEventType::ENV_READ:    part = "ER"; break;
            case RuntimeEventType::ENV_WRITE:   part = "EW"; break;
            case RuntimeEventType::FILE_READ:   part = "FR"; break;
            case RuntimeEventType::FILE_WRITE:  part = "FW"; break;
            case RuntimeEventType::NET_CONNECT: part = "NC"; break;
            case RuntimeEventType::SHELL_EXEC:  part = "SX"; break;
            case RuntimeEventType::AGENT_SEND:  part = "AS"; break;
            case RuntimeEventType::AGENT_RESPONSE: part = "AR"; break;
            case RuntimeEventType::ENCODE:      part = "EN"; break;
            case RuntimeEventType::DECODE:      part = "DE"; break;
            case RuntimeEventType::PROCESS_EXEC: part = "PX"; break;
            default: part = "XX"; break;
        }
        // Include content fingerprint for agent events — breaks mechanical
        // "ARAS" pattern so isCircular() only fires on actual loops
        if (!ev.content_fingerprint.empty()) {
            part += ":" + ev.content_fingerprint.substr(0, 8);
        }
        parts.push_back(part);
    }
    std::sort(parts.begin(), parts.end());
    std::string result;
    for (const auto& p : parts) result += p;
    return result;
}

bool ContextDriftAnalyzer::isCircular(const DriftState& state,
                                       const std::string& fingerprint) const {
    if (state.turn_fingerprints.size() < 2) return false;
    // Check if same fingerprint appeared in last 3 turns
    int matches = 0;
    size_t check_count = std::min(state.turn_fingerprints.size(), static_cast<size_t>(config_->thresholds.circular_lookback));
    for (size_t i = state.turn_fingerprints.size() - check_count;
         i < state.turn_fingerprints.size(); i++) {
        if (state.turn_fingerprints[i] == fingerprint) {
            matches++;
        }
    }
    return matches >= 2;
}

void ContextDriftAnalyzer::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    drift_states_.clear();
    turns_analyzed_ = 0;
}

size_t ContextDriftAnalyzer::totalTurnsAnalyzed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return turns_analyzed_;
}

void ContextDriftAnalyzer::updateCheckpointState(int handle_id, double pressure,
                                                   int consecutive, int checkpoint_turn) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = drift_states_[handle_id];
    state.last_pressure_score = pressure;
    state.consecutive_high_pressure_turns = consecutive;
    if (checkpoint_turn >= 0) {
        state.last_checkpoint_turn = checkpoint_turn;
    }
}

void ContextDriftAnalyzer::setInheritedPressure(int handle_id, double pressure) {
    std::lock_guard<std::mutex> lock(mutex_);
    drift_states_[handle_id].inherited_pressure = std::max(0.0, std::min(1.0, pressure));
}

void ContextDriftAnalyzer::resetCoherence(int handle_id, double amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = drift_states_.find(handle_id);
    if (it != drift_states_.end()) {
        // Diminishing returns: recovery gets harder as coherence approaches cap.
        // Instant recovery events (challenge pass, pipeline transition) shouldn't
        // erase all evidence of degradation — sustained healthy behavior earns
        // that back through natural healing instead.
        double cap = config_ ? config_->coherence_recovery_cap : 1.0;
        double old = it->second.coherence_score;
        if (old < cap) {
            it->second.coherence_score = old + amount * (cap - old);
            it->second.coherence_score = std::min(cap, it->second.coherence_score);
        }
        // Clear history and derivatives to prevent false velocity/acceleration signals
        it->second.coherence_history.clear();
        it->second.coherence_history.push_back(it->second.coherence_score);
        it->second.coherence_velocity = 0.0;
        it->second.coherence_acceleration = 0.0;
        it->second.last_recovery_turn = it->second.last_checked_turn;
    }
}

void ContextDriftAnalyzer::initializeMandateKeywords(
    int handle_id, const std::unordered_set<std::string>& keywords) {
    std::lock_guard<std::mutex> lock(mutex_);
    drift_states_[handle_id].mandate_keywords = keywords;
}

} // namespace governance
} // namespace naab
