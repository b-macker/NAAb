// naab-gov explain FILE -- deterministic findings, then a model's explanation
// of them (Bundle 15 PR 2: "reviewers explain deterministic findings; they
// never decide").
//
// What decides is printed first, and comes from tools that already exist:
//   manifest   `naab-gov manifest infer` -- what the program needs, and what
//              the draft could not infer;
//   taint      `naab-gov taint --json` -- flows of tainted data to a sink;
//   sequences  BSD_MATCH events of a telemetry file (--telemetry), read through
//              the bounded reader `naab-gov replay` uses.
// Each is run as this binary's own subcommand, so there is one implementation
// of each finding and this file computes none.
//
// Then, when govern.json names an explainer (`output.explainer`, an entry of
// `agents`), that agent is asked once to explain the findings in plain words.
//
// THE MODEL DECIDES NOTHING. Its reply is printed under a label, records no
// check result, writes no file, and cannot change the exit status: the status
// is fixed before the model is asked. A reply that matches the secret or PII
// patterns is withheld, as the governance voice's guide is (A40).
//
// WHAT REACHES THE MODEL is the findings only, never the program's text and
// never a free-text field of telemetry:
//   - names (functions, sinks, patterns, actions, languages): ident(), at most
//     64 characters of [A-Za-z0-9_.:-]; anything else is withheld;
//   - literals the program wrote (files, hosts, agents, import paths):
//     literal(), at most 96 characters without whitespace; anything else is
//     withheld and counted;
//   - sentences the tools wrote (a flow's origin, a manifest note): phrase(),
//     at most 200 characters of a fixed set, a quoted span inside one held to
//     literal();
//   - of a BSD_MATCH event: pattern_name, level, steps_matched and path only.
//     Its `events` field joins the matched events' free text, which a program
//     or a model wrote, and is never read.
// The prompt as a whole then passes the secret patterns (always) and the PII
// patterns (under the project's no_pii); on a match the model is not asked.
// The reply is scanned as any model output is: for secrets unless the config
// set no_secrets false, for PII under no_pii.
//
// Exit: 0 every section was gathered (whatever it found, whatever the model
// said or failed to say); 2 UNMEASURABLE -- a section could not be gathered
// (named in the report); 4 usage, parse or config error.
#include "naab/agent_provider.h"
#include "naab/bounded_jsonl.h"
#include "naab/governance.h"
#include "naab/subprocess_helpers.h"
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---- what may reach a model ------------------------------------------------

bool identChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == ':' || c == '-';
}

// A name. Empty when the text is not one.
std::string ident(const std::string& s) {
    if (s.empty() || s.size() > 64) return "";
    for (unsigned char c : s) if (!identChar(c)) return "";
    return s;
}

// A literal the program wrote: a path, a host, an agent name. No whitespace,
// so it cannot carry a sentence. Empty when the text does not qualify.
std::string literal(const std::string& s) {
    if (s.empty() || s.size() > 96) return "";
    for (unsigned char c : s) {
        if (!(identChar(c) || c == '/' || c == '@' || c == '~' || c == '*' || c == '+')) return "";
    }
    return s;
}

// A sentence one of the tools wrote. A double-quoted span inside it is program
// text (an import path) and is held to literal(). Empty when it does not qualify.
std::string phrase(const std::string& s) {
    if (s.empty() || s.size() > 200) return "";
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') {
            size_t end = s.find('"', i + 1);
            if (end == std::string::npos) return "";
            const std::string inner = literal(s.substr(i + 1, end - i - 1));
            out += inner.empty() ? "(withheld)" : inner;
            i = end;
            continue;
        }
        if (!(identChar(c) || c == ' ' || c == ',' || c == ';' || c == '(' || c == ')' ||
              c == '<' || c == '>' || c == '/' || c == '\''))
            return "";
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// ---- running this binary's own subcommands ----------------------------------

std::string selfPath(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::string(buf, n);
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p.string();
#endif
    return argv0 ? std::string(argv0) : std::string("naab-gov");
}

struct Section {
    std::string state = "absent";  // gathered | unmeasurable | absent (not asked for)
    std::string reason;            // why unmeasurable
    json data = json::object();    // the finding, as the tool gave it
};

int runSelf(const std::string& self, const std::vector<std::string>& args,
            std::string& out, std::string& err) {
    return naab::runtime::execute_subprocess_with_pipes(
        self, args, out, err, nullptr, nullptr, naab::runtime::ChildStdin::Null);
}

Section gatherTaint(const std::string& self, const std::string& file, const std::string& config) {
    Section s;
    std::vector<std::string> args = {"taint", file, "--json"};
    if (!config.empty()) { args.push_back("--config"); args.push_back(config); }
    std::string out, err;
    const int rc = runSelf(self, args, out, err);
    json j = json::parse(out, nullptr, false);
    if ((rc != 0 && rc != 1 && rc != 2) || !j.is_object()) {
        s.state = "unmeasurable";
        s.reason = "naab-gov taint gave no report (exit " + std::to_string(rc) + ")";
        return s;
    }
    if (rc == 2) {
        s.state = "unmeasurable";
        s.reason = j.value("reason", std::string("no flow was found, but some constructs were not analysed"));
        s.data = j;
        return s;
    }
    s.state = "gathered";
    s.data = j;
    return s;
}

Section gatherManifest(const std::string& self, const std::string& file) {
    Section s;
    std::string out, err;
    const int rc = runSelf(self, {"manifest", "infer", file, "--stdout"}, out, err);
    json j = rc == 0 ? json::parse(out, nullptr, false) : json();
    if (rc != 0 || !j.is_object()) {
        s.state = "unmeasurable";
        s.reason = rc == 2 ? "naab-gov manifest infer met a construct it does not know"
                           : "naab-gov manifest infer gave no draft (exit " + std::to_string(rc) + ")";
        return s;
    }
    s.state = "gathered";
    s.data = j;
    return s;
}

// BSD_MATCH events, counted by (pattern, level, path). Only those three fields
// and steps_matched are read; a line over the bounds makes the section
// unmeasurable, because a count over part of a file is not the file's count.
Section gatherSequences(const std::string& telemetry) {
    Section s;
    std::ifstream in(telemetry, std::ios::binary);
    if (!in.is_open()) {
        s.state = "unmeasurable";
        s.reason = "the telemetry file cannot be opened";
        return s;
    }
    std::map<std::tuple<std::string, std::string, std::string>, std::pair<int, int>> counts;
    int unread = 0, events = 0, odd = 0;
    std::string line;
    bool oversized = false;
    while (naab::evidence::readBoundedLine(in, line, naab::evidence::kMaxTelemetryLineBytes, oversized)) {
        if (oversized) { ++unread; continue; }
        if (line.empty()) continue;
        if (!naab::evidence::jsonNestingWithin(line, 64)) { ++unread; continue; }
        json ev = json::parse(line, nullptr, false);
        if (!ev.is_object()) { ++unread; continue; }
        ++events;
        auto str = [&ev](const char* k) -> std::string {
            auto it = ev.find(k);
            if (it == ev.end() || !it->is_string()) return "";
            const std::string& v = it->get_ref<const std::string&>();
            return v.size() > 256 ? "" : v;
        };
        if (str("event_type") != "BSD_MATCH") continue;
        const std::string name = ident(str("pattern_name"));
        const std::string level = str("level");
        const std::string path = str("path");
        const bool level_ok = level == "advisory" || level == "detect" || level == "soft" ||
                              level == "approval_required" || level == "hard" || level == "observe";
        const bool path_ok = path == "completed" || path == "pre_execution";
        if (name.empty() || !level_ok || !path_ok) { ++odd; continue; }
        int steps = 0;
        const std::string st = str("steps_matched");
        if (!st.empty() && st.size() <= 4 && st.find_first_not_of("0123456789") == std::string::npos)
            steps = std::atoi(st.c_str());
        auto& c = counts[{name, level, path}];
        c.first++;
        if (steps > c.second) c.second = steps;
    }
    if (unread > 0) {
        s.state = "unmeasurable";
        s.reason = std::to_string(unread) + " line(s) of the telemetry file could not be read within the bounds";
        return s;
    }
    if (events == 0) {
        s.state = "unmeasurable";
        s.reason = "the telemetry file holds no event";
        return s;
    }
    s.state = "gathered";
    s.data["events_read"] = events;
    s.data["matches_not_understood"] = odd;
    s.data["matches"] = json::array();
    for (const auto& [k, c] : counts)
        s.data["matches"].push_back({{"pattern", std::get<0>(k)}, {"level", std::get<1>(k)},
                                     {"path", std::get<2>(k)}, {"count", c.first}, {"steps", c.second}});
    return s;
}

// ---- the findings as text: printed, and (the same lines) sent ---------------

// Built twice from the same findings: once for the person at the terminal (the
// tools' own values, made printable), once for the model (filtered as above).
struct Lines {
    bool for_model = false;
    std::vector<std::string> text;
    int withheld = 0;   // fields that did not qualify and were left out (model copy)
    int findings = 0;   // lines that state a finding
};

std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (out.size() >= 200) { out += "..."; break; }
        out.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
    }
    return out;
}

// `raw` is the tool's value, `ok` the same value after its filter ("" = refused).
std::string orWithheld(const std::string& raw, const std::string& ok, Lines& L) {
    if (!L.for_model) return printable(raw);
    if (!ok.empty()) return ok;
    ++L.withheld;
    return "(withheld)";
}

void manifestLines(const Section& s, Lines& L) {
    if (s.state != "gathered") return;
    auto list = [&](const char* label, const json& arr, bool names) {
        if (!arr.is_array() || arr.empty()) return;
        std::string row = std::string("manifest ") + label + ":";
        for (const auto& v : arr) {
            if (!v.is_string()) continue;
            const std::string raw = v.get<std::string>();
            row += " " + orWithheld(raw, names ? ident(raw) : literal(raw), L);
        }
        L.text.push_back(row);
        ++L.findings;
    };
    const json& d = s.data;
    list("actions", d.value("actions", json::array()), true);
    list("languages", d.value("languages", json::array()), true);
    list("hosts", d.value("hosts", json::array()), false);
    if (d.contains("files") && d["files"].is_object()) {
        list("files read", d["files"].value("read", json::array()), false);
        list("files written", d["files"].value("write", json::array()), false);
    }
    list("agents", d.value("agents", json::array()), false);
    if (d.contains("_inference") && d["_inference"].is_array()) {
        for (const auto& n : d["_inference"]) {
            if (!n.is_string()) continue;
            L.text.push_back("manifest not inferred: " + orWithheld(n.get<std::string>(), phrase(n.get<std::string>()), L));
            ++L.findings;
        }
    }
}

void taintLines(const Section& s, Lines& L) {
    if (s.state != "gathered") return;
    const json& d = s.data;
    if (d.contains("flows") && d["flows"].is_array()) {
        for (const auto& f : d["flows"]) {
            if (!f.is_object()) continue;
            const std::string kind = f.value("kind", std::string());
            if (kind != "value" && kind != "control") { ++L.withheld; continue; }
            const std::string sink = f.value("sink", std::string());
            const std::string fn = f.value("function", std::string());
            const std::string origin = f.value("origin", std::string());
            std::string row = "taint " + kind + " flow at line " +
                              std::to_string(f.value("line", 0)) + ": sink " +
                              orWithheld(sink, ident(sink), L) + " in function " +
                              orWithheld(fn, ident(fn), L) + ", from " +
                              orWithheld(origin, phrase(origin), L);
            const std::string via = f.value("via", std::string());
            if (!via.empty()) row += " (" + orWithheld(via, phrase(via), L) + ")";
            L.text.push_back(row);
            ++L.findings;
        }
    }
    if (d.contains("not_analysed") && d["not_analysed"].is_object()) {
        for (auto it = d["not_analysed"].begin(); it != d["not_analysed"].end(); ++it) {
            L.text.push_back("taint not analysed (" + std::to_string(it.value().is_number_integer() ? it.value().get<int>() : 0) +
                             "): " + orWithheld(it.key(), phrase(it.key()), L));
            ++L.findings;
        }
    }
}

void sequenceLines(const Section& s, Lines& L) {
    if (s.state != "gathered") return;
    for (const auto& m : s.data.value("matches", json::array())) {
        L.text.push_back("sequence pattern " + m.value("pattern", std::string()) + " matched " +
                         std::to_string(m.value("count", 0)) + " time(s) at level " +
                         m.value("level", std::string()) + " (" + m.value("path", std::string()) + ", " +
                         std::to_string(m.value("steps", 0)) + " steps)");
        ++L.findings;
    }
}

void detachStdin() {
#ifdef _WIN32
    int fd = _open("NUL", _O_RDONLY);
    if (fd >= 0) { _dup2(fd, 0); if (fd != 0) _close(fd); }
#else
    int fd = ::open("/dev/null", O_RDONLY);
    if (fd >= 0) { ::dup2(fd, 0); if (fd != 0) ::close(fd); }
#endif
}

const char* kPromptHead =
    "You are explaining the output of deterministic governance tools to the person "
    "who wrote a NAAb program. The findings below were computed by those tools. "
    "Explain in plain words what each one means and what a person could look at. "
    "You decide nothing: do not say whether the program is safe, approved or "
    "blocked, and do not suggest switching a check off. At most twelve short lines.\n\n"
    "Findings:\n";

}  // namespace

int cmdExplain(const std::vector<std::string>& args, const char* argv0) {
    std::string file, config_path, telemetry;
    bool as_json = false, no_model = false;
    auto usage = [](const std::string& why) {
        std::cerr << "naab-gov explain: " << why << "\n"
                     "Usage: naab-gov explain <file.naab> [--telemetry <file.jsonl>] "
                     "[--config <govern.json>] [--no-model] [--json]\n";
        return 4;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--config") {
            if (i + 1 >= args.size()) return usage("--config needs a path");
            config_path = args[++i];
        } else if (a == "--telemetry") {
            if (i + 1 >= args.size()) return usage("--telemetry needs a path");
            telemetry = args[++i];
        } else if (a == "--json") {
            as_json = true;
        } else if (a == "--no-model") {
            no_model = true;
        } else if (!a.empty() && a[0] != '-' && file.empty()) {
            file = a;
        } else {
            return usage("unknown option: " + a);
        }
    }
    if (file.empty()) return usage("expected a .naab file");
    if (!fs::is_regular_file(file)) return usage("not a file: " + file);
    if (!telemetry.empty() && !fs::is_regular_file(telemetry)) return usage("not a file: " + telemetry);
    if (!config_path.empty() && !fs::is_regular_file(config_path))
        return usage("govern.json not found: " + config_path);
    const fs::path abs = fs::absolute(file).lexically_normal();

    // The config: the one a run of the file would find, or the one named. It
    // says who the explainer is and which patterns its reply must pass.
    detachStdin();
    naab::governance::GovernanceEngine engine;
    bool have_config = true;
    if (!config_path.empty()) {
        if (!engine.loadFromFile(config_path)) {
            std::cerr << "naab-gov explain: govern.json does not load: " << engine.getLastError() << "\n";
            return 4;
        }
    } else if (!engine.discoverAndLoad(abs.parent_path().string())) {
        if (engine.getLastError() != "not_found") {
            std::cerr << "naab-gov explain: govern.json does not load: " << engine.getLastError() << "\n";
            return 4;
        }
        have_config = false;
    }

    // 1. The findings. Deterministic; nothing below this block changes them.
    const std::string self = selfPath(argv0);
    Section manifest = gatherManifest(self, abs.string());
    Section taint = gatherTaint(self, abs.string(), config_path);
    Section sequences;
    if (!telemetry.empty()) sequences = gatherSequences(telemetry);

    Lines shown, L;   // L is the model's copy
    L.for_model = true;
    for (Lines* x : {&shown, &L}) {
        manifestLines(manifest, *x);
        taintLines(taint, *x);
        sequenceLines(sequences, *x);
    }

    // 2. The exit status, fixed before any model is asked.
    const bool unmeasurable = manifest.state == "unmeasurable" || taint.state == "unmeasurable" ||
                              sequences.state == "unmeasurable";
    const int status = unmeasurable ? 2 : 0;

    // 3. The explanation: text beside the findings, never part of them.
    std::string exp_status, exp_agent, exp_text;
    const std::string explainer = have_config ? engine.getRules().output.explainer : std::string();
    const naab::governance::AgentConfig* cfg = nullptr;
    if (have_config)
        for (const auto& a : engine.getRules().agents)
            if (a.name == explainer) { cfg = &a; break; }
    if (no_model) {
        exp_status = "not asked: --no-model";
    } else if (explainer.empty()) {
        exp_status = "not asked: govern.json names no explainer";
    } else if (!cfg) {
        exp_status = "not asked: the explainer is not an entry of agents";
    } else if (L.findings == 0) {
        exp_status = "not asked: there is no finding to explain";
    } else {
        exp_agent = ident(explainer);
        std::string prompt = kPromptHead;
        for (const auto& t : L.text) prompt += "- " + t + "\n";
        const std::string key = naab::runtime::resolveApiKey(cfg->api_key_env);
        if (key.empty()) {
            exp_status = "not asked: the explainer has no API key in this environment";
        } else if (!engine.firstSecretPattern(prompt).empty() || !engine.piiFinding(prompt).empty()) {
            exp_status = "not asked: the findings matched the secret or PII patterns";
        } else {
            auto resp = naab::runtime::callAgentSimple(*cfg, key, prompt);
            if (!resp.success || resp.content.empty()) {
                exp_status = "none: the explainer did not answer";
            } else if (!engine.modelOutputSecretFinding(resp.content).empty()) {
                exp_status = "withheld: the reply matched the secret patterns";
            } else if (!engine.piiFinding(resp.content).empty()) {
                exp_status = "withheld: the reply matched the PII patterns";
            } else {
                exp_status = "given";
                exp_text = resp.content;
            }
        }
    }

    if (as_json) {
        json j;
        j["file"] = abs.string();
        j["result"] = unmeasurable ? "unmeasurable" : "report";
        auto sec = [](const Section& s) {
            json o;
            o["state"] = s.state;
            if (!s.reason.empty()) o["reason"] = s.reason;
            if (s.state == "gathered") o["data"] = s.data;
            return o;
        };
        j["sections"] = {{"manifest", sec(manifest)}, {"taint", sec(taint)}, {"sequences", sec(sequences)}};
        j["findings"] = shown.text;
        j["findings_for_model"] = L.text;
        j["fields_withheld_from_model"] = L.withheld;
        j["explanation"] = {{"status", exp_status}, {"written_by_model", exp_status == "given"},
                            {"agent", exp_agent}, {"text", exp_text}};
        std::cout << j.dump(2) << "\n";
        return status;
    }

    std::cout << "naab-gov explain: " << abs.string() << "\n";
    auto head = [](const char* name, const Section& s, const char* tool) {
        std::cout << "  " << name << " (" << tool << "): ";
        if (s.state == "gathered") std::cout << "gathered\n";
        else if (s.state == "absent") std::cout << "not asked for\n";
        else std::cout << "UNMEASURABLE -- " << s.reason << "\n";
    };
    head("manifest ", manifest, "naab-gov manifest infer");
    head("taint    ", taint, "naab-gov taint");
    head("sequences", sequences, "BSD_MATCH events of --telemetry, as recorded; naab-gov replay checks the file");
    std::cout << "\nFindings (computed by the tools above; these are what decide):\n";
    if (shown.text.empty()) std::cout << "  none\n";
    for (const auto& t : shown.text) std::cout << "  " << t << "\n";
    if (L.withheld > 0)
        std::cout << "  (" << L.withheld << " field(s) above are not a plain name or path and are "
                     "withheld from the model's copy)\n";
    if (sequences.state == "gathered" && sequences.data.value("matches_not_understood", 0) > 0)
        std::cout << "  (" << sequences.data.value("matches_not_understood", 0)
                  << " BSD_MATCH event(s) had a field this command does not accept, and are not listed)\n";
    std::cout << "\nExplanation";
    if (exp_status == "given") {
        std::cout << " (written by a model, agent " << exp_agent
                  << "; it decides nothing and changes no result above):\n";
        std::istringstream ss(exp_text);
        std::string line;
        // A model's text goes to a terminal: control characters (escape
        // sequences among them) are shown as '?', and the length is bounded.
        size_t shown_bytes = 0;
        while (std::getline(ss, line) && shown_bytes < 8192) {
            for (char& ch : line) {
                unsigned char c = static_cast<unsigned char>(ch);
                if ((c < 0x20 && c != '\t') || c == 0x7f) ch = '?';
            }
            shown_bytes += line.size() + 1;
            std::cout << "  | " << line << "\n";
        }
    } else {
        std::cout << ": " << exp_status << "\n";
    }
    std::cout << "\nresult: " << (unmeasurable ? "UNMEASURABLE -- a section above could not be gathered"
                                               : "report") << "\n";
    return status;
}
