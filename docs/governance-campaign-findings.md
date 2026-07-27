# Governance campaign findings (living-script runs 7–23)

A debugging campaign against `examples/living-script_extended/`, driven by live
multi-agent runs on Gemini. Seventeen live runs, thirteen engine defects, four
proposals withdrawn on evidence.

This page exists because the reasoning was the expensive part and it was spread
across forty commit messages and seven test headers. The code is recoverable
from the repository; **why** each thing is the way it is was not.

Every row links a defect to the mechanism that caused it, the test that pins it,
and — separately — whether the fix has ever been exercised against a live API.
That last column is the one that gets assumed. A green stub test reads exactly
like verified behaviour until someone checks which live runs it actually fired
in.

---

## How to read the live-status column

| Status | Means |
|---|---|
| **confirmed** | Observed working on a live run, with before/after evidence |
| **inert-verified** | Live run shows it correctly declining to fire; the firing path is stub-only |
| **stub-only** | Passes its test; no live run has produced the triggering condition |
| **n/a** | Cannot be distinguished live by construction (e.g. same-default decoupling) |

---

## Engine defects found and fixed

### 1. The pulse counted scanned source, not agent turns

`consecutive_passes` drove the "suspiciously uniform" tripwire. Every
`recordPass()` site in the engine is a static-source, capability, plugin or
runtime-pin check — **there is no `recordPass` on the agent-behaviour path at
all**, because CDD, BSD, admission and output admissibility only call
`enforce()`, and only on failure. So the counter measured how much clean source
had been scanned.

`agent_governance_active_` did not contain it: that flag is a one-way latch set
at the first `agent.create()`, so every `codegen.run()` and polyglot block after
an agent existed fed ~5 uniform passes straight in. Runs 15–17 all ended
DEGRADED on codegen volume alone, with no agent misbehaviour anywhere.

- Fix `f6b6f49` — the counter ticks once per analyzed agent turn, resets on any enforcement
- Test `test_pulse_uniformity.sh` PU-01/PU-01b (pre-fix: streak 45 across 6 turns)
- **Live status: confirmed** (run 18: streaks 27/15/5 against a threshold of 50)

### 2. Pulse degradation was unattributable

A DEGRADED verdict named no cause. The six degradation signals were counted and
discarded; `PULSE_DEGRADED` went to the BSD detector, which surfaces only if a
pattern happens to match it. A degrade-then-recover between two
`governance.health()` samples was visible **only** as an unexplained two-step
jump in `governance_epoch`. Establishing that took four rounds of forensics
against a preserved run.

- Fix `f6b6f49` (`degradation_reasons`, latched so a sticky verdict keeps its cause), `85b29da` (`PULSE_TRANSITION` telemetry), `555b67d` (report the streak *before* the epoch boundary zeroes it)
- Test `test_pulse_uniformity.sh` PU-04, PU-05, PU-05b, PU-05c
- **Live status: confirmed** for the event (run 19 epoch arithmetic BALANCED on both segments; 3 transitions in run 21). **stub-only** for the streak field — no live transition since `555b67d` has carried a non-zero streak.

### 3. `thinking_collapse` scored a missing JSON field as agent behaviour

Gemini omits `thoughtsTokenCount` from `usageMetadata` on some responses. The
parser left `thinking_tokens` at its initialiser `0`, so *"the model did no
thinking"* and *"the API did not mention thinking"* arrived as the same value.

This was the single largest coherence drain in the campaign: 0.4858 over 10
turns in run 21, 0.64 over 11 turns and three OA quarantines in run 22 — all for
an absent field. Note `response_degenerate` (S23) gets away with
`output_tokens > 0` as its reported-check because zero output is essentially
impossible; **zero thinking is legitimate and common**, so S9 needed an explicit
flag rather than the same trick.

- Fix `dbf55a4` — `thinking_reported` travels parser → `NormalizedResponse` → `ProviderResponse` → `emitEvent` → `RuntimeEvent` → S9, which skips unreported turns rather than pushing them into the rolling mean
- Test `test_thinking_reported.sh` TR-01/TR-02/TR-03 (pre-fix: `unreported` scored identically to a genuine collapse — 14 firings, coherence 0.16)
- **Live status: confirmed** (run 23: zero firings on any agent; developer coherence floor 0.5175 → 0.83; quarantines 3 → 0)

### 4. `persona_fingerprint` borrowed another signal's window

S17 read `thresholds.thinking_history_window`, under a comment claiming that
value was "the coherence history" — which it also is not. That window decides
how many response keyword counts back the persona baseline, so tuning the
*thinking* signal silently moved the *persona* verdict.

- Fix `1e74f8d` — `persona_history_window`, same default
- Test `test_persona_window.sh` PW-01/PW-02 (pre-fix: thinking window 20 → 4 moved the persona baseline 49.60 → 52.75 on a byte-identical response stream)
- **Live status: n/a** — both keys default to 20 and no shipped config tunes them apart, so no live run can distinguish the states

### 5. Validation ground truth could be laundered by deleting tests

S22's fail→pass transition credits coherence back. That is the reward path for
test erosion: an agent that fails pytest, deletes the failing test, and passes is
paid for the deletion. A pytest exit code cannot separate the two, because **a
suite with no tests left passes**.

- Fix `62dcd3e` (a pass whose evidence count fell earns no recovery credit, and does *not* consume the outstanding failure — otherwise one shrink launders it away), `081f460` (a shrink is scored *as* an S22 failure, so an agent that never failed cannot erode for free; `last_evidence_count` survives `agent.reset()`, which the living script triggers on low coherence)
- Test `test_validation_signal.sh` Groups L and M
- **Live status: stub-only** — every live run reported monotonically non-decreasing counts, so nothing has yet presented the logic with a shrinking suite

### 6. Other engine fixes

| Defect | Mechanism | Fix | Test |
|---|---|---|---|
| Empty challenge response scored as incoherence | HTTP 200 with no content scores `-1.0` by construction; agent faces an unpassable challenge | `7a7d422` | `test_challenge_fail_path.sh` Group H |
| Entity challenge scored against a growing union | Union dilutes overlap toward zero for any evolving agent | `7c6320b` | Group I |
| Challenge denominator reported the sighting count | Off by an order of magnitude; a low ratio was unattributable | `8a6b00d` | Group I-04 |
| `agent.propose` gated on governance level | Level is scrutiny, not authorization; the error's own remedy was unsatisfiable | `3d9e5b6` | `test_propose_commit.sh` Group F |
| One noisy signal could kill alone | Coherence is a weighted sum; `semantic_stability` on varied phrasing killed a compliant agent at turn 8 | `8e16750` | `test_quarantine_corroboration.sh` |
| A pass erased an unscored failure | One-slot latch; a failed send left it unconsumed | `25a052f` | Group K |
| Per-agent prompt shape charged as drift | Harness prompt structure fired signals the agent never chose | `cd4a40a` | `test_per_agent_signals.sh` |

---

## Harness and orchestration fixes

These are not engine defects. They are places where the harness or the living
script reported something other than what happened.

| Issue | Fix |
|---|---|
| Live artifacts deleted on exit — every forensic question cost another API run | `409b9fe` (`KEEP_TMP=1`) |
| Assertions recorded outcomes without failing on them | `5d8857a` |
| "pytest did not run" reported to the engine as a test *failure* (absent key reads false) | `57d5032` |
| Refine loop validated and told the engine nothing — the phase where erosion actually happened | `3ed8196` |
| Stub startup picked one random port and gave up; a busy port read as a governance failure | in `081f460` |

---

## Recorded, deliberately not changed

Each of these is a real observation that did **not** justify a change. They are
here so the next person does not rediscover them and "fix" them.

**S9's baseline is frozen and never re-computed.** Re-baselining after a
sustained shift would stop a genuine collapse from ever paying — blinding the
signal to exactly the failure it exists to detect. That is the boiling-frog
property `rate_normalized_floor` was added elsewhere to prevent. The frozen
baseline only produced a permanent charge because it was being fed
non-measurements; with those removed it is correct.

**S17's baseline is captured during warm-up, and the deviation test is `abs()`
on a single response** — no rolling mean, unlike S9. An agent whose early turns
are short acknowledgements is judged for the rest of the run against its
warm-up, and one that later produces *more* pays like one that collapses. This
fired on five of six agents in run 23. It has never caused a quarantine or a
kill, so it is a calibration complaint, not a correctness defect.

**S17 is inert unless `adaptive_baseline_enabled` is true.** `persona_baseline_mean`
is gated on `baseline_complete`, which is only set inside the adaptive-baseline
block. At the engine default (off) the signal never fires at all.

**CDD cannot see test erosion from the response stream.** An agent emitting
fluent, on-mandate, non-repeating code while deleting its tests scores coherence
1.0 with zero signals fired under the *full* shipped signal set. Erosion reaches
the engine only through `agent.record_validation`'s evidence count. Pinned by
`test_developer_blindspot.sh` DB-04 so a future signal that catches it announces
itself.

**`keyword_stuffed` is detected but under-consequenced.** Off-topic substance
wearing the mandate's vocabulary scores 0.40 with 2 consecutive quarantines —
below a `max_quarantine_streak` of 3. Flagged every turn, never terminated.

---

## Proposals withdrawn on evidence

Kept because the reasoning against them is the useful part.

1. **Gate kills on `mandate_alignment` alone.** Two tests said it matched the
   full signal set — both used a single failure mode, and mandate_alignment is
   the signal built for that mode. `test_failure_mode_coverage.sh` across six
   behaviours showed `keyword_stuffed` and `repetition` are caught by the
   aggregate and missed by it alone.
2. **Score challenges on `max(recall, precision)`.** Would have made the probe
   fakeable — a keyword-stuffed answer passes.
3. **Rewrite the entity expected-set construction.** The live distribution
   refuted the premise: entity challenges scored `kr=1.000` at denominators of
   106–138, and one denominator produced both a pass and a fail.
4. **Re-enable `context_growth` on the developer.** Predicted it would fire where
   `thinking_collapse` fired, naming the real cause. Run 22: `context_growth`
   fired 4 turns and stopped, `thinking_collapse` fired 11 and continued, with
   one turn of overlap. The run terminated on advisory escalation with 23 checks
   never evaluated. Reverted in `9f63c4e`.

---

## Method notes

Three things this campaign kept relearning:

**Trace the code before running the experiment.** Every fix that came from
reading call graphs held up. Both hypotheses formed by reading a live run and
theorising were wrong — one cost a run, the other was an overstatement corrected
by the next run's data.

**Measure the thing, not a proxy for it.** `test_persona_window.sh` went through
three fixtures trying to make the signal *fire* so firing counts could be
compared — each iteration was tuning a scenario until it produced the expected
answer. Reading the baseline directly out of the CDD decision snapshot settled
it immediately, because the baseline is what the window actually controls.

**Make the test able to say "inconclusive".** `TR-04` skips with "announcement
may be redundant" when the two runs already differ; `PW-02` fails if the window
answers to no key at all, so a pinned value cannot pass by looking stable. A
test that can only pass or fail cannot tell you it measured nothing.
