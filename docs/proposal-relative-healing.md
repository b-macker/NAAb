# R5 — Healing relative to the agent's own damage rate

Status: **design, not implemented.** No code in this document has been run.
Every quantitative claim below is labelled by provenance: **traced** (followed
to the point of effect in the current tree), **observed** (measured in a run),
or **derived** (arithmetic from the design; a prediction, not a measurement).

## Why R5 exists

`coherence_natural_healing` is a per-turn constant. Whether it suppresses
escalation depends on the workload's own damage rate, which varies per agent,
per config, per signal set. R3 — raise the constant — was withdrawn on that
basis and re-examined and upheld in #172:

- **observed** (#172): the recovery gate first passes at rate 0.13; below 0.12
  an agent earns nothing across 25 clean turns.
- **observed** (#172): on a light profile, rate 0.30 suppressed escalation
  entirely — a drifting agent stayed at `normal`.

So the usable window for a constant is bounded above by suppression and below
by inertness, and both bounds move with the workload. There is no value that is
correct everywhere. R5 removes the constant instead of tuning it.

## What was traced before designing

All line numbers are `src/runtime/behavioral_sequence.cpp` unless noted.

1. **The healing block** (2216-2256). `want = coherence_natural_healing *
   heal_factor`; `granted = min(want, allowance, deficit)`; the grant is added
   to the raw score and the *realized* amount (post-floor) is booked.
2. **`heal_factor = 1.0 / (1.0 + signals_fired_this_turn)`** (2217). That
   counter is incremented at **22 sites**, every one gated behind `if (p > 0.0)`
   (e.g. 1056-1061). **Absorbed signals do not throttle healing** — a signal
   that fires but is zeroed by the adaptive baseline does not appear here.
3. **Damage booking** (2195-2200): `delta = coh_at_turn_start - max(0.0,
   score)`, channel-agnostic (net movement, so any future increase path is
   booked without being special-cased).
4. **`allowance = coherence_damage_total - coherence_healed_total`** (2219),
   debited by `resetCoherence` on the same ledger (2592-2599).
5. **Ratchet** (`governance_config.cpp:4098-4104`) covers three coherence keys;
   raising any of them is the loosening direction.

### The trace fact that shapes the design

Damage is measured **post-clamp** (the comment at `behavioral_sequence.h:385`
is explicit: penalties absorbed by the 0.0 floor are not counted). A floored
agent therefore books **zero** further damage however badly it behaves.

Consequence: the damage-rate window must be a window over **damaging turns
only**, never over all turns. A window over all turns would fill with zeros
while an agent sat floored, drag the mean toward 0, and make healing approach
0 exactly when the agent most needs it — reproducing the trap R5 exists to
remove, in a form that looks like it was fixed. This is not a detail that
survives being designed from memory.

## The change

```
D_mean = mean of the last N per-damaging-turn deltas    // damaging turns ONLY
base   = (f > 0.0) ? f * D_mean : coherence_natural_healing
want   = base * heal_factor                              // unchanged
granted = min(want, allowance, deficit)                  // unchanged
```

New config, both under `context_drift`:

| key | default | meaning |
|---|---|---|
| `coherence_healing_damage_fraction` (`f`) | **0.0** | 0 = off, absolute mode, byte-identical to today |
| `coherence_damage_window` (`N`) | 20 | how many damaging turns the mean spans |

`f = 0.0` by default, so this ships **inert**: no existing config changes
behaviour, and the 34 `governance_v4` tests that assert on coherence without
setting a healing key are untouched. Whether to default it ON is a **separate
decision requiring its own evidence**, deliberately not proposed here.

Only the selection of `base` changes. Booking, the ledger, the deficit cap and
the clamp are all untouched, so `test_bounded_healing.sh`'s conservation
invariant (BH-08, `coherence == 1 - damage + healed`) should still hold — it
must be re-run rather than assumed.

## Why it has no cliff — derived, not observed

**Recovery time.** Total recoverable is `allowance`, and `allowance ~
n_damaging * D_mean`. Healing per clean turn is `f * D_mean` (heal_factor = 1
when no signal fires). So

```
clean turns to full recovery  ~  (n_damaging * D_mean) / (f * D_mean)
                              =  n_damaging / f
```

The `D_mean` cancels. **Recovery time depends on how many turns the agent
drifted and on `f`, not on how hard it drifted.** That is the scale-free
property, and it is what "no cliff" means precisely.

**Suppression.** Suppression requires healing to outpace damage while drift is
ongoing.

- On a damaging turn with `k >= 1` signals firing: loses `D`, gains
  `f * D_mean / (1 + k) <= f * D_mean / 2`. With `D ~ D_mean`, net `<=
  D * (f/2 - 1) < 0` for any `f < 1`.
- Alternating one clean turn with one damaging turn: net per pair is
  `f * D_mean - D_mean = D_mean * (f - 1) < 0` for any `f < 1`.

**`f < 1` is the safety invariant.** At `f >= 1` healing matches or beats the
agent's own damage rate, the mechanism becomes pumpable, and the cliff returns
in a scale-free form — which is worse than the constant, because it would then
be wrong on every profile at once rather than some. `f` must be ratcheted and
should arguably be clamped.

## What the design forbids

A mechanism that forbids nothing has not been tested. This one forbids:

- **P1** Recovery time (in clean turns) is invariant under scaling the damage
  magnitude. Multiply a fixture's penalties by 3 and the clean-turn count to
  recover must not change. An implementation carrying any absolute term fails
  this.
- **P2** No `f < 1` suppresses escalation on any profile, however light.
- **P3** At `f >= 1`, suppression reappears.

P3 is not decoration. Without it, "no suppression at any tested `f`" is
indistinguishable from "the harness cannot detect suppression" — which is
exactly how the light-profile control failed in #172, where HIGH was
unreachable with healing off and the cell that mattered was vacuous.

## Acceptance gates, each with a demonstrated failure case

| gate | asserts | fails when |
|---|---|---|
| RH-01 | absent key is byte-identical to explicit `0.0` | the default is not what it appears |
| RH-02 | the `test_recovery_default.sh` fixture recovers at `f = 0.5` | recovery still unreachable |
| RH-03 | **P1** — penalties x3, same clean-turn count to recover | any absolute term survives in `base` |
| RH-04 | no suppression at `f < 1` on the light profile | healing outruns light drift |
| RH-05 | **POSITIVE CONTROL** — suppression *does* appear at `f = 1.5` | the harness cannot see suppression, and RH-04 proves nothing |
| RH-06 | alternating clean/dirty nets negative | the ledger is pumpable |
| RH-07 | raising `f` mid-run is a ratchet violation | the bound is re-grantable |
| RH-08 | **a floored agent still heals** | the window counts non-damaging turns and decays to 0 |

RH-05 and RH-08 are the load-bearing pair. RH-08 is the one that catches the
post-clamp trap described above; RH-05 is the one without which the whole
suppression argument is unfalsifiable.

## Regression surface

- **DriftState**: one `std::deque<double>` (existing convention — 12 such
  deques already), plus the window mean in `snapshotState` (2340-2341) so
  decisions stay replayable from preserved evidence.
- **`agent.reset()` must NOT clear the window.** Same reasoning as
  `last_evidence_count` for S22: scripts call reset when coherence drops, so
  zeroing the damage baseline would make healing self-cancelling exactly when
  it first matters.
- **Ratchet**: add `coherence_healing_damage_fraction` to the `coh[]` array at
  `governance_config.cpp:4098` (raising = loosening). `coherence_damage_window`
  is **not** obviously monotone — a shorter window is more reactive in both
  directions — so it should be treated as ratchet-any-change rather than
  guessed at, or left out with that reasoning recorded.
- **Threading**: both ledger fields are mutated only under
  `ContextDriftAnalyzer::mutex_` (held by `recordTurn` and `resetCoherence`);
  the new deque sits beside them and inherits that, with no new
  synchronisation.

## What R5 does not fix

- CDD remains blind to erosion arriving through the response stream; validation
  outcome (S22) is still the only channel for external ground truth.
- Coherence is still **consumed** present-tense while **computed** as
  worst-ever-sustained. R5 makes recovery possible; it does not make that
  semantics coherent.
- `test_recovery_default.sh` stays red until the default moves, which R5 does
  not propose. R5 makes a future default defensible; it does not set one.


---

## Implementation outcome (2026-08-26) — what measuring changed

Implemented. Two things the design got wrong, both found by running it.

### 1. The proposed positive control was unreachable

RH-05 was specified as "suppression reappears at `f >= 1.5`". The
implementation clamps `f` below 1.0, because that IS the safety invariant. The
clamp makes the proposed control impossible by construction, and asserting "no
suppression at any `f`" without a reachable positive case would have repeated
#172's vacuous control exactly.

RH-05 moved to the mechanism R5 replaces: absolute healing at 0.30 on the same
light profile, which #172 observed suppressing escalation. RH-09 pins the clamp
separately. **A safety clamp and a positive control can be in direct tension;
the clamp wins, and the control has to move.**

### 2. "Recovery" on the acceptance fixture was suppression all along

RH-02 and RH-08 failed on first run. The cause was not in the code.

On that fixture S17 `persona_fingerprint` fires **0.0500 on every recovery turn,
forever** — its baseline is set once from the five warm-up turns and the
recovery responses differ from them in keyword count (observed: turns 18-40,
that signal alone). Relative healing cannot climb through it, and the reason is
structural rather than a tuning failure: once the damage window fills with the
residual `r`, the rate IS `r`, so the grant is `f*r/(1+k) < r` for any `f < 1`.
The signal always outruns its own contribution to the healing rate.

The instinct was to redesign R5 — a decaying high-water reference that would not
collapse to the residual. **Measuring first showed that would have been building
the defect deliberately.** Absolute healing at 0.30 does climb through, and here
is what that looks like (observed):

```
turn 18  coh 0.1000   persona_fingerprint=0.05
turn 19  coh 0.2000   persona_fingerprint=0.05      +0.10/turn
...                                                  = 0.30*1/(1+1) - 0.05
turn 27  coh 1.0000   persona_fingerprint=0.05
turn 40  coh 1.0000   persona_fingerprint=0.05   <- pinned, 14 turns
```

Fourteen consecutive turns at PERFECT coherence while a penalty signal fires on
every one of them. Out-healing a live signal IS suppression; there is no version
of it that is recovery. **Refusing to heal while a signal fires is R5 behaving
correctly, and the gate was rewarding the opposite.**

So the gate moved, not the mechanism. Verified afterwards, with the control that
makes it mean anything:

| arm | min | final | level |
|---|---|---|---|
| `f=0.5`, S17 on | 0.0000 | 0.0000 | elevated |
| `f=0.5`, S17 off | 0.0000 | **1.0000** | **normal** |
| **no healing**, S17 off | 0.0000 | 0.0000 | elevated |
| absolute 0.30, S17 off | 0.0000 | 1.0000 | normal |

Row 3 is the control: removing the signal alone recovers nothing, so row 2 is
healing doing the work. **R5 works.**

### Consequence for the gate merged in #172

`test_recovery_default.sh` (RD-03/RD-04) is mis-specified for the same reason
and can only ever be passed by a suppressing mechanism. Its header now says so.
It stays red and stays in `GOVV4_SWEEP_SKIP`, but the justification recorded
with it — "the engine has no behaviour-only recovery path" — was wrong, and its
cited reassurance (`heal_factor` throttling) is precisely the bound absolute
0.30 overpowers.

### What the derived arithmetic missed

The proposal's `recovery ~ n_damaging/f clean turns` assumed `k = 0` and no
residual damage. With any persistently firing signal the term `1/(1+k)` and the
residual's own entry in the window both bind, and the result inverts from
"recovers in bounded time" to "cannot recover at all". Derived arithmetic is a
prediction; this is the case where it was confidently wrong in prose.
