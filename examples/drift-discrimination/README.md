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

```
signal                  WARMUP (correct)   REPETITION (drift)  RECOVERY (correct)
circular                    0%                   87%                  0%
response_repetition         0%                   87%                  0%
instruction_recall        100%                    0%                100%
entity_consistency         80%                   12%                100%
semantic_stability         60%                   12%                100%
mandate_alignment          20%                    0%                  0%
```

Two signals discriminate perfectly. **Three are inverted** — `instruction_recall`
most starkly, firing on every turn of correct work and never once during drift.

Coherence reaches **0.000 before the drift phase begins**, destroyed across five
turns of correct work, with natural healing enabled. The three inverted signals
cost roughly 0.3 per turn against a healing rate of 0.03 ÷ (1 + signals fired).

## What this does and does not establish

**Does:** the inversion is a property of those signals, not of a particular
scenario or model. It reproduces exactly, on demand, with the drift detectors
firing correctly in the same run as the control.

**Does not:** say what the inverted signals *should* measure. All three score
inter-turn variation, so they penalise a correct agent moving between sub-tasks
and go quiet when it repeats itself. Deciding what replaces that is a design
question, not a defect fix — see `docs/open-investigations.md` C1d.
