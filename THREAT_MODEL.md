# NAAb Language Threat Model

## 1. Trust Model & Core Philosophy

NAAb is both (a) a general-purpose polyglot programming language and (b) a governance enforcement layer for code produced by humans, LLMs, or a mix of the two. The README states the problem directly: *"Govern AI-generated code at the language level. Prompts are suggestions. `govern.json` is policy."*

The trust model has **two distinct actors** that the earlier version of this document conflated as "developer":

- **Policy Author (Trusted).** The human who writes `govern.json`, runs `naab-lang`, and controls the execution environment. They own the gate rules and the machine.
- **Code Writer (Semi-Trusted).** The party who authors the NAAb code — could be a human developer, an LLM, or an AI agent. Their code is the *message* that must clear the gate before execution.

The governance system exists precisely because these two actors are **not identical**. If the code writer were fully trusted, governance would serve no purpose. The entire reason for `govern.json`, the pre-flight scanner, runtime taint tracking, and post-execution quality gates is to enforce the policy author's rules on code that may have originated from a less-trustworthy source — most commonly an LLM.

**Polyglot blocks (`<<python`, `<<rust`, `<<cpp`) execute with full OS privileges once they start running.** NAAb does not attempt to sandbox foreign runtimes. However, the *contents* of these blocks are scanned pre-flight, their bound variables are taint-tracked at runtime, and their compilation invocations are constrained (no `#pragma GCC plugin`, no absolute-path `#include`, etc.).

Governance is **policy enforcement, not a hostile sandbox.** It catches mistakes — accidental secret leakage, hallucinated APIs, banned imports, unsafe patterns, dangerous data flows — before they execute. It does not try to defend against a determined attacker who controls `govern.json` and the source code simultaneously.

---

## 2. The Gate: How Enforcement Works

`govern.json` is the single source of truth. The policy author writes it; three enforcement layers consume it:

| Layer | When it runs | Mechanism |
|-------|--------------|-----------|
| **Pre-flight** | Before any bytecode executes | `naab-gov` standalone CLI; `discoverAndLoad()` during startup; AST scanning; dangerous pattern regex; `naab.lock` signature verification |
| **Runtime** | During execution | `OP_GOV_TAINT_MARK`, `OP_GOV_TAINT_CHECK_ASSIGN`, `OP_GOV_CHECK_POLYGLOT_VARS` bytecode opcodes; shadow taint stack; `checkTaintedSink`; resource limits |
| **Post-execution** | After successful execution | `evaluateQualityGate()` — aggregate threshold checks |

**Enforcement levels:**
- **HARD** — blocks execution, exit code 3, no override.
- **SOFT** — blocks execution, exit code 3, `--governance-override` bypass allowed.
- **ADVISORY** — warns and continues, exit code 0.

The three layers are **complementary, not competing**. Pre-flight catches obvious static violations (banned imports, hardcoded secrets, known-dangerous patterns). Runtime catches dynamic flows that static analysis cannot predict — this is the safety net for clever LLM code that clears the pre-flight scanner. Post-execution catches aggregate metrics that only resolve after a run (complexity totals, coverage thresholds).

---

## 3. Threat Actors

### Trusted

- **Policy Author** — human who writes `govern.json`, runs `naab-lang`, owns the machine.
- **CI/CD Pipeline** — runs `naab-lang --lock-check` and gated builds in a controlled environment under the policy author's control.

### Semi-Trusted (Governed)

- **Human Code Writer** — may make honest mistakes; governance catches them before they reach production or a sensitive sink.
- **LLM / AI Agent** — generates NAAb code, often including polyglot blocks. The **primary motivation** for the entire governance system. Subject to per-agent role restrictions via `--agent-id`. Treated identically to human code writers at the gate — the message is inspected, not the author. An LLM producing `<<python os.system(user_input)>>` is exactly the kind of mistake the gate exists to catch.

### Untrusted

- **Remote Attackers** — target exposed services (the REST API `/api/v1/execute` endpoint, when enabled).
- **Malicious Workspaces** — untrusted repositories that attempt zero-click exploitation of the LSP server (`naab-lsp`) or the scanner (`naab-gov`) when opened or scanned.
- **Supply Chain Attackers** — poisoned `naab.lock`, malicious third-party `.naab` modules, stdlib shadowing via locally-placed `math.naab`, tampered `govern.json`.

---

## 4. In-Scope Attack Surfaces

### 4.1 Supply Chain & Environment

- Lockfile tampering (`naab.lock` poisoning).
- Standard library shadowing (a malicious `math.naab` in the working directory overriding the legitimate stdlib module).
- Environment variable injection that alters runtime behavior (`LD_PRELOAD`, `PYTHONPATH`, `NODE_OPTIONS`, etc.).
- Tampered `govern.json` in an untrusted workspace cloned by the policy author.

### 4.2 Tooling & Workspaces (Zero-Click Execution)

- LSP Server vulnerabilities: RCE on file open, DoS via massive workspaces, crafted `.naab` files that exploit the parser or governance pre-flight during indexing.
- Scanner (`naab-gov`) vulnerabilities: ReDoS, stack overflows from malformed `govern.json`, adversarial directory traversal during a scan of an untrusted repository.

### 4.3 Exposed Services

- The REST API (`/api/v1/execute`) must authenticate every request (done, R11).
- The REST API must rate-limit request volume — authentication alone does not prevent DoS by an authenticated client flooding the endpoint.
- Per-request resource limits (timeout, memory, body size) must be enforced independently of server-wide defaults.

### 4.4 The Gate Itself

The governance engine, pre-flight scanner, and runtime taint system must not be silently bypassable by *accidentally malformed* code from a semi-trusted writer. This is the core value proposition — if the gate leaks, the policy author has no meaningful protection against LLM code drift. Specific concerns:

- **Polyglot block content scanning** — regex pattern bypasses via quoting tricks, line continuation, alternate string prefixes, or comment styles that the scanner doesn't normalize.
- **AST taint tracking** — dynamic flows an LLM might produce (nested container mutations, closure captures, async callbacks) that static pre-flight misses. This is exactly what the R5–R20 runtime taint work protects against, and it is **in scope under this threat model**.
- **Per-agent role enforcement** — `--agent-id` must actually restrict what a given agent can do, not just label its output.

### 4.5 Compiler Invocation

Polyglot blocks in C++, Rust, and Go are compiled before execution. The compiler invocation itself — not just the resulting binary — is a trust boundary. `#pragma GCC plugin`, absolute-path `#include "/etc/shadow"`, Rust `include_str!("/...")`, and similar constructs must be blocked at the pre-flight layer (done R16, kept in scope as a standing requirement).

---

## 5. Out-of-Scope (Not Vulnerabilities)

- **Intentional destructive code from the policy author.** A user writing `<<python import os; os.system('rm -rf ~') >>` and running it locally is not a vulnerability — it is equivalent to writing `rm -rf` in a shell script. The policy author controls the machine.
- **Polyglot block escape at execution time.** Once a `<<python>>`, `<<rust>>`, or `<<cpp>>` block begins executing, NAAb does not sandbox the foreign runtime. Python can call `os.system`, Rust can call `std::fs`, C++ can link arbitrary libraries. The gate inspects block *contents* before execution; after execution begins, the foreign runtime runs with full OS privileges.
- **Infinite loops in user NAAb code.** Resource limits may catch these, but they are a policy concern, not a security vulnerability.
- **A sufficiently clever LLM that produces semantically malicious code that passes all `govern.json` rules.** The gate enforces *stated* policy — it cannot catch undefined threats. The policy author is responsible for writing rules that capture their actual requirements.
- **Bypassing governance by editing `govern.json`.** The policy author owns the file. Changing it is policy revision, not an attack.

### Critical Clarification on Runtime Taint Findings

Runtime taint bypasses **are in scope** when the threat is "LLM-generated code inadvertently leaks a secret because the runtime taint system failed to mark a container as tainted" (e.g., V-GOV-013's `arr[0][1] = secret` case). This is the gate failing to catch code drift — the primary purpose of the runtime layer.

Runtime taint bypasses **are out of scope** when the threat is "a determined attacker who controls the NAAb source manually constructs a tainted value without going through a tagged source and reaches a sink." In that scenario the attacker is the policy author — they can just edit `govern.json`.

The distinction is the *author's intent*. The gate's job is to catch mistakes, not to defeat adversarial humans who own the environment.

---

## 6. Open Gaps

Items identified during the R5–R20 audit cycle and this threat model revision that remain unaddressed:

1. **Stdlib shadowing** (§4.1) — `import math` resolves local filesystem paths before the trusted stdlib. A `math.naab` in the working directory can shadow the built-in `math` module. Not yet fixed.
2. **REST API rate limiting** (§4.3) — authentication exists (R11); request-rate throttling does not. An authenticated client can still DoS the `/execute` endpoint by flooding requests.
3. **`naab-gov` adversarial directory scanning** (§4.2) — partially addressed (R12 ReDoS, R16 JSON depth in `project_context.cpp`), but the full directory-traversal attack surface when scanning an untrusted repository has not been audited.
4. **Per-agent role enforcement audit** (§4.4) — `--agent-id` changes behavior and logs telemetry, but has not been audited against the "agent bypasses its own declared role" threat. If roles are not enforced at the gate layer, the flag is decorative.
5. **Test runner coverage gap** — `run-all-tests.sh` `TEST_DIRS` list omits `tests/vm/` and `tests/cli/`; shell-based security test scripts (`tests/security/test_r*_fixes.sh`) are not picked up by the runner. Not a security vulnerability, but it distorts the security test count and hides regressions in recently-added test directories.

---

## 7. Historical Context

This document supersedes a 34-line version written after the R20 audit round. The earlier version correctly identified the in-scope attack surfaces (supply chain, tooling, REST API) but contained three framing errors that generated downstream confusion:

1. It described "the developer" as a single trusted actor, conflating the policy author with the code writer. The README's "AI Code Drift" problem statement makes clear these are distinct.
2. It implied governance was purely pre-execution ("policy, not sandboxing") without acknowledging that the runtime taint layer is a real, HARD-level enforcement mechanism and an intentional safety net for dynamic flows that static analysis cannot predict.
3. It did not name LLMs or AI agents as threat actors despite the system being explicitly designed around them.

The practical consequence of these errors was a retrospective conclusion that the R5–R20 runtime taint fixes were "whac-a-mole theater." Under the revised model, that work is correctly placed: it hardens the gate's safety net against exactly the kind of dynamic flow bugs an LLM produces. The R10 false-positive cluster (4 of 5 findings "already fixed") remains correctly refuted — those were audit-quality issues, not threat-model issues.
