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
| **Admissibility** | The post-execution gate: is the *result* coherent enough to enter state? Coherence threshold + pulse override; optionally extended to each tool call. | `checkOutputAdmissibility()`, `enforceOutputAdmissibilityGate()` | `circuit_breaker.output_admissibility` (`enabled`, `threshold`, `action`, `level`, `inadmissible_history`, `gate_tool_calls`, `on_undetermined`) | `OUTPUT_ADMISSIBILITY_EVAL`, `OUTPUT_INADMISSIBLE` |
| **Commit** | Conversation state advances: history append + turn increment. Split from accounting — blocked or excluded results never enter the context replayed on later turns. | split-commit section of `agentSend()`; `agentCommit()` | `output_admissibility.inadmissible_history` | `AGENT_PROPOSAL_COMMIT`, transcript `history_committed` |
| **Refusal** | Structural rejection: `GovernanceHardError` (uncatchable by NAAb try/catch), signed refusal attestation, `g_governance_hard_block` belt-and-suspenders exit 3. | `enforce()` in `governance_engine.cpp` | enforcement `level` fields | refusal attestations, `BSD_MATCH` |

## The third verdict: undetermined

The gate has two outcomes in the table above and **three** in practice. A pass
can mean "this result is coherent" or it can mean "nothing was in a position to
say otherwise", and those are different claims.

While a handle is inside its adaptive baseline window, `in_baseline` suppresses
every *statistical* signal's penalty. Coherence stays at its ceiling regardless
of what the agent produced, and the threshold comparison passes on a number that
was never a judgement.

`checkOutputAdmissibility()` computes
`determined = !adaptive_baseline_enabled || baseline_complete` and sets
`OutputAdmissibilityResult.undetermined`. `OUTPUT_ADMISSIBILITY_EVAL` reports
`result: "undetermined"` alongside `baseline_state`
(`calibrating` / `complete` / `disabled`); `response.admissibility.undetermined`
carries it to scripts on both directions.

**The label is one-directional.** It marks the *pass* direction only. A low
coherence stays meaningful while calibrating, because objective signals — S1
`circular_actions`, S21 `response_repetition` — are exempt from baseline
absorption and charge from turn 1. The gate can still rule OUT; it cannot rule
IN.

`output_admissibility.on_undetermined` chooses what to do with it:

| value | behaviour |
|---|---|
| `"pass"` (default) | deliver; identical to behaviour before the field existed |
| `"quarantine"` | hold the response — `admissible: false`, same shape as a coherence quarantine |
| `"block"` | `enforce()` at `level` |

Ratchet: `pass` < `quarantine` < `block`; relaxing mid-run is a loosening
violation.

**Undetermined holds never advance the quarantine streak.** The streak is a
termination counter for the quarantine-and-commit degradation loop; an
undetermined turn is absence of evidence, not evidence of decay. It is also an
unconditional kill if counted — `adaptive_baseline_window` and
`max_quarantine_streak` both default to 5, so counting undetermined holds would
terminate every agent on its last calibrating turn regardless of behaviour.

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

  A quarantined response contaminates state through two distinct
  channels, and `inadmissible_history` governs only one of them:

  - **Model-context channel** — with `"commit"`, the degraded response is
    replayed to the model on every later turn (subject to context
    windowing). `"exclude"` closes this channel: the model never re-reads
    its quarantined output (the paired user message is also dropped to
    preserve role alternation, so the agent "forgets" the exchange).
  - **CDD-state channel** — context-drift scoring runs *before* the
    admissibility gate, so DriftState (response fingerprints, entity
    contexts, semantic-stability keywords, the coherence penalty itself)
    absorbs the quarantined response under BOTH settings. This is by
    design: the gate's own verdict is derived from that evidence, and
    governance cannot un-see behavior that actually occurred.

  Choose `"exclude"` to stop degraded output from steering the model;
  do not expect it to reset governance's memory of the turn — only
  `agent.reset()` clears drift state.

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
