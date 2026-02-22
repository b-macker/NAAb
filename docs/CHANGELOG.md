# Changelog

All notable changes to NAAb will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-02-21

### Added
- **Polyglot Enhancements**
  - Native data binding for Ruby, PHP, compiled languages (context file serialization)
  - JSON Sovereign Pipe (`naab_return()`, `-> JSON` header, sentinel parsing)
  - Persistent sub-runtime contexts (`runtime` keyword, `.exec()` method)
  - Block header first-line awareness (Go `package main`, PHP `<?php`)
  - Error mapping with SourceMapper wired into polyglot catch blocks
  - Polyglot error context (code preview in error messages)
  - Exception propagation from polyglot blocks to try/catch
  - Configurable polyglot timeouts
  - Parallel polyglot execution with dependency analysis
  - Thread-safe temp files for C++, Rust, C# executors
  - Python indentation fix for embedded blocks
- **Language Features**
  - Lambda expressions with closure capture (`fn(x) { return x * 2 }`)
  - If expressions (`let x = if cond { a } else { b }`)
  - Pipeline operator fix (evaluate right side lazily)
  - Copy-on-assignment value semantics for arrays and dicts
  - Assignment-in-condition detection
  - Control flow validation (break/continue outside loops)
  - Type coercion errors (strict arithmetic, permissive string concat)
  - Silent bug detection (div-by-zero, modulo-by-zero, overflow)
- **Error Messages & Developer Experience**
  - "Did you mean?" suggestions using Levenshtein distance (all 12 stdlib modules)
  - Common mistake helpers (~35 patterns: camelCase, Python/JS naming conventions)
  - Better function argument count/type errors
  - Arrow function syntax helper errors
  - Debug module (debug.inspect, debug.type)
  - LLM-friendly parser (keyword aliases `func`/`function`, semicolons, helper errors)
- **Standard Library**
  - 204 error messages across 12 modules with fuzzy matching
  - Missing functions: string.reverse, string.char_at, env.set_var
  - Type naming consistency ("array" not "list")
- **CLI & REPL**
  - Auto-detect `.naab` extension (no `run` subcommand needed)
  - `--help` flag support
  - Buffered output fix (fflush)
- **Infrastructure**
  - Sandboxing and permissions (capability-based access control)
  - Block versioning and deprecation warnings
  - Semantic version parser and comparator
  - Comprehensive mono test suite (325 PASS, 0 FAIL across 12 sections)
  - Portable path resolution (no hardcoded platform paths)

### Changed
- All hardcoded Termux paths replaced with portable path resolution
- CMakeLists.txt uses `project(VERSION ...)` for version management
- Version display shows git commit hash and build timestamps
- Security library includes sandboxing infrastructure
- README updated to reflect accurate feature set and test counts
- All GitHub URLs updated to `github.com/b-macker/NAAb`
- SECURITY.md contact info updated to use GitHub Security Advisories

### Fixed
- `>>` polyglot block delimiter now only closes at line start
- JavaScript type conversion in polyglot blocks
- Python large integer handling
- Pipeline operator evaluation order
- String.reverse and string.char_at dispatch
- Debug module registration

## [0.1.0] - 2024-12-27

### Added
- Phase 1: Security Hardening
  - Resource limits with 30-second execution timeout
  - Input validation and path canonicalization
  - SHA256 code integrity verification (crypto_utils.h/cpp)
  - Security audit logging with JSON format and rotation
  - Timeout protection for C++, JavaScript, and Python executors
  - Path traversal prevention
  - Command injection protection
- Phase 4: Debugging and Performance (completed first)
  - Lazy block loading for faster startup
  - Modern C++ syntax support
  - Test organization into tests/ directory

### Fixed
- Interpreter performance issues
- Block loading errors
- Debugger include path (debugger.h → naab/debugger.h)
- Type mismatch in executeStmt (simplified breakpoint handling)

### Security
- POSIX signal-based timeout mechanism using `alarm()` and `SIGALRM`
- QuickJS interrupt handler for JavaScript timeout enforcement
- Path validation using `realpath()` before library loading
- 30s compilation timeout, 5s dlopen timeout, 10s FFI call timeout
- Security audit logs written to `~/.naab/logs/security.log`

## [0.0.1] - 2024-12-15

### Added
- Multi-language block support (C++, JavaScript, Python)
- REPL with readline support (naab-repl, naab-repl-rl)
- Standard library with 13 modules:
  - Core: io, json, http
  - String/Data: string, array, math
  - System: time, env, csv
  - Advanced: regex, crypto, file, collections
- Cross-language type marshalling
- 24,491 blocks across 15 languages
- Block registry with SQLite backend
- Dynamic C++ compilation and loading
- QuickJS integration for JavaScript
- pybind11 integration for Python (optional)
- Comprehensive documentation:
  - USER_GUIDE.md
  - API_REFERENCE.md
  - ARCHITECTURE.md
  - QUICK_REFERENCE.md
  - Tutorial series
- Lexer, Parser, AST, Type Checker, Interpreter
- Language Registry for executor management
- C++ executor with libffi for function calls
- JavaScript executor with QuickJS
- Python executor with pybind11

### Infrastructure
- CMake build system (C++17, CMake 3.15+)
- External dependencies: Abseil, fmt, spdlog, SQLite3
- Build script: build-and-install.sh
- Test suite with multiple test executables
- Example programs for cross-language integration

[Unreleased]: https://github.com/b-macker/NAAb/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/b-macker/NAAb/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/b-macker/NAAb/releases/tag/v0.1.0
[0.0.1]: https://github.com/b-macker/NAAb/releases/tag/v0.0.1
