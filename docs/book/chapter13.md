# Chapter 13: Security and Isolation

Executing code from multiple languages within a single application introduces unique security challenges. NAAb addresses these through a multi-layered security model designed to isolate foreign code execution and protect the host system.

## 13.1 The Security Model

NAAb's security architecture is built on the principle of **least privilege**. By default, the NAAb runtime operates with the permissions of the user running the process, but it employs strategies to contain the impact of the foreign code blocks it orchestrates.

### 13.1.1 Executor Isolation

Each language executor runs in a specific context:

*   **Embedded Executors (JavaScript, Python)**: These run within the same process as NAAb but are logically isolated.
    *   **JavaScript (QuickJS)**: Runs in a separate `JSRuntime` and `JSContext`. It does not have direct access to the host's file system or network unless explicitly provided via NAAb's bridge (though standard JS capabilities depend on the build configuration).
    *   **Python**: Runs in a sub-interpreter (where supported) or a controlled global namespace. Variables are injected explicitly, preventing accidental access to internal NAAb state.

*   **Subprocess Executors (Bash, Ruby, Go, Rust)**: These run as separate OS processes. This provides a strong boundary; a crash or memory leak in a Ruby block will not crash the main NAAb application. Data is marshaled via standard input/output pipes.

*   **Compiled Executors (C++)**: These are compiled into shared objects (`.so`/`.dll`) and loaded dynamically. While performant, they share the same memory space as NAAb. **Warning:** Malformed C++ code (e.g., buffer overflows) *can* crash the entire application. Use C++ blocks with caution and rigorous testing.

## 13.2 Permissions and Safety

While NAAb orchestrates execution, it does not currently impose a rigid capability-based permission system (e.g., "deny network access to Python blocks") in the core open-source build. Security relies on:

1.  **Code Review**: Treat `<<lang` blocks with the same scrutiny as native dependencies.
2.  **Environment Variables**: Sensitive data (API keys, passwords) should be passed via environment variables or secure vault integrations, not hardcoded in blocks.
3.  **Input Validation**: Always validate data coming *back* from a foreign block before using it in critical logic.

## 13.3 Best Practices for Secure Polyglot Development

1.  **Minimize Scope**: Keep polyglot blocks small and focused on a single task.
2.  **Avoid Global State**: Do not rely on global state in foreign languages that might persist unpredictably between block executions.
3.  **Sanitize Inputs**: When passing strings to `<<bash` blocks, be wary of shell injection attacks. Prefer using NAAb's variables which are passed via environment variables to the shell process, rather than string interpolation.

```naab
// BAD: Vulnerable to injection
// <<bash echo "Hello " + user_input >>

// GOOD: Safer variable passing
// <<bash[user_input] echo "Hello $user_input" >>
```
