// naab-gov heal <telemetry.jsonl> --config <govern.json> [--min N] [--json]
//
// Reads what a run recorded and proposes ONE kind of edit to the config it ran
// under: a level key the file WRITES as "advisory", whose rule fired at least
// --min times, raised to "soft". Report only: no model, no file written, no
// config changed. A person makes the edit and signs it outside the run
// (docs/security-decisions.md: no in-process signing).
//
// It proposes nothing unless the evidence holds:
//   - `naab-gov replay` on the same file (run as a child of this binary, which
//     reads the chain key itself -- this command never touches it) reports the
//     chain MATCH for every run and no decision DIVERGED;
//   - a run counts only when its RunStart `config_fingerprint` equals the given
//     config's, and it recorded no accepted mid-run reload (after one, the
//     config that produced its events is not the file given).
// Each proposal is then asked of the ratchet, `naab-gov ratchet OLD NEW
// --ratchet-only`, with NEW the config with that one value changed: it must be
// accepted AND the ratchet must name the key in a tightening notice. A key the
// engine does not read gives no such notice and is not proposed.
//
// Skipped and named, never guessed: a rule with no level key written in the
// file (the ratchet refuses a level key ADDED mid-run, and an absent key's
// default is not in the file to edit), a written level that is not "advisory",
// a rule name outside the plain-name alphabet.
//
// The telemetry file is untrusted input: bounded lines (bounded_jsonl.h),
// fields read as short strings, a rule name used only when it is a plain dotted
// name, nothing pattern-matched, nothing echoed raw.
//
// Not judged, and printed in every report: the key is found by the rule's NAME
// (the longest dotted prefix under which the file writes a level); that it sets
// this rule's level is confirmed only by the ratchet reading it as a level and
// by its written value matching the level the events carry. Counts are of
// recorded events: with telemetry.deduplicate_checks on, repeats at one source
// line are recorded once, so a count can be low, never high.
//
// Exit: 0 the evidence held and there is nothing to propose; 1 at least one
// proposal; 2 UNMEASURABLE (the evidence did not hold, or no run matched the
// config); 4 usage or a file that does not load.

#include "gov_self.h"
#include "naab/bounded_jsonl.h"
#include "naab/governance.h"
#include "naab/scoped_temp_dir.h"
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// A plain dotted name: what a rule name or a run id may be to be used at all.
bool plainName(const std::string& s, size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// A string field of at most 256 bytes, else "".
std::string field(const json& ev, const char* key) {
    auto it = ev.find(key);
    if (it == ev.end() || !it->is_string()) return "";
    const std::string& s = it->get_ref<const std::string&>();
    return s.size() <= 256 ? s : std::string();
}

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool isLevelName(const std::string& v) {
    const std::string l = lower(v);
    return l == "advisory" || l == "detect" || l == "soft" || l == "approval_required" || l == "hard";
}

std::vector<std::string> splitDots(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '.') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// The level key the config WRITES for a rule: the longest dotted prefix of the
// rule's name that is either a level string itself (the shorthand,
// "no_pii": "advisory") or an object holding a string "level".
struct LevelKey {
    bool found = false;
    std::vector<std::string> path;   // to the string to change
    std::string written;             // its value, as written
    std::string dotted() const {
        std::string s;
        for (const auto& p : path) { if (!s.empty()) s += "."; s += p; }
        return s;
    }
};

LevelKey writtenLevelKey(const json& config, const std::string& rule) {
    const std::vector<std::string> parts = splitDots(rule);
    for (size_t k = parts.size(); k >= 1; --k) {
        const json* node = &config;
        bool ok = true;
        for (size_t i = 0; i < k; ++i) {
            if (!node->is_object() || parts[i].empty()) { ok = false; break; }
            auto it = node->find(parts[i]);
            if (it == node->end()) { ok = false; break; }
            node = &*it;
        }
        if (!ok) continue;
        LevelKey key;
        key.path.assign(parts.begin(), parts.begin() + static_cast<long>(k));
        if (node->is_string() && isLevelName(node->get<std::string>())) {
            key.found = true;
            key.written = node->get<std::string>();
            return key;
        }
        if (node->is_object()) {
            auto lv = node->find("level");
            if (lv != node->end() && lv->is_string()) {
                key.found = true;
                key.path.push_back("level");
                key.written = lv->get<std::string>();
                return key;
            }
        }
    }
    return {};
}

void setAt(json& config, const std::vector<std::string>& path, const std::string& value) {
    json* node = &config;
    for (const auto& p : path) node = &(*node)[p];
    *node = value;
}

// Engine text for the report: printable ASCII, at most 160 bytes.
std::string plainText(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (out.size() >= 160) break;
        const unsigned char c = static_cast<unsigned char>(ch);
        out.push_back(c >= 0x20 && c < 0x7f ? ch : '?');
    }
    return out;
}

struct RatchetAnswer {
    bool asked = false;      // the child ran and printed its JSON
    bool accepted = false;
    bool named_level = false;  // a notice names this key as tightened
    std::string notice;
    std::string violation;     // the ratchet's first reason, when it refuses
};

// What a mid-run reload of `old_path` by `candidate` would decide, as
// `naab-gov ratchet` answers it. The candidate is written to a scratch file the
// guard removes; the ratchet judges it as replacing the old file in place.
RatchetAnswer askRatchet(const std::string& self, const std::string& old_path, const json& candidate,
                         const std::string& key) {
    RatchetAnswer a;
    std::error_code ec;
    naab::runtime::ScopedTempDir scratch(fs::temp_directory_path(ec), "naab_heal_");
    if (scratch.path().empty() || !fs::is_directory(scratch.path(), ec)) return a;
    const fs::path file = scratch.path() / "govern.json";
    {
        std::ofstream f(file, std::ios::binary);
        f << candidate.dump(2) << "\n";
        if (!f) return a;
    }
    std::string out, err;
    const int rc = naab::govcli::runSelf(
        self, {"ratchet", old_path, file.string(), "--ratchet-only", "--json"}, out, err);
    const json j = json::parse(out, nullptr, false);
    if (!j.is_object() || !j.contains("outcome") || !j["outcome"].is_string()) return a;
    a.asked = true;
    a.accepted = rc == 0 && j["outcome"] == "accepted";
    if (j.contains("notices") && j["notices"].is_array()) {
        for (const auto& n : j["notices"]) {
            if (!n.is_string()) continue;
            const std::string s = n.get<std::string>();
            // The ratchet writes a tightening it compared as "<key>: ...". A
            // key it does not read gets no notice at all.
            if (s.compare(0, key.size() + 1, key + ":") == 0) {
                a.named_level = true;
                a.notice = plainText(s);
                break;
            }
        }
    }
    if (j.contains("violations") && j["violations"].is_array() && !j["violations"].empty() &&
        j["violations"][0].is_string())
        a.violation = plainText(j["violations"][0].get<std::string>());
    return a;
}

}  // namespace

int cmdHeal(const std::vector<std::string>& args, const char* argv0) {
    std::string telemetry, config_path;
    long long min_count = 3;
    bool as_json = false;
    auto usage = [](const std::string& why) {
        std::cerr << "naab-gov heal: " << why << "\n"
                     "Usage: naab-gov heal <telemetry.jsonl> --config <govern.json> [--min N] [--json]\n";
        return 4;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--config") {
            if (i + 1 >= args.size()) return usage("--config needs a path");
            config_path = args[++i];
        } else if (a == "--min") {
            if (i + 1 >= args.size()) return usage("--min needs a count");
            try {
                size_t used = 0;
                min_count = std::stoll(args[i + 1], &used);
                if (used != args[i + 1].size() || min_count < 2 || min_count > 1000000)
                    return usage("--min needs 2..1000000");
            } catch (...) { return usage("--min needs 2..1000000"); }
            ++i;
        } else if (a == "--json") {
            as_json = true;
        } else if (!a.empty() && a[0] != '-' && telemetry.empty()) {
            telemetry = a;
        } else {
            return usage("unknown option: " + a);
        }
    }
    if (telemetry.empty()) return usage("expected a telemetry file");
    if (config_path.empty()) return usage("--config <govern.json> is required: the config the run used");
    std::error_code ec;
    if (!fs::is_regular_file(telemetry, ec)) return usage("not a file: " + telemetry);
    if (!fs::is_regular_file(config_path, ec)) return usage("not a file: " + config_path);

    naab::govcli::detachStdin();

    // The config, twice: as the engine loads it (for the run identity over its
    // whole extends chain) and as raw JSON (for what the file itself writes).
    json raw;
    {
        std::ifstream f(config_path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        raw = json::parse(ss.str(), nullptr, false);
        if (!raw.is_object()) {
            std::cerr << "naab-gov heal: " << config_path << " is not a JSON object\n";
            return 4;
        }
    }
    std::string fingerprint;
    {
        naab::governance::GovernanceEngine engine;
        if (!engine.loadFromFile(fs::absolute(config_path).lexically_normal().string())) {
            std::cerr << "naab-gov heal: govern.json does not load: " << engine.getLastError() << "\n";
            return 4;
        }
        fingerprint = engine.configFingerprint();
    }
    if (fingerprint.empty()) {
        std::cerr << "naab-gov heal: the config has no run identity\n";
        return 4;
    }

    const std::string self = naab::govcli::selfPath(argv0);
    std::vector<std::string> refusals;   // why nothing may be proposed
    std::vector<std::string> notes;

    // 1. The evidence: replay's own verdict, read from its JSON.
    {
        std::string out, err;
        const int rc = naab::govcli::runSelf(self, {"replay", telemetry, "--json"}, out, err);
        const json j = json::parse(out, nullptr, false);
        auto count = [&](const char* check, const char* what) -> long long {
            if (!j.is_object() || !j.contains("checks")) return -1;
            const auto& c = j["checks"];
            if (!c.is_object() || !c.contains(check) || !c[check].is_object()) return -1;
            const auto& v = c[check];
            if (!v.contains(what) || !v[what].is_number_integer()) return -1;
            return v[what].get<long long>();
        };
        if (!j.is_object() || !j.contains("checks") || !j["checks"].is_object()) {
            refusals.push_back("naab-gov replay gave no report for the file (exit " + std::to_string(rc) + ")");
        } else {
            const long long cm = count("chain", "match"), cd = count("chain", "diverged"),
                            cu = count("chain", "unmeasurable");
            if (cm < 0 || cd < 0 || cu < 0) refusals.push_back("naab-gov replay's report has no chain verdict");
            else if (cd > 0) refusals.push_back("the telemetry chain does not verify (" + std::to_string(cd) + " run(s) DIVERGED)");
            else if (cu > 0) refusals.push_back("the telemetry chain could not be verified for " + std::to_string(cu) +
                                                " run(s) (a keyed chain without its key, or a run with no RunEnd)");
            else if (cm == 0) refusals.push_back("the file holds no chained run to verify");
            long long diverged = 0;
            for (auto it = j["checks"].begin(); it != j["checks"].end(); ++it) {
                if (it.key() == "chain" || !it.value().is_object()) continue;
                auto d = it.value().find("diverged");
                if (d != it.value().end() && d->is_number_integer()) diverged += d->get<long long>();
            }
            if (diverged > 0)
                refusals.push_back("naab-gov replay reports " + std::to_string(diverged) +
                                   " recorded decision(s) DIVERGED from their evidence");
            auto nr = j.find("lines_not_read");
            if (nr != j.end() && nr->is_number_integer() && nr->get<long long>() > 0)
                refusals.push_back(std::to_string(nr->get<long long>()) + " line(s) of the file were not read (over the bounds)");
        }
    }

    // 2. The runs and their advisory violations.
    struct Run {
        std::string fingerprint;
        bool has_start = false, reloaded = false;
        std::map<std::string, long long> advisory;   // rule -> count
    };
    std::map<std::string, Run> runs;
    std::vector<std::string> run_order;
    long long odd_names = 0, unreadable = 0;
    {
        std::ifstream in(telemetry, std::ios::binary);
        std::string line;
        bool oversized = false;
        while (naab::evidence::readBoundedLine(in, line, naab::evidence::kMaxTelemetryLineBytes, oversized)) {
            if (oversized) { ++unreadable; continue; }
            if (line.empty()) continue;
            if (!naab::evidence::jsonNestingWithin(line, naab::evidence::kMaxJsonDepth)) { ++unreadable; continue; }
            const json ev = json::parse(line, nullptr, false);
            if (!ev.is_object()) { ++unreadable; continue; }
            const std::string run_id = field(ev, "run_id");
            const std::string type = field(ev, "event_type");
            if (!plainName(run_id, 64)) { if (!type.empty()) ++odd_names; continue; }
            if (!runs.count(run_id)) run_order.push_back(run_id);
            Run& r = runs[run_id];
            if (type == "RunStart") {
                r.has_start = true;
                r.fingerprint = field(ev, "config_fingerprint");
            } else if (type == "CONFIG_ADJUSTMENT") {
                if (field(ev, "accepted") == "true") r.reloaded = true;
            } else if (type == "RuleViolation") {
                if (lower(field(ev, "level")) != "advisory") continue;
                const std::string rule = field(ev, "rule_name");
                if (!plainName(rule, 128)) { ++odd_names; continue; }
                r.advisory[rule]++;
            }
        }
    }
    if (unreadable > 0 && refusals.empty())
        refusals.push_back(std::to_string(unreadable) + " line(s) of the file were not read");

    std::vector<std::string> used_runs, skipped_runs;
    std::map<std::string, long long> counts;         // rule -> count over the runs used
    std::map<std::string, long long> rule_runs;      // rule -> runs it fired in
    for (const auto& id : run_order) {
        const Run& r = runs[id];
        if (!r.has_start) { skipped_runs.push_back(id + ": no RunStart, so no config identity"); continue; }
        if (r.fingerprint != fingerprint) {
            skipped_runs.push_back(id + ": ran under another config (its identity is not this file's)");
            continue;
        }
        if (r.reloaded) {
            skipped_runs.push_back(id + ": accepted a mid-run reload, so its events are not all this file's");
            continue;
        }
        used_runs.push_back(id);
        for (const auto& [rule, n] : r.advisory) { counts[rule] += n; rule_runs[rule]++; }
    }
    if (refusals.empty() && used_runs.empty())
        refusals.push_back("no run in the file ran under this config");
    if (odd_names > 0)
        notes.push_back(std::to_string(odd_names) + " event(s) with a run id or rule name that is not a plain name were not counted");

    // 3. The one mapping: a repeated advisory -> the level key the file writes.
    struct Proposal { std::string key; std::vector<std::string> path; std::vector<std::string> rules; long long count = 0; std::string notice; };
    std::vector<Proposal> proposals;
    std::vector<std::string> skipped;
    json repeated = json::array();
    if (refusals.empty()) {
        std::map<std::string, Proposal> by_key;
        for (const auto& [rule, n] : counts) {
            if (n < min_count) continue;
            repeated.push_back({{"rule", rule}, {"count", n}, {"runs", rule_runs[rule]}});
            const LevelKey key = writtenLevelKey(raw, rule);
            const std::string head = rule + " (" + std::to_string(n) + " advisory violation(s))";
            if (!key.found) {
                skipped.push_back(head + ": the config writes no level key for it; a level key added mid-run is "
                                         "refused by the ratchet, so there is no edit to propose");
                continue;
            }
            if (lower(key.written) != "advisory") {
                skipped.push_back(head + ": " + key.dotted() + " is written \"" + lower(key.written) +
                                  "\", not \"advisory\" -- the events and the file disagree, so nothing is proposed");
                continue;
            }
            Proposal& p = by_key[key.dotted()];
            p.key = key.dotted();
            p.path = key.path;
            p.rules.push_back(rule);
            p.count += n;
        }
        for (auto& [k, p] : by_key) {
            json candidate = raw;
            setAt(candidate, p.path, "soft");
            const RatchetAnswer a = askRatchet(self, config_path, candidate, p.key);
            std::string rules_s;
            for (const auto& r : p.rules) { if (!rules_s.empty()) rules_s += ", "; rules_s += r; }
            if (!a.asked) {
                skipped.push_back(p.key + " (" + rules_s + "): the ratchet gave no answer, so the edit is unchecked and not proposed");
            } else if (!a.accepted) {
                skipped.push_back(p.key + " (" + rules_s + "): the ratchet refuses advisory -> soft here" +
                                  (a.violation.empty() ? std::string() : " (" + a.violation + ")"));
            } else if (!a.named_level) {
                skipped.push_back(p.key + " (" + rules_s + "): the ratchet accepts the edit but names no tightening for this key, so the engine does not read it as a level");
            } else {
                p.notice = a.notice;
                proposals.push_back(p);
            }
        }
    }

    const int status = !refusals.empty() ? 2 : !proposals.empty() ? 1 : 0;
    const char* result = status == 2 ? "UNMEASURABLE" : status == 1 ? "PROPOSED" : "NOTHING TO PROPOSE";
    const char* not_judged =
        "Not judged: a key is found by its rule's name; that it sets that rule's level is confirmed only by the "
        "ratchet reading it as a level and by its written value matching the events'. Counts are of recorded "
        "events (with telemetry.deduplicate_checks on, repeats at one source line are recorded once). "
        "Nothing is written or signed: a person edits the file and signs it outside the run.";

    if (as_json) {
        json j;
        j["result"] = result;
        j["config"] = config_path;
        j["config_fingerprint"] = fingerprint;
        j["min"] = min_count;
        j["refused"] = refusals;
        j["runs_used"] = used_runs;
        j["runs_skipped"] = skipped_runs;
        j["repeated"] = repeated;
        j["skipped"] = skipped;
        j["notes"] = notes;
        j["proposals"] = json::array();
        for (const auto& p : proposals)
            j["proposals"].push_back({{"key", p.key}, {"from", "advisory"}, {"to", "soft"}, {"rules", p.rules},
                                      {"count", p.count}, {"ratchet", "accepted"}, {"ratchet_notice", p.notice}});
        j["not_judged"] = not_judged;
        std::cout << j.dump(2) << "\n";
        return status;
    }

    std::cout << "naab-gov heal: " << telemetry << "\n"
              << "  config: " << config_path << " (identity " << fingerprint << ")\n"
              << "  a rule is repeated at " << min_count << " or more advisory violations (--min)\n";
    if (!refusals.empty()) {
        std::cout << "\nNothing is proposed, because the evidence did not hold:\n";
        for (const auto& r : refusals) std::cout << "  - " << r << "\n";
    }
    std::cout << "\nRuns under this config: " << used_runs.size() << "\n";
    for (const auto& r : skipped_runs) std::cout << "  skipped " << r << "\n";
    if (refusals.empty()) {
        std::cout << "\nProposed (" << proposals.size() << "):\n";
        if (proposals.empty()) std::cout << "  none\n";
        for (const auto& p : proposals) {
            std::cout << "  - " << p.key << ": \"advisory\" -> \"soft\"   (" << p.count << " advisory violation(s):";
            for (const auto& r : p.rules) std::cout << " " << r;
            std::cout << ")\n      the ratchet accepts it as a tightening: " << p.notice << "\n";
        }
        if (!skipped.empty()) {
            std::cout << "\nRepeated, and not proposed (" << skipped.size() << "):\n";
            for (const auto& s : skipped) std::cout << "  - " << s << "\n";
        }
    }
    for (const auto& n : notes) std::cout << "\nNote: " << n << "\n";
    std::cout << "\n" << not_judged << "\n"
              << "\nresult: " << result << "\n";
    return status;
}
