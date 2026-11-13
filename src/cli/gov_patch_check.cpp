// naab-gov patch-check BASE PATCHED -- the gate a patch written by an agent
// must pass before a person looks at it (Bundle 15 PR 3).
//
// BASE and PATCHED are two directory trees: the project before the patch and
// after it. The command compares them and decides, deterministically, with no
// model anywhere. The agent that wrote the patch is outside the engine; a
// person still merges.
//
// REFUSED (exit 1) when the patch
//   - adds, changes or removes a GOVERNANCE file: any govern.json, any *.sig,
//     any *.manifest.json, anything under a trusted-keys directory, a file the
//     base config's `extends` chain or `governance_plugins` name, a project
//     context file the engine reads (CLAUDE.md, .cursorrules, ...), or anything
//     under `patch_check.protected_paths`. The reload ratchet's question --
//     may this config change? -- is answered the strict way: not by a patch;
//   - changes or removes an EXISTING test file (a file of BASE under
//     `patch_check.test_paths`, default tests/ and test/). Adding one is
//     allowed. This is the plan's "may not touch test assertions", judged by
//     the bytes: a count cannot tell a real assertion from `assert True`;
//   - adds or changes a symbolic link (a link names a file by another path);
//   - with --run-contracts: gives a function a contract counterexample, a
//     governance block or a parse failure that BASE did not have.
//
// PRINTED, deciding nothing: the lexical assertion-site count of the test
// paths before and after; what `naab-gov manifest infer` lists for each changed
// program that BASE's version did not; and the `naab-gov taint` flow counts.
//
// UNMEASURABLE (exit 2): BASE has no file under the test paths (so "tests did
// not shrink" says nothing); a tree is too large to compare; the base config
// has contract clauses and --run-contracts was not given (the clause check was
// not made); or the contract search could not measure a contracted function of
// a changed program (a worker that did not report included). A refusal
// outranks it.
//
// --run-contracts RUNS the patched program's functions (`naab-gov contracts`,
// in a governed worker). It is never run when the patch touched a governance
// file: the config the worker would load is then the patch's own.
//
// Exit: 0 accepted, 1 refused, 2 UNMEASURABLE, 4 usage or config error.
#include "naab/crypto_utils.h"
#include "naab/governance.h"
#include "naab/subprocess_helpers.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
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

constexpr size_t kMaxFiles = 200000;          // per tree
constexpr size_t kMaxPrintedPrograms = 40;    // changed programs given to the printed tools

struct Entry {
    bool is_link = false;
    std::string digest;   // sha256 of the bytes, or "link:" + target
};
using Tree = std::map<std::string, Entry>;   // generic relative path -> entry

std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (out.size() >= 200) { out += "..."; break; }
        out.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
    }
    return out;
}

// Walks `root` without following links. False when the tree is over the bound
// or cannot be read; `why` says which.
bool readTree(const fs::path& root, Tree& out, std::string& why) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec) { why = "cannot read " + root.string() + ": " + ec.message(); return false; }
    for (; it != end; it.increment(ec)) {
        if (ec) { why = "cannot read under " + root.string() + ": " + ec.message(); return false; }
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        const fs::file_status st = fs::symlink_status(p, ec);
        if (ec) { why = "cannot stat " + p.string(); return false; }
        if (fs::is_directory(st)) {
            // The version-control directory is not part of the project's files.
            if (name == ".git") it.disable_recursion_pending();
            continue;
        }
        // Lexical: fs::relative() resolves a link and would name its target.
        const std::string rel = p.lexically_relative(root).generic_string();
        if (rel.empty() || rel.rfind("..", 0) == 0) { why = "cannot name " + p.string(); return false; }
        Entry e;
        if (fs::is_symlink(st)) {
            e.is_link = true;
            e.digest = "link:" + fs::read_symlink(p, ec).generic_string();
        } else if (fs::is_regular_file(st)) {
            e.digest = naab::security::CryptoUtils::sha256File(p.string());
        } else {
            e.digest = "special";
        }
        out[rel] = std::move(e);
        if (out.size() > kMaxFiles) {
            why = root.string() + " holds more than " + std::to_string(kMaxFiles) + " files";
            return false;
        }
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// `prefix` names a directory (or one file) relative to the root. Compared
// without case: on a case-insensitive filesystem Tests/ is tests/.
bool underPrefix(const std::string& rel, std::string prefix) {
    while (!prefix.empty() && prefix.rfind("./", 0) == 0) prefix.erase(0, 2);
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    if (prefix.empty()) return false;
    const std::string r = lower(rel), p = lower(prefix);
    return r == p || (r.size() > p.size() && r.compare(0, p.size(), p) == 0 && r[p.size()] == '/');
}

// The project context files the engine reads into governance
// (project_context.cpp, LLM_FILES), matched at any depth: discovery walks up.
const char* const kContextFiles[] = {"claude.md", ".cursorrules", "gemini.md", ".aider.conf.yml"};
const char* const kContextPaths[] = {".github/copilot-instructions.md", ".claude/settings.json",
                                     ".gemini/settings.json", ".continue/config.json"};

// Why `rel` is a governance file, or "".
std::string governanceReason(const std::string& rel, const std::set<std::string>& named,
                             const std::vector<std::string>& protected_paths) {
    const std::string r = lower(rel);
    const std::string base = r.substr(r.rfind('/') == std::string::npos ? 0 : r.rfind('/') + 1);
    if (base == "govern.json") return "a govern.json";
    if (endsWith(base, ".sig")) return "a signature";
    if (endsWith(base, ".manifest.json")) return "a capability manifest";
    if (r.rfind("trusted-keys/", 0) == 0 || r.find("/trusted-keys/") != std::string::npos)
        return "a trusted key";
    for (const char* n : kContextFiles) if (base == n) return "a project context file the engine reads";
    for (const char* n : kContextPaths)
        if (r == n || endsWith(r, std::string("/") + n)) return "a project context file the engine reads";
    if (named.count(r)) return "named by the base config (extends or governance_plugins)";
    for (const auto& p : protected_paths)
        if (underPrefix(rel, p)) return "under patch_check.protected_paths";
    return "";
}

bool isTestPath(const std::string& rel, const std::vector<std::string>& test_paths) {
    for (const auto& p : test_paths) if (underPrefix(rel, p)) return true;
    return false;
}

// Assertion sites, lexically: words that begin `assert`, `ASSERT_` or
// `EXPECT_`. Printed only. It reads `assert True` as an assertion and a
// skipped test as a test, which is why it decides nothing.
long assertionSites(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return 0;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > (8u << 20)) return 0;
    long n = 0;
    auto word = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    for (size_t i = 0; i < text.size(); ++i) {
        if (i > 0 && word(static_cast<unsigned char>(text[i - 1]))) continue;
        if (text.compare(i, 6, "assert") == 0 || text.compare(i, 7, "ASSERT_") == 0 ||
            text.compare(i, 7, "EXPECT_") == 0)
            ++n;
    }
    return n;
}

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

int runSelf(const std::string& self, const std::vector<std::string>& args, std::string& out) {
    std::string err;
    return naab::runtime::execute_subprocess_with_pipes(
        self, args, out, err, nullptr, nullptr, naab::runtime::ChildStdin::Null);
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

// The files BASE's config names beyond itself: each `extends` parent (followed
// up to 8 levels) and each governance plugin, as lower-case relative paths.
// Read from the raw JSON: the question is which FILES are governance, not what
// they say. A name that leaves the tree is not in the tree to be patched.
void namedByConfig(const fs::path& root, const fs::path& config, std::set<std::string>& out) {
    fs::path cur = config;
    for (int depth = 0; depth < 8; ++depth) {
        std::ifstream f(cur, std::ios::binary);
        if (!f.is_open()) return;
        std::stringstream ss;
        ss << f.rdbuf();
        json j = json::parse(ss.str(), nullptr, false);
        if (!j.is_object()) return;
        auto add = [&](const fs::path& p) {
            std::error_code ec;
            const std::string rel = fs::relative(fs::weakly_canonical(p, ec), root, ec).generic_string();
            if (!ec && !rel.empty() && rel.rfind("..", 0) != 0) out.insert(lower(rel));
        };
        add(cur);
        if (j.contains("governance_plugins") && j["governance_plugins"].is_array())
            for (const auto& gp : j["governance_plugins"])
                if (gp.is_object() && gp.contains("file") && gp["file"].is_string())
                    add(cur.parent_path() / gp["file"].get<std::string>());
        if (!j.contains("extends") || !j["extends"].is_string()) return;
        cur = cur.parent_path() / j["extends"].get<std::string>();
    }
}

struct ContractVerdicts {
    int exit = -1;
    std::map<std::string, std::string> by_function;   // declared functions only
};

ContractVerdicts contractsOf(const std::string& self, const fs::path& file, const std::string& naab) {
    ContractVerdicts v;
    std::string out;
    std::vector<std::string> a = {"contracts", file.string(), "--json"};
    if (!naab.empty()) { a.push_back("--naab"); a.push_back(naab); }
    v.exit = runSelf(self, a, out);
    json j = json::parse(out, nullptr, false);
    if (j.is_object() && j.contains("functions") && j["functions"].is_array()) {
        for (const auto& f : j["functions"]) {
            if (!f.is_object() || !f.contains("function") || !f["function"].is_string()) continue;
            if (f.value("why", std::string()) == "not declared in this file") continue;
            v.by_function[f["function"].get<std::string>()] = f.value("verdict", std::string("UNMEASURABLE"));
        }
    }
    return v;
}

}  // namespace

int cmdPatchCheck(const std::vector<std::string>& args, const char* argv0) {
    std::string base_arg, patched_arg, config_path, naab_path;
    bool as_json = false, run_contracts = false;
    auto usage = [](const std::string& why) {
        std::cerr << "naab-gov patch-check: " << why << "\n"
                     "Usage: naab-gov patch-check <base-dir> <patched-dir> [--config <govern.json>] "
                     "[--run-contracts [--naab <naab-lang>]] [--json]\n";
        return 4;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--config") {
            if (i + 1 >= args.size()) return usage("--config needs a path");
            config_path = args[++i];
        } else if (a == "--naab") {
            if (i + 1 >= args.size()) return usage("--naab needs a path");
            naab_path = args[++i];
        } else if (a == "--json") {
            as_json = true;
        } else if (a == "--run-contracts") {
            run_contracts = true;
        } else if (!a.empty() && a[0] != '-' && base_arg.empty()) {
            base_arg = a;
        } else if (!a.empty() && a[0] != '-' && patched_arg.empty()) {
            patched_arg = a;
        } else {
            return usage("unknown option: " + a);
        }
    }
    if (base_arg.empty() || patched_arg.empty()) return usage("expected a base and a patched directory");
    std::error_code ec;
    if (!fs::is_directory(base_arg, ec)) return usage("not a directory: " + base_arg);
    if (!fs::is_directory(patched_arg, ec)) return usage("not a directory: " + patched_arg);
    const fs::path base = fs::weakly_canonical(base_arg, ec);
    const fs::path patched = fs::weakly_canonical(patched_arg, ec);
    if (base == patched) return usage("the base and the patched directory are the same");

    // The config: BASE's own. PATCHED's is never read for a decision -- a patch
    // does not get to say what its gate checks.
    detachStdin();
    naab::governance::GovernanceEngine engine;
    fs::path config_file;
    bool have_config = false;
    if (!config_path.empty()) {
        if (!fs::is_regular_file(config_path, ec)) return usage("govern.json not found: " + config_path);
        config_file = fs::weakly_canonical(config_path, ec);
    } else if (fs::is_regular_file(base / "govern.json", ec)) {
        config_file = base / "govern.json";
    }
    if (!config_file.empty()) {
        if (!engine.loadFromFile(config_file.string())) {
            std::cerr << "naab-gov patch-check: govern.json does not load: " << engine.getLastError() << "\n";
            return 4;
        }
        have_config = true;
    }
    std::vector<std::string> test_paths = {"tests/", "test/"};
    std::vector<std::string> protected_paths;
    bool config_has_clauses = false;
    if (have_config) {
        const auto& pc = engine.getRules().patch_check;
        if (!pc.test_paths.empty()) test_paths = pc.test_paths;
        protected_paths = pc.protected_paths;
        for (const auto& [name, c] : engine.getRules().contracts.functions)
            if (!c.requires_clauses.empty() || !c.ensures_clauses.empty()) config_has_clauses = true;
    }
    std::set<std::string> named;
    if (have_config) namedByConfig(base, config_file, named);

    std::vector<std::string> refused, unmeasurable, notes;
    Tree bt, pt;
    std::string why;
    if (!readTree(base, bt, why) || !readTree(patched, pt, why)) {
        if (as_json) std::cout << json({{"result", "unmeasurable"}, {"unmeasurable", {why}}}).dump(2) << "\n";
        else std::cout << "naab-gov patch-check: UNMEASURABLE -- " << why << "\n";
        return 2;
    }

    // What the patch did, file by file.
    std::vector<std::string> added, changed, removed;
    for (const auto& [rel, e] : pt) {
        auto it = bt.find(rel);
        if (it == bt.end()) added.push_back(rel);
        else if (it->second.digest != e.digest || it->second.is_link != e.is_link) changed.push_back(rel);
    }
    for (const auto& [rel, e] : bt) if (!pt.count(rel)) removed.push_back(rel);

    bool governance_touched = false;
    auto judge = [&](const std::string& rel, const char* what) {
        const std::string g = governanceReason(rel, named, protected_paths);
        if (!g.empty()) {
            refused.push_back(std::string(what) + " " + printable(rel) + ": " + g);
            governance_touched = true;
        }
    };
    for (const auto& rel : added) judge(rel, "adds");
    for (const auto& rel : changed) judge(rel, "changes");
    for (const auto& rel : removed) judge(rel, "removes");

    for (const auto& rel : changed)
        if (isTestPath(rel, test_paths))
            refused.push_back("changes " + printable(rel) + ": an existing test file");
    for (const auto& rel : removed)
        if (isTestPath(rel, test_paths))
            refused.push_back("removes " + printable(rel) + ": an existing test file");

    for (const auto& rel : added)
        if (pt[rel].is_link) refused.push_back("adds " + printable(rel) + ": a symbolic link");
    for (const auto& rel : changed)
        if (pt[rel].is_link) refused.push_back("changes " + printable(rel) + ": a symbolic link");

    // The test paths: how much BASE holds, and what was added beside it.
    long base_tests = 0, base_sites = 0, patched_sites = 0;
    std::vector<std::string> tests_added;
    for (const auto& [rel, e] : bt)
        if (!e.is_link && isTestPath(rel, test_paths)) { ++base_tests; base_sites += assertionSites(base / rel); }
    for (const auto& [rel, e] : pt)
        if (!e.is_link && isTestPath(rel, test_paths)) patched_sites += assertionSites(patched / rel);
    for (const auto& rel : added) if (isTestPath(rel, test_paths)) tests_added.push_back(rel);
    if (base_tests == 0)
        unmeasurable.push_back("the base holds no file under the test paths, so nothing shows the tests were kept");

    // Changed and added programs: the printed tools, and the contract search.
    std::vector<std::string> programs;
    for (const auto& rel : added) if (endsWith(lower(rel), ".naab") && !pt[rel].is_link) programs.push_back(rel);
    for (const auto& rel : changed) if (endsWith(lower(rel), ".naab") && !pt[rel].is_link) programs.push_back(rel);
    std::sort(programs.begin(), programs.end());

    const std::string self = selfPath(argv0);
    json printed = json::array();
    if (!governance_touched) {
        size_t shown = 0;
        for (const auto& rel : programs) {
            if (shown++ >= kMaxPrintedPrograms) {
                notes.push_back(std::to_string(programs.size() - kMaxPrintedPrograms) +
                                " more changed program(s) were not given to the printed tools");
                break;
            }
            json row = {{"program", rel}};
            // Capabilities the draft manifest lists now and did not before.
            auto draft = [&](const fs::path& f) {
                std::string out;
                if (!fs::is_regular_file(f) || runSelf(self, {"manifest", "infer", f.string(), "--stdout"}, out) != 0)
                    return json();
                return json::parse(out, nullptr, false);
            };
            const json before = draft(base / rel), after = draft(patched / rel);
            json grew = json::array();
            if (after.is_object()) {
                auto list = [&](const json& d, const char* k, const char* sub) -> std::set<std::string> {
                    std::set<std::string> s;
                    if (!d.is_object() || !d.contains(k)) return s;
                    const json& v = sub ? d[k].value(sub, json::array()) : d[k];
                    if (v.is_array()) for (const auto& x : v) if (x.is_string()) s.insert(x.get<std::string>());
                    return s;
                };
                const std::pair<const char*, const char*> keys[] = {
                    {"actions", nullptr}, {"languages", nullptr}, {"hosts", nullptr}, {"agents", nullptr}};
                for (const auto& [k, sub] : keys)
                    for (const auto& x : list(after, k, sub))
                        if (!list(before, k, sub).count(x)) grew.push_back(std::string(k) + ": " + printable(x));
                for (const char* sub : {"read", "write"})
                    for (const auto& x : list(after, "files", sub))
                        if (!list(before, "files", sub).count(x))
                            grew.push_back(std::string("files ") + sub + ": " + printable(x));
            } else {
                grew.push_back("(naab-gov manifest infer gave no draft for the patched file)");
            }
            row["manifest_new"] = grew;
            auto flows = [&](const fs::path& f) -> int {
                std::string out;
                if (!fs::is_regular_file(f)) return 0;
                const int rc = runSelf(self, {"taint", f.string(), "--json"}, out);
                const json j = json::parse(out, nullptr, false);
                if ((rc != 0 && rc != 1) || !j.is_object() || !j.contains("flows")) return -1;
                return static_cast<int>(j["flows"].size());
            };
            row["taint_flows_before"] = flows(base / rel);
            row["taint_flows_after"] = flows(patched / rel);
            printed.push_back(row);
        }
    } else if (!programs.empty()) {
        notes.push_back("the patch touched a governance file, so no tool was run on the patched programs");
    }

    json contract_rows = json::array();
    if (config_has_clauses && !programs.empty()) {
        if (governance_touched) {
            // Already refused; the patched code is not run under its own config.
        } else if (!run_contracts) {
            unmeasurable.push_back("the base config has contract clauses and --run-contracts was not given, "
                                   "so the clauses were not checked on the changed programs");
        } else {
            for (const auto& rel : programs) {
                const bool in_base = fs::is_regular_file(base / rel);
                const ContractVerdicts after = contractsOf(self, patched / rel, naab_path);
                const ContractVerdicts before = in_base ? contractsOf(self, base / rel, naab_path) : ContractVerdicts{};
                json row = {{"program", rel}, {"exit_before", in_base ? before.exit : -1}, {"exit_after", after.exit}};
                if (after.exit == 4 && !(in_base && before.exit == 4))
                    refused.push_back(printable(rel) + ": the patched file does not parse, or its contracts cannot be read");
                else if (after.exit == 3 && !(in_base && before.exit == 3))
                    refused.push_back(printable(rel) + ": governance blocks the contract search of the patched file");
                else if (after.exit < 0 || after.exit > 4)
                    unmeasurable.push_back(printable(rel) + ": the contract search did not report");
                for (const auto& [fn, verdict] : after.by_function) {
                    auto b = before.by_function.find(fn);
                    const std::string was = b == before.by_function.end() ? "" : b->second;
                    if (verdict == "COUNTEREXAMPLE" && was != "COUNTEREXAMPLE")
                        refused.push_back(printable(rel) + ": " + printable(fn) + " breaks a contract clause it did not break before");
                    else if (verdict == "UNMEASURABLE")
                        // Whatever the base read: a clause the search could not
                        // test in the patch was not checked. A worker that did
                        // not report lands here too, so a dead interpreter can
                        // never read as "no counterexample".
                        unmeasurable.push_back(printable(rel) + ": the contract search could not measure " + printable(fn) + " in the patch");
                }
                // A contracted function the patch removed from the file is no
                // longer checked there at all.
                for (const auto& [fn, was] : before.by_function)
                    if (!after.by_function.count(fn) && after.exit != 4)
                        unmeasurable.push_back(printable(rel) + ": " + printable(fn) + " had a contract in the base and is not declared in the patch");
                contract_rows.push_back(row);
            }
        }
    }

    const char* result = !refused.empty() ? "refused" : !unmeasurable.empty() ? "unmeasurable" : "accepted";
    const int status = !refused.empty() ? 1 : !unmeasurable.empty() ? 2 : 0;

    if (as_json) {
        json j;
        j["base"] = base.string();
        j["patched"] = patched.string();
        j["config"] = have_config ? config_file.string() : "";
        j["result"] = result;
        j["files"] = {{"added", added.size()}, {"changed", changed.size()}, {"removed", removed.size()}};
        j["refused"] = refused;
        j["unmeasurable"] = unmeasurable;
        j["notes"] = notes;
        j["tests"] = {{"paths", test_paths}, {"base_files", base_tests}, {"added", tests_added},
                      {"assertion_sites_before", base_sites}, {"assertion_sites_after", patched_sites}};
        j["printed_only"] = printed;
        j["contracts"] = contract_rows;
        std::cout << j.dump(2) << "\n";
        return status;
    }

    std::cout << "naab-gov patch-check: " << base.string() << " -> " << patched.string() << "\n"
              << "  config: " << (have_config ? config_file.string() : "none (the built-in rules only)") << "\n"
              << "  files: " << added.size() << " added, " << changed.size() << " changed, "
              << removed.size() << " removed\n";
    std::cout << "\nRefused (" << refused.size() << "):\n";
    if (refused.empty()) std::cout << "  none\n";
    for (const auto& r : refused) std::cout << "  - " << r << "\n";
    if (!unmeasurable.empty()) {
        std::cout << "\nNot measured (" << unmeasurable.size() << "):\n";
        for (const auto& u : unmeasurable) std::cout << "  - " << u << "\n";
    }
    std::cout << "\nTests (paths:";
    for (const auto& p : test_paths) std::cout << " " << printable(p);
    std::cout << "): " << base_tests << " file(s) in the base, each unchanged unless listed above; "
              << tests_added.size() << " added\n";
    for (const auto& t : tests_added) std::cout << "  + " << printable(t) << "\n";
    if (!tests_added.empty())
        std::cout << "  (an added file can change how existing tests run -- a conftest.py, a sourced helper; "
                     "read each)\n";
    std::cout << "  assertion sites, counted lexically (printed only; it cannot tell a real assertion from "
                 "`assert True`): " << base_sites << " -> " << patched_sites << "\n";
    if (!printed.empty()) {
        std::cout << "\nPrinted only (these decide nothing):\n";
        for (const auto& row : printed) {
            std::cout << "  " << printable(row["program"].get<std::string>()) << ": taint flows "
                      << row["taint_flows_before"] << " -> " << row["taint_flows_after"]
                      << " (-1: no report)\n";
            for (const auto& g : row["manifest_new"])
                std::cout << "    the draft manifest newly lists " << g.get<std::string>() << "\n";
        }
    }
    for (const auto& n : notes) std::cout << "  note: " << n << "\n";
    std::cout << "\nresult: " << (status == 0 ? "ACCEPTED -- a person still reviews and merges"
                                  : status == 1 ? "REFUSED" : "UNMEASURABLE") << "\n";
    return status;
}
