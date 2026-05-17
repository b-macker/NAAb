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
}

bool BehavioralSequenceDetector::isEnabled() const {
    return config_ && config_->enabled;
}

SequenceMatchResult BehavioralSequenceDetector::recordEvent(const RuntimeEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_ || !config_->enabled || config_->patterns.empty()) return {};

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
    for (const auto& pattern : config_->patterns) {
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

    for (const auto& matcher : step.match_any) {
        bool type_match = false;

        // Check against event detail (includes method name like "env.get('HOME')")
        if (event.detail.find(matcher) != std::string::npos) {
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
        // Fix #11: backward-compat aliases for NET_CONNECT
        else if ((matcher == "http.post" || matcher == "http.get" ||
                  matcher == "http" || matcher == "net_connect") &&
                 event.type == RuntimeEventType::NET_CONNECT) {
            type_match = true;
        }

        if (type_match) {
            // If there's a detail glob, also check it
            if (!step.detail_glob.empty()) {
                if (!globMatch(event.detail, step.detail_glob)) {
                    continue;  // Glob didn't match, try next matcher
                }
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
    pattern_states_.clear();
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

    auto& state = drift_states_[handle_id];
    state.handle_id = handle_id;

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
        }

        for (const auto& t : turn_types) {
            state.seen_event_types.insert(t);
        }
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
}

} // namespace governance
} // namespace naab
