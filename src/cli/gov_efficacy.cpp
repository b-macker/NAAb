// naab-gov efficacy TELEMETRY [--hmac-key VALUE] [--json]
//
// Bundle 20 PR 4 (docs/plan-live-guidance.md, "Measuring whether it helped").
// What happened after each piece of guidance reached an agent, read from the
// telemetry a run wrote: per delivery, did the trigger that selected it come
// back in the next analysed turns (include/naab/guidance_efficacy.h)? Report
// only, by the owner's decision of 2026-10-12: nothing at runtime decides on
// it, and nothing here changes a verdict. The same judge() the next run's
// startup uses, over every line whose own hash recomputes.
//
// Correlation, not causation: a section whose trigger keeps coming back may be
// wrong, or the agent may ignore it, or the trigger may be structural. A held-
// out arm (`guidance.holdout`) is what lets the two arms be compared; without
// one, causation is reported UNMEASURABLE.

#include "naab/crypto_utils.h"
#include "naab/governance.h"
#include "naab/guidance_efficacy.h"
#include "naab/run_history.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;
using naab::governance::GovernanceEngine;
namespace hist = naab::history;
namespace gd = naab::guidance;

namespace {

// Operator- and file-written text reaches the report only through this.
std::string plain(const std::string& s) {
    if (s.empty()) return "-";
    std::string out;
    for (char ch : s) {
        if (out.size() >= 80) { out += "..."; break; }
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == ' ' || c == '.' || c == '_' || c == '-' || c == ':' || c == '/' ||
                        c == '#' || c == ',' || c == '+' || c == '=';
        out.push_back(ok ? ch : '?');
    }
    return out;
}

struct Counts {
    std::map<std::string, int> by;   // result -> count
    int total = 0;
    void add(const std::string& r) { ++by[r]; ++total; }
    int judged() const {
        auto g = [&](const char* k) { auto it = by.find(k); return it == by.end() ? 0 : it->second; };
        return g("recurred") + g("quiet") + g("resolved");
    }
    int get(const char* k) const { auto it = by.find(k); return it == by.end() ? 0 : it->second; }
};

const char* const kResults[] = {"recurred", "quiet", "resolved", "undetermined",
                                "incomplete", "mixed_model", "not_judged"};

std::string countsText(const Counts& c) {
    std::string out;
    for (const char* r : kResults) {
        const int n = c.get(r);
        if (!n) continue;
        if (!out.empty()) out += ", ";
        std::string label = r;
        for (auto& ch : label) if (ch == '_') ch = ' ';
        out += label + " " + std::to_string(n);
    }
    return out.empty() ? "nothing" : out;
}

json countsJson(const Counts& c) {
    json o = json::object();
    for (const char* r : kResults) o[r] = c.get(r);
    o["total"] = c.total;
    o["judged"] = c.judged();
    return o;
}

void usage() {
    std::cerr <<
        "Usage: naab-gov efficacy TELEMETRY [--hmac-key VALUE] [--json]\n\n"
        "What followed each piece of guidance an agent was given: did the drift\n"
        "signal (or failed validation) that selected it come back in the next\n"
        "analysed turns? Report only; correlation unless guidance.holdout made arms.\n\n"
        "  --hmac-key VALUE    the key the chain was sealed with (else the one\n"
        "                      --verify-telemetry-chain reads)\n"
        "  --json              the report as JSON on stdout\n\n"
        "Exit: 0 a report; 2 UNMEASURABLE (no delivery verifies); 4 usage.\n";
}

}  // namespace

int cmdEfficacy(const std::vector<std::string>& args) {
    std::string file, key;
    bool key_given = false, as_json = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") as_json = true;
        else if (a == "--hmac-key" && i + 1 < args.size()) { key = args[++i]; key_given = true; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "naab-gov efficacy: unknown option\n";
            usage();
            return 4;
        } else if (file.empty()) file = a;
        else { usage(); return 4; }
    }
    if (file.empty()) { usage(); return 4; }
    if (!key_given) key = GovernanceEngine::telemetryChainKeyFromEnvironment();
    const auto hash = [&key](const std::string& body) {
        return key.empty() ? naab::security::CryptoUtils::sha256(body)
                           : naab::security::CryptoUtils::hmacSha256(body, key);
    };

    const hist::EfficacyReport rep = hist::readEfficacyReport(file, hash);
    if (!rep.opened) {
        std::cerr << "naab-gov efficacy: cannot open the telemetry file\n";
        return 4;
    }

    // Group: section (key) x arm, signal x arm, model x arm.
    struct Section { std::string source, heading; Counts delivered, held_out; };
    std::map<std::string, Section> sections;
    std::vector<std::string> order;
    std::map<std::string, std::pair<Counts, Counts>> signals, models;
    int delivered = 0, held = 0;
    for (const auto& o : rep.outcomes) {
        const bool h = o.arm == "held_out";
        (h ? held : delivered)++;
        const std::string key_s = o.section_key.empty() ? "(unkeyed)" : o.section_key;
        if (!sections.count(key_s)) { order.push_back(key_s); sections[key_s] = {o.source, o.heading, {}, {}}; }
        (h ? sections[key_s].held_out : sections[key_s].delivered).add(o.result);
        if (o.trigger.rfind("signal:", 0) == 0 || o.trigger == "validation_failed") {
            auto& s = signals[o.trigger];
            (h ? s.second : s.first).add(o.result);
        }
        auto& m = models[o.model.empty() ? "(not recorded)" : o.model];
        (h ? m.second : m.first).add(o.result);
    }

    std::string unmeasurable;
    if (rep.lines == 0) {
        unmeasurable = "the file holds no guidance delivery or drift turn (guidance or drift detection off)";
    } else if (rep.verified == 0) {
        unmeasurable = key.empty()
            ? "no line verifies: an unchained file, a keyed chain read without its key, or edited lines"
            : "no line verifies with the key given";
    } else if (rep.outcomes.empty()) {
        unmeasurable = "no guidance reached an agent in the lines that verify";
    }
    const std::string causation = held == 0
        ? "UNMEASURABLE: no candidate was held out (guidance.holdout is 0), so the delivered arm has "
          "nothing to be compared with"
        : "arms recorded: compare the delivered and held-out counts above (the draw is per candidate "
          "and reproducible; naab-gov replay checks it)";
    const std::vector<std::string> not_judged = {
        "whether a delivered text was the reason a signal went quiet (correlation; see causation)",
        "a trigger other than a drift signal or a failed validation (start, violation, pull)",
        "what the reply took from the text (GUIDANCE_RESPONSE recall is lexical echo)",
        "guidance kept in another telemetry file, or before the last 64 MiB of this one",
    };

    if (as_json) {
        json out;
        out["verdict"] = unmeasurable.empty() ? "REPORT" : "UNMEASURABLE";
        if (!unmeasurable.empty()) out["reason"] = unmeasurable;
        out["keyed"] = !key.empty();
        out["lines"] = rep.lines;
        out["verified"] = rep.verified;
        out["not_verified"] = rep.lines - rep.verified;
        out["cut"] = rep.cut;
        out["delivered"] = delivered;
        out["held_out"] = held;
        json hv = json::array();
        for (const auto& v : rep.holdouts) hv.push_back(plain(v));
        out["holdout_values"] = hv;
        out["causation"] = causation;
        json sa = json::array();
        for (const auto& k : order) {
            const auto& s = sections[k];
            sa.push_back({{"section_key", plain(k)}, {"source", plain(s.source)}, {"heading", plain(s.heading)},
                          {"delivered", countsJson(s.delivered)}, {"held_out", countsJson(s.held_out)}});
        }
        out["sections"] = sa;
        json sg = json::object();
        for (const auto& [t, c] : signals)
            sg[plain(t)] = {{"delivered", countsJson(c.first)}, {"held_out", countsJson(c.second)}};
        out["triggers"] = sg;
        json md = json::object();
        for (const auto& [m, c] : models)
            md[plain(m)] = {{"delivered", countsJson(c.first)}, {"held_out", countsJson(c.second)}};
        out["models"] = md;
        out["not_judged"] = not_judged;
        std::cout << out.dump(2) << "\n";
    } else {
        std::cout << "Guidance efficacy: " << plain(file) << " (report only; nothing at runtime decides on it)\n";
        std::cout << "  lines read " << rep.lines << ", verified " << rep.verified;
        if (rep.lines > rep.verified) std::cout << " (" << (rep.lines - rep.verified) << " not used: their hash does not recompute)";
        if (rep.cut) std::cout << "; the file is longer than the 64 MiB read";
        std::cout << "\n";
        if (!unmeasurable.empty()) {
            std::cout << "  UNMEASURABLE: " << unmeasurable << "\n";
        } else {
            std::cout << "  " << delivered << " delivered, " << held << " held out";
            if (!rep.holdouts.empty()) {
                std::cout << " (holdout";
                for (const auto& v : rep.holdouts) std::cout << " " << plain(v);
                std::cout << ")";
            }
            std::cout << "; each judged over the next " << gd::kWindowTurns << " analysed turns of its handle\n";
            std::cout << "\nSections:\n";
            for (const auto& k : order) {
                const auto& s = sections[k];
                std::cout << "  " << plain(s.source) << " > " << plain(s.heading) << "  [" << plain(k.substr(0, 8)) << "]\n";
                std::cout << "    delivered " << s.delivered.total << ": " << countsText(s.delivered) << "\n";
                if (s.held_out.total)
                    std::cout << "    held out " << s.held_out.total << ": " << countsText(s.held_out) << "\n";
            }
            if (!signals.empty()) {
                std::cout << "\nTriggers:\n";
                for (const auto& [t, c] : signals) {
                    std::cout << "  " << plain(t) << "  delivered: " << countsText(c.first);
                    if (c.second.total) std::cout << "; held out: " << countsText(c.second);
                    std::cout << "\n";
                }
            }
            std::cout << "\nModels:\n";
            for (const auto& [m, c] : models) {
                std::cout << "  " << plain(m) << "  delivered: " << countsText(c.first);
                if (c.second.total) std::cout << "; held out: " << countsText(c.second);
                std::cout << "\n";
            }
            std::cout << "\nCausation: " << causation << "\n";
        }
        std::cout << "\nNot judged:\n";
        for (const auto& n : not_judged) std::cout << "  - " << n << "\n";
    }
    return unmeasurable.empty() ? 0 : 2;
}
