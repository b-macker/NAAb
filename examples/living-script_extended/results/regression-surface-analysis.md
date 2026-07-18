# Regression-Surface Analysis: the Jul 15 → Jul 17 living-script_extended runs

**Question.** The Jul 15 run of `examples/living-script_extended` finished 104/105
harness checks, 4/4 features, final verdict APPROVED, developer coherence floor 0.915.
Two runs on Jul 17 finished, respectively: 1/4 features + REJECTED + three
token-exhaustion errors (coherence floor 0.65), and a mid-run hard kill
(`Agent exceeded maximum quarantine streak`, coherence 0.44) that failed 66 harness
checks. The initial post-mortem attributed this to "model-side variance, not an
engine regression." This document re-examines that conclusion by tracing the
**actual regression surface** of every change that landed between the two run
binaries, instead of assuming the changes were additive/orthogonal to the run.

**Verdict up front.** The variance-only conclusion is wrong in attribution. Of the
three commits in the delta, two have provably empty surfaces for this run — but the
third (`c254cc9`, PR #85) changed the run's own success criteria *and* its
coherence-penalty economy, and those changes interact with a pre-existing
quarantine-streak kill switch that committed pre-#85 telemetry shows was already
being approached within one step. The Jul 17 outcomes are: (a) largely a
**measurement change** (the feature score), (b) a **new interaction**
(S22 × output-admissibility streak — the run-2 kill), and (c) **residual model
variance** (which trajectory a given run draws). Details and evidence below.

---

## 1. The delta set

The Jul 15 approved run is "Run #38" — the run whose analysis *produced* PR #85
(the commit message of `c254cc9` says so explicitly). It therefore predates all
three commits in the delta:

| Commit | Landed | Summary |
|---|---|---|
| `c254cc9` (#85) | Jul 15 22:15 | S22 `validation_outcome` CDD signal; living-script validation gating; telemetry attribution; developer budget 600K→900K |
| `66edf61` (#86) | Jul 16 19:17 | `http_impl.cpp` CURLOPT_NOSIGNAL + connect timeout; test safety nets |
| `ae78b7f` (#86) | Jul 16 21:33 | RLIMIT_NPROC soft-only fix in `python_c_executor.cpp` |

Both Jul 17 runs executed on a binary containing all three (branch head `bf7a762`).

## 2. Per-commit surface trace

### 2.1 `66edf61` (curl NOSIGNAL / connect timeout) — surface: **EMPTY**

- The agent API path does **not** go through `http_impl.cpp`. `agent_provider.cpp`
  builds its own handle (`curl_easy_init()` at `src/runtime/agent_provider.cpp:89`)
  and sets neither `CURLOPT_NOSIGNAL` nor `CURLOPT_CONNECTTIMEOUT_MS`
  (full option list at `agent_provider.cpp:89-140`). The commit's curl changes are
  confined to the stdlib `http` module.
- `living-script.naab` contains no `http.` calls and no `<<python>>` blocks
  (verified by grep over `examples/living-script_extended/src/`).
- The `run-all-tests.sh` and `test_taint_polyglot_vm001.sh` hunks are outside
  `run.sh`'s execution path.

**Conclusion: zero mechanism by which this commit changes living-script behavior.**

### 2.2 `ae78b7f` (RLIMIT_NPROC soft-only) — surface: **EMPTY for these runs**

The pre-fix bug was real and severe (`setrlimit(RLIMIT_NPROC, {0,0})` zeroes the
hard limit irreversibly for non-root; the RAII restore then fails with EPERM,
leaving the whole process unable to fork or create threads). But the guard only
activates when the current sandbox blocks fork:

- `python_c_executor.cpp:210-216` — the guard is gated on
  `SubprocessContainment::fromCurrentSandbox("python3").block_fork`.
- `subprocess_helpers.cpp:216` — `block_fork = !config.allow_fork`.
- `sandbox.cpp:86` — the **ELEVATED** level sets `allow_fork = true`.
- `examples/living-script_extended/src/govern.json:22` —
  `"sandbox_level": "elevated"`, and the script's mid-run govern.json rewrites
  never touch `sandbox_level` (its `allowed_agent_fields` allowlist at
  `living-script.naab:127` covers only context/token/lease fields).

So in every living-script run — before and after the fix — the NPROC guard was
**inert**: `block_fork` is false under `elevated`, the `setrlimit` never executes,
and neither the bug nor its fix could affect `codegen.run_strict("python", ...)`
(line 820) or the `process.run("python3", -m pytest)` validation path (line 269).
The bug bit `run-all-tests.sh` (which runs governance tests under the default
`standard` sandbox), which is exactly where it was discovered.

**Conclusion: no behavioral delta for living-script from this commit.**

### 2.3 `c254cc9` (#85) — surface: **NON-EMPTY, three distinct mechanisms**

#### (a) Measurement change: `FEATURE|N|complete` is now gated on pytest

Pre-#85, the script printed `FEATURE|N|complete` **unconditionally** after each
feature cycle. #85 gates it on a re-validated pytest pass
(`living-script.naab:1059-1077`) and emits `|incomplete` otherwise.

The committed Jul 13 baseline run (`results_20260713_220122.txt`) shows what the
old ruler hid:

```
VALIDATE|feat2|pytest_passed=false   →  FEATURE|2|complete
VALIDATE|feat3|pytest_passed=false   →  FEATURE|3|complete
VALIDATE|feat4|pytest_passed=false   →  FEATURE|4|complete
```

8 of that run's 10 pytest executions exited non-zero, and the final state of the
evolved code still failed 4 of 14 tests — yet the run scored "4/4 features
complete" and passed its harness. **Under the post-#85 ruler, the Jul 13 baseline
run would have scored 1/4 — identical to the Jul 17 run-1 score that was read as a
regression.** The Jul 15 run's pytest line-level status cannot be verified from
committed evidence (its result files exist only on the Termux device), but the only
committed pre-#85 run demonstrates that "4/4 complete" and "tests actually pass"
were decoupled claims. The features-complete delta between Jul 15 and Jul 17 is
therefore not evidence of a behavioral regression; the two runs were scored with
different rulers.

#### (b) Penalty-economy change: S22 makes a pre-existing kill path reachable

New in #85: the script feeds ground truth into the drift analyzer —
`agent.record_validation(handles["developer"], feat_passed)`
(`living-script.naab:1059`) — and the engine charges for it:

- S22 fires on a latched failing validation and subtracts a **flat**
  `base_penalty(weights.validation_outcome, …)` — deliberately not
  baseline-absorbed (`behavioral_sequence.cpp:1344-1360`).
- Default weight **0.15** (`governance.h:1381`); the living-script govern.json's
  `context_drift.weights` block does not override it.
- With `rate_normalized: false` (govern.json:242), `base_penalty` returns the raw
  weight: **each failed feature validation costs the developer 0.15 coherence.**
- The penalty is asymmetric: a *passing* validation consumes the latch silently —
  there is no recovery credit on the pass branch (`behavioral_sequence.cpp:1345`
  penalizes only `!last_validation_passed`). Recovery comes only from natural
  healing (+0.03/clean check, govern.json `coherence_natural_healing`) and step-up
  challenge passes (+0.25, `coherence_recovery_amount`).

This lands on top of an output-admissibility configuration that **predates #85**
(#85's govern.json diff touches only `max_total_tokens`):

```json
"output_admissibility": { "enabled": true, "threshold": 0.60,
  "action": "quarantine", "max_quarantine_streak": 5 }
```

The OA gate compares raw CDD coherence to 0.60 (`governance_engine.cpp:6452-6530`);
five consecutive quarantined sends throw `GovernanceHardError` and kill the agent.

**The critical baseline fact:** the pre-#85 economy was *already* driving the
developer to the edge of this kill switch. In the committed Jul 13 telemetry
(`telemetry_20260713_220122.jsonl`), developer handle 3 decays
0.945 → 0.83 → 0.74 → 0.60 → **0.4042** across turns 14–22, producing
**4 consecutive OA quarantines (turns 20–23)** — one short of the streak-5 kill —
with `VALIDATION_RECORDED: 0` (S22 did not exist). The drain came from
`context_growth` (0.07, firing on effectively every check as fix-cycle context
balloons past 3× baseline) compounding with `instruction_recall`,
`instruction_conflict`, and `persona_fingerprint`.

Post-#85 arithmetic: the same fix-cycle churn now *additionally* pays a flat 0.15
per feature whose tests still fail. Three failed features ≈ −0.45 on top of the
context-growth drain, against +0.03/turn healing. A trajectory that pre-#85 dipped
to ~0.40 and survived at streak 4 now goes deeper and stays under 0.60 longer —
crossing streak 5. The observed Jul 17 numbers fit: run 1's floor of 0.65 is
consistent with ~2 consumed validation failures on an otherwise-clean trajectory
(0.95 − 0.30), and run 2's 0.44-with-kill matches the Jul 13 dip shape plus S22's
extra drain extending the sub-threshold streak past 5.

**Conclusion: the run-2 death is not a new bug and not pure model variance — it is
the pre-existing OA streak boundary, which pre-#85 runs already grazed (4/5),
made reliably reachable by S22's flat penalty. Each piece is individually
defensible (S22's flatness is by design; the streak limit is by design; the
threshold predates both); their composition changed the run's survival envelope.
That composition is precisely what the additivity assumption missed.**

Two secondary notes on the S22 mechanics for future tuning:

- S22 is gated on `!in_baseline` like every other signal
  (`behavioral_sequence.cpp:1348`, `:858`), so with
  `adaptive_baseline_enabled: true, adaptive_baseline_window: 3` (govern.json)
  a validation failure consumed during the first 3 analyzed checks of a handle is
  counted but **not charged** — the "never baseline-absorbed" property holds for
  the adaptive *modulation*, not the baseline *window*.
- With `check_interval_turns: 2`, the latch (`has_validation_result`) is consumed
  at the next CDD analysis; two `record_validation` calls between analyses
  overwrite each other — only the last outcome is charged.

#### (c) Budget change: 600K → 900K, and it still exhausted

#85 raised developer `max_total_tokens` from 600K to 900K (govern.json:92) "to
give the two-pass cross-run workload headroom." Run 1 on Jul 17 exhausted 900K —
consumption grew past even the +50% headroom. Committed pre-#85 telemetry has no
token fields of this vintage, so run-level attribution rests on the prior
session's measurements (715K on Jul 15 vs 1.13M on Jul 17). Directionally,
#85 raises expected consumption through mechanisms that scale with low coherence:
re-validation adds a pytest cycle per initially-failing feature; sub-0.85
coherence triggers coherence-correction injections
(`coherence_correction_threshold: 0.85`); quarantined responses under
`inadmissible_history: "commit"` still consume full tokens while making no
progress. A degrading run now burns tokens *faster* precisely because it is
degrading — the token exhaustion and the coherence collapse in run 1 are the same
event seen through two meters, not independent failures.

#### (d) The `context_strategy` hunk — cosmetic

The remaining script hunk in #85 (`living-script.naab:1619-1621`) fixes where an
INTROSPECTION-phase *print statement* reads `context_strategy` from (environment
top level instead of the `limits` dict, where it never existed). Display only; no
behavioral surface.

## 3. Revised attribution

| Observation (Jul 17 vs Jul 15) | Prior attribution | Corrected attribution |
|---|---|---|
| Features 4/4 → 1/4 | model produced worse code | **Measurement change** (#85 gating). The only committed pre-#85 run also scores 1/4 under the new ruler. |
| REJECTED verdict | model quality | Downstream of the feature score → largely measurement change. |
| Coherence floor 0.915 → 0.65 → 0.44 | model drift | **S22 flat penalties** (−0.15/failed feature) stacked on the pre-existing context-growth drain. Pre-#85 runs already hit 0.40 (Jul 13, committed) — the *floor* is not new; S22 makes reaching it systematic rather than trajectory-dependent. |
| Run-2 quarantine-streak kill | "governance working as designed" | True but incomplete: the streak boundary predates #85 and was already being grazed at 4/5. **#85 changed the kill probability, not the model.** |
| Token exhaustion at 900K | verbose retries (model) | Mixed: coupled to the coherence collapse via re-validation cycles, correction injections, and commit-mode quarantine burn — partially #85-induced. |
| Engine/harness assertions | not a regression | Confirmed — unchanged, 104/105 in run 1; run-2 harness failures are the downstream shadow of the mid-script kill, not independent check failures. |

Model variance remains real — it decides *which* trajectory a run draws (Jul 15
drew a clean one, Jul 13 and both Jul 17 runs drew churny ones), and it is why the
`memory_store`-vs-`store_memory` bug appeared at all. But variance selects the
input to the economy; #85 changed what the economy does with that input.

## 4. Recommendations (analysis only — no changes made)

1. **Record the binary's commit hash in every results file** (run.sh can emit
   `git rev-parse HEAD`). This entire investigation had to reconstruct which
   binary ran from commit timestamps; one line in the results header removes that
   ambiguity permanently.
2. **Decide the intended S22 × streak semantics.** If a persistently-failing
   feature is *supposed* to kill the run, the current behavior is correct and the
   Jul 17 run 2 is a pass, not a failure — then the harness should treat exit-3
   during FEATURE phases as an expected outcome rather than cascading 66 check
   failures. If it is *not* supposed to kill the run, consider either a recovery
   credit on passing validation (the current pass branch is silent) or excluding
   S22-driven quarantines from `max_quarantine_streak`.
3. **Revisit `context_growth` in fix-cycle-heavy phases.** It fires on virtually
   every check once fix cycles inflate context (it was the largest single
   contributor to the Jul 13 dip on committed evidence). Per-agent
   `context_window` limits would cap the input-token growth that feeds it.
4. **Re-baseline expectations under the new ruler.** "4/4 features complete" from
   pre-#85 runs is not comparable to post-#85 scores; historical comparisons
   should either re-derive old scores from their `VALIDATE|featN|pytest_passed`
   lines or bracket pre-#85 feature counts as unvalidated.

## 5. Evidence index

- Delta commits: `c254cc9`, `66edf61`, `ae78b7f` (all on `master` at `bf7a762`)
- Empty-surface proofs: `src/runtime/agent_provider.cpp:89-140`,
  `src/runtime/python_c_executor.cpp:196-218`,
  `src/runtime/subprocess_helpers.cpp:198-217`, `src/runtime/sandbox.cpp:68-92`,
  `examples/living-script_extended/src/govern.json:22`
- S22 mechanics: `src/runtime/behavioral_sequence.cpp:821-831,1338-1360,2349-2353`,
  `include/naab/governance.h:1345,1381`
- OA gate: `src/runtime/governance_engine.cpp:6452-6530`
- Pre-#85 baseline: `results_20260713_220122.txt`,
  `telemetry_20260713_220122.jsonl` (this directory)
- Jul 15 / Jul 17 run figures: measured in the prior analysis session from files
  on the originating device (not committed to this repository); marked as such
  wherever used.

---

## Follow-up: the Jul 18 run

The Jul 18 run executed on the harness fixes from this branch (105/105 checks,
first-ever L5-07 pass) and provided the cleanest evidence yet for where the
remaining failure mass lives. All Jul 18 figures below were measured in the
follow-up forensic session from files on the originating device (not committed
here), like the Jul 15/17 figures above.

### Four-run comparison

| Metric | Jul 15 | Jul 17 r1 | Jul 17 r2 | Jul 18 |
|---|---|---|---|---|
| Harness pass/fail/skip | 104/0/1 | 104/0/1 | 45/66/8¹ | 105/0/0 |
| Features complete | 4/4² | 1/4 | crashed | 2/4 |
| Final approved | true | false | — | false |
| Send errors | 0 | 3 | crash | 0 |
| Dev min coherence | 0.915 | 0.65 | 0.44 | 0.7275 |
| Health | healthy | healthy | — | degraded |
| Challenges pass/fail | 8/0 | 2/0 | 3/0 | 15/1³ |

¹ Pre-dates the governance-kill harness handling added on this branch.
² Pre-#85 unconditional "complete" — see §2.3(a); not comparable to later runs.
³ Telemetry count across all run_id segments; the results file under-reported
(12) because per-handle `AGENT_STATE` counters reset between the two script
invocations — closed by the telemetry-wide challenge count now emitted by both
harnesses.

### What the Jul 18 forensics established

1. **S22 is the dominant coherence killer, exactly as designed and exactly as
   predicted in §2.3(b).** The developer's fatal cascade (T22: −0.15
   `validation_outcome` → 0.805; T26: triple-stack → 0.7275) tracks repeated
   pytest failures on a division-by-zero defect the developer could not fix.
   No quarantine, no OA fail (floor 0.7275 > 0.60 threshold) — the run survived
   but was rejected.
2. **The penalty loop had no exit.** A passing validation earned zero credit
   (`behavioral_sequence.cpp` S22 pass branch was empty), so even a fixed
   feature could not recover what its failures cost. Addressed on this branch:
   fail→pass transitions now credit `context_drift.validation_recovery_amount`
   (default 0.075; transition-gated so credits ≤ failures; ratchet-protected).
3. **Challenges validated recall, not competence.** The developer passed
   step-up challenges at 0.667 keyword overlap (T25) while its code failed
   pytest — challenges quizzed instructions the agent remembered perfectly.
   Addressed on this branch: new priority-0 `validation` contextual challenge
   type, grounded in the failure detail now carried by
   `agent.record_validation(handle, passed, detail)`.
4. **The fix loop starved the model of the defect.** The pytest-fix prompt sent
   only FAILED test *names* — the developer never saw `ZeroDivisionError` vs
   the expected exception type, and failed the same fix twice. Addressed on
   this branch: fix prompts now include pytest short-traceback `E `-lines
   (capped), and the developer system_prompt + test prompts pin a single
   exception convention (caller-facing errors are always `ValueError`),
   removing the feature-3/4 exception-type mismatch class.
5. **Features 3–4 remain a model-capability ceiling.** Across every run since
   Jul 15, `evaluate()` and the plugin system defeat the model while features
   1–2 complete. The interventions above shrink the blast radius of that
   ceiling (recoverable coherence, defect-grounded challenges, actionable fix
   prompts); they do not raise the ceiling itself.

### Evidence index (Jul 18 additions)

- S22 recovery: `src/runtime/behavioral_sequence.cpp` (S22 consume block),
  `context_drift.validation_recovery_amount` in `include/naab/governance.h`,
  ratchet in `src/runtime/governance_config.cpp` (`compareForRatchet`)
- Validation challenge: `src/stdlib/agent_impl.cpp` (priority-0 selection,
  `agentRecordValidation` detail arg), `DriftState.validation_failure_keywords`
- Script/prompt changes: both `examples/living-script*/src/living-script.naab`
  and `src/govern.json` (exception-convention invariant, `E `-line detail,
  `record_validation` detail arg)
- Jul 18 run figures: follow-up forensic session, originating device.

---

## Round 3: post-#88 live confirmation and the feature-2 contract fix

The first live run on the #88 changes went APPROVED (105/105 harness, 3/4
features, coherence floor 0.86, tokens back to 746K). Forensics on that run
(follow-up session, originating device) confirmed each intervention with exact
values: one S22 recovery of +0.075 fired only on the fail→pass transition
(VR2→VR3), a `validation`-type challenge fired grounded in 9 failure-detail
keywords and passed at 0.667, feature 3's division-by-zero and feature 4's
exception types were correct on first attempt. The "ghost run" in that
telemetry file is the harness's own L3-03 cross-run invocation (second
`run_id` segment in the same file, by design) — analyses must filter by
`run_id`.

**Feature 2's coin flip had a locatable cause.** The per-feature reminder
`"memory_recall() returns None when no value is stored (do NOT raise
ValueError)"` was appended to `invariants` AFTER that feature's ops and test
prompts had already been sent — so it reached only the fix prompts and later
features, while the test prompt's blanket "invalid input MUST be tested with
pytest.raises(ValueError)" line pushed the tests the opposite way, and the
test prompt never saw the ops code at all. Two independently-derived,
contradictory contracts.

Interventions in this round (scripts/harness/tests only — no engine changes):

1. **Shared per-feature CONTRACT**: error semantics for each feature are now
   one string computed at feature start and included verbatim in the ops
   prompt, the test prompt, and the fix prompts; the test prompt additionally
   embeds the current `operations.py` so tests are written against real
   signatures.
2. **Two-attempt fix loop**: PYTEST_FIX now retries up to twice with
   re-validation between attempts; attempt 2 shows both files plus the
   contract and may fix a test only where it contradicts the contract.
3. **Operator ground truth**: the post-feature operator consult now carries
   `validation_passed` and states complete/INCOMPLETE explicitly (the
   post-#88 run's operator had misread an incomplete feature as complete).
4. **`challenge_failure` kill kind**: a failed step-up challenge throws
   GovernanceHardError (exit 3) — the same designed-kill class as the
   quarantine streak. Both harnesses now classify it (`detect_gov_kill`),
   with GK-01 attributability against `AGENT_CHALLENGE_FAIL` telemetry.
5. **Deterministic challenge-failure coverage**:
   `tests/governance_v4/test_challenge_fail_path.sh` forces both challenge
   outcomes with the loopback stub (off-topic answer → FAIL → exit 3 +
   telemetry; on-topic answer → PASS → run completes), closing the gap where
   the fail path had never been exercised outside live-model luck.
