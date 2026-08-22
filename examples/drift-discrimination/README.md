# Drift-discrimination experiment

**Question:** does the engine tell correct work apart from real drift?

## Why a fixture and not a live model

A live model cannot answer this. With sampling, every configuration arm receives
different responses, so you measure model noise instead of the engine. With
greedy decoding the model never breaks out of a repetition loop, so the recovery
phase never happens — which is precisely how the first attempt at this failed:
all four arms came back identical because the phenomenon under test had been
removed by the constraint that made the arms comparable.

A recorded sequence is deterministic *and* contains the phenomenon, because it
was put there.

## The three phases

| phase | turns | content |
|---|---|---|
| WARMUP | 1–5 | varied, on-mandate, **correct** |
| REPETITION | 6–13 | byte-identical responses — unambiguous drift |
| RECOVERY | 14–25 | varied, on-mandate, **correct** again |

Two correct phases bracketing the drift is what makes it decisive. A signal
firing at similar rates in WARMUP and REPETITION does not discriminate; one
firing *more* in the correct phases than the drift phase is **anti-correlated**
with what it exists to detect.

## Running it

```
bash examples/drift-discrimination/run.sh
```

Exits non-zero only if the experiment itself is invalid — the run did not
complete, or the fingerprint signals stopped detecting the repetition. Those
signals are the **positive control**: without them firing, "signal X fires on
correct work" is equally well explained by a broken fixture, and no row in the
table can be read.

It deliberately does **not** assert that every signal discriminates. Three of
them currently do not, and a permanently-red gate teaches people to ignore it.

## Result as of this commit

The same fixture, the same signals, one config key apart.

### `adaptive_baseline_enabled: false` — the engine default

```
signal                  WARMUP (correct)   REPETITION (drift)  RECOVERY (correct)
circular                    0%                   87%                  0%
response_repetition         0%                   87%                  0%
instruction_recall        100%                    0%                100%
entity_consistency         80%                   12%                100%
semantic_stability         60%                   12%                100%
mandate_alignment          20%                    0%                  0%

coherence   WARMUP 0.810 -> 0.000   REPETITION 0.000 -> 0.000   RECOVERY 0.000 -> 0.000
```

Coherence is destroyed across five turns of **correct** work, before the drift
phase begins.

### `adaptive_baseline_enabled: true`

```
coherence   WARMUP 1.000 -> 1.000   REPETITION 1.000 -> 0.000   RECOVERY 0.030 -> 0.360
```

Coherence holds through correct work, collapses during drift, and recovers.
That is the behaviour the signals are supposed to produce.

**The per-signal firing rates barely move between the two runs.** The signals
fire just as invertedly either way. What changes is whether a firing *pays*:
adaptive baselining learns each agent's normal signal rate and absorbs it. The
raw firing rate is not the engine's decision variable, and reading the table as
though it were is the mistake this experiment originally made.

## A correction, and what it cost

An earlier version of this README reported the first table alone and concluded:

> the inversion is a property of those signals, not of a particular scenario or
> model

That was wrong, and wrong by exactly one unset config key. This experiment's
`govern.json` set no `adaptive_baseline_enabled`, the engine default is
`false`, and every measurement taken here — and every conclusion drawn from
them across the wider campaign — was therefore taken with the engine's
calibration switched off.

The chain of conclusions that followed, each overturned by the next level of
tracing: *the signals are inverted* → *no lexical metric can separate correct
work from drift* → *the signals should be removed* → *the test protecting them
is confounded* → **the signals work; the default does not enable them
properly.**

Nothing in that chain was caught by a failing test. Every vacuity check passed
at every step, because vacuity checks establish that a harness is honest about
what it measures, not that it measures the right quantity.

## What this does and does not establish

**Does:** the uncalibrated engine penalises correct work of every lexical shape,
hard enough to floor coherence before any drift occurs. That reproduces exactly,
on demand, with the drift detectors firing correctly in the same run.

**Does:** adaptive baselining converts the same signals into a working
detector on this fixture.

**Does not:** say whether `adaptive_baseline_enabled` should default to true.
Baselining absorbs, and on a short run relative to its window it absorbs
everything — including the fingerprint signals that are the positive control
here. `rate_normalized_floor` and `adaptive_absorption_limit` exist for that
risk. Changing a shipped default needs its own measurement across run lengths.

**Does not:** cover the parroting case. An agent that restates its mandate and
does no work holds coherence at 1.000 in **both** modes — see
`tests/governance_v4/test_signal_contract.sh`, contracts C3 and C4.
