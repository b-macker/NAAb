# De-escalation cluster (C1a, C1c, C1d) — live measurement findings

Investigation date: 2026-09-06
Branch: `investigate/deescalation-cluster-c1`
Commit base: `06052e4c` (master), rebased to `f031a71e`

## Summary

The register entries C1a, C1c and C1d describe real behaviour that was correct
when measured (keyed runs 4-6, pre-August 2026). On shipped defaults, the
**precondition** these entries assume — coherence at 0.0 — is not reached by
ordinary varied work (25 turns of software engineering tasks against
`gemini-flash-lite-latest`). The decisive change was the
`adaptive_baseline_enabled` flip to default-on (2026-08-29, #176), made for an
unrelated reason (S17's frozen warm-up baseline). That flip absorbs the signals
(S10, S11, S14, S15) that were causing the coherence floor.

**What this does NOT show:** ARM A never left NORMAL, so it produced zero
de-escalation events. This proves escalation did not happen on this workload.
It is NOT evidence that de-escalation is reachable — testing that claim would
require an arm that REACHES elevated and then has pressure fall, and neither
arm does. If any workload reaches ELEVATED on shipped defaults, C1a's
arithmetic (composite floor 0.45 > elevated_threshold 0.40 at coherence=0)
is unchanged.

**Accidental resolution.** The `adaptive_baseline_enabled` flip was made for
S17's frozen warm-up baseline, not for de-escalation. Nobody chose this
resolution. No test asserts that de-escalation is reachable on shipped
defaults. A future change to adaptive baselining (e.g. reverting the flip for
an S17-related reason) could silently re-expose the entire cluster.

**Partly addressed since this was written.** `tests/governance_v4/test_coherence_floor_precondition.sh`
now asserts the PRECONDITION on shipped defaults, with a positive control
(baselining off must still floor) and a detection control (verbatim repeats
must still charge). So a revert of the flip fails loudly instead of silently.
The sentence above still holds for the other half: **no test asserts that
de-escalation is reachable**, because nothing has ever exercised
`elevated -> normal`. That fixture is still unbuilt, and C1a's arithmetic
remains untested rather than refuted.

## Recommendation

**No code change.** Update the register entries to note that the precondition
(coherence at 0.0) is narrower than stated — it is not reached by ordinary
varied work on shipped defaults. The entries remain valid both for configs
that set `adaptive_baseline_enabled: false` and for any workload that does
reach zero coherence on defaults.

**Cost of this recommendation:** none, because no loosening is proposed. The
loosening already shipped (#176) and has been running for 8 days with a full
test suite passing.

**Residual risk:** the current behaviour rests on an accident (#176's side
effect), not a decision. A future S17 change could silently reverse it.
Mitigations: (a) add a regression test asserting coherence > 0 on a
varied-work fixture with shipped defaults, (b) document the dependency in
C1e's blocker so a future S17 reviewer is warned. Neither is proposed here —
flagged for decision.

## Evidence

### ARM A — shipped defaults (adaptive baseline ON)

25 turns of varied software engineering work against `gemini-flash-lite-latest`,
all correct on-mandate output. Config: CDD at defaults, `check_interval_turns: 1`,
no healing, no output admissibility, no step-up challenges, `thinking_budget: 0`.

| Turn | Coherence | Signals fired | Pressure | Level  | Penalizing signals |
|------|-----------|--------------|----------|--------|--------------------|
| 1    | 1.0000    | 0            | 0.0000   | normal | —                  |
| 2    | 1.0000    | 0            | 0.0050   | normal | — (S10,S15 absorbed) |
| 3    | 1.0000    | 0            | 0.0100   | normal | — (S10,S11,S15 absorbed) |
| 4    | 1.0000    | 0            | 0.0150   | normal | — (baseline window) |
| 5    | 1.0000    | 0            | 0.0200   | normal | — (baseline window) |
| 6    | 1.0000    | 0            | 0.0250   | normal | — (last baseline turn) |
| 7    | 0.9500    | 1            | 0.0925   | normal | S17=0.05           |
| 8    | 0.9033    | 1            | 0.0975   | normal | S12=0.047          |
| 9    | 0.8333    | 1            | 0.1025   | normal | S12=0.07           |
| 10   | 0.7133    | 2            | 0.1700   | normal | S12=0.07, S17=0.05 |
| 11   | 0.6433    | 1            | 0.1408   | normal | S12=0.07           |
| 12   | 0.5733    | 1            | 0.1808   | normal | S12=0.07           |
| 13   | 0.5033    | 1            | 0.2208   | normal | S12=0.07           |
| 14   | 0.4333    | 1            | 0.2608   | normal | S12=0.07           |
| 15   | 0.3633    | 1            | 0.3008   | normal | S12=0.07           |
| 16   | 0.2752    | 2            | 0.4124   | normal | S12=0.07, S17=0.02 |
| 17   | 0.2752    | 0            | 0.2924   | normal | —                  |
| 18-25| 0.2752    | 0            | ~0.31    | normal | —                  |

Coherence floors at 0.2752, NOT 0.0. Level stays NORMAL throughout — pressure
peaks at 0.41 for one turn (below `elevated_sustained` requirement of 2
consecutive). From turn 17 onward, zero signals fire and pressure declines.

**Why S10/S15 are absorbed:** they fire on turns 2-6 (inside the baseline
window) at a rate that becomes the baseline. Post-baseline, they fire at or
below that rate, so `adaptive_penalty` returns 0 and
`signals_fired_this_turn` is NOT incremented (source:
`behavioral_sequence.cpp`, the increment is inside `if (p > 0.0)`).

**Why coherence floors at 0.2752 and not 0.0:** S12 (context_growth) stops
firing at turn 16 because the adaptive EMA tracks linear input-token growth
and the ratio falls below `context_growth_factor` (3.0). No other signal
continues to penalize, so coherence stops declining.

### ARM C — register conditions (adaptive baseline OFF)

Same prompts, same model, same config except `adaptive_baseline_enabled: false`.
20 turns before global timeout.

| Turn | Coherence | Signals fired | Pressure | Level    |
|------|-----------|--------------|----------|----------|
| 1    | 1.0000    | 0            | 0.0000   | normal   |
| 2    | 0.6100    | 4            | 0.3000   | normal   |
| 3    | 0.2200    | 4            | 0.5000   | normal   |
| 4    | 0.0000    | 4            | 0.6150   | elevated |
| 5    | 0.0000    | 3            | 0.5575   | elevated |
| 7    | 0.0000    | 5            | 0.6300   | elevated |
| 8    | 0.0000    | 5            | 0.6350   | elevated |
| 9    | 0.0000    | 6            | 0.6400   | high     |
| 10-20| 0.0000    | 4-6          | 0.64-0.70| high     |

Coherence floors at 0.0 by turn 3. ELEVATED at turn 4, HIGH at turn 9.
De-escalation calm counter never starts (4-6 signals fire every turn, so
target level never drops below current). This reproduces the register's
claims exactly.

### Reproducing these runs

The `govern.json` copies here are **unsigned**, deliberately. Two `.sig` files
were committed with the original artifacts, but `*.sig` is gitignored
(`.gitignore:193`) and they had been force-added past that rule. A committed
signature only verifies against the trust store holding its private half, so it
is inert on any other machine and actively misleading on this one — the same
defect #198 removed from the gate suite.

On a machine with **no** trusted keys installed, the configs run as-is. On a
keyed machine an unsigned `govern.json` is an `INTEGRITY BLOCK` (exit 3) that
`--no-governance` cannot escape, so sign them locally first:

```
NAAB_SIGNING_KEY=<your key> build/naab-lang --sign-governance   # run in this dir
```

If a run here reports no telemetry or an unexplained exit 3, check
`ls ~/.naab/trusted-keys` before reading it as a finding.

### Provenance

All numbers in the tables above are **observed** — the system produced them
under conditions I set (authored prompts, configured govern.json). The
prompts are authored (25 varied software engineering tasks); the engine's
response to them is what is observed.

Evidence tier: **verified** — both arms ran against a live model, with the
other arm as its positive control. ARM A proves the mechanism (adaptive
baseline) is live; ARM C proves the mechanism is what prevents the floor.

### What this does NOT establish

- **De-escalation reachability.** ARM A never escalated, so it produced zero
  de-escalation events. This is "isolation can remove the property under
  test" (investigation-method.md). A workload that reaches ELEVATED on
  shipped defaults would face unchanged C1a arithmetic.

- **Threshold correctness.** Nothing here says the coherence floor of 0.275
  on shipped defaults is the RIGHT floor. It may be too lenient (scrutiny
  should have triggered) or too aggressive (coherence should not drop at all
  on correct work). That question requires a value judgement this measurement
  cannot supply.

- **Model robustness.** n=1 per arm, one model. A model that produces shorter
  or more varied responses might trip additional signals. The claim is that
  this workload on shipped defaults does not produce the register's scenario;
  the claim is NOT that no workload or model ever will.

- **The C1a arithmetic is unchanged.** At coherence=0 the composite floor is
  still 0.45 > elevated_threshold 0.40. What changed is that coherence did
  not reach 0.0 on this workload with shipped defaults.

### Direction of error

If this measurement is wrong, it errs toward **false reassurance** — claiming
the cluster is resolved when it is not. I checked this direction by running
ARM C, which reproduces the register's findings exactly. The mechanism
(adaptive baseline absorbing S10/S11/S14/S15) is confirmed in both
directions: ON absorbs them, OFF does not.

### Limitations

- **n=1 per arm**, one model — stated, not hedged away.
- **Arms are not matched**: ARM A ran 25 turns; ARM C ran 20 turns before
  the 600s global timeout. Per-turn rates should NOT be compared across arms.
  The comparison is qualitative (coherence floors at 0.275 vs 0.0; NORMAL
  throughout vs ELEVATED at turn 4, HIGH at turn 9).
- **ARM A never escalated**, so this measurement says nothing about whether
  de-escalation is reachable on shipped defaults. The claim is limited to:
  the precondition (coherence at 0.0) is not reached by this workload.

### Adversarial pass

1. The adaptive baseline flip was **accidental** (done for S17, not for
   de-escalation) — nobody designed this resolution. No test asserts that
   de-escalation is reachable or that coherence stays above 0.0 on varied
   work with shipped defaults. A future change to S17 or adaptive baselining
   could silently re-expose the entire cluster with no regression signal.
2. S12 stops firing at turn 16 because EMA catches up — a non-linear growth
   pattern might not converge. Not tested.
3. An operator setting `adaptive_baseline_enabled: false` gets the old
   behaviour. The register remains accurate for that config.

## Register update

C1a, C1c, C1d should be updated to note:
- Measured 2026-09-06: the precondition (coherence at 0.0) is not reached by
  ordinary varied work on shipped defaults. Escalation did not occur.
- This does NOT prove de-escalation is reachable — ARM A never escalated.
- The adaptive_baseline_enabled flip (#176, 2026-08-29) is the decisive change,
  but it was accidental (S17 fix, not a de-escalation decision) and unprotected
  by any regression test.
- The entries remain valid for configs that set `adaptive_baseline_enabled: false`
  and for any workload that reaches zero coherence on defaults.
- Status: **precondition narrowed on shipped defaults; arithmetic unchanged**
