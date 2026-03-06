# Changelog

All notable changes to NAAb will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
