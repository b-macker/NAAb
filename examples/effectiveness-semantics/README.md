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

**A and B have no solution anywhere in the search space.** Not at any offset, not
at any window, against every scenario count tried (3, then 4). That is the strong
result and it is a negative one: the current definition is not mistuned, it is
measuring a quantity that saturates, and no parameter choice repairs it.

Concretely, on a scenario where the agent genuinely recovered, the current
definition returned **−0.246** — confidently wrong in the dangerous direction,
reporting a successful intervention as a failed one.

**The parameter question is not yet answerable, and the harness says so.** Of ten
scenarios: five were mislabelled by me and excluded, two more had
horizon-dependent ground truth and were excluded, one is a control that
correctly never escalated. The two survivors are both NEGATIVE, so a definition
that answered NEGATIVE every time would score perfectly. The harness refuses to
compare rather than report that.

## The finding underneath

**No POSITIVE scenario survived validation.** Every "helped" variant measured as
ZERO or NEGATIVE, and the reason generalises past this experiment:

- the pre-window, anchored at the escalation turn, spans back into the
  pre-drift period where the penalty rate is 0, so "before" looks good;
- the post-window still contains the agent's response lag, so "after" looks bad.

Both push a genuine recovery toward "no change" or "worse". **The anchor is
wrong, not only the quantity** — a stronger claim than "use penalty rate instead
of coherence", and one that applies to any measure taken relative to the
escalation turn. A usable definition probably has to anchor on when the drift
began, or on when the behaviour changed, rather than on when the engine reacted.

## What would make this answerable

Scenarios in which a recovery is measurable at all: a drift period long enough
to dominate the pre-window, and a response lag short enough to leave the
post-window. Until then the parameter choice is unsupported, and this directory
sets no constants.

## Running it

```
bash examples/effectiveness-semantics/run.sh
```

Reports; does not gate. `KEEP_WORK=1` retains the run directory.
