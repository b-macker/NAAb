#pragma once
// The REPL refuses to start beside a govern.json it would not apply.
//
// Every REPL front end (naab-lang with no arguments, `naab-lang --repl`, code
// piped into either, and the naab-repl* binaries) runs a tree-walker that
// loads no govern.json: none calls setSourceCode(), where discovery happens.
// Measured 2026-10-26: beside an enforce config that blocks reading NAABPROBE,
// `printf 'use env\nprint(env.get("NAABPROBE"))' | naab-lang` printed the
// value and exited 0, while the same code as a file was a HARD block (exit 3).
// The REPL can neither apply such a config nor report under it, so it does not
// start where one is enforced or audited (owner's decision 2026-10-26) and
// points to `naab-lang run`. Beside mode off, or with no govern.json found, it
// runs as before. --no-governance changes nothing here: it waives only the
// requirement that a config be found.
#include "naab/governance.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace naab {
namespace repl {

// The exit status the REPL must leave with before reading any input, or 0 when
// it may start. Discovery walks up from the working directory, as for a file
// run from there.
inline int refuseBesideGovernance() {
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (ec) return 0;  // nothing can be discovered, so nothing is ignored
    governance::GovernanceEngine gov;
    const bool loaded = gov.discoverAndLoad(cwd.string());
    if (!loaded && governance::g_governance_hard_block) {
        // A config was found and failed its integrity check: the run path
        // blocks here too (exit 3).
        fprintf(stderr,
            "[governance] INTEGRITY BLOCK: Governance configuration is tamper-protected.\n"
            "  Execution is blocked. Only the project owner can authorize changes.\n");
        fflush(nullptr);
        return 3;
    }
    if (!loaded) return 0;
    using governance::GovernanceMode;
    const auto base = gov.modeUnderEnvironment("");
    const auto with_default = gov.modeUnderEnvironment(gov.getRules().default_env);
    if (base == GovernanceMode::OFF && with_default == GovernanceMode::OFF) return 0;
    const bool enforce = base == GovernanceMode::ENFORCE || with_default == GovernanceMode::ENFORCE;
    fprintf(stderr,
        "Error: the REPL cannot run beside this project's govern.json\n\n"
        "  Got: govern.json in mode %s\n"
        "  Expected: no govern.json, or one in mode off\n\n"
        "  Help:\n"
        "  - The REPL does not apply govern.json, so it does not start where\n"
        "    one is enforced or audited. Nothing was run.\n"
        "  - Put the code in a file and run it: naab-lang run FILE.naab\n",
        enforce ? "enforce" : "audit");
    fflush(nullptr);
    return 4;
}

}  // namespace repl
}  // namespace naab
