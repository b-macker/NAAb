// naab-gov history TELEMETRY -- what a project's run history holds (Bundle 20).
//
// A `naab-lang run` with history on (history.enabled, telemetry and its chain
// on) reads what earlier runs refused from their telemetry, and records what
// became of each refusal: refused again, passed (repaired), or met again in an
// imported module. This command shows that record from the telemetry file
// alone, through the same reader the run uses (include/naab/run_history.h):
//   remembered  refusals the next run would start from: the last
//               HISTORY_CARRY in the file, and the refusals after it (a run
//               killed before writing its carry still counts)
//   repaired    the carry's recent repairs: the rule passed for the function
//               at the same or a stricter level (a recorded pass, not a scan)
//   sightings   HISTORY_SUBJECT events: a remembered function refused again,
//               passed, or met in an imported module
//   resets      HISTORY_RESET events: a run found its anchor no longer matched
//
// The file is untrusted input. Every event is used only when its own hash
// recomputes (HMAC with the chain's key when it is keyed), lines are read
// within naab/bounded_jsonl.h's bounds, and text copied from the file into the
// report is cut to a short run of plain characters. It shows facts the engine
// writes -- rule, level, file, function, run id, the store name of a refused
// function's text -- and never that text.
//
// Report only: it loads no govern.json, runs no program, writes nothing, and
// never changes a verdict. A run decides with the real checks under its own
// govern.json; history only chooses where they look.
//
// Exit: 0 a report; 2 UNMEASURABLE (no history event in the file verifies --
// none was written, or the chain is keyed and the key was not given, or the
// lines were edited); 4 usage, or a file that cannot be opened.
#include "naab/crypto_utils.h"
#include "naab/governance.h"
#include "naab/run_history.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;
using naab::governance::GovernanceEngine;
namespace hist = naab::history;

namespace {

// Text from the file, for the report: letters, digits and a few marks only,
// at most 96 bytes. Anything else becomes '?', so a line in the file cannot
// write escape sequences or forged report lines to the reader's terminal.
std::string shown(const std::string& s) {
    if (s.empty()) return "-";
    std::string out;
    for (char ch : s) {
        if (out.size() >= 96) { out += "..."; break; }
        unsigned char c = static_cast<unsigned char>(ch);
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
                  c == ':' || c == '/' || c == '+' || c == '=' || c == ',';
        out.push_back(ok ? ch : '?');
    }
    return out;
}

bool hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// Where the run kept a refused function's text, relative to govern.json's
// directory; "" when the recorded hash is not one.
std::string storeName(const std::string& sha) {
    return hex64(sha) ? ".naab_cache/" + sha + ".subject.txt" : std::string();
}

const char* kindText(const std::string& kind) {
    if (kind == "refused_again") return "refused again";
    if (kind == "passed") return "passed";
    if (kind == "seen_in_module") return "met in an imported module";
    return nullptr;
}

const std::vector<std::string>& notJudged() {
    static const std::vector<std::string> v = {
        "whether a repair is a real fix: \"repaired\" says the rule passed for the function "
        "at the same or a stricter level; a golden-case note says which literals the change "
        "added, which a legitimate fix of a base case can add too",
        "history kept in another telemetry file, or before the last 64 MiB of this one",
        "the code itself: a refused function's text stays in the project's .naab_cache/",
    };
    return v;
}

void usage() {
    std::cerr <<
        "Usage: naab-gov history TELEMETRY [--hmac-key VALUE] [--json]\n\n"
        "What a project's run history holds: the refusals the next run starts\n"
        "from, their repairs, and where remembered code was met again.\n\n"
        "  --hmac-key VALUE    the key the chain was sealed with (else the one\n"
        "                      --verify-telemetry-chain reads)\n"
        "  --json              the report as JSON on stdout\n\n"
        "Exit: 0 a report; 2 UNMEASURABLE (no history event verifies); 4 usage.\n";
}

}  // namespace

int cmdHistory(const std::vector<std::string>& args) {
    std::string file, key;
    bool key_given = false, as_json = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") as_json = true;
        else if (a == "--hmac-key" && i + 1 < args.size()) { key = args[++i]; key_given = true; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "naab-gov history: unknown option\n";
            usage();
            return 4;
        } else if (file.empty()) file = a;
        else { usage(); return 4; }
    }
    if (file.empty()) { usage(); return 4; }
    // The same source --verify-telemetry-chain and replay use when none is given.
    if (!key_given) key = GovernanceEngine::telemetryChainKeyFromEnvironment();
    const auto hash = [&key](const std::string& body) {
        return key.empty() ? naab::security::CryptoUtils::sha256(body)
                           : naab::security::CryptoUtils::hmacSha256(body, key);
    };

    const hist::HistoryReport rep = hist::readHistoryReport(file, hash);
    if (!rep.opened) {
        std::cerr << "naab-gov history: cannot open the telemetry file\n";
        return 4;
    }
    const hist::RunHistory& h = rep.history;

    std::string unmeasurable;
    if (rep.history_lines == 0) {
        unmeasurable = "the file holds no run history (history off, telemetry's chain off, "
                       "or no run has refused a function-body check)";
    } else if (rep.verified == 0) {
        unmeasurable = key.empty()
            ? "no history event verifies: a keyed chain read without its key, or edited lines"
            : "no history event verifies with the key given: another key, an unkeyed chain, "
              "or edited lines";
    }
    const long long unverified = rep.history_lines - rep.verified;

    if (as_json) {
        json out;
        out["verdict"] = unmeasurable.empty() ? "REPORT" : "UNMEASURABLE";
        if (!unmeasurable.empty()) out["reason"] = unmeasurable;
        out["keyed"] = !key.empty();
        out["history_events"] = rep.history_lines;
        out["verified"] = rep.verified;
        out["not_verified"] = unverified;
        out["cut"] = rep.cut;
        out["carry_run"] = shown(h.carry_run);
        json rem = json::array();
        for (const auto& r : h.remembered) {
            json e = {{"rule", shown(r.rule)}, {"level", shown(r.level)}, {"file", shown(r.file)},
                      {"line", r.line}, {"function", shown(r.function)},
                      {"refused_in_run", shown(r.origin_run)}, {"unseen_runs", r.unseen}};
            if (!storeName(r.sha256).empty()) e["stored"] = storeName(r.sha256);
            rem.push_back(e);
        }
        out["remembered"] = rem;
        json rep_a = json::array();
        for (const auto& r : h.repairs) {
            json e = {{"rule", shown(r.rule)}, {"level", shown(r.level)}, {"file", shown(r.file)},
                      {"function", shown(r.function)}, {"refused_in_run", shown(r.refused_run)},
                      {"repaired_in_run", shown(r.repaired_run)}};
            if (!storeName(r.refused_sha256).empty()) e["stored"] = storeName(r.refused_sha256);
            if (r.golden_case > 0) e["golden_case"] = r.golden_case;
            rep_a.push_back(e);
        }
        out["repaired"] = rep_a;
        json sg = json::array();
        for (const auto& s : rep.sightings) {
            if (!kindText(s.kind)) continue;
            json e = {{"kind", s.kind}, {"run", shown(s.run)}, {"rule", shown(s.rule)},
                      {"level", shown(s.level)}, {"file", shown(s.file)},
                      {"function", shown(s.function)}, {"origin_run", shown(s.origin_run)}};
            if (s.kind == "seen_in_module") {
                e["origin_file"] = shown(s.origin_file);
                e["origin_function"] = shown(s.origin_function);
            }
            sg.push_back(e);
        }
        out["sightings"] = sg;
        json rs = json::array();
        for (const auto& r : rep.resets)
            rs.push_back(hist::knownResetReason(r) ? r : "(not a reason this version writes)");
        out["resets"] = rs;
        out["not_judged"] = notJudged();
        std::cout << out.dump(2) << "\n";
        return unmeasurable.empty() ? 0 : 2;
    }

    std::cout << "Run history: " << rep.verified << " of " << rep.history_lines
              << " history event(s) verified" << (key.empty() ? "" : " (keyed)");
    if (rep.cut) std::cout << "; only the last 64 MiB of the file read";
    std::cout << "\n";
    if (!unmeasurable.empty()) {
        std::cout << "UNMEASURABLE: " << unmeasurable << "\n";
        return 2;
    }
    if (unverified > 0)
        std::cout << "  " << unverified << " history event(s) did not verify and were not read\n";

    std::cout << "\nRemembered refusals (" << h.remembered.size() << "), from "
              << (h.carry_run.empty() ? std::string("the refusals in the file (no carry yet)")
                                      : "the carry of run " + shown(h.carry_run))
              << ":\n";
    if (h.remembered.empty()) std::cout << "  none\n";
    for (const auto& r : h.remembered) {
        std::cout << "  " << shown(r.rule) << " (" << shown(r.level) << ")  " << shown(r.file)
                  << ":" << r.line << "  fn " << shown(r.function) << "  refused in run "
                  << shown(r.origin_run);
        if (r.unseen > 0) std::cout << "  not scanned for " << r.unseen << " run(s)";
        if (!storeName(r.sha256).empty()) std::cout << "\n      text: " << storeName(r.sha256);
        std::cout << "\n";
    }

    std::cout << "\nRepaired (" << h.repairs.size() << "):\n";
    if (h.repairs.empty()) std::cout << "  none\n";
    for (const auto& r : h.repairs) {
        std::cout << "  " << shown(r.rule) << " (" << shown(r.level) << ")  " << shown(r.file)
                  << "  fn " << shown(r.function) << "  refused in run " << shown(r.refused_run)
                  << ", passed in run " << shown(r.repaired_run);
        if (!storeName(r.refused_sha256).empty())
            std::cout << "\n      refused text: " << storeName(r.refused_sha256);
        if (r.golden_case > 0)
            std::cout << "\n      the change that made it pass inserts golden case #" << r.golden_case
                      << "'s input and expected value as literals";
        std::cout << "\n";
    }

    size_t shown_sightings = 0;
    for (const auto& s : rep.sightings) shown_sightings += kindText(s.kind) ? 1 : 0;
    std::cout << "\nRemembered code met again (" << shown_sightings << "):\n";
    if (shown_sightings == 0) std::cout << "  none\n";
    for (const auto& s : rep.sightings) {
        const char* k = kindText(s.kind);
        if (!k) continue;
        std::cout << "  run " << shown(s.run) << ": " << shown(s.rule) << " (" << shown(s.level)
                  << ")  " << k << ": " << shown(s.file) << "  fn " << shown(s.function);
        if (s.kind == "seen_in_module")
            std::cout << "  (refused as " << shown(s.origin_file) << " fn "
                      << shown(s.origin_function) << " in run " << shown(s.origin_run) << ")";
        std::cout << "\n";
    }

    if (!rep.resets.empty()) {
        std::cout << "\nResets (" << rep.resets.size() << "):\n";
        for (const auto& r : rep.resets)
            std::cout << "  " << (hist::knownResetReason(r) ? r : "(not a reason this version writes)")
                      << "\n";
    }

    std::cout << "\nNot judged:\n";
    for (const auto& n : notJudged()) std::cout << "  - " << n << "\n";
    return 0;
}
