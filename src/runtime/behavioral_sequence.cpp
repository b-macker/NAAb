#include "naab/governance.h"
#include "naab/behavioral_sequence.h"
#include <algorithm>
#include <sstream>

namespace naab {
namespace governance {

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

void BehavioralSequenceDetector::buildDefaultPatterns() {
    default_patterns_.clear();

    // Helper to create a step matching event types with optional detail globs
    auto makeStep = [](std::vector<std::string> types, std::vector<std::string> globs = {}) {
        SequenceStep step;
        step.match_any = std::move(types);
        step.detail_globs = std::move(globs);
        return step;
    };

    // 1. Credential harvesting: ENV_READ(secret/key/token) → NET_CONNECT
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
        // Check against type string directly
        else if (type_str == matcher) {
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

    // Signal 1: Circular actions (same fingerprint repeats)
    if (config_->signals.circular_actions && isCircular(state, fp)) {
        state.circular_action_count++;
        state.coherence_score -= config_->weights.circular;
        state.signals_fired_this_turn++;
    }

    // Signal 2: Repeated failures (same error string seen multiple times)
    if (config_->signals.repeated_failures && !error_if_any.empty()) {
        state.recent_errors.push_back(error_if_any);
        if (state.recent_errors.size() > 10) state.recent_errors.pop_front();
        int same_count = 0;
        for (const auto& e : state.recent_errors) {
            if (e == error_if_any) same_count++;
        }
        if (same_count >= 3) {
            state.repeated_failures++;
            state.coherence_score -= config_->weights.repeated_failure;
            state.signals_fired_this_turn++;
        }
    }

    // Signal 3: Scope creep (new event types appearing that weren't seen before)
    if (config_->signals.scope_creep && !turn_events.empty()) {
        std::unordered_set<std::string> turn_types;
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
                default: part = "XX"; break;
            }
            turn_types.insert(part);
        }

        int new_types = 0;
        for (const auto& t : turn_types) {
            if (state.seen_event_types.find(t) == state.seen_event_types.end()) {
                new_types++;
            }
        }

        // Only fire after enough history and multiple new types at once
        if (state.turn_fingerprints.size() >= 3 && new_types >= 2) {
            state.scope_creep_count++;
            state.coherence_score -= config_->weights.scope_creep;
            state.signals_fired_this_turn++;
        }

        for (const auto& t : turn_types) {
            state.seen_event_types.insert(t);
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
                state.coherence_score -= config_->weights.contradiction;
                state.signals_fired_this_turn++;
                break;  // one contradiction per turn is enough
            }
        }

        // Update previous turn blocked capabilities for next check
        state.prev_turn_blocked_caps = this_turn_blocked;
    }

    // Clamp coherence score to [0.0, 1.0]
    state.coherence_score = std::max(0.0, std::min(1.0, state.coherence_score));

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
    size_t check_count = std::min(state.turn_fingerprints.size(), size_t(3));
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

} // namespace governance
} // namespace naab
