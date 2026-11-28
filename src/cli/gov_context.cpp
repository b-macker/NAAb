// naab-gov context TARGET -- what governs a rule, a language, an agent role or
// a file, read from govern.json and the project's own files (Bundle 21 PR 4).
//
// For the target it reports:
//   rules            each rule that applies: the govern.json section it belongs
//                    to (GovernanceEngine::ruleSectionInfo(), the table a
//                    refusal reads its rationale from), whether the section is
//                    on and at what level, which files of the extends chain
//                    write it, and its rationale (displayableRationale(), the
//                    filter a refusal uses)
//   project_context  what CLAUDE.md, the linter configs and the manifests add,
//                    and whether each addition took effect (the loader a run
//                    uses, applied to a copy of the rules)
//   guidance         the sections a refusal of the rule would show, or a pull
//                    for the language or role: source and heading only
//   history          the run history's refusals and repairs for the rule or
//                    file (the reader `naab-gov history` uses)
//
// Report only, by the bundle's rule: it runs none of the program, fires no
// hook, writes no telemetry and changes no verdict. It reads the decide-only
// gates (pathPolicyDenial(), the language lists the language gate reads), not
// the enforcing ones, which record results and can fire an operator's hook.
//
// Nothing it prints carries a value a check matches or scores against: no
// detection pattern or list entry (banned names, blocked commands, custom rule
// patterns), threshold, weight or limit, no name of a variable that holds a
// key, no sanitizer list, no contract clause or golden case, no guidance text
// and no stored code. A coding agent that reads this learns what governs its
// work and why, not what to write to get past it.
//
// Exit: 0 a report; 2 UNMEASURABLE (no govern.json governs the target, or no
// section of it governs a rule of that name); 4 usage, or a govern.json that
// does not load.
#include "gov_self.h"
#include "naab/crypto_utils.h"
#include "naab/governance.h"
#include "naab/language_descriptors.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/project_context.h"
#include "naab/run_history.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using naab::governance::GovernanceEngine;
using naab::governance::GovernanceRules;
namespace hist = naab::history;

namespace {

// Text from a file or a config, for the report: letters, digits and a few
// marks, at most `max` bytes; anything else reads '?', so no file can write
// escape sequences or forged report lines to the reader's terminal.
std::string plain(const std::string& s, size_t max = 96) {
    if (s.empty()) return "-";
    std::string out;
    for (char ch : s) {
        if (out.size() >= max) { out += "..."; break; }
        unsigned char c = static_cast<unsigned char>(ch);
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == ':' || c == '/' || c == '+' ||
                  c == '=' || c == ',' || c == '[' || c == ']' || c == '#' || c == ' ' ||
                  c == '(' || c == ')' || c == '\'' || c == '"' || c == '*';
        out.push_back(ok ? ch : '?');
    }
    return out;
}

std::string relTo(const fs::path& base, const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::relative(p, base, ec);
    if (ec || r.empty()) return p.generic_string();
    return r.generic_string();
}

// ---------------------------------------------------------------------------
// The extends chain, as files: the loaded govern.json first, then each parent.
// ---------------------------------------------------------------------------
struct ChainFile {
    fs::path path;
    json doc;
};

std::vector<ChainFile> readChain(const fs::path& config, std::vector<std::string>& notes) {
    std::vector<ChainFile> chain;
    std::set<std::string> seen;
    fs::path cur = config;
    for (int depth = 0; depth < 8; ++depth) {
        std::error_code ec;
        const std::string key = fs::weakly_canonical(cur, ec).generic_string();
        if (!seen.insert(key).second) { notes.push_back("the extends chain repeats a file; read up to it"); break; }
        std::ifstream in(cur);
        if (!in) { notes.push_back("an extends parent could not be opened: " + plain(cur.filename().string())); break; }
        json doc;
        try {
            doc = json::parse(in);
        } catch (...) {
            notes.push_back("an extends parent is not JSON: " + plain(cur.filename().string()));
            break;
        }
        chain.push_back({cur, doc});
        if (!doc.is_object() || !doc.contains("extends") || !doc["extends"].is_string()) break;
        fs::path parent = doc["extends"].get<std::string>();
        if (parent.is_relative()) parent = cur.parent_path() / parent;
        cur = parent.lexically_normal();
    }
    return chain;
}

// Does `doc` write anything at `section`? A dotted path; "custom_rules[ID]" is
// the custom rule with that id, "governance_plugins[].rules[ID]" the plugin
// rule with that id, "contracts.functions.F" that function's contract.
bool writesSection(const json& doc, const std::string& section) {
    if (!doc.is_object() || section.empty()) return false;
    auto idInArray = [](const json& arr, const std::string& id) {
        if (!arr.is_array()) return false;
        for (const auto& e : arr)
            if (e.is_object() && e.contains("id") && e["id"].is_string() && e["id"].get<std::string>() == id)
                return true;
        return false;
    };
    const std::string cr = "custom_rules[";
    if (section.rfind(cr, 0) == 0 && section.back() == ']') {
        return doc.contains("custom_rules") &&
               idInArray(doc["custom_rules"], section.substr(cr.size(), section.size() - cr.size() - 1));
    }
    const std::string gp = "governance_plugins[].rules[";
    if (section.rfind(gp, 0) == 0 && section.back() == ']') {
        const std::string id = section.substr(gp.size(), section.size() - gp.size() - 1);
        if (!doc.contains("governance_plugins") || !doc["governance_plugins"].is_array()) return false;
        for (const auto& p : doc["governance_plugins"])
            if (p.is_object() && p.contains("rules") && idInArray(p["rules"], id)) return true;
        return false;
    }
    const json* node = &doc;
    std::stringstream ss(section);
    std::string seg;
    while (std::getline(ss, seg, '.')) {
        if (!node->is_object() || !node->contains(seg)) return false;
        node = &(*node)[seg];
    }
    return true;
}

// ---------------------------------------------------------------------------
// The report
// ---------------------------------------------------------------------------
struct Ctx {
    GovernanceEngine& eng;
    fs::path config_dir;
    std::vector<ChainFile> chain;
    fs::path config_path;
};

json ruleEntry(Ctx& c, const std::string& rule) {
    const GovernanceRules& r = c.eng.getRules();
    const auto info = GovernanceEngine::ruleSectionInfo(r, rule);
    json e;
    e["rule"] = plain(rule);
    e["section"] = info.section.empty() ? json(nullptr) : json(plain(info.section));
    if (info.enabled >= 0) e["enabled"] = info.enabled == 1;
    if (!info.level.empty()) e["level"] = info.level;
    if (!info.mode.empty()) e["mode"] = plain(info.mode);
    json set_in = json::array();
    for (const auto& f : c.chain)
        if (writesSection(f.doc, info.section)) set_in.push_back(plain(relTo(c.config_dir, f.path)));
    e["set_in"] = set_in;
    if (!info.rationale.empty()) {
        std::string why;
        const std::string shown = c.eng.displayableRationale(r, info.rationale, &why);
        if (shown.empty()) {
            e["rationale"] = nullptr;
            e["rationale_withheld"] = why;
        } else {
            e["rationale"] = shown;
        }
    } else {
        e["rationale"] = nullptr;
    }
    return e;
}

// project_context: what the project's files add, applied to a copy of the
// rules, so each extraction's status is the one a run would give it.
json projectContext(Ctx& c, const std::string& start_dir,
                    const std::function<bool(const naab::governance::ContextExtraction&)>& related) {
    json pc;
    const GovernanceRules& r = c.eng.getRules();
    if (!r.project_context.enabled) {
        pc["enabled"] = false;
        return pc;
    }
    pc["enabled"] = true;
    naab::governance::ProjectContextLoader loader;
    std::vector<naab::governance::ContextExtraction> ex;
    try {
        ex = loader.loadContext(start_dir, r.project_context);
        if (!r.project_context.dry_run) {
            auto level = naab::governance::EnforcementLevel::ADVISORY;
            if (r.project_context.enforcement_level == "soft") level = naab::governance::EnforcementLevel::SOFT;
            else if (r.project_context.enforcement_level == "hard") level = naab::governance::EnforcementLevel::HARD;
            // The rules as a run has them BEFORE project context is applied:
            // the loaded engine's rules already hold what the files add, so
            // applying again would read every addition as redundant.
            GovernanceEngine pre;
            GovernanceRules copy = pre.loadFromFile(c.config_path.string()) ? pre.getRules() : r;
            loader.applyToRules(copy, ex, level);
        }
    } catch (const std::exception&) {
        pc["unmeasurable"] = "the project's files could not be read";
        return pc;
    }
    json items = json::array();
    for (const auto& x : ex) {
        if (related && !related(x)) continue;
        json it;
        it["source"] = plain(x.source_file);
        if (x.line_number > 0) it["line"] = x.line_number;
        it["category"] = plain(x.category);
        it["maps_to"] = plain(x.mapped_rule);
        it["status"] = plain(x.status);
        it["wrote_rule"] = x.wrote_rule;
        // A ban's directive names what it bans: a detection pattern, not shown.
        if (x.category != "banned") it["directive"] = plain(x.directive);
        if (!x.language.empty()) it["language"] = plain(x.language);
        items.push_back(it);
    }
    pc["extractions"] = items;
    return pc;
}

json guidanceList(const std::vector<naab::governance::GuidanceDelivery>& d, size_t withheld) {
    json g;
    json items = json::array();
    for (const auto& x : d) items.push_back({{"source", plain(x.source)}, {"heading", plain(x.heading)}});
    g["sections"] = items;
    if (withheld > 0) g["withheld"] = withheld;
    return g;
}

json historyFor(Ctx& c, const std::string& telemetry, const std::string& key,
                const std::function<bool(const std::string& rule, const std::string& file)>& keep) {
    json h;
    if (telemetry.empty()) {
        h["unmeasurable"] = "no telemetry file: telemetry is off in govern.json and none was given";
        return h;
    }
    h["telemetry"] = plain(relTo(c.config_dir, telemetry));
    const auto hash = [&key](const std::string& body) {
        return key.empty() ? naab::security::CryptoUtils::sha256(body)
                           : naab::security::CryptoUtils::hmacSha256(body, key);
    };
    const hist::HistoryReport rep = hist::readHistoryReport(telemetry, hash);
    if (!rep.opened) {
        h["unmeasurable"] = "the telemetry file does not exist yet";
        return h;
    }
    if (rep.history_lines > 0 && rep.verified == 0) {
        h["unmeasurable"] = "no history event verifies (a keyed chain read without its key, or edited lines)";
        return h;
    }
    json rem = json::array(), rep_a = json::array();
    for (const auto& x : rep.history.remembered) {
        if (!keep(x.rule, x.file)) continue;
        rem.push_back({{"rule", plain(x.rule)}, {"level", plain(x.level)}, {"file", plain(x.file)},
                       {"line", x.line}, {"function", plain(x.function)},
                       {"refused_in_run", plain(x.origin_run)}});
    }
    for (const auto& x : rep.history.repairs) {
        if (!keep(x.rule, x.file)) continue;
        rep_a.push_back({{"rule", plain(x.rule)}, {"level", plain(x.level)}, {"file", plain(x.file)},
                         {"function", plain(x.function)}, {"refused_in_run", plain(x.refused_run)},
                         {"repaired_in_run", plain(x.repaired_run)}});
    }
    h["remembered"] = rem;
    h["repaired"] = rep_a;
    return h;
}

std::string languageVerdict(const GovernanceRules& r, const std::string& runtime, std::string& rule_out) {
    if (naab::lang::languageListBlocks(r.blocked_languages, runtime)) {
        rule_out = "languages.blocked";
        return "refused";
    }
    if (!r.allowed_languages.empty() && !naab::lang::languageListAdmits(r.allowed_languages, runtime)) {
        rule_out = "languages.allowed";
        return "refused";
    }
    rule_out.clear();
    return "allowed";
}

const naab::governance::AgentConfig* findRole(const GovernanceRules& r, const std::string& name) {
    for (const auto& a : r.agents)
        if (a.name == name) return &a;
    return nullptr;
}

// The polyglot block languages of a .naab file, by the run's own lexer.
std::vector<std::string> blockLanguages(const std::string& text) {
    std::vector<std::string> out;
    try {
        naab::lexer::Lexer lx(text);
        for (const auto& t : lx.tokenize()) {
            if (t.type != naab::lexer::TokenType::INLINE_CODE) continue;
            std::string lang = t.value.substr(0, t.value.find(':'));
            lang = lang.substr(0, lang.find('['));
            lang = lang.substr(0, lang.find("->"));
            if (std::find(out.begin(), out.end(), lang) == out.end()) out.push_back(lang);
        }
    } catch (...) {
    }
    return out;
}

const std::vector<std::string>& notShown() {
    static const std::vector<std::string> v = {
        "values: detection patterns and list entries, thresholds, weights and limits",
        "the names of variables that hold keys, and sanitizer lists",
        "contract clauses and golden cases",
        "guidance text (a program's governance.guidance() returns it) and stored refused code",
    };
    return v;
}

const std::vector<std::string>& notJudged() {
    static const std::vector<std::string> v = {
        "whether a check fires on a given text (naab-gov check, naab-gov lint)",
        "what a reload or another environment would change (naab-gov diff, naab-gov ratchet)",
        "a level raised during a run (advisory escalation, the governance level's HIGH promotion)",
        "guidance from https, command and MCP sources: naab-gov never fetches them",
    };
    return v;
}

void printRule(const json& e) {
    std::cout << "  " << e["rule"].get<std::string>();
    if (e["section"].is_null()) {
        std::cout << "\n    section: none -- no govern.json section governs it\n";
        return;
    }
    std::cout << "\n    section: " << e["section"].get<std::string>();
    if (e.contains("enabled")) std::cout << (e["enabled"].get<bool>() ? "  (on)" : "  (off)");
    if (e.contains("level")) std::cout << "  level " << e["level"].get<std::string>();
    if (e.contains("mode")) std::cout << "  mode " << e["mode"].get<std::string>();
    std::cout << "\n    set in: ";
    if (e["set_in"].empty()) std::cout << "no file (engine default)";
    for (size_t i = 0; i < e["set_in"].size(); ++i)
        std::cout << (i ? ", " : "") << e["set_in"][i].get<std::string>();
    std::cout << "\n";
    if (e.contains("rationale_withheld"))
        std::cout << "    why: (not shown -- it holds a value governance withholds: "
                  << e["rationale_withheld"].get<std::string>() << ")\n";
    else if (!e["rationale"].is_null())
        std::cout << "    why: " << e["rationale"].get<std::string>() << "\n";
    else
        std::cout << "    why: (no rationale written)\n";
}

void printReport(const json& out) {
    std::cout << "Context for " << out["target"]["kind"].get<std::string>() << " "
              << out["target"]["name"].get<std::string>() << "\n";
    std::cout << "govern.json: " << out["config"]["file"].get<std::string>();
    if (out["config"]["chain"].size() > 1) {
        std::cout << "  (extends";
        for (size_t i = 1; i < out["config"]["chain"].size(); ++i)
            std::cout << " " << out["config"]["chain"][i].get<std::string>();
        std::cout << ")";
    }
    std::cout << "  mode " << out["config"]["mode"].get<std::string>() << "\n";
    for (const auto& n : out["config"]["notes"]) std::cout << "  note: " << n.get<std::string>() << "\n";

    if (out.contains("language")) {
        const auto& l = out["language"];
        std::cout << "\nLanguage " << l["runtime"].get<std::string>() << ": " << l["verdict"].get<std::string>();
        if (l.contains("by")) std::cout << " (" << l["by"].get<std::string>() << ")";
        std::cout << "\n";
        if (l.contains("role")) std::cout << "  agent role " << l["role"].get<std::string>() << ": "
                                          << l["role_verdict"].get<std::string>() << "\n";
        if (l.contains("separate_process_refused") && l["separate_process_refused"].get<bool>())
            std::cout << "  shell is disabled: a language that runs as a separate process is refused at run time\n";
        for (const auto& s : l["settings"]) std::cout << "  setting: " << s.get<std::string>() << "\n";
    }
    if (out.contains("agent")) {
        const auto& a = out["agent"];
        std::cout << "\nAgent role " << a["name"].get<std::string>() << ":\n";
        for (auto it = a.begin(); it != a.end(); ++it) {
            if (it.key() == "name") continue;
            std::cout << "  " << it.key() << ": " << it.value().dump() << "\n";
        }
    }
    if (out.contains("file")) {
        const auto& f = out["file"];
        std::cout << "\nFile " << f["path"].get<std::string>() << ":\n";
        std::cout << "  program read: " << f["read"].get<std::string>() << "\n";
        std::cout << "  program write: " << f["write"].get<std::string>() << "\n";
        for (const auto& p : f["protected_as"]) std::cout << "  protected: " << p.get<std::string>() << "\n";
        if (f.contains("blocks")) {
            for (const auto& b : f["blocks"])
                std::cout << "  block language " << b["language"].get<std::string>() << ": "
                          << b["verdict"].get<std::string>() << "\n";
        }
        if (f.contains("contracts")) {
            for (const auto& ct : f["contracts"]) {
                std::cout << "  contract on fn " << ct["function"].get<std::string>() << ":";
                for (const auto& k : ct["kinds"]) std::cout << " " << k.get<std::string>();
                std::cout << "\n";
            }
        }
        if (f.contains("manifest")) std::cout << "  capability manifest: " << f["manifest"].get<std::string>() << "\n";
    }

    std::cout << "\nRules (" << out["rules"].size() << "):\n";
    if (out["rules"].empty()) std::cout << "  none\n";
    for (const auto& e : out["rules"]) printRule(e);

    const auto& pc = out["project_context"];
    std::cout << "\nProject files (project_context): ";
    if (!pc["enabled"].get<bool>()) {
        std::cout << "off\n";
    } else if (pc.contains("unmeasurable")) {
        std::cout << "UNMEASURABLE: " << pc["unmeasurable"].get<std::string>() << "\n";
    } else {
        std::cout << pc["extractions"].size() << "\n";
        for (const auto& x : pc["extractions"]) {
            std::cout << "  " << x["source"].get<std::string>();
            if (x.contains("line")) std::cout << ":" << x["line"].get<int>();
            std::cout << "  " << x["category"].get<std::string>() << " -> " << x["maps_to"].get<std::string>()
                      << "  [" << x["status"].get<std::string>() << "]";
            if (x["wrote_rule"].get<bool>()) std::cout << "  (wrote a rule; the file is read-only to the program)";
            if (x.contains("directive")) std::cout << "\n      " << x["directive"].get<std::string>();
            std::cout << "\n";
        }
    }

    const auto& g = out["guidance"];
    std::cout << "\nGuidance: ";
    if (g.contains("off")) {
        std::cout << "off\n";
    } else {
        std::cout << g["sections"].size() << " section(s)";
        if (g.contains("withheld")) std::cout << ", " << g["withheld"].get<size_t>() << " withheld";
        std::cout << "\n";
        for (const auto& s : g["sections"])
            std::cout << "  " << s["source"].get<std::string>() << " > " << s["heading"].get<std::string>() << "\n";
    }

    if (out.contains("history")) {
        const auto& h = out["history"];
        std::cout << "\nRun history: ";
        if (h.contains("unmeasurable")) {
            std::cout << "UNMEASURABLE: " << h["unmeasurable"].get<std::string>() << "\n";
        } else {
            std::cout << h["remembered"].size() << " refusal(s) remembered, " << h["repaired"].size()
                      << " repaired\n";
            for (const auto& x : h["remembered"])
                std::cout << "  refused: " << x["rule"].get<std::string>() << " (" << x["level"].get<std::string>()
                          << ")  " << x["file"].get<std::string>() << ":" << x["line"].get<int>() << "  fn "
                          << x["function"].get<std::string>() << "  run " << x["refused_in_run"].get<std::string>()
                          << "\n";
            for (const auto& x : h["repaired"])
                std::cout << "  repaired: " << x["rule"].get<std::string>() << " (" << x["level"].get<std::string>()
                          << ")  " << x["file"].get<std::string>() << "  fn " << x["function"].get<std::string>()
                          << "  refused in run " << x["refused_in_run"].get<std::string>() << ", passed in run "
                          << x["repaired_in_run"].get<std::string>() << "\n";
        }
    }

    std::cout << "\nNot shown:\n";
    for (const auto& n : out["not_shown"]) std::cout << "  - " << n.get<std::string>() << "\n";
    std::cout << "Not judged:\n";
    for (const auto& n : out["not_judged"]) std::cout << "  - " << n.get<std::string>() << "\n";
}

int unmeasurable(bool as_json, const std::string& reason) {
    if (as_json) {
        std::cout << json{{"verdict", "UNMEASURABLE"}, {"reason", reason}}.dump(2) << "\n";
    } else {
        std::cout << "UNMEASURABLE: " << reason << "\n";
    }
    return 2;
}

void usage() {
    std::cerr <<
        "Usage: naab-gov context TARGET [--config PATH] [--telemetry FILE] [--env NAME]\n"
        "                        [--agent-id NAME] [--hmac-key VALUE] [--json]\n\n"
        "What governs TARGET, and why: the rules that apply, where in the extends\n"
        "chain each is set, their rationale, what the project's files add, the\n"
        "guidance a refusal would show, and the run history's refusals and repairs.\n"
        "Report only: it runs nothing and writes nothing.\n\n"
        "  TARGET  rule:NAME, lang:NAME, agent:NAME or file:PATH; a bare name is a\n"
        "          file when one exists, else an agent role, a language or a rule\n"
        "  --config PATH     the govern.json to read (else the one a run would find)\n"
        "  --telemetry FILE  the run history's telemetry file (else govern.json's)\n"
        "  --env NAME        the environment (else governance.default_env)\n"
        "  --agent-id NAME   the agent id a run would have (else govern.json's)\n"
        "  --hmac-key VALUE  the key the telemetry chain was sealed with\n"
        "  --json            the report as JSON on stdout\n\n"
        "Exit: 0 a report; 2 UNMEASURABLE; 4 usage, or a govern.json that does not load.\n";
}

}  // namespace

int cmdContext(const std::vector<std::string>& args) {
    std::string target, config_path, telemetry, env_name, agent_id, key;
    bool as_json = false, key_given = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](std::string& v) {
            if (i + 1 >= args.size()) return false;
            v = args[++i];
            return true;
        };
        if (a == "--json") as_json = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--config") { if (!next(config_path)) { usage(); return 4; } }
        else if (a == "--telemetry") { if (!next(telemetry)) { usage(); return 4; } }
        else if (a == "--env") { if (!next(env_name)) { usage(); return 4; } }
        else if (a == "--agent-id") { if (!next(agent_id)) { usage(); return 4; } }
        else if (a == "--hmac-key") { if (!next(key)) { usage(); return 4; } key_given = true; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "naab-gov context: unknown option\n";
            usage();
            return 4;
        } else if (target.empty()) target = a;
        else { usage(); return 4; }
    }
    if (target.empty()) { usage(); return 4; }

    // The target's kind: an explicit prefix, else the first that fits.
    std::string kind, name = target;
    for (const auto& [prefix, k] : std::vector<std::pair<std::string, std::string>>{
             {"rule:", "rule"}, {"lang:", "language"}, {"language:", "language"},
             {"agent:", "agent"}, {"file:", "file"}}) {
        if (target.rfind(prefix, 0) == 0) { kind = k; name = target.substr(prefix.size()); break; }
    }
    if (name.empty()) { usage(); return 4; }
    std::error_code ec;
    if (kind.empty() && fs::exists(name, ec)) kind = "file";

    // The policy: the govern.json a run would find (from the file's directory
    // for a file, else from here), or the one named.
    naab::govcli::detachStdin();
    const fs::path here = fs::current_path(ec);
    fs::path start = here;
    if (kind == "file") start = fs::absolute(name, ec).parent_path();
    GovernanceEngine engine;
    if (!config_path.empty()) {
        if (!fs::is_regular_file(config_path, ec)) {
            std::cerr << "naab-gov context: govern.json not found: " << config_path << "\n";
            return 4;
        }
        const std::string abs = fs::absolute(config_path, ec).lexically_normal().string();
        if (!engine.loadProjectConfig(abs, kind == "file" ? start.string() : fs::path(abs).parent_path().string())) {
            std::cerr << "naab-gov context: govern.json does not load: " << engine.getLastError() << "\n";
            return 4;
        }
    } else if (!engine.discoverAndLoad(start.string())) {
        if (engine.getLastError() == "not_found")
            return unmeasurable(as_json, "no govern.json is found from " + plain(start.generic_string(), 200) +
                                             ", so nothing governs the target");
        std::cerr << "naab-gov context: govern.json does not load: " << engine.getLastError() << "\n";
        return 4;
    }
    // As a run: the agent id is the flag, else govern.json's; the environment
    // the flag, else governance.default_env.
    if (agent_id.empty()) agent_id = "anonymous";
    if (!engine.getRules().agent_id_config.empty() && agent_id == "anonymous")
        agent_id = engine.getRules().agent_id_config;
    engine.setAgentId(agent_id);
    engine.applyAgentRole();
    if (env_name.empty()) env_name = engine.getRules().default_env;
    if (!env_name.empty()) {
        if (!engine.getRules().environments.count(env_name)) {
            std::cerr << "naab-gov context: environment '" << env_name << "' is not in govern.json\n";
            return 4;
        }
        engine.applyEnvironment(env_name);
    }
    const GovernanceRules& rules = engine.getRules();

    if (kind.empty()) {
        if (findRole(rules, name)) kind = "agent";
        else if (naab::lang::findLanguage(name)) kind = "language";
        else kind = "rule";
    }

    const fs::path loaded = fs::path(engine.getLoadedPath());
    Ctx c{engine, loaded.parent_path(), {}, loaded};
    std::vector<std::string> notes;
    c.chain = readChain(loaded, notes);
    if (!key_given) key = GovernanceEngine::telemetryChainKeyFromEnvironment();
    if (telemetry.empty() && rules.telemetry_output.enabled && !rules.telemetry_output.output_file.empty()) {
        fs::path t = rules.telemetry_output.output_file;
        if (t.is_relative()) t = c.config_dir / t;
        telemetry = t.lexically_normal().string();
    }

    json out;
    out["verdict"] = "REPORT";
    out["target"] = {{"kind", kind}, {"name", plain(name, 200)}};
    json chain = json::array();
    for (const auto& f : c.chain) chain.push_back(plain(relTo(c.config_dir, f.path), 200));
    out["config"] = {{"file", plain(loaded.filename().string())},
                     {"chain", chain},
                     {"mode", engine.isActive() ? (rules.mode == naab::governance::GovernanceMode::AUDIT ? "audit" : "enforce") : "off"},
                     {"notes", notes}};
    json rule_entries = json::array();
    std::set<std::string> rule_names;
    auto addRule = [&](const std::string& r) {
        if (rule_names.insert(r).second) rule_entries.push_back(ruleEntry(c, r));
    };
    json guidance;
    if (!engine.guidanceEnabled()) guidance["off"] = true;

    if (kind == "rule") {
        const auto info = GovernanceEngine::ruleSectionInfo(rules, name);
        if (info.section.empty())
            return unmeasurable(as_json, "no section of govern.json governs a rule named " + plain(name, 200));
        addRule(name);
        if (!guidance.contains("off")) {
            size_t withheld = 0;
            auto d = engine.guidanceForRefusal(name, &withheld);
            guidance = guidanceList(d, withheld);
        }
        const std::string last = name.substr(name.rfind('.') + 1);
        out["project_context"] = projectContext(c, c.config_dir.string(), [&](const auto& x) {
            const std::string m = x.mapped_rule;
            const std::string mlast = m.substr(m.rfind('.') + 1);
            return !mlast.empty() && (mlast == last || name.find(mlast) != std::string::npos);
        });
        out["history"] = historyFor(c, telemetry, key,
                                    [&](const std::string& r, const std::string&) { return r == name; });
    } else if (kind == "language") {
        const std::string runtime = naab::lang::runtimeLanguage(name);
        std::string by;
        json l;
        l["runtime"] = plain(runtime);
        l["verdict"] = languageVerdict(rules, runtime, by);
        if (!by.empty()) { l["by"] = by; addRule(by); }
        if (const auto* role = findRole(rules, engine.getAgentId())) {
            std::string rv = "allowed";
            if (naab::lang::languageListBlocks(role->blocked_languages, runtime) ||
                (!role->allowed_languages.empty() && !naab::lang::languageListAdmits(role->allowed_languages, runtime)))
                rv = "refused";
            l["role"] = plain(role->name);
            l["role_verdict"] = rv;
        }
        l["separate_process_refused"] = !rules.capabilities.shell.enabled || !rules.shell_allowed;
        json settings = json::array();
        auto it = rules.languages.per_language.find(naab::lang::canonicalLanguage(name));
        if (it != rules.languages.per_language.end()) {
            const auto& lc = it->second;
            auto count = [&](const char* what, size_t n) {
                if (n) settings.push_back(std::string(what) + ": " + std::to_string(n) + " (not listed)");
            };
            count("banned functions", lc.banned_functions.size());
            count("banned globals", lc.banned_globals.size());
            count("banned keywords", lc.banned_keywords.size());
            count("banned imports", lc.banned_imports.size());
            count("banned namespaces", lc.banned_namespaces.size());
            count("banned includes", lc.banned_includes.size());
            count("banned commands", lc.banned_commands.size());
            if (lc.timeout > 0) settings.push_back("a time limit");
            if (lc.max_lines > 0) settings.push_back("a line limit");
            if (lc.max_output_size > 0) settings.push_back("an output size limit");
        }
        l["settings"] = settings;
        out["language"] = l;
        addRule("languages.per_language.banned_functions");
        if (!guidance.contains("off"))
            guidance = guidanceList(engine.guidancePull(name, "", 5, false), 0);
        const std::string canon = naab::lang::canonicalLanguage(name);
        out["project_context"] = projectContext(c, c.config_dir.string(), [&](const auto& x) {
            return x.language == canon || x.mapped_rule.rfind(canon + ".", 0) == 0 ||
                   x.mapped_rule.rfind("*.", 0) == 0;
        });
    } else if (kind == "agent") {
        const auto* role = findRole(rules, name);
        if (!role) return unmeasurable(as_json, "govern.json defines no agent role named " + plain(name, 200));
        json a;
        a["name"] = plain(role->name);
        json actions = json::array();
        for (const auto& x : role->allowed_actions) actions.push_back(plain(x));
        a["allowed_actions"] = actions;
        a["shell_allowed"] = role->shell_allowed;
        a["shell_content_allowed"] = role->shell_content_allowed;
        a["network_allowed"] = role->network_allowed;
        json al = json::array(), bl = json::array();
        for (const auto& x : role->allowed_languages) al.push_back(plain(x));
        for (const auto& x : role->blocked_languages) bl.push_back(plain(x));
        a["allowed_languages"] = al;
        a["blocked_languages"] = bl;
        a["allowed_paths"] = std::to_string(role->allowed_paths.size()) + " (not listed)";
        a["blocked_paths"] = std::to_string(role->blocked_paths.size()) + " (not listed)";
        a["tools_enabled"] = role->tools_enabled;
        json tools = json::array();
        for (const auto& x : role->tools) tools.push_back(plain(x));
        a["tools"] = tools;
        a["output_contract"] = role->output_contract.format.empty() ? "none" : plain(role->output_contract.format);
        json sig = json::object();
        for (const auto& [k, v] : role->context_drift_signals) sig[plain(k)] = v;
        a["drift_signal_overrides"] = sig;
        a["propose"] = role->propose_candidates_max > 0;
        a["standing_lease"] = role->standing_lease_turns > 0 || role->standing_lease_seconds > 0;
        a["context_strategy"] = plain(role->context_strategy);
        out["agent"] = a;
        for (const char* r : {"agent_role.action_matrix", "agent_role.shell", "agent_role.network", "agent_role.language"})
            addRule(r);
        if (!guidance.contains("off"))
            guidance = guidanceList(engine.guidancePull(name, "", 5, false), 0);
        out["project_context"] = projectContext(c, c.config_dir.string(),
                                                [](const auto&) { return false; });
    } else {  // file
        const fs::path abs = fs::absolute(name, ec).lexically_normal();
        json f;
        f["path"] = plain(relTo(c.config_dir, abs), 200);
        auto verdict = [&](bool write) {
            const std::string why = engine.pathPolicyDenial(abs.string(), write);
            return why.empty() ? std::string("allowed") : std::string("refused");
        };
        f["read"] = verdict(false);
        f["write"] = verdict(true);
        json prot = json::array();
        const std::string canon = engine.resolvePolicyPath(abs.string());
        if (engine.isEvidenceLogPath(canon)) prot.push_back("governance evidence (telemetry, transcript or audit trail)");
        if (engine.isEngineStatePath(canon) || engine.isEngineStateDir(canon)) prot.push_back("engine state");
        if (engine.isGuidanceSourcePath(canon)) prot.push_back("a guidance source");
        if (engine.isGovernancePluginPath(canon)) prot.push_back("a governance plugin");
        if (engine.isProjectContextPath(canon)) prot.push_back("a project file whose text became a rule");
        const std::string fname = abs.filename().string();
        if (fname == "govern.json" || fname == "govern.json.sig") prot.push_back("the governance configuration");
        f["protected_as"] = prot;
        std::string text;
        if (abs.extension() == ".naab") {
            std::ifstream in(abs, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        if (!text.empty()) {
            json blocks = json::array();
            for (const auto& lang : blockLanguages(text)) {
                std::string by;
                const std::string v = languageVerdict(rules, naab::lang::runtimeLanguage(lang), by);
                blocks.push_back({{"language", plain(lang)}, {"verdict", v}});
                if (!by.empty()) addRule(by);
            }
            f["blocks"] = blocks;
            json contracts = json::array();
            try {
                naab::lexer::Lexer lx(text);
                auto tokens = lx.tokenize();
                naab::parser::Parser parser(tokens);
                parser.setSource(text, abs.string());
                auto program = parser.parseProgram();
                for (const auto& fn : program->getFunctions()) {
                    auto it = rules.contracts.functions.find(fn->getName());
                    if (it == rules.contracts.functions.end()) continue;
                    const auto& fc = it->second;
                    json kinds = json::array();
                    if (!fc.must_call.empty()) kinds.push_back("must_call");
                    if (!fc.must_contain.empty()) kinds.push_back("must_contain");
                    if (!fc.must_produce.empty()) kinds.push_back("must_produce");
                    if (!fc.must_derive_from.empty()) kinds.push_back("must_derive_from");
                    if (!fc.must_vary.empty()) kinds.push_back("must_vary");
                    if (!fc.must_differentiate.empty()) kinds.push_back("must_differentiate");
                    if (!fc.must_handle_case.empty()) kinds.push_back("must_handle_case");
                    if (!fc.must_satisfy.empty()) kinds.push_back("must_satisfy");
                    if (!fc.requires_clauses.empty()) kinds.push_back("requires");
                    if (!fc.ensures_clauses.empty()) kinds.push_back("ensures");
                    if (fc.min_arity >= 0 || fc.max_arity >= 0) kinds.push_back("arity");
                    if (!fc.return_type.empty()) kinds.push_back("return_type");
                    contracts.push_back({{"function", plain(fn->getName())}, {"kinds", kinds}});
                    addRule("contracts." + fn->getName());
                }
                f["contracts"] = contracts;
            } catch (const std::exception&) {
                f["parse"] = "the file does not parse; its functions are not listed";
            }
            if (rules.manifest.required) {
                fs::path m = abs;
                m.replace_extension(".manifest.json");
                f["manifest"] = fs::exists(m, ec) ? "required, present beside the program (not verified here)"
                                                   : "required, and none is beside the program";
                addRule("manifest.required");
            }
        }
        out["file"] = f;
        const std::string rel = relTo(c.config_dir, abs);
        out["history"] = historyFor(c, telemetry, key, [&](const std::string&, const std::string& file) {
            return file == rel;
        });
        for (const auto& x : out["history"].value("remembered", json::array())) addRule(x["rule"].get<std::string>());
        if (!guidance.contains("off")) {
            json all = json::array();
            size_t withheld_total = 0;
            for (const auto& r : rule_names) {
                size_t w = 0;
                for (const auto& d : engine.guidanceForRefusal(r, &w))
                    all.push_back({{"source", plain(d.source)}, {"heading", plain(d.heading)}});
                withheld_total += w;
            }
            guidance["sections"] = all;
            if (withheld_total > 0) guidance["withheld"] = withheld_total;
        }
        out["project_context"] = projectContext(c, start.string(), [&](const auto& x) {
            return engine.resolvePolicyPath(x.source_path) == canon;
        });
    }
    out["rules"] = rule_entries;
    out["guidance"] = guidance;
    out["not_shown"] = notShown();
    out["not_judged"] = notJudged();

    if (as_json) std::cout << out.dump(2) << "\n";
    else printReport(out);
    return 0;
}
