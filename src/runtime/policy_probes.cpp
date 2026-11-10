// policy_probes.cpp -- see include/naab/policy_probes.h.
//
// Moved here from tests/property/ratchet_property.cpp (Bundle 8) unchanged,
// so the harness and `naab-gov diff` (Bundle 13) judge a request the same way.

#include "naab/policy_probes.h"

#include "naab/governance.h"
#include "naab/subprocess_helpers.h"

#include <algorithm>
#include <exception>

namespace naab::governance {

const char* probeVerdictName(int verdict) {
    switch (verdict) {
        case kProbeAllow: return "allowed";
        case kProbeWarn: return "warned";
        case kProbeCatchable: return "blocked(catchable)";
        case kProbeBlock: return "blocked";
        default: return "?";
    }
}

const char* probeModeName(int value) {
    switch (value) {
        case 2: return "enforce";
        case 1: return "audit";
        case 0: return "off";
        default: return "?";
    }
}

namespace {

int gateVerdict(const std::function<std::string()>& gate) {
    try {
        return gate().empty() ? kProbeAllow : kProbeWarn;
    } catch (const GovernanceHardError&) {
        return kProbeBlock;
    } catch (const std::exception&) {
        return kProbeCatchable;
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

int fsVerdict(GovernanceEngine& e, const std::string& path, const std::string& mode) {
    // As VM::stdlibCallGates: the mode gate, then the path gate.
    int a = gateVerdict([&] { return e.checkFilesystemAllowed(mode); });
    if (a == kProbeBlock || a == kProbeCatchable) return a;
    return std::max(a, gateVerdict([&] { return e.checkPathAccess(path, mode); }));
}

}  // namespace

std::vector<PolicyProbe> buildPolicyProbes(const ProbeAlphabet& al) {
    std::vector<PolicyProbe> v;
    v.push_back({"mode", false, [](GovernanceEngine& e) {
        switch (e.getMode()) {
            case GovernanceMode::ENFORCE: return 2;
            case GovernanceMode::AUDIT: return 1;
            default: return 0;
        }
    }});
    for (const auto& n : al.env_names) {
        v.push_back({"env.read:" + n, true,
                     [n](GovernanceEngine& e) { return gateVerdict([&] { return e.checkEnvVarRead(n); }); }});
        v.push_back({"env.write:" + n, true,
                     [n](GovernanceEngine& e) { return gateVerdict([&] { return e.checkEnvVarWrite(n); }); }});
    }
    for (const auto& n : al.child_env_names) {
        // What a child the program starts receives: the parent's own decision
        // (buildChildEnvironment's policy), read from the engine bound to the
        // thread, as at every spawn.
        v.push_back({"env.child:" + n, false, [n](GovernanceEngine& e) {
            GovernanceEngine* prev = GovernanceEngine::getCurrent();
            GovernanceEngine::setCurrent(&e);
            const bool withheld =
                naab::runtime::shouldScrubEnvVar(naab::runtime::currentEnvScrubPolicy(), n);
            GovernanceEngine::setCurrent(prev);
            return withheld ? kProbeBlock : kProbeAllow;
        }});
    }
    for (const auto& p : al.paths) {
        for (const char* mode : {"read", "write"}) {
            const std::string m = mode;
            v.push_back({std::string("fs.") + m + ":" + p, true,
                         [p, m](GovernanceEngine& e) { return fsVerdict(e, p, m); }});
        }
    }
    for (const auto& p : al.write_paths) {
        v.push_back({"fs.write:" + p, true,
                     [p](GovernanceEngine& e) { return fsVerdict(e, p, "write"); }});
    }
    v.push_back({"net", true,
                 [](GovernanceEngine& e) { return gateVerdict([&] { return e.checkNetworkAllowed(); }); }});
    for (const auto& h : al.hosts) {
        v.push_back({"net.host:" + h, false, [h](GovernanceEngine& e) {
            return e.networkPolicyDenial(h, 443, true).empty() ? kProbeAllow : kProbeBlock;
        }});
    }
    for (const auto& h : al.plain_http_hosts) {
        v.push_back({"net.plain_http:" + h, false, [h](GovernanceEngine& e) {
            return e.networkPolicyDenial(h, 80, false).empty() ? kProbeAllow : kProbeBlock;
        }});
    }
    v.push_back({"shell", true,
                 [](GovernanceEngine& e) { return gateVerdict([&] { return e.checkShellAllowed(); }); }});
    for (const auto& l : al.languages) {
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
}

int judgePolicyProbe(GovernanceEngine& e, const PolicyProbe& p) {
    if (p.gated_by_active && !e.isActive()) return kProbeAllow;
    return p.run(e);
}

std::vector<int> judgePolicyProbes(GovernanceEngine& e, const std::vector<PolicyProbe>& probes) {
    std::vector<int> out;
    out.reserve(probes.size());
    for (const auto& p : probes) out.push_back(judgePolicyProbe(e, p));
    return out;
}

}  // namespace naab::governance
