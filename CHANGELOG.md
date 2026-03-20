# Changelog

All notable changes to NAAb will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.5.1] - 2026-03-20

### Fixed
- Float serialization in polyglot bindings — `serializeValueForLanguage()` used `std::to_string()` (6 decimals), now uses `%.15g` matching `Value::toString()` (FIX-1)
- Dict key escaping in polyglot serialization — keys with `"`, `\`, `\n` now properly escaped across all 7 language paths (FIX-2)
- Subscript assignment taint propagation — `dict["k"] = tainted` and `arr[0] = tainted` now mark container as tainted (FIX-3)
- Struct field assignment taint propagation — `obj.field = tainted` now marks struct as tainted (FIX-4)
- Dot-notation method mutation taint — `arr.push(tainted)`, `dict.put("k", tainted)` now propagate taint at 4 code locations (FIX-5)
- Stale mono test assertion — float concatenation test updated from `"3.140000"` to `"3.14"` (FIX-6)
- Documentation: math.pi/math.e gotcha removed (both cases work), float format gotcha updated
- Catch-throw taint restore — exception paths in catch blocks now restore outer taint state (BUG-1)
- ForStmt taint from sources — `for item in env.get("PATH")` now marks loop variable as tainted (BUG-2)
- ReturnStmt taint collapsed to unified `checkRhsTainted()` helper — 33 lines to 3 (BUG-3)
- Parallel polyglot all-language taint — was shell-only, now checks python_exec, go_exec, etc. (BUG-4)
- `checkRhsTainted()` nested CallExpr guard removed + consume-once semantics — `let x = arr[get_tainted()]` detects inner return taint without false-positiving on subsequent literals (BUG-5 + FIX-A)
- Deduplicated polyglot sink-type mapping into `checkPolyglotBoundVarTaint()` helper (FIX-D)

### Changed
- Governance regression suite: 319 → 339 tests (split categories: passes, error behavior, missing executor)
- Mono exhaustive test: 292 → 293 PASS (0 FAIL)
- Test runner categories split: error behavior tests vs missing executor tests vs skip files
- Real taint e2e integration test added (12 scenarios exercising full taint pipeline)

## [0.5.0] - 2026-03-19

### Added
- **Advisory Noise Reduction** — governance output reduced from 100+ lines to 3-5
  - Grouped duplicate call warnings (DX-6): deferred, deduplicated, configurable threshold
  - Grouped polyglot try/catch warnings (DX-7): compact multi-function summary
  - `emitAdvisory()` centralized output with `output.max_advisories` cap (default 15)
  - Suppression summary: "... and N more advisories suppressed"
- **Configurable advisory thresholds** in govern.json:
  - `code_quality.duplicate_calls` — enabled, threshold (default 3), max_entries (default 5)
  - `code_quality.polyglot_try_catch` — enabled, max_entries (default 3)
  - `output.max_advisories` (default 15, 0 = unlimited), `output.advisory_summary`
- **15 DX Governance Improvements** (DX-1 through DX-15):
  - Taint matching changed from substring to prefix (DX-1)
  - All-language polyglot binding taint checks — python_exec, go_exec, etc. (DX-2)
  - MemberExpr sanitizer detection — module.sanitize_foo() clears taint (DX-3)
  - Reserved name warnings — result_, returning_, etc. (DX-4)
  - Unused binding detection within same scope block (DX-5)
  - Duplicate call detection in function bodies (DX-6)
  - Try/catch polyglot advisory warning (DX-7)
  - Scope pattern validation with glob matching (DX-8)
  - Nim JSON error helper for -> JSON blocks (DX-9)
  - Type-safe binding hints for Go/Nim complex types (DX-10)
  - Cross-language hallucination detection — +5 Nim, +3 Go patterns (DX-11)
  - Per-language polyglot_output taint — "polyglot_output:python" syntax (DX-12)
  - JSON roundtrip waste detection (DX-13)
  - Missing executor install guidance per language (DX-14)
  - govern.json schema validation with sanitizer false positive warnings (DX-15)
- **Exhaustive Governance Test Suite** — 197 edge-case tests across 19 files
  - 10 categories: sinks, sources, expressions, async, modules, degraded, recursive, polyglot, interactions, docs
  - Chaos testing: taint washing, silent swallow, direct bypass, concurrent taint, GC stress
- **C++ Native Scanner** (`naab-lang --scan`) — 139 checks across 11 source files
  - 6 categories: redundancy, code_quality, complexity, style, security, language-specific
  - Language modules: Python (14), JavaScript (12), C++ (12), Go (9), Rust (10), NAAb (11)
  - Text + JSON + SARIF output, govern.json configurable
- **GC Cycle Detector Fix** (BUG-10) — env_stack_ tracks all live environments during GC

### Fixed
- Optimization hints verbose 10-line block printed at all enforcement levels — now only for "hard"
- MatchExpr taint propagation — match arms now checked via expressionContainsTaint
- AwaitExpr taint propagation — async return taint chain fixed with lastReturnWasTainted check
- 7 critical review followup fixes: assignment/return/for-stmt await paths, dead code removal
- Polyglot return in functions (BUG-1) — returning_ flag checked in CompoundStmt loop

### Changed
- govern-template.json synced to v4.0 (was v3.0 at root level)
- Governance regression suite: 208 → 339 tests (100% pass rate)

## [0.4.0] - 2026-03-18

### Added
- **Governance v4.0 — Taint Tracking** — variable-level tracking of untrusted data from
  sources (env.get, io.read_line, file.read, polyglot_output) through to sinks (shell_exec,
  file.write, http.*, env.set_var) with configurable enforcement (hard/soft/advisory)
  - Propagation through string concatenation, interpolation, function returns, for-loop iterators
  - Sanitizer functions clear taint (validate_*, sanitize_*, escape_*, int(), float())
  - Name collision fix: taint cleared on variable redeclaration with clean value
  - Thread-safe taint_set_ with mutex for async/parallel execution
  - Taint state saved/restored around module loading to prevent cross-module leaks
- **Cross-Module Input Contracts** — parameter type validation at function call sites
  via govern.json `contracts.functions.*.params` (format: `"name:type"`)
  - `validate_inputs: true` enables input contract checking
  - Checked on both callFunction() and direct call dispatch paths
- **Enhanced Audit Trail** — JSONL logging with tamper-evident hash chains
  - `polyglot_timing`: execution duration per polyglot block
  - `taint_decisions`: every taint mark/clear/block decision logged
  - `contract_checks`: every contract pass/fail logged with details
  - Parallel polyglot blocks now logged in audit trail
- **Dead Conditional Scanner** — regex-based detection for impossible conditions
  - `len(x) < 0`, contradictory comparisons, null access after null check, type impossibilities
  - Data flow analysis: range tracking from math.max/min/clamp/for-loops
- **Configurable Scanner** — govern.json `scanner` section with per-check enable/disable
- **17 Edge Case Bugs Fixed** during exhaustive hardening:
  - Assignment path taint tracking (BUG-A)
  - Expression tree taint propagation for string concat/interpolation (BUG-B/C)
  - Function return taint via lastReturnTainted flag (BUG-D)
  - For-loop iterator taint from tainted iterables (BUG-E)
  - HTTP URL taint sink check / SSRF prevention (BUG-F)
  - env.set_var() taint sink check (BUG-G)
  - Async interpreter governance propagation (BUG-I)
  - Parallel polyglot bound variable taint check (BUG-J)
  - Await resolution return contract check (BUG-K)
  - taint_set_ mutex for thread safety (BUG-N)
  - Module loading taint state isolation (BUG-O)
  - Parallel polyglot audit logging (BUG-P)
  - Direct call path contract audit logging (BUG-Q)

### Changed
- govern.json template updated with v4.0 sections (taint_tracking, enhanced contracts, audit)
- FutureValue now stores func_name for return contract checking at await resolution

### Tests
- 49 governance v4 tests across 12 test files (all passing)
- 208/208 regression suite (100% pass rate)

## [0.3.1] - 2026-03-05

### Added
- **Project Context Awareness** — governance reads project files to supplement `govern.json`
  - Layer 1: LLM instruction files (CLAUDE.md, .cursorrules, copilot-instructions.md, gemini.md)
  - Layer 2: Linter/formatter configs (.editorconfig, .eslintrc, .prettierrc, biome.json)
  - Layer 3: Package manifests (package.json, go.mod, Cargo.toml, pyproject.toml)
  - Opt-in (`"project_context": { "enabled": true }` in govern.json), off by default
  - govern.json always overrides — project context supplements, never conflicts
  - Each layer independently toggleable, dry-run mode, surgical rule suppression
  - Extracted language preferences feed into polyglot optimization scoring
  - Manifest detection is advisory-only — never blocks new languages
  - Full extraction report with source file + line number for every rule
  - 29 edge case tests covering conflicts, priority_source, code block skipping, and more
- Nim, Zig, Julia polyglot executors (3 new languages, 15 total)
- Governance v3.0 — 50+ checks, 13 config sections, per-language rules, custom rules engine
- Polyglot optimization system — 7 detection layers, 205+ patterns, task-language scoring
- Empirical profiling and calibration for governance suggestions
- Cross-language consensus verification

## [0.2.0] - 2025-02-24

### Added
- Pattern matching with `match` expressions
- Async/await support for concurrent function execution
- Dedicated Go executor for polyglot blocks
- REPL tab completion (keywords, stdlib, user symbols, dot notation)
- Lambda expressions with closure capture
- If expressions (`let x = if cond { a } else { b }`)
- Parallel polyglot execution with dependency analysis
- Persistent sub-runtime contexts (`runtime` keyword)
- JSON sovereign pipe (`naab_return()`, `-> JSON` header)
- Native data binding for polyglot blocks (Ruby, PHP, compiled langs)
- Debug module (`debug.inspect`, `debug.type`)
- "Did you mean?" suggestions with fuzzy matching
- Common mistake helper errors (~35 patterns)
- Polyglot error mapping with source location
- Polyglot timeout configuration
- CI workflows: Build & Test, Sanitizers (ASan/UBSan), CodeQL, Supply Chain, Release

### Fixed
- Pipeline operator evaluates right side lazily
- Polyglot `>>` delimiter only closes at line start
- JavaScript type conversion in polyglot blocks
- Python large integer handling
- Python indentation preservation in polyglot blocks
- Thread-safe temp files for C++, Rust, C# executors
- Copy-on-assignment semantics for arrays and dicts
- Break/continue validation outside loops
- Assignment-in-condition detection

## [0.1.0] - 2024-12-01

### Added
- Core language: variables, functions, control flow, loops
- Polyglot block execution (Python, JavaScript, Rust, C++, C#, Shell, Ruby, PHP)
- Standard library: array, string, math, time, env, file, io, json, http, csv, regex, crypto
- REPL with linenoise (arrow keys, history, Ctrl+R search)
- Block registry with SQLite storage
- Error reporting with source locations
