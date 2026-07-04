# NAAb Repository Audit Report

**Date:** 2026-07-02
**Scope:** Full repository at commit `d0e4d6c` — source (`src/`, `include/`), build system, CI workflows, tests, docs, and repo hygiene.
**Method:** Static analysis (pattern scans, manual code review, cross-referencing docs against code, git metadata analysis). Dynamic testing (build + test suite) was **not possible** in the audit environment: the network policy blocked cloning the `external/` submodules (abseil, fmt, spdlog, json, tomlplusplus, googletest), and CMake hard-requires them with no system-library fallback. All build/test-dependent claims are therefore unverified here.

---

## Summary

| Severity | Count |
|----------|-------|
| High     | 6     |
| Medium   | 10    |
| Low / Informational | 6 |

The codebase is in generally good shape — constant-time crypto comparisons, consistent exception-safety re-throw patterns, OpenSSL-backed crypto, no `strcpy`/`sprintf`/`gets`, CodeQL + sanitizer + supply-chain CI. The most impactful problems are a **silent test-coverage gap** (200-file test suite never executed due to a directory-name typo), **undefined behavior in the `int()` builtin** on both execution engines, a **crashable `time` stdlib module**, and a **stale dependency lockfile** that contradicts the actual submodule pins.

---

## High-severity findings

### H1. `run-all-tests.sh` never runs the real chapter-verification suite (directory name typo)
`run-all-tests.sh:44` registers `"tests/chapter verification"` (with a **space**). Two directories exist:

- `tests/chapter verification/` — 2 files (a MONO test + chaos_tests)
- `tests/chapter_verification/` — **200 files** (ch01–ch06 suites, IronDome project, etc.)

The runner silently skips missing dirs (`[ ! -d "$dir" ] && continue`), so if the space-named dir were ever removed nothing would even warn. As-is, the 200-file underscore suite is invisible to `run-all-tests.sh` — the "396 tests, 0 unexpected failures" headline excludes it. The `SKIP_DIRS` entry at line 165 repeats the space-named variant.

**Fix:** consolidate the two directories, point the runner at the surviving one, and make the runner fail loudly on a registered-but-missing directory.

### H2. Undefined behavior in `int(string)` conversion — both engines
NAAb ints are 32-bit (NaN-boxed). The numeric path clamps correctly (`NaabVal::toInt()` clamps to `INT_MIN`/`INT_MAX`; `expressions.cpp:1053` range-checks). But the **string path does an unclamped cast**:

- `src/vm/vm.cpp:261` — `makeInt(static_cast<int>(std::stod(s)))`
- `src/interpreter/call_dispatch.cpp:2850` — same pattern

`int("99999999999")` or `int("1e20")` converts an out-of-range `double` to `int`, which is **undefined behavior** in C++ (in practice: garbage values, and a trap under UBSan — the `sanitizers.yml` workflow may eventually catch this). Also inconsistent: `int(3000000000.0)` clamps to `INT_MAX` while `int("3000000000")` is UB.

**Fix:** clamp through the same logic as `toInt()` in both call sites.

### H3. `time` stdlib: `std::localtime` null-pointer dereference + thread-unsafety
`src/stdlib/time_impl.cpp` calls `std::localtime(&time)` at 8+ sites (lines 98, 141, 159, 177, 195, 213, 231, 249, …) and passes the result straight to `std::put_time` **without a null check**. For out-of-range timestamps (`time.format_timestamp(-9999999999999, ...)`) `localtime` returns `nullptr` → UB/crash from a pure NAAb script.

Additionally `std::localtime`/`std::gmtime` are non-reentrant. The runtime runs concurrent agent worker threads (CLAUDE.md documents this; `agent_impl.cpp` carefully uses `localtime_r`/`localtime_s`), so concurrent `time.*` calls can race. `src/runtime/audit_logger.cpp:123` and `src/runtime/tamper_evident_logger.cpp:593` also use `std::gmtime` — those run on telemetry paths that agent threads exercise.

**Fix:** switch to `localtime_r`/`gmtime_r` (with `_s` variants on Windows, as agent_impl.cpp already does) and error out on null.

### H4. `DEPENDENCIES.lock` diverges from actual submodule pins
The lockfile ("DO NOT EDIT MANUALLY — locks all dependency versions for reproducible builds") disagrees with `git ls-tree HEAD external/` on **4 of 5** comparable pins:

| Dependency | Lockfile commit | Actual submodule pin |
|---|---|---|
| abseil-cpp | `273292d1…` | `17947175…` |
| fmt | `e69e5f97…` | `7bce2257…` |
| spdlog | `76fb40d9…` | `0209b12c…` |
| nlohmann-json | `bc889afb…` | `9cca280a…` |
| googletest | `f8d7d77c…` | `f8d7d77c…` ✔ |

`tomlplusplus` is a submodule but absent from the lockfile entirely. The header says `naab_version: 0.8.1` / generated 2026-02-21, while the CHANGELOG is at 1.4.0+. Any tooling or audit trusting this file gets wrong answers; the hermetic-build story (`Dockerfile.hermetic`) is undermined.

**Fix:** regenerate via `scripts/update-dependencies.sh` and add a CI check that fails when lockfile and submodule pins diverge.

### H5. Private key material committed to the repository
Tracked files include real PEM private keys and a "prod"-named secret:

- `docs/book/verification/ch0_full_projects/Vigilant/config/{ca_key,client_key,server_key}.pem` — `BEGIN PRIVATE KEY`
- `tests/chapter_verification/ch0_full_projects/IronDome/config/key.pem` — `BEGIN PRIVATE KEY`
- `docs/book/verification/ch0_full_projects/Vigilant/config/api_key.secret` — `VIGILANT_PROD_KEY_72e13e3490a8f58b`

These are almost certainly demo fixtures, but: (a) they're named/labeled like production material (`api_key.secret`, "PROD"), (b) they match `.gitignore` patterns (see M1) yet are tracked, and (c) the gitleaks config path-allowlists `docs/.*` and `tests/.*`, so **secret scanning will never flag anything under these trees** — a real secret dropped there would ship silently.

**Fix:** generate demo certs at test setup time (a script already exists for other suites), or clearly mark fixtures (e.g. `DUMMY_` prefix) and narrow the gitleaks path allowlist.

### H6. Gitleaks allowlist regex neuters hex-secret detection repo-wide
`.gitleaks.toml` global allowlist includes:

```toml
'''[a-f0-9]{7,40}'''  # Git commit hashes (in CHANGELOG, docs, etc.)
```

This is an **unanchored** regex allowlisting any string containing 7–40 lowercase-hex chars — which describes a large class of real credentials (hex API tokens, HMAC secrets, the `VIGILANT_PROD_KEY_72e13e3490a8f58b` above). Combined with the `docs/`, `tests/`, `examples/`, `demo/` path allowlists, the supply-chain workflow's gitleaks job provides much weaker coverage than it appears to.

**Fix:** drop the hex regex (or anchor it to commit-hash contexts) and prefer `.gitleaksignore` fingerprints for specific false positives.

---

## Medium-severity findings

### M1. 25 tracked files violate the repo's own `.gitignore`
`git ls-files -i -c --exclude-standard` reports 25 files, including:

- **Committed binaries:** `docs/book/verification/.../Vigilant/proxy/gateway_vessel` (8.9 MB), `bin/gateway_vessel` (5.8 MB), two `shield_vessel` binaries (~0.5 MB each) — ~16 MB of unreviewable executables inflating a 11.5 MB pack to this size
- The private keys from H5
- `tests/gorilla/naab-{29,31_a,36,47}/results/*` — timestamped test-run outputs and telemetry JSONL
- `external/quickjs-2021-03-27/Makefile`, `.obj/.d` — vendored-build leftovers
- Two `govern*.json.sig` files (test comments elsewhere say ".sig files are gitignored")

**Fix:** `git rm --cached` the lot; binaries should be reproducible from source or fetched at test time.

### M2. Stray development artifacts committed at repo root
`codegen_telemetry.jsonl` (200 lines), `codegen_py_telemetry.jsonl` (128 lines), `int_test.txt`, `taint_test.txt`, `poly_report.txt` are one-off run outputs, and the JSONL files embed the developer's machine paths (`/data/data/com.termux/files/home/...`). They look like accidental `git add -A` passengers.

### M3. `BRANCH-REPORT.md` and `PR23-BRANCH-REPORT.md` are byte-identical
Two 36 KB copies of the same report at the root. Keep one (or move to `docs/`).

### M4. CI: 4 of 10 workflows have no `permissions:` block
`bindings.yml`, `ci.yml`, `sanitizers.yml`, `windows.yml` run with the repository's default `GITHUB_TOKEN` permissions (potentially write). Add `permissions: contents: read` at workflow level.

### M5. CI: third-party actions pinned to mutable tags, not SHAs
`softprops/action-gh-release@v2`, `anchore/scan-action@v5`, `msys2/setup-msys2@v2`, `gitleaks/gitleaks-action@v2`, `sigstore/cosign-installer@v3`, `anthropics/claude-code-action@v1`, etc. A compromised tag republish executes attacker code with repo credentials — a notable gap given the repo ships a dedicated `supply-chain.yml`. Pin third-party actions to full commit SHAs (`actions/*` org is lower risk but SHA-pinning everything is the standard).

### M6. Unguarded `std::stoi` in the parser
`src/parser/parser.cpp:1302`: `explicit_value = std::stoi(value_token.value)` for enum variant values. `enum { A = 99999999999 }` throws raw `std::out_of_range` from deep inside the parser instead of producing a NAAb parse error with file/line/hint — contrary to the project's own error-message conventions. (This is the only unguarded numeric-conversion site found in lexer/parser.)

### M7. Unescaped interpolation into `system()` in language detection
`src/cli/main.cpp:4037-4047`: `check_cmd = "which " + lang.runner + " >/dev/null 2>&1"; system(check_cmd)`. `lang.runner` comes from the language registry/config rather than remote input, so exploitability is low, but every other subprocess path in the codebase deliberately avoids the shell (`subprocess_helpers.cpp` uses `fork`/`execvp` precisely to prevent this). This site (and the adjacent `popen` at :4137, which does quote) should use the same helper or shell-escape consistently.

### M8. Windows quoting weaknesses in shell-outs
- `src/packages/package_manager.cpp:297` (Windows path): `tar xzf "{path}" -C "{dir}"` via `std::system` — `cmd.exe` double-quote quoting is escapable with embedded quotes/carets; the comment relies on `parseSpec` rejecting metacharacters, but `tarball_path`/`temp_dir` derive from filesystem paths too.
- `governance_reports.cpp:486` `shellEscape()` uses POSIX single-quote escaping; if governance hooks (`fireHook` → `system()`) ever run on Windows, single quotes are not quoting characters for `cmd.exe` and the escaping is ineffective.

**Fix:** route both through `subprocess_helpers` (argv-based) or add a Windows-specific escaper.

### M9. No git tags despite versioned CHANGELOG and release workflow
CHANGELOG documents 1.0.0 → 1.4.0 (and skips 1.2.0/1.3.0 entirely — versions jump 1.1.0 → 1.4.0), `release.yml` exists, but `git tag` is empty. Releases are not reconstructible from the repo; `[Unreleased]` has been absorbing everything since April.

### M10. `verify_governance_invariants.sh` reports false failures when the binary is missing
The script hardcodes `NAAB_BIN="./build/naab-lang"` and, when the binary doesn't exist, prints `11/11 FAIL — INVARIANT VIOLATIONS DETECTED` instead of "binary not built". Anyone (or any CI job) reading the output gets a false alarm about governance invariants. Add an existence check that exits with a distinct message/code. (Same applies to `run-all-tests.sh`, which at least would fail fast on first use of the binary.)

---

## Low / informational findings

### L1. 14 of 51 `tests/` directories are not referenced by `run-all-tests.sh`
`analyzer`, `chapter_verification` (H1), `e2e`, `enterprise`, `gorilla`, `governance_app`, `governance_v4_degraded`, `helpers`, `llm-ab-testing`, `performance`, `project_context`, `scanner`, `unit`, `usability`. Some are intentionally manual (gorilla needs live LLM keys), but CLAUDE.md lists `e2e`, `gorilla`, `scanner` as suite categories — either wire them in, document them as manual-only, or prune.

### L2. Documentation drift
- CLAUDE.md says "stdlib/ 22 modules" — there are **23** `*_impl.cpp` (the `governance` module is missing from the count/list, though documented elsewhere in the same file).
- USER_GUIDE.md:30 says "3-tier enforcement (HARD/SOFT/ADVISORY)"; README/CLAUDE.md say 4 tiers (+ DETECT).
- DEPENDENCIES.lock header says `naab_version: 0.8.1` (see H4).

### L3. Developer machine paths baked into 165 tracked files
`/data/data/com.termux/files/home/...` appears in 165 files — mostly docs/book verification transcripts, demo casts, and the committed telemetry JSONL (M2). Harmless functionally (the `src/` occurrences are legitimate Termux platform fallbacks), but it leaks the development environment and makes doc examples non-portable.

### L4. Build hard-requires submodules with no fallback
`CMakeLists.txt:171-183` uses `add_subdirectory(external/...)` unconditionally — no `find_package` fallback or `FetchContent`. In network-restricted environments (like this audit sandbox) or partial clones, configuration fails with raw CMake errors. Consider a submodule-presence check with a clear error message, and/or optional system-library fallback.

### L5. Deprecated OpenSSL 3.x low-level hash API
`src/runtime/crypto_utils.cpp` uses `SHA256_Init/Update/Final` and `SHA256()`, deprecated since OpenSSL 3.0 — deprecation warnings now, removal risk later. Migrate to `EVP_Digest*` (the file already includes `<openssl/evp.h>`).

### L6. `demo/` vs `demos/` split
Two top-level demo directories with different content (`demo/` = governance scenes, `demos/` = agent-orchestration). Merge or rename for discoverability.

---

## Positive observations

- `CryptoUtils::constantTimeCompare` is correctly branchless including on length mismatch, and is used at all sensitive comparison sites found (REST API keys, HMAC nonces, lockfile sigs).
- REST API auth (`src/api/rest_api.cpp`) does constant-time compare, default-deny scoping for unknown endpoints, and rate-limits after auth.
- Exception safety is consistently handled: all `catch (...)` blocks in interpreter/VM hot paths restore state and re-throw; `GovernanceHardError` re-throw guards are present in both engines (6 sites each).
- No `strcpy`/`strcat`/`sprintf`/`gets`/`tmpnam` anywhere in `src/`.
- Subprocess execution goes through argv-based `fork`/`execvp` with a 5-layer containment design (with the two exceptions noted in M7/M8).
- Agent handle secrets come from `/dev/urandom` with an explicit degraded-mode warning; API keys are never written to transcripts/telemetry (only env-var *names*).
- No merge-conflict markers; no tracked `.env`/credential files outside the demo fixtures noted above.

## Suggested remediation order

1. **H1** (test runner typo) — one-line fix, immediately restores ~200 tests of coverage.
2. **H2 + H3** (UB / crash bugs) — small, contained patches; add regression tests.
3. **H5 + H6 + M1 + M2** (secrets & hygiene) — one `git rm --cached` sweep plus gitleaks config tightening.
4. **H4 + M9** (lockfile & tags) — regenerate lock, add CI drift check, backfill tags.
5. **M4 + M5** (CI hardening) — mechanical workflow edits.
6. Remaining M/L items as maintenance.
