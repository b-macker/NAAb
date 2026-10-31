#pragma once
// The CLI's run-limit and sandbox-level overlays, decided in one place.
//
// govern.json is the project's policy and a command-line flag a local
// convenience, so a flag may only TIGHTEN what govern.json configures. These
// are the decisions src/cli/main.cpp applies (its two run paths call them),
// and tests/property/ratchet_property.cpp checks them over every combination
// of flag, configured value and mode (Bundle 8).

#include "naab/governance.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace naab {
namespace runlimits {

// Sandbox levels, most restrictive first. An unrecognised spelling ranks as
// the LEAST restrictive so it can never out-rank a real level by accident. The
// CLI value is validated before this runs (an unknown spelling exits), so in
// practice this only guards a malformed sandbox_level in govern.json.
inline int sandboxLevelRank(const std::string& level) {
    if (level == "restricted")   return 0;
    if (level == "standard")     return 1;
    if (level == "elevated")     return 2;
    if (level == "unrestricted") return 3;
    return 3;
}

inline bool isSandboxLevel(const std::string& level) {
    return level == "restricted" || level == "standard" || level == "elevated" ||
           level == "unrestricted";
}

// Whether govern.json's security.sandbox_level replaces the level the run has
// so far (the flag's, else the CLI default "unrestricted"): when it is at least
// as strict. A13: the configured level used to apply only when NO flag was
// passed, so a flag silently out-ranked govern.json.
// A value that is not a sandbox level applies nothing (the load warns about it,
// warnUnknownConfiguredSandboxLevel); it used to become the run's level name
// while rebuilding nothing.
inline bool configuredLevelApplies(const std::string& configured, const std::string& current) {
    return isSandboxLevel(configured) && sandboxLevelRank(configured) <= sandboxLevelRank(current);
}

// Whether the run must be clamped to `standard`: under enforce, a level above
// standard must come from govern.json (owner's decision 2026-10-25). Only a
// real level grants it. This tested `configured.empty()`, so a misspelled
// security.sandbox_level ("elevaetd") switched the clamp off and an enforce run
// kept whatever level it had -- unrestricted with no flag (A94, found by
// tests/property/ratchet_property.cpp).
inline bool enforceClampApplies(bool enforce, const std::string& configured,
                                const std::string& current) {
    return enforce && !isSandboxLevel(configured) &&
           sandboxLevelRank(current) > sandboxLevelRank("standard");
}

// The level a run ends with, composed from the two rules above in the order
// main.cpp applies them.
inline std::string effectiveSandboxLevel(const std::string& cli_level,
                                         const std::string& configured, bool enforce) {
    std::string level = cli_level;
    if (configuredLevelApplies(configured, level)) level = configured;
    if (enforceClampApplies(enforce, configured, level)) level = "standard";
    return level;
}

// A flag may only TIGHTEN a configured run limit: min(flag, configured). With
// nothing configured the flag, else the built-in default, applies. A
// configured 0 is left out, so it cannot be read as "no limit".
//
// History: 6c9426af took max(flag, configured) so "--timeout can extend but
// not shrink the govern.json limit"; the owner reversed that on 2026-10-25
// ("govern.json is king"). The memory limit took the flag whenever it was not
// the 512 default, so --memory-limit beat a configured limit upward.
inline void applyConfiguredRunLimits(const naab::governance::GovernanceRules& rules,
                                     unsigned int& timeout, bool timeout_from_flag,
                                     size_t& memory_limit, bool memory_from_flag) {
    if (rules.runtime.timeout_configured && rules.runtime.timeout > 0) {
        const unsigned int configured = static_cast<unsigned int>(rules.runtime.timeout);
        timeout = timeout_from_flag ? std::min(timeout, configured) : configured;
    }
    if (rules.runtime.memory_limit > 0) {
        memory_limit = memory_from_flag ? std::min(memory_limit, rules.runtime.memory_limit)
                                        : rules.runtime.memory_limit;
    }
}

}  // namespace runlimits
}  // namespace naab
