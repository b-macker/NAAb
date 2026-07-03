# AI Slop Audit — Round 9 (40-Level Taxonomy)

Exhaustive slop audit of the NAAb language codebase (~134K lines C++ across 303 files,
plus tests/scripts/CI) against the 40-level AI-slop taxonomy. Discipline: verify before
reporting — every finding cites file:line, survives "bug or design decision?", and passes
the anti-false-positive checklist. The prior Gemini audit had a 77% FP rate; this round
eliminated ~20 candidate findings that turned out to be working-as-designed.

**Result: 9 verified findings (1 HIGH, 3 MEDIUM, 5 LOW). 8 fixed, 1 deferred.**
Baseline preserved: security leak checks 874/874, full suite green (excluding 3
pre-existing environmental failures documented below).

---

## Summary

| ID | Level | Severity | Confidence | File | Status | Description |
|----|-------|----------|------------|------|--------|-------------|
| F3 | L19 | HIGH | HIGH | run-all-tests.sh:1419,1432,1321 | FIXED | 3 test sections appended to FAILED_TESTS without incrementing FAILED — security-test failures silently swallowed (suite exits 0) |
| F4 | L37 | MEDIUM | HIGH | src/stdlib/math_impl.cpp:87,96,105 | FIXED | `math.floor/ceil/round` cast out-of-range/NaN double to int32 — undefined behavior |
| F5 | L19/L20 | MEDIUM | HIGH | tests/governance_v4/*, tests/agent/* | PARTIAL | 15 test scripts never invoked by runner or CI; 3 verified-passing ones wired in, rest triaged |
| F8 | L34 | LOW | HIGH | src/runtime/governance_config.cpp:1778 | FIXED | `loadHook` iterates `args` without `.is_array()` guard (violates repo type-guard convention) |
| F1 | L21 | LOW | HIGH | src/runtime/stack_tracer.cpp:43 | FIXED | Stack traces always emit ANSI color, ignoring `--no-color`/non-TTY — ANSI leaks into logs |
| F2 | L1 | LOW | HIGH | src/debugger/debugger.cpp:251 | FIXED | `inspectVariable()` always returns null (stale TODO); REPL `:var` always says "not found" |
| F6 | L7 | LOW | MEDIUM | src/runtime/governance_reports.cpp:607 | FIXED | Windows hook `quoteArg` mishandles trailing backslashes — arg splitting/merging |
| C1 | L33 | LOW | HIGH | expressions.cpp:1065, compiler.cpp:535,644 | FIXED | Out-of-range float literal leaks raw "stod" as the error message |
| F7 | L7/L10 | LOW | MEDIUM | src/cli/main.cpp:172 | DEFERRED | Subprocess-fallback Python executor silently returns null in expression position |

---

## Findings (detail)

### F3 — Test runner swallows security-test failures — HIGH (L19 Testing Illusion)
**Location:** `run-all-tests.sh:1419` (test_uncatchable.sh), `:1432` (test_subprocess_containment.sh), `:1321` (test_drift_detection.sh)
**Finding:** Three sections append to `FAILED_TESTS` but never increment the `FAILED` counter. The suite's exit gate is `[ $FAILED -gt 0 ]`, so a failure of the GovernanceHardError-uncatchability test, the subprocess-containment security test, or drift detection produces exit 0 and prints "ALL TESTS ACCOUNTED FOR."
**Evidence:** `else\n    FAILED_TESTS+=("test_uncatchable.sh")\nfi` — no `FAILED=$((FAILED + 1))`. Verified: these are exactly the security tests whose whole purpose is to prove HARD blocks can't be caught and subprocess containment holds. This is the same class as the prior audit's H1 finding (a runner typo that hid 42 tests).
**Verification:** Enumerated all 40 `FAILED_TESTS+=` sites; only these 3 (plus one intentionally-commented line for a documented pre-existing config test) lacked the increment.
**Fix:** Added `FAILED=$((FAILED + 1))` at all three sites. With the fix, the container's subprocess-containment failure (RLIMIT_NPROC not enforced here) now surfaces honestly instead of being hidden.

### F4 — Undefined behavior casting double to int in math builtins — MEDIUM (L37 Numeric Overflow)
**Location:** `src/stdlib/math_impl.cpp:87,96,105`
**Finding:** `math.floor/ceil/round` did `static_cast<int>(std::floor(x))`. Casting a double whose value is outside `[INT_MIN, INT_MAX]` — or NaN — to `int` is undefined behavior in C++. `math.floor(3e10)` returned a garbage/implementation-defined value silently.
**Evidence:** The value layer already handles this correctly: `NaabVal::toInt()` (naab_val.cpp:507-514) clamps NaN→0 and saturates to INT_MAX/INT_MIN, and the `int(string)` builtin was clamped by a prior audit's H2 fix (call_dispatch.cpp:2849). The math module was the outlier. VM routes `math.*` through this same stdlib function, so one fix covers both engines.
**Fix:** Added a `clampToInt()` helper matching `NaabVal::toInt()` semantics; applied to all three. Regression test: `tests/bugs/test_math_int_cast_clamp.naab`.

### F5 — Orphaned test scripts never run — MEDIUM (L19/L20 Testing Illusion / Ownership)
**Location:** 15 scripts, incl. `tests/governance_v4/{test_hooks.sh, test_transcript.sh, test_output_admissibility.sh, test_rate_limiter.sh, test_reality_checkpoint.sh, test_semantic_signals.sh, test_naab44_fixes.sh}`, `tests/agent/{test_agent_governance_depth.sh, _symmetry.sh, _unification.sh, test_execution_governance.sh}`, and 4 under `governance_v4/{depth,docs,edge}/`.
**Finding:** These scripts exist and (several) pass, but are referenced by neither `run-all-tests.sh` nor any `.github/workflows/*.yml`. `test_hooks.sh` is brand new (PR #64) and covers the entire governance-hook feature — 18 assertions that would never run in CI. `test_transcript.sh` is cited in CLAUDE.md as *the* transcript test (28 assertions). Features shipping with tests that never execute means regressions are invisible.
**Verification:** For each of `tests/governance_v4/*.sh`, `tests/agent/*.sh`, and nested dirs, checked membership in the runner and CI. Ran the three no-API-key scripts standalone: test_hooks 18/18, test_output_admissibility 26/0, test_naab44_fixes 12/0.
**Fix (partial):** Wired the three verified-passing, no-credential scripts into `run-all-tests.sh` (test_hooks gated POSIX-only since it uses the fork/execv hook path). The remaining scripts require live API keys or deeper triage and are left for a follow-up rather than wiring untested scripts that could introduce spurious CI failures.

### F8 — Missing type guard on hook `args` — LOW (L34 Configuration Explosion)
**Location:** `src/runtime/governance_config.cpp:1778`
**Finding:** `if (hj.contains("args")) for (auto& a : hj["args"]) ...` iterated `args` without checking `.is_array()`. The repo has a documented convention of 507 JSON type guards in this file (config-fuzz hardening); this one was missing. `"args": "foo"` (string) would iterate characters/values; `"args": {...}` would iterate object values.
**Verification:** Confirmed against the surrounding guarded parses (every sibling field checks its JSON type). Not on any known-intentional list.
**Fix:** Added `&& hj["args"].is_array()`.

### F1 — Stack traces ignore the global color setting — LOW (L21 UX/Communication)
**Location:** `src/runtime/stack_tracer.cpp:43`
**Finding:** `formatTrace()` always called `StackFormatter::formatColored()`, embedding ANSI escapes into stack traces that are interpolated into exception `.what()` strings (js_executor.cpp:202, cpp_executor.cpp:577/585, rust_executor.cpp:440). `Diagnostic::isGlobalColorEnabled()` exists (error_reporter.h:43) and is set from `--no-color` (main.cpp:1258) but was never consulted here — so ANSI leaks into logs and non-TTY output. A stale TODO on the exact line acknowledged the gap.
**Fix:** Honor `Diagnostic::isGlobalColorEnabled()`, falling back to the existing `formatPlain()`.

### F2 — Debugger `inspectVariable` is a permanent stub — LOW (L1 Empty Placeholder)
**Location:** `src/debugger/debugger.cpp:251`
**Finding:** `inspectVariable()` unconditionally returned null behind a stale TODO ("Use when Environment interface is available"). The interface *is* available (`Environment::has/get`, interpreter.h:419-421) and `current_environment_` is populated at 7 interpreter sites. So the REPL `:var <name>` command (repl_commands.cpp:592) always printed "Variable not found" regardless of state.
**Fix:** Look the name up via `has()` (guard) + `get()`. `listLocalVariables()` already worked via call-stack frames, so `:locals` was unaffected; this closes the single-variable path.

### F6 — Windows hook arg-quoting mishandles backslashes — LOW (L7 Interface-Glue, Windows-only)
**Location:** `src/runtime/governance_reports.cpp:607` (`fireHook` Windows `CreateProcessA` path)
**Finding:** `quoteArg` escaped `"` as `\"` but did not double backslashes preceding a quote or at the end of an argument, violating the `CommandLineToArgvW` rules. An argument ending in `\` produced `"...\"`, where the trailing backslash escapes the closing quote and merges the argument with the next one. POSIX (`execv`) is unaffected.
**Verification:** Standard Windows quoting rule (2N+1 backslashes before a quote, 2N for a trailing run). Cannot execute on this POSIX host, hence MEDIUM confidence, but the fix is the textbook algorithm.
**Fix:** Track backslash runs and emit the correct doubled counts before quotes and at arg end.

### C1 — Out-of-range float literal leaks "stod" — LOW (L33 Error Propagation)
**Location:** `src/interpreter/expressions.cpp:1065`, `src/vm/compiler.cpp:535`, `src/vm/compiler.cpp:644`
**Finding:** Float literals were parsed with an unguarded `std::stod`. A literal exceeding double range (e.g. a 400-digit decimal) throws `std::out_of_range`, surfacing as `Error: stod` — an internal implementation detail, not a NAAb diagnostic. The integer-literal path was already guarded ("Invalid integer literal"); the float path was the asymmetric outlier, in both engines.
**Verification:** Reproduced `Error: stod` on both VM and tree-walker before the fix; both now report `Invalid float literal: <value>`.
**Fix:** Wrapped all three `stod` sites with a clear message, mirroring the existing integer-literal handling.

### F7 — Subprocess-fallback Python silently returns null — LOW, DEFERRED (L7/L10)
**Location:** `src/cli/main.cpp:172` (`GenericSubprocessExecutor` registered when pybind11 absent)
**Finding:** When built without pybind11, expression-position `<<python ... >>` blocks return null with no warning (the subprocess executor yields a value only via stdout), whereas the embedded pybind11 executor returns the expression value. This is a silent semantic divergence between build configurations. It also caused ~10 baseline test "failures" that were actually a missing build dependency, misclassified because the runner's "Python support not available" grep never matches VM-mode output.
**Why deferred:** A correct fix (making the subprocess executor capture expression values, or emitting a clear diagnostic) is a design change to the executor contract, larger than this audit's scope. Recommended follow-up: emit a one-time stderr warning when an expression-position polyglot block yields no value under the subprocess fallback. In this environment the immediate cause was resolved by installing pybind11 (rebuild enabled the embedded executor; those 10 failures disappeared).

---

## Per-Level Status (all 40)

**Clean / verified working-as-designed:** L2 (test assertions verify behavior, not just types), L3 (CDD weights configurable + labeled advisory), L4 (governance heuristic boundaries labeled), L6 (edge cases guarded: JSON int range at json_impl.cpp:114, env parse try/catch, NaabVal saturation), L8 (module-load governance skip documented), L9 (sandbox tiers by design; env scrub verified), L10 (error paths log; F1 was the exception), L11 (docs refreshed in PR #65), L12 (CI present), L13 (n/a), L14 (async drain / detached timers use generation counters — no cascade), L15 (recovery paths present), L16 (telemetry has run_id + hash chain), L17 (context windowing caps O(n²); buildIndex has transaction), L18 (submodules pinned; DEPENDENCIES.lock matches actual hashes — verified all 6), L20 (F5 orphans were the finding), L22 (UTF-8 standard), L23 (bounded loops/timeouts), L24 (build fragility → submodule init + libcurl/pybind11 deps, environmental), L25 (telemetry path sanitization intact; no new PII vectors), L26 (API versions pinned intentionally), L27 (retry/backoff in agent_impl by design), L28 (dismiss-test-alerts legit), L29 (data transforms guarded), L31 (thread-locals + two-phase mutex sound; validateHandle constant-time), L32 (no dangling c_str/detach lifetime bugs found), L35 (JSON large-int → double promotion correct), L38 (EINTR handled in subprocess waitpid; urandom fopen/fclose paired), L39 (init order sound; PR #64 fork-safety verified), L40 (handle nonce + config_name authenticated at validateHandle; ratchet invariants hold).

**Findings:** L1 (F2), L7 (F6/F7), L19 (F3/F5), L21 (F1), L33 (C1), L34 (F8), L37 (F4).

---

## Notes / Observations (not promoted to findings)

- `run-all-tests.sh:1300-1312` deliberately does not count `test_govern_json_config.sh` failures ("pre-existing CI-only issue") — documented decision, standing debt.
- `fireHook` POSIX child scrubs only the 3 `NAAB_*` secret keys while the Windows path applies the full `shouldScrubEnvVar` policy — a platform asymmetry that errs toward *more* scrubbing on Windows, so not a security gap.
- PR #64's subprocess_helpers env-scrub refactor is correct: `use_custom_env` covers the policy-active case pre-fork (subprocess_helpers.cpp:664); the removed child-side loop was genuinely dead code.
- CLI flag registration is asymmetric between the global pre-scan and the run-command loop (e.g. `--memory-limit`, `--quiet` position-sensitive), but each unknown position fails loudly (exit 1/4), so this is a UX nit, not silent slop.
- The prior "24,167 blocks" REPL hardcode fix (Gemini G-2) missed two REPL variants: `repl_optimized.cpp:113,131` and `repl_readline.cpp:329,346` still print it. These are separate binaries (`naab-repl-opt`, `naab-repl-rl`); left as an observation (cosmetic, matches the already-accepted pattern in the primary REPL's earlier state).

---

## Environmental baseline failures (pre-existing, not introduced)

These fail in this container on a clean tree, independent of the audit changes:
- `test_subprocess_containment.sh` T7 — RLIMIT_NPROC=0 fork-block not enforced by this container's kernel/cgroup; the chr()-constructed subprocess is not blocked. Now correctly counted as a failure thanks to F3.
- `test_init_governance.sh` — the `check` helper's nested-quote `python3 -c` assertions error under this shell even though the generated govern.json has the exact expected section counts (verified manually: 7 capabilities, 23 code_quality, 10 restrictions). Test-harness fragility, not a product bug.
- `test_libnaab_build.sh` — expects the `libnaab.a`/`.so` target, which was not built (only `naab-lang` was targeted).
