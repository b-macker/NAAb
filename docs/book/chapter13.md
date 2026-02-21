# Chapter 13: Security and Isolation

Executing code from multiple languages within a single application introduces unique security challenges. NAAb addresses these through a multi-layered security model designed to isolate foreign code execution and protect the host system.

## 13.1 The Security Model

NAAb's security architecture is built on the principle of **least privilege**. By default, the NAAb runtime operates with the permissions of the user running the process, but it employs strategies to contain the impact of the foreign code blocks it orchestrates.

### 13.1.1 Executor Isolation

Each language executor runs in a specific context:

*   **Embedded Executors (JavaScript, Python)**: These run within the same process as NAAb but are logically isolated.
    *   **JavaScript (QuickJS)**: Runs in a separate `JSRuntime` and `JSContext`. It does not have direct access to the host's file system or network unless explicitly provided via NAAb's bridge.
    *   **Python**: Runs in a controlled global namespace. Variables are injected explicitly, preventing accidental access to internal NAAb state.

*   **Subprocess Executors (Bash, Ruby, Go, Rust, PHP)**: These run as separate OS processes. This provides a strong boundary; a crash or memory leak in a Ruby block will not crash the main NAAb application. Data is marshaled via standard input/output pipes.

*   **Compiled Executors (C++, C#)**: These are compiled to native code and executed as subprocesses. While performant, malformed C++ code (e.g., buffer overflows) can produce undefined behavior in the subprocess. Use compiled blocks with caution and rigorous testing.

### 13.1.2 Design Principles

NAAb's security follows six core principles:

1. **Defense in Depth** — Multiple layers so no single failure compromises the system
2. **Least Privilege** — Run with minimal necessary permissions
3. **Fail Securely** — Errors do not create security vulnerabilities
4. **Complete Mediation** — Check every access, every time (no cached security decisions)
5. **Separation of Concerns** — Security logic in dedicated modules (`path_security`, `ffi_validator`, `safe_math`)
6. **Open Design** — Security through design, not obscurity

## 13.2 Security Architecture

### 13.2.1 Architecture Layers

NAAb implements defense in depth across six layers:

**Layer 1 — Input Layer**: Validates all external inputs before processing. Enforces size limits on files, strings, and collections.

**Layer 2 — Parsing Layer**: Converts source text to AST safely. The parser enforces a recursion depth limit (1000) and has been fuzz-tested with sanitizers.

**Layer 3 — Type Checking Layer**: Ensures type safety at compile time. Includes generic type validation and runtime type validation at FFI boundaries.

**Layer 4 — Execution Layer**: Executes code safely with call stack limits (10,000), smart pointers (RAII, no manual memory management), arithmetic overflow checking, and array bounds checking.

**Layer 5 — Standard Library Layer**: Provides safe built-in operations. File I/O uses path canonicalization and traversal prevention. All operations enforce size limits and input validation.

**Layer 6 — FFI Layer**: Safely marshals data across language boundaries. Validates types, enforces size limits, rejects NaN/Infinity, and detects null bytes.

### 13.2.2 Security Components

**Path Security** (`naab/path_security.h`): Prevents directory traversal and unauthorized file access through path canonicalization, `..` blocking, base directory validation, and dangerous pattern detection (null bytes, control characters).

**FFI Validator** (`naab/ffi_validator.h`): Validates data crossing language boundaries with type safety checking, string size limits, collection depth limits (100 levels), total payload size limits (10MB), and NaN/Infinity rejection.

**Safe Math** (`naab/safe_math.h`): Prevents arithmetic overflow/underflow using compiler builtins (`__builtin_add_overflow`), division by zero detection, and array bounds checking.

**Error Sanitizer** (`naab/error_sanitizer.h`): Prevents information leakage through error messages by sanitizing file paths (absolute to relative), redacting sensitive values (passwords, API keys), sanitizing memory addresses, and simplifying C++ type names. Operates in three modes: development, production, and strict.

**Resource Limits** (`naab/limits.h`): Prevents resource exhaustion attacks:

```
Input Limits:
  MAX_FILE_SIZE           = 10MB
  MAX_POLYGLOT_BLOCK_SIZE = 1MB
  MAX_LINE_LENGTH         = 10,000 chars

Parse Tree Limits:
  MAX_PARSE_DEPTH = 1,000
  MAX_AST_NODES   = 1,000,000

Runtime Limits:
  MAX_CALL_STACK_DEPTH = 10,000
  MAX_STRING_LENGTH    = 10MB
  MAX_ARRAY_SIZE       = 10,000,000
  MAX_DICT_SIZE        = 10,000,000
```

### 13.2.3 Data Flow Security

All data flows through validation at each stage:

**User Input to Execution:**
```
User Input → Input size limit → Lexer → Token validation →
Parser → Depth limit → AST → Type checking →
Interpreter → Overflow/bounds checks → Result → Error sanitization → Output
```

**File Operations:**
```
User Path → Dangerous pattern detection → Canonicalization →
Traversal check → Directory whitelist → File size limit → File I/O
```

**FFI Boundary:**
```
NAAb Value → Type validation → Size validation → Serialization →
Polyglot Executor → Result → Return value validation → NAAb Value
```

## 13.3 Threat Model

NAAb uses the **STRIDE** methodology to identify and mitigate threats.

### 13.3.1 Trust Boundaries

The system has four trust boundaries:

1. **User Input → Runtime**: Untrusted NAAb source enters the runtime via `naab-lang run`, REPL, or eval. Protected by input size limits, parser depth limits, and sanitizer detection.

2. **Runtime → File System**: NAAb code accesses host files via stdlib I/O operations. Protected by path canonicalization, traversal prevention, and base directory whitelisting.

3. **Runtime → FFI/Polyglot**: Data crosses language boundaries to Python, JS, C++, etc. Protected by FFI type validation, size limits, depth limits, and null byte detection.

4. **Build System → Releases**: Source becomes signed release artifacts. Protected by dependency pinning, SBOM generation, artifact signing (cosign), and secret scanning.

### 13.3.2 Threat Categories

**Spoofing**: Mitigated by artifact signing and dependency pinning. Users verify release signatures.

**Tampering**: Mitigated by RAII/smart pointers, ASan/UBSan sanitizers in CI, path security for file operations, and safe arithmetic for overflow prevention.

**Repudiation**: Partially mitigated by basic logging. Tamper-evident logging is a post-1.0 enhancement.

**Information Disclosure**: Mitigated by error message sanitization (path, address, value redaction across three modes), and path traversal prevention.

**Denial of Service**: Mitigated by input size caps, parser depth limits, recursion limits, file size limits, and FFI payload limits. CPU exhaustion remains user responsibility (use OS-level controls).

**Elevation of Privilege**: Mitigated by sanitizers, continuous fuzzing, path security, and FFI validation. Note: FFI is powerful by design — polyglot blocks can perform any operation their host language supports.

### 13.3.3 Residual Risks

These risks are accepted and documented:

1. **CPU Resource Exhaustion** — User code can consume 100% CPU. Mitigation: OS-level limits (cgroups, ulimit).
2. **FFI Privilege Escalation** — Polyglot blocks can perform privileged operations. Mitigation: FFI validation and user education. This is by design.
3. **Tamper-Evident Logging** — Local attackers can modify logs. Planned for post-1.0.

## 13.4 Best Practices for Secure Polyglot Development

### 13.4.1 Writing Secure Blocks

1. **Minimize Scope**: Keep polyglot blocks small and focused on a single task.
2. **Avoid Global State**: Do not rely on global state in foreign languages that might persist unpredictably between block executions.
3. **Sanitize Inputs**: When passing strings to `<<bash` blocks, be wary of shell injection attacks. Use NAAb's variable binding syntax, which passes variables via environment variables to the shell process:

```naab
// BAD: Vulnerable to injection
// <<bash echo "Hello " + user_input >>

// GOOD: Safer variable passing
// <<bash[user_input] echo "Hello $user_input" >>
```

4. **Validate Return Values**: Always validate data coming back from a foreign block before using it in critical logic.

### 13.4.2 Deployment Security

For production deployments, consider these measures:

**Container Isolation:**
```bash
docker run --rm \
  -v $PWD:/workspace:ro \
  --security-opt=no-new-privileges \
  --cap-drop=ALL \
  naab-lang:latest run /workspace/script.naab
```

**Resource Limits:**
```bash
ulimit -t 300      # 5 minutes CPU time
ulimit -v 1000000  # 1GB virtual memory
```

**File System Sandboxing:**
```bash
naab-lang run --allowed-dirs=$PWD,/tmp script.naab
```

### 13.4.3 Security Testing

NAAb's CI pipeline includes:

- **Sanitizers**: ASan and UBSan run on every build (see `.github/workflows/sanitizers.yml`)
- **Fuzzing**: Continuous fuzzing for parser, interpreter, and FFI boundaries
- **Supply Chain**: Secret scanning (gitleaks), SBOM generation, artifact signing (cosign), vulnerability scanning (Grype)
- **Security Tests**: Comprehensive security test suite in `tests/security/`

## 13.5 Safety Checklist Summary

NAAb's implementation addresses these safety categories:

| Category | Key Measures | Status |
|----------|-------------|--------|
| Memory Safety | Bounds checking, smart pointers, RAII, sanitizers | Implemented |
| Type Safety | Static types, FFI validation, no unsafe casts | Implemented |
| Input Handling | Size caps, canonicalization, regex timeouts, fuzz testing | Implemented |
| Error Handling | Sanitized errors, no leaked secrets, mandatory cleanup | Implemented |
| FFI/ABI Safety | Type validation, size limits, boundary fuzzing | Implemented |
| Supply Chain | Dependency pinning, SBOM, signing, hermetic builds | Implemented |
| Arithmetic Safety | Overflow detection, division-by-zero guards, bounds checking | Implemented |
| Concurrency | Thread-safe temp files for compiled executors | Partial |
| Cryptographic Safety | Deferred to host language implementations | N/A |

For the complete safety checklist with 192 individual items across 14 categories, see the project's security documentation.
