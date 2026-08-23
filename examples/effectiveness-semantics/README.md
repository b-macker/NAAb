# Effectiveness semantics

**Question:** in what quantity should "did escalating help?" be measured?

`escalation_effectiveness` is the engine's only record of whether one of its own
interventions worked — mean coherence over the N turns after an escalation,
minus coherence at the escalation. Nothing consumes it. Before wiring it to a
decision, the definition has to be right, and that is a semantics question.

A first pass answered it by argument. This answers it by measurement.

## What it does

Ten scenarios with intended verdicts, run against the real engine. Ground truth
is then **re-derived from the measured per-turn penalty total** either side of
the escalation — the one quantity that does not saturate at the coherence floor
— and any scenario whose measurement contradicts its label is **excluded rather
than scored**. A mislabelled scenario silently rewards whichever definition
shares its error.

Four candidate definitions are then swept over offset 1–6 × window 2–8:

| | definition |
|---|---|
| A | mean(coherence post) − coherence at escalation — **the current one** |
| B | coherence slope across the window |
| C | mean(penalty pre) − mean(penalty post) |
| D | the same, normalised by the pre-window rate |

## Results

**A and B have no solution anywhere in the search space** — not at any offset,
post-window or pre-window, across 252 points and every scenario count tried.
That is the strong result and it is negative: the current definition is not
mistuned, it is measuring a quantity that saturates, and no parameter choice
repairs it. On a scenario where the agent genuinely recovered it returned
**−0.246**, reporting a successful intervention as a failed one.

**C and D each have 82 solutions**, and the *shape* of that set is the finding:

| axis | solutions span |
|---|---|
| offset | 1–6 — every value |
| post-window | 2–8 — every value |
| **pre-window** | **1–2 only** |

Indifference on two axes and a hard constraint on the third is what a real
constraint looks like, as opposed to a lucky point. The measurement needs the
**pre-window to be the triggering drift** — the one or two turns between drift
onset and the engine reacting — and does not much care how long it then watches.

## The finding: the anchor, not just the quantity

A first pass concluded that no POSITIVE scenario could survive at all, and that
recovery was structurally unmeasurable. That was wrong, and wrong because of the
analysis rather than the engine: the derivation used a **symmetric** window
either side of the escalation.

Escalation fires ~2 turns after drift onset. So a symmetric 8-turn pre-window
reaches back into the clean period, "before" looks good, and a genuine recovery
reads as no-change or worse. Anchoring the pre-window on drift onset — and
decoupling it from the post-window — turned two scenarios from "mislabelled" to
correctly POSITIVE and produced the result above.

Both the quantity and the anchor were wrong, and the anchor was the one hiding
the answer.

## What this does not settle

Four usable scenarios covering three verdicts. Better than the degenerate two
that an earlier run produced, and still small. Five scenarios remain excluded as
mislabelled by me — in each case the measurement was right and my intent was
wrong, which is why ground truth here is re-derived rather than declared.

This directory reports. It sets no constants in the engine.

## Next

The constraint is on the pre-window, so the open question is whether pre-window
1–2 holds when drift onset and escalation are further apart than the ~2 turns
these scenarios produce. A config with a higher `elevated_threshold` would widen
that gap and is the cheapest way to test it.

## Running it

```
bash examples/effectiveness-semantics/run.sh
```

Reports; does not gate. `KEEP_WORK=1` retains the run directory.
