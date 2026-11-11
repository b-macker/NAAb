// naab-gov diff <old> <new> -- which requests a govern.json edit newly allows
// or refuses (Bundle 13).
//
// Each file is loaded as `naab-lang run` loads the govern.json it discovers
// (loadProjectConfig, then the agent role and the environment), from the
// file's own directory, so its relative entries and a program's relative
// paths resolve where a run started there would resolve them. Then:
//
//   Requests  every probe (include/naab/policy_probes.h, the probes the ratchet
//             property harness judges) is judged under each file, without and
//             with --governance-override. The probes come from the entries the
//             two files name -- environment variables, paths, hosts, evidence
//             files -- plus every language in the language table and one
//             unlisted control per kind, so a request no entry names is
//             represented. Each probe is judged on a FRESH engine: a request
//             is judged as the first request of a run, so state one gate
//             leaves (an advisory's occurrence count) cannot decide another.
//   Settings  the decision a mid-run reload makes, GovernanceEngine::
//             evaluateReload() (what `naab-gov ratchet` asks), with <new>
//             judged as replacing <old> in place: what a reload would refuse,
//             and every other change it names.
//
// Report only: it judges and prints, and changes nothing. Exit: 0 no request
// and no setting differs, 1 something differs, 4 usage or a file that does not
// load.

#include "naab/governance.h"
#include "naab/language_descriptors.h"
#include "naab/policy_probes.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using naab::governance::GovernanceEngine;
using naab::governance::PolicyProbe;

namespace {

// A request no entry names, one per kind. .invalid is reserved (RFC 2606), so
// no real host is judged by accident; no lookup is made either way.
const char* const kUnlistedEnv = "NAAB_DIFF_UNLISTED";
const char* const kUnlistedPath = "naab-diff-unlisted.txt";
const char* const kUnlistedAbsPath = "/naab-diff-unlisted/probe.txt";
const char* const kUnlistedHost = "naab-diff-unlisted.invalid";
const char* const kChildName = "naab-diff-probe";

// The loaded file's directory becomes the working directory while it is
// loaded and judged, as for a run started there; put back on every exit.
struct CwdGuard {
    fs::path saved;
    bool ok = false;
    explicit CwdGuard(const fs::path& dir) {
        std::error_code ec;
        saved = fs::current_path(ec);
        if (ec) return;
        fs::current_path(dir, ec);
        ok = !ec;
    }
    ~CwdGuard() {
        std::error_code ec;
        if (!saved.empty()) fs::current_path(saved, ec);
    }
};

// What the gates print while they are judged (an advisory's warning, a
// block's message) is not the report: stdout and stderr go to the null
// device for the duration.
struct QuietOutput {
    int saved_out = -1, saved_err = -1;
    QuietOutput() {
        std::fflush(stdout);
        std::fflush(stderr);
        std::cout.flush();
        std::cerr.flush();
#ifdef _WIN32
        int nul = _open("NUL", _O_WRONLY);
        if (nul < 0) return;
        saved_out = _dup(1);
        saved_err = _dup(2);
        _dup2(nul, 1);
        _dup2(nul, 2);
        _close(nul);
#else
        int nul = open("/dev/null", O_WRONLY);
        if (nul < 0) return;
        saved_out = dup(1);
        saved_err = dup(2);
        dup2(nul, 1);
        dup2(nul, 2);
        close(nul);
#endif
    }
    ~QuietOutput() {
        std::fflush(stdout);
        std::fflush(stderr);
        std::cout.flush();
        std::cerr.flush();
#ifdef _WIN32
        if (saved_out >= 0) { _dup2(saved_out, 1); _close(saved_out); }
        if (saved_err >= 0) { _dup2(saved_err, 2); _close(saved_err); }
#else
        if (saved_out >= 0) { dup2(saved_out, 1); close(saved_out); }
        if (saved_err >= 0) { dup2(saved_err, 2); close(saved_err); }
#endif
    }
};

struct Side {
    std::string arg;   // as given
    std::string path;  // absolute
    fs::path dir;
};

struct Startup {
    std::string env;    // --env, else the file's governance.default_env
    bool env_given = false;
    std::string agent;  // --agent-id
};

// Loads `side` as a run loads its discovered govern.json. The working
// directory must already be side.dir. `err` is set when it does not load.
std::unique_ptr<GovernanceEngine> loadSide(const Side& side, const Startup& st, bool override_on,
                                           std::string& err) {
    auto e = std::make_unique<GovernanceEngine>();
    if (!e->loadProjectConfig(side.path, side.dir.string())) {
        err = "cannot load " + side.arg;
        return nullptr;
    }
    // As main.cpp: the agent id is the flag, else "anonymous", and an
    // "anonymous" one takes govern.json's agent_id; the environment is the
    // flag, else governance.default_env.
    std::string agent = st.agent.empty() ? "anonymous" : st.agent;
    if (!e->getRules().agent_id_config.empty() && agent == "anonymous")
        agent = e->getRules().agent_id_config;
    e->setAgentId(agent);
    e->applyAgentRole();
    std::string env = st.env_given ? st.env : e->getRules().default_env;
    if (!env.empty()) {
        if (!e->getRules().environments.count(env)) {
            if (st.env_given) {
                err = "environment '" + env + "' is not in " + side.arg;
                return nullptr;
            }
        } else {
            e->applyEnvironment(env);
        }
    }
    if (override_on) {
        e->setOverrideEnabled(true);
        e->setOverrideReason("naab-gov diff");
    }
    return e;
}

void addUnique(std::vector<std::string>& v, std::set<std::string>& seen, const std::string& s) {
    if (s.empty()) return;
    if (seen.insert(s).second) v.push_back(s);
}

// The govern.json and .sig self-protection names for `dir`, as absolute
// entries addGovernanceProtectedPaths() adds; they are probed by their
// relative names instead, so each side judges its own file.
bool isSelfProtectedEntry(const std::string& entry, const std::vector<fs::path>& dirs) {
    fs::path p(entry);
    if (!p.is_absolute()) return false;
    std::error_code ec;
    const auto canon = fs::weakly_canonical(p, ec);
    for (const auto& d : dirs) {
        for (const char* name : {"govern.json", "govern.json.sig"}) {
            std::error_code ec2;
            if (fs::weakly_canonical(d / name, ec2) == canon && !ec && !ec2) return true;
        }
    }
    return false;
}

naab::governance::ProbeAlphabet alphabetFrom(const std::vector<const GovernanceEngine*>& engines,
                                             const std::vector<fs::path>& dirs) {
    naab::governance::ProbeAlphabet a;
    std::set<std::string> env_seen, path_seen, write_seen, host_seen;
    for (const auto* e : engines) {
        const auto& r = e->getRules();
        const auto& ev = r.capabilities.env_vars;
        for (const auto* list : {&ev.blocked_read, &ev.allowed_read, &ev.blocked_write,
                                 &ev.allowed_write, &ev.blocked_subprocess_vars,
                                 &ev.allowed_subprocess_vars}) {
            for (const auto& n : *list) addUnique(a.env_names, env_seen, n);
        }
        for (const auto& pre : ev.blocked_subprocess_prefixes)
            addUnique(a.env_names, env_seen, pre + "NAAB_DIFF_PROBE");
        const auto& fsc = r.capabilities.filesystem;
        for (const auto* list : {&fsc.blocked_paths, &fsc.allowed_paths}) {
            for (const auto& entry : *list) {
                if (isSelfProtectedEntry(entry, dirs)) continue;
                std::string base = entry;
                while (base.size() > 1 && (base.back() == '/' || base.back() == '\\')) base.pop_back();
                addUnique(a.paths, path_seen, base);
                addUnique(a.paths, path_seen, base + "/" + kChildName);
            }
        }
        // The evidence files the engine writes: a program may read them and
        // never write them (isEvidenceLogPath), so they are probed as writes.
        if (r.telemetry_output.enabled) addUnique(a.write_paths, write_seen, r.telemetry_output.output_file);
        if (r.transcript.enabled) addUnique(a.write_paths, write_seen, r.transcript.output_file);
        if (r.audit.level != "none")
            addUnique(a.write_paths, write_seen,
                      r.audit.output_file.empty() ? std::string(".governance-audit.jsonl")
                                                  : r.audit.output_file);
        const auto& net = r.capabilities.network;
        for (const auto* list : {&net.allowed_hosts, &net.blocked_hosts}) {
            for (const auto& h : *list) {
                if (h.rfind("*.", 0) == 0) {
                    addUnique(a.hosts, host_seen, std::string(kChildName) + "." + h.substr(2));
                    addUnique(a.hosts, host_seen, h.substr(2));
                } else {
                    addUnique(a.hosts, host_seen, h);
                }
            }
        }
    }
    addUnique(a.env_names, env_seen, kUnlistedEnv);
    a.child_env_names = a.env_names;
    for (const char* essential : {"PATH", "HOME"})
        if (!env_seen.count(essential)) a.child_env_names.push_back(essential);
    for (const char* p : {"govern.json", "govern.json.sig", kUnlistedPath, kUnlistedAbsPath})
        addUnique(a.paths, path_seen, p);
    // A write path is already probed for writes when it is also an entry.
    std::vector<std::string> writes;
    for (const auto& w : a.write_paths)
        if (!path_seen.count(w)) writes.push_back(w);
    a.write_paths = writes;
    addUnique(a.hosts, host_seen, kUnlistedHost);
    a.plain_http_hosts = a.hosts;
    for (const auto& d : naab::lang::allLanguages()) a.languages.push_back(d.canonical);
    return a;
}

std::string label(const std::string& id, int v) {
    return id == "mode" ? naab::governance::probeModeName(v) : naab::governance::probeVerdictName(v);
}

struct Change {
    std::string id;
    int before, after, before_o, after_o;
    std::string direction;  // loosened / tightened / mixed
};

}  // namespace

int cmdDiff(const std::vector<std::string>& args) {
    std::vector<std::string> files;
    Startup st;
    bool as_json = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--env" && i + 1 < args.size()) {
            st.env = args[++i];
            st.env_given = true;
        } else if (a == "--agent-id" && i + 1 < args.size()) {
            st.agent = args[++i];
        } else if (a == "--json") {
            as_json = true;
        } else if (!a.empty() && a[0] != '-') {
            files.push_back(a);
        } else {
            std::cerr << "naab-gov diff: unknown option: " << a << "\n";
            return 4;
        }
    }
    if (files.size() != 2) {
        std::cerr << "naab-gov diff: expected two files\n"
                     "Usage: naab-gov diff <old govern.json> <new govern.json> [--env NAME] "
                     "[--agent-id NAME] [--json]\n";
        return 4;
    }
    Side sides[2];
    for (int i = 0; i < 2; ++i) {
        if (!fs::is_regular_file(files[i])) {
            std::cerr << "naab-gov diff: not a file: " << files[i] << "\n";
            return 4;
        }
        sides[i].arg = files[i];
        sides[i].path = fs::absolute(files[i]).lexically_normal().string();
        sides[i].dir = fs::path(sides[i].path).parent_path();
    }

    // One load per side with its messages shown (a load warning belongs in
    // the report); the comparison engines and every probe engine load quietly.
    std::unique_ptr<GovernanceEngine> base[2];
    for (int i = 0; i < 2; ++i) {
        CwdGuard cwd(sides[i].dir);
        if (!cwd.ok) {
            std::cerr << "naab-gov diff: cannot enter the directory of " << sides[i].arg << "\n";
            return 4;
        }
        std::string err;
        base[i] = loadSide(sides[i], st, false, err);
        if (!base[i]) {
            std::cerr << "naab-gov diff: " << err << "\n";
            return 4;
        }
    }

    const std::vector<fs::path> dirs = {sides[0].dir, sides[1].dir};
    const auto alphabet = alphabetFrom({base[0].get(), base[1].get()}, dirs);
    const std::vector<PolicyProbe> probes = naab::governance::buildPolicyProbes(alphabet);

    // verdicts[side][override][probe]
    std::vector<int> verdicts[2][2];
    for (int i = 0; i < 2; ++i) {
        CwdGuard cwd(sides[i].dir);
        for (int o = 0; o < 2; ++o) {
            verdicts[i][o].reserve(probes.size());
            for (const auto& p : probes) {
                std::string err;
                QuietOutput quiet;
                auto e = loadSide(sides[i], st, o == 1, err);
                if (!e) {
                    // It loaded a moment ago; a file changed underneath is
                    // not a verdict.
                    verdicts[i][o].push_back(-1);
                    continue;
                }
                verdicts[i][o].push_back(naab::governance::judgePolicyProbe(*e, p));
            }
        }
    }
    for (int i = 0; i < 2; ++i) {
        for (int o = 0; o < 2; ++o) {
            for (int v : verdicts[i][o]) {
                if (v < 0) {
                    std::cerr << "naab-gov diff: " << sides[i].arg
                              << " stopped loading while it was judged\n";
                    return 4;
                }
            }
        }
    }

    std::vector<Change> changes;
    for (size_t k = 0; k < probes.size(); ++k) {
        const int b = verdicts[0][0][k], a = verdicts[1][0][k];
        const int bo = verdicts[0][1][k], ao = verdicts[1][1][k];
        if (b == a && bo == ao) continue;
        const bool less = a < b || ao < bo;  // refuses less: loosened
        const bool more = a > b || ao > bo;  // refuses more: tightened
        changes.push_back({probes[k].id, b, a, bo, ao,
                           less && more ? "mixed" : less ? "loosened" : "tightened"});
    }

    // Settings: the reload decision itself. evaluateReload() is what a running
    // engine calls when govern.json changes; it judges the second file as
    // replacing the first in place (its relative entries resolve against the
    // first file's project, and the first file's startup overlays are laid
    // over it), and installs nothing. The signature is not checked: the
    // question is what the edit changes, not whether it is signed.
    std::vector<std::string> loosened, other;
    {
        CwdGuard cwd(sides[0].dir);
        using Outcome = GovernanceEngine::ReloadEvaluation::Outcome;
        GovernanceEngine::ReloadEvaluation ev;
        bool threw = false;
        {
            QuietOutput quiet;
            try {
                ev = base[0]->evaluateReload(sides[1].path, /*verify_signature=*/false);
            } catch (const std::exception&) {
                threw = true;
            }
        }
        if (threw || ev.outcome == Outcome::Unreadable || ev.outcome == Outcome::TooWide ||
            ev.outcome == Outcome::ExtendsRefused) {
            std::cerr << "naab-gov diff: " << sides[1].arg
                      << " cannot be judged as a reload of " << sides[0].arg << "\n";
            return 4;
        }
        loosened = ev.violations;
        other = ev.notices;
    }

    const bool differs = !changes.empty() || !loosened.empty() || !other.empty();
    const std::vector<std::string> not_probed = {
        "function-scope capabilities, shell blocked_commands, taint, contracts, CDD, BSD, "
        "agent dispatch and limits: compared under settings, not as requests",
        "agent roles other than the one --agent-id (or govern.json's agent_id) selects, and "
        "environments other than --env (or governance.default_env)",
        "guidance sources, the scoring calibration file and file extensions",
    };

    if (as_json) {
        json reqs = json::array();
        for (const auto& c : changes) {
            reqs.push_back({{"request", c.id},
                            {"change", c.direction},
                            {"before", label(c.id, c.before)},
                            {"after", label(c.id, c.after)},
                            {"before_override", label(c.id, c.before_o)},
                            {"after_override", label(c.id, c.after_o)}});
        }
        json out = {{"old", sides[0].arg},
                    {"new", sides[1].arg},
                    {"probes", probes.size()},
                    {"requests", reqs},
                    {"settings", {{"loosened", loosened}, {"other", other}}},
                    {"unlisted_controls",
                     {{"env", kUnlistedEnv}, {"path", kUnlistedPath},
                      {"absolute_path", kUnlistedAbsPath}, {"host", kUnlistedHost}}},
                    {"not_probed", not_probed}};
        std::cout << out.dump(1) << "\n";
        return differs ? 1 : 0;
    }

    std::cout << "naab-gov diff: " << sides[0].arg << " -> " << sides[1].arg << "\n\n";
    std::cout << "Requests: " << probes.size()
              << " judged, each as the first request of a run started in its file's directory\n";
    auto printGroup = [&](const char* title, const char* dir) {
        std::vector<const Change*> g;
        for (const auto& c : changes)
            if (c.direction == dir) g.push_back(&c);
        if (g.empty()) return;
        std::cout << "  " << title << " (" << g.size() << "):\n";
        for (const auto* c : g) {
            std::cout << "    " << c->id << "  " << label(c->id, c->before) << " -> "
                      << label(c->id, c->after);
            if (c->before_o != c->before || c->after_o != c->after)
                std::cout << "  (with --governance-override: " << label(c->id, c->before_o)
                          << " -> " << label(c->id, c->after_o) << ")";
            std::cout << "\n";
        }
    };
    if (changes.empty()) {
        std::cout << "  no request is judged differently\n";
    } else {
        printGroup("newly allowed or less refused", "loosened");
        printGroup("newly refused or more refused", "tightened");
        printGroup("changed both ways", "mixed");
    }
    std::cout << "\nSettings, as a mid-run reload of the first file by the second would judge them:\n";
    if (loosened.empty() && other.empty()) {
        std::cout << "  no setting the reload ratchet compares differs\n";
    } else {
        if (!loosened.empty()) {
            std::cout << "  a reload would refuse (" << loosened.size() << "):\n";
            for (const auto& s : loosened) std::cout << "    " << s << "\n";
        }
        if (!other.empty()) {
            std::cout << "  other changes (" << other.size() << "):\n";
            for (const auto& s : other) std::cout << "    " << s << "\n";
        }
    }
    std::cout << "\nNot judged as requests:\n";
    for (const auto& n : not_probed) std::cout << "  - " << n << "\n";
    std::cout << "  - a request no entry names is represented by one control each: env "
              << kUnlistedEnv << ", paths " << kUnlistedPath << " and " << kUnlistedAbsPath
              << ", host " << kUnlistedHost << "\n";
    return differs ? 1 : 0;
}
