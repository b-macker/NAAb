# Round 17 Architectural Audit Plan: Unbounded Parsing, Compilation TOCTOU, and Compile-Time Sandbox Escapes

## 1. Objective
Perform an exhaustive architectural review of the NAAb Language project, focusing on memory safety in external dependency parsing, temporary file management during polyglot execution, and the boundaries of the execution sandbox. This round aims to identify remaining Denial of Service vectors in newly integrated components (LSP, Project Context) and highlight critical blind spots in compiled polyglot execution.

## 2. Scope
- **LSP Server JSON Parsing:** `tools/naab-lsp/lsp_server.cpp` (RPC message parsing).
- **Project Context Loader Parsing:** `src/runtime/project_context.cpp` (`parseJsonConfig` for `.eslintrc`, `package.json`, etc.).
- **Polyglot Compiler Temp Files:** `src/runtime/cpp_executor_adapter.cpp`, `rust_executor.cpp`, `go_executor.cpp`, and other compiled executors (predictable `/tmp` paths and symlink resolution).
- **Compile-Time Code Execution:** The interaction between the NAAb sandbox and underlying polyglot compilers (`g++`, `rustc`, etc.).

## 3. Detailed Audit Phases

### Phase 1: Unbounded JSON Parsing in LSP Server (Critical)
- **Investigation:** Analyze `json::parse` in `tools/naab-lsp/lsp_server.cpp`.
- **Vulnerability:** The LSP server parses incoming JSON-RPC messages without depth limits.
- **Risk:** An attacker can send a maliciously crafted, deeply nested JSON payload (e.g., `[[[[...]]]]`) to trigger a stack overflow in `nlohmann::json`, crashing the language server (DoS).

### Phase 2: Unbounded JSON Parsing in Project Context Loader (High)
- **Investigation:** Analyze `ProjectContextLoader::parseJsonConfig` in `src/runtime/project_context.cpp`.
- **Vulnerability:** The context loader parses project files like `package.json` or `.eslintrc.json` using `nlohmann::json::parse` without depth limits.
- **Risk:** Scanning an untrusted directory with `naab-gov` will crash the scanner if the directory contains a maliciously nested JSON configuration file.

### Phase 3: Predictable Temp File TOCTOU Privilege Escalation (Critical)
- **Investigation:** Analyze temporary file generation across all compiled executors.
- **Vulnerability:** NAAb generates predictable temporary file names in a globally writable directory (`/tmp`) without using atomic, exclusive creation flags (like `O_EXCL`) during compilation.
- **Risk:** An attacker can pre-create symbolic links pointing to sensitive files (like `/etc/passwd`). When NAAb runs the compiler (`g++`, `rustc`, etc.), the compiler will overwrite the symlink target with the compiled binary, yielding arbitrary file overwrite.

### Phase 4: Compiler-Driven Sandbox Escape (High)
- **Investigation:** Analyze the interaction between the NAAb sandbox and underlying polyglot compilers.
- **Vulnerability:** NAAb sandboxes polyglot execution at *runtime*, but applies zero sandboxing during the *compilation* phase.
- **Risk:** An attacker can submit code containing compile-time file reads or code execution directives (e.g., C++ `#include "/etc/shadow"` to leak file contents via compiler error messages, or `#pragma GCC plugin` to execute arbitrary code during compilation). This completely circumvents the NAAb sandbox.

## 4. Deliverables
- Comprehensive Audit Report (Round 17).
- Reproduction strategies for Temp File TOCTOU and Compile-Time execution.
- Remediation roadmap for securing the compilation phase and enforcing JSON depth limits across all components.
