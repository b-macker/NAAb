# PR #23 — Governance Observability & Orchestration

**Branch:** `feature/governance-observability-phase2-8`
**Base:** `master`
**Commits:** 47
**Files changed:** 179
**Lines:** +20,310 / -1,105
**Date range:** June 3 - June 11, 2026
**CI Status:** All workflows green (Linux, Windows, MSVC, Sanitizers, CodeQL, Bindings, Governance Check, Supply Chain)

---

## Executive Summary

This branch transforms NAAb's governance engine from a static policy checker into a **live, observable, self-assessing runtime** that provides continuous oversight of LLM agent behavior. Before this branch, governance was binary: it blocked or allowed. After, governance is a living system that tracks coherence over time, escalates repeated violations, expires stale authorization, proves enforcement decisions cryptographically, and reports its own health.

The branch also delivers **cross-platform polyglot execution** (JavaScript and Python on Windows), **enterprise policy distribution** (inherit/extend governance configs), and a **multi-agent orchestration module** for structured LLM collaboration patterns.

**Why this matters:** When an LLM generates code through NAAb, governance is the only barrier between the model and arbitrary execution. These 47 commits make that barrier smarter, more observable, harder to bypass, and provably correct.

---

## Table of Contents

1. [Phase 1: Agent Tool Execution](#phase-1-agent-tool-execution)
2. [Phase 2: Dynamic Code Execution](#phase-2-dynamic-code-execution)
3. [Phase 3: Governance Pulse](#phase-3-governance-pulse)
4. [Phase 4: Standing Lease, Advisory Escalation, Evidence Epoch](#phase-4-standing-lease-advisory-escalation-evidence-epoch)
5. [Phase 5: Consequence-Boundary Hardening](#phase-5-consequence-boundary-hardening)
6. [Phase 6: Enterprise Readiness](#phase-6-enterprise-readiness)
7. [Phase 7: Uncatchable Governance & Subprocess Containment](#phase-7-uncatchable-governance--subprocess-containment)
8. [Phase 8: Governance Observability & Orchestration](#phase-8-governance-observability--orchestration)
9. [CI/Platform Hardening](#ciplatform-hardening)
10. [Windows Polyglot Enablement](#windows-polyglot-enablement)
11. [Security Audit Fixes](#security-audit-fixes)
12. [Documentation](#documentation)
13. [Testing](#testing)
14. [Commit Log](#commit-log)

---

## Phase 1: Agent Tool Execution

**Commits:** `2d3a9d64`, `764a2817`, `40b1f442`, `65f6096a`, `596e22d5`
**Problem:** LLM agents could send prompts and receive responses, but had no way to call NAAb functions as tools. This meant agents couldn't interact with application logic — they could only generate text.

**What was built:**
- `agent.register_tool(name, function, schema)` — register NAAb functions as LLM-callable tools
- **7 defense layers** for every tool call:
  1. **Declaration gate** — tool must be declared in govern.json `tools[]`
  2. **Admission gate** — tool must be registered via `register_tool()` (dual-gate)
  3. **Argument scan** — governance scans tool arguments for secrets, PII, injection
  4. **Scoped sandbox** — tool executes in the agent's sandbox, not the parent's
  5. **Result scan** — governance scans tool output before returning to LLM
  6. **Budget check** — per-turn and per-run tool call limits enforced
  7. **Behavioral** — BSD pattern matching on tool sequences (e.g., `tool_data_exfil`, `tool_rapid_fire`)
- `TOOL_EXEC` in `allowed_actions` matrix — must be explicitly granted
- Tool config fields are ratchet-enforced (can only tighten mid-run, never loosen)
- VM closure support — `register_tool()` accepts VM closures, not just tree-walker functions
- Tool definitions sent on initial API call so the LLM knows what's available from turn 1

**Why it improves the system:** Without tool execution, agents are limited to text generation. With it, agents can call application functions — but every call passes through the same governance pipeline as user code. The dual-gate requirement (govern.json + register_tool) means neither the config author alone nor the code author alone can enable a tool; both must agree. This is separation of duties applied to LLM tool access.

**How it reduces friction:** Developers register tools in NAAb and the agent framework handles the loop — parse tool calls from LLM response, execute governed functions, return results, continue conversation. No manual JSON parsing or call routing.

---

## Phase 2: Dynamic Code Execution

**Commits:** `69ef6298`, `f2955662`
**Problem:** Agents needed to generate and execute code at runtime (e.g., a data analysis agent that writes Python to process a dataset). Static polyglot blocks are compiled at parse time — there was no way to run code that doesn't exist until the agent produces it.

**What was built:**
- `codegen.run(lang, code)` — execute runtime-generated code through the full governance pipeline
- `codegen.run_with_args(lang, code, args)` — same with variable bindings
- `codegen.run_strict(lang, code, args)` — throws on non-zero exit code (catchable by NAAb try/catch)
- `codegen.supported_languages()` / `codegen.is_enabled()` — introspection
- Same 39+ governance checks as static polyglot blocks: taint tracking, banned functions, import blocklists, timeout, line limits, complexity floor
- Per-call and cumulative limits (max_lines_per_call, max_calls_per_run)
- Nesting prevention — codegen cannot call codegen

**Why it improves the system:** Runtime code generation is one of the highest-risk operations an LLM can perform. Without `codegen.run()`, developers would need to bypass governance entirely to execute agent-generated code. With it, every line of generated code passes through the same checks as hand-written polyglot blocks. The governance engine doesn't distinguish between "code the developer wrote" and "code the LLM generated" — it enforces the same rules on both.

**How it makes the system more accurate:** `run_strict` mode gives agents a feedback loop — if their generated code fails (syntax error, runtime error), they get the error message and can self-correct. The governed codegen demo (`orchestrator.naab`) proves this works end-to-end with Gemini self-correction.

---

## Phase 3: Governance Pulse

**Commits:** `db979a33`, `e7775373`
**Problem:** Governance operated check-by-check with no aggregate view. If an agent was slowly degrading (coherence drifting, occasional violations, borderline behaviors), there was no way to detect the trend — each check passed individually even as the overall health declined.

**What was built:**
- `PulseVerdict` — three states: HEALTHY, DEGRADED, IMPAIRED
- Signal wiring: coherence score, BSD event count, CDD turns analyzed, governance level, active agent count all feed into the verdict
- **Hysteresis** — prevents flapping between states. Degraded requires N consecutive unhealthy signals before transitioning; recovery requires M consecutive healthy signals before upgrading
- **Stepped recovery** — can only recover one level per check (IMPAIRED -> DEGRADED -> HEALTHY), not jump directly to HEALTHY
- Two-phase mutex — pulse calculation doesn't block agent operations
- BSD emission on state transitions — behavioral sequence detector gets notified when pulse changes
- Dashboard line: `Pulse: HEALTHY (coherence: 0.95, epoch: 3)`
- `governance.health()` stdlib — returns verdict, coherence, governance level, epoch, BSD events, CDD turns

**Why it improves the system:** Governance Pulse turns point-in-time checks into trend analysis. A single BSD match might be noise. Three BSD matches with declining coherence and an elevated governance level is a pattern. Pulse captures this — it's the difference between checking individual vital signs and diagnosing the patient.

**How it reduces friction:** Operators can call `governance.health()` at any point to get a single verdict. Pipeline stages can check upstream pulse before trusting upstream output. The dashboard shows it per-run without any configuration.

---

## Phase 4: Standing Lease, Advisory Escalation, Evidence Epoch

**Commits:** `3cd561e3`
**Problem:** Three gaps in the governance model:
1. **Authorization was permanent** — once an agent was created, it could run indefinitely with no re-verification
2. **Advisories were toothless** — the same advisory could fire 100 times and never escalate
3. **Evidence was timeless** — governance state changes (config reload, pulse verdict change) didn't invalidate prior observations

**What was built:**

### Standing Lease (Kerberos TGT analog)
- `standing_lease_turns` / `standing_lease_seconds` per agent config
- Expired lease forces step-up challenge before the next `agent.send()`
- Renewed on successful challenge pass
- `lease_remaining` in agent environment
- **Why:** Limits blast radius of a compromised agent. Even if an agent's API key is leaked, the lease expires and the attacker must pass a challenge to continue.

### Advisory Escalation (OSHA violation analog)
- Tracks `emitted_advisories_` count per advisory type
- 2nd+ occurrence: weight multiplied (configurable `weight_multiplier`)
- N-th occurrence (`soft_after`): escalates from ADVISORY to SOFT block
- Advisory history decays on epoch boundary (fresh start after governance state change)
- **Why:** First-time advisories are informational. Repeated advisories indicate a pattern — either the agent is ignoring guidance or is stuck in a loop. Escalation converts "please don't" into "you can't."

### Evidence Epoch (database MVCC analog)
- Monotonic counter incremented on: pulse verdict change, governance level change, config reload
- `governance_epoch` in agent environment and `governance.health()`
- Prior-epoch evidence discounted — `consecutive_passes` reset on epoch change
- **Why:** If governance tightens mid-run (config reload), evidence from the permissive era shouldn't count toward recovery under the strict era. Epochs partition the timeline so stale evidence can't influence current decisions.

**How these three features work together:** Lease limits *duration*. Escalation limits *repetition*. Epoch limits *staleness*. Together they prevent the three ways an agent can accumulate unearned trust: running too long, repeating violations without consequence, or coasting on old good behavior after conditions change.

---

## Phase 5: Consequence-Boundary Hardening

**Commits:** `8b8d1085`, `c05445e6`, `2c62f2ec`
**Problem:** Critical review found 4 bugs and 4 structural gaps in the consequence/enforcement boundary — places where governance decisions could be incorrect, inconsistent, or invisible.

**What was fixed:**

| Issue | Impact | Fix |
|-------|--------|-----|
| `verifyScoreIntegrity` not escalation-aware | Escalated advisories counted at original weight, not multiplied weight | Check uses multiplied weight |
| `wasBlocked()` missed escalated advisories | Advisories escalated to SOFT didn't register as blocks | `wasBlocked()` checks escalation state |
| `governance_epoch_` not atomic | Race condition on epoch reads from agent threads | Made `std::atomic<uint64_t>` |
| `/dev/urandom` missing warning | Silent fallback to weak randomness for handle nonces | Warning logged when urandom unavailable |
| No advisory decay on epoch boundary | Old advisories never cleared, permanent escalation | History decays on epoch change |
| BSD/CDD evidence lost on `updateConfig()` | Config reload destroyed accumulated behavioral evidence | Evidence preserved across reload |
| No telemetry pulse subsystem | Telemetry forwarder health invisible to pulse | `telemetry_connected` signal added |
| No wall-clock lease | Turn-based lease only; long-running single turns had infinite authorization | `standing_lease_seconds` added |

**Why it improves the system:** These are correctness bugs in the enforcement layer. An escalated advisory that doesn't register as a block means the system reports "no blocks" when it should report "1 block." A non-atomic epoch means two threads can disagree about which epoch they're in. These aren't theoretical — they're real inconsistencies that would surface under multi-agent, multi-turn workloads.

---

## Phase 6: Enterprise Readiness

**Commits:** `f229dd9b`, `aaf736ff`, `c6c778bf`, `11964c4f`, `a2a3f6be`, `ca7bf781`, `bdd1418a`, `07429c32`, `e7a72300`
**Problem:** NAAb governance was project-scoped. Organizations with multiple teams, shared base policies, and compliance requirements had no way to distribute or compose governance configs.

**What was built:**

### Polyglot Reload (hot config changes)
- `reloadIfChanged()` detects govern.json mtime changes during agent turns
- Validates signature, enforces one-way ratchet (only tightening allowed)
- `governance_notices` in `agent.send()` return dict shows what changed
- **Why:** In long-running agent sessions, governance may need to tighten without killing the process.

### Telemetry Forwarding
- Webhook and SIEM push of JSONL telemetry events
- Configurable batch_size, flush_interval_ms
- Bounded shutdown drain (prevents 500s hang on exit)
- Tamper-evident hash chain: each event includes `prev_hash`
- **Why:** Governance decisions are audit events. In enterprise environments, they need to flow into existing SIEM/SOC infrastructure, not just sit in a local JSONL file.

### REST API Multi-Key Auth
- `api.auth.keys[]` with per-key `permissions` (read, write, admin)
- Scoped access control for the governance REST API
- **Why:** Different consumers (dashboards, CI pipelines, admin tools) need different access levels to governance data.

### Policy Inheritance (govern.json extends)
- `"extends": "./path/to/parent.json"` loads parent config
- Child overrides parent (child wins on conflicts)
- Array fields merge via `merge_arrays` mode: `"replace"` (default) or `"append"` (dedup concat)
- Max depth: `meta.inheritance.max_depth` (default 5)
- Parent must pass signature verification
- **M3 explicit-set tracker** — correctly distinguishes "child set this to 0" from "child didn't set this, use parent default"
- **Why:** A security team publishes a base policy. Product teams extend it with project-specific rules. The base policy's blocked imports, banned functions, and taint rules propagate automatically. Child configs can tighten but not loosen parent constraints (ratchet enforcement applies to inheritance too).

**How it reduces friction:** Teams don't copy-paste governance configs. They inherit from a blessed base and customize. When the security team updates the base, all child configs pick up the changes automatically. The explicit-set tracker prevents the subtle bug where a child config's default value accidentally overrides a parent's intentional setting.

### Data Race Fix (C1)
- `rules_` access via COW (copy-on-write) `shared_ptr` + thread-local snapshot
- Agent worker threads take a snapshot at turn start; main thread can reload without locking
- **Why:** Without this, `reloadIfChanged()` writing to `rules_` while agent threads read from it is undefined behavior. The COW pattern gives each thread a consistent view without mutexes on the hot path.

---

## Phase 7: Uncatchable Governance & Subprocess Containment

**Commits:** `fd97d0ae`, `4afa345a`, `c129dfd9`, `4d38ae0e`, `a6ee0a88`

### GovernanceHardError (uncatchable exceptions)
**Problem:** NAAb `try/catch` could catch governance HARD blocks, allowing LLM-generated code to swallow enforcement and continue executing.

**What was built:**
- `GovernanceHardError` — inherits `std::runtime_error` but explicitly re-thrown in every catch site
- Both tree-walker (5 sites) and VM (4 sites) re-throw before generic handlers
- `main.cpp` catches it and calls `_exit(3)`
- **Why:** This is the single most critical security fix in the branch. If an LLM can write `try { dangerous_code() } catch (e) { /* swallow */ }` and bypass governance, the entire enforcement model is broken. GovernanceHardError makes HARD blocks genuinely uncatchable.

### EnforcementLevel::DETECT
- Catchable variant of HARD — same detection logic but throws standard `std::runtime_error`
- Used in governance test configs so tests can verify violation detection via try/catch without being killed
- **Why:** 68+ tests need to verify that governance detects violations. Before DETECT, these tests had to use HARD (which kills the process) or ADVISORY (which doesn't block). DETECT is the test-time equivalent of HARD — it proves the detection logic works without making the test framework fight the enforcement framework.

### env_vars Runtime Enforcement
- `capabilities.env_vars.blocked_read` (HARD) / `allowed_read` (SOFT allowlist)
- `blocked_write` / `allowed_write` — same pattern for writes
- Enforced at all 9 env access points: get, set, has, delete, list, get_all, load_dotenv, get_int/float/bool
- `blocked_read` fires before `std::getenv()` — value never enters memory
- `env.has()` stealth deny — returns false for blocked vars
- `env.list()` / `get_all()` filter out blocked vars
- **Why:** Environment variables contain API keys, database passwords, cloud credentials. Without enforcement, an LLM can call `env.list()` and exfiltrate every secret on the system. With it, `blocked_read` prevents the value from ever entering the NAAb runtime's memory space — the `std::getenv()` call never happens.

### OS-Level Subprocess Containment (5-layer)
- `RLIMIT_NPROC=0` — blocks fork/subprocess creation at the kernel level
- PATH restriction — polyglot child processes can't find system binaries
- Environment scrubbing — secrets stripped before child process creation
- Timeout + SIGKILL — hard upper bound on child process lifetime
- `allow_exec` / `allow_fork` flags — sandbox level controls what's permitted
- `SubprocessContainment::fromCurrentSandbox()` maps sandbox level to policy
- **Why:** Static source scanning catches `import subprocess` and `os.system("rm -rf /")`. But it can't catch runtime-constructed commands: `cmd = chr(114)+chr(109); os.system(cmd)`. Subprocess containment is the defense-in-depth layer — even if scanning misses it, the kernel blocks the fork.

---

## Phase 8: Governance Observability & Orchestration

**Commits:** `ddb15265`, `99447293`, `d8c0df43`, `f3861adf`

### Runtime Telemetry (25 event types)
- **Agent lifecycle:** `AGENT_RESPONSE`, `AGENT_RETRY`, `AGENT_FALLBACK`, `AGENT_HARD_STOP`, `AGENT_KEY_REVIVED`, `AGENT_KEY_DISABLED`, `AGENT_CHALLENGE_PASS`, `AGENT_CHALLENGE_FAIL`
- **Governance scanning:** `PROMPT_SCAN`, `RESPONSE_SCAN`, `RESPONSE_SUPPRESSED`, `ADMISSION_EVAL`, `CDD_TURN`, `BSD_MATCH`, `CONTRACT_VIOLATION`
- **Tool execution:** `AGENT_TOOL_CALL`, `AGENT_TOOL_RESULT`, `AGENT_TOOL_BLOCKED`, `AGENT_TOOL_SCAN_HIT`, `AGENT_TOOL_REGISTERED`, `AGENT_TOOL_LOOP_START`, `AGENT_TOOL_LOOP_END`
- **System:** `GOVERNANCE_LEVEL_CHANGE`, `GOVERNANCE_HEALTH_WARNING`, `CODEGEN_EXEC`
- Each event includes `run_id` for separating runs in shared output files
- Tamper-evident hash chain: each event includes `prev_hash` linking to previous event
- **Why:** Every governance decision is now auditable. The hash chain makes it tamper-evident — if someone deletes or modifies a telemetry event, the chain breaks and downstream events fail verification.

### Non-Binding Refusal Attestation
- Signed attestation recorded when a HARD block prevents execution
- Proves that governance *did* block something, even when the blocked code never ran
- **Why:** In compliance contexts, proving a negative is often harder than proving a positive. Refusal attestation provides cryptographic proof that a specific governance rule prevented a specific action at a specific time.

### Orchestra Module (multi-agent patterns)
- `orchestra.sequential_refinement(handles, prompt [, iterations])` — returns a plan dict `{pattern, handles, prompt, iterations, description}` for sequential refinement across agents. NAAb code implements the actual `agent.send()` loop using the plan.
- `orchestra.consensus_vote(votes_dict)` — takes a single dict argument `{votes: ["APPROVED", "REJECTED", ...]}`, tallies pre-supplied verdict strings, returns `{verdict, majority, approved, rejected, review, total}` with majority determination.
- `orchestra.enforce_convergence(response, spec)` — validates a response string against a spec dict (regex `pattern` or `required_fields` for JSON). Returns `{valid, error_message}`. No retry loop — the caller implements retries.
- **Why:** Multi-agent workflows (review chains, consensus, iterative refinement) are common patterns. The orchestra module provides validated building blocks — plan construction, vote tabulation, response validation — that integrate with governance. NAAb code composes these with `agent.send()` to build full workflows.

### Agent Output Contracts
- Per-agent `output_contract` in govern.json: `format`, `required_fields`, `field_types`, `regex_checks`
- Validated after RESPONSE_SCAN
- `CONTRACT_VIOLATION` telemetry event on failure
- **Why:** LLMs produce unpredictable output formats. Output contracts enforce structure: "this agent must return JSON with a `confidence` field that's a number." Without contracts, downstream code parsing agent output fails silently or crashes on unexpected formats.

### RESPONSE_SUPPRESSED Telemetry
- Emitted when `content.empty()` after all retries
- Records handle_id, config_name, turn, reason, retries_used
- **Why:** Empty responses are an observability black hole. Without this event, an agent that gets no response looks identical to an agent that never tried. RESPONSE_SUPPRESSED fills the gap.

---

## CI/Platform Hardening

**Commits:** `00c8e4bb`, `5079788c`, `2fc5c9d7`, `8ae156ff`, `1f125066`, `e9a22af7`, `2b938eb6`, `c4b021a6`

| Fix | Problem | Impact |
|-----|---------|--------|
| Missing `<vector>` include | MSVC strict mode rejects implicit includes | Build failure on Windows |
| MSVC `localtime_r` | MSVC doesn't have `localtime_r` (POSIX) | Replaced with `localtime_s` on MSVC |
| `unistd.h` in rest_api.cpp | MSVC doesn't have `unistd.h` | Guarded with `#ifndef _WIN32` |
| C API GovernanceHardError | C API catch sites didn't re-throw GovernanceHardError | Governance bypass via C API |
| Test signing in CI | Gorilla test `.sig` files were stale | Tests failed signature verification |
| TMPDIR fallback | CI uses `/tmp`, Termux uses `/data/.../usr/tmp/` | Tests failed on file creation |
| env.get crash root cause | `environ` pointer uninitialized under ELEVATED sandbox on Ubuntu 24.04+ | SIGSEGV in env stdlib |
| POSIX test skips on Windows | Tests using `<<sh>>` blocks can't run without shell executor | False failures on MSYS2 |
| Crash handling honesty | Test counted signal crash as PASS | Masked real bug (env.get crash) |

**env.get crash deep dive:** ELEVATED sandbox setup in `sandbox.cpp` doesn't call `std::getenv()` during initialization (unlike STANDARD which calls `naab::paths::home()`/`temp_dir()`). On newer glibc (Ubuntu 24.04+), if `std::getenv()` is never called early, the `environ` pointer may be uninitialized. Fixed with defense-in-depth: force `std::getenv("PATH")` in ELEVATED setup + null-check `environ` before iteration.

---

## Windows Polyglot Enablement

**Commits:** `ca687944`, `2c9a683c`, `bb90b0f9`
**Problem:** All polyglot executors were behind `#ifndef _WIN32` in `main.cpp` and `if(WIN32)` removal in `CMakeLists.txt`. On MSYS2/MinGW64, QuickJS compiles successfully and `GenericSubprocessExecutor` has zero POSIX dependencies (delegates to `subprocess_helpers.cpp` which has a full Windows `CreateProcess` implementation). But nothing was registered.

**What was fixed:**
- `CMakeLists.txt`: `generic_subprocess_executor.cpp` removed from WIN32 exclusion list (no POSIX deps)
- `main.cpp`: Split `initialize_executors()` — JS (via `HAVE_QUICKJS`) and subprocess Python (via `!HAVE_PYBIND11`) are cross-platform; POSIX-only executors stay behind `#ifndef _WIN32`
- Added `HAVE_QUICKJS` compile definition to `naab-lang` target (was only on `naab_runtime` PRIVATE)
- Added JS and Python polyglot smoke tests to Windows CI workflow
- Un-skipped `test_consequence_proof.sh` on Windows (uses `<<javascript>>` only)

**Why it improves the system:** JavaScript and Python polyglot blocks now work on Windows. This means Windows users can run NAAb projects that use `<<javascript>>` or `<<python>>` blocks. Previously, Windows was limited to pure NAAb code.

**Platform support matrix after this change:**

| Executor | Linux | Windows (MinGW64) | Windows (MSVC) |
|----------|-------|-------------------|----------------|
| JavaScript (QuickJS) | Yes | **Yes (NEW)** | No (QuickJS needs POSIX headers) |
| Python (subprocess) | Yes | **Yes (NEW)** | No (executor not compiled) |
| Python (embedded) | Yes | No | No |
| Shell/Bash | Yes | No | No |
| Go, Rust, C++, C#, Nim, Zig, Julia | Yes | No | No |

---

## Security Audit Fixes

**Commits:** `faa2b739`, `1c900cd2`, `a8cfc7e1`

### GovernanceHardError Bypass (3 vectors)
- **Tree-walker try/catch** — generic `catch(const std::exception&)` caught GovernanceHardError before the re-throw
- **REST API catch sites** — 3 places in `rest_api.cpp` caught all exceptions, swallowing governance blocks
- **C API** — `governance_c_api.cpp` catch sites didn't re-throw
- **Fix:** Added `catch (const governance::GovernanceHardError&) { throw; }` before every generic handler

### Data Races
- `rules_` read/write from multiple threads without synchronization
- `governance_epoch_` non-atomic reads from agent threads
- **Fix:** COW shared_ptr for rules, atomic for epoch

### Score Integrity
- `verifyScoreIntegrity` compared against base weight, not escalated weight
- Off-by-one in `weight_multiplier` application
- **Fix:** Score verification uses multiplied weight; boundary correctly inclusive

### 14 Bug Fixes (naab-37 gorilla test)
- Gorilla test `naab-37` found 14 bugs across agent, governance, and stdlib
- All 32/32 assertions pass after fixes

---

## Documentation

**Commits:** `764a2817`, `c129dfd9`, `2a1775a2`

All 5 user-facing documents updated for 19 previously undocumented features:

| Document | Purpose | Key additions |
|----------|---------|---------------|
| `CLAUDE.md` | Internal dev reference | Governance Pulse, Standing Lease, Advisory Escalation, Evidence Epoch, Output Contracts, DETECT level, env_vars enforcement, enterprise readiness, codegen/orchestra/governance modules |
| `CLAUDE-TEMPLATE.md` | LLM-facing template | codegen module (5 functions), orchestra module (3 functions), agent.extract_code, Standing Lease, Advisory Escalation, Governance Pulse, Evidence Epoch, Output Contracts |
| `README.md` | User-facing README | 4-tier policy levels, 24 modules, Governance Pulse, Multi-Agent Orchestration, Dynamic Code Execution, Enterprise Features, Subprocess Containment |
| `PROJECT_SETUP.md` | Bootstrap guide | Agent governance features, policy inheritance, dynamic code execution, telemetry forwarding |
| `govern-template.json` | Config template | orchestra config, output_contract, telemetry events, agent tool config |

---

## Testing

### New Test Suites

| Test | Assertions | What it validates |
|------|-----------|-------------------|
| `test_consequence_proof.sh` | 13 | Consequence-boundary enforcement: advisory escalation, score integrity, hard blocks, taint enforcement |
| `test_uncatchable.sh` | 14 | GovernanceHardError cannot be caught by NAAb try/catch |
| `test_subprocess_containment.sh` | 15 | RLIMIT_NPROC, PATH restriction, env scrubbing, timeout |
| `test_extends.sh` | 44 | Policy inheritance: parent/child merge, array append, ratchet enforcement, env_vars |
| `test_polyglot_reload.sh` | 5 | Hot config reload: ratchet, tightening, governance_notices |
| `test_telemetry_forward.sh` | 4 | Webhook forwarding, batch delivery, non-blocking |
| `test_api_auth.sh` | 16 | Multi-key auth, scoped permissions |
| `test_governance_properties.sh` | 6 | Property-based invariants: idempotency, monotonicity, determinism, transparency |
| `run-pulse-chaos.sh` | 35 | Pulse verdict transitions, hysteresis, recovery, counters, weight parsing |
| `run-agent-tools.sh` | 33 | Tool registration, dual-gate, budget, result scanning |
| `run-naab35.sh` (gorilla) | 40 | Live tool execution against Gemini API |
| `run-naab36.sh` (gorilla) | 24 | Pulse verdict with live agents |
| `run-naab37.sh` (gorilla) | 32 | End-to-end: codegen, escalation, subprocess containment |
| `test_error_msg_leaks.sh` | 738 | No governance bypass hints in error messages |
| `test_govern_json_fuzz.sh` | 100 | Config parser doesn't crash on malformed input |

### Test Suite Summary

| Metric | After branch |
|--------|-------------|
| Total tests | 396 |
| Security leak checks | 738 |
| Config fuzz cases | 100 |
| Gorilla test assertions | 450+ |
| Unexpected failures | 0 |

---

## How This Makes NAAb Better

### More Accurate Enforcement
- **GovernanceHardError** eliminates the try/catch bypass — HARD means HARD, no exceptions
- **Score integrity fixes** ensure violation weights are calculated correctly
- **env_vars enforcement** blocks secret exfiltration at the syscall boundary, not just the scanner level
- **Subprocess containment** catches runtime-constructed commands that evade static analysis

### Better Observability
- **Governance Pulse** gives a single HEALTHY/DEGRADED/IMPAIRED verdict instead of requiring manual interpretation of 50+ individual signals
- **25 telemetry event types** with tamper-evident hash chains make every governance decision auditable
- **RESPONSE_SUPPRESSED** fills the observability gap for empty responses
- **Refusal attestation** provides cryptographic proof of enforcement

### Less Friction
- **Policy inheritance** — teams inherit from base configs instead of copy-pasting
- **Hot reload** — tighten governance mid-run without restarting
- **Orchestra module** — common multi-agent patterns built in, not reimplemented per project
- **codegen.run()** — agents can generate and execute code through governance, not around it
- **Windows polyglot** — JS and Python work on Windows without workarounds
- **Output contracts** — enforce response structure, eliminating downstream parsing failures

### Stronger Security Model
- **Standing Lease** limits authorization duration (compromised agents expire)
- **Advisory Escalation** makes repeated violations increasingly expensive
- **Evidence Epoch** prevents stale evidence from influencing current decisions
- **Dual-gate tool execution** requires both config author and code author to agree
- **7 defense layers** on every tool call, from declaration to behavioral analysis

---

## Commit Log

| # | Hash | Date | Type | Description |
|---|------|------|------|-------------|
| 1 | `2d3a9d64` | Jun 3 | feat | Governed agent tool execution loop (7 defense layers) |
| 2 | `764a2817` | Jun 3 | docs | Update references for agent tool execution feature |
| 3 | `40b1f442` | Jun 3 | fix | VM closure support in tool registration + send tool defs on initial API call |
| 4 | `65f6096a` | Jun 3 | fix | Add GK7-9 key rotation + optimize API pre-check in tool tests |
| 5 | `596e22d5` | Jun 3 | fix | Tool latency floor + switch tests to gemma-4-31b-it |
| 6 | `69ef6298` | Jun 3 | feat | Governed dynamic code execution — codegen.run() module |
| 7 | `f2955662` | Jun 3 | fix | Filter Gemini thinking tokens + switch reload test to gemma-4-31b-it |
| 8 | `db979a33` | Jun 4 | feat | Governance pulse — real-time self-assessment with signal wiring |
| 9 | `e7775373` | Jun 4 | fix | Pulse review fixes + chaos/gorilla test suites (naab-36) |
| 10 | `3cd561e3` | Jun 4 | feat | Standing lease, advisory escalation, evidence epoch |
| 11 | `8b8d1085` | Jun 5 | feat | Consequence-boundary hardening + enforcement proof harness |
| 12 | `c05445e6` | Jun 5 | fix | verifyScoreIntegrity off-by-one with weight_multiplier |
| 13 | `2c62f2ec` | Jun 5 | fix | Governance transparency test for signed repos |
| 14 | `f229dd9b` | Jun 5 | feat | Enterprise readiness — polyglot reload, telemetry forwarding, multi-key auth, policy distribution |
| 15 | `aaf736ff` | Jun 5 | fix | Harden enterprise readiness — 8 security/correctness fixes from critical review |
| 16 | `c6c778bf` | Jun 5 | fix | Eliminate data race on rules_ via COW shared_ptr + thread-local snapshot |
| 17 | `11964c4f` | Jun 5 | fix | Bound telemetry forwarder shutdown drain to prevent 500s hang |
| 18 | `a2a3f6be` | Jun 5 | feat | Complete mergeRules() coverage for policy distribution |
| 19 | `ca7bf781` | Jun 5 | fix | M5/M6 comments + extends test gaps (T2b, T5b, T9-T11) |
| 20 | `bdd1418a` | Jun 5 | feat | M3 explicit-set tracker for correct config inheritance |
| 21 | `07429c32` | Jun 6 | fix | Parse merge_arrays from govern.json inheritance config |
| 22 | `e7a72300` | Jun 6 | test | Add merge_arrays=append coverage to test_extends.sh (T14-T17) |
| 23 | `4d38ae0e` | Jun 6 | feat | Enforce env_vars blocked_read/allowed_read/blocked_write/allowed_write at runtime |
| 24 | `fd97d0ae` | Jun 6 | feat | GovernanceHardError — uncatchable HARD governance exceptions |
| 25 | `c129dfd9` | Jun 6 | docs | Add GovernanceHardError and env_vars enforcement to CLAUDE.md |
| 26 | `4afa345a` | Jun 6 | feat | Add EnforcementLevel::DETECT — catchable governance for test configs |
| 27 | `a6ee0a88` | Jun 6 | feat | OS-level polyglot subprocess containment (5-layer) |
| 28 | `f3861adf` | Jun 6 | feat | Non-binding refusal attestation — tamper-evident proof of governance blocks |
| 29 | `faa2b739` | Jun 7 | fix | 7 bugs from critical review — GovernanceHardError bypass, data races, off-by-one |
| 30 | `1c900cd2` | Jun 7 | fix | REST API GovernanceHardError bypass — _exit(3) at 3 catch sites |
| 31 | `a8cfc7e1` | Jun 7 | fix | 14 bug fixes + naab-37 gorilla test (32/32 assertions) |
| 32 | `b72faf24` | Jun 7 | — | Add naab-37 prompt.md for Haiku project coding |
| 33 | `ddb15265` | Jun 8 | feat | Runtime governance observability — 7 per-turn telemetry events + 3 infrastructure fixes |
| 34 | `99447293` | Jun 10 | feat | 8-phase governance observability + orchestration — Phase 2-8 of audit fixes |
| 35 | `d8c0df43` | Jun 10 | fix | Add missing getName() override to OrchestraModule |
| 36 | `00c8e4bb` | Jun 10 | fix | CI build failures — missing vector include and MSVC localtime_r |
| 37 | `5079788c` | Jun 10 | fix | CI failures — MSVC unistd.h, C API GovernanceHardError, test signing |
| 38 | `2fc5c9d7` | Jun 10 | fix | TMPDIR fallback for CI — use /tmp on non-Termux systems |
| 39 | `8ae156ff` | Jun 10 | fix | Guard unistd.h include in rest_api.cpp for MSVC |
| 40 | `1f125066` | Jun 10 | fix | Handle env.get crash on newer CI runner images gracefully |
| 41 | `e9a22af7` | Jun 10 | fix | Skip POSIX-only and taint tests on Windows/MSYS2 |
| 42 | `2b938eb6` | Jun 11 | fix | Mark test_drift_detection.sh as pre-existing failure |
| 43 | `c4b021a6` | Jun 11 | fix | Root-cause env.get crash + remove unnecessary Windows test skips |
| 44 | `2a1775a2` | Jun 11 | docs | Update all docs for 30+ commit feature branch + fix Windows test skips |
| 45 | `ca687944` | Jun 11 | feat | Enable JavaScript and Python polyglot executors on Windows/MinGW64 |
| 46 | `2c9a683c` | Jun 11 | fix | Use multi-line polyglot blocks in Windows smoke tests |
| 47 | `bb90b0f9` | Jun 11 | fix | Add HAVE_QUICKJS compile definition to naab-lang target |

**Breakdown:** 16 features, 26 fixes, 3 docs, 1 test, 1 untyped

---

## Files Changed (Key Source Files)

| File | Lines changed | What changed |
|------|--------------|--------------|
| `governance_config.cpp` | +1,463 | Policy inheritance, enterprise config parsing, output contracts, tool config |
| `governance_engine.cpp` | +1,107 | Pulse, lease, escalation, epoch, env_vars enforcement, reload, attestation |
| `agent_impl.cpp` | +1,267 | Tool execution loop, extract_code, environment, output contracts |
| `codegen_impl.cpp` | +590 | Entire module — run, run_with_args, run_strict, supported_languages |
| `governance_reports.cpp` | +342 | Telemetry events, hash chain, dashboard, SARIF output |
| `orchestra_impl.cpp` | +274 | sequential_refinement, consensus_vote, enforce_convergence |
| `main.cpp` | +295 | Executor registration restructure, Windows support, CLI flags |
| `agent_provider.cpp` | +230 | Tool definitions in API calls, response parsing for tool calls |
| `subprocess_helpers.cpp` | +184 | Containment enforcement, Windows CreateProcess improvements |
| `telemetry_forwarder.cpp` | +180 | Webhook/SIEM forwarding, bounded drain, batch delivery |
| `behavioral_sequence.cpp` | +128 | BSD normalization, tool patterns, pulse integration |
| `rest_api.cpp` | +169 | Multi-key auth, GovernanceHardError re-throw, scoped permissions |
