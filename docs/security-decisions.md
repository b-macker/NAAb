# Security Decisions

Records of deliberate security trade-offs — proposals that were evaluated,
adversarially analyzed, and rejected or reshaped. Check this file before
re-proposing a convenience feature that touches the governance boundary.

## Rejected: `governance.resign()` (in-process govern.json re-signing)

**Proposed** (living-script findings, 2026-07): a stdlib function that re-signs
govern.json using the signing key the engine already holds in memory, so
scripts that legitimately modify govern.json don't need out-of-band access to
`NAAB_SIGNING_KEY` (which is scrubbed from both the `env` stdlib and all
polyglot/subprocess children).

**Rejected because** it collapses a three-step attack into one. Today,
weakening a running governance config requires:

1. writing a weakened govern.json (possible whenever `shell.enabled: true` —
   govern.json self-protection blocks stdlib `file.write()`, not subprocess
   writes), **and**
2. access to the signing key path (blocked — `NAAB_SIGNING_KEY` is scrubbed
   from script-visible env and child processes), **and**
3. running `naab-lang --sign-governance` with that key.

`governance.resign()` eliminates steps 2 and 3: the engine would sign whatever
is on disk, and the mid-run hot-reload would accept it. Any agent able to
influence the script (or the developer writing it) could then launder a
weakened config through the signature check. The out-of-band signing step is
the load-bearing barrier that keeps the signing key a defense-in-depth gate
rather than a rubber stamp — the `NAAB_SIGNING_KEY` scrubbing in
`src/stdlib/env_impl.cpp` and subprocess env scrub policy are intentional and
must not be bypassed by an in-process signing path.

**If this need resurfaces**, the accepted workflow is: modify govern.json,
then re-sign from *outside* the NAAb process (operator shell, CI step) with
`naab-lang --sign-governance`. Do not add an in-process signing primitive.

## Reshaped: CDD rate normalization requires a floor penalty

`context_drift.rate_normalized` divides signal penalties by turns elapsed so
sparse signals in long runs aren't over-penalized. Unmitigated, this enables
the boiling-frog attack: an adversary who paces drift (one deviation every N
turns) drives each penalty toward zero and can drift indefinitely.
`rate_normalized_floor` (default 0.5) guarantees a firing signal always pays
at least that fraction of its base weight. The floor is ratchet-enforced
(lowering mid-run is a violation), and enabling `rate_normalized` itself
mid-run is a ratchet violation because it strictly reduces penalties.

## Reshaped: config reloads reset the CDD rate window, not the baseline

Operator config changes (e.g. a `system_prompt` rewrite) used to fire
`persona_drift`/`mandate_alignment` as if the agent had drifted. The fix is a
*scoped* reset in `ContextDriftAnalyzer::onAgentConfigChanged()`: only the
post-baseline rate window (signal snapshots + `post_baseline_checks`) restarts
for agents whose config actually changed. Learned baseline statistics
(mean/stddev), coherence, and turn history are preserved. A full baseline
clear was rejected: an operator flip-flopping a freely-adjustable field could
keep agents in a permanent penalty-free grace period. With the scoped reset,
a zeroed rate window makes the adaptive path fall back to the full base
penalty — repeated reloads are stricter, not looser.

## Rejected: adaptive natural healing (faster recovery at low coherence)

Proposed alongside raising `coherence_natural_healing`. Rejected because it
inverts CDD logic: a low-coherence agent is the one under the most scrutiny,
and faster recovery at low coherence enables sawtooth drift (drift → fast
recover → drift) that never accumulates enough penalty to trigger
intervention — especially combined with rate normalization. Healing stays a
fixed per-turn rate; the engine default remains 0.0 (opt-in) with 0.02 as the
recommended template value.

## Reshaped: telemetry hash chain is file-anchored, not process-anchored

The tamper-evident chain originally seeded `prev_hash` from the constant
`chain_genesis` on every process start. Each run's chain was internally
sound, but nothing linked run N's final hash to run N+1's first event —
deleting an entire run from a shared JSONL file was undetectable, and the
verifier reported a false BREAK at every legitimate run boundary, making
multi-run files unverifiable in practice. The chain is now anchored to the
output file: every write seeds `prev_hash` from the file's last chained
event (read under the same flock that serializes writers), so runs and
flock-serialized concurrent processes link into one continuous chain. A
`RunStart` anchor explicitly commits to the predecessor's tail hash and a
`RunEnd` anchor declares the run's chained event count. The verifier treats
pre-continuity genesis re-seeds as `LEGACY RESTART` warnings so old files
remain verifiable. The audit chain gets the same tail-seed on first write.

Residual limitations, accepted deliberately:

- **Trailing truncation of the final run** (deleting its tail after removing
  `RunEnd`, or deleting the entire final run) is only advisory-detectable —
  a crash is indistinguishable from truncation without an external witness.
  Mitigation is `telemetry.forwarding` (webhook/SIEM), which ships events
  off-host as they occur.
- **The evidence writer lives in the governed process.** The chain detects
  after-the-fact file tampering; it cannot prove a compromised process wrote
  honest events in the first place. HMAC keys (`tamper_evidence.hmac_key_env`)
  raise the bar — an attacker without the key cannot re-forge the chain.

Evidence collection is ratchet-protected: disabling
`telemetry.tamper_evidence`, `telemetry.decision_snapshots`,
`audit.tamper_evidence`, or `telemetry.transcript` mid-run is a ratchet
violation — evidence switches are one-way once enabled.

## Reshaped: transcripts stay outside the chain but are committed into it

The agent transcript remains a plain audit/debug log (no hash chain of its
own — by design, it is opt-in and content-heavy). Instead, every written
entry now carries an `entry_hash` (SHA-256 of the entry as built, before the
hash field is added), and when the telemetry chain is active a chained
`TRANSCRIPT_REF` event commits that hash into the tamper-evident record.
Editing a transcript entry after the fact breaks its recomputed hash against
the chained reference. Chaining the transcript directly was rejected: it
would couple the debug log's availability to the evidence chain and bloat
the hash-verified surface with full prompt/response content.

## Investigated: do detection-only CDD signals pump governance pressure?

A living-script run analysis inferred that `entity_consistency` (S15) — which
fires in detection at nearly every developer turn but is absorbed by the
adaptive baseline (zero coherence penalty) — was nonetheless driving pressure
escalation (governance level → elevated) and triggering wasteful step-up
challenges. Tracing the code disproves the premise: `signal_density` (circuit
breaker Factor 3) reads `signals_fired_this_turn`, and that counter is
incremented behind the **same** `if (p > 0.0)` gate as the coherence penalty
(`behavioral_sequence.cpp`, e.g. the S19/S22 blocks). An absorbed firing has
`p == 0`, so it increments only `turn_fired[]`/the cumulative counter
(telemetry) and contributes **zero** to `signal_density`. Detection-only or
baseline-absorbed signals therefore cannot pump pressure through signal
density.

The escalation observed was signal-independent: conversation-depth (Factor 4,
rises monotonically with turn count on long sessions), temporal coherence
decay, and `coherence_velocity` (S6) feeding Factor 7 (`coherence_acceleration`)
by design. No engine change is warranted for the "detection-only signals pump
pressure" claim — the engine already behaves as desired. Benign long-session
escalation is a tuning matter (`circuit_breaker.elevated_sustained`,
`expected_conversation_depth`) handled per-config, not in the engine.

## Added: S22 validation_outcome uses a flat penalty, never adaptive baselining

The S22 signal folds external ground-truth (pytest / `enforce_convergence`
pass-fail) into coherence via `agent.record_validation()`. It deliberately uses
`base_penalty` (flat, `rate_normalized_floor`-guaranteed) rather than
`adaptive_penalty`. Adaptive baselining returns 0 once a signal's post-baseline
rate sits at/below its learned mean — correct for heuristic signals, but wrong
for ground truth: an agent that fails tests every turn during the baseline
window would train a "failure is normal" mean and then have subsequent failures
absorbed to zero, recreating the exact "coherence 1.0 through rejected code"
blindness the signal exists to close. A failing test is objective evidence the
output is wrong and must always cost coherence. Consequently S22 is excluded
from `snapshotSignalCounters` (the adaptive-baseline snapshot map), alongside
S6/S7 which are also non-baseline signals.
