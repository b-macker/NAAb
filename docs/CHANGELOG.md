# Changelog

All notable changes to NAAb will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Phase 3: Sandboxing and Permissions
  - Capability-based permission system (sandbox.h/cpp)
  - Filesystem access control with path whitelisting
  - Network access control (connect/listen restrictions)
  - Permission levels: RESTRICTED, STANDARD, ELEVATED, UNRESTRICTED
  - ScopedSandbox for RAII-style sandbox activation
  - SandboxManager for per-block permission configuration
  - Sandbox violation exceptions with detailed error messages
- Phase 2 Extensions: Block Versioning
  - Block version compatibility checking (min_runtime_version)
  - Deprecation warning system with formatted display
  - Version bumping automation script (scripts/bump_version.sh)
  - BlockMetadata extended with version fields
- Phase 2: Versioning and Release Management
  - Semantic version parser and comparator (semver.h/cpp)
  - Runtime version tracking with git metadata and build timestamps
  - Version macros in config.h for consistent version display
  - Enhanced `naab-lang version` command with build information
- CHANGELOG.md following Keep a Changelog format

### Changed
- CMakeLists.txt now uses `project(VERSION ...)` for version management
- Version display now shows git commit hash and build timestamp
- Removed hardcoded version strings from main.cpp
- Security library now includes sandboxing infrastructure

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
- Build script: build_and_install.sh
- Test suite with multiple test executables
- Example programs for cross-language integration

[Unreleased]: https://github.com/naab-lang/naab/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/naab-lang/naab/releases/tag/v0.1.0
[0.0.1]: https://github.com/naab-lang/naab/releases/tag/v0.0.1
