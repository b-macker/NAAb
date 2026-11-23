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
//   guidance       a delivery's recorded text hashes to its recorded_sha256
//                  (and is the text delivered when the two hashes agree); a
//                  GUIDANCE_RESPONSE names a delivery of the same run, handle,
//                  turn and trigger; its trigger_recurred is what the reply's
//                  CDD_TURN paid; a holdout draw recomputes from the run id and
//                  ordinal, falls on the recorded side of the holdout, and the
//                  run's ordinals run 0..N once each under one holdout value
//   claims         each AGENT_CLAIM's id names its own handle, turn and index;
//                  a reply's claims agree on the reply and their count; each
//                  validation's `settles` names claims of that handle's reply
//                  at its `turn`, all of them, none settled before; its
//                  claims_only_positive and contradicts follow from the settled
//                  claims and the outcome; the reply is one an AGENT_RESPONSE
//                  or the transcript records; and with --transcript the claim
//                  reader, re-run on the reply's text, reads the claims
//                  recorded (Bundle 20 PR 5)
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
#include "naab/claim_reader.h"
#include "naab/crypto_utils.h"
#include "naab/governance.h"
#include "naab/guidance_efficacy.h"
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

// An AGENT_CLAIM (Bundle 20 PR 5): decision fields only, never text.
struct ClaimRec {
    std::string handle, turn, kind, positive, negated, span, reply;
    long long index = -1, of = -1;
    bool settled = false;
    long long line = 0;
};

// A GUIDANCE_RESPONSE, judged once the whole file is read.
struct GuidanceReply {
    std::string id, handle, turn, trigger, recurred;
    long long line = 0;
};

struct RunState {
    std::unordered_map<std::string, double> prev_coherence;        // by handle
    std::unordered_set<std::string> prev_unknown;                   // after a reset
    std::unordered_map<std::string, std::string> turn_coherence;   // handle|turn -> printed
    std::string pulse;                                              // last to_verdict
    std::unordered_set<std::string> reply_hashes;                   // content_sha256
    std::unordered_set<std::string> replies;                        // the joined origin
    // guidance (Bundle 20 PR 4)
    std::unordered_map<std::string, std::string> deliveries;        // id -> handle|turn|trigger
    std::vector<GuidanceReply> guidance_replies;
    // handle|turn_at_request -> each CDD_TURN row: its analyzed field and penalties
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> request_rows;
    std::map<long long, int> holdout_ordinals;                      // ordinal -> times recorded
    std::set<std::string> holdout_values;                           // as printed
    // claims (Bundle 20 PR 5)
    std::unordered_map<std::string, ClaimRec> claims;               // claim id -> claim
    std::map<std::string, std::vector<std::string>> reply_claims;   // handle|turn -> ids, file order
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

    // --- guidance ------------------------------------------------------
    bool lines_skipped = false;   // a line was too large to read, or not JSON

    void requestRow(const json& ev, RunState& st) {
        std::string handle, tar, analyzed, pen;
        if (!field(ev, "handle_id", handle) || !field(ev, "turn_at_request", tar)) return;
        field(ev, "analyzed", analyzed);
        field(ev, "penalties_detail", pen);
        auto& rows = st.request_rows[handle + "|" + tar];
        if (rows.size() < 16) rows.emplace_back(analyzed, pen);
    }

    void guidanceDelivered(const json& ev, RunState& st, const std::string& run, long long line) {
        std::string id, channel, delivered, recorded;
        field(ev, "delivery_id", id);
        field(ev, "channel", channel);
        // A200(a): `sha256` covers the text as delivered, `recorded_sha256` the
        // text as the telemetry writer recorded it (it sanitises absolute paths).
        if (!field(ev, "recorded_sha256", recorded)) {
            note("guidance", Verdict::Unmeasurable, run, line,
                 "delivery " + plain(id) + ": a writer before recorded_sha256, whose only hash "
                 "covers the text before the telemetry writer sanitised it");
        } else {
            auto t = ev.find("text");
            if (t == ev.end() || !t->is_string()) {
                note("guidance", Verdict::Diverged, run, line,
                     "delivery " + plain(id) + ": no text is recorded");
            } else if (naab::security::CryptoUtils::sha256(t->get_ref<const std::string&>()) != recorded) {
                note("guidance", Verdict::Diverged, run, line,
                     "delivery " + plain(id) + ": the recorded text does not hash to its recorded_sha256");
            } else if (!field(ev, "sha256", delivered) || delivered != recorded) {
                note("guidance", Verdict::Unmeasurable, run, line,
                     "delivery " + plain(id) + ": the record holds its own hash, but the text "
                     "delivered differs from it (a path the telemetry writer sanitised, or bytes "
                     "that were not UTF-8, recorded as U+FFFD), so what "
                     "was delivered is not in the file");
            } else {
                note("guidance", Verdict::Match, run, line, "");
            }
        }
        if (channel != "agent") return;
        std::string handle, turn, trigger;
        field(ev, "handle_id", handle);
        field(ev, "turn", turn);
        field(ev, "trigger", trigger);
        if (!id.empty() && st.deliveries.size() < 100000)
            st.deliveries.emplace(id, handle + "|" + turn + "|" + trigger);
        holdoutDraw(ev, st, run, line, true);
    }

    // The holdout draw of a delivered or held-out candidate (Bundle 20 PR 4):
    // sha256("<run_id>:<ordinal>") read as a number in [0, 1), held out below
    // the run's holdout and delivered at or above it.
    void holdoutDraw(const json& ev, RunState& st, const std::string& run, long long line,
                     bool was_delivered) {
        std::string ord_s, draw_s, ho_s;
        const bool o = field(ev, "holdout_ordinal", ord_s);
        const bool d = field(ev, "holdout_draw", draw_s);
        const bool h = field(ev, "holdout", ho_s);
        if (was_delivered && !o && !d && !h) return;   // no holdout in this run
        const std::string what = was_delivered ? "delivered candidate" : "held-out candidate";
        long long ord = -1;
        if (o && !ord_s.empty() && ord_s.size() <= 12 &&
            ord_s.find_first_not_of("0123456789") == std::string::npos)
            ord = std::stoll(ord_s);
        double draw = 0.0, ho = 0.0;
        if (ord < 0 || !d || !h || !decimal(draw_s, draw) || !decimal(ho_s, ho)) {
            note("guidance", Verdict::Unmeasurable, run, line,
                 what + ": the holdout ordinal, draw or value is missing or not a number");
            return;
        }
        if (st.holdout_ordinals.size() < 100000) st.holdout_ordinals[ord]++;
        st.holdout_values.insert(ho_s);
        const double exact = naab::guidance::holdoutDraw(run, static_cast<unsigned long long>(ord));
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", exact);
        // Both figures are printed to six places, so each is half a unit of
        // the last place from the number it stands for.
        if (std::fabs(exact - draw) > 0.0000005 + 1e-12) {
            note("guidance", Verdict::Diverged, run, line,
                 what + " " + plain(ord_s) + ": draw " + plain(draw_s) +
                 ", the run id and ordinal give " + buf);
            return;
        }
        if (!(ho > 0.0 && ho <= 1.0)) {
            note("guidance", Verdict::Diverged, run, line,
                 what + " " + plain(ord_s) + ": holdout " + plain(ho_s) + " is outside (0, 1]");
            return;
        }
        if (std::fabs(exact - ho) < 0.0000005) {
            note("guidance", Verdict::Unmeasurable, run, line,
                 what + " " + plain(ord_s) + ": the draw equals the holdout as printed");
            return;
        }
        const bool held = exact < ho;
        if (held == was_delivered) {
            note("guidance", Verdict::Diverged, run, line,
                 what + " " + plain(ord_s) + ": draw " + buf + (held ? " is below" : " is not below") +
                 " the holdout " + plain(ho_s));
            return;
        }
        note("guidance", Verdict::Match, run, line, "");
    }

    // The signal name a CDD_TURN's penalties_detail uses for config key `key`.
    static std::string paidLabel(const std::string& key) {
        for (int i = 0; i < naab::governance::NUM_CDD_SIGNALS; ++i)
            if (key == naab::governance::kCddSignalKeys[i])
                return naab::governance::ContextDriftAnalyzer::signalName(i);
        return "";
    }

    static bool paid(const std::string& penalties, const std::string& label) {
        size_t pos = 0;
        int seen = 0;
        while (pos < penalties.size() && ++seen <= 64) {
            size_t end = penalties.find(',', pos);
            if (end == std::string::npos) end = penalties.size();
            const std::string term = penalties.substr(pos, end - pos);
            if (term.substr(0, term.find('=')) == label) return true;
            pos = end + 1;
        }
        return false;
    }

    // What can only be judged once the whole file is read.
    void finishGuidance() {
        for (auto& [run, st] : runs) {
            for (const auto& r : st.guidance_replies) {
                const std::string where = "reply to delivery " + plain(r.id);
                auto it = st.deliveries.find(r.id);
                if (it == st.deliveries.end()) {
                    note("guidance", lines_skipped ? Verdict::Unmeasurable : Verdict::Diverged, run, r.line,
                         where + ": no agent-channel delivery of this run has that id" +
                         (lines_skipped ? std::string(", and some lines were not read") : ""));
                } else if (it->second != r.handle + "|" + r.turn + "|" + r.trigger) {
                    note("guidance", Verdict::Diverged, run, r.line,
                         where + ": the delivery was recorded for another handle, turn or trigger");
                } else {
                    note("guidance", Verdict::Match, run, r.line, "");
                }
                if (r.recurred.empty() || r.trigger.rfind("signal:", 0) != 0) continue;
                const std::string label = paidLabel(r.trigger.substr(7));
                if (label.empty()) {
                    note("guidance", Verdict::Unmeasurable, run, r.line,
                         where + ": the trigger names a signal this version does not know");
                    continue;
                }
                int analysed = 0, skipped = 0;
                std::string pen;
                auto rows = st.request_rows.find(r.handle + "|" + r.turn);
                if (rows != st.request_rows.end()) {
                    for (const auto& [a, p] : rows->second) {
                        if (a == "true") { ++analysed; pen = p; }
                        else if (a == "false") ++skipped;
                    }
                }
                if (r.recurred == "not_analyzed") {
                    if (analysed > 0)
                        note("guidance", Verdict::Diverged, run, r.line,
                             where + ": recorded as not analysed, and the turn's CDD_TURN was analysed");
                    else if (skipped > 0) note("guidance", Verdict::Match, run, r.line, "");
                    else note("guidance", Verdict::Unmeasurable, run, r.line,
                              where + ": no CDD_TURN records the reply's turn");
                    continue;
                }
                if (r.recurred != "true" && r.recurred != "false") {
                    note("guidance", Verdict::Unmeasurable, run, r.line,
                         where + ": trigger_recurred " + plain(r.recurred) + " is not one this version knows");
                    continue;
                }
                if (analysed == 0) {
                    note("guidance", skipped > 0 ? Verdict::Diverged : Verdict::Unmeasurable, run, r.line,
                         where + (skipped > 0
                                  ? ": records whether the trigger recurred, and the turn's CDD_TURN was not analysed"
                                  : ": no analysed CDD_TURN records the reply's turn"));
                } else if (analysed > 1) {
                    note("guidance", Verdict::Unmeasurable, run, r.line,
                         where + ": more than one analysed CDD_TURN records the reply's turn");
                } else if (paid(pen, label) != (r.recurred == "true")) {
                    note("guidance", Verdict::Diverged, run, r.line,
                         where + ": trigger_recurred " + plain(r.recurred) + ", and " + plain(label) +
                         (paid(pen, label) ? " paid" : " did not pay") + " on the turn's CDD_TURN");
                } else {
                    note("guidance", Verdict::Match, run, r.line, "");
                }
            }
            if (!st.holdout_ordinals.empty()) {
                const long long last = st.holdout_ordinals.rbegin()->first;
                long long missing = 0, repeated = 0;
                for (long long i = 0; i <= last && i < 100000; ++i) {
                    auto it = st.holdout_ordinals.find(i);
                    if (it == st.holdout_ordinals.end()) ++missing;
                    else if (it->second > 1) ++repeated;
                }
                if (missing || repeated) {
                    note("guidance", lines_skipped && !repeated ? Verdict::Unmeasurable : Verdict::Diverged,
                         run, 0, std::to_string(missing) + " holdout ordinal(s) below the last are not "
                         "recorded and " + std::to_string(repeated) + " are recorded more than once");
                } else {
                    note("guidance", Verdict::Match, run, 0, "");
                }
            }
            if (st.holdout_values.size() > 1)
                note("guidance", Verdict::Diverged, run, 0,
                     "the holdout changed during the run (it is fixed at startup)");
        }
    }

    // --- transcript ----------------------------------------------------
    bool transcript_given = false;
    bool transcript_partial = false;                 // a line was not read
    std::unordered_set<std::string> entry_hashes;    // of entries as written
    long long transcript_edited = 0;
    // A reply's text by its hash, for re-reading claims: an agent_send entry's
    // raw_response.content and processed_response. At most 64 MiB held.
    std::unordered_map<std::string, std::string> transcript_texts;
    size_t transcript_text_bytes = 0;

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
                if (j.is_object()) {
                    auto keep = [&](const json& t) {
                        if (!t.is_string()) return;
                        const std::string& text = t.get_ref<const std::string&>();
                        if (text.empty() || transcript_text_bytes + text.size() > (64u << 20)) return;
                        auto h = naab::security::CryptoUtils::sha256(text);
                        if (transcript_texts.emplace(h, text).second) transcript_text_bytes += text.size();
                    };
                    if (j.contains("processed_response")) keep(j["processed_response"]);
                    if (j.contains("raw_response") && j["raw_response"].is_object() &&
                        j["raw_response"].contains("content"))
                        keep(j["raw_response"]["content"]);
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
        if (type == "CDD_TURN") { requestRow(ev, st); cddTurn(ev, st, run, line); }
        else if (type == "GUIDANCE_DELIVERED") guidanceDelivered(ev, st, run, line);
        else if (type == "GUIDANCE_WITHHELD") {
            std::string reason;
            if (field(ev, "reason", reason) && reason == "holdout") holdoutDraw(ev, st, run, line, false);
        } else if (type == "GUIDANCE_RESPONSE") {
            GuidanceReply r;
            field(ev, "delivery_id", r.id);
            field(ev, "handle_id", r.handle);
            field(ev, "turn_at_request", r.turn);
            field(ev, "trigger", r.trigger);
            field(ev, "trigger_recurred", r.recurred);
            r.line = line;
            if (st.guidance_replies.size() < 100000) st.guidance_replies.push_back(r);
        }
        else if (type == "OUTPUT_ADMISSIBILITY_EVAL") admissibility(ev, st, run, line);
        else if (type == "PULSE_TRANSITION") field(ev, "to_verdict", st.pulse);
        else if (type == "AGENT_RESET") {
            std::string h;
            if (field(ev, "handle_id", h)) st.prev_unknown.insert(h);
        } else if (type == "TRANSCRIPT_REF") transcriptRef(ev, run, line);
        else if (type == "AGENT_RESPONSE") agentResponse(ev, st);
        else if (type == "CODEGEN_EXEC" || type == "POLYGLOT_EXEC") execEvent(ev, st, run, line);
        else if (type == "AGENT_CLAIM") agentClaim(ev, st, run, line);
        else if (type == "VALIDATION_RECORDED") validationRecorded(ev, st, run, line);
    }

    // --- claims (Bundle 20 PR 5) ---------------------------------------
    long long claims_recorded = 0, claims_settled = 0;

    static bool count(const std::string& s, long long& out) {
        if (s.empty() || s.size() > 9) return false;
        long long v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    }

    void agentClaim(const json& ev, RunState& st, const std::string& run, long long line) {
        std::string id, idx, of;
        ClaimRec c;
        c.line = line;
        field(ev, "claim_id", id);
        field(ev, "handle_id", c.handle);
        field(ev, "turn_at_request", c.turn);
        field(ev, "kind", c.kind);
        field(ev, "positive", c.positive);
        field(ev, "negated", c.negated);
        field(ev, "span", c.span);
        field(ev, "reply_sha256", c.reply);
        field(ev, "index", idx);
        field(ev, "of", of);
        ++claims_recorded;
        const std::string where = "claim " + plain(id);
        if (!count(idx, c.index) || !count(of, c.of) || c.index >= c.of) {
            note("claims", Verdict::Diverged, run, line, where + ": its index and count are not a position in the reply");
            return;
        }
        if (id != c.handle + "." + c.turn + "." + idx) {
            note("claims", Verdict::Diverged, run, line, where + ": the id does not name the claim's own handle, turn and index");
            return;
        }
        if (st.claims.count(id)) {
            note("claims", Verdict::Diverged, run, line, where + ": recorded twice");
            return;
        }
        if (st.claims.size() >= 200000) return;
        st.claims.emplace(id, c);
        st.reply_claims[c.handle + "|" + c.turn].push_back(id);
    }

    void validationRecorded(const json& ev, RunState& st, const std::string& run, long long line) {
        std::string settles, turn, handle, passed, only, contradicts;
        if (!ev.contains("settles")) return;          // a writer before claims were recorded
        if (!field(ev, "settles", settles)) {
            note("claims", Verdict::Unmeasurable, run, line,
                 "a validation's settles field is not a string of at most 256 bytes, so what it settles was not read");
            return;
        }
        field(ev, "turn", turn);
        field(ev, "handle_id", handle);
        field(ev, "passed", passed);
        field(ev, "claims_only_positive", only);
        field(ev, "contradicts", contradicts);
        if (settles.empty()) {
            if (contradicts == "true")
                note("claims", Verdict::Diverged, run, line,
                     "a validation that settles no claim records a contradiction");
            return;
        }
        std::vector<std::string> ids;
        for (size_t p = 0; p <= settles.size();) {
            size_t e = settles.find(',', p);
            if (e == std::string::npos) e = settles.size();
            ids.push_back(settles.substr(p, e - p));
            p = e + 1;
        }
        const std::string where = "validation settling " + std::to_string(ids.size()) + " claim(s)";
        for (const auto& id : ids) {
            auto it = st.claims.find(id);
            if (it == st.claims.end()) {
                note("claims", lines_skipped ? Verdict::Unmeasurable : Verdict::Diverged, run, line,
                     where + ": " + plain(id) + " is no earlier AGENT_CLAIM of this run" +
                     (lines_skipped ? std::string(", and some lines were not read") : ""));
                return;
            }
            if (it->second.handle != handle || it->second.turn != turn) {
                note("claims", Verdict::Diverged, run, line,
                     where + ": " + plain(id) + " is another handle's or another turn's claim");
                return;
            }
            if (it->second.settled) {
                note("claims", Verdict::Diverged, run, line, where + ": " + plain(id) + " was settled before");
                return;
            }
        }
        const auto& group = st.reply_claims[handle + "|" + turn];
        if (ids.size() != group.size()) {
            note("claims", Verdict::Diverged, run, line,
                 where + ": the reply's " + std::to_string(group.size()) +
                 " recorded claim(s) are not all settled");
            return;
        }
        bool any = false, all_positive = true;
        for (const auto& id : ids) {
            auto& c = st.claims[id];
            c.settled = true;
            ++claims_settled;
            any = true;
            if (c.positive != "true") all_positive = false;
        }
        const long long of = st.claims[ids.front()].of;
        if (of != static_cast<long long>(ids.size())) {
            note("claims", Verdict::Unmeasurable, run, line,
                 where + ": the reply holds " + std::to_string(of) +
                 " claims and only the first were recorded, so whether all were positive is not known");
            return;
        }
        const bool only_positive = any && all_positive;
        const bool expect = passed == "false" && only_positive;
        if ((only == "true") != only_positive) {
            note("claims", Verdict::Diverged, run, line,
                 where + ": claims_only_positive " + plain(only) + ", and the settled claims say otherwise");
        } else if ((contradicts == "true") != expect) {
            note("claims", Verdict::Diverged, run, line,
                 where + ": contradicts " + plain(contradicts) + " does not follow from passed " +
                 plain(passed) + " and the settled claims");
        } else {
            note("claims", Verdict::Match, run, line, "");
        }
    }

    void finishClaims() {
        for (auto& [run, st] : runs) {
            for (const auto& [key, ids] : st.reply_claims) {
                const ClaimRec& first = st.claims[ids.front()];
                const std::string where = "the claims of handle " + plain(first.handle) +
                                          " turn " + plain(first.turn);
                bool consistent = true;
                std::set<long long> seen;
                for (const auto& id : ids) {
                    const ClaimRec& c = st.claims[id];
                    if (c.reply != first.reply || c.of != first.of || !seen.insert(c.index).second)
                        consistent = false;
                }
                if (!consistent) {
                    note("claims", Verdict::Diverged, run, first.line,
                         where + ": they disagree on the reply, their count, or an index");
                    continue;
                }
                auto text = transcript_texts.find(first.reply);
                if (st.reply_hashes.count(first.reply) || text != transcript_texts.end()) {
                    note("claims", Verdict::Match, run, first.line, "");
                } else {
                    note("claims", Verdict::Unmeasurable, run, first.line,
                         where + ": no AGENT_RESPONSE of this run, and no transcript entry, "
                         "records the reply (a tool-loop turn or a committed proposal writes none)");
                }
                if (text == transcript_texts.end()) {
                    note("claims", Verdict::Unmeasurable, run, first.line,
                         where + (transcript_given
                                  ? std::string(": no transcript text hashes to the reply, so it was not re-read")
                                  : std::string(": no transcript was given, so the reply was not re-read")));
                    continue;
                }
                const auto reading = naab::claims::readClaims(text->second);
                bool same = static_cast<long long>(reading.claims.size()) == first.of;
                for (const auto& id : ids) {
                    if (!same) break;
                    const ClaimRec& c = st.claims[id];
                    const auto& r = reading.claims[static_cast<size_t>(c.index)];
                    same = c.kind == naab::claims::kindName(r.kind) &&
                           c.positive == (r.positive ? "true" : "false") &&
                           c.negated == (r.negated ? "true" : "false") &&
                           c.span == std::to_string(r.begin) + "-" + std::to_string(r.end);
                }
                if (same) note("claims", Verdict::Match, run, first.line, "");
                else note("claims", Verdict::Diverged, run, first.line,
                          where + ": the claim reader, re-run on the reply, reads other claims than were recorded");
            }
        }
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
    rp.lines_skipped = chain.skipped > 0 || chain.corrupt > 0;

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

    rp.finishGuidance();
    rp.finishClaims();

    long long matched = 0, diverged = 0, unmeasurable = 0;
    for (const auto& kv : rp.totals.match) matched += kv.second;
    for (const auto& kv : rp.totals.diverged) diverged += kv.second;
    for (const auto& kv : rp.totals.unmeasurable) unmeasurable += kv.second;
    // A file with nothing to check is never a MATCH: chain() records a file
    // with no chained event as UNMEASURABLE, so the counts are never all zero.
    const int rc = diverged > 0 ? 1 : unmeasurable > 0 ? 2 : 0;
    const char* overall = rc == 1 ? "DIVERGED" : rc == 2 ? "UNMEASURABLE" : "MATCH";

    static const char* kChecks[] = {"chain", "admissibility", "coherence", "transcript",
                                    "provenance", "guidance", "claims"};
    static const char* kNotChecked =
        "Not checked: the scorer is not re-run, so a penalty that should not have been "
        "charged, or one that was never listed, is not seen; governance level changes, "
        "step-up challenges, contracts, sequence matches and every other event type; which "
        "section guidance selection should have chosen, and what followed a delivery after "
        "the reply's own turn (naab-gov efficacy reports that); whether S24 charged a "
        "contradiction (whether it is on is not recorded); whether the files are the "
        "ones the run wrote, beyond the chain.";

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
        out["claims"] = {{"recorded", rp.claims_recorded}, {"settled", rp.claims_settled},
                         {"never_settled", rp.claims_recorded - rp.claims_settled}};
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
    if (rp.claims_recorded > 0)
        printf("\n  claims: %lld recorded, %lld settled by a validation, %lld never settled\n",
               rp.claims_recorded, rp.claims_settled, rp.claims_recorded - rp.claims_settled);
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
    std::cout << "\n  " << kNotChecked << "\n\n  Result: " << overall << "\n";
    return rc;
}
