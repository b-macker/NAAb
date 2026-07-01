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

// Stop words: English function words >3 chars + LLM response boilerplate.
// No programming terms — those are domain-relevant for coding assistants.
static const std::unordered_set<std::string> kStopWords = {
    "that", "this", "with", "from", "have", "your", "will", "also",
    "each", "more", "like", "just", "some", "when", "then",
    "into", "here", "been", "both", "want", "used", "them", "than",
    "what", "were", "they", "does", "done", "very", "much", "most",
    "only", "over", "such", "should", "would", "could", "about",
    "other", "their", "there", "which", "these", "those", "being",
    "after", "before",
    "sure", "great", "lets", "following", "below",
    "approach", "solution", "need", "look"
};

// Extract keywords (>3 chars) from text, lowercased — local version for CDD signals
static void extractKeywordsLocal(const std::string& text, std::unordered_set<std::string>& out) {
    std::string current;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (current.size() > 3 && !kStopWords.count(current)) out.insert(current);
            current.clear();
        }
    }
    if (current.size() > 3 && !kStopWords.count(current)) out.insert(current);
}

// Extract ordered plan steps from agent response text.
// Matches patterns: "1.", "1)", "Step 1:", "Phase 1:", "- Step 1"
// Returns vector of keyword sets, one per plan step (empty if no plan found).
static std::vector<std::unordered_set<std::string>> extractPlanSteps(const std::string& text) {
    std::vector<std::unordered_set<std::string>> steps;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        // Skip leading whitespace for pattern matching
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string trimmed = line.substr(start);
        bool is_step = false;
        // Pattern: "1." or "1)" (numbered list)
        if (trimmed.size() >= 2 && std::isdigit(static_cast<unsigned char>(trimmed[0]))) {
            size_t i = 1;
            while (i < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[i]))) i++;
            if (i < trimmed.size() && (trimmed[i] == '.' || trimmed[i] == ')')) {
                is_step = true;
            }
        }
        // Pattern: "Step N:" or "Phase N:" (case-insensitive)
        if (!is_step) {
            std::string lower = trimmed.substr(0, std::min(trimmed.size(), size_t(10)));
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if ((lower.substr(0, 5) == "step " || lower.substr(0, 6) == "phase ") &&
                lower.size() > 6) {
                // Check that a digit follows
                size_t dpos = (lower[0] == 's') ? 5 : 6;
                if (dpos < lower.size() && std::isdigit(static_cast<unsigned char>(lower[dpos]))) {
                    is_step = true;
                }
            }
        }
        // Pattern: "- Step N" or "* Step N" (bulleted step list)
        if (!is_step && trimmed.size() > 2 && (trimmed[0] == '-' || trimmed[0] == '*') &&
            trimmed[1] == ' ') {
            std::string rest = trimmed.substr(2);
            size_t rs = rest.find_first_not_of(" \t");
            if (rs != std::string::npos) {
                std::string rl = rest.substr(rs, 10);
                std::transform(rl.begin(), rl.end(), rl.begin(), ::tolower);
                if ((rl.substr(0, 5) == "step " || rl.substr(0, 6) == "phase ") &&
                    rl.size() > 6) {
                    size_t dpos = (rl[0] == 's') ? 5 : 6;
                    if (dpos < rl.size() && std::isdigit(static_cast<unsigned char>(rl[dpos]))) {
                        is_step = true;
                    }
                }
            }
        }
        if (is_step) {
            std::unordered_set<std::string> kw;
            extractKeywordsLocal(trimmed, kw);
            if (!kw.empty()) {
                steps.push_back(std::move(kw));
            }
        }
    }
    // Require at least 2 steps to constitute a "plan"
    if (steps.size() < 2) steps.clear();
    return steps;
}

// Negation/reversal markers — words that indicate contradictory intent
static bool hasNegationMarker(const std::unordered_set<std::string>& keywords) {
    static const std::unordered_set<std::string> negation_words = {
        "never", "stop", "skip", "remove", "disable", "avoid", "ignore",
        "cancel", "delete", "drop", "exclude", "reject", "undo", "revert",
        "without", "instead", "opposite", "contrary"
    };
    for (const auto& kw : keywords) {
        if (negation_words.count(kw)) return true;
    }
    return false;
}

// Extract entity mentions from response keywords.
// Entities are keywords that look like identifiers: longer than 4 chars and not
// common stop words. Returns a map of entity → context keywords (other keywords
// in the same response that co-occur, providing entity "context").
static std::unordered_map<std::string, std::unordered_set<std::string>>
extractEntityContext(const std::unordered_set<std::string>& response_keywords) {
    // Stop words to exclude from entity detection (common function words >3 chars)
    static const std::unordered_set<std::string> stop_words = {
        "this", "that", "with", "from", "have", "will", "been", "were", "they",
        "them", "then", "than", "what", "when", "where", "which", "while",
        "about", "after", "again", "could", "would", "should", "their", "there",
        "these", "those", "being", "other", "because", "between", "through",
        "before", "during", "under", "above", "each", "every", "into", "over",
        "some", "more", "most", "also", "just", "only", "very", "much", "such",
        "here", "does", "done", "make", "made", "like", "true", "false", "null",
        "none", "return", "function", "error", "value", "string", "number"
    };
    std::unordered_map<std::string, std::unordered_set<std::string>> result;
    // Identify entity-like keywords (longer, not stop words)
    std::vector<std::string> entities;
    std::vector<std::string> context_words;
    for (const auto& kw : response_keywords) {
        if (kw.size() > 4 && !stop_words.count(kw)) {
            entities.push_back(kw);
        }
        context_words.push_back(kw);
    }
    // Each entity gets all other keywords as context
    for (const auto& entity : entities) {
        auto& ctx = result[entity];
        for (const auto& cw : context_words) {
            if (cw != entity) ctx.insert(cw);
        }
    }
    return result;
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

    // Track gate-passing turns after baseline completes (for adaptive penalty rate)
    if (state.baseline_complete) {
        state.post_baseline_checks++;
    }

    // Compute fingerprint for this turn's events
    std::string fp = computeFingerprint(turn_events);
    state.turn_fingerprints.push_back(fp);
    if (static_cast<int>(state.turn_fingerprints.size()) > config_->fingerprint_window) {
        state.turn_fingerprints.pop_front();
    }

    // Base penalty: flat weight or rate-normalized. Makes early turns more
    // tolerant and sustained bad behavior more penalizing.
    auto base_penalty = [&](double weight, int count) -> double {
        if (config_->rate_normalized && state.turns_analyzed > 1) {
            double rate = static_cast<double>(count) / state.turns_analyzed;
            return rate * weight;
        }
        return weight;
    };

    // Adaptive penalty: modulates weight using per-signal baseline statistics.
    // Uses post_baseline_checks (gate-passing turns after baseline) as denominator,
    // matching the counting unit of signal counters. Returns 0 when post-baseline
    // firing rate is at/below baseline mean + k*stddev; proportional excess above.
    auto adaptive_penalty = [&](double weight, int count, int sig) -> double {
        if (config_->adaptive_baseline_enabled && state.baseline_complete &&
            state.post_baseline_checks > 0) {
            const auto& bl = state.signal_baselines[sig];
            int post_count = count - bl.snapshot;
            double current_rate = static_cast<double>(post_count) / state.post_baseline_checks;

            double threshold = bl.mean +
                config_->adaptive_baseline_sensitivity * std::max(bl.stddev, 0.1);

            if (current_rate <= threshold) return 0.0;

            double excess = current_rate - threshold;
            return weight * std::min(excess / std::max(threshold, 0.01), 1.0);
        }
        return base_penalty(weight, count);
    };

    // Adaptive baselining: during baseline window, count signals but suppress penalties
    bool in_baseline = config_->adaptive_baseline_enabled && !state.baseline_complete;
    // Per-turn signal counters (used for baseline accumulation)
    std::array<int, NUM_CDD_SIGNALS> turn_fired = {};

    // Signal 1: Circular actions (same fingerprint repeats)
    if (config_->signals.circular_actions && isCircular(state, fp)) {
        state.circular_action_count++;
        turn_fired[SIG_CIRCULAR]++;
        if (!in_baseline) {
            double p = adaptive_penalty(config_->weights.circular, state.circular_action_count, SIG_CIRCULAR);
            if (p > 0.0) {
                state.coherence_score -= p;
                state.signals_fired_this_turn++;
            }
        }
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
                turn_fired[SIG_REPEATED_FAILURES]++;
                if (!in_baseline) {
                    double p = adaptive_penalty(config_->weights.repeated_failure, state.repeated_failures, SIG_REPEATED_FAILURES);
                    if (p > 0.0) {
                        state.coherence_score -= p;
                        state.signals_fired_this_turn++;
                    }
                }
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
            turn_fired[SIG_SCOPE_CREEP]++;
            if (!in_baseline) {
                double p = adaptive_penalty(config_->weights.scope_creep, state.scope_creep_count, SIG_SCOPE_CREEP);
                if (p > 0.0) {
                    state.coherence_score -= p;
                    state.signals_fired_this_turn++;
                }
            }
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
                turn_fired[SIG_VOCAB_CONTRACTION]++;
                if (!in_baseline) {
                    double p = adaptive_penalty(config_->weights.vocabulary_contraction, state.vocabulary_contraction_count, SIG_VOCAB_CONTRACTION);
                    if (p > 0.0) {
                        state.coherence_score -= p;
                        state.signals_fired_this_turn++;
                    }
                }
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
                std::transform(det.begin(), det.end(), det.begin(), ::tolower);
                if (det.find("file") != std::string::npos ||
                    det.find("path") != std::string::npos) this_turn_blocked.insert("filesystem");
                if (det.find("network") != std::string::npos ||
                    det.find("http") != std::string::npos) this_turn_blocked.insert("network");
                if (det.find("shell") != std::string::npos ||
                    det.find("exec") != std::string::npos) this_turn_blocked.insert("execution");
                if (det.find("env") != std::string::npos)
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
                turn_fired[SIG_CONTRADICTIONS]++;
                if (!in_baseline) {
                    double p = adaptive_penalty(config_->weights.contradiction, state.contradictions, SIG_CONTRADICTIONS);
                    if (p > 0.0) {
                        state.coherence_score -= p;
                        state.signals_fired_this_turn++;
                    }
                }
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
        state.coherence_velocity = state.coherence_history[n - 1] - state.coherence_history[n - 2];
        state.coherence_acceleration = state.coherence_velocity - prev_velocity;

        if (state.coherence_velocity < config_->thresholds.velocity_drop) {
            turn_fired[SIG_COHERENCE_VELOCITY]++;
            if (!in_baseline) {
                state.coherence_score -= base_penalty(config_->weights.coherence_velocity, 1);
                state.signals_fired_this_turn++;
            }
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
                    turn_fired[SIG_CAPABILITY_UNDERUTIL]++;
                    if (!in_baseline) {
                        state.coherence_score -= base_penalty(config_->weights.capability_underutilization, 1);
                        state.signals_fired_this_turn++;
                    }
                }
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
                    turn_fired[SIG_RESPONSE_QUALITY]++;
                    if (!in_baseline) {
                        double p = adaptive_penalty(config_->weights.response_quality,
                                                    state.response_quality_count, SIG_RESPONSE_QUALITY);
                        if (p > 0.0) {
                            state.coherence_score -= p;
                            state.signals_fired_this_turn++;
                        }
                    }
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
                turn_fired[SIG_THINKING_COLLAPSE]++;
                if (!in_baseline) {
                    double p = adaptive_penalty(config_->weights.thinking_collapse,
                                                state.thinking_collapse_count, SIG_THINKING_COLLAPSE);
                    if (p > 0.0) {
                        state.coherence_score -= p;
                        state.signals_fired_this_turn++;
                    }
                }
            }
        }
    }

    // Signal 12: Context growth — detect prompt bloat (input tokens exceeding baseline)
    // As context grows, high-signal tokens get diluted, degrading all other signals.
    if (config_->signals.context_growth) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && ev.input_tokens > 0) {
                state.input_tokens_history.push_back(ev.input_tokens);
                if (static_cast<int>(state.input_tokens_history.size()) >
                    config_->thresholds.coherence_history_size) {
                    state.input_tokens_history.pop_front();
                }
            }
        }
        // Set or update baseline mean. Initial set when baseline completes or enough history.
        // Rolling EMA update when adaptive baseline active — tracks natural context growth.
        if (!state.input_tokens_history.empty()) {
            bool should_update = false;
            if (state.input_tokens_baseline_mean < 0.0) {
                should_update = (state.baseline_complete ||
                    static_cast<int>(state.input_tokens_history.size()) >=
                    std::max(3, config_->adaptive_baseline_window));
            } else if (config_->adaptive_baseline_enabled && state.baseline_complete) {
                should_update = true;
            }
            if (should_update) {
                double sum = 0.0;
                for (int t : state.input_tokens_history) sum += t;
                double window_mean = sum / static_cast<double>(state.input_tokens_history.size());
                if (state.input_tokens_baseline_mean < 0.0) {
                    state.input_tokens_baseline_mean = window_mean;
                } else {
                    // EMA alpha=0.1: baseline doubles after ~7 cycles at 2x sustained growth.
                    // Slow adaptation catches sudden bloat while allowing natural growth.
                    state.input_tokens_baseline_mean =
                        0.9 * state.input_tokens_baseline_mean + 0.1 * window_mean;
                }
            }
        }
        // Fire when current input tokens exceed baseline by factor
        if (state.input_tokens_baseline_mean > 0.0 && !state.input_tokens_history.empty()) {
            int current = state.input_tokens_history.back();
            if (static_cast<double>(current) >
                state.input_tokens_baseline_mean * config_->thresholds.context_growth_factor) {
                state.context_growth_count++;
                turn_fired[SIG_CONTEXT_GROWTH]++;
                if (!in_baseline) {
                    double p = adaptive_penalty(config_->weights.context_growth,
                                                state.context_growth_count, SIG_CONTEXT_GROWTH);
                    if (p > 0.0) {
                        state.coherence_score -= p;
                        state.signals_fired_this_turn++;
                    }
                }
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
                        turn_fired[SIG_SEMANTIC_STABILITY]++;
                        if (!in_baseline) {
                            double p = adaptive_penalty(config_->weights.semantic_stability,
                                                        state.semantic_stability_count, SIG_SEMANTIC_STABILITY);
                            if (p > 0.0) {
                                state.coherence_score -= p;
                                state.signals_fired_this_turn++;
                            }
                        }
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
                    turn_fired[SIG_MANDATE_ALIGNMENT]++;
                    if (!in_baseline) {
                        double p = adaptive_penalty(config_->weights.mandate_alignment,
                                                    state.mandate_drift_count, SIG_MANDATE_ALIGNMENT);
                        if (p > 0.0) {
                            state.coherence_score -= p;
                            state.signals_fired_this_turn++;
                        }
                    }
                }
                break;
            }
        }
    }

    // Signal 13: Instruction recall — check if agent references earlier user instructions
    if (config_->signals.instruction_recall && !state.instruction_keywords.empty()) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && !ev.content_keywords.empty()) {
                int found = 0;
                for (const auto& kw : state.instruction_keywords) {
                    if (ev.content_keywords.count(kw)) found++;
                }
                double recall = static_cast<double>(found) /
                                static_cast<double>(state.instruction_keywords.size());
                if (recall < config_->thresholds.instruction_recall_min) {
                    state.instruction_recall_count++;
                    turn_fired[SIG_INSTRUCTION_RECALL]++;
                    if (!in_baseline) {
                        double p = adaptive_penalty(config_->weights.instruction_recall,
                                                    state.instruction_recall_count, SIG_INSTRUCTION_RECALL);
                        if (p > 0.0) {
                            state.coherence_score -= p;
                            state.signals_fired_this_turn++;
                        }
                    }
                }
                break;
            }
        }
    }

    // Signal 14: Plan drift — detect divergence from stated multi-step plan
    // Plan steps are extracted by extractPlanFromResponse() called from agent_impl.cpp.
    // Here we only check which step the current response addresses.
    if (config_->signals.plan_drift && !state.plan_step_keywords.empty()) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && !ev.content_keywords.empty()) {
                {
                    // Find the step with highest keyword overlap
                    int best_step = -1;
                    double best_overlap = 0.0;
                    for (size_t i = 0; i < state.plan_step_keywords.size(); i++) {
                        const auto& step_kw = state.plan_step_keywords[i];
                        if (step_kw.empty()) continue;
                        int found = 0;
                        for (const auto& kw : step_kw) {
                            if (ev.content_keywords.count(kw)) found++;
                        }
                        double overlap = static_cast<double>(found) /
                                         static_cast<double>(step_kw.size());
                        if (overlap > best_overlap) {
                            best_overlap = overlap;
                            best_step = static_cast<int>(i);
                        }
                    }

                    bool drifted = false;
                    if (best_overlap >= config_->thresholds.plan_step_overlap_min && best_step >= 0) {
                        // Regression: went back to an earlier step
                        if (best_step < state.plan_last_step_matched) {
                            drifted = true;
                        }
                        // Skip: jumped forward by more than 1 step
                        if (best_step > state.plan_last_step_matched + 1 &&
                            state.plan_last_step_matched >= 0) {
                            drifted = true;
                        }
                        state.plan_last_step_matched = best_step;
                    } else if (state.plan_last_step_matched >= 0) {
                        // No step matched above threshold — agent abandoned plan
                        drifted = true;
                    }

                    if (drifted) {
                        state.plan_drift_count++;
                        turn_fired[SIG_PLAN_DRIFT]++;
                        if (!in_baseline) {
                            double p = adaptive_penalty(config_->weights.plan_drift,
                                                        state.plan_drift_count, SIG_PLAN_DRIFT);
                            if (p > 0.0) {
                                state.coherence_score -= p;
                                state.signals_fired_this_turn++;
                            }
                        }
                    }
                }
                break;
            }
        }
    }

    // Signal 15: Entity consistency — detect contradictory entity-context associations
    if (config_->signals.entity_consistency) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && !ev.content_keywords.empty()) {
                auto current_entities = extractEntityContext(ev.content_keywords);
                bool contradiction_found = false;
                for (const auto& [entity, current_ctx] : current_entities) {
                    auto it = state.entity_context.find(entity);
                    if (it != state.entity_context.end() && !it->second.empty() &&
                        !current_ctx.empty()) {
                        // Compute Jaccard between historical and current context
                        size_t intersection = 0;
                        for (const auto& kw : current_ctx) {
                            if (it->second.count(kw)) intersection++;
                        }
                        size_t union_size = it->second.size() + current_ctx.size() - intersection;
                        double overlap = union_size > 0 ?
                            static_cast<double>(intersection) / static_cast<double>(union_size) : 1.0;
                        if (overlap < config_->thresholds.entity_context_min_overlap) {
                            contradiction_found = true;
                            break;
                        }
                    }
                    // Merge current context into historical (accumulate, don't replace)
                    auto& hist = state.entity_context[entity];
                    for (const auto& kw : current_ctx) hist.insert(kw);
                }
                if (contradiction_found) {
                    state.entity_consistency_count++;
                    turn_fired[SIG_ENTITY_CONSISTENCY]++;
                    if (!in_baseline) {
                        double p = adaptive_penalty(config_->weights.entity_consistency,
                                                    state.entity_consistency_count, SIG_ENTITY_CONSISTENCY);
                        if (p > 0.0) {
                            state.coherence_score -= p;
                            state.signals_fired_this_turn++;
                        }
                    }
                }
                break;
            }
        }
    }

    // Signal 16: Instruction conflict — detect contradictory user instructions
    // Checks if the latest instruction in the history conflicts with any prior one.
    // Conflict = high topic keyword overlap + negation marker present.
    if (config_->signals.instruction_conflict && state.instruction_history.size() >= 2) {
        const auto& latest = state.instruction_history.back();
        bool conflict_found = false;
        // Check latest against all prior instructions in window
        for (size_t i = 0; i + 1 < state.instruction_history.size(); i++) {
            const auto& prior = state.instruction_history[i];
            if (prior.empty() || latest.empty()) continue;
            // Compute topic overlap (Jaccard)
            size_t intersection = 0;
            for (const auto& kw : latest) {
                if (prior.count(kw)) intersection++;
            }
            size_t union_size = prior.size() + latest.size() - intersection;
            double overlap = union_size > 0 ?
                static_cast<double>(intersection) / static_cast<double>(union_size) : 0.0;
            // High topic overlap + negation marker = contradiction
            if (overlap >= config_->thresholds.instruction_conflict_topic_overlap &&
                (hasNegationMarker(latest) || hasNegationMarker(prior))) {
                conflict_found = true;
                break;
            }
        }
        if (conflict_found) {
            state.instruction_conflict_count++;
            turn_fired[SIG_INSTRUCTION_CONFLICT]++;
            if (!in_baseline) {
                double p = adaptive_penalty(config_->weights.instruction_conflict,
                                            state.instruction_conflict_count, SIG_INSTRUCTION_CONFLICT);
                if (p > 0.0) {
                    state.coherence_score -= p;
                    state.signals_fired_this_turn++;
                }
            }
        }
    }

    // Signal 17: Persona fingerprint — detect linguistic style shifts
    if (config_->signals.persona_fingerprint) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && !ev.content_keywords.empty()) {
                int kw_count = static_cast<int>(ev.content_keywords.size());
                state.response_keyword_counts.push_back(kw_count);
                // Use same window as coherence history
                int window = config_->thresholds.thinking_history_window;
                while (static_cast<int>(state.response_keyword_counts.size()) > window)
                    state.response_keyword_counts.pop_front();

                // Establish baseline when adaptive baseline completes
                if (state.baseline_complete && state.persona_baseline_mean < 0.0 &&
                    state.response_keyword_counts.size() >= 3) {
                    double sum = 0, sq_sum = 0;
                    for (int c : state.response_keyword_counts) {
                        sum += c;
                        sq_sum += static_cast<double>(c) * c;
                    }
                    double n = static_cast<double>(state.response_keyword_counts.size());
                    state.persona_baseline_mean = sum / n;
                    state.persona_baseline_stddev = std::sqrt(
                        std::max(0.0, sq_sum / n - (sum / n) * (sum / n)));
                    if (state.persona_baseline_stddev < 1.0)
                        state.persona_baseline_stddev = 1.0;  // floor to prevent zero-div
                }

                // Fire when current deviates from baseline by > factor * stddev
                if (state.persona_baseline_mean >= 0.0) {
                    double deviation = std::abs(static_cast<double>(kw_count) -
                                                state.persona_baseline_mean);
                    if (deviation > config_->thresholds.persona_deviation_factor *
                                    state.persona_baseline_stddev) {
                        state.persona_drift_count++;
                        turn_fired[SIG_PERSONA_FINGERPRINT]++;
                        if (!in_baseline) {
                            double p = adaptive_penalty(config_->weights.persona_fingerprint,
                                                        state.persona_drift_count, SIG_PERSONA_FINGERPRINT);
                            if (p > 0.0) {
                                state.coherence_score -= p;
                                state.signals_fired_this_turn++;
                            }
                        }
                    }
                }
                break;
            }
        }
    }

    // Signal 18: Tool chain integrity — detect tool result misrepresentation
    // Compares tool result keywords stored by recordToolResult() against response keywords.
    // If agent references a tool but its response keywords have low overlap with actual
    // tool results, the agent may be fabricating or misremembering tool output.
    if (config_->signals.tool_chain_integrity && !state.tool_result_keywords.empty()) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && !ev.content_keywords.empty()) {
                // Check each tool's result keywords against response
                bool misrepresented = false;
                for (const auto& [tool_name, result_kw] : state.tool_result_keywords) {
                    if (result_kw.empty()) continue;
                    // Only check if the response mentions the tool name
                    if (!ev.content_keywords.count(tool_name)) continue;
                    // Compute recall: how many result keywords appear in response
                    int found = 0;
                    for (const auto& kw : result_kw) {
                        if (ev.content_keywords.count(kw)) found++;
                    }
                    double recall = static_cast<double>(found) /
                                    static_cast<double>(result_kw.size());
                    if (recall < config_->thresholds.tool_result_recall_min) {
                        misrepresented = true;
                        break;
                    }
                }
                if (misrepresented) {
                    state.tool_integrity_count++;
                    turn_fired[SIG_TOOL_CHAIN_INTEGRITY]++;
                    if (!in_baseline) {
                        double p = adaptive_penalty(config_->weights.tool_chain_integrity,
                                                    state.tool_integrity_count, SIG_TOOL_CHAIN_INTEGRITY);
                        if (p > 0.0) {
                            state.coherence_score -= p;
                            state.signals_fired_this_turn++;
                        }
                    }
                }
                break;
            }
        }
    }

    // Signal 19: Claim-result reconciliation — detect tool outcome misrepresentation.
    // Checks whether agent response claims success/error that contradicts actual tool outcome.
    // Unlike Signal 18 (keyword recall), this detects STATUS misrepresentation:
    // agent says "successfully updated" when tool returned an error.
    if (config_->signals.claim_result_reconciliation && !state.tool_last_outcome.empty()) {
        for (const auto& ev : turn_events) {
            if (ev.type == RuntimeEventType::AGENT_RESPONSE && !ev.content_keywords.empty()) {
                bool mismatch_detected = false;
                int tools_checked = 0;
                int tools_matched = 0;

                static const std::vector<std::string> success_words = {
                    "success", "succeeded", "completed", "done", "created",
                    "wrote", "saved", "passed", "found", "returned", "worked"
                };
                static const std::vector<std::string> failure_words = {
                    "error", "failed", "failure", "exception", "unable",
                    "could", "cannot", "denied", "blocked", "timeout"
                };

                for (const auto& [tool_name, succeeded] : state.tool_last_outcome) {
                    // Only check if response mentions the tool
                    std::string tool_lower = tool_name;
                    for (auto& c : tool_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (!ev.content_keywords.count(tool_lower)) continue;
                    tools_checked++;

                    bool claims_success = false;
                    bool claims_failure = false;
                    for (const auto& w : success_words) {
                        if (ev.content_keywords.count(w)) { claims_success = true; break; }
                    }
                    for (const auto& w : failure_words) {
                        if (ev.content_keywords.count(w)) { claims_failure = true; break; }
                    }

                    // Mismatch: claims success but tool failed (and no failure words),
                    // OR claims failure but tool succeeded (and no success words).
                    // Ambiguous cases (both or neither) do NOT fire.
                    if ((claims_success && !succeeded && !claims_failure) ||
                        (claims_failure && succeeded && !claims_success)) {
                        mismatch_detected = true;
                    } else {
                        tools_matched++;
                    }
                }

                // Track rolling accuracy
                if (tools_checked > 0) {
                    double accuracy = static_cast<double>(tools_matched) /
                                      static_cast<double>(tools_checked);
                    state.claim_accuracy_history.push_back(accuracy);
                    while (state.claim_accuracy_history.size() > 20)
                        state.claim_accuracy_history.pop_front();
                }

                if (mismatch_detected) {
                    state.claim_result_mismatch_count++;
                    turn_fired[SIG_CLAIM_RESULT]++;
                    if (!in_baseline) {
                        double p = adaptive_penalty(config_->weights.claim_result_reconciliation,
                                                    state.claim_result_mismatch_count, SIG_CLAIM_RESULT);
                        if (p > 0.0) {
                            state.coherence_score -= p;
                            state.signals_fired_this_turn++;
                        }
                    }
                }
                break;
            }
        }
        // Clear tool outcomes after reconciliation (per-turn check, not cumulative)
        state.tool_last_outcome.clear();
    }

    // Adaptive baseline: accumulate per-turn signal counts during window
    if (config_->adaptive_baseline_enabled) {
        if (!state.baseline_complete) {
            for (int i = 0; i < NUM_CDD_SIGNALS; i++) {
                state.signal_baselines[i].sum += turn_fired[i];
                state.signal_baselines[i].sum_sq += static_cast<double>(turn_fired[i]) * turn_fired[i];
            }
            state.baseline_turns_counted++;

            if (state.baseline_turns_counted >= config_->adaptive_baseline_window) {
                double n = static_cast<double>(state.baseline_turns_counted);
                auto computeStddev = [n](double sum_sq, double mean) -> double {
                    double variance = (n > 1.0)
                        ? ((sum_sq / n) - (mean * mean)) * (n / (n - 1.0))
                        : 0.0;
                    return (variance > 0.0) ? std::sqrt(variance) : 0.0;
                };
                for (int i = 0; i < NUM_CDD_SIGNALS; i++) {
                    state.signal_baselines[i].mean = state.signal_baselines[i].sum / n;
                    state.signal_baselines[i].stddev = computeStddev(
                        state.signal_baselines[i].sum_sq, state.signal_baselines[i].mean);
                }
                state.baseline_complete = true;
                state.baseline_completed_turn = turn_number;

                // Snapshot cumulative counters (17 signals with counters; S6/S7 stay at 0)
                state.signal_baselines[SIG_CIRCULAR].snapshot = state.circular_action_count;
                state.signal_baselines[SIG_REPEATED_FAILURES].snapshot = state.repeated_failures;
                state.signal_baselines[SIG_SCOPE_CREEP].snapshot = state.scope_creep_count;
                state.signal_baselines[SIG_CONTRADICTIONS].snapshot = state.contradictions;
                state.signal_baselines[SIG_VOCAB_CONTRACTION].snapshot = state.vocabulary_contraction_count;
                state.signal_baselines[SIG_RESPONSE_QUALITY].snapshot = state.response_quality_count;
                state.signal_baselines[SIG_THINKING_COLLAPSE].snapshot = state.thinking_collapse_count;
                state.signal_baselines[SIG_SEMANTIC_STABILITY].snapshot = state.semantic_stability_count;
                state.signal_baselines[SIG_MANDATE_ALIGNMENT].snapshot = state.mandate_drift_count;
                state.signal_baselines[SIG_CONTEXT_GROWTH].snapshot = state.context_growth_count;
                state.signal_baselines[SIG_INSTRUCTION_RECALL].snapshot = state.instruction_recall_count;
                state.signal_baselines[SIG_PLAN_DRIFT].snapshot = state.plan_drift_count;
                state.signal_baselines[SIG_ENTITY_CONSISTENCY].snapshot = state.entity_consistency_count;
                state.signal_baselines[SIG_INSTRUCTION_CONFLICT].snapshot = state.instruction_conflict_count;
                state.signal_baselines[SIG_PERSONA_FINGERPRINT].snapshot = state.persona_drift_count;
                state.signal_baselines[SIG_TOOL_CHAIN_INTEGRITY].snapshot = state.tool_integrity_count;
                state.signal_baselines[SIG_CLAIM_RESULT].snapshot = state.claim_result_mismatch_count;
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

    // Escalation effectiveness: accumulate post-escalation coherence readings
    if (state.escalation_turn >= 0 &&
        state.post_escalation_turns_counted < config_->escalation_effectiveness_window) {
        state.post_escalation_coherence_sum += state.coherence_score;
        state.post_escalation_turns_counted++;
    }

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
        // Flat additive recovery, capped at coherence_recovery_cap.
        // Penalties are additive (weight * count), so recovery must also be
        // additive to maintain arithmetic balance across lease cycles.
        // The cap prevents overshooting — no diminishing returns needed.
        double cap = config_ ? config_->coherence_recovery_cap : 1.0;
        double old = it->second.coherence_score;
        if (old < cap) {
            it->second.coherence_score = std::min(cap, old + amount);
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

void ContextDriftAnalyzer::addInstructionKeywords(
    int handle_id, const std::unordered_set<std::string>& keywords) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = drift_states_[handle_id];
    for (const auto& kw : keywords) {
        state.instruction_keywords.insert(kw);
    }
    // Also store per-turn keyword set for instruction conflict detection
    if (config_ && config_->signals.instruction_conflict && !keywords.empty()) {
        state.instruction_history.push_back(keywords);
        int window = config_->thresholds.instruction_conflict_window;
        while (static_cast<int>(state.instruction_history.size()) > window) {
            state.instruction_history.pop_front();
        }
    }
}

void ContextDriftAnalyzer::extractPlanFromResponse(
    int handle_id, const std::string& response_text) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = drift_states_[handle_id];
    if (state.plan_extracted) return;  // only attempt once
    state.plan_extracted = true;
    auto steps = extractPlanSteps(response_text);
    if (!steps.empty()) {
        state.plan_step_keywords = std::move(steps);
    }
}

void ContextDriftAnalyzer::recordToolResult(
    int handle_id, const std::string& tool_name,
    const std::unordered_set<std::string>& result_keywords) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = drift_states_[handle_id];
    // Store latest result keywords per tool (replace, don't merge — results are per-call)
    state.tool_result_keywords[tool_name] = result_keywords;
}

void ContextDriftAnalyzer::recordToolOutcome(
    int handle_id, const std::string& tool_name, bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = drift_states_[handle_id];
    state.tool_last_outcome[tool_name] = success;
}

void ContextDriftAnalyzer::recordEscalation(
    int handle_id, int from_level, int to_level) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = drift_states_.find(handle_id);
    if (it != drift_states_.end()) {
        it->second.escalation_turn = it->second.last_checked_turn;
        it->second.escalation_from_level = from_level;
        it->second.escalation_to_level = to_level;
        it->second.escalation_coherence_at = it->second.coherence_score;
        it->second.post_escalation_coherence_sum = 0.0;
        it->second.post_escalation_turns_counted = 0;
    }
}

} // namespace governance
} // namespace naab
