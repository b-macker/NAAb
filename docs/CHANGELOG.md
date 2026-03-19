# Changelog

All notable changes to NAAb will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.2] - 2026-03-19

### Added
- **Exhaustive governance edge-case test suite** — 197 tests across 19 files covering bypass paths, state consistency, async/degraded scenarios, expression taint for all 15 AST types, scope pattern matching, per-language taint, enforcement levels, config edge cases, cross-feature interactions, and documented known limitations.

### Fixed
- **Taint matching: substring→prefix** — `isTaintSource()`, `isSanitizer()`, `checkTaintedSink()` now use prefix matching instead of substring. `"int("` no longer matches `"print("`. (FIX-DX-1)
- **All-language polyglot taint checks** — tainted variable binding checks now apply to all languages (Python, Go, JS, Nim), not just Shell. Configure per-language sinks: `"python_exec"`, `"go_exec"`, etc. (FIX-DX-2)
- **MemberExpr sanitizer detection** — `module.sanitize_foo()` calls now correctly clear taint (was only detecting plain `sanitize_foo()` calls). (FIX-DX-3)

### Added
- **Reserved name warning** — warns when variables shadow interpreter internals like `result_`. (FIX-DX-4)
- **Unused binding detection** — warns when polyglot-bound variables are never used in the block code. (FIX-DX-5)
- **Duplicate call detection** — advisory when same function (e.g., `json.parse()`) is called multiple times in a function body. (FIX-DX-6)
- **Try/catch polyglot advisory** — warns when polyglot blocks lack error handling. (FIX-DX-7)
- **Scope pattern validation** — warns when governance scope patterns match zero functions in the file. (FIX-DX-8)
- **Nim JSON error helper** — language-specific `-> JSON` guidance for Nim polyglot blocks. (FIX-DX-9)
- **Type-safe binding hints** — hints for Go/Nim when complex types (dict/array) need manual parsing. (FIX-DX-10)
- **Cross-language hallucination patterns** — expanded Nim (+5) and Go (+3) patterns for cross-language API confusion. (FIX-DX-11)
- **Per-language polyglot_output taint** — `"polyglot_output:python"` syntax for language-specific taint sources. (FIX-DX-12)
- **JSON roundtrip hint** — detects `json.stringify()` → bind → `json.loads()` waste pattern. (FIX-DX-13)
- **Missing executor helper** — install guidance per language when executor not found. (FIX-DX-14)
- **govern.json schema validation** — warns about sanitizer patterns prone to false positives and empty scope patterns. (FIX-DX-15)

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
