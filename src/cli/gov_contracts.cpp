// naab-gov contracts FILE -- the counterexample search (Bundle 11).
//
// For every function FILE declares that govern.json gives a requires or
// ensures clause, generate inputs (deterministically, from --seed), run them
// in a `naab-lang run --contract-search` worker under the project's govern.json,
// set aside the inputs that do not meet requires, and report the first input
// that meets them and still breaks a clause -- its own ensures, or a clause of
// a function it calls.
//
// Opt-in and separate from `naab-gov check`, which never runs code: this RUNS
// the program's functions with generated arguments, in a worker process, with
// every effect governed as in a run. A program can game its own search (the
// worker runs its code); what keeps it honest is that the same clauses are
// enforced at every call when the program runs.
//
// Exit: 0 no counterexample found, 1 a counterexample, 2 UNMEASURABLE (nothing
// was tested: no contracted function, or no generated input met requires, or
// the worker did not report), 3 a governance block during the search, 4 a
// config or usage error.
#include "naab/contract_expr.h"
#include "naab/governance.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/ast.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include "../api/worker.h"
#include <cstdlib>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Target {
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> types;  // contract "params" ("name:type"), else "any"
    std::vector<std::string> requires_clauses, ensures_clauses;
};

// The values a parameter of each type is tried with, simplest first (the
// first cases enumerate them; the rest are drawn at random). Plain JSON, so the
// worker reads exactly these.
std::vector<json> pool(const std::string& type, const std::vector<long long>& literals) {
    std::vector<json> ints = {0, 1, -1, 2, -2, 3, 10, -10, 100, -100, 2147483647, -2147483647};
    // Boundaries of the clauses' own numbers: `n >= 10` is tried at 9, 10, 11.
    for (long long l : literals) {
        for (long long d : {-1LL, 0LL, 1LL}) {
            long long v = l + d;
            if (v >= -2147483647LL && v <= 2147483647LL) ints.push_back(v);
        }
    }
    std::vector<json> floats = {0.0, 0.5, -0.5, 1.5, -1.5, 1e9};
    std::vector<json> strings = {"", "a", "ab", "A", " ", "0", "-1", "hello"};
    std::vector<json> bools = {true, false};
    std::vector<json> lists = {json::array(), json::array({0}), json::array({1, 2, 3}),
                               json::array({-1}), json::array({"a"}), json::array({1, "a"})};
    std::vector<json> dicts = {json::object(), json{{"k", 1}}, json{{"a", 1}, {"b", 2}}};
    std::vector<json> out;
    auto add = [&](const std::vector<json>& v) { out.insert(out.end(), v.begin(), v.end()); };
    if (type == "int") add(ints);
    else if (type == "float") { add(floats); add(ints); }
    else if (type == "string") add(strings);
    else if (type == "bool") add(bools);
    else if (type == "array" || type == "list") add(lists);
    else if (type == "dict") add(dicts);
    else {
        add(ints); add(strings); add(bools); out.push_back(nullptr);
        add(floats); add(lists); add(dicts);
    }
    // Keep the first occurrence of each value, in order.
    std::vector<json> uniq;
    std::set<std::string> seen;
    for (auto& v : out) {
        if (seen.insert(v.dump()).second) uniq.push_back(v);
    }
    return uniq;
}

std::vector<json> generateCases(const Target& t, int count, uint32_t seed) {
    std::vector<long long> literals;
    for (const auto* list : {&t.requires_clauses, &t.ensures_clauses}) {
        for (const auto& c : *list) {
            for (long long l : naab::contracts::contractClauseIntLiterals(c)) literals.push_back(l);
        }
    }
    std::vector<std::vector<json>> pools;
    for (size_t i = 0; i < t.params.size(); ++i) pools.push_back(pool(t.types[i], literals));

    std::vector<json> cases;
    std::set<std::string> seen;
    auto push = [&](json args) {
        if (seen.insert(args.dump()).second) cases.push_back(std::move(args));
    };
    if (t.params.empty()) {
        push(json::array());
        return cases;
    }
    // First half: enumerate the pools in order (mixed radix), so the simplest
    // combinations come first and a counterexample found is a small one.
    const int systematic = std::max(1, count / 2);
    for (int i = 0; i < systematic && static_cast<int>(cases.size()) < count; ++i) {
        json args = json::array();
        long long rest = i;
        bool wrapped = false;
        for (const auto& p : pools) {
            args.push_back(p[static_cast<size_t>(rest % static_cast<long long>(p.size()))]);
            rest /= static_cast<long long>(p.size());
        }
        if (rest > 0) wrapped = true;
        if (wrapped) break;  // every combination enumerated
        push(std::move(args));
    }
    // Then random draws. mt19937's sequence is fixed by the standard; the
    // distributions are not, so values are taken from its raw output.
    std::mt19937 rng(seed);
    int attempts = 0;
    while (static_cast<int>(cases.size()) < count && attempts < count * 20) {
        ++attempts;
        json args = json::array();
        for (const auto& p : pools) {
            uint32_t r = rng();
            if (r % 4 == 0) {
                // a fresh integer in [-1000, 1000]
                args.push_back(static_cast<long long>(rng() % 2001) - 1000);
            } else {
                args.push_back(p[r % p.size()]);
            }
        }
        push(std::move(args));
    }
    return cases;
}

std::string selfDir(const char* argv0) {
#ifndef _WIN32
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p.parent_path().string();
#endif
    if (argv0 && std::string(argv0).find('/') != std::string::npos) {
        return fs::absolute(fs::path(argv0)).parent_path().string();
    }
    return "";
}

}  // namespace

int cmdContracts(const std::vector<std::string>& args, const char* argv0) {
    std::string file, naab_path;
    int cases_per_fn = 200;
    uint32_t seed = 1;
    int timeout = 60;
    bool as_json = false;
    auto usage = [](const std::string& why) {
        std::cerr << "naab-gov contracts: " << why << "\n"
                     "Usage: naab-gov contracts <file.naab> [--seed N] [--cases N] "
                     "[--timeout S] [--naab PATH] [--json]\n";
        return 4;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto num = [&](long long lo, long long hi, long long& out) {
            if (i + 1 >= args.size()) return false;
            try {
                size_t used = 0;
                long long v = std::stoll(args[i + 1], &used);
                if (used != args[i + 1].size() || v < lo || v > hi) return false;
                out = v;
                ++i;
                return true;
            } catch (...) { return false; }
        };
        long long v = 0;
        if (a == "--seed") { if (!num(0, 4294967295LL, v)) return usage("--seed needs 0..4294967295"); seed = static_cast<uint32_t>(v); }
        else if (a == "--cases") { if (!num(1, 100000, v)) return usage("--cases needs 1..100000"); cases_per_fn = static_cast<int>(v); }
        else if (a == "--timeout") { if (!num(1, 3600, v)) return usage("--timeout needs 1..3600 seconds"); timeout = static_cast<int>(v); }
        else if (a == "--naab" && i + 1 < args.size()) naab_path = args[++i];
        else if (a == "--json") as_json = true;
        else if (!a.empty() && a[0] != '-' && file.empty()) file = a;
        else return usage("unknown option: " + a);
    }
    if (file.empty()) return usage("expected a .naab file");
#ifdef _WIN32
    (void)argv0; (void)seed; (void)timeout; (void)as_json; (void)cases_per_fn;
    std::cerr << "naab-gov contracts: the search runs in a worker process, which this "
                 "platform does not support\n";
    return 4;
#else
    if (!fs::is_regular_file(file)) return usage("not a file: " + file);
    const fs::path abs = fs::absolute(file).lexically_normal();
    const std::string dir = abs.parent_path().string();

    // The functions the file declares, with their parameter names.
    std::string source;
    {
        std::ifstream f(abs);
        std::ostringstream ss;
        ss << f.rdbuf();
        source = ss.str();
    }
    std::map<std::string, std::vector<std::string>> declared;
    try {
        naab::lexer::Lexer lex(source);
        auto tokens = lex.tokenize();
        naab::parser::Parser parser(tokens);
        parser.setSource(source, abs.string());
        auto program = parser.parseProgram();
        for (const auto& fn : program->getFunctions()) {
            std::vector<std::string> ps;
            for (const auto& p : fn->getParams()) ps.push_back(p.name);
            declared[fn->getName()] = ps;
        }
    } catch (const std::exception& e) {
        std::cerr << "naab-gov contracts: " << abs.string() << " does not parse: " << e.what() << "\n";
        return 4;
    }

    // The contracts, from the govern.json a run of this file would find.
    naab::governance::GovernanceEngine engine;
    if (!engine.discoverAndLoad(dir)) {
        if (engine.getLastError() == "not_found") {
            std::cout << "naab-gov contracts: UNMEASURABLE -- no govern.json is found from "
                      << dir << ", so no function has a contract\n";
            return 2;
        }
        std::cerr << "naab-gov contracts: govern.json does not load: " << engine.getLastError() << "\n";
        return 4;
    }
    std::vector<Target> targets;
    std::vector<std::string> not_declared;
    for (const auto& [name, contract] : engine.getRules().contracts.functions) {
        if (contract.requires_clauses.empty() && contract.ensures_clauses.empty()) continue;
        auto it = declared.find(name);
        if (it == declared.end()) { not_declared.push_back(name); continue; }
        Target t;
        t.name = name;
        t.params = it->second;
        t.requires_clauses = contract.requires_clauses;
        t.ensures_clauses = contract.ensures_clauses;
        for (const auto& p : t.params) {
            std::string type = "any";
            for (const auto& spec : contract.params) {
                auto colon = spec.find(':');
                if (colon != std::string::npos && spec.substr(0, colon) == p) type = spec.substr(colon + 1);
            }
            t.types.push_back(type);
        }
        targets.push_back(std::move(t));
    }

    // The cases, in one file the worker reads.
    json case_list = json::array();
    struct CaseRef { size_t target; json args; };
    std::vector<CaseRef> refs;
    for (size_t ti = 0; ti < targets.size(); ++ti) {
        for (auto& a : generateCases(targets[ti], cases_per_fn, seed)) {
            case_list.push_back({{"function", targets[ti].name}, {"args", a}});
            refs.push_back({ti, std::move(a)});
        }
    }

    struct Tally {
        int cases = 0, met = 0, threw = 0, returned = 0, missing = 0;
        bool found = false;
        json counterexample;
    };
    std::vector<Tally> tally(targets.size());
    int worker_rc = -1;
    std::string worker_err;
    bool reported = false;
    std::string unmeasurable;

    if (!refs.empty()) {
        if (naab_path.empty()) {
            std::string d = selfDir(argv0);
            naab_path = d.empty() ? "naab-lang" : (fs::path(d) / "naab-lang").string();
        }
        if (!fs::is_regular_file(naab_path)) {
            std::cerr << "naab-gov contracts: naab-lang not found at " << naab_path
                      << " (give it with --naab PATH)\n";
            return 4;
        }
        naab_path = fs::absolute(naab_path).string();

        const char* tmpdir_env = std::getenv("TMPDIR");
        std::string tmpl_s = std::string(tmpdir_env && *tmpdir_env ? tmpdir_env : "/tmp") +
                             "/naab-contracts-XXXXXX";
        std::vector<char> tbuf(tmpl_s.begin(), tmpl_s.end());
        tbuf.push_back('\0');
        char* made = mkdtemp(tbuf.data());
        if (!made) {
            std::cerr << "naab-gov contracts: cannot make a temporary directory\n";
            return 4;
        }
        const std::string tdir = made;
        const std::string cases_path = tdir + "/cases.json";
        {
            std::ofstream cf(cases_path);
            cf << json{{"cases", case_list}}.dump();
        }

        std::vector<json> records;
        bool done_seen = false;
        long long done_count = -1;
        int done_lines = 0;
        auto fl = std::make_shared<naab::api::InFlight>();
        auto r = naab::api::runWorker(
            naab_path, dir,
            {"--contract-search", cases_path, "--event-stream-fd", "3",
             "--timeout", std::to_string(timeout), abs.string()},
            timeout, fl,
            [&](const std::string& line) {
                json j = json::parse(line, nullptr, false);
                if (j.is_discarded() || !j.is_object()) return false;
                if (j.contains("contract_search_case")) { records.push_back(j["contract_search_case"]); return true; }
                if (j.contains("contract_search_done")) {
                    ++done_lines;
                    done_seen = true;
                    const auto& d = j["contract_search_done"];
                    if (d.contains("cases") && d["cases"].is_number_integer()) done_count = d["cases"].get<long long>();
                    return true;
                }
                return false;
            });
        std::error_code ec;
        fs::remove_all(tdir, ec);
        worker_rc = (r.started && !r.timed_out && r.term_signal == 0) ? r.exit_code : -1;
        worker_err = r.err;

        if (!r.started) unmeasurable = "the worker did not start";
        else if (r.timed_out) unmeasurable = "the worker ran out of time (--timeout)";
        else if (r.term_signal != 0) unmeasurable = "the worker was killed by signal " + std::to_string(r.term_signal);
        else if (worker_rc == 3) {
            // A governance block during the search: as in a run.
        } else if (worker_rc == 4) {
            std::cerr << worker_err;
            return 4;
        } else if (!done_seen || done_lines != 1) {
            unmeasurable = done_lines > 1 ? "the worker reported more than once"
                                          : "the worker did not finish the search";
        } else if (done_count != static_cast<long long>(refs.size()) ||
                   records.size() != refs.size()) {
            unmeasurable = "the worker reported " + std::to_string(records.size()) + " of " +
                           std::to_string(refs.size()) + " cases";
        } else {
            reported = true;
        }
        if (reported) {
            for (const auto& rec : records) {
                if (!rec.contains("index") || !rec["index"].is_number_integer()) { reported = false; break; }
                long long idx = rec["index"].get<long long>();
                if (idx < 0 || idx >= static_cast<long long>(refs.size())) { reported = false; break; }
                const auto& ref = refs[static_cast<size_t>(idx)];
                Tally& tl = tally[ref.target];
                ++tl.cases;
                std::string outcome = rec.value("outcome", "");
                if (outcome == "requires_unmet") continue;
                if (outcome == "missing" || outcome == "bad_case") { ++tl.missing; continue; }
                ++tl.met;
                if (outcome == "returned") ++tl.returned;
                else if (outcome == "threw") ++tl.threw;
                else if (outcome == "violated" && !tl.found) {
                    tl.found = true;
                    tl.counterexample = rec;
                    tl.counterexample["args"] = ref.args;
                }
            }
            if (!reported) unmeasurable = "the worker's report does not match the cases sent";
        }
    }

    // Report.
    int found = 0, measured = 0;
    json out = {{"file", abs.string()}, {"seed", seed}, {"cases_per_function", cases_per_fn},
                {"functions", json::array()}};
    for (size_t ti = 0; ti < targets.size(); ++ti) {
        const auto& t = targets[ti];
        const auto& tl = tally[ti];
        json fj = {{"function", t.name}, {"cases", tl.cases}, {"met_requires", tl.met},
                   {"returned", tl.returned}, {"threw", tl.threw}};
        std::string verdict;
        if (!reported) verdict = "UNMEASURABLE";
        else if (tl.found) { verdict = "COUNTEREXAMPLE"; ++found; ++measured; fj["counterexample"] = tl.counterexample; }
        else if (tl.met == 0) verdict = "UNMEASURABLE";
        else { verdict = "NONE_FOUND"; ++measured; }
        fj["verdict"] = verdict;
        out["functions"].push_back(fj);
    }
    for (const auto& n : not_declared) {
        out["functions"].push_back({{"function", n}, {"verdict", "UNMEASURABLE"},
                                    {"why", "not declared in this file"}});
    }
    int rc;
    if (worker_rc == 3) rc = 3;
    else if (found > 0) rc = 1;
    else if (measured == 0) rc = 2;
    else rc = 0;
    out["exit"] = rc;
    if (!unmeasurable.empty()) out["unmeasurable"] = unmeasurable;

    if (as_json) {
        std::cout << out.dump(2) << "\n";
    } else {
        std::cout << "naab-gov contracts: " << abs.filename().string() << " (seed " << seed
                  << ", up to " << cases_per_fn << " cases per function)\n";
        if (worker_rc == 3) {
            std::cout << "  BLOCKED: governance stopped the search, as it would stop a run:\n";
            std::istringstream es(worker_err);
            std::string l;
            int shown = 0;
            while (std::getline(es, l) && shown < 12) { std::cout << "    " << l << "\n"; ++shown; }
        }
        if (!unmeasurable.empty()) std::cout << "  UNMEASURABLE: " << unmeasurable << "\n";
        if (targets.empty() && not_declared.empty()) {
            std::cout << "  UNMEASURABLE: no function has a requires or ensures clause\n";
        }
        for (const auto& fj : out["functions"]) {
            const std::string name = fj["function"];
            const std::string verdict = fj["verdict"];
            if (fj.contains("why")) {
                std::cout << "  " << name << ": UNMEASURABLE -- " << fj["why"].get<std::string>() << "\n";
                continue;
            }
            std::cout << "  " << name << ": " << fj["cases"] << " cases, " << fj["met_requires"]
                      << " met requires, " << fj["threw"] << " threw";
            if (verdict == "NONE_FOUND") std::cout << " -- no counterexample found\n";
            else if (verdict == "UNMEASURABLE") std::cout << " -- UNMEASURABLE (no input met requires)\n";
            else if (verdict == "COUNTEREXAMPLE") {
                const auto& ce = fj["counterexample"];
                std::string args_s;
                for (const auto& a : ce["args"]) {
                    if (!args_s.empty()) args_s += ", ";
                    args_s += a.dump();
                }
                std::cout << "\n    COUNTEREXAMPLE: " << name << "(" << args_s << ")\n";
                std::string in_fn = ce.value("in_function", name);
                std::cout << "      " << ce.value("kind", "") << " of '" << in_fn << "' does not hold: "
                          << ce.value("clause", "") << "\n";
                std::cout << "      " << ce.value("detail", "") << "\n";
            } else {
                std::cout << "\n";
            }
        }
        std::cout << "  (exit " << rc << ": 0 none found, 1 counterexample, 2 unmeasurable, "
                     "3 blocked, 4 config error)\n";
    }
    return rc;
#endif
}
