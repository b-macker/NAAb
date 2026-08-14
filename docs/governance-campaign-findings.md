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

## A fix that reached one caller

The Windows CI job stalled four times inside `CLI tests — shell suites`: the step
sat `in_progress` for ~47 minutes, the runner was killed service-side, and the log
archive 404'd. Two of those runs carried a `timeout-minutes: 25` added expressly to
preserve logs — it never fired, so the runner could not enforce its own step
timeout either. The cause could not be read out of anything. It could only be
excluded.

The diagnosis was already in the tree, and had been before any of it:

> *"Stub-backed HTTP tests hang on Windows/MSYS2 due to signal propagation and
> process cleanup issues. Skip entirely — Linux CI validates the behavior."*
> — `test_absorption_degenerate.sh`

That comment sat in **1 of the 29 suites** that launch `agent_stub.py`. The other
28 kept running on Windows. Excluding all 29 took the step from a 47-minute hang
to **2m04s**.

The same shape had already happened once in this file's own table. `081f460`
fixed stub startup picking a single random port — *"a one-shot pick turned a busy
port into a scenario that silently produced no data"* — and that fix reached
**2 of 29**. Three CI runs later, three different stub suites failed on Linux for
what was very likely that same collision, and the obvious repair (add the retry
everywhere) is the withdrawn proposal below, because it hung Windows instead.

| fix | diagnosed in | reached |
|---|---|---|
| stub port retry (`081f460`) | 1 suite | 2 of 29 |
| Windows/MSYS2 stub guard | 1 suite | 1 of 29 |

The generalised shape is a **judgment applied to one of two twins**, and it has
now recurred often enough to be worth naming as its own failure mode rather than
a series of accidents: the per-agent ratchet that guarded `network_allowed` but
not `shell_allowed`; the field ratchet that covered agents which already existed
but not agents added; the two unrelated senses of `AGENT_SEND`; and — in the
write-up of the Aug 4 run itself — one withholding decision applied to
`transcript_*.jsonl` and `telemetry_*.jsonl`, which differ by ~17× in size and
entirely in whether they carry model content. In every case the reasoning was
correct for the twin it was reached on. Nothing carried it to the other.

The defect is not duplication. Copies of a helper are cheap and each file stays
runnable alone, which is worth something. The defect is that **a finding was
recorded where the other callers could not see it** — a comment in one file is
invisible to the 28 people who will next hit the same wall.

The platform guard is now one definition in `tests/helpers/stub_platform.sh`, so
the next finding about it lands on every caller at once. The *launcher itself* is
still 29 near-copies, and the port retry still reaches only some of them — the
same exposure, unfixed, and the reason it was not consolidated with the guard is
that the correct Windows behaviour of that code is exactly what is still unknown.
Consolidating it would freeze one guess into 29 callers.

Note what this cost. Four stalls, three misattributed Linux failures, one
incorrect one-commit bisect, and a "fix" that had to be reverted — all
downstream of a correct diagnosis nobody could find.

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

## Diagnostics the engine had and did not say

Runs 1 and 2 of the E6 keyed pair both died on a limit that was doing its job,
and both were first attributed to the wrong knob (see `open-investigations.md`,
E6). Neither misdiagnosis was caused by a wrong value — every number printed was
correct. They were caused by the engine declining to say *which* thing the number
belonged to. The fixes below are all of that shape: no behaviour changes, the
engine only stops withholding what it already computed.

| Gap | What was missing | Fix | Test | Live status |
|---|---|---|---|---|
| Budget error named no budget | Two similarly-named token budgets; the message quoted neither | `38366e5` | `test_limit_attribution.sh` LA-01/02 | confirmed |
| `pressure` had no inputs | Composite emitted, ten weighted factors discarded | `596bd82` | sum-vs-total check, residual 0.0 over 36 rows | confirmed |
| De-escalation predicate invisible | Calm counter and its owning handle were engine-internal | `596bd82` | trace reads 1 → 2 → step-down | confirmed |
| `AGENT_RESPONSE` had no turn | Joining it to `CDD_TURN` required a positional guess | `596bd82`, corrected below | `test_telemetry_join_key.sh` | confirmed |
| Telemetry label ≠ config key | Copying a firing signal's name into govern.json disabled nothing | `1bbbd79` | `test_signal_key_suggestion.sh` | stub-only |

Two method notes worth keeping, because both nearly produced a worse artifact
than the gap they closed:

**The pressure formula was re-derived externally and was wrong.** Ten factors,
not five, and `coherence_prox` divides by `coherence_threshold` (default 0.7).
The wrong form matched every floored-coherence row — where both forms give 1.0 —
and no other, so it survived spot-checking and reached a merged document.
`pressure_detail` therefore carries weighted CONTRIBUTIONS rather than raw
factors: contributions sum to the composite, so the decomposition is checkable
against the total instead of merely plausible. Five of the ten weights ship at
0.0, so any external tool hardcoding the weighting is silently wrong on a config
that opts into `semantic_deviation` or `codegen_pressure`.

**A joinable-looking field that is off by one is worse than no field — and
this one shipped before the trace caught it.** `current_turn` at the
`AGENT_RESPONSE` site is the count *before* the response is counted, so the field
was emitted as `current_turn + 1`. That reasoning was incomplete: `AGENT_RESPONSE`
fires ONCE, *before* the tool loop, and each tool round-trip advances the counter,
with one further increment after the loop. `CDD_TURN` therefore reports
`request_turn + round_trips + 1`, and no value computed at response time can
predict it.

The `+1` was verified against a tool-FREE scenario — 39/39, identical key sets —
which structurally could not exercise the tool path. A two-round-trip stub run
gives `AGENT_RESPONSE` 1 against `CDD_TURN` 3. The exact defect the note above
warned about, committed by the same change that wrote the warning, because the
verification and the claim had different scopes.

Fixed by keying both events on `turn_at_request`, assigned at the single point
the response arrives so neither emitter predicts anything; `CDD_TURN` keeps its
own post-loop `turn`. `report.py` now joins on it (with a positional fallback for
telemetry written before the key existed). Tests: `test_telemetry_join_key.sh`,
where JK-01 is the tool case the original verification could not have caught and
JK-02 is the tool-free control the broken version satisfied.

Method note that generalises: **a verification narrower than the claim it
supports is not evidence for the claim.** Nothing about 39/39 was wrong; it was
answering a smaller question than the one being asserted.

---

## Found in `src/`, not fixed

Both were found while building the diagnostics above. Both are left alone
deliberately: repairing either changes what a security control does, which is not
a drive-by edit inside an unrelated increment.

**`ErrorSanitizer` over-redacts identifiers.** Its `API_KEY` pattern
(`include/naab/error_sanitizer.h:189`) alternates on the bare word `token`, then
`[:\s]*` matches EMPTY, then the capture eats 8+ identifier characters. So
`max_tokens_per_run (60) exceeded` renders as `max_<redacted> (60) exceeded`;
`tokens_remaining` and `authorization_required` are swallowed whole. A security
control whose false positives destroy the diagnostics it was never meant to
touch — and the reason the run-level budget error does not name its key at
rendered output even though its source string does. Pinned by
`test_limit_attribution.sh` LA-03, written to PASS while the defect stands and
FAIL the moment it is fixed. The candidate fix (require a separator) LOOSENS a
redaction pattern; the open question is whether any real secret form lacks one.

**The leak suite's comment filter cannot match.** `test_error_msg_leaks.sh` pipes
`grep -n` output into `grep -v '^\s*//'`, but every line already carries an `N:`
prefix by then, so the anchor never matches and commented-out code is scanned as
if it were live. A sibling filter in the same pipeline *does* account for the
prefix, so this is a bug rather than a choice. Repairing it INCREASES what the
suite reports, so it may surface existing violations needing triage. Fifth
instance in this campaign of a control structurally incapable of firing.

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
5. **Check `allowed_actions` in `agent.commit()`.** Claimed as a gap in the same
   family as #8 and #13: `commit` re-checks the lease and CRITICAL but not the
   action matrix, and removing an action mid-run is *ratchet-legal* (recorded as
   `tightened` in `governance_config.cpp`), so a proposal looked committable
   after its agent's `AGENT_SEND` was gone. **The gap does not exist.**
   `agentCommit` already compares `getReloadCount()` against the count stamped
   into the proposal at propose time, and `reload_count_++` fires only on an
   *accepted* reload — so any config change that could remove the action has
   already invalidated the proposal. `allowed_actions` is config-derived and has
   no other mutation path, and `s_pending_proposals` has exactly one insert,
   inside `agentPropose`, behind that function's own `AGENT_SEND` check, so a
   caller cannot supply a proposal either. Covered by `test_propose_commit.sh`
   H-01/H-02. The reason it looked open: the gates were enumerated with a grep
   for `allowed_actions|AGENT_SEND|checkCriticalSuspension|leaseExpiredLocked|checkAdmission`,
   and `reload_count` was not in the pattern. See the method note below.
6. **Give the stub launcher a port retry and a longer readiness ceiling.** Three
   CI runs failed in three *different* stub-backed suites — the signature of the
   launcher, not a regression — so the launcher was hardened across the 9 suites
   then known to share the idiom. Linux went green; Windows stalled. A
   one-commit bisect looked conclusive (the commit before passed, the delta was
   the launcher alone) and **was coincidence**: the next commit restored the
   Windows path to byte-equivalent prior behaviour and it stalled anyway. Two
   things were wrong at once. The retry's cleanup used `kill` followed by an
   unbounded `wait`, which cannot return if TERM is ignored — a hazard this
   repo's own `run-all-tests.sh` comments already warn about for MSYS2. And
   "the 9 suites" came from grepping one launcher idiom rather than for
   `agent_stub.py`; the real population is 29. The retry now runs on POSIX only.

---

## Method notes

Five things this campaign kept relearning:

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

**A grep defines what you are able to see.** Three times in one session a
conclusion was wrong because the pattern that produced the evidence was narrower
than the thing being reasoned about, and nothing in the output said so — absence
from a grep result reads exactly like absence from the code.

| pattern | what it hid | consequence |
|---|---|---|
| `allowed_actions\|AGENT_SEND\|checkCriticalSuspension\|leaseExpiredLocked\|checkAdmission` | `agentCommit`'s reload-generation check | a security gap reported that did not exist |
| `seq 1 50); do grep -q READY` (one launcher idiom) | 20 of 29 suites launching the stub | an exclusion experiment whose negative result would have been uninterpretable |
| `taint_tracking.sink_violation` | the second violation message format | a working taint engine reported as broken |

The third is the one already recorded above as a *detector carrying the defect it
was hunting*; it is the same error at one remove, and the keyword-filter
postscript below is a fourth. The habit that catches it is cheap: after a grep
that will support a conclusion, ask what the pattern **cannot** match, and widen
it once on purpose. Search for the artifact (`agent_stub.py`) rather than for a
usage of it, and enumerate a function's guards by reading it rather than by
filtering it.

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

---

## Keyed campaign run — Aug 4, 2026

A live keyed run against 6 Gemini keys, targeting the five stub-only /
inert-verified entries in this document. Run completed without governance
termination: 163 pass, 1 fail, 7 skip. Artifact directory:
`/data/data/com.termux/files/usr/tmp/living-script-13648/work-13648-15101/`.

### Confirming the run was keyed

```
grep -c "No GK1 API key" keyed-run.log    → 0
echo "${GK1:0:6}"                          → AQ.Ab8
```

57 live API sends, 0 errors. 163 pass / 1 fail / 7 skip. Two step-up challenges
passed (one `tool_result` type at kr=0.500, one `validation` type at kr=0.929).
S22 fired on a real test failure (turn 19 of segment 2,
`penalties_detail: validation_outcome=0.1500`, coherence 1.0 → 0.865) and
recovery credit applied three turns later (`validation_recovery=+0.0750`).

The single failure was **`[L1-03] No adjustments tracked (ADJUSTMENTS=0)`**,
and it is unrelated to taint, telemetry integrity, pulse, validation or
propose/commit — it does not invalidate any conclusion below.

The *reason* first recorded here — that the operator judged the team fine and
held steady — is the **rare** case, not the usual one. Over the 33 committed
archive runs carrying a `SUMMARY_ADJUSTMENTS` line, 24 applied adjustments and 9
did not; of those 9, **8 show the operator PROPOSING adjustments that were then
rejected** (`OPERATOR|adjustment_rejected`, `BLOCKED: … can only decrease`,
`Skipped unknown agent`) and only **1** is a genuine deliberate hold. So the
common cause was being reported in the wording of the uncommon one, and
`L1-03`'s single message could not tell them apart.

The Aug 4 run classifies as **FAIL(rejected)** under the new logic: 11 operator
decisions (9 `none`, 1 `observe`, 1 `create_agent`), 0 `adjust` actions, **4
rejection markers** (`BLOCKED: context_window can only decrease (20 -> 50)`,
`No valid changes to apply` ×2, `Skipped unknown field: system_prompt`). The
operator included adjustment fields in its JSON despite `action=none`, and the
harness rejected them — the common case, not the rare deliberate hold. Evidence:
`results_20260804_225224.txt` (now committed).

The operator's action vocabulary is **five** values, not the "observe/none" the
first write-up assumed. Across all 38 committed `results_*.txt`: `adjust` (123),
`observe` (82), `none` (75), `create_agent` (54), `transition` (15). Only
`adjust` is a config adjustment; `create_agent` spawns a runtime handle for an
agent that must *already* exist in govern.json (`living-script.naab:1098`
onward), incrementing `agents_created` and never `adjustments_made`. So treating
everything ≠ `adjust` as non-adjusting is correct for all five — but the
vocabulary is worth stating, because two successive write-ups of this same
assertion each named only the values that happened to appear in the run in front
of them.

`L1-03` now distinguishes the three cases (run.sh): adjustments applied → PASS;
rejection markers present → FAIL naming the count; decisions present, none
asking to adjust, no rejections → SKIP. The skip is gated on **positive**
evidence, because skipping on `ADJUSTMENTS=0` alone would have silently absorbed
those 8 rejected-proposal runs — absence of evidence read as evidence of a
deliberate hold, which is the exact defect class this document exists for.
Validated by replaying all 33 archived runs through the new logic: 24 PASS,
8 FAIL, 1 SKIP.

### Per-target results

| Finding | Status before | Triggered? | Evidence | Status after |
|---|---|---|---|---|
| #2 streak field | stub-only | No | Zero `PULSE_TRANSITION` events across 117 analyzed CDD turns (all 117 analyzed=true, 115 SEMANTIC_TURN). Pulse was evaluated on every turn and stayed HEALTHY — the negative is a non-occurrence with ample opportunity, not an absence of evaluation. | stub-only |
| #5 evidence count shrink | stub-only | No | 12 `VALIDATION_RECORDED` events across 2 segments. Segment 1: 14→15→17→19→21→24 (monotonic, all pass). Segment 2: 11→12→17→19(fail)→22(pass)→22(fail). No decreasing count within any segment. | stub-only |
| #8 lease expire mid-deliberation | stub-only | No | Both propose/commit pairs completed in <1s (same-second timestamps). Developer lease: 600s/20 turns, at turn 9 of 50. No expiry anywhere close. | stub-only |
| #9 config generation guard | inert-verified | No (refusal stub-only) | Propose/commit path ran live (2 each). 8 `CONFIG_ADJUSTMENT` events occurred in the same runs, but all AFTER the commit timestamps (first reload at 22:43:55, commit at 22:43:53). No reload landed inside a deliberation gap. **Provenance:** these 8 are *not* operator tuning — `adjustments_made` increments only on apply (`living-script.naab:773`) and this run applied zero, so they are reload attempts from other mtime changes (the engine-ratchet probe, the cross-run config restore). `CONFIG_ADJUSTMENT` fires inside `reloadIfChanged()` on any detected change, including signature failures and ratchet rejections. The conclusion is unaffected — a reload is a reload for "did one land in a deliberation gap" — but "8 adjustments" beside "0 adjustments" otherwise reads as a contradiction. | inert-verified |
| #13 CRITICAL suspension | stub-only | No — **and could not have** | Zero `GOVERNANCE_LEVEL_CHANGE` across 117 analyzed turns, all `governance_level: "normal"`. 7 turns exceeded `elevated_threshold` (0.35), peak 0.375; **two** handles reached the `elevated_sustained: 2` boundary — developer turns 19–20 (0.3525, 0.3575) and operator turns 10–11 (0.37, **0.375**, the higher pair). Escalation was nonetheless **structurally unreachable**: the level test needs `composite >= threshold` AND `consecutive >= sustained`, and `consecutive` advances only while `composite >= reality_checkpoint.pressure_threshold`, which stayed at its **0.70** default because the run disabled the section. `cdd_snapshot` confirms `consecutive_high_pressure_turns == 0` on every snapshot in the run. See below. | stub-only |

**Evidence provenance.** `telemetry_20260804_225224.jsonl` is committed, so every
count above is re-derivable here. It was initially withheld as "full API
traffic" — true of the *transcript* (`transcript_*.jsonl`, ~8.9 MB, raw prompts
and responses), false of the *telemetry* (1.3 MB): auditing all 142 keys and
every string field over 80 chars finds no model content — the longest values are
a taint warning naming its sink, the script's own convergence `pattern`, a
ratchet `violations` list, and `signals_detail`; `result` is only `pass`/`block`;
response text appears solely as `content_hash`. The transcript stays out. One
withholding decision had been applied to two artifacts differing ~7× in size and
entirely in whether they carry content.

### The pressure path was inert for the whole run

Chasing #13's near-miss found something better than a near-miss. Level escalation
requires **both** `composite >= circuit_breaker.<level>_threshold` **and**
`consecutive >= <level>_sustained` (`governance_engine.cpp` ~7230). The second
half is fed by a counter that increments only while
`composite >= context_drift.reality_checkpoint.pressure_threshold` (~7176) —
a **different** threshold, defaulting to **0.70**.

`living-script_extended/src/govern.json` sets `elevated_threshold: 0.35` and
disables `reality_checkpoint`, leaving its `pressure_threshold` at 0.70. Peak
composite across the run was 0.375. So `consecutive` never left 0 — confirmed
directly, not inferred: `consecutive_high_pressure_turns == 0` on every
`cdd_snapshot` in all 1342 events. Two handles sat at the `elevated_sustained: 2`
boundary and the level could not have moved for either.

This is the document's own subject matter turned on the engine. "Pressure never
escalated because the agents behaved" and "pressure could not escalate because a
second threshold silently gated it" produce an identical observation — 117 turns
at `normal` — and the first reading is the flattering one. It was written up
twice before the counter was checked.

The defaults carry the same shape: `elevated_threshold` 0.4 and `high_threshold`
0.6 both sit below the 0.7 gate, so **the pressure path cannot reach ELEVATED or
HIGH on a default config** — only CRITICAL (0.8) clears it. Those levels remain
reachable via the pulse floors (IMPAIRED → 2, DEGRADED → 1), so this is a dead
knob rather than a dead level, and it was recorded in `CLAUDE.md` only as a
*test-staging* gotcha, never as operator-facing behaviour.

Not changed: the coupling itself. Making the counter track the lowest
circuit-breaker threshold would raise escalation frequency across every existing
config — a behaviour change dressed as a bug fix, and the wrong thing to land off
a documentation trail. What changed is the diagnosis. The load-time note said
"circuit breaker levels will update", which is what misled this config; it now
names the gate and lists precisely which configured levels are unreachable
through pressure, and stays silent when `pressure_threshold` is at or below them
(verified both ways, plus silent when the checkpoint is enabled).

### Escalation requires evidence, not longevity (Aug 8, v2)

The first keyed run of v2 on the post-#129 engine came back entirely quiescent:
35 CDD turns, peak composite **0.225**, nothing above `elevated_threshold` 0.35,
no escalation, no challenges. Both counter families read 0 — which proves nothing
about their independence, since neither gate was approached.

The obvious explanation was topology: v2 spreads 35 turns across 11 handles (3.2
each, max 6) where v1 ran 117 across 7 (16.7 each, max 44), and
`conversation_depth` is scaled per handle. That explanation is **wrong**, and the
correct one is more useful.

`depth` is clamped: `std::min(1.0, turn / expected_conversation_depth)`. Its
contribution therefore caps at its weight, **0.10**, however long a handle runs.
`risk_score_proximity` likewise caps at 0.20. So with coherence intact and no
signals firing, composite pressure has a hard ceiling of **0.30 — below
`elevated_threshold` 0.35**.

A well-behaved agent cannot escalate at *any* run length or topology. Escalation
requires at least one of: coherence degradation, a signal firing, or BSD partial
progress — all of which are behavioural evidence rather than mere longevity.
v1's peak of 0.375 confirms it from the other side: that turn carried
`signals_fired: 2`, coherence down to 0.708, and penalties from
`vocab_contraction` and `entity_consistency`. v2's peak carried coherence 1.0 and
zero signals.

This is a good property and it was not designed deliberately so much as
discovered — the two factors that accumulate for free are capped below the first
escalation threshold, so scrutiny tracks misbehaviour rather than uptime. Worth
knowing before anyone "fixes" the depth clamp or raises its weight.

Two corrections to the analysis that produced this, both in the direction of
overstating what longevity buys: the claim that a 40-turn handle contributes
`40/20 * 0.10 = 0.20` of depth pressure drops the clamp (it is 0.10), and the
report's "max 12 turns on engine_dev" does not reconcile with the telemetry
(`engine_dev` has 5 CDD turns; the highest for any handle is 6).

**Consequence for instrument choice:** v2 is a good example and a poor pressure
instrument, but not for the reason first proposed. Any scenario whose agents
behave will sit at or below 0.30. Exercising de-escalation or the upper levels
needs a scenario that produces genuine drift, not merely a longer one.

### Keyed run Aug 8: escalation confirmed working, with the gate corrected

One line changed — `reality_checkpoint: {"enabled": false, "pressure_threshold":
0.35}` — and nothing else. First live run in the campaign in which the pressure
escalation path executed at all.

**Confirmed.** One `GOVERNANCE_LEVEL_CHANGE`: `normal -> elevated`, tester,
turn 11, at exactly `elevated_sustained: 2`. `consecutive_high_pressure_turns`
reached 2 (it was pinned at 0 on every prior run). Peak composite 0.375 —
*identical* to Aug 4. The pressure was never the blocker; the gate was. Aug 4
had **7** turns above 0.35 and could not escalate; this run had **3** and did.

**Still unexercised, and the run could not have exercised them:**

- **De-escalation hysteresis.** Escalation landed at turn 11 of 12 in segment 1,
  and the raising handle (tester) never took another turn in that process. Calm
  turns are counted only from `deescalate_pressure_handle_`, so
  `deescalate_sustained: 3` had nothing to count and could not fire by
  construction. Segment 1 ended at `elevated`; segment 2 is a **new process**
  that starts at `normal`. The apparent recovery is a restart, not the
  mechanism — a distinction worth stating because the run summary read it as
  "level stayed elevated for the rest of the run", which is true of the process
  and false of the run.
- **HIGH and CRITICAL.** Peak 0.375 against `high_threshold` 0.55. Not close.

**Challenge attribution — 3 of 8, not 8 of 8.** The run passed 8 step-up
challenges and failed none, but they do not all belong to the escalation.
`step_up_at_level` is `elevated`, yet three fired while the level was `normal`,
two of them in segment 2 which never escalated at all. Reading them against the
per-agent lease config accounts for it: the developer's fire at turn 20 in
**both** segments (`standing_lease_turns: 20`), and the reviewer's at turns 2-3
(`standing_lease_seconds: 300` — it is consulted intermittently, so five minutes
of wall clock elapses between its turns). Lease expiry forces a challenge
regardless of level, as does `step_up_on_inadmissible`, which this config
enables. Only the operator's three, at turns 8-10 under `elevated`, are
escalation-driven. A challenge count is not an escalation measurement.

So the honest status: **escalation confirmed**, de-escalation and the upper two
levels still stub-only, and challenge counts need attributing to their trigger
before they mean anything about scrutiny.

No status upgrades to "confirmed". All five conditions are legitimate
non-occurrences — the run was clean enough that the corrective mechanisms had
nothing to correct. The run confirms that their *non-firing* paths behave
correctly (no false positives, no vacuous passes from the gates being absent),
but cannot confirm the firing paths.

### Taint count

Measured 8 violations in the primary segment (9 in the cross-run segment).
Consistent with prior measurements of 7, 8, 8. Baseline lowered from 12 to 10
(25% headroom, down from 50%) — a sanitizer loss would add ~16 violations,
still well above the baseline.

The headroom is against **8, not 9**, and the reason is ordering rather than
scoping: there is one `WORKDIR` and one `telemetry.jsonl`, which the cross-run
section reuses, but `TAINT_N` is computed at run.sh:1573 and `L25-03` asserts at
~1637, while the cross-run segment does not start until ~1916. The assertion has
already evaluated before those ~9 further violations are appended. Stated here
because the two numbers sitting side by side invite the conclusion that the
baseline has 1 of slack; it has 2, and a later reader changing the ORDER of these
sections would silently make that false.

### Two defects in the harness, found reviewing this run

**`summary.json` recorded a failure count with no identity.** This run committed
`"fail": 1` into the permanent record; the failing assertion's ID lived only in
`results_<timestamp>.txt`, which was not committed. A count that cannot be
triaged later is close to useless when the run costs money to repeat. `fail()`
now accumulates IDs and `summary.json` carries `"failed": [...]`. `gk_fail()`
routes through `fail()`, and `fail()` is the only site incrementing
`FAIL_COUNT`, so one change covers every failure path.

**`grep -c ... || echo 0` produced `"0\n0"`, inverting L25-03's diagnosis.**
`grep -c` prints `0` *and* exits 1 when nothing matches, so the fallback appends
a second zero. The `${TAINT_N:-0}` guards cannot help — the variable is set, just
to a non-integer — so both integer tests error, bash reads that as false, and
L25-03 reports *"a new unsanitized sink path was added"* when taint tracking
in fact emitted **nothing at all**. Precisely inverted, in the one situation the
assertion exists for. Two sites (`TAINT_N`, `ESC_N`); the correct `|| true` form
was already in use three assertions later at L19b-03. Same class as the
`test_semantic_signals.sh` fix in #119 — fixed there, still live here.

### Assertions that PASSed while their mechanism did not run

None identified in this run. The L22 OA assertions correctly SKIPped on the
quarantine path (0 quarantines). The L24-05 refusal attestation assertion
correctly noted "nothing to attest" (no governance refusals occurred). Q02
counted real challenge passes (1 in segment 1, confirmed against
`AGENT_CHALLENGE_PASS` telemetry). The `TELEMETRY_AUDIT|consequence|
AGENT_CHALLENGE_PASS=0` line reads the telemetry file mid-run, before the
challenge fires at turn 20 — this is a known timing limitation of the in-script
telemetry audit, not a vacuous assertion (Q02 counts from the script's own
tracking).

### Keyed runs Aug 10 (v3): the ladder walks live, on drift nobody scripted

Two keyed runs of `living-script_v3`. **Neither finished** — both died on a
configured limit that was doing its job — but run 2 got far enough to answer the
question v1 and v2 could not.

**Confirmed live, against a real model:** `normal -> elevated` at turn 8,
`elevated -> high` at turn 10, `drift_worker` the pressure handle at both. HIGH
had previously been reached only against the stub. The drift was not scripted:
no agent was ever instructed to misbehave, and the phases differ only in how
*specified* the task is — DRIFT_PRESSURE names the same objects as the mandate
without saying what to do with them. Real signals on real responses moved the
ladder.

**Two further live confirmations from the same run:**

- **The conservation invariant holds against live traffic** — residual
  `0.000000000000` over all 16 snapshots
  (`coherence == 1 - damage + healed`). The bounded-healing ledger had only ever
  been checked against stub runs.
- **S20 fired 0 times.** The scenario's prompts were reworded after an earlier
  version fired `prompt_compliance` on 35 of 36 turns, including the phases where
  the agent was doing exactly the right thing. That is the scenario penalising
  the agent for the *harness author's* vocabulary — the instruction-following
  trap arriving through the user prompt instead of the system prompt. The reword
  held against a real model, which is the only place it could be tested: S20's
  first gate compares the prompt against the mandate, so it is decided before the
  response exists.

**Still not observed: de-escalation live.** Run 2 died one turn into
DRIFT_RECOVERY, the only phase that could have produced it.

**Both runs died on a limit whose name was not the one in the error text**, and
in both cases the first diagnosis named the sibling knob:

| run | died at | actual cause | first diagnosis (wrong) |
|---|---|---|---|
| 1 | 33s, 11 of ~39 calls | `limits.timeout.global`, unset, defaults to **30s** | `agent_dispatch.default_timeout_seconds` — the PER-CALL timeout; nothing reads it as a run limit |
| 2 | 110070 tokens | per-agent `max_total_tokens`, unset, defaults to **100000** | `hard_stop.max_tokens_per_run` — set to 500000 and never approached |

`run.sh`'s `timeout 600s` wrapper never governed run 1: the engine's own limit
always fired first, so the visible backstop was structurally unreachable — the
same shape as an inert mechanism, in the harness rather than the engine.

Both fixed by setting the limits explicitly, with `meta` entries recording that
the two similarly-named limits are different and that the error text quotes the
per-agent one. Also worth keeping: history is resent every turn, so token spend
grows roughly with the **square** of turn count — 19 calls cost 110k against a
36-call scenario.

**A gate of mine was mis-measuring its own precondition.** V3-11 counted every
analyzed `CDD_TURN` after the first escalation as a "calm turn", and reported
*"de-escalation did not fire despite 8 calm turns"* when all 8 fired 2-3 signals
each. A correct non-firing was reported as a governance failure — the mirror
image of vacuity, and the same root shape as every other entry in this document:
the predicate the gate stated and the predicate it evaluated were not the same
one. Now requires empty `penalties_detail`.

## Vacuity audit: tier 1

`tests/security` and `tests/governance_v4` — 105 suites, 782 `pass`/`ok` sites.
Filtered to 129 absence-guarded assertions, then to the ones whose text claims a
negative property. The question asked of each: *what state makes this pass
without the property holding?*

### Assertions that could not fail on any input

Seven, found by looking for `if/else` regions where every reachable branch
passes. (A first pass reported 17; nesting-aware re-checking cut it to 7 — the
other 10 were inner blocks inside an outer `if` that does have a `fail`. The
detector needed the same correction the tests did.)

| assertion | what it claimed | what was true |
|---|---|---|
| `test_governance_validity.sh` T19/T20/T21 | "contradiction detection ran" | asserted nothing; the stated excuse ("may not print without dashboard") was refuted by the command's own `--governance-dashboard` flag |
| `test_r11_fixes.sh` T2 | opt-in config discovery works | its own comment read "something else is wrong — still pass the security property" |
| `test_sandbox_symlink_f.sh` F4 | unrestricted sandbox follows symlinks | premise false by design (below) |
| `test_stdlib_shadow_e.sh` E4 | path-separated import loads local module | the fixture never parsed (below) |
| `test_d1_reconciliation.sh` A09 | `claim_accuracy` absent before first tool | engine emits `1.0`, always (below) |

Three of these were concealing real defects. That is the argument for the audit:
an unfailable assertion does not merely fail to catch regressions, it preserves
whatever misunderstanding it was written with.

**T20 was hiding dead engine code.** CONTRA-007 ("language in both allowed and
blocked") iterated `languages.allowed` looking for members of `languages.blocked`
— a set the loader empties at parse time, erasing blocked languages from allowed
so blocked wins fail-closed (`governance_config.cpp` ~312). The intersection is
empty by construction; the check could never fire. It now reads the overlap the
loader records before resolving it. Level moved from a hardcoded `SOFT` to the
configured `contradiction_detection.max_level` (ADVISORY by default): the
hardcode ignored the operator's cap, and waking a dormant check at SOFT would
newly `exit(3)` every config with overlapping lists — a blocking change smuggled
in as a reporting fix. The loader already neutralises the danger; what was
missing was telling the operator. CONTRA-001 keeps its hardcoded SOFT because it
is live today, so relaxing it would be a real weakening rather than a dead knob.

**E4 had never executed its own program.** Two stacked fixture bugs — `as *
mymod` (invalid; the form is `as name`) and `use io` inside `main` (only legal at
file scope). Both fixed; E4 now passes on the property.

**A09's premise was inverted.** It asserted `claim_accuracy` is absent before the
first tool call. `agent_impl.cpp` emits `1.0` on both paths — empty history
(:816) and no DriftState at all (:855). The field is never absent, so the branch
that "accidentally" passed was the correct one. Now asserts the documented
default and fails if it moves (verified by changing it to 0.5).

**F4's premise was false by design.** `readFileNoFollow` is gated on whether a
sandbox is *installed*, not on its level, so `--sandbox-level unrestricted` still
refuses symlinks. That is deliberate: the guard closes a TOCTOU window between
the path check and the open, which is not a path-policy question and does not
relax with the policy. F4 now asserts the level-independence. F3 was the real
positive control for F1/F2 all along.

### Catch-all `else → pass`

Branches that accepted any unrelated failure as success. Converted to SKIP —
"the mechanism did not run" is not "the mechanism held".

- `test_env_scrub_polyglot.sh` T3 — a **secret-leak** test passing on "may have
  been scrubbed or Python error". A Python error hides the key for the same
  reason a missing interpreter does: the code that reads the environment never
  ran. This repo's Python executor is disabled without pybind11, so the branch
  was live, not hypothetical.
- `test_block_integrity_rt004.sh` T1 — passed while its own message said
  "integrity path not reached in this context".
- `test_polyglot_taint_gov006.sh` T2 — passed on any non-zero exit: parse error,
  missing executor, crash.

### `test_orphan_kill_rt003.sh` — three defects from one unused variable

`MARKER="naab_orphan_test_$$"` was declared with a comment explaining precisely
why a unique identifier was needed, then never used: `pgrep` and `pkill` both
matched the generic `sleep 30`. Consequences, all live: a vacuous PASS when the
child never spawned; a spurious FAIL from any unrelated `sleep 30` on the box;
and `pkill -f "sleep 30"` on the failure path, **killing other users' and other
suites' processes** on a shared runner. Now observes specific PIDs — which makes
the control possible (assert the child *did* start) and makes both the check and
the cleanup incapable of touching anything not ours.

Demonstrated rather than asserted: with the subprocess removed from the fixture,
the old test reports `PASS: no orphan sleep process after timeout`; the new one
reports SKIP.

### Non-findings worth recording

Vacuous in isolation, correct because a sibling assertion fails on the same
absence — the `L24-02`/`L24-03` shape. Left alone, documented in place:
`test_signing_bypass.sh` T1 (T1b fails if keygen produced no `.pub`),
`test_challenge_fail_path.sh` A-07 (A-03 fails if the telemetry file is absent).

Clean, and the pattern to copy: `test_drift_sensitivity.sh` DS-01/DS-02 —
`${L0_SINGLE:-1}` defaults to a FAILING value so an unset measurement cannot pass,
paired with a cross-check that quarantine fires at *some* drift level.

### Method note

Every fix was verified by breaking the property and confirming the assertion
fails, then restoring. That is the only technique that has caught this class:
five of these had been read by reviewers without the defect being visible,
because a passing test looks identical either way.

Not audited: tier 3 (`tests/gorilla`, 62 candidates) — older scenario tests,
weaker claims, and auditing them would triple the work.

## Vacuity audit: tier 2

`tests/governance`, `tests/cli`, `tests/property`, `tests/vm` — 39 suites, 168
`pass`/`ok` sites. Far cleaner than tier 1 in structure, and it still contained
the single worst assertion found in either tier.

### A suite that had never tested its own subject

`tests/cli/test_js_timeout_rt002.sh` exists for **V-RT-002: JS blocks respect
`--timeout`**. Both its tests passed. Neither had ever exercised a JS timeout.

Three defects stacked, each hiding the next:

1. The fixture was a bare `while(true) {}`. The JS executor wraps a block as an
   *expression*, so this is `(while(true) {}` — a `SyntaxError`. The block never
   ran.
2. T1's branch accepted **any** non-zero exit as proof: `elif [[ "$ec" -ne 0 ]];
   then ok "timeout enforced"`. A parse failure in 0 seconds satisfied a test
   for a 3-second timeout, because "the timeout fired" and "the code never
   parsed" produce the same observation.
3. `ec` was never reset between tests. `|| ec=$?` does not fire on success, so
   T2 read **T1's** exit code; its catch-all `ok "non-zero exit … acceptable"`
   then reported that as its own pass.

Fixed: the fixture is now an IIFE (`(function(){ while(true) {} })()`) that the
expression wrapper accepts; `ec` resets per test; and T1 requires `elapsed >= 2`
alongside the non-zero exit. The elapsed floor is the load-bearing part — it is
what makes "exited non-zero" mean the timeout rather than a crash. The corrected
suite runs the full 3 seconds and reports `InternalError: interrupted`, so
V-RT-002's fix is now demonstrated for the first time rather than assumed.

Restoring the old fixture under the new assertions fails with the syntax error
quoted; under the old assertions the identical state was a PASS.

**Branch order is part of the fix, and I got it wrong first.** The
executor-missing check lived in the final `else`, reachable only when `ec == 0`
— but a missing JS executor exits NON-ZERO, so that branch had always been
unreachable and the old code caught the case one branch earlier as "timeout
enforced". Adding a fast-exit failure while leaving the check where it sat
converted every platform without a JS executor from a false pass into a hard
FAIL. Linux CI could not have caught it: Linux has the executor. Found by
re-reading the diff before merge, confirmed by pointing the block at a
nonexistent language (pre-fix: FAIL, post-fix: SKIP), and fixed by hoisting the
executor check above the exit-code branches. The general form: **when you make a
previously-accepting branch strict, check what used to land in it.**

### `test_govern_json_config.sh` T6 / T10

Both unfailable. T6's fallback condition was `elif [ $? -eq 0 ] || true` — the
`|| true` makes it unconditional, and `$?` referred to the preceding `[ -f ]`
test rather than to naab, so T6 passed on every possible outcome. T10's was a
plain `else … pass "(no crash)"`.

T6 was also masking a real mismatch: `govern.json` is discovered relative to the
**script's** directory, while `governance.telemetry` resolves relative to the
**CWD**. Running from the repo root therefore wrote `telemetry.json` into the
repo root while the assertion looked for it in `$WORK_DIR`, and the fallback
reported success — so neither the mis-based path nor the stray file was ever
visible. The test now runs from `$WORK_DIR` (as T10 already did) and fails if
the file is absent. The path-base inconsistency itself is left alone: changing
where telemetry lands would move files for every existing user, which is not a
change to make off a test audit.

### Corrections to my own detectors

The absence-guard detector reported 32 tier-2 candidates. Most were false
positives, and the reasons are worth recording because they bound what these
scans can claim:

- `count==0` fired on `[ $RC -eq 0 ] && [ -f … ] && grep -q …` conjunctions,
  where the exit-code test is paired with positive evidence (`test_drift_detection.sh`
  T54/T56). Not vacuous.
- Two hits in `tests/cli/test_report_formats.sh` were Python `pass` **statements**
  inside a heredoc, not the shell helper.
- `test_drift_detection.sh`'s "unchanged file passes" assertions are each
  followed by a "modified file blocks" assertion — the pairing shape, correct as
  built. That suite also isolates its own trust store, which is the practice the
  leaking suites below lack.

And the sharpest instance in the campaign happened to the *instrument*, not the
subject. Waiting on CI for this very PR, I armed a poll loop that counted
incomplete checks:

```
sum(1 for c in d.get('check_runs', []) if c.get('status') != 'completed')
```

`curl` to the GitHub API is not authorized in this session — it returns
`{"message": "GitHub access is not enabled for this session..."}`, a payload with
no `check_runs` key. The `.get(..., [])` default turned that into an empty list,
the sum came out `0`, and the loop announced **ALL CHECKS COMPLETED** while eight
jobs were still running — including every compile-and-test job.

"Zero incomplete checks" and "no access to any checks" are the same observation,
and the script took the flattering reading. The `.get` default was the permissive
branch and I never asked what could land in it — the identical mistake as the JS
timeout branch order, made minutes after writing that lesson down, in the tool
built to verify the audit. A monitor that cannot distinguish *the work finished*
from *I cannot see the work* is not a monitor. Any watcher of this kind needs to
require **positive** evidence — a check count greater than zero — exactly as
`test_drift_sensitivity.sh` defaults `${L0_SINGLE:-1}` to a failing value.

I also assumed, from the V-GOV-009 work, that `naab-lang` discovers `govern.json`
from the CWD, and concluded that 11 of the 12 tests in `test_govern_json_config.sh`
were loading the wrong config. That was wrong: **`naab-lang` discovers from the
script's directory** — `naab-gov` was the binary with CWD-based discovery. Tested
before writing it up, which is the only reason it is a paragraph here rather than
a finding.

### Still open: the trust-store leak

A key dated Aug 3 sits in `~/.naab/trusted-keys`. Suites that write an unsigned
`govern.json` then hit `INTEGRITY BLOCK` fail standalone while passing under
`run-all-tests.sh`, which isolates the store: `test_env_scrub_polyglot.sh`,
`test_polyglot_taint_gov006.sh`, and `test_govern_json_config.sh` (8 of 12
failing standalone, 12/12 isolated). Pre-existing on master, and the same shape
one level up — the harness hides the state the test would have reported. Not
fixed here; whichever suite installs a key without isolation needs finding first.

## `"enabled": false` that turns a check on

Working the contradiction-check coverage list turned this up sideways, which is
the same way `CONTRA-007` and the pressure ladder were found.

`CONTRA-010` fires when `capabilities.shell.blocked_commands` is set while
`restrictions.code_injection` is off. A config written to trip it — blocked
commands plus `"restrictions": {"code_injection": {"enabled": false}}` — stayed
silent. The check was fine. The config was not doing what it said: writing
`"enabled": false` had **enabled** code_injection.

Six of the twelve `restrictions.*` sub-blocks enable themselves from the mere
presence of the block and never read `enabled` at all:

| honours `enabled` | ignores it, forces on |
|---|---|
| `vcs_secret_extraction`, `obfuscation`, `data_exfiltration`, `resource_abuse`, `information_disclosure` | `dangerous_calls`, `shell_injection`, `privilege_escalation`, `code_injection`, `crypto`, `imports` |

(`polyglot_output` has no `enabled` concept at all.)

So the same JSON means opposite things depending on which sibling it is written
under, and omitting the block is the only way to leave one of the six off. All
six were confirmed empirically, not read off the source: each fires with
`"enabled": false` exactly as it does with `true`, and not at all when the block
is absent. The control matters as much — `data_exfiltration` with a canary
pattern gives 0 findings at `false` and 1 at `true`, so the key demonstrably
works on that side of the table.

**Direction of risk.** Fail-closed: the accident is *more* enforcement, not
less. Nothing is unprotected because of this. What is wrong is that an operator
who believes they turned a check off is wrong, has no way to find out, and
cannot generalise from the five siblings where the key does work.

**What was changed: only the silence.** The value is still ignored. Making all
twelve honour the key is a **loosening** — every config carrying `"enabled":
false` today is being enforced and would silently stop being enforced on
upgrade. That is the trade already rejected for default-on secret scanning, run
in the other direction, and it is not one to make while closing a reporting gap.
So the six now print

```
[governance] Warning: "restrictions.crypto.enabled": false has no effect — this
check is enabled by the presence of its block. Remove the "crypto" block to
leave it disabled.
```

following the precedent of the existing `security.blocked_commands` warning. One
helper, called from six sites, with a brace-accurate audit asserting the call
reaches exactly the sub-blocks that force and none that read — because "a fix
that reached one of N callers" is this campaign's most repeated finding and a
sixth copy of a `fprintf` is how it happens.

`tests/governance_v4/test_restrictions_enabled_key.sh` covers it, and RE-10 is
the gate that matters: it asserts the check *still runs* under `"enabled":
false`, so it fails if someone later converts this into the loosening. Verified
by doing exactly that (degradation E4). RE-07/RE-08/RE-09 are the controls
against an indiscriminate warning — without them, warning on every sub-block
passes six of ten gates.

### A vacuity in the gate written to catch a vacuity

RE-10 first passed while measuring nothing. Its counter was
`grep -c 'restrictions.crypto'`, and the new warning *names the rule it is
warning about* — so the gate counted its own warning as evidence that the check
had run. It was only visible because the trust-store leak (below) was in its
active state at that moment: every config was an `INTEGRITY BLOCK`, nothing
executed at all, and RE-10 still passed. Under an isolated trust store it would
have passed for the right reason and the defect would have shipped.

Two things follow. A gate must exclude the artefact it introduced from its own
evidence. And the environment did the work here that review did not — the same
lesson as everything else in this document, arriving from the least convenient
direction available.

The trust-store leak also has a better description now than "unknown cause": the
store is not written during a run. `~/.naab/trusted-keys` is dated Aug 3 and
unchanged since; what varies is that suites using `trust_setup.sh` point
`NAAB_TRUST_STORE_DIR` elsewhere while they run. Probes issued during that
window see an empty store and succeed; the same probes outside it are blocked.
Findings taken from ad-hoc runs against a developer container are therefore
timing-dependent unless the probe isolates the store itself.

## Three data limits that are configured, ratcheted, and enforce nothing

The A3 sweep, run properly. The first attempt was worthless and is recorded
first because the mistake is more useful than the result.

### Evidence mining does not discriminate

Cross-referencing all 107 rule names reaching `enforce()`/`recordPass()`
against 21 MB of archived runs reported **95 of 107 "never fired"** — including
five I had fired by hand an hour earlier. Every archived run is `living-script`,
which exercises agent governance and almost no static-source checks, so the
sweep measured *what those runs covered*, not *what can fire*. Only 12 rules
appear at all.

That is this campaign's own rule turned on its own instrument: a zero is only
evidence if you can say what would have made it non-zero. **Source reachability
discriminates; evidence mining does not.**

### What reachability found

Of 82 check-bearing `GovernanceEngine` methods, **five have zero call sites**
anywhere in `src/` — defined once, declared once in `governance.h`, never
invoked:

| method | verdict |
|---|---|
| `checkStringLength` | backs an inert key |
| `checkNestingDepth` | backs an inert key |
| `checkDictSize` | backs an inert key |
| `checkOutputSize` | **not** a gap — the value has a live consumer |
| `checkCodegenAllowed` | **not** a gap — dead duplicate of live enforcement |

Two of the five would have been false findings, which is the argument for
tracing each one rather than reporting the list:

- **`limits.data.output_size` is enforced**, at `polyglot.cpp:718`. But it
  throws a plain `std::runtime_error` instead of calling `enforce()`, so it
  produces no governance finding, no telemetry and no report entry, and is
  **catchable** by NAAb `try/catch` unlike the HARD block it resembles.
  Enforcement without evidence — a different finding, filed as A3a.
- **`codegen.*` is fully enforced** in `codegen_impl.cpp` — `enabled`,
  `allowed_languages`, `blocked_languages` and `max_code_size_bytes` all have
  live checks. `checkCodegenAllowed` is a dead duplicate: a maintenance hazard
  (someone fixing it would achieve nothing) rather than a hole.

So the real finding is three keys: **`limits.data.string_length`,
`nesting_depth` and `dict_size`** are parsed, recorded in `explicitly_set` so
they take part in the ratchet and in inheritance, clamped — and then read by
nothing except their own dead methods. An operator can set a HARD data limit,
watch it survive validation, and get no enforcement at all.

### The sharper half: one setting, two spellings, different behaviour

`limits.data.max_json_depth` is documented as an alias for `nesting_depth` and
writes the same struct field — but it *also* calls `setMaxJsonDepth()`, which
`json_impl.cpp` reads at parse time. So `max_json_depth` enforces and
`nesting_depth` does nothing, for what the config presents as one setting. The
warning names the working spelling for that reason; telling an operator only
that their key is dead leaves them unable to find the one that works.

### Why the checks were not simply wired in

This is the part that assuming additivity would have got wrong. The dead checks
enforce at **HARD** — exit 3, uncatchable — and **32 config sites in this repo
already set these keys**, including:

- both `govern-template.json` copies, the files users are told to copy;
- `tests/gorilla/naab-32/phases/phase1-hardening.json`, which sets
  `dict_size: 50` and `nesting_depth: 8` — tight enough to block ordinary data,
  and passing today *only because the check is dead*.

Wiring them in would start hard-blocking configs that pass today, including the
project's own template and one of its own tests. That is the third time in this
campaign that the safe-looking direction turned out to be a behaviour change
wearing a bug fix's clothes, after default-on secret scanning and
`restrictions.*.enabled`. So again only the silence is fixed: the three keys now
warn, and nothing about what is enforced changes.

### The second shape found nothing, which is worth recording

The checksheet names two shapes. Shape one (a check whose method nothing calls)
produced the five above. Shape two — *a check reading state that a loader
normalizes away*, the `CONTRA-007` shape — was swept across every
clear/erase/overwrite of parsed state in `governance_config.cpp` and yields
**exactly one instance, the already-fixed line 315**. Everything else is
clear-then-repopulate, which is ordinary. A negative result, recorded so the
sweep is not repeated.

A generalised version — config fields the loader writes that nothing reads —
returned 0 candidates under a conservative test and 139 under a loose one, both
useless: the loose test misses reads through local references, and the
conservative test cannot tell a live reader from a dead one. Only the
reachability-aware combination works, and it reproduces the hand analysis
exactly.

`tests/governance_v4/test_inert_limits.sh` covers the result, 8 gates. LD-04 to
LD-06 are the controls — three sibling keys in the same config block that *are*
enforced, so a warning that fired on the block would satisfy LD-01..03
completely. LD-08 is the substantive one: it asserts the warning is *true*, by
showing a tiny `string_length` does not block a long string while a tiny
`max_json_depth` does block deep JSON. Its first degradation attempt was itself
wrong — it "wired in" `string_length` by setting the JSON depth, which LD-08's
string test cannot see — and the degradation that works breaks the live half
instead, collapsing the comparison to two absences.

## Blast-radius audit of a shared test helper

Changing `agent_stub.py` to report `promptTokenCount` derived from the real
request body (#142) altered a helper used by **38 suites**. The full suite came
back 441/441, which says nothing on its own: a suite that quietly begins
exercising a different path while still passing is invisible, and that is
exactly how `test_quarantine_corroboration.sh` had been holding its premise by
accident for however long the constant had been there.

**Method.** Run the whole suite twice — once with the old constant restored,
once shipped — normalise timestamps, temp paths and PIDs, and diff at line
granularity rather than comparing totals.

**Result: byte-identical.** 3948 lines each way, 24 differing lines, every one a
process id inside a temp path (`gov001_32601` vs `gov001_12046`). No suite
changed behaviour.

**That result is worthless without the next step,** because "no diff" and "an
instrument that cannot detect a diff" are the same observation — the failure
this document is largely a catalogue of. So the stub was mutated to report a
flatly absurd `promptTokenCount` of 50000 and the suite run a third time:

| arm | unexpected failures | diff vs shipped |
|---|---|---|
| old constant (10) | 0 | 24 lines, all PIDs |
| **shipped** (body-derived) | **0** | — |
| mutant (50000) | **20** | 850 lines |

The instrument is sensitive. Twenty suites detect input-token magnitude, and the
shipped values perturb none of them.

**The prediction going in was wrong**, and in the more useful direction: the
expectation was that the mutant diff would ALSO be empty, on the theory that
these suites are short enough that nothing input-token-keyed can fire, making
the whole class untested rather than unaffected. Twenty failures refute that.
The suites are sensitive; the change is genuinely benign.

**Byproduct worth keeping — the blast-radius map.** Any future change to
input-token reporting should expect these twenty to move, and they are the
twenty to run first:

    absorption_degenerate, adversarial_detection, cb_pressure_counter,
    challenge_fail_path, code_aware_keywords, deescalation_hysteresis,
    deescalation_multiagent, entity_window, failure_mode_coverage,
    instruction_recall_windowing, output_admissibility, per_agent_signals,
    propose_commit, quarantine_corroboration, signal_discrimination,
    split_commit, tool_admissibility_gate, truncation_exposure,
    validation_signal, velocity_no_double_count

Six further suites pin `input_tokens` explicitly and are immune by construction
(`bounded_healing`, `developer_blindspot`, `persona_window`, `pulse_uniformity`,
`shell_content_split`, `thinking_reported`).

**What this does NOT establish.** The comparison is on OUTPUT. A suite that
takes a different internal path while producing identical output does not
appear, and no claim is made that none does.

## De-escalation is unreachable for a real agent, and that is now measured

Six keyed runs of `living-script_v3` were spent trying to observe the
de-escalation hysteresis live. It never fired. The useful result is not the
absence — it is that the absence stopped being a scenario shortfall and became
a measured property of the interaction between the composite formula and how a
real model actually behaves.

**The formula, confirmed exactly.** For floored coherence and saturated depth,

    composite = 0.35·(1 − coherence) + 0.10·depth + 0.25·min(1, signals/4)

reproduces **every** row of runs 4, 5 and 6, including the fractional values at
turn 12 where depth is still `(turn−1)/12` rather than 1. With coherence at 0.0
the first term is pinned at 0.35, so the floor is 0.45 and leaving HIGH (hold
0.55) requires **≤1 signal for 3 consecutive turns**. Two signals is 0.575.

**What real traffic supplies.** Across the three runs there are 84
post-escalation worker turns:

| run | ≤1-signal turns | longest consecutive | min pressure |
|---|---|---|---|
| 4 | 1 of 25 | 1 | 0.5125 |
| 5 | 0 of 18 | 0 | 0.5417 |
| 6 | 3 of 29 | 1 | 0.4958 |

Four turns in 84, **never two in a row**, against a requirement of three. In run
6 `entity_consistency` fired on 34 of 36 turns and `semantic_stability` on 29.
At ~5% of turns quiet enough, three consecutive is ~0.01% — it does not happen
at any run length, and a longer recovery phase does not help.

**Four prompt revisions did not move it, and that is the evidence.** Runs 3-6
each changed the scenario's prompts on a different causal theory: repeated
prompts cause repetition signals (true — `response_repetition` and
`circular_actions` fired 8 times each in run 3 and vanished when prompts were
varied); varied prompts should therefore quiet the signals (false). Varying them
merely moved which signal fired. `semantic_stability` measures Jaccard overlap
between CONSECUTIVE RESPONSES, so a phase that varies its prompts guarantees
consecutive answers share less vocabulary. Repeat the prompt and repetition
signals fire; vary it and stability fires. Two signals either way.

That is why this is recorded as a finding rather than fixed by a fifth revision.
The scenario is not failing to be quiet enough; **a working agent doing varied
work is not quiet by this measure at all.**

**What it does NOT say.** Escalation works — the ladder reached ELEVATED and
HIGH on real drift in every run, attributed to the right handle. The hysteresis
code is not shown to be broken; it is shown to have a precondition that real
traffic does not supply once coherence floors. And nothing here is an engine
defect on its face: it is a design question about whether scrutiny should be
sheddable by an agent whose ordinary output keeps two signals lit.

Filed as **C1c**, which generalises C1a from `elevated → normal` to
de-escalation from any level. The three available remedies are all loosenings
(widen the per-level margin, count calm as a rate rather than a consecutive run,
or require healing — which R3 withdrew as globally unsettable and R5 supersedes),
so none should be taken off this evidence alone.

**Also confirmed live across these runs, five times each:** the coherence
conservation invariant (`coherence == 1 − damage + healed`, residual
0.000000000000 on every snapshot of every run) and S20 silence after the prompt
reword. Run 6 is also the first keyed run to complete end to end, after the
`retry` gap was closed — runs 1-5 died at a timeout, a token budget, and three
API errors respectively.

## A compliant agent doing ordinary work floors its coherence in six turns

This is the largest result of the v3 campaign and it was nearly tuned away.

`living-script_v3` was built on the premise that drift would come from TASK
STRUCTURE: DESIGN and IMPLEMENT are ordinary well-specified work, DRIFT_PRESSURE
is genuinely under-specified, and the ladder should escalate during the third
phase. Worker turns are 1-3 DESIGN, 4-6 IMPLEMENT, 7-14 DRIFT_PRESSURE,
15-36 DRIFT_RECOVERY.

**Against a real model the premise is false.** Coherence across three keyed runs:

| turn | 1 | 2 | 3 (end DESIGN) | 4 | 5 | 6 (end IMPLEMENT) | 7 (drift starts) |
|---|---|---|---|---|---|---|---|
| run 4 | 1.00 | 0.91 | 0.74 | 0.56 | 0.48 | 0.21 | 0.03 |
| run 5 | 1.00 | 0.82 | 0.64 | 0.46 | 0.29 | 0.21 | 0.03 |
| run 6 | 1.00 | 0.83 | 0.56 | 0.29 | 0.20 | 0.03 | 0.00 |

Ordinary work consumed **79%, 79% and 97%** of the agent's coherence before the
drift phase sent a single prompt. By turn 7-8 the agent is floored at 0.00, and
the escalation to ELEVATED at turn 7 (runs 4 and 6) or 8 (run 5) was already
inevitable. DRIFT_PRESSURE had nothing left to damage; the phase built to cause
the escalation did not cause it.

The prompts that did this are not adversarial and were not trying to be:
*"Outline the Calculator class design covering the add and subtract methods and
the history entry recording"*, then *"Implement the Calculator class add method,
recording each history entry"*. Six turns of that, and the agent is at zero.

**What fires is the ordinary variation between one good answer and the next.**
`entity_consistency` fired on 34 of 36 turns in run 6 and `semantic_stability` on
29 -- both from consecutive responses about different Calculator methods sharing
less vocabulary than the thresholds want. No agent misbehaved in any of the six
runs.

### Why this was nearly lost

V3-03 asserts the DESIGN phase is a control. It failed three consecutive keyed
runs, and across those runs four remedies were considered, ALL of which would
have suppressed the finding to get a green:

1. revert the gate to its pre-#142 form ("no level change before turn 3"), which
   passed this exact situation while printing "the baseline is a baseline";
2. fit a coherence floor threshold chosen after seeing 0.74 and 0.64;
3. delete the gate as unsatisfiable;
4. revise the prompts a fifth time.

The gate is correct. What it reports is not a mis-tuned scenario -- it is **CDD
at default thresholds failing to distinguish ordinary varied coding work from
drift**, on three independent live runs. The failure message now says so, and
carries the two coherence figures, so the red is self-explanatory rather than
something a later reader tunes away. This is the one place in the campaign where
a permanently-red gate is the right outcome: it is red about its own subject.

### It also reframes C1c

C1c described de-escalation as unreachable "once coherence floors". The floor is
not caused by misbehaviour. An agent that never misbehaved is floored by turn 7
doing exactly what it was asked, and is then held at HIGH for the rest of the run
by the same two signals its ordinary output keeps lit. The de-escalation finding
and this one are the same mechanism seen from two ends.

### What it does NOT establish

Signal *thresholds* are the suspect, not the signal set: `semantic_stability`
(0.25 Jaccard) and `entity_consistency` (0.25 over a 5-sighting window) may
simply be tuned for prose agents rather than code agents, and the code-aware
keyword extractor exists precisely because that distinction has bitten before.
Nothing here has isolated which threshold is wrong, or measured a non-coding
agent as a control, and no threshold should be moved on this evidence alone --
lowering a detector's sensitivity because a scenario tripped it is how a
governance system is quietly disarmed.

## The C1d modality control, and why it is inconclusive

C1d needs a location: is CDD miscalibrated for CODE output, or for varied work of
any kind? The two signals responsible — `semantic_stability` and
`entity_consistency` — read the RESPONSE STREAM and nothing else, so the control
looked answerable without an API: run the identical scenario twice, changing only
the responses. `examples/living-script_v3/probe_modality.sh` does that, with
`V3_FIXTURE` overriding the generated stream.

Neither arm is authored. Both signals measure vocabulary overlap between
consecutive responses, so an author who writes the arms is choosing the quantity
under measurement and gets back whatever they expected. Both arms are lifted
verbatim from repository text written for other purposes.

**It does not work, and the probe says so rather than returning a verdict.**

| attempt | stimulus | floored at | live reference |
|---|---|---|---|
| 1 | chunks across MANY src/*.cpp and docs/*.md | turn 3-4 | turn 7-8 |
| 2 | consecutive chunks of ONE large file each | turn 4 | turn 7-8 |

Both attempts are HARSHER than the thing they model. A real agent's turns are
successive work on one task, so its responses restate the same domain vocabulary;
consecutive slices of a source file jump between unrelated functions. "Both arms
floor" is what any sufficiently heterogeneous stream produces regardless of
modality — it is not a result about ordinary work.

**The calibration guard is the reusable part.** The first version of the probe
checked only the too-gentle failure (neither arm floors) and would have reported
attempt 1 as a clean positive: *both arms floor, therefore C1d is not about code*.
Too gentle is easy to notice because nothing happens. **Too harsh looks like a
result**, which is worse, and it is the one that fired. The probe now refuses to
compare arms whose floor turn is far below the live reference.

**Stopped rather than tuned.** The stimulus could be made to floor at turn 7 —
sliding windows, overlapping chunks, hand-picked passages — but that is tuning a
stimulus against a target already known, which manufactures whichever verdict it
is tuned toward. Same failure as retuning a scenario until a gate goes green, one
level up.

**One lead, deliberately not a finding.** With length and document scope matched,
`semantic_stability` fired 12/36 for code and 35/36 for prose. If that survives
proper calibration it points the OPPOSITE way from the hypothesis — the
code-aware extractor helping code and leaving prose worse off. Measured under a
stimulus known to be off-calibration, so it is a lead only.

**What would settle C1d's location:** a keyed run of the same scenario shape with
a prose agent — same phases, same turn counts, same thresholds, a documentation
task instead of a Calculator. That is the control this probe was trying to buy
cheaply, and it cannot be bought cheaply with repository text.

## C1d located: it is not about code, and prose is worse

The modality control ran keyed on the prose arm (overlay confirmed applied to all
three system prompts; run header reads `keyed, prose arm`). The answer inverts
the hypothesis the control was built to test.

| | turn 3 | turn 6 | floored | escalated |
|---|---|---|---|---|
| code (keyed runs 4/5/6) | 0.74 / 0.64 / 0.56 | 0.21 / 0.21 / 0.03 | turn 7-8 | turn 7-8 |
| **prose** (keyed) | **0.46** | **0.00** | **turn 5** | **turn 5** |

Prose lands outside the code arm's three-run spread at turn 3 and is fully
floored two to three turns earlier. That separation is what makes a single prose
run worth reading at all: the code arm varied 0.74/0.64/0.56 on identical config,
so anything inside that band would have settled nothing.

**The signal mix says where it comes from:**

| signal | code (run 6) | prose |
|---|---|---|
| `semantic_stability` | 29/36 | **35/36** |
| `entity_consistency` | 34/36 | 35/36 |
| `plan_drift` | 14/36 | **29/36** |

So **C1d is not about code output.** Ordinary varied work of any modality trips
these thresholds and prose trips them harder. The code-aware keyword extractor
(`include/naab/keyword_extract.h`) appears to be doing its job -- emitting
component words for camelCase and letter-digit boundaries keeps code answers in a
denser shared token space, which is plausibly why the coding agent survived two
turns longer than the prose one. The suspects are the **0.25 defaults** on
`semantic_stability` and `entity_consistency`, not the extractor.

**The keyless probe was directionally right despite being off-calibration.** It
reported S10 at 12/36 code vs 35/36 prose and was recorded as a lead explicitly
not to be trusted, because its stimulus floored at turn 4 against a live 7-8. The
live arms give 29 vs 35 -- same ordering, compressed magnitude. Worth noting for
calibration judgement: an over-severe stimulus preserved the sign and destroyed
the scale. It was still right to refuse the verdict; the guard cost one prose run
and bought a result that does not depend on trusting a broken instrument.

### Caveats that survive the result

- **n=1 for prose against n=3 for code.** The separation is clean and outside the
  code band, but a second prose run is what would make it robust.
- **Response length is a real confound.** Prose input-token baseline was 1717
  against the code arm's 1220, and peak 9051 against 7777 — handbook sections are
  wordier than code snippets. Longer responses mean larger keyword sets, which
  changes Jaccard dynamics independently of modality. This finding does NOT
  isolate modality from verbosity, and a proper follow-up would match response
  length between arms.
- **Still no threshold should move on this.** The finding says the defaults are
  the suspect and the extractor is not; it does not say what either threshold
  should be, and lowering a detector's sensitivity because two scenarios tripped
  it is how a governance system is quietly disarmed.
