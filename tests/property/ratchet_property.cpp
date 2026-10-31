// ratchet_property -- "governance only tightens", checked over generated configs
// (Bundle 8).
//
// THE PROPERTY
//
// A mid-run reload is refused when it would loosen governance (the reload
// ratchet, checkRatchetViolation() in governance_config.cpp). The ratchet
// compares two rule sets with its own probes; this program checks the claim
// from the outside: for every generated pair (OLD, NEW) that a reload ACCEPTS,
// no request the engine refused under OLD may be allowed under NEW, judged by
// the engine's own gates (checkEnvVarRead, checkPathAccess, checkShellAllowed,
// checkLanguageAllowed, checkSecrets, ...). Each case runs the real path: OLD is
// loaded as `naab-lang run` loads a discovered govern.json (loadProjectConfig),
// with the startup overlays a run can carry (an agent role, an environment, a
// capability lease, --governance-override); NEW is written over the same file
// with a later mtime; reloadIfChanged() decides and installs. Nothing the
// harness compares is computed by the harness: the verdicts are what the gates
// return or throw.
//
// Properties (each counterexample is printed with its seed, case, config pair
// and the request whose verdict loosened):
//   reload   accepted => every probe at least as strict as before, with and
//            without --governance-override; refused => nothing changed
//   overlay  an agent role and a capability lease each narrow the config they
//            are laid over: every probe at least as strict as without them
//   extends  measured, not asserted, except the mode floor (see --extends)
//
// Verdicts, ordered: 0 allowed, 1 warned (advisory), 2 blocked but catchable
// (detect), 3 blocked. A gate whose caller checks isActive() first is allowed
// outright when the engine is inactive (mode off), as at its call sites.
//
// Controls, each reported and each able to fail the run:
//   - both outcomes occur: some generated reloads are accepted and some
//     refused (a ratchet that refuses everything would satisfy "reload")
//   - every probe takes at least two verdicts over the run (a probe stuck at
//     one value is a broken instrument, reported UNMEASURABLE)
//   - planted loosening: the comparator, given a fresh engine loaded with a
//     NEW that drops a blocked variable (the ratchet bypassed), must report it
//
// Each case runs in its own forked process: the lease is process-wide state
// with no way to withdraw it, and a crash in one case must be a finding about
// that case, not the end of the run.
//
// Usage: ratchet_property [--cases N] [--seed S] [--show K] [--dump DIR M]
//                         [--extends N] [--extends-show K] [--quiet]
// Exit: 0 every property and control held; 1 a counterexample; 2 a control
// failed or the harness could not run (UNMEASURABLE).

#include "naab/governance.h"
#include "naab/run_limits.h"
#include "naab/subprocess_helpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;
using naab::governance::GovernanceEngine;
using naab::governance::GovernanceHardError;
using naab::governance::GovernanceMode;

namespace {

// ---------------------------------------------------------------------------
// Alphabets. Policy entries and the requests probed are separate lists: an
// entry the probes never touch, or a probe no entry names, is how a gap
// between "listed" and "covered" shows.
// ---------------------------------------------------------------------------
const std::vector<std::string> kPathEntries = {"vault/", "vault/inner/", "data/", "logs/", "."};
const std::vector<std::string> kPathProbes = {"vault/x.txt", "vault/inner/y.txt", "data/a.txt",
                                              "logs/t.txt", "top.txt"};
const std::vector<std::string> kEnvEntries = {"API_KEY", "api_key", "TOKEN", "HOME", "AWS_SECRET", "PATH"};
const std::vector<std::string> kEnvProbes = {"API_KEY", "TOKEN", "HOME", "AWS_SECRET", "OTHER_VAR"};
const std::vector<std::string> kEnvPrefixes = {"AWS_", "API_"};
const std::vector<std::string> kHostEntries = {"example.com", "api.example.com", "*.example.com",
                                               "evil.test", "10.0.0.1"};
const std::vector<std::string> kHostProbes = {"example.com", "api.example.com", "evil.test",
                                              "10.0.0.1", "other.org"};
const std::vector<std::string> kLangs = {"python", "shell", "javascript", "ruby", "php"};
const std::vector<std::string> kActions = {"FS_READ", "FS_WRITE", "NET_CONNECT", "SHELL_EXEC",
                                           "ENV_READ", "ENV_WRITE", "AGENT_SEND"};
const std::vector<std::string> kLevels = {"advisory", "detect", "soft", "hard"};

struct Rng {
    std::mt19937_64 g;
    explicit Rng(uint64_t s) : g(s) {}
    size_t below(size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(g); }
    bool chance(double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(g) < p; }
    template <class T> const T& pick(const std::vector<T>& v) { return v[below(v.size())]; }
    std::vector<std::string> subset(const std::vector<std::string>& v, double p) {
        std::vector<std::string> out;
        for (const auto& s : v) if (chance(p)) out.push_back(s);
        return out;
    }
};

// ---------------------------------------------------------------------------
// A config is a set of knobs, each absent or holding one JSON value; NEW is
// OLD with a few knobs re-rolled, so most pairs are near each other, which is
// where an off-by-one in the ratchet lives.
// ---------------------------------------------------------------------------
using Knobs = std::map<std::string, json>;  // knob name -> value (absent = not in map)

json rollKnob(Rng& r, const std::string& k) {
    auto lst = [&](const std::vector<std::string>& alpha, double p) { return json(r.subset(alpha, p)); };
    auto boolean = [&]() { return json(r.chance(0.5)); };
    if (k == "mode") return json(r.pick(std::vector<std::string>{"off", "audit", "enforce", "enforce"}));
    if (k == "fs.mode") return json(r.pick(std::vector<std::string>{"none", "read", "write"}));
    if (k == "fs.allowed_paths") return lst(kPathEntries, 0.3);
    if (k == "fs.blocked_paths") return lst(kPathEntries, 0.4);
    if (k == "env.read" || k == "env.write") return boolean();
    if (k == "env.blocked_read" || k == "env.blocked_write") return lst(kEnvEntries, 0.4);
    if (k == "env.allowed_read" || k == "env.allowed_write") return lst(kEnvEntries, 0.4);
    if (k == "env.scrub_mode") return json(r.pick(std::vector<std::string>{"", "blocklist", "allowlist"}));
    if (k == "env.blocked_subprocess_vars" || k == "env.allowed_subprocess_vars") return lst(kEnvEntries, 0.4);
    if (k == "env.blocked_subprocess_prefixes") return lst(kEnvPrefixes, 0.5);
    if (k == "net.enabled" || k == "net.https_only") return boolean();
    if (k == "net.allowed_hosts") return lst(kHostEntries, 0.3);
    if (k == "net.blocked_hosts") return lst(kHostEntries, 0.4);
    if (k == "shell") {
        // The object form and the bare form set different mirror copies
        // (reconcileCapabilityMirrors); both are written by real configs.
        switch (r.below(4)) {
            case 0: return json{{"enabled", true}};
            case 1: return json{{"enabled", false}};
            case 2: return json(true);
            default: return json(false);
        }
    }
    if (k == "lang.allowed") return lst(kLangs, 0.6);
    if (k == "lang.blocked") return lst(kLangs, 0.3);
    if (k == "cq.no_secrets" || k == "cq.no_pii") {
        switch (r.below(5)) {
            case 0: return json(false);
            case 1: return json(true);
            case 2: return json(r.pick(kLevels));
            case 3: return json{{"enabled", true}, {"level", r.pick(kLevels)}};
            default: return json{{"enabled", false}};
        }
    }
    if (k == "agent.allowed_paths") return lst(kPathEntries, 0.3);
    if (k == "agent.blocked_paths") return lst(kPathEntries, 0.4);
    if (k == "agent.shell_allowed" || k == "agent.network_allowed") return boolean();
    if (k == "agent.allowed_actions") return lst(kActions, 0.5);
    if (k == "envs.dev.mode") return json(r.pick(std::vector<std::string>{"off", "audit", "enforce"}));
    return json(nullptr);
}

const std::vector<std::string>& knobNames() {
    static const std::vector<std::string> k = {
        "mode", "fs.mode", "fs.allowed_paths", "fs.blocked_paths",
        "env.read", "env.write", "env.blocked_read", "env.allowed_read", "env.blocked_write",
        "env.allowed_write", "env.scrub_mode", "env.blocked_subprocess_vars",
        "env.allowed_subprocess_vars", "env.blocked_subprocess_prefixes",
        "net.enabled", "net.https_only", "net.allowed_hosts", "net.blocked_hosts",
        "shell", "lang.allowed", "lang.blocked", "cq.no_secrets", "cq.no_pii",
        "agent.allowed_paths", "agent.blocked_paths", "agent.shell_allowed",
        "agent.network_allowed", "agent.allowed_actions", "envs.dev.mode"};
    return k;
}

Knobs randomKnobs(Rng& r) {
    Knobs k;
    k["mode"] = r.chance(0.9) ? rollKnob(r, "mode") : json(nullptr);
    if (k["mode"].is_null()) k.erase("mode");
    for (const auto& name : knobNames()) {
        if (name == "mode") continue;
        if (r.chance(0.35)) k[name] = rollKnob(r, name);
    }
    return k;
}

Knobs mutate(Rng& r, const Knobs& base) {
    Knobs k = base;
    const size_t edits = 1 + r.below(3);
    for (size_t i = 0; i < edits; ++i) {
        const std::string& name = r.pick(knobNames());
        auto it = k.find(name);
        const json v = it == k.end() ? json(nullptr) : it->second;
        const size_t op = r.below(4);
        if (op == 0 || it == k.end()) {
            k[name] = rollKnob(r, name);                 // set or re-roll
        } else if (op == 1) {
            k.erase(name);                                // absent
        } else if (v.is_array()) {
            json a = v;                                   // one element in or out
            if (!a.empty() && r.chance(0.5)) {
                a.erase(a.begin() + static_cast<long>(r.below(a.size())));
            } else {
                json fresh = rollKnob(r, name);
                if (fresh.is_array() && !fresh.empty()) {
                    const std::string e = fresh[r.below(fresh.size())];
                    if (std::find(a.begin(), a.end(), e) == a.end()) a.push_back(e);
                }
            }
            k[name] = a;
        } else {
            k[name] = rollKnob(r, name);
        }
    }
    return k;
}

json toConfig(const Knobs& k) {
    json c = {{"version", "5.0"}};
    auto get = [&](const std::string& n) -> const json* {
        auto it = k.find(n);
        return it == k.end() ? nullptr : &it->second;
    };
    if (auto v = get("mode")) c["mode"] = *v;
    auto put = [&](const char* knob, json::json_pointer ptr) {
        if (auto v = get(knob)) c[ptr] = *v;
    };
    put("fs.mode", json::json_pointer("/capabilities/filesystem/mode"));
    put("fs.allowed_paths", json::json_pointer("/capabilities/filesystem/allowed_paths"));
    put("fs.blocked_paths", json::json_pointer("/capabilities/filesystem/blocked_paths"));
    put("env.read", json::json_pointer("/capabilities/env_vars/read"));
    put("env.write", json::json_pointer("/capabilities/env_vars/write"));
    put("env.blocked_read", json::json_pointer("/capabilities/env_vars/blocked_read"));
    put("env.allowed_read", json::json_pointer("/capabilities/env_vars/allowed_read"));
    put("env.blocked_write", json::json_pointer("/capabilities/env_vars/blocked_write"));
    put("env.allowed_write", json::json_pointer("/capabilities/env_vars/allowed_write"));
    put("env.scrub_mode", json::json_pointer("/capabilities/env_vars/subprocess_scrub_mode"));
    put("env.blocked_subprocess_vars", json::json_pointer("/capabilities/env_vars/blocked_subprocess_vars"));
    put("env.allowed_subprocess_vars", json::json_pointer("/capabilities/env_vars/allowed_subprocess_vars"));
    put("env.blocked_subprocess_prefixes",
        json::json_pointer("/capabilities/env_vars/blocked_subprocess_prefixes"));
    put("net.enabled", json::json_pointer("/capabilities/network/enabled"));
    put("net.https_only", json::json_pointer("/capabilities/network/https_only"));
    put("net.allowed_hosts", json::json_pointer("/capabilities/network/allowed_hosts"));
    put("net.blocked_hosts", json::json_pointer("/capabilities/network/blocked_hosts"));
    put("shell", json::json_pointer("/capabilities/shell"));
    put("lang.allowed", json::json_pointer("/languages/allowed"));
    put("lang.blocked", json::json_pointer("/languages/blocked"));
    put("cq.no_secrets", json::json_pointer("/code_quality/no_secrets"));
    put("cq.no_pii", json::json_pointer("/code_quality/no_pii"));
    json agent = {{"name", "worker"}};
    bool any_agent = false;
    auto agentField = [&](const char* knob, const char* field) {
        if (auto v = get(knob)) { agent[field] = *v; any_agent = true; }
    };
    agentField("agent.allowed_paths", "allowed_paths");
    agentField("agent.blocked_paths", "blocked_paths");
    agentField("agent.shell_allowed", "shell_allowed");
    agentField("agent.network_allowed", "network_allowed");
    agentField("agent.allowed_actions", "allowed_actions");
    if (any_agent) c["agents"] = json::array({agent});
    if (auto v = get("envs.dev.mode")) {
        c["environments"] = {{"dev", {{"mode", *v}}}, {"ci", {{"mode", "enforce"}}}};
    }
    return c;
}

// ---------------------------------------------------------------------------
// Probes: one request each, judged by the gate a program's call reaches.
// ---------------------------------------------------------------------------
enum Verdict { kAllow = 0, kWarn = 1, kCatchable = 2, kBlock = 3 };
const char* verdictName(int v) {
    switch (v) {
        case kAllow: return "allowed";
        case kWarn: return "warned";
        case kCatchable: return "blocked(catchable)";
        case kBlock: return "blocked";
        default: return "?";
    }
}

int gateVerdict(const std::function<std::string()>& gate) {
    try {
        return gate().empty() ? kAllow : kWarn;
    } catch (const GovernanceHardError&) {
        return kBlock;
    } catch (const std::exception&) {
        return kCatchable;
    }
}

// Built at run time from fragments: the scanners must see a credential-shaped
// and an SSN-shaped value, and the source must hold neither.
std::string secretText() {
    std::string key = std::string("AK") + "IA";
    for (char c : std::string("QWERTYUIOPASDFGH")) key += c;
    return "let k = \"" + key + "\"\n";
}
std::string piiText() {
    return "ssn: " + std::to_string(100 + 23) + "-" + std::to_string(40 + 5) + "-" +
           std::to_string(6000 + 789) + "\n";
}

struct Probe {
    std::string id;
    bool gated_by_active;  // its call sites skip it when the engine is inactive
    std::function<int(GovernanceEngine&)> run;
};

const std::vector<Probe>& probes() {
    static const std::vector<Probe> p = [] {
        std::vector<Probe> v;
        v.push_back({"mode", false, [](GovernanceEngine& e) {
            switch (e.getMode()) {
                case GovernanceMode::ENFORCE: return 2;
                case GovernanceMode::AUDIT: return 1;
                default: return 0;
            }
        }});
        for (const auto& n : kEnvProbes) {
            v.push_back({"env.read:" + n, true,
                         [n](GovernanceEngine& e) { return gateVerdict([&] { return e.checkEnvVarRead(n); }); }});
            v.push_back({"env.write:" + n, true,
                         [n](GovernanceEngine& e) { return gateVerdict([&] { return e.checkEnvVarWrite(n); }); }});
        }
        for (const auto& n : std::vector<std::string>{"API_KEY", "TOKEN", "AWS_SECRET", "OTHER_VAR", "PATH"}) {
            // What a child the program starts receives: the parent's own
            // decision (buildChildEnvironment's policy), read from the engine
            // bound to the thread, as at every spawn.
            v.push_back({"env.child:" + n, false, [n](GovernanceEngine& e) {
                GovernanceEngine* prev = GovernanceEngine::getCurrent();
                GovernanceEngine::setCurrent(&e);
                const bool withheld = naab::runtime::shouldScrubEnvVar(naab::runtime::currentEnvScrubPolicy(), n);
                GovernanceEngine::setCurrent(prev);
                return withheld ? kBlock : kAllow;
            }});
        }
        for (const auto& p : kPathProbes) {
            for (const char* mode : {"read", "write"}) {
                const std::string m = mode;
                v.push_back({std::string("fs.") + m + ":" + p, true, [p, m](GovernanceEngine& e) {
                    // As VM::stdlibCallGates: the mode gate, then the path gate.
                    int a = gateVerdict([&] { return e.checkFilesystemAllowed(m); });
                    if (a == kBlock || a == kCatchable) return a;
                    return std::max(a, gateVerdict([&] { return e.checkPathAccess(p, m); }));
                }});
            }
        }
        v.push_back({"net", true,
                     [](GovernanceEngine& e) { return gateVerdict([&] { return e.checkNetworkAllowed(); }); }});
        for (const auto& h : kHostProbes) {
            v.push_back({"net.host:" + h, false, [h](GovernanceEngine& e) {
                return e.networkPolicyDenial(h, 443, true).empty() ? kAllow : kBlock;
            }});
        }
        v.push_back({"net.plain_http:example.com", false, [](GovernanceEngine& e) {
            return e.networkPolicyDenial("example.com", 80, false).empty() ? kAllow : kBlock;
        }});
        v.push_back({"shell", true,
                     [](GovernanceEngine& e) { return gateVerdict([&] { return e.checkShellAllowed(); }); }});
        for (const auto& l : kLangs) {
            v.push_back({"lang:" + l, true,
                         [l](GovernanceEngine& e) { return gateVerdict([&] { return e.checkLanguageAllowed(l); }); }});
        }
        v.push_back({"scan.secret", true,
                     [](GovernanceEngine& e) { return gateVerdict([&] { return e.checkSecrets(secretText()); }); }});
        v.push_back({"scan.model_secret", true, [](GovernanceEngine& e) {
            return gateVerdict([&] { return e.checkModelOutputSecrets(secretText()); });
        }});
        v.push_back({"scan.pii", true,
                     [](GovernanceEngine& e) { return gateVerdict([&] { return e.checkPii(piiText()); }); }});
        return v;
    }();
    return p;
}

std::vector<int> judge(GovernanceEngine& e) {
    std::vector<int> out;
    for (const auto& p : probes()) {
        if (p.gated_by_active && !e.isActive()) { out.push_back(kAllow); continue; }
        out.push_back(p.run(e));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Startup: what a run lays over the loaded file.
// ---------------------------------------------------------------------------
struct Startup {
    bool role = false;           // --agent-id worker
    std::string env;             // --env NAME
    bool lease = false;          // --lease-grant ...
    std::vector<std::string> lease_actions;
    std::string describe() const {
        std::string s = std::string("role=") + (role ? "worker" : "-") + " env=" + (env.empty() ? "-" : env) +
                        " lease=";
        if (!lease) return s + "-";
        std::string a;
        for (const auto& x : lease_actions) a += (a.empty() ? "" : ",") + x;
        return s + (a.empty() ? "(none)" : a);
    }
};

void silenceStderr() {
#ifndef _WIN32
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) { dup2(fd, 2); close(fd); }
#endif
}

void writeFile(const fs::path& p, const std::string& s, int mtime_offset) {
    { std::ofstream o(p, std::ios::trunc); o << s; }
    // A reload notices a change by the file's mtime, in whole seconds.
    const auto base = fs::file_time_type::clock::now() - std::chrono::hours(1);
    fs::last_write_time(p, base + std::chrono::seconds(mtime_offset));
}

std::unique_ptr<GovernanceEngine> startEngine(const fs::path& cfg, const Startup& s, bool override_on) {
    auto e = std::make_unique<GovernanceEngine>();
    if (!e->loadProjectConfig(cfg.string(), cfg.parent_path().string())) return nullptr;
    e->setAgentId(s.role ? "worker" : "anonymous");
    e->applyAgentRole();
    if (!s.env.empty()) e->applyEnvironment(s.env);
    if (override_on) {
        e->setOverrideEnabled(true);
        e->setOverrideReason("ratchet property harness");
    }
    return e;
}

// One reload case, run in the current (forked) process. Returns a JSON result.
json runReloadCase(const fs::path& dir, const json& old_cfg, const json& new_cfg, const Startup& s,
                   bool bypass_ratchet = false) {
    json res;
    if (s.lease) GovernanceEngine::setLeaseGrant(s.lease_actions);
    const fs::path cfg = dir / "govern.json";
    writeFile(cfg, old_cfg.dump(1), 0);
    auto a = startEngine(cfg, s, false);
    auto ao = startEngine(cfg, s, true);
    auto b = startEngine(cfg, s, false);
    auto bo = startEngine(cfg, s, true);
    if (!a || !ao || !b || !bo) { res["invalid"] = a ? a->getLastError() : "OLD did not load"; return res; }
    res["before"] = judge(*a);
    res["before_override"] = judge(*ao);
    writeFile(cfg, new_cfg.dump(1), 10);
    if (bypass_ratchet) {
        // The planted-loosening control: NEW loaded fresh, as if the ratchet
        // had accepted it. The comparator must call this a loosening.
        auto c = startEngine(cfg, s, false);
        auto co = startEngine(cfg, s, true);
        if (!c || !co) { res["error"] = "NEW did not load"; return res; }
        res["accepted"] = true;
        res["after"] = judge(*c);
        res["after_override"] = judge(*co);
        return res;
    }
    const bool acc = b->reloadIfChanged();
    const bool acc_o = bo->reloadIfChanged();
    res["accepted"] = acc;
    res["accepted_override"] = acc_o;
    res["after"] = judge(*b);
    res["after_override"] = judge(*bo);
    // The same pair under the startup with no role and no lease, for the
    // overlay property (a role and a lease narrow what they are laid over).
    return res;
}

json runBaselineCase(const fs::path& dir, const json& old_cfg, const Startup& s) {
    json res;
    const fs::path cfg = dir / "govern.json";
    writeFile(cfg, old_cfg.dump(1), 0);
    auto a = startEngine(cfg, s, false);
    auto ao = startEngine(cfg, s, true);
    if (!a || !ao) { res["invalid"] = "OLD did not load"; return res; }
    res["before"] = judge(*a);
    res["before_override"] = judge(*ao);
    return res;
}

json runExtendsCase(const fs::path& dir, const json& parent, const json& child) {
    json res;
    const fs::path cfg = dir / "govern.json";
    Startup none;
    writeFile(cfg, parent.dump(1), 0);
    auto p = startEngine(cfg, none, false);
    if (!p) { res["invalid"] = "parent did not load"; return res; }
    res["parent"] = judge(*p);
    writeFile(dir / "parent.json", parent.dump(1), 0);
    writeFile(cfg, child.dump(1), 5);
    auto c = startEngine(cfg, none, false);
    if (!c) { res["invalid"] = "child did not load"; return res; }
    res["child"] = judge(*c);
    return res;
}

// Run `fn` in a forked child, returning its JSON result (or {"crash": ...}).
json inChild(const std::function<json()>& fn) {
#ifdef _WIN32
    return fn();
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) return json{{"error", "pipe failed"}};
    fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) return json{{"error", "fork failed"}};
    if (pid == 0) {
        close(pipefd[0]);
        silenceStderr();
        std::string out;
        try { out = fn().dump(); }
        catch (const std::exception& e) { out = json{{"error", std::string("exception: ") + e.what()}}.dump(); }
        size_t off = 0;
        while (off < out.size()) {
            ssize_t w = write(pipefd[1], out.data() + off, out.size() - off);
            if (w <= 0) break;
            off += static_cast<size_t>(w);
        }
        close(pipefd[1]);
        _exit(0);
    }
    close(pipefd[1]);
    std::string buf;
    char tmp[65536];
    ssize_t n;
    while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0) buf.append(tmp, static_cast<size_t>(n));
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return json{{"crash", WTERMSIG(status)}};
    try { return json::parse(buf); }
    catch (...) { return json{{"error", "unparseable child result"}}; }
#endif
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
struct Finding {
    std::string property;
    uint64_t seed;
    size_t index;
    std::string startup;
    std::string probe;
    int before, after;
    bool override_on;
    json old_cfg, new_cfg;
};

void printFinding(const Finding& f) {
    std::cout << "COUNTEREXAMPLE property=" << f.property << " seed=" << f.seed << " case=" << f.index << "\n"
              << "  startup: " << f.startup << (f.override_on ? " --governance-override" : "") << "\n"
              << "  request: " << f.probe << "  before=" << verdictName(f.before)
              << "  after=" << verdictName(f.after) << "\n"
              << "  old: " << f.old_cfg.dump() << "\n"
              << "  new: " << f.new_cfg.dump() << "\n";
}

// How much a verdict refuses: a warning lets the request through like an
// allowed one, so the two compare equal.
int refusal(int verdict) { return verdict <= kWarn ? 0 : verdict - 1; }

// Every position where `after` refuses less than `before`.
std::vector<size_t> loosened(const json& before, const json& after) {
    std::vector<size_t> out;
    if (!before.is_array() || !after.is_array() || before.size() != after.size()) return {SIZE_MAX};
    for (size_t i = 0; i < before.size(); ++i)
        if (refusal(after[i].get<int>()) < refusal(before[i].get<int>())) out.push_back(i);
    return out;
}

Startup randomStartup(Rng& r) {
    Startup s;
    s.role = r.chance(0.3);
    if (r.chance(0.2)) s.env = r.chance(0.7) ? "dev" : "ci";
    s.lease = r.chance(0.2);
    if (s.lease) s.lease_actions = r.subset(kActions, 0.5);
    return s;
}

// The CLI's run-limit and sandbox-level overlays (naab/run_limits.h, the
// functions main.cpp calls), checked exhaustively: a flag may lower what
// govern.json configures, never raise it; a sandbox-level flag never ends
// looser than no flag; and under enforce a run is above `standard` only when
// govern.json grants that level. With nothing configured a run-limit flag
// applies as given (by design: the built-in default is not a policy).
int checkFlagOverlays(std::vector<std::string>& failures) {
    using naab::runlimits::effectiveSandboxLevel;
    using naab::runlimits::sandboxLevelRank;
    const std::vector<std::string> flag_levels = {"", "restricted", "standard", "elevated", "unrestricted"};
    const std::vector<std::string> conf_levels = {"", "restricted", "standard", "elevated", "unrestricted", "bogus"};
    int checked = 0;
    for (bool enforce : {false, true}) {
        for (const auto& conf : conf_levels) {
            const std::string none = effectiveSandboxLevel("unrestricted", conf, enforce);
            for (const auto& flag : flag_levels) {
                const std::string cli = flag.empty() ? "unrestricted" : flag;
                const std::string eff = effectiveSandboxLevel(cli, conf, enforce);
                ++checked;
                const bool conf_valid = conf == "restricted" || conf == "standard" || conf == "elevated" ||
                                        conf == "unrestricted";
                if (sandboxLevelRank(eff) > sandboxLevelRank(none))
                    failures.push_back("sandbox level: flag " + cli + " over configured '" + conf + "'" +
                                       (enforce ? " (enforce)" : "") + " gives " + eff + ", looser than " + none +
                                       " without the flag");
                if (conf_valid && sandboxLevelRank(eff) > sandboxLevelRank(conf))
                    failures.push_back("sandbox level: flag " + cli + " raises configured " + conf + " to " + eff);
                if (enforce && !conf_valid && sandboxLevelRank(eff) > sandboxLevelRank("standard"))
                    failures.push_back("sandbox level: under enforce, flag " + cli + " with no configured level gives " +
                                       eff + ", above standard");
            }
        }
    }
    naab::governance::GovernanceRules rules;
    for (int conf_t : {0, 3, 50}) {
        for (bool configured : {false, true}) {
            for (unsigned flag_t : {0u, 1u, 5u, 100u}) {
                for (size_t conf_m : {size_t(0), size_t(64), size_t(2048)}) {
                    for (size_t flag_m : {size_t(0), size_t(32), size_t(512), size_t(4096)}) {
                        rules.runtime.timeout = conf_t;
                        rules.runtime.timeout_configured = configured;
                        rules.runtime.memory_limit = conf_m;
                        unsigned t = flag_t ? flag_t : 30u;
                        size_t m = flag_m ? flag_m : size_t(512);
                        naab::runlimits::applyConfiguredRunLimits(rules, t, flag_t != 0, m, flag_m != 0);
                        ++checked;
                        if (configured && conf_t > 0 && t > static_cast<unsigned>(conf_t))
                            failures.push_back("timeout: flag " + std::to_string(flag_t) + " raises configured " +
                                               std::to_string(conf_t) + " to " + std::to_string(t));
                        if (conf_m > 0 && m > conf_m)
                            failures.push_back("memory: flag " + std::to_string(flag_m) + " raises configured " +
                                               std::to_string(conf_m) + " to " + std::to_string(m));
                    }
                }
            }
        }
    }
    return checked;
}

}  // namespace

int main(int argc, char** argv) {
    size_t cases = 300, show = 5, extends_cases = 0, dump_count = 0, extends_show = 0;
    uint64_t seed = 1;
    std::string dump_dir;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--cases") cases = std::stoul(next());
        else if (a == "--seed") seed = std::stoull(next());
        else if (a == "--show") show = std::stoul(next());
        else if (a == "--extends") extends_cases = std::stoul(next());
        else if (a == "--extends-show") extends_show = std::stoul(next());
        else if (a == "--dump") { dump_dir = next(); dump_count = std::stoul(next()); }
        else if (a == "--quiet") quiet = true;
        else { std::cerr << "ratchet_property: unknown option " << a << "\n"; return 2; }
    }
#ifdef _WIN32
    std::cout << "UNMEASURABLE: the harness forks each case; POSIX only\n";
    return 2;
#else
    // A machine's own trust store or signing key would make every unsigned
    // config fail to load or reload; the harness owns its environment.
    const fs::path work = fs::temp_directory_path() /
                          ("naab_ratchet_prop_" + std::to_string(getpid()));
    fs::create_directories(work / "home");
    fs::create_directories(work / "cfg");
    setenv("HOME", (work / "home").string().c_str(), 1);
    fs::create_directories(work / "trust");
    setenv("NAAB_TRUST_STORE_DIR", (work / "trust").string().c_str(), 1);
    unsetenv("NAAB_GOVERN_KEY");
    unsetenv("NAAB_SIGNING_KEY");
    fs::current_path(work / "cfg");  // relative probes resolve where the config is
    const fs::path dir = work / "cfg";

    std::vector<Finding> findings;
    size_t accepted = 0, refused = 0, errors = 0, crashes = 0, disagree = 0, invalid = 0;
    std::vector<std::set<int>> seen(probes().size());
    auto note = [&](const json& v) {
        if (!v.is_array()) return;
        for (size_t i = 0; i < v.size() && i < seen.size(); ++i) seen[i].insert(v[i].get<int>());
    };
    auto record = [&](const char* property, size_t idx, const Startup& s, const json& before,
                      const json& after, bool ov, const json& o, const json& n) {
        for (size_t p : loosened(before, after)) {
            Finding f{property, seed, idx, s.describe(),
                      p == SIZE_MAX ? "(verdict vectors differ in length)" : probes()[p].id,
                      p == SIZE_MAX ? -1 : before[p].get<int>(), p == SIZE_MAX ? -1 : after[p].get<int>(),
                      ov, o, n};
            findings.push_back(f);
        }
    };

    Rng rng(seed);
    for (size_t i = 0; i < cases; ++i) {
        Knobs ok = randomKnobs(rng);
        Knobs nk = rng.chance(0.85) ? mutate(rng, ok) : randomKnobs(rng);
        const json o = toConfig(ok), n = toConfig(nk);
        const Startup s = randomStartup(rng);
        json res = inChild([&] { return runReloadCase(dir, o, n, s); });
        if (res.contains("crash")) { ++crashes; std::cout << "CRASH case=" << i << " signal=" << res["crash"]
                                                          << "\n  old: " << o.dump() << "\n  new: " << n.dump() << "\n"; continue; }
        if (res.contains("invalid")) { ++invalid; continue; }  // the loader refused OLD (e.g. CONTRA-001)
        if (res.contains("error")) {
            if (errors++ < show)
                std::cout << "ERROR case=" << i << ": " << res["error"].get<std::string>()
                          << "\n  startup: " << s.describe() << "\n  old: " << o.dump()
                          << "\n  new: " << n.dump() << "\n";
            continue;
        }
        const bool acc = res["accepted"].get<bool>();
        if (acc != res["accepted_override"].get<bool>()) ++disagree;
        (acc ? accepted : refused)++;
        note(res["before"]); note(res["after"]); note(res["before_override"]); note(res["after_override"]);
        if (acc) {
            record("reload", i, s, res["before"], res["after"], false, o, n);
            record("reload", i, s, res["before_override"], res["after_override"], true, o, n);
        } else if (res["before"] != res["after"] || res["before_override"] != res["after_override"]) {
            Finding f{"refused-reload-changed-state", seed, i, s.describe(), "(a refused reload changed a verdict)",
                      -1, -1, false, o, n};
            findings.push_back(f);
        }
        // overlay: the role and the lease narrow what they are laid over.
        if (s.role || s.lease) {
            Startup bare = s;
            bare.role = false;
            bare.lease = false;
            bare.lease_actions.clear();
            json base = inChild([&] { return runBaselineCase(dir, o, bare); });
            if (base.contains("before")) {
                record("overlay", i, s, base["before"], res["before"], false, o, o);
                record("overlay", i, s, base["before_override"], res["before_override"], true, o, o);
            }
        }
        if (!dump_dir.empty() && i < dump_count) {
            const fs::path d = fs::path(dump_dir) / ("case-" + std::to_string(i));
            fs::create_directories(d);
            { std::ofstream(d / "govern.json") << o.dump(1); }
            { std::ofstream(d / "new.json") << n.dump(1); }
            // naab-gov ratchet refuses an --env the old config lacks (exit 4),
            // where a run warns and continues, so the suite needs to know.
            const bool env_known = s.env.empty() ||
                (o.contains("environments") && o["environments"].contains(s.env));
            json meta = {{"accepted", acc}, {"role", s.role}, {"env", s.env}, {"lease", s.lease},
                         {"lease_actions", s.lease_actions}, {"env_known", env_known}};
            { std::ofstream(d / "meta.json") << meta.dump(); }
        }
    }

    // extends: measured. Reported as a table of which requests a child can
    // loosen relative to its parent, per merge strategy; only the mode floor
    // is a property here (mergeRules: "Mode is a security floor").
    std::map<std::string, std::map<std::string, size_t>> extends_loosened;  // strategy -> probe -> count
    std::map<std::string, size_t> extends_examples;
    size_t extends_run = 0;
    for (size_t i = 0; i < extends_cases; ++i) {
        Knobs pk = randomKnobs(rng);
        Knobs ck = mutate(rng, pk);
        json parent = toConfig(pk), child = toConfig(ck);
        const std::string strategy = rng.chance(0.5) ? "parent_wins" : "child_wins";
        const std::string arrays = rng.chance(0.5) ? "append" : "replace";
        child["extends"] = "./parent.json";
        child["meta"] = {{"inheritance", {{"merge_strategy", strategy}, {"merge_arrays", arrays}}}};
        json res = inChild([&] { return runExtendsCase(dir, parent, child); });
        if (res.contains("invalid")) { ++invalid; continue; }
        if (!res.contains("child")) { ++errors; continue; }
        ++extends_run;
        note(res["parent"]); note(res["child"]);
        for (size_t p : loosened(res["parent"], res["child"])) {
            if (p == SIZE_MAX) continue;
            extends_loosened[strategy + "/" + arrays][probes()[p].id]++;
            if (extends_examples[strategy + "/" + arrays]++ < extends_show)
                printFinding(Finding{"extends(" + strategy + "/" + arrays + ", measured)", seed, i, "-",
                                     probes()[p].id, res["parent"][p].get<int>(), res["child"][p].get<int>(),
                                     false, parent, child});
            if (probes()[p].id == "mode") {
                Finding f{"extends-mode-floor", seed, i, "-", "mode", res["parent"][p].get<int>(),
                          res["child"][p].get<int>(), false, parent, child};
                findings.push_back(f);
            }
        }
    }

    // Control: planted loosening -- the ratchet bypassed, so the comparator
    // must see API_KEY become readable.
    size_t planted_seen = 0;
    {
        json o = {{"version", "5.0"}, {"mode", "enforce"},
                  {"capabilities", {{"env_vars", {{"blocked_read", {"API_KEY"}}}}}}};
        json n = {{"version", "5.0"}, {"mode", "enforce"}};
        json res = inChild([&] { return runReloadCase(dir, o, n, Startup{}, /*bypass=*/true); });
        if (res.contains("after")) planted_seen = loosened(res["before"], res["after"]).size();
    }

    std::vector<std::string> flag_failures;
    const int flag_checked = checkFlagOverlays(flag_failures);

    // ---- report ----
    std::cout << "ratchet property: seed=" << seed << " cases=" << cases << " accepted=" << accepted
              << " refused=" << refused << " invalid=" << invalid << " errors=" << errors
              << " crashes=" << crashes << "\n";
    std::map<std::string, size_t> by_prop;
    for (const auto& f : findings) by_prop[f.property]++;
    std::map<std::string, size_t> shown;
    for (const auto& f : findings) {
        if (shown[f.property]++ < show) printFinding(f);
    }
    for (const auto& [p, c] : by_prop) std::cout << "  property " << p << ": " << c << " counterexample(s)\n";
    if (extends_cases) {
        std::cout << "extends (measured; a child loosening its parent is reported, not failed):"
                  << " cases=" << extends_run << "\n";
        for (const auto& [strat, m] : extends_loosened) {
            std::cout << "  " << strat << ":";
            for (const auto& [probe, c] : m) std::cout << " " << probe << "=" << c;
            std::cout << "\n";
        }
    }
    std::cout << "flag overlays: " << flag_checked << " combinations, " << flag_failures.size() << " failure(s)\n";
    for (size_t i = 0; i < flag_failures.size() && i < show; ++i) std::cout << "  " << flag_failures[i] << "\n";

    bool control_failed = false;
    std::vector<std::string> stuck;
    for (size_t i = 0; i < seen.size(); ++i) if (seen[i].size() < 2) stuck.push_back(probes()[i].id);
    if (!quiet || !stuck.empty()) {
        std::cout << "control: probes that took one verdict only: " << stuck.size() << "\n";
        for (const auto& s : stuck) std::cout << "  UNMEASURABLE probe " << s << "\n";
    }
    if (!stuck.empty()) control_failed = true;
    if (cases > 0 && (accepted == 0 || refused == 0)) {
        std::cout << "control FAILED: a run must see both accepted and refused reloads\n";
        control_failed = true;
    }
    if (planted_seen == 0) {
        std::cout << "control FAILED: the planted loosening was not detected\n";
        control_failed = true;
    } else if (!quiet) {
        std::cout << "control: planted loosening detected (" << planted_seen << " request(s))\n";
    }
    if (disagree) {
        std::cout << "control FAILED: " << disagree << " reload(s) decided differently with --governance-override\n";
        control_failed = true;
    }
    if (errors) {
        std::cout << "control FAILED: " << errors << " case(s) could not run\n";
        control_failed = true;
    }

    std::error_code ec;
    fs::current_path(fs::temp_directory_path(), ec);
    fs::remove_all(work, ec);

    if (!findings.empty() || crashes || !flag_failures.empty()) return 1;
    return control_failed ? 2 : 0;
#endif
}
