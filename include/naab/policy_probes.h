// policy_probes.h -- requests judged by the gates a program's call reaches.
//
// One request per probe ("read env var X", "write file P", "connect to host
// H", "run a shell block", ...), judged by asking the engine's own gate --
// checkEnvVarRead(), checkFilesystemAllowed() + checkPathAccess(),
// checkNetworkAllowed(), networkPolicyDenial(), checkShellAllowed(),
// checkLanguageAllowed(), checkSecrets(), checkModelOutputSecrets(),
// checkPii() and what a child process receives -- so nothing here decides a
// verdict itself: a verdict is what the gate returns or throws.
//
// Two tools share this one copy: the ratchet property harness
// (tests/property/ratchet_property.cpp, Bundle 8), which judges generated
// configs over fixed alphabets, and `naab-gov diff` (Bundle 13), which judges
// two real configs over the entries they name. Before Bundle 13 the probes
// lived in the harness alone; a second copy in naab-gov would have been free
// to drift from the one the harness checks.
//
// Relative paths are passed to the gates as written, so they resolve against
// the process's working directory, as a program's own relative paths do:
// callers run the probes from the directory of the config being judged.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace naab::governance {

class GovernanceEngine;

// Verdicts, ordered: a higher value refuses more.
enum ProbeVerdict : int {
    kProbeAllow = 0,      // the gate passed
    kProbeWarn = 1,       // the gate reported (advisory) and the call goes on
    kProbeCatchable = 2,  // refused, catchable by the program (detect, or a sandbox error)
    kProbeBlock = 3,      // refused, uncatchable (GovernanceHardError)
};

// "allowed", "warned", "blocked(catchable)", "blocked".
const char* probeVerdictName(int verdict);

// The "mode" probe is not on that scale: 2 enforce, 1 audit, 0 off.
const char* probeModeName(int value);

// The requests to build probes for. Every list may be empty.
struct ProbeAlphabet {
    std::vector<std::string> env_names;         // env.read:N and env.write:N
    std::vector<std::string> child_env_names;   // env.child:N (what a child process receives)
    std::vector<std::string> paths;             // fs.read:P and fs.write:P
    std::vector<std::string> write_paths;       // fs.write:P only
    std::vector<std::string> hosts;             // net.host:H (https, port 443)
    std::vector<std::string> plain_http_hosts;  // net.plain_http:H (http, port 80)
    std::vector<std::string> languages;         // lang:L
};

struct PolicyProbe {
    std::string id;
    bool gated_by_active;  // its call sites skip the gate when the engine is inactive
    std::function<int(GovernanceEngine&)> run;
};

// Probes in a fixed order: mode, env.read/env.write per name, env.child,
// fs.read/fs.write per path, fs.write per write path, net, net.host,
// net.plain_http, shell, lang, scan.secret, scan.model_secret, scan.pii.
std::vector<PolicyProbe> buildPolicyProbes(const ProbeAlphabet& alphabet);

// One verdict per probe, in order. A probe whose call sites check isActive()
// first is allowed outright when the engine is inactive (mode off).
std::vector<int> judgePolicyProbes(GovernanceEngine& engine,
                                   const std::vector<PolicyProbe>& probes);

// Judges one probe, with the same isActive() rule.
int judgePolicyProbe(GovernanceEngine& engine, const PolicyProbe& probe);

}  // namespace naab::governance
