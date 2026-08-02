# Governance campaign findings (living-script runs 7 onward)

A debugging campaign against `examples/living-script_extended/`, driven by live
multi-agent runs on Gemini. Two phases so far: the **coherence phase** (runs
7–23, thirteen engine defects, four proposals withdrawn on evidence) and the
**transition-admissibility phase** that followed it, which asked whether the
system can prove an inadmissible action never became real.

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

## Transition-admissibility phase

The coherence phase asked whether the engine could *tell* that an agent had gone
wrong. This phase asks the harder question the engine claims to answer: **can it
prevent an inadmissible action from becoming operationally real, and prove that
it did?** Proving a negative needs a control — an absence is only evidence when
the same test shows the action forming under admission — so every level here is
paired, and `test_nonformation_proof.sh` Group A exists to give Group B meaning.

Three of the six defects below were found by *writing the proof* rather than by
reasoning about the code — three of those in the evidence layer itself, which
had been running unverified. The rest came from tracing the propose/commit path
after #105 established the invariant they violate.

Two of the six (#11 and #12) are the same defect class: **chained events written
without the count keeping up**, so the verifier accuses an untouched file. They
were found six days and one wrong conclusion apart.

### 7. Every signed attestation named no key

`ed25519Fingerprint()` reads with `PEM_read_bio_PUBKEY` and deliberately refuses
private keys. All three `emit*Attestation` sites handed it the **signing** key,
so fingerprinting threw into the surrounding catch and left `key_fingerprint`
as `""`. Every signed attestation NAAb had ever written carried a valid
signature that could not be attributed to a signer.

The assertion merged one PR earlier could not have caught it: it grepped
`'"key_fingerprint":"'`, which also matches an empty value. That is the defect
class this phase kept producing — **an assertion that looks specific but is
satisfiable without the property holding** (see *Method notes*).

- Fix `1e6924b` — fingerprint the derived public half via `ed25519PublicFromPrivate()`; the value now equals what the trust store reports for the same key
- Test `test_nonformation_proof.sh` C-05 (tightened to require hex content), living-script `L24-03`
- **Live status: confirmed** (keyed run: 22 signed attestations, 0 empty fingerprints)

### 8. A lease could lapse in the gap propose/commit exists to create

`agent.propose()` gated the standing lease; `agent.commit()` did not gate it at
all. Deliberation time is the entire point of the split, so a wall-clock lease
could expire inside it and the commit landed anyway — `agent.send()` refusing the
same handle at the same instant while commit advanced the turn and appended
history.

Authority is time-varying and a proposal is not, so the check belongs on the
commit half. Only the wall-clock half can fire there: turns cannot advance
between propose and commit, so a turn-based lease that passed propose is still
valid at commit **by construction** — G-02 holds that claim honest rather than
assuming it.

- Fix `dfd443e` — `leaseExpiredLocked()` inside commit's existing `s_agent_mutex` block; no new lock, no lock-ordering change
- Test `test_propose_commit.sh` Group G
- **Live status: stub-only** — no live run has yet let a lease expire mid-deliberation

### 9. Propose was the one entry point where expired authority passed silently

Two holes in the same invariant. `agentPropose`'s lease test sat inside
`if (cb.step_up_enabled)`, which defaults to **false** — so by default propose
did not consult the lease at all, while send hard-throws on expiry and commit
now checks unconditionally. And a candidate generated under one configuration
could still be committed under another, because commit deliberately does not
call `reloadIfChanged()` (an accepted reload re-baselines `mandate_keywords`
and would score a candidate against a mandate it never received).

The stamp is `reload_count_`, not `governance_epoch_`: the epoch also moves on
pulse verdict transitions, and the pulse sawtooths, so proposals would be voided
during normal degraded operation. The change was **not additive** —
`reload_count_` was a plain `int` read unsynchronised by `getReloadCount()`,
which would have been a data race once agent worker threads read it during
`batch`/`fan_out`. It is now `std::atomic<int>`.

- Fix `f357275`
- Test `test_propose_commit.sh` Groups F and G
- **Live status: inert-verified** — the propose/commit path does run live (keyed runs record commit attestations alongside send attestations), and no reload has yet landed inside a deliberation gap, so the refusal itself is stub-only

### 10. Naming a function `validate_*` does not clear taint

`checkRhsSanitized()` (`governance_taint.cpp:204`) clears taint only when a
sanitizer call is the **right-hand side of an assignment**. Being *inside* a
function whose name matches `validate_` does nothing for values that function
writes internally — which is exactly why `validate_python_tool()` violates: it
writes unvalidated LLM code to disk and AST-parses it **afterwards**. That is a
true positive, and calling it sanitized would be a lie.

Two corrections to the record came out of the same run. The escalation this
change was justified by **never fired**: zero `ESCALATED` lines at 18
violations, because epoch boundaries reset advisory history
(`decayAdvisoryHistory()`) and the occurrence counter peaked at 5 of 8. The
sanitizer boundary is the honest model of the trust boundary; it is not
breakage avoidance, and `L25-03`'s comment now says so.

- Fix `142c015` — 16 build-path extraction sites routed through `sanitize_llm_code()`
- Test living-script `L25-01..03` (control write proves taint is watching; the pass condition is a documented count baseline, because the violation message names the sink *type* and *source* line, never the write target)
- **Live status: confirmed** — build path contributed **zero** violations across all feature iterations in two keyed runs

### 11. The chain verifier reported tampering on files nobody had touched

`emitEndOfRunHealthWarnings()` writes chained telemetry from its own code path
rather than through `writeAgentTelemetry()`. It seeded `prev_hash` from the
in-memory `last_telemetry_hash_` and never incremented
`chained_events_this_run_`, so:

- `RunEnd` under-declared its count and `--verify-telemetry-chain` reported a
  hard **BREAK** — "declares 2 but 4 observed" — on an untouched file;
- when the warnings were a process's first chained events the in-memory hash was
  empty, so `prev_hash` fell back to genesis mid-file (spurious **LEGACY
  RESTART**) and the lazy `RunStart` anchor landed *behind* the events it was
  supposed to anchor.

A verifier that cries tamper on its own output is worse than no verifier: it
trains the reader to ignore the one signal that is supposed to be unignorable.
The bug was found by `L24-06` — the level added in #106 specifically because the
example had been building a chain nothing ever verified.

The unit was `2` per affected run, matching the two inert-instrumentation
warnings (CDD and BSD enabled with no agent activity).

**I concluded from that** that the originally-reported gap of 53 was an
artifact of aggregating several run groups, and that this writer was the
complete explanation. That was wrong, and the way it was wrong is worth
keeping: every large run in the data I had balanced exactly (628/628,
485/485), which is consistent with "one writer, unit 2" — but only because
none of those runs tripped the quality gate. The second cause (#12) was
invisible in that sample, not absent from the system. A hypothesis that
explains all the data you have is not thereby the whole mechanism.

- Fix `4dfd38b` — `chainPrevLocked(fp)` plus the counter increment, inside the lock the lambda already held; every other chained writer already did both
- Test `test_evidence_chain.sh` Group E (E-01 is the control: without warnings actually firing, E-02..E-04 would be vacuous)
- **Live status: confirmed** — the next keyed run put every small run group at `diff 0`, exactly where the health warnings fire

### 12. A run that reported twice was counted once

`writeReports()` is called from ~17 sites, and a clean `execute()` is not the
last of them: `main.cpp`'s contract-error, quality-gate and baseline-regression
exits all call it again after the VM has already written and sealed the run.
The five file-based report formats are idempotent (they truncate and rewrite),
but telemetry **appends**, and `check_results_` is **never cleared** — so the
second call re-emitted the entire result set after `RunEnd` had already declared
the count. `run_end_emitted_` then prevented the declaration from ever catching
up.

On a live run: `RunEnd declares 737 chained events but 785 observed` — the
residual being one full second dump (`GovernanceCheck` 38 + `RuleViolation` 8 +
`GovernanceCheckSummary` 1 + `ScoringSnapshot` 1 = 48). Someone had already
noticed the double-call and worked around it in exactly one place; the comment
`// writeReports() already called inside execute() on success` sits three lines
above two more call sites that do it anyway.

Two consequences, and the second is the one that would have gone unnoticed:
the verifier reported tampering on an untouched file, **and** every count
derived from the telemetry file was inflated — including `L25-03`'s taint total,
which had been reading roughly double the true number of violations.

The fix makes the invariant self-healing rather than patching this instance:
`RunEnd` is re-emitted whenever chained events followed the previous one (the
verifier reads the last `RunEnd` per `run_id`, so the newest declaration wins),
and the dump resumes from an index instead of restarting, so results are
written once.

- Test `test_evidence_chain.sh` Group F (F-01 is the control: unless the quality gate actually fires, `writeReports` ran once and F-02..F-04 prove nothing)
- **Live status: confirmed.** Reproduced in miniature (pre-fix 10 records / 13 chained / declares 8 / BREAK; post-fix 5 / 8 / 8 / clean), then on a keyed run: all four run groups at `diff 0`, including a 708-event group carrying **two** `RunEnd` anchors — the reconciliation firing, with the second declaring 708 of 708. That group reached the second `writeReports()` through an exit path other than the quality gate, so the invariant held somewhere the constructed repro never went.

Two things the same run settled about the *taint* count, which this defect had
been inflating. `L25-03` measured 7, 8, **18**, 8 across runs, and the 18 is the
single run where the double-dump fired. The apparent run-to-run variance was the
artifact; a violation fires about once per distinct sink site, so the total
tracks the ~8 sites rather than the length of the run. An earlier comment in
`run.sh` reasoned that the count "is NOT monotone in run length" and declined to
tighten the baseline on that basis — correct to decline on two points, but the
premise was reading the artifact as signal. With inflation impossible the
baseline moved 18 → 12.

### 13. A suspended action could still become real

`checkAdmission()` denies at CRITICAL with a HARD block — *"all autonomous
actions suspended until pressure subsides."* `agent.propose()` is gated on it.
`agent.commit()` re-checked the handle, `max_turns`, the lease (#8) and the
config generation (#9) — but never the level. Commit is where the transition
becomes real: history appended, turn advanced.

What makes it more than theoretical is that the proposing handle need do nothing
at all. `governance_level_` is a single **engine-global** atomic written from CDD
processing, driven by whichever handle last took a turn; `s_pending_proposals` is
keyed **per handle**, so only that handle's own activity invalidates its
proposal. A *sibling* agent's misbehaviour therefore escalates to CRITICAL across
the deliberation gap while the proposal stays valid — which is exactly the shape
`batch`, `fan_out` and pipelines run in.

The reasoning that justifies commit's other omissions does not extend here, and
that is the trap: `hard_stopped` and `recordAutonomousAction` are omitted because
they guard *spend* and commit makes no API call. CRITICAL is not a spend guard.
Hence `checkCriticalSuspension()` — the CRITICAL branch split out of
`checkAdmission()` — rather than calling `checkAdmission()` wholesale, which
would also project an autonomous action against the exposure limit and refuse a
commit that costs nothing.

- Test `test_propose_commit.sh` Group J (J-02 is the control: without escalation the same commit must succeed, or J-01 shows only that commits fail)
- **Live status: stub-only** — no live run has yet reached CRITICAL while a proposal was outstanding

Two things the test got wrong first, both worth keeping:

**The staging is two gates in series.** `consecutive_high_pressure_turns` only
increments while `composite >= reality_checkpoint.pressure_threshold`, and the
CRITICAL target needs `composite >= critical_threshold && consecutive >=
critical_sustained`. Setting only the circuit-breaker half pins the counter at
zero and the level never moves — the first version reported "staging never
reached CRITICAL", which is the control working rather than the fix failing.

**The second version passed with the fix reverted.** The sibling sent twice, and
the *second* send's own `checkAdmission()` hard-blocked once the first had
escalated: the process died before the commit, stderr still said CRITICAL, and
the assertion matched a blocked sibling send rather than the gate under test. One
send plus a `PRE_COMMIT` marker fixed it. Third time in this campaign that a
detection method carried the very defect it was hunting.

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

## Taint tracking: two defects that hid each other

Found by tracing the taint tracker cold, after the propose/commit path was
exhausted. Taint is implemented **twice** — the tree-walker walks the AST
(`expressionContainsTaint`), the VM carries it on `taint_stack_` — and nothing
checked that the two agree. A differential run over 12 scenarios found two
divergences, in opposite directions, each masking the other.

**14. The declaration after a taint source inherited its taint.**
`checkRhsTainted()` clears `lastReturnWasTainted` on entry, then *sets it again*
when it identifies a direct source, so `VarDeclStmt` can read `lastTaintSource()`
for lineage. Nothing cleared it afterwards, so the next declaration's step-3
check picked it up:

```naab
let t = env.get("HOME")      // legitimately tainted
let c = "a clean literal"    // <- silently tainted by the stale flag
```

A false positive on clean data, which makes every taint count untrustworthy —
including `L25-03`'s baseline. It presented intermittently: with more statements
in between, the stale flag lands on a throwaway variable that never reaches a
sink. The identical hazard was already fixed for `ExprStmt` (`V13-S7`); the
declaration path was missed.

**15. `try` as an expression laundered taint.** `expressionContainsTaint()` did
not handle `TryCatchExpr`, so `let x = try { tainted } catch (e) { "" }`
produced an untainted `x`. The walk returns **false for any node type it does
not handle**, so this is fail-open by construction — `YieldExpr` was missing too.

**They cancelled.** Defect 14 was tainting the try-expression anyway, so 15 was
invisible; a test of the try case passed *because of a bug*. Fixing 14 exposed
15 as a fresh divergence. The vacuity check shows it directly: with both
reverted `try_expr` **passes**, and it only fails once the stale-flag fix is in.

**Why nothing caught them.** `tests/differential/` exists precisely to catch
VM/tree-walker divergence, but `diff_runner.py` runs `--no-governance` — taint
is switched off there by construction. The property suite's
`test_taint_monotonicity.naab` runs on one engine, so a divergence is invisible
to it too. `test_taint_engine_parity.sh` closes the gap, and its pass condition
is agreement **plus** a per-engine expectation: two engines that both miss a
taint agree perfectly.

**A retraction.** A third finding — "the tree-walker misses inline subscripts at
sinks" — was wrong, and was a measurement artifact. Violations print in two
formats (`checkTaintedSink` names the variable; `checkExpressionTaintedSink`'s
expression path does not), and the detector matched only one, so a working
engine looked broken. Fourth instance in this campaign of a *detector* carrying
the defect it was hunting; see the method note on keyword filters.

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

Four things this campaign kept relearning:

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

**An assertion that looks specific can be satisfiable without the property
holding.** This appeared three times in the transition-admissibility phase and
was never caught by review: `grep '"key_fingerprint":"'` matches an empty value
(#7 above); `L24-02` asserted attestation-count *equality* against a counter
incremented on paths that do not attest; `L25-03` asserted a *total* on the
assumption that the build path was the only route to a file sink. The only
defence that worked was **running the degraded case** — reverting the fix and
confirming the assertion fails. An assertion never observed failing has not been
tested, it has been written.

A systematic audit followed, because four accidental discoveries are a poor
sampling method. Scope: 360 assertion sites in `run.sh` and 2294 across 235
shell suites. The result was mostly reassuring — most absence checks already
pair with a positive existence check, a fail-safe default (`${X:-1}`, where
unset means *fail*), or a named control (`TR-01` is labelled "the
anti-regression"; `PU-01` fails outright if its scenario produced fewer than six
samples). What it did find:

| finding | shape |
|---|---|
| `H01` | health verdict inferred from stderr containing the string `"governance"` — which every governed run prints. Passed hardest on a run that died before reporting its own health. |
| 25 sites / 7 security suites | the suite defined only `ok()` and `fail()` — **no `skip()`** — so "executor not available" had nowhere to go but PASS |
| `L24-03` | trivially true at zero attestations; non-vacuous only because a *neighbouring* assertion stayed strict |
| `DB-01`, `DB-03`, `DB-04` | initialiser `0` and fallback `"1.00 0 0"` are indistinguishable from "the scenario never ran" |
| `E04` | asserted a line was absent from a dashboard that a failed run never produced |

`test_gov_comment_styles_gov002.sh` is the one to remember: it reported
**"Results: 4/4 passed"** while verifying nothing at all — every check
unverifiable in that environment. It now reports `0/4 passed, 4 skipped`.

The generalisable rule is narrower than "write better assertions": **an
assertion about an absence needs the thing it is absent from to exist.** A
missing line in a document nobody wrote, a zero from a counter nobody
incremented, and a clean result are the same observation until something
distinguishes them.

One postscript, because it is the same lesson at one remove. The first pass
found 19 of those 25 sites; reviewing the diff before merge found six more in
files already being edited. The filter had searched for `not available` and
`may be`, and the survivors read *"may **not be** available"*, *"depth may not
have been reached"*, and *"(acceptable)"*. A keyword filter over prose is itself
an assertion that can be satisfied without the property holding — it finds the
phrasings you thought of. Reading the changed files end to end is what caught
the rest.
