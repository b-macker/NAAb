# Security Model & Threat Assumptions

## What Governance Is

NAAb's governance engine is a **policy engine** — it catches honest mistakes, AI hallucinations, and coding anti-patterns before they execute. It is designed for a threat model where the adversary is:

- An AI model generating wrong idioms across languages (86+ cross-language patterns)
- A developer copy-pasting code with stubs, incomplete logic, or debug artifacts
- Accidental data flow from untrusted sources (env vars, polyglot output) to sensitive sinks (shell, env)

## What Governance Catches

| Category | Examples |
|----------|----------|
| **AI hallucinations** | `.push()` in Python, `print()` in JS, `json.stringify()` instead of `json.dumps()` |
| **Stubs & oversimplification** | `def validate(): return True`, `pass`-only bodies, identity functions |
| **Incomplete logic** | `except: pass`, bare raises, `"something went wrong"` messages |
| **Code quality** | TODO/FIXME, debug artifacts, hardcoded secrets (entropy-based), mock data |
| **Taint tracking** | `env.get()` reaching `shell()` without sanitization |
| **PII exposure** | SSN patterns, credit card numbers, API keys in string literals |

## What Governance Does NOT Protect Against

- **Malicious actors intentionally bypassing checks.** Governance rules are static analysis on polyglot blocks. A determined human can restructure code to avoid pattern detection.
- **Supply chain attacks.** Governance does not verify the integrity of imported modules, polyglot executors, or system packages.
- **Runtime exploits.** Buffer overflows, use-after-free, or memory corruption in polyglot executor subprocesses are outside governance's scope.
- **Memory safety in executors.** Each polyglot executor runs as a subprocess. NAAb does not sandbox the OS-level behavior of Python, Go, Rust, etc.
- **Privilege escalation.** `govern.json` controls what NAAb *attempts* to run, not what the OS *allows*. If the process has root, polyglot code has root.

## Taint Tracking Caveat

Taint tracking is **name-based**, not formal information flow analysis. It tracks named variables from sources (`env.get`, `polyglot_output`) through expressions to sinks (`shell`, `env.set_var`). Limitations:

- A determined actor can launder data through intermediate variables or restructured expressions
- Taint uses **prefix matching** for sanitizers — `"validate_"` matches `validate_input` but not `revalidate_input`
- Multi-level async taint propagates one level (parent to child snapshot), not through grandchild chains
- Module imports skip body scanning for performance; runtime enforcement still applies

## Polyglot Executor Model

Each polyglot executor (Python, JavaScript, Go, Rust, etc.) runs as an **OS subprocess**. There is no capability restriction beyond:

1. What `govern.json` declares (language allowlists, banned functions, timeouts)
2. What the operating system enforces (user permissions, seccomp, containers)

`govern.json` is a policy layer, not a sandbox. If you need OS-level isolation, use containers, seccomp profiles, or similar mechanisms around the NAAb process.

## The `--governance-override` Flag

The `--governance-override` CLI flag bypasses **soft** rules only. **Hard** rules cannot be overridden by any flag. This is by design — soft rules exist for cases where a human developer has reviewed the code and accepts the risk.

## Three Policy Levels

| Level | Behavior | Override |
|-------|----------|----------|
| **HARD** | Block execution | Cannot be overridden |
| **SOFT** | Block execution | `--governance-override` bypasses |
| **ADVISORY** | Warn and continue | Always runs, logged in audit trail |

## Reporting Vulnerabilities

If you discover a security vulnerability in NAAb (the language runtime, not a governance policy gap):

1. **Do not open a public issue**
2. Use [GitHub Security Advisories](https://github.com/b-macker/NAAb/security/advisories) to report privately
3. Or email the maintainer directly (see GitHub profile)

We follow responsible disclosure and will acknowledge reports promptly.
