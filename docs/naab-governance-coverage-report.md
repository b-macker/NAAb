# NAAb Governance Coverage Report
## Signal Chain Traceability, Continuous Governance, and Authority Lifecycle

**Date:** June 19, 2026
**Scope:** Sections 2-5 from naab-41 consequence-boundary analysis
**Validation:** 66/66 checks passed, 23/23 deterministic on 2nd pass (naab-41 test)

---

## 1. Signal Chain Traceability — Observation Through Consequence

The question posed was not "are the controls present" but "can you trace a single signal through every transformation that leads to consequence." The naab-41 test (66 checks) proves the gate works. This section maps whether the gate knows what it's gating.

### 1.1 Observation → Data
Lexer turns source text into tokens. Parser turns tokens into AST. Agent module turns API responses into structured content. These are mechanical transformations — no governance applied, and none needed. The signal hasn't crossed a consequence boundary yet.

### 1.2 Data → Evidence
`checkPolyglotBlock()` runs 44 checks against the AST node (governance_checks.cpp:6254-6449). Each produces a `CheckResult` with 15 fields (governance.h:2186-2205):
- `rule_name`, `level`, `passed`, `rationale`, `decision_trace`
- CDD produces coherence scores
- BSD produces pattern matches
- Taint tracking produces flow status

This is where raw data gains evidentiary structure. Before this point, the system has data. After it, the system has evidence.

### 1.3 Evidence → Standing
`enforce()` (governance_engine.cpp) is the standing gate. Evidence without standing is recorded but doesn't block. Standing derives from four sources:

| Source | Mechanism | Test Proof |
|--------|-----------|------------|
| Config authority | Signed govern.json (Ed25519) | T3: corrupted signature blocks execution |
| Temporal validity | Epoch, lease TTL | T4: staleness is evaluated |
| Scope | Language/action matrix | T8a-T8e: enforcement levels exercised |
| Freshness | Stale signatures rejected | T3: mid-run injection rejected |

### 1.4 Evidence → Inference
Three inference mechanisms — each arithmetic, not heuristic:

1. **Scoring:** Individual check weights accumulate into a risk score. *Proved:* L3 test — `5 + 10 = 15`, dashboard matches sum.
2. **Advisory escalation:** Occurrence count triggers severity upgrade. *Proved:* L5 test — 3rd occurrence crosses `soft_after` threshold.
3. **Pulse verdict:** Subsystem health signals combine into HEALTHY/DEGRADED/IMPAIRED with hysteresis to prevent gaming recovery.

### 1.5 Inference → Workflow
The `enforce()` switch statement (6 branches, governance_engine.cpp:1017-1127):

| Branch | Effect |
|--------|--------|
| NONE | No enforcement (line 1018) |
| HARD | `GovernanceHardError` — uncatchable, `_exit(3)` (line 1021) |
| DETECT | Catchable variant — same detection, std::runtime_error (line 1026) |
| APPROVAL_REQUIRED | Requires prior `--approve` (line 1033) |
| SOFT | Warning + block (line 1048) |
| ADVISORY | Warning, logged, hardens on repetition (line 1073) |

`recordPass()` (line 859) is a separate function called on pass, not a branch of the enforce() switch. This is the narrowest point in the pipeline. Every inference funnels through one function. The 66-check test exercised all six branches (T8a-T8e, T2b).

### 1.6 Workflow → Authority
Ed25519 signature on govern.json. Trust store keys verified at startup, mid-execution injection rejected. Ratchet enforcement prevents loosening. Epoch freshness discounts stale authority. The authority is the signed policy, not the runtime.

### 1.7 Authority → Action
Binary crossing point:
- **Pass:** `enforce()` returns `""` → `executeWithReturn()` fires.
- **Block:** `GovernanceHardError` thrown → uncatchable (T7a proved) → `main.cpp` catches → `_exit(3)`.

There is no partial execution. The telemetry hash chain records which outcome occurred.

### 1.8 Post-Crossing — What the System Remembers
- **Telemetry hash chain** — L1: 15 events, 0 broken links. Append-only, tamper-evident.
- **RefusalAttestation** — L2: signed, `execution_prevented:true`, `binding_status:non-binding`.
- **Decision trace** — per-check rationale and rule_name.

### 1.9 The Honest Gap — Reversibility at the Crossing Point
If the crossing was a **block**: the RefusalAttestation records it. The code never executed. Nothing to reverse. The operator adjusts govern.json, re-signs, re-runs. Epoch increments, prior evidence resets.

If the crossing was a **pass**: the code executed. Telemetry records that it happened, but execution is irreversible. No undo. No appeal queue. No second-opinion gate. The closest analog is advisory escalation — the system remembers the pattern was allowed, and hardens on repetition. But that's forward-looking prevention, not backward-looking reversal.

**Architectural position:** NAAb treats execution as irreversible. The entire governance engine is upstream of consequence precisely because downstream reversal is not possible at the runtime layer. The system's answer to "who can challenge the crossing" is: the policy author, before the next run — not an affected party, after the fact.

---

## 2. Continuous Governance During Live Agent Execution

The question: when an agent is executing directions but the environment changes mid-run, is that covered?

NAAb's continuous governance layer was built for exactly this scenario. The signal chain during a live agent run:

### 2.1 Standing Decays
`standing_lease_seconds` and `standing_lease_turns` enforce temporal TTL on agent authorization (Kerberos TGT analog). The lease expires and forces step-up challenge before the agent can continue. Not a periodic check — it fires at the crossing point (`agent.send()`).

### 2.2 Authority Is Re-Verified
`reloadIfChanged()` detects govern.json mtime changes between agent turns. Signature re-validated. Ratchet enforced — controls can only tighten mid-run, never loosen. Config change notices surface to the agent via `governance_notices` in the response dict.

### 2.3 Old Evidence Loses Weight
Evidence epoch increments on:
- Pulse verdict change
- Governance level change
- Config reload

`consecutive_passes` resets to zero. The agent cannot coast on passes earned before the environment shifted. (MVCC analog — old reads don't count after a schema change.)

### 2.4 Coherence Is Monitored
CDD tracks drift across turns. If the agent's behavior changes (because the environment changed and its responses adapted), the coherence score drops. Temporal decay erodes trust when idle.

### 2.5 Behavioral Patterns Are Tracked
BSD watches event sequences across a sliding window. Environmental change that causes unusual patterns (rapid tool calls, data exfiltration sequences) triggers pattern matches.

### 2.6 The Agent Sees the Change
Three awareness channels:

| Channel | What It Provides |
|---------|-----------------|
| Birth snapshot | World state at `agent.create()` |
| Live environment | `agent.environment()` — remaining capacity, key health, coherence, lease |
| Config change notices | Ratchet violations detected per-agent |

### 2.7 Upstream Provenance Propagates
In pipelines, downstream stages see `upstream_provenance`:
- Whether the upstream model fell back
- How many retries occurred
- Coherence at output
- Which API keys are dead
- Latency and pressure metrics

The environment change is visible to every stage that follows.

### 2.8 Design Assumption
The architecture assumes the environment **will** change. The lease is the TGT — no indefinite authorization. The epoch is the MVCC version — old reads don't count. The ratchet is the one-way valve — the environment can get stricter, never looser.

---

## 3. Three-Layer Execution Coverage Model

NAAb provides governance at three distinct temporal layers:

### Layer 1: Pre-Execution — The Consequence Boundary Gate
`checkPolyglotBlock()` → `enforce()`. Nine pre-execution questions answered before any code runs.

**Coverage:** 44 static checks (governance_checks.cpp:6254-6449), normalization, injection detection, capability gates, taint analysis, co-occurrence detection, obfuscation detection across 10 polyglot languages.

**Proof:** naab-41 test — 66/66 checks, 23/23 deterministic on 2nd pass.

### Layer 2: During Execution — Continuous Agent Governance
Lease decay, epoch resets, coherence monitoring, ratchet enforcement, config reload, upstream provenance. The environment changes; the governance adapts. Standing is re-earned, not assumed.

**Coverage:** Standing lease (time + turn TTL), advisory escalation (repeated findings harden), pulse verdict (HEALTHY/DEGRADED/IMPAIRED with hysteresis), behavioral sequence detection, config change notices, upstream provenance propagation.

### Layer 3: At Execution Time — The Binding Point
`enforce()` returns `""` and `executeWithReturn()` fires, OR `GovernanceHardError` throws and `_exit(3)` kills the process.

**Properties:**
- Uncatchable (NAAb try/catch explicitly re-throws in 5 interpreter sites, 6 VM sites — 11 total)
- Fail-closed (process terminates, no fallback)
- Binary (execute or don't — no partial execution)
- Recorded (telemetry hash chain captures which outcome occurred)

### Post-Crossing
Tamper-evident receipts, RefusalAttestation, decision traces. Not reversible, but provable.

### Scope Boundary
NAAb covers observation-through-action at the **runtime layer**. Identity, network, storage, and reversal belong to the deployment stack above and below. This is a scope boundary, not a gap — the 8 items identified in the initial critical review as "missing" are correctly out-of-scope for a language runtime.

---

## 4. Authority Lifecycle — Created, Transferred, Maintained, Challenged, Revoked

NAAb governs **authority**, not code. The system never asks "is this code safe." It asks "does this code have authority to execute, and does that authority still hold."

### 4.1 Authority Created
| Mechanism | Detail |
|-----------|--------|
| Ed25519 keypair | `--keygen` generates signing keys |
| Trust store | `--trust-key` installs public key |
| Policy signing | govern.json signed by key holder |

Authority doesn't emerge from the system — a human creates it, signs it, installs it. The runtime never generates its own authority.

### 4.2 Authority Transferred
| Mechanism | Detail |
|-----------|--------|
| `extends`/inheritance | Parent policy flows to child (enterprise policy distribution) |
| Pipeline provenance | Downstream stages inherit trust signals from upstream |
| Agent birth snapshot | Handles carry authority state from `agent.create()` |
| Standing lease | Time-bounded authority grant to act |

Each transfer is scoped and traceable.

### 4.3 Authority Maintained
| Mechanism | Detail |
|-----------|--------|
| Signature verification | Every load and reload |
| Ratchet enforcement | Can only tighten, never loosen mid-run |
| Epoch monotonicity | Counter never goes backward |
| Lease renewal | Authority persists only while governance checks succeed |
| Trust store tamper detection | Keys removed mid-execution trigger immediate block |

### 4.4 Authority Challenged
| Mechanism | Detail |
|-----------|--------|
| Advisory escalation | Repeated findings harden from ADVISORY → HARD (direct escalation via GovernanceHardError, governance_engine.cpp:1093) |
| Step-up challenges | Elevated governance levels require coherence proof |
| Coherence drift detection | Behavioral deviation erodes trust |
| Pulse verdict transitions | HEALTHY → DEGRADED → IMPAIRED, with hysteresis to prevent gaming recovery |

### 4.5 Authority Revoked
| Mechanism | Detail |
|-----------|--------|
| Lease expiry | Time-based and turn-based, non-negotiable |
| Epoch boundary | Prior evidence discounted, consecutive passes reset |
| Key rotation | Dead keys revive only after cooldown |
| `GovernanceHardError` | Uncatchable, irreversible revocation of execution authority |
| Config reload + ratchet | Policy author can tighten controls mid-run; agent cannot refuse |

### 4.6 The Distinction
The 66 checks prove not that the system detects bad code, but that **authority flows through a traceable, challengeable, revocable chain from human signer to execution boundary**. The first question (is this code safe?) is about intelligence. The second (does this code have authority?) is about governance. NAAb answers the second.

---

## Validation Summary

| Metric | Result |
|--------|--------|
| naab-41 total checks | 66/66 passed |
| Phase 1 — source verification | 15/15 (11 confirmed, 4 drifted line numbers) |
| Phase 2 — behavioral tests | 21/21 (9 pre-execution + 4 proof surface + epoch) |
| Phase 3 — log evidence with math | 5/5 (hash chain, attestation, score sum, determinism, escalation) |
| Phase 4 — deterministic 2nd pass | 23/23 (all behavioral tests reproduce identically) |
| Signal chain coverage | Observation → Data → Evidence → Standing → Inference → Workflow → Authority → Action → Memory |
| Temporal coverage | Pre-execution, during-execution, at-execution |
| Authority lifecycle | Created → Transferred → Maintained → Challenged → Revoked |
| Identified scope boundary | Runtime layer; identity/network/storage/reversal belong to deployment stack |

---

## Appendix: Code Evidence (Deep Verification, June 19 2026)

All claims in this report were verified against source code by three parallel audits. Five inaccuracies were corrected (marked with **CORRECTED**). Twenty-eight claims confirmed.

### Corrections Applied

| # | Original Claim | Corrected To | Evidence |
|---|---------------|-------------|----------|
| 1 | "50+ checks" in checkPolyglotBlock() | **44 checks** | governance_checks.cpp:6254-6449 (all `err = check*()` calls counted) |
| 2 | "7 branches" in enforce() | **6 branches** (recordPass() is separate) | governance_engine.cpp:1017-1127 |
| 3 | "3 interpreter, 4 VM" re-throw sites | **5 interpreter + 6 VM = 11** | interpreter.cpp:1702,1874,2596,2643,2707; vm.cpp:262,275,2006,3082,3313,3383 |
| 4 | "ADVISORY → SOFT → HARD" | **ADVISORY → HARD** (direct) | governance_engine.cpp:1078-1093 throws GovernanceHardError |
| 5 | naab-41 header "62 checks" | **66 checks** (Phase 4 has 23 not 21) | run-naab41.sh header stale; runtime yields 66 |

### Confirmed Claims — File:Line Index

| Claim | File:Line |
|-------|-----------|
| CheckResult 15 fields | governance.h:2186-2205 |
| enforce() at line 886 | governance_engine.cpp:886 |
| enforce() returns "" on pass | governance_engine.cpp:1019,1039,1066,1125,1128 |
| GovernanceHardError on HARD | governance_engine.cpp:1024 |
| _exit(3) on catch | main.cpp:2670-2676 |
| Hash chain prev_hash | governance_reports.cpp:920-927 |
| RefusalAttestation fields | governance_reports.cpp:193-287 |
| Scoring accumulation | governance_engine.cpp:918-993 |
| Advisory soft_after + weight_multiplier | governance_engine.cpp:1073-1094, governance.h:1479,1483 |
| PulseVerdict enum + hysteresis | governance.h:1510, governance_engine.cpp:6395-6420 |
| standing_lease at agent.send() | agent_impl.cpp:1104-1121, governance.h:1940-1941 |
| reloadIfChanged() mtime+sig+ratchet | governance_config.cpp:3241-3301, agent_impl.cpp:987 |
| Evidence epoch increments | governance_engine.cpp:6422-6426, governance.h:3011 |
| CDD drift + temporal decay | behavioral_sequence.h:195-239, governance.h:1302-1307 |
| BSD sliding window | behavioral_sequence.h:143-192, governance.h:1280 |
| Birth snapshot in handle | agent_impl.cpp:699 |
| agent.environment() live query | agent_impl.cpp:2873-2888 |
| governance_notices in response | agent_impl.cpp:2645-2652, 987-988 |
| upstream_provenance 7 fields | agent_impl.cpp:3400-3456 |
| --keygen Ed25519 | main.cpp:553-578 |
| --trust-key installs key | main.cpp:591-633 |
| extends/inheritance + merge_arrays | governance_config.cpp:507,1557,3553 |
| Ratchet tighten only | governance_config.cpp:2882-2895 |
| Epoch monotonicity (only ++) | governance_engine.cpp:6425 |
| Trust store tamper detection | governance_engine.cpp:4024-4044 |
| Step-up challenges (keyword coherence) | agent_impl.cpp:181-222 |
| Lease expiry non-negotiable | agent_impl.cpp:867-883 |
| Key rotation revival after cooldown | agent_impl.cpp:163-173 |
| Config reload: agent cannot refuse | agent_impl.cpp:987-997 |
