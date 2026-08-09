# Proposal: bounded coherence healing

Status: **proposal, not implemented.** Every change below is a behaviour change,
and two of them are loosenings. This document exists so the decision is made on
traced facts rather than on the intuition that started it.

The originating question: *if an agent recovers, shouldn't coherence move too?*

---

## 1. What was traced

All of this is read out of the source, not inferred.

### `coherence_score` write sites

| kind | count | where |
|---|---|---|
| penalties (`-=`) | 23 | `behavioral_sequence.cpp`, one per CDD signal |
| temporal decay (`-=`) | 1 | `:796`, gated on `temporal_decay_enabled` |
| S22 validation credit (`+=`) | 1 | `:1458`, capped at 1.0 |
| natural healing (`+=`) | 1 | `:1985` |
| clamp to `[0,1]` | 1 | `:1989` |
| `resetCoherence` (`=`) | 1 | `:2279`, capped at `coherence_recovery_cap` |

### Read sites — every one is a live gate

| consumer | file:line |
|---|---|
| lease renewal floor | `governance_engine.cpp:6398` |
| output admissibility threshold | `:6517` |
| tool-call gating | `:6577` |
| `coherence_prox` in the pressure composite | `:7109` |
| drift threshold | `behavioral_sequence.cpp:2006` |

### Threading

`recordTurn` takes `std::lock_guard<std::mutex> lock(mutex_)` at entry
(`behavioral_sequence.cpp:780`), before any penalty or healing runs.
`resetCoherence` (`:2269`) takes the same mutex. `DriftState` is per-handle
inside `drift_states_`, guarded by that one mutex.

**Consequence for this proposal:** an accumulator stored on `DriftState` needs
no new locking, because both mutating paths already hold `mutex_`. This matters
because `agent.batch()`/`fan_out()` dispatch concurrently on a thread pool, so
several handles' `recordTurn` calls genuinely do run on different threads.

### Existing recovery is already live in three places

`recoverCoherence(handle_id)` → `resetCoherence(handle_id, coherence_recovery_amount)`:

| call site | trigger |
|---|---|
| `agent_impl.cpp:2332` | step-up challenge **passed** |
| `agent_impl.cpp:6213` | pipeline stage **failed** — comment: *"prevent floor-grinding"* |
| `agent_impl.cpp:6285` | pipeline stage **transition** |

That comment matters: **the floor-grinding problem is already recognised
in-tree.** The existing mitigation is ad-hoc — a flat `+0.2` at three specific
boundaries, with no relationship to how much damage was actually done, and
unavailable to any agent that is not in a pipeline and never passes a challenge.

### Ratchet coverage — a pre-existing gap

| key | default | ratcheted? |
|---|---|---|
| `coherence_natural_healing` | 0.0 | **no** |
| `coherence_recovery_amount` | 0.2 | **no** |
| `coherence_recovery_cap` | 1.0 | **no** |
| `validation_recovery_amount` | 0.075 | yes |
| `rate_normalized_floor` | 0.5 | yes |

Raising any of the first three mid-run reduces penalties, which is a loosening
by the same reasoning that ratchets the last two. This gap exists today and is
**widened by anything that makes healing matter more.**

---

## 2. The finding that changes the framing

The proposal started from "healing defaults to 0.0, so coherence only ever
falls". That is true of the engine default and was true of `living-script_v3`,
whose config omitted the key — **but 25 shipped configs set it**, including both
`govern-template.json` copies (0.02) and living-script v1 and v2 (0.03).

So the honest statement is not "healing is off". It is worse:

> **At the rates actually shipped, healing is inert.**

`heal_factor = 1.0 / (1.0 + signals_fired_this_turn)`, so one firing signal
halves an already-small number, while a single signal's penalty is 0.08–0.15.
0.03 × 0.5 = 0.015 against a penalty an order of magnitude larger. It cannot
win, and it is clamped at 0, so it leaves no trace of having tried.

Measured on `living-script_v3` (keyless, identical fixture, only the rate varied):

| `coherence_natural_healing` | inadmissible | final coherence | levels reached |
|---|---|---|---|
| 0.0 (engine default) | 30 / 36 | 0.000 | normal→elevated→high→elevated |
| **0.03 (v1 and v2 ship this)** | **30 / 36** | **0.000** | normal→elevated→high→elevated |
| 0.25 | 18 / 36 | 0.525 | normal→elevated→high→elevated→**normal** |

0.03 changes **nothing** — not one quarantine, not one thousandth of coherence.

This is the campaign's signature defect for the third time: a mechanism that is
configured, believed to work, and structurally incapable of affecting anything.
`CONTRA-007` and the circuit-breaker ladder were the first two. It differs only
in being a matter of degree rather than of reachability, which is why no
reachability sweep would have caught it.

### What it costs

In the v3 run, **22 of 22 turns in the recovery phase were quarantined** while
the agent produced clean, on-mandate, non-repeating work whose content is
indistinguishable from the DESIGN phase that passed. Once floored:

- lease renewal can never be met again;
- output admissibility rejects everything;
- coherence-correction is pinned at its most forceful tier;
- pressure has a hard floor of `0.35 (coherence_prox) + 0.10 (depth) = 0.45`.

A graduated system collapses into a single permanent state, and the gate stops
measuring the response in front of it.

---

## 3. Why "just raise the number" is not the proposal

0.25 restores full function — including `elevated → normal`, which has never
been observed — without preventing escalation. It is also **pumpable**: a clean
turn heals 0.25 (`heal_factor` 1.0) while a bad turn costs ~0.10, so an agent
alternating bad and good turns nets positive. That is precisely the boiling-frog
attack `rate_normalized_floor` exists to block.

The engine already contains the correct pattern, in S22:

> *credits ≤ failures — pass-spam cannot pump coherence*

Recovery is permitted, but total recovery can never exceed total damage.

---

## 4. The proposal

**One new field, one new rule.**

```cpp
// DriftState, beside coherence_score
double coherence_damage_total = 0.0;   // lifetime sum of penalties applied
double coherence_healed_total = 0.0;   // lifetime sum of healing granted
```

Both mutated only inside `recordTurn` and `resetCoherence`, both of which
already hold `mutex_`. No new lock, no new ordering.

**Rule:** healing granted on a turn is
`min(coherence_natural_healing * heal_factor, coherence_damage_total - coherence_healed_total)`.

Consequences:

- an agent that never drifted has a zero ledger and heals nothing — healing
  cannot inflate a healthy agent's score;
- an agent that drifted can recover, at most, exactly what it lost;
- alternating bad/good turns nets **zero**, not positive: the ledger grows by
  the penalty and the healing is capped by the same amount;
- a floored agent that behaves cleanly for long enough returns to function,
  which is the property being asked for.

`min_coherence_lifetime` continues to record the worst point ever reached, so
nothing is forgotten — the permanent record and the live gauge stop being the
same variable, which is what the existence of both implies was intended.

**`resetCoherence` must debit the same ledger.** It grants a flat `+0.2` at
three call sites with no reference to damage. If it does not participate,
bounded healing is bypassable by any agent in a pipeline, and the bound is
decorative.

---

## 5. Regression surface, per change

Not additive. Each item is a distinct behaviour change with its own blast
radius.

| # | change | direction | who is affected |
|---|---|---|---|
| R1 | ledger fields + bound on `:1985` | **tightening** for the 25 configs that set healing today (healing becomes capped); no-op for configs at 0.0 | the 25 configs, all currently at 0.01–0.05 where healing is inert anyway, so the practical delta is ~0 |
| R2 | `resetCoherence` debits the ledger | **tightening** — pipeline agents and challenge-passers get less than a flat +0.2 once their ledger is exhausted | anything using pipelines or step-up challenges |
| R3 | raising the shipped default rate | **loosening** — penalties partially refunded | every config; would need the ratchet first |
| R4 | ratchet the three keys | tightening | mid-run reloads that raise them; none known |

**R3 is the one that actually changes outcomes, and it is the loosening.**
R1 and R2 make healing *safe*; only R3 makes it *matter*. They should not be
bundled: shipping R1+R2+R4 is defensible on its own and leaves the rate decision
open.

### Specific breakage to expect

- **`living-script_v3`'s own gates.** V3-04/V3-05 require ELEVATED and HIGH to
  be reached. Verified at 0.25 they still are — the ladder climbs and then
  returns — but any rate high enough to prevent the initial escalation breaks
  the scenario, and the fixtures were generated at 0.0. Fixtures must be
  regenerated with whatever rate ships, and V3-11's negative fixture re-derived.
- **6 test suites set `coherence_natural_healing`** and 54 mention coherence.
  Those setting it to 0.0 explicitly (`naab-31`, `naab-32`, `naab-33`,
  `naab-43`, `naab-53`, `naab-31_b`, `governance_v4/depth`) are asserting the
  monotonic behaviour and are the ones to check first.
- **`tests/gorilla/naab-38/govern-agent.json` sets 0.05**, the highest shipped
  value — the most likely to notice R1's cap.

---

## 6. Tests, with the degradation each must fail

Same discipline as `test_inert_limits.sh` and the v3 gates: every assertion
ships with the mutation that must break it.

| gate | claim | degradation that must fail it |
|---|---|---|
| BH-01 | a never-drifted agent heals nothing | remove the ledger bound → healing accrues on a clean agent |
| BH-02 | a drifted agent recovers | set the bound to 0 → no recovery |
| BH-03 | recovery never exceeds damage | remove the `min()` → coherence exceeds its own loss |
| BH-04 | alternating bad/good nets ≤ 0 | grant healing before debiting the ledger |
| BH-05 | `resetCoherence` debits the ledger | revert R2 → pipeline agent recovers past its damage |
| BH-06 | escalation still reachable | raise the rate until ELEVATED is never hit |
| BH-07 | `min_coherence_lifetime` still records the floor | let it follow `coherence_score` back up |

BH-03 and BH-04 are the load-bearing pair: without them the whole proposal is
just "raise the number", which is the thing being argued against.

---

## 7. Recommendation

Ship **R1, R2 and R4** — bounded healing, a participating `resetCoherence`, and
the ratchet. Together they change almost nothing today (healing is inert at
shipped rates) and make the knob safe to turn.

Hold **R3**, the rate, for an explicit decision. It is the loosening, it is the
one with real blast radius, and the v3 measurement gives a defensible starting
point (0.25 restores `elevated → normal` without preventing escalation) rather
than a guess.

What is **not** proposed: enabling healing by default at a rate that matters, in
the same change that makes it bounded. That would be a behaviour change hidden
inside a safety improvement — the pattern this campaign has now rejected three
times, in default-on secret scanning, `restrictions.*.enabled`, and the inert
`limits.data` keys.

---

## R3 — WITHDRAWN ON EVIDENCE (2026-08-09)

R3 was "raise the shipped healing rate", held back as the loosening while
R1/R2/R4 shipped. It is now withdrawn, because measuring it properly showed
there is no safe absolute number to raise it to.

### The re-measurement

The original evidence for R3 — 0.25 restores `elevated → normal` — was taken
**before the ledger existed**, so it had to be redone rather than carried over.
It reproduced, and the bound saturates exactly (healed == damage):

| rate | inadmissible | final coherence | damage / healed | levels |
|---|---|---|---|---|
| 0.0 | 30/36 | 0.000 | 1.00 / 0.00 | …→high→elevated |
| 0.03 (shipped) | 30/36 | 0.000 | 1.10 / 0.48 | …→high→elevated |
| 0.25 | 18/36 | 0.396 | 3.70 / 3.70 | …→high→elevated→**normal** |
| 0.45 | 7/36 | 0.375 | 4.16 / 4.16 | …→high→elevated→normal |
| 0.48 | 7/36 | — | — | …→high→elevated→normal |
| **0.50** | 6/36 | 0.443 | 4.35 / 4.35 | normal→elevated→normal — **HIGH never reached** |

Runs are deterministic: 0.25 gave identical output three times, so these are
single-run numbers that can be trusted.

Two things the ledger changed that are worth recording. 0.03 is no longer
byte-identical to 0.0 — it now books 0.478 of healing where previously the
clamp swallowed it — though the *outcome* is unchanged, so "inert at shipped
rates" survives as a statement about behaviour. And `damage` is path-dependent,
growing from 1.00 to 4.35 as the rate rises, because each recovery gets
re-damaged. The bound is therefore "cumulative recovery never exceeds
cumulative harm", not "you may only ever recover 1.0" — and alternating one bad
turn with one good one still nets exactly zero, so it remains unpumpable.

### Why there is no number to pick

The cliff at 0.50 looked like a wide safe band with comfortable margin. It is
not a fixed cliff. Re-running the same scenario with two CDD signals disabled on
`drift_worker` — less damage per turn, nothing else changed:

| scenario | HIGH lost at |
|---|---|
| v3, full drift | 0.50 |
| v3, two signals disabled | **0.15** |

**The cliff is damage-relative.** Healing suppresses escalation once it outpaces
the scenario's own damage rate, and that rate varies with which signals fire —
which varies per agent, per config, per workload. A default of 0.25 is
comfortable for one profile and silently prevents HIGH for a lighter one.

Silently is the operative word: an agent that never escalates looks exactly like
an agent that behaved. That is the failure mode this entire campaign exists to
catch, and shipping a fixed default would have manufactured it.

### What replaces R3

Nothing, for now. The rate stays where it is and the finding is documented:
**`coherence_natural_healing` must be tuned relative to a workload's damage
rate, and cannot be set correctly as a global constant.**

The principled fix is a different change — make healing relative rather than
absolute, e.g. capped at a fraction of the agent's own recent damage per
damaging turn, so it self-scales and has no cliff to find. That is new design
with its own regression surface and is not proposed here. Tracked as R5 in
`docs/open-investigations.md`.
