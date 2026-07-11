# Transition Admissibility

NAAb governs agent behavior as a sequence of **transitions** — discrete,
gated changes to conversation and system state. This document names the
layer, its lifecycle, and where each concept lives in code, config, and
telemetry. The machinery predates the vocabulary; the names below are the
canonical ones going forward.

## Lifecycle of one transition

```
proposed transition ──► admission ──► execution ──► admissibility ──► commit
     (proposal)         (pre-gate)    (API/tools)     (post-gate)        │
                            │                             │              └─ conversation state
                            └── refusal ◄─────────────────┘                 advances
                              (structural)
```

| Concept | Definition | Code | Config | Telemetry |
|---|---|---|---|---|
| **Proposed transition** | A candidate state change that has not executed. At program granularity: the source itself (preflight intent gate, preflight scanner, agent review). At turn granularity: `agent.propose()` candidates. | `agentPropose()` in `agent_impl.cpp`; `preflightIntentCheck()`, `runAgentReview()` | `agents.<name>.propose_candidates_max` | `AGENT_PROPOSE` |
| **Admission** | The pre-execution gate: would the *next* action exceed admissible bounds? Seven criteria — governance level, projected action count, coherence floor, pipeline depth, projected unique agents, checkpoint cooldown, risk budget. | `checkAdmission()` in `governance_engine.cpp` (enforces under the `exposure_tracking` rule name; `admission` is an accepted rationale alias) | `exposure_tracking` section | `ADMISSION_EVAL` |
| **Execution** | The API call and any governed tool execution. Accounting (tokens, latency, exposure counters) commits here — the call truthfully happened even if the result is later rejected. | retry loop + tool loop in `agentSend()` | `agents` block, `agent_dispatch` | `AGENT_RESPONSE`, `AGENT_TOOL_CALL` |
| **Admissibility** | The post-execution gate: is the *result* coherent enough to enter state? Coherence threshold + pulse override; optionally extended to each tool call. | `checkOutputAdmissibility()`, `enforceOutputAdmissibilityGate()` | `circuit_breaker.output_admissibility` (`enabled`, `threshold`, `action`, `level`, `inadmissible_history`, `gate_tool_calls`) | `OUTPUT_ADMISSIBILITY_EVAL`, `OUTPUT_INADMISSIBLE` |
| **Commit** | Conversation state advances: history append + turn increment. Split from accounting — blocked or excluded results never enter the context replayed on later turns. | split-commit section of `agentSend()`; `agentCommit()` | `output_admissibility.inadmissible_history` | `AGENT_PROPOSAL_COMMIT`, transcript `history_committed` |
| **Refusal** | Structural rejection: `GovernanceHardError` (uncatchable by NAAb try/catch), signed refusal attestation, `g_governance_hard_block` belt-and-suspenders exit 3. | `enforce()` in `governance_engine.cpp` | enforcement `level` fields | refusal attestations, `BSD_MATCH` |

## Split commit (accounting vs state)

`agent.send()` separates two kinds of commit:

- **Accounting commit** (always): tracker turns/tokens/latency and
  `recordAutonomousAction()` exposure counters commit *before* the
  post-receive gates. An attempted-but-blocked transition still counts
  toward the next admission projection — exposure tracking measures
  attempts, not just successes.
- **State commit** (conditional): the user/assistant history append happens
  *after* CDD and the output-admissibility gate. A blocked turn (including
  DETECT-level blocks caught by NAAb `try/catch`) never enters the handle's
  conversation history. For quarantine/attest actions,
  `output_admissibility.inadmissible_history` controls disposition:
  `"commit"` (default, response stays in context) or `"exclude"`.
  With `"commit"`, degraded content re-enters the context of every later
  turn, so `output_admissibility.max_quarantine_streak` (default 5,
  `0` = disabled) caps consecutive quarantined responses: exceeding it
  throws `GovernanceHardError`, terminating the degradation loop instead
  of letting quarantined output poison the conversation indefinitely.
  Ratchet: removing or raising the streak limit mid-run is a violation.

## Selective adjudication (propose → select → commit)

`agent.propose(handle, message, n)` executes the full pre-send gauntlet
once, generates up to `propose_candidates_max` candidates with **no state
commit** (no history, no turn, no CDD mutation, no tool execution), and
scores each against the current DriftState snapshot. Candidates carry an
anti-forge `__proposal` HMAC nonce; the authoritative content lives
server-side.

`orchestra.select_admissible(candidates [, spec])` is a pure ranking
function over the admissible candidates (optionally spec-checked with the
same pattern/required_fields logic as `enforce_convergence`).

`agent.commit(handle, proposal)` runs the normal post-receive pipeline
exactly once for the selected candidate: BSD response event, CDD, the full
enforce-capable output-admissibility gate, then the split commit.
Proposals are single-use and invalidated by any subsequent
send/propose/commit on the handle. Step-up-challenge or expired-lease
states refuse `propose()` (fail-closed) — re-authorization must go through
`agent.send()`.

## Naming note

Historical asymmetry, kept for compatibility: `checkAdmission()` enforces
under the rule name `exposure_tracking` (its config section), emits
`ADMISSION_EVAL` telemetry, and the post-gate owns `output_admissibility`.
Configs and telemetry consumers depending on those names keep working;
`admission` resolves to the same rationale in reports.
