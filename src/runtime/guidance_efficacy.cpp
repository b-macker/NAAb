// Guidance efficacy: see include/naab/guidance_efficacy.h.

#include "naab/guidance_efficacy.h"

#include "naab/governance.h"   // the CDD signal names (behavioral_sequence.h is not self-contained)
#include "naab/crypto_utils.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace naab {
namespace guidance {

double holdoutDraw(const std::string& run_id, unsigned long long ordinal) {
    const std::string h = naab::security::CryptoUtils::sha256(run_id + ":" + std::to_string(ordinal));
    if (h.size() < 13) return 1.0;   // never held out on a hash this cannot read
    unsigned long long v = 0;
    for (size_t i = 0; i < 13; ++i) {
        const char c = h[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) return 1.0;
        v = v * 16 + static_cast<unsigned long long>(d);
    }
    return static_cast<double>(v) / 4503599627370496.0;   // 16^13
}

const std::vector<std::pair<const char*, const char*>>& eventClassTable() {
    // "intervention": changes what an agent is told, or the state it is judged
    // in, inside a delivery's window. Everything else only observes.
    static const std::vector<std::pair<const char*, const char*>> t = {
        {"ADMISSION_EVAL", "observation"},
        {"AGENT_CHALLENGE_FAIL", "intervention"},     // a challenge prompt was sent
        {"AGENT_CHALLENGE_PASS", "intervention"},     // ... and a pass recovers coherence
        {"AGENT_CHALLENGE_SKIPPED", "observation"},   // no challenge reached the agent
        {"AGENT_FALLBACK", "observation"},            // a model change: MIXED_MODEL judges it
        {"AGENT_HARD_STOP", "observation"},
        {"AGENT_KEY_DISABLED", "observation"},
        {"AGENT_KEY_REVIVED", "observation"},
        {"AGENT_KEY_THROTTLED", "observation"},
        {"AGENT_PROPOSAL_COMMIT", "observation"},
        {"AGENT_PROPOSE", "observation"},
        {"AGENT_RESET", "intervention"},              // the handle's history is cleared
        {"AGENT_RESPONSE", "observation"},
        {"AGENT_RETRY", "observation"},
        {"AGENT_TOOL_BLOCKED", "observation"},
        {"AGENT_TOOL_CALL", "observation"},
        {"AGENT_TOOL_LOOP_END", "observation"},
        {"AGENT_TOOL_LOOP_START", "observation"},
        {"AGENT_TOOL_REGISTERED", "observation"},
        {"AGENT_TOOL_RESULT", "observation"},
        {"BSD_MATCH", "observation"},
        {"CAPABILITY_VIOLATION", "observation"},
        {"CDD_TURN", "observation"},
        {"CODEGEN_EXEC", "observation"},
        {"CONFIG_ADJUSTMENT", "intervention"},        // an accepted reload changes the rules
        {"CONTRACT_VIOLATION", "observation"},
        {"CONVERGENCE_CHECK", "observation"},
        {"GOVERNANCE_HEALTH_QUERY", "observation"},
        {"GOVERNANCE_HEALTH_WARNING", "observation"},
        {"GOVERNANCE_LEVEL_CHANGE", "observation"},
        {"GUIDANCE_DELIVERED", "intervention"},       // another delivery to the handle
        {"GUIDANCE_RESPONSE", "observation"},
        {"GUIDANCE_SOURCE_REFRESH", "observation"},
        {"GUIDANCE_VALIDATION", "observation"},
        {"GUIDANCE_WITHHELD", "observation"},
        {"HISTORY_LOADED", "observation"},
        {"HISTORY_RESET", "observation"},
        {"MANDATE_INJECTION", "intervention"},        // a reminder or a correction was prepended
        {"OUTPUT_ADMISSIBILITY_EVAL", "observation"},
        {"OUTPUT_INADMISSIBLE", "observation"},
        {"POLYGLOT_EXEC", "observation"},
        {"PROMPT_SCAN", "observation"},
        {"PULSE_TRANSITION", "observation"},
        {"QUARANTINE_STREAK_EXCEEDED", "observation"},
        {"QUARANTINE_UNCORROBORATED", "observation"},
        {"RECONCILIATION_TURN", "observation"},
        {"RESPONSE_SCAN", "observation"},
        {"RESPONSE_SUPPRESSED", "observation"},
        {"RESPONSE_TRUNCATED", "observation"},
        {"SEMANTIC_TURN", "observation"},
        {"SIGNAL_INERT", "observation"},
        {"THINKING_UNREPORTED", "observation"},
        {"TRANSCRIPT_REF", "observation"},
        {"VALIDATION_RECORDED", "observation"},       // the outcome of a validation trigger
        {"VALIDATION_SCORED_AT_EXIT", "observation"},
    };
    return t;
}

const char* eventClass(const std::string& event_type) {
    for (const auto& [k, v] : eventClassTable())
        if (event_type == k) return v;
    return nullptr;
}

bool judgedEventType(const std::string& type) {
    if (type == "GUIDANCE_WITHHELD" || type == "CDD_TURN" || type == "VALIDATION_RECORDED") return true;
    const char* c = eventClass(type);
    return c && std::string(c) == "intervention";
}

namespace {

std::string field(const Event& e, const char* k) {
    auto it = e.f.find(k);
    return it == e.f.end() ? std::string() : it->second;
}

int intField(const Event& e, const char* k) {
    const std::string s = field(e, k);
    if (s.empty() || s.size() > 9) return -1;
    for (char c : s) if (c < '0' || c > '9') return -1;
    return std::atoi(s.c_str());
}

// The name a CDD_TURN's penalties_detail gives signal `key`: the telemetry
// label, which differs from the config key for S1-S7 and S19.
std::string paidLabel(const std::string& key) {
    for (int i = 0; i < governance::NUM_CDD_SIGNALS; ++i)
        if (key == governance::kCddSignalKeys[i])
            return governance::ContextDriftAnalyzer::signalName(i);
    return key;
}

bool paidOn(const Event& turn, const std::string& label) {
    const std::string pd = field(turn, "penalties_detail");
    size_t pos = 0;
    while (pos <= pd.size()) {
        size_t comma = pd.find(',', pos);
        if (comma == std::string::npos) comma = pd.size();
        const std::string item = pd.substr(pos, comma - pos);
        const size_t eq = item.find('=');
        if (item.substr(0, eq) == label) return true;
        pos = comma + 1;
    }
    return false;
}

// Something else done to this handle inside the window.
bool intervenes(const Event& ev, const Event& subject) {
    const char* c = eventClass(ev.type);
    if (!c || std::string(c) != "intervention") return false;
    if (ev.run != subject.run) return false;
    if (ev.type == "CONFIG_ADJUSTMENT") return field(ev, "accepted") == "true";   // run-wide
    if (ev.type == "GUIDANCE_DELIVERED") {
        const std::string ch = field(ev, "channel");
        if (ch == "terminal") return false;             // shown at the terminal, not to an agent
        if (ch == "agent") {
            if (ev.handle != subject.handle) return false;
            // Delivered with the same request: one intervention, not two.
            return field(ev, "turn") != field(subject, "turn");
        }
        return true;   // the script pulled guidance: it may pass it to any agent
    }
    // An intervention with no handle is taken as run-wide (the conservative
    // reading: undetermined rather than a false quiet).
    return ev.handle.empty() || ev.handle == subject.handle;
}

}  // namespace

std::vector<Outcome> judge(const std::vector<Event>& events) {
    std::vector<Outcome> out;
    for (size_t i = 0; i < events.size(); ++i) {
        const Event& e = events[i];
        Outcome o;
        if (e.type == "GUIDANCE_DELIVERED" && field(e, "channel") == "agent") {
            o.arm = "delivered";
            o.id = field(e, "delivery_id");
        } else if (e.type == "GUIDANCE_WITHHELD" && field(e, "reason") == "holdout" &&
                   field(e, "channel") == "agent") {
            o.arm = "held_out";
            o.id = "h" + field(e, "holdout_ordinal");
        } else {
            continue;
        }
        o.run = e.run;
        o.handle = e.handle;
        o.trigger = field(e, "trigger");
        o.section_key = field(e, "section_key");
        o.source = field(e, "source");
        o.heading = field(e, "heading");
        o.turn = intField(e, "turn");
        if (o.handle.empty() || o.turn < 0) { o.result = "not_judged"; out.push_back(o); continue; }

        if (o.trigger.rfind("signal:", 0) == 0) {
            const std::string label = paidLabel(o.trigger.substr(7));
            std::set<std::string> models;
            bool intervened = false;
            for (size_t j = i + 1; j < events.size() && o.result.empty(); ++j) {
                const Event& ev = events[j];
                if (ev.run != e.run) continue;
                if (ev.type == "CDD_TURN") {
                    if (ev.handle != e.handle || intField(ev, "turn") < o.turn) continue;
                    if (field(ev, "analyzed") == "false") continue;   // an interval-skipped turn re-shows stale state
                    if (intervened) { o.result = "undetermined"; break; }
                    ++o.window_turns;
                    const std::string m = field(ev, "model");
                    if (!m.empty()) { models.insert(m); if (o.model.empty()) o.model = m; }
                    if (paidOn(ev, label)) o.result = "recurred";
                    else if (o.window_turns >= kWindowTurns) o.result = "quiet";
                } else if (intervenes(ev, e)) {
                    intervened = true;
                }
            }
            if (o.result.empty()) o.result = "incomplete";
            if (models.size() > 1 && o.result != "undetermined") o.result = "mixed_model";
        } else if (o.trigger == "validation_failed") {
            bool intervened = false;
            for (size_t j = i + 1; j < events.size() && o.result.empty(); ++j) {
                const Event& ev = events[j];
                if (ev.run != e.run) continue;
                if (ev.type == "VALIDATION_RECORDED" && ev.handle == e.handle) {
                    o.result = intervened ? "undetermined"
                             : field(ev, "passed") == "true" ? "resolved" : "recurred";
                } else if (intervenes(ev, e)) {
                    intervened = true;
                }
            }
            if (o.result.empty()) o.result = "incomplete";
        } else {
            o.result = "not_judged";
        }
        out.push_back(o);
    }
    return out;
}

namespace {

bool hex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

}  // namespace

void addOutcomes(std::vector<SectionTally>& tallies, const std::vector<Outcome>& outcomes) {
    for (const auto& o : outcomes) {
        if (!hex32(o.section_key)) continue;
        auto it = std::find_if(tallies.begin(), tallies.end(),
                               [&](const SectionTally& t) { return t.key == o.section_key; });
        SectionTally t;
        if (it != tallies.end()) { t = *it; tallies.erase(it); }
        else t.key = o.section_key;
        if (o.arm == "held_out") {
            ++t.held_out;
        } else {
            ++t.delivered;
            if (o.result == "recurred") ++t.recurred;
            else if (o.result == "quiet") ++t.quiet;
            else if (o.result == "resolved") ++t.resolved;
            else if (o.result == "undetermined") ++t.undetermined;
            else if (o.result == "incomplete") ++t.incomplete;
            else if (o.result == "mixed_model") ++t.mixed_model;
            else ++t.not_judged;
        }
        tallies.push_back(t);   // most recently delivered last
    }
    if (tallies.size() > kMaxTallies)
        tallies.erase(tallies.begin(), tallies.begin() + static_cast<std::ptrdiff_t>(tallies.size() - kMaxTallies));
}

void mergeTallies(std::vector<SectionTally>& into, const std::vector<SectionTally>& other) {
    for (const auto& o : other) {
        auto it = std::find_if(into.begin(), into.end(),
                               [&](const SectionTally& t) { return t.key == o.key; });
        if (it == into.end()) { into.push_back(o); continue; }
        auto mx = [](int& a, int b) { a = std::max(a, b); };
        mx(it->delivered, o.delivered); mx(it->held_out, o.held_out);
        mx(it->recurred, o.recurred); mx(it->quiet, o.quiet); mx(it->resolved, o.resolved);
        mx(it->undetermined, o.undetermined); mx(it->incomplete, o.incomplete);
        mx(it->mixed_model, o.mixed_model); mx(it->not_judged, o.not_judged);
    }
    if (into.size() > kMaxTallies)
        into.erase(into.begin(), into.begin() + static_cast<std::ptrdiff_t>(into.size() - kMaxTallies));
}

bool plausibleTally(const SectionTally& t) {
    if (!hex32(t.key)) return false;
    const int v[] = {t.delivered, t.held_out, t.recurred, t.quiet, t.resolved,
                     t.undetermined, t.incomplete, t.mixed_model, t.not_judged};
    for (int x : v) if (x < 0 || x > 1000000) return false;
    return t.judged() + t.undetermined + t.incomplete + t.mixed_model + t.not_judged <= t.delivered;
}

bool neverResolved(const SectionTally& t) {
    return t.judged() >= kUnresolvedLineMin && t.recurred == t.judged();
}

}  // namespace guidance
}  // namespace naab
