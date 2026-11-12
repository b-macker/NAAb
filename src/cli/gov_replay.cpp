// naab-gov replay TELEMETRY -- checks recorded decisions against their
// recorded evidence (Bundle 14 PR 2).
//
// It does NOT re-execute anything. It reads a telemetry file (and, when given,
// the agent transcript) and asks of each decision the file records whether the
// evidence recorded beside it agrees:
//   chain          each run's hash chain, by the same function
//                  --verify-telemetry-chain uses (analyzeTelemetryChain), with
//                  the same key source
//   admissibility  an OUTPUT_ADMISSIBILITY_EVAL's result and disposition follow
//                  from its own coherence, threshold, baseline state and
//                  configured actions, and its coherence is the one the turn's
//                  CDD_TURN or its cdd_snapshot records
//   coherence      each analysed CDD_TURN's coherence is the previous analysed
//                  one for that handle, less the listed penalties, plus the
//                  listed credits and adjustments
//   transcript     each TRANSCRIPT_REF's entry_hash is the hash of an entry in
//                  the transcript
//   provenance     code an execution event attributes to a model reply names a
//                  reply an AGENT_RESPONSE of the same run recorded
//
// Each is MATCH, DIVERGED or UNMEASURABLE. A decision whose evidence is not in
// the files given -- no snapshot and no CDD_TURN, no transcript, a chain sealed
// with a key that was not given, a line too large to read -- is UNMEASURABLE,
// never MATCH.
//
// The files are untrusted input: they carry model-written text, and whoever
// can write them can write anything. Every line is read through
// naab/bounded_jsonl.h (a byte bound and a nesting bound), decision fields are
// read only as short strings and plain decimals, nothing read is executed or
// matched against a pattern, and text copied from the file into the report is
// cut to a short run of plain characters.
//
// Report only: it loads no govern.json, runs no program, prerequisite or hook,
// and opens no connection.
//
// Exit: 1 when anything DIVERGED; else 2 when anything is UNMEASURABLE (or the
// file holds nothing to check); else 0. 4 for a usage error or a file that
// cannot be opened.
#include "naab/bounded_jsonl.h"
#include "naab/crypto_utils.h"
#include "naab/governance.h"
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
using naab::governance::GovernanceEngine;

namespace {

enum class Verdict { Match, Diverged, Unmeasurable };

const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::Match: return "MATCH";
        case Verdict::Diverged: return "DIVERGED";
        case Verdict::Unmeasurable: return "UNMEASURABLE";
    }
    return "?";
}

struct Finding {
    std::string check;
    Verdict verdict;
    std::string run;      // already made plain
    long long line = 0;   // 0 = not about one line
    std::string detail;   // already made plain
};

// Text from the file, for the report: letters, digits and a few marks only,
// at most 64 bytes. Anything else becomes '?', so a line in the file cannot
// write escape sequences or forged report lines to the reader's terminal.
std::string plain(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (out.size() >= 64) { out += "..."; break; }
        unsigned char c = static_cast<unsigned char>(ch);
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
                  c == ':' || c == '+' || c == '=' || c == ',';
        out.push_back(ok ? ch : '?');
    }
    return out;
}

// A decision field: a string of at most 256 bytes. Longer, absent or of
// another type reads as absent.
bool field(const json& ev, const char* key, std::string& out) {
    auto it = ev.find(key);
    if (it == ev.end() || !it->is_string()) return false;
    const std::string& s = it->get_ref<const std::string&>();
    if (s.size() > 256) return false;
    out = s;
    return true;
}

// A plain decimal: [+-]digits[.digits], at most 24 bytes. No exponent, no
// names (nan, inf), no hex.
bool decimal(const std::string& s, double& out) {
    if (s.empty() || s.size() > 24) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') { neg = (s[0] == '-'); i = 1; }
    double v = 0.0;
    size_t digits = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++digits) v = v * 10.0 + (s[i] - '0');
    if (i < s.size() && s[i] == '.') {
        ++i;
        double scale = 0.1;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++digits) {
            v += (s[i] - '0') * scale;
            scale *= 0.1;
        }
    }
    if (i != s.size() || digits == 0) return false;
    out = neg ? -v : v;
    return true;
}

// "name=value,name=value": at most 64 terms. A value that is not a plain
// decimal carries no amount (validation_credit_withheld=evidence_shrank).
// Returns false when the list itself cannot be read.
bool sumTerms(const std::string& list, bool penalties, double& sum, int& count) {
    size_t pos = 0;
    int seen = 0;
    while (pos < list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        if (++seen > 64) return false;
        std::string term = list.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = term.find('=');
        if (eq == std::string::npos) continue;
        std::string value = term.substr(eq + 1);
        double v = 0.0;
        if (!decimal(value, v)) continue;
        // In penalties_detail an unsigned amount is a penalty and a "+" one a
        // credit; in coherence_adjustments every amount carries its own sign.
        if (penalties && value[0] != '+' && value[0] != '-') v = -v;
        sum += v;
        ++count;
    }
    return true;
}

struct RunState {
    std::unordered_map<std::string, double> prev_coherence;        // by handle
    std::unordered_set<std::string> prev_unknown;                   // after a reset
    std::unordered_map<std::string, std::string> turn_coherence;   // handle|turn -> printed
    std::string pulse;                                              // last to_verdict
    std::unordered_set<std::string> reply_hashes;                   // content_sha256
    std::unordered_set<std::string> replies;                        // the joined origin
};

std::string replyKey(const std::string& content, const std::string& request,
                     const std::string& model, const std::string& config,
                     const std::string& handle, const std::string& turn) {
    return content + "|" + request + "|" + model + "|" + config + "|" + handle + "|" + turn;
}

struct Totals {
    std::map<std::string, long long> match, diverged, unmeasurable;
    void add(const Finding& f) {
        (f.verdict == Verdict::Match ? match
         : f.verdict == Verdict::Diverged ? diverged : unmeasurable)[f.check]++;
    }
};

class Replay {
public:
    std::vector<Finding> findings;   // DIVERGED and UNMEASURABLE only
    Totals totals;

    void note(const std::string& check, Verdict v, const std::string& run,
              long long line, const std::string& detail) {
        Finding f{check, v, plain(run), line, detail};
        totals.add(f);
        if (v != Verdict::Match && findings.size() < 500) findings.push_back(f);
    }

    // --- chain ---------------------------------------------------------
    void chain(const GovernanceEngine::TelemetryChainReport& rep, bool key_given) {
        if (rep.chained == 0) {
            note("chain", Verdict::Unmeasurable, "", 0,
                 "the file holds no chained event: nothing here is sealed");
        }
        if (rep.corrupt > 0) {
            note("chain", Verdict::Diverged, "", 0,
                 std::to_string(rep.corrupt) + " line(s) are not readable as JSON events");
        }
        if (rep.skipped > 0) {
            note("chain", Verdict::Unmeasurable, "", 0,
                 std::to_string(rep.skipped) + " line(s) exceed the size or nesting "
                 "bound and were not read");
        }
        for (const auto& r : rep.runs) {
            if (r.link_breaks > 0 || r.count_mismatch) {
                std::string d;
                if (r.link_breaks > 0)
                    d = std::to_string(r.link_breaks) + " event(s) do not link to the one before";
                if (r.count_mismatch) {
                    if (!d.empty()) d += "; ";
                    d += "RunEnd declares " + std::to_string(r.declared) +
                         " chained events, " + std::to_string(r.chained) + " are present";
                }
                note("chain", Verdict::Diverged, r.run_id, 0, d);
            } else if (r.hash_mismatches > 0) {
                if (r.hash_mismatches == r.chained && r.chained >= 2) {
                    // Every event fails its hash and every link holds: what a
                    // chain sealed with a key looks like when verified without
                    // that key. It cannot be told from every event having been
                    // edited, so it is not a MATCH and not a named divergence.
                    note("chain", Verdict::Unmeasurable, r.run_id, 0,
                         key_given
                         ? "no event's hash verifies with the key given, and every "
                           "link holds: sealed with another key, or every event edited"
                         : "no event's hash verifies without a key, and every link "
                           "holds: sealed with a key that was not given, or every "
                           "event edited");
                } else {
                    note("chain", Verdict::Diverged, r.run_id, 0,
                         std::to_string(r.hash_mismatches) + " of " +
                         std::to_string(r.chained) + " event(s) fail their hash");
                }
            } else if (!r.has_end) {
                note("chain", Verdict::Unmeasurable, r.run_id, 0,
                     "the run has no RunEnd: events after the last one present "
                     "cannot be ruled out");
            } else {
                note("chain", Verdict::Match, r.run_id, 0, "");
            }
        }
    }

    // --- transcript ----------------------------------------------------
    bool transcript_given = false;
    bool transcript_partial = false;                 // a line was not read
    std::unordered_set<std::string> entry_hashes;    // of entries as written
    long long transcript_edited = 0;

    bool loadTranscript(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        transcript_given = true;
        std::string line;
        bool oversized = false;
        long long n = 0;
        while (naab::evidence::readBoundedLine(
                   in, line, naab::evidence::kMaxTranscriptLineBytes, oversized)) {
            if (!oversized && line.empty()) continue;
            ++n;
            if (oversized ||
                !naab::evidence::jsonNestingWithin(line, naab::evidence::kMaxJsonDepth)) {
                transcript_partial = true;
                continue;
            }
            // The writer hashes the entry before adding entry_hash; an entry it
            // could not parse is written, and hashed, as the bare line.
            std::string hash = naab::security::CryptoUtils::sha256(line);
            try {
                json j = json::parse(line);
                if (j.is_object() && j.contains("entry_hash") && j["entry_hash"].is_string()) {
                    std::string stored = j["entry_hash"].get<std::string>();
                    j.erase("entry_hash");
                    hash = naab::security::CryptoUtils::sha256(j.dump());
                    if (hash != stored) {
                        ++transcript_edited;
                        note("transcript", Verdict::Diverged, "", n,
                             "transcript entry does not hash to its own entry_hash");
                    }
                }
            } catch (const json::exception&) {
                // keep the bare line's hash
            }
            entry_hashes.insert(hash);
        }
        return true;
    }

    // --- decisions -----------------------------------------------------
    std::unordered_map<std::string, RunState> runs;

    void event(const json& ev, long long line) {
        std::string type, run;
        if (!field(ev, "event_type", type)) return;
        field(ev, "run_id", run);
        RunState& st = runs[run];
        if (type == "CDD_TURN") cddTurn(ev, st, run, line);
        else if (type == "OUTPUT_ADMISSIBILITY_EVAL") admissibility(ev, st, run, line);
        else if (type == "PULSE_TRANSITION") field(ev, "to_verdict", st.pulse);
        else if (type == "AGENT_RESET") {
            std::string h;
            if (field(ev, "handle_id", h)) st.prev_unknown.insert(h);
        } else if (type == "TRANSCRIPT_REF") transcriptRef(ev, run, line);
        else if (type == "AGENT_RESPONSE") agentResponse(ev, st);
        else if (type == "CODEGEN_EXEC" || type == "POLYGLOT_EXEC") execEvent(ev, st, run, line);
    }

    void cddTurn(const json& ev, RunState& st, const std::string& run, long long line) {
        std::string handle, turn, coh, analyzed, pen, adj;
        if (!field(ev, "handle_id", handle) || !field(ev, "coherence", coh)) return;
        field(ev, "analyzed", analyzed);
        if (analyzed != "true") return;   // an interval-skipped row re-shows old state
        if (field(ev, "turn", turn)) st.turn_coherence[handle + "|" + turn] = coh;
        double c = 0.0;
        if (!decimal(coh, c)) {
            note("coherence", Verdict::Unmeasurable, run, line,
                 "handle " + plain(handle) + ": the recorded coherence is not a number");
            st.prev_unknown.insert(handle);
            return;
        }
        // A writer from before coherence_adjustments existed listed penalties
        // only (healing, decay and recovery went unrecorded), so its rows do
        // not carry the evidence this check adds up.
        const bool lists_all = field(ev, "penalties_detail", pen) &&
                               field(ev, "coherence_adjustments", adj);
        bool unknown = st.prev_unknown.count(handle) > 0;
        double expected = 1.0;
        auto it = st.prev_coherence.find(handle);
        if (it != st.prev_coherence.end()) expected = it->second;
        int terms = 0;
        double delta = 0.0;
        bool readable = sumTerms(pen, true, delta, terms) && sumTerms(adj, false, delta, terms);
        st.prev_coherence[handle] = c;
        st.prev_unknown.erase(handle);
        if (unknown) {
            note("coherence", Verdict::Unmeasurable, run, line,
                 "handle " + plain(handle) + " turn " + plain(turn) +
                 ": the coherence it started from is not recorded (after a reset)");
            return;
        }
        if (!lists_all) {
            note("coherence", Verdict::Unmeasurable, run, line,
                 "handle " + plain(handle) + " turn " + plain(turn) +
                 ": the row does not record its penalties and adjustments");
            return;
        }
        if (!readable) {
            note("coherence", Verdict::Unmeasurable, run, line,
                 "handle " + plain(handle) + " turn " + plain(turn) +
                 ": the penalty list is too long to read");
            return;
        }
        expected += delta;
        // Every figure is printed to four places, so each carries half a unit
        // of the last place; 0.0006 is the bound the engine's own
        // reconciliation test pins.
        double tolerance = std::max(0.0006, 0.00005 * (2 + terms) + 1e-6);
        if (std::fabs(expected - c) <= tolerance) {
            note("coherence", Verdict::Match, run, line, "");
        } else {
            char buf[160];
            snprintf(buf, sizeof(buf), ": recorded %.4f, the listed penalties and "
                     "adjustments give %.4f", c, expected);
            note("coherence", Verdict::Diverged, run, line,
                 "handle " + plain(handle) + " turn " + plain(turn) + buf);
        }
    }

    void admissibility(const json& ev, RunState& st, const std::string& run, long long line) {
        std::string handle, turn, result, coh, thr, action, disposition;
        field(ev, "handle_id", handle);
        field(ev, "turn", turn);
        const std::string where = "handle " + plain(handle) + " turn " + plain(turn);
        double c = 0.0, t = 0.0;
        if (!field(ev, "result", result) || !field(ev, "coherence", coh) ||
            !field(ev, "threshold", thr) || !field(ev, "action", action) ||
            !field(ev, "disposition", disposition) || !decimal(coh, c) || !decimal(thr, t)) {
            note("admissibility", Verdict::Unmeasurable, run, line,
                 where + ": the event lacks a field the decision is made of");
            return;
        }
        // Both figures are printed to four places: equal printed values cannot
        // say which side of the threshold the score was on.
        const bool tie = std::fabs(c - t) < 0.00005;
        const bool above = !tie && c > t;
        std::string why;
        Verdict v = Verdict::Match;
        auto diverge = [&](const std::string& d) {
            if (v != Verdict::Diverged) { v = Verdict::Diverged; why = d; }
        };
        auto unmeasure = [&](const std::string& d) {
            if (v == Verdict::Match) { v = Verdict::Unmeasurable; why = d; }
        };

        if (result == "pass" || result == "undetermined") {
            std::string baseline, on_undetermined;
            field(ev, "baseline_state", baseline);
            field(ev, "on_undetermined", on_undetermined);
            if (!tie && !above) diverge("result " + plain(result) + " with coherence " +
                                        plain(coh) + " below threshold " + plain(thr));
            else if (tie) unmeasure("coherence equals the threshold as printed");
            if (result == "pass" && baseline == "calibrating")
                diverge("result pass while the baseline is calibrating");
            if (result == "undetermined" && baseline != "calibrating")
                diverge("result undetermined with baseline_state " + plain(baseline));
            std::string expected = "admitted";
            if (result == "undetermined" && on_undetermined != "pass")
                expected = on_undetermined == "block" ? "blocked" : "quarantined";
            if (disposition != expected)
                diverge("disposition " + plain(disposition) + ", the recorded result and "
                        "policy give " + expected);
        } else if (result == "fail") {
            std::string pulse_override;
            field(ev, "pulse_override", pulse_override);
            if (pulse_override == "true") {
                if (st.pulse != "IMPAIRED" && st.pulse != "impaired")
                    unmeasure("failed on the pulse verdict, and no PULSE_TRANSITION to "
                              "impaired precedes it in this run");
            } else {
                if (above) diverge("result fail with coherence " + plain(coh) +
                                   " above threshold " + plain(thr));
                else if (tie) unmeasure("coherence equals the threshold as printed");
            }
            std::string expected = action == "block" ? "blocked"
                                   : action == "attest" ? "attested" : "quarantined";
            if (disposition != expected)
                diverge("disposition " + plain(disposition) + ", the recorded action gives " +
                        expected);
        } else {
            unmeasure("result " + plain(result) + " is not one this version knows");
        }

        // The coherence the decision used, against where else it is recorded.
        bool evidence = false;
        auto snap = ev.find("cdd_snapshot");
        if (snap != ev.end() && snap->is_object()) {
            auto cs = snap->find("coherence_score");
            if (cs != snap->end() && cs->is_number()) {
                evidence = true;
                char buf[32];
                snprintf(buf, sizeof(buf), "%.4f", cs->get<double>());
                double sc = 0.0;
                if (!decimal(buf, sc) || std::fabs(sc - c) > 1e-9)
                    diverge("coherence " + plain(coh) + ", its snapshot records " + plain(buf));
            }
        }
        auto row = st.turn_coherence.find(handle + "|" + turn);
        if (row != st.turn_coherence.end()) {
            double rc = 0.0;
            if (decimal(row->second, rc)) {
                evidence = true;
                if (std::fabs(rc - c) > 1e-9)
                    diverge("coherence " + plain(coh) + ", the turn's CDD_TURN records " +
                            plain(row->second));
            }
        }
        if (!evidence)
            unmeasure("neither a snapshot nor an analysed CDD_TURN records the coherence "
                      "this decision used");
        note("admissibility", v, run, line, v == Verdict::Match ? "" : where + ": " + why);
    }

    void transcriptRef(const json& ev, const std::string& run, long long line) {
        std::string hash;
        if (!field(ev, "entry_hash", hash)) {
            note("transcript", Verdict::Unmeasurable, run, line, "the reference has no entry_hash");
            return;
        }
        if (!transcript_given) {
            note("transcript", Verdict::Unmeasurable, run, line,
                 "no transcript was given to check the reference against");
        } else if (entry_hashes.count(hash)) {
            note("transcript", Verdict::Match, run, line, "");
        } else if (transcript_partial) {
            note("transcript", Verdict::Unmeasurable, run, line,
                 "no entry read has this hash, and some transcript lines were not read");
        } else {
            note("transcript", Verdict::Diverged, run, line,
                 "no transcript entry hashes to the referenced " + plain(hash).substr(0, 16));
        }
    }

    void agentResponse(const json& ev, RunState& st) {
        std::string content, request, model, config, handle, turn;
        if (!field(ev, "content_sha256", content) || content.empty()) return;
        field(ev, "request_sha256", request);
        field(ev, "model", model);
        field(ev, "config_name", config);
        field(ev, "handle_id", handle);
        field(ev, "turn_at_request", turn);
        st.reply_hashes.insert(content);
        st.replies.insert(replyKey(content, request, model, config, handle, turn));
    }

    void execEvent(const json& ev, RunState& st, const std::string& run, long long line) {
        std::string attribution;
        if (!field(ev, "code_attribution", attribution)) return;   // a writer before provenance
        if (attribution != "model_output_match") return;           // nothing is claimed
        std::string code, content, request, model, config, handle, turn, derivation;
        field(ev, "code_sha256", code);
        field(ev, "origin_request_sha256", request);
        field(ev, "origin_model", model);
        field(ev, "origin_config_name", config);
        field(ev, "origin_handle_id", handle);
        field(ev, "origin_turn_at_request", turn);
        field(ev, "origin_derivation", derivation);
        if (!field(ev, "origin_content_sha256", content) || content.empty()) {
            note("provenance", Verdict::Diverged, run, line,
                 "attributed to a model reply, with no reply hash recorded");
            return;
        }
        if (derivation == "response" && code != content) {
            note("provenance", Verdict::Diverged, run, line,
                 "recorded as the whole reply, but the code's hash is not the reply's");
            return;
        }
        if (st.replies.count(replyKey(content, request, model, config, handle, turn))) {
            note("provenance", Verdict::Match, run, line, "");
        } else if (st.reply_hashes.count(content)) {
            note("provenance", Verdict::Diverged, run, line,
                 "an AGENT_RESPONSE records that reply, under another request, model, "
                 "agent or turn than the origin recorded here");
        } else {
            note("provenance", Verdict::Unmeasurable, run, line,
                 "no earlier AGENT_RESPONSE of this run records the reply (a tool-loop "
                 "turn, a proposal candidate, or an event that is not in the file)");
        }
    }
};

void usage() {
    std::cerr <<
        "Usage: naab-gov replay TELEMETRY [--transcript FILE] [--hmac-key VALUE] [--json]\n\n"
        "Checks recorded decisions against their recorded evidence. Nothing is\n"
        "re-executed, no govern.json is loaded and nothing is run or fetched.\n\n"
        "  --transcript FILE   the agent transcript the run wrote, to check each\n"
        "                      TRANSCRIPT_REF against\n"
        "  --hmac-key VALUE    the key the chain was sealed with (else the one\n"
        "                      --verify-telemetry-chain reads from the environment)\n"
        "  --json              the report as JSON on stdout\n\n"
        "Exit: 0 every decision checked matches its evidence; 1 something\n"
        "DIVERGED; 2 nothing diverged but something is UNMEASURABLE; 4 usage.\n";
}

}  // namespace

int cmdReplay(const std::vector<std::string>& args) {
    std::string file, transcript, key;
    bool key_given = false, as_json = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") as_json = true;
        else if (a == "--transcript" && i + 1 < args.size()) transcript = args[++i];
        else if (a == "--hmac-key" && i + 1 < args.size()) { key = args[++i]; key_given = true; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "naab-gov replay: unknown option\n";
            usage();
            return 4;
        } else if (file.empty()) file = a;
        else { usage(); return 4; }
    }
    if (file.empty()) { usage(); return 4; }
    // The same source --verify-telemetry-chain uses when no key is given.
    if (!key_given) key = GovernanceEngine::telemetryChainKeyFromEnvironment();

    Replay rp;
    GovernanceEngine::TelemetryChainReport chain;
    GovernanceEngine::analyzeTelemetryChain(
        file, key, naab::evidence::kMaxTelemetryLineBytes, naab::evidence::kMaxJsonDepth,
        false, chain);
    if (!chain.opened) {
        std::cerr << "naab-gov replay: cannot open the telemetry file\n";
        return 4;
    }
    if (!transcript.empty() && !rp.loadTranscript(transcript)) {
        std::cerr << "naab-gov replay: cannot open the transcript file\n";
        return 4;
    }
    rp.chain(chain, !key.empty());

    long long events = 0;
    {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "naab-gov replay: cannot open the telemetry file\n";
            return 4;
        }
        std::string line;
        bool oversized = false;
        long long n = 0;
        while (naab::evidence::readBoundedLine(
                   in, line, naab::evidence::kMaxTelemetryLineBytes, oversized)) {
            if (!oversized && line.empty()) continue;
            ++n;
            if (oversized ||
                !naab::evidence::jsonNestingWithin(line, naab::evidence::kMaxJsonDepth))
                continue;   // counted by the chain pass
            try {
                json ev = json::parse(line);
                if (!ev.is_object()) continue;
                ++events;
                rp.event(ev, n);
            } catch (const json::exception&) {
                // counted by the chain pass
            }
        }
    }

    long long matched = 0, diverged = 0, unmeasurable = 0;
    for (const auto& kv : rp.totals.match) matched += kv.second;
    for (const auto& kv : rp.totals.diverged) diverged += kv.second;
    for (const auto& kv : rp.totals.unmeasurable) unmeasurable += kv.second;
    // A file with nothing to check has not been checked.
    const bool nothing = (matched + diverged + unmeasurable) == 0;
    const int rc = diverged > 0 ? 1 : (unmeasurable > 0 || nothing) ? 2 : 0;
    const char* overall = rc == 1 ? "DIVERGED" : rc == 2 ? "UNMEASURABLE" : "MATCH";

    static const char* kChecks[] = {"chain", "admissibility", "coherence", "transcript",
                                    "provenance"};
    static const char* kNotChecked =
        "Not checked: the scorer is not re-run, so a penalty that should not have been "
        "charged, or one that was never listed, is not seen; governance level changes, "
        "step-up challenges, guidance, contracts, sequence matches and every other event "
        "type; whether the files are the ones the run wrote, beyond the chain.";

    if (as_json) {
        json out;
        out["result"] = overall;
        out["events_read"] = events;
        out["lines"] = chain.lines;
        out["lines_not_read"] = chain.skipped;
        json checks = json::object();
        for (const char* c : kChecks) {
            auto get = [&](const std::map<std::string, long long>& m) {
                auto it = m.find(c);
                return it == m.end() ? 0LL : it->second;
            };
            checks[c] = {{"match", get(rp.totals.match)},
                         {"diverged", get(rp.totals.diverged)},
                         {"unmeasurable", get(rp.totals.unmeasurable)}};
        }
        out["checks"] = checks;
        json list = json::array();
        for (const auto& f : rp.findings) {
            list.push_back({{"check", f.check}, {"verdict", verdictName(f.verdict)},
                            {"run", f.run}, {"line", f.line}, {"detail", f.detail}});
        }
        out["findings"] = list;
        out["findings_listed"] = static_cast<long long>(rp.findings.size());
        out["not_checked"] = kNotChecked;
        std::cout << out.dump(2) << "\n";
        return rc;
    }

    std::cout << "naab-gov replay: recorded decisions against their recorded evidence\n"
              << "  " << chain.lines << " line(s), " << events << " event(s) read, "
              << chain.runs.size() << " run(s)\n\n";
    for (const char* c : kChecks) {
        auto get = [&](const std::map<std::string, long long>& m) {
            auto it = m.find(c);
            return it == m.end() ? 0LL : it->second;
        };
        printf("  %-14s MATCH %lld  DIVERGED %lld  UNMEASURABLE %lld\n", c,
               get(rp.totals.match), get(rp.totals.diverged), get(rp.totals.unmeasurable));
    }
    fflush(stdout);
    if (!rp.findings.empty()) std::cout << "\n";
    for (const auto& f : rp.findings) {
        std::cout << "  " << verdictName(f.verdict) << " [" << f.check << "]";
        if (!f.run.empty()) std::cout << " run " << f.run;
        if (f.line > 0) std::cout << " line " << f.line;
        std::cout << ": " << f.detail << "\n";
    }
    if (diverged + unmeasurable > static_cast<long long>(rp.findings.size()))
        std::cout << "  ... " << (diverged + unmeasurable - rp.findings.size())
                  << " more not listed\n";
    if (nothing) std::cout << "\n  The file holds nothing this command checks.\n";
    std::cout << "\n  " << kNotChecked << "\n\n  Result: " << overall << "\n";
    return rc;
}
