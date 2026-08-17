# Open investigations

A checksheet, not a narrative. `governance-campaign-findings.md` records what was
found and why; this records **what has not been looked at yet**, so it does not
have to be rediscovered from a chat log.

Each item: what to check, why it might matter, and what would settle it. Status
is one of **open**, **in progress**, **parked** (deliberate, with a reason), or
**done** (move the finding to the campaign doc and delete the row).

---

## A. Inert mechanisms — the high-yield category

Two engine features have now been found **configured, believed working, and
structurally incapable of firing**:

- `CONTRA-007` iterated a set its own loader empties at parse time, so the
  conflict it looks for cannot exist by the time it runs.
- The circuit-breaker ladder's middle levels were gated by a *second* threshold
  (default 0.70) sitting above their own (0.4 / 0.6), so ELEVATED and HIGH could
  never be reached through pressure.

Both were found by accident, from a different direction. Neither was found by
review. That base rate is the argument for looking systematically.

**The two shapes to search for:**
1. a check reading state that a loader, normalizer or resolver clears first;
2. a gate whose counter/condition is fed by a threshold other than its own.

| # | Item | Status | How to settle it |
|---|---|---|---|
| A1 | **6 of 9 contradiction checks have no test at all** — `CONTRA-001, 003, 004, 005, 008, 010`. Of the three that do, one (`007`) was dead. `CONTRA-006` does not exist; explain the gap. | **in progress** | All nine now confirmed to fire against a hand-built config (probed 2026-08-09) — none is a `CONTRA-007` repeat. Two notes: `CONTRA-001` is hardcoded SOFT, so it aborts the load and prints its description with an **empty `Rule:` field** — the pattern id never reaches the operator; and probing it turned up the `restrictions.*.enabled` finding below. Remaining: commit a coverage suite so this cannot regress, and fix the empty `Rule:`. |
| A1a | **A SOFT/HARD contradiction block does not name which pattern fired.** `CONTRA-001` at SOFT prints `Rule: ` empty. The ADVISORY path prints `contradiction.CONTRA-001` correctly. | open | Pass `rule_name` through the `formatError` call in `detectContradictions()`. Small, but it is the difference between an operator being able to look the pattern up and not. |
| A2 | **Systematic inert-mechanism sweep of the engine.** Beyond CONTRA: which other checks read state that is normalized away, or depend on a counter fed by a different threshold? | open | Enumerate `enforce()` / `recordPass()` call sites; for each, ask what input would make it fire and whether that input can reach it. |
| A3 | **Which `enforce()` sites can never fire?** | **done — see the campaign doc** | The evidence-mining version of this row was wrong and is recorded as such: cross-referencing 107 rule names against 21 MB of archived runs reported 95 "never fired", including five fired by hand an hour earlier. The archive is all `living-script`, so it measures coverage, not capability. **Source reachability** is the discriminator. Result: 5 check-bearing methods with zero call sites; 3 genuinely inert config keys; 1 enforcement-without-evidence gap; 1 dead duplicate. |
| A3a | **`limits.data.output_size` enforces without producing evidence.** `polyglot.cpp:718` throws a plain `std::runtime_error` rather than calling `enforce()`, so the block yields no governance finding, no telemetry, no report entry — and is catchable by NAAb `try/catch`, unlike the HARD block it resembles. | open | Routing it through `enforce()` changes behaviour (catchable → uncatchable). Needs the same decision as C1a. |
| A3c | **The bounded-healing ledger could be over-counted without detection** — G2 inflated `coherence_damage_total` and no gate failed. | **done** | Closed by BH-08, which asserts the exact conservation invariant `coherence == 1 - damage + healed`. Note the check originally specified here (`damage ≈ (1 - min_coherence_lifetime) + healed`) was **wrong** — it only holds if all healing follows the coherence minimum. Testing it is how that was found. |
| R5 | **Make healing relative rather than absolute.** `coherence_natural_healing` is a per-turn constant, so whether it suppresses escalation depends on the workload's damage rate — measured cliff at 0.50 for one profile and 0.15 for a lighter one. A rate capped at a fraction of the agent's own recent damage per damaging turn would self-scale and have no cliff. | open | New design; needs its own trace of the regression surface. The alternative is accepting that this knob cannot be set globally and documenting it, which is the current state. |
| V3a | **S20's `prompt_alignment` denominator is the MANDATE's keyword count, not the prompt's** (`behavioral_sequence.cpp:1899`). A short, perfectly on-topic prompt against a long mandate scores low and trips gate 1 before the response is examined. v3's original prompts fired S20 on **35 of 36 turns**, including phases where the agent was behaving correctly. | open | Fixed in v3 by wording every prompt on-mandate, but the engine behaviour is unchanged and will bite any operator whose mandate is longer than their prompts. Worth deciding whether the denominator should be the prompt's keyword count, or whether the threshold should scale with mandate length. |
| A3b | **Four dead check methods remain in the tree**: `checkStringLength`, `checkNestingDepth`, `checkDictSize`, `checkCodegenAllowed`. The first three back inert keys; the fourth duplicates live enforcement in `codegen_impl.cpp`. | open | Delete, or wire behind an opt-in. Wiring is **not** additive — see the campaign doc's regression surface. |

## B. Known engine behaviour, decided but unfixed

| # | Item | Status | Note |
|---|---|---|---|
| B1 | **Scrutiny does not survive a process restart.** `governance_level_` is `std::atomic<int>{0}` with no persistence and no restore path. An agent that escalated gets a clean slate in the next process — observed live (v1 Aug 8: segment 1 ended `elevated`, segment 2 began `normal`). A long-running system that restarts periodically can never accumulate scrutiny. | **open — undocumented** | Not yet written up anywhere. Decide: document as a boundary, or persist. Persisting raises scope, expiry and tamper-evidence questions that should not be answered off one run. |
| B2 | **`governance.telemetry` resolves CWD-relative while `govern.json` is discovered script-dir-relative.** An operator putting the two side by side gets telemetry wherever they happened to run from. | parked | Documented. Changing it moves files for every existing user. |
| B3 | **Response secret/PII scanning defaults off.** A config that never sets `code_quality.no_secrets` / `no_pii` has *no* response scanning. | parked | Deliberate: enabling by default would start blocking responses that pass today. Worth knowing if you assumed otherwise. |
| B4 | **CRITICAL is marginally harder to reach after #129** — needs `critical_sustained` turns at or above `critical_threshold`, rather than that many above 0.70 plus one spike. | parked | Accepted trade: severity should now be caught at ELEVATED/HIGH instead. Revisit if a live run shows CRITICAL missed where it was wanted. |
| B5 | **On DEFAULT settings, `context_growth` (S12) fires permanently from ~turn 8 with no recovery path.** Its baseline is the mean of the first `adaptive_baseline_window` (5) turns and is never revisited, because the EMA that would track natural growth is gated on `adaptive_baseline_enabled` — default **false**. With no `context_window` either, and `context_growth_factor` at 3.0, any long single-handle conversation exceeds 3× its own opening turns and stays there. Measured on v3 keyed run 3: fired on **29 of 29 turns** from turn 8. Because composite pressure has room for only ~1 signal below `high_threshold` once coherence floors, a permanently-firing signal consumes that slot and makes de-escalation unreachable. | **open — new, undocumented** | Classified during #142 as "a v3 config gap, not an engine defect", which is true of v3 specifically — v1 and `_extended` set `context_window` 20 **and** `context_growth_factor` 5.0, and a keyless 2×2 confirms both are required (window only 25 firings, factor only 17, both 0). But that answers *what v3 did wrong*, not *whether the defaults are right*. A signal that cannot stop firing is a permanent coherence tax rather than a drift detector, and the mitigation ships off. Decide: change a default, warn at load when the combination is unwindowed + non-adaptive + factor 3.0, or document as intended. **Do not close this by pointing at v1's config.** |

## C. Never observed working (stub-only)

Passing tests, no live run has produced the triggering condition. See the
campaign doc's live-status column.

| # | Item | Status | Blocker |
|---|---|---|---|
| C1 | **De-escalation hysteresis** | **observed working (stub)** | `living-script_v3` steps `high → elevated` at turn 18 — first observation of the hysteresis firing. Only the live run remains. |
| C1b | **Healing is inert at every shipped rate.** 25 configs set `coherence_natural_healing`, including both templates (0.02) and living-script v1/v2 (0.03). Varying only the rate on v3: 0.0 and 0.03 give **identical** results to the quarantine; 0.25 restores `elevated → normal`. `heal_factor = 1/(1+signals)` halves an already-small number against penalties of 0.08–0.15. | **addressed; R3 withdrawn** | Bound (R1/R2), channel-agnostic booking (S1/S2) and ratchet (R4) shipped. **R3 (raise the rate) is withdrawn on evidence**: the rate at which healing suppresses escalation is damage-relative, not absolute — HIGH is lost at 0.50 in v3 and at 0.15 in the same scenario with two signals disabled. No global constant is correct. See `docs/proposal-bounded-coherence-healing.md`. |
| C1a | **`elevated → normal` is unreachable once coherence floors.** With `coherence_natural_healing` at its default 0.0, coherence never climbs back, and `coherence_prox` alone holds composite above `elevated_threshold` — 18 quiet turns in the v3 run left it pinned at 0.5125 vs a 0.35 threshold. | open | A design question, not obviously a defect: scrutiny earned arguably should not be cheap to shed. But pair it with B1 (a process restart clears the level outright) and the combination is odd — unshakeable within a process, free by restarting it. Both available fixes are loosenings, so this needs a decision before code. **See C1c: live evidence now generalises this from `elevated → normal` to de-escalation from ANY level.** |
| C1d | **A compliant agent doing ordinary work floors its coherence in six turns — and it is NOT about code.** v3's DESIGN (1-3) and IMPLEMENT (4-6) are ordinary well-specified on-mandate work. Code arm (keyed runs 4/5/6): coherence 0.74/0.64/0.56 at turn 3, floored turn 7-8. **Prose arm (keyed, same config via `prose_overlay.json`, only the task differs): 0.46 at turn 3, floored turn 5** — outside the code arm's three-run spread and two turns earlier. Signals, code vs prose: `semantic_stability` 29/36 vs **35/36**, `plan_drift` 14/36 vs **29/36**, `entity_consistency` 34/36 vs 35/36. | **located; threshold decision open** | Ordinary varied work of ANY modality trips these thresholds and prose trips them harder, so the suspects are the **0.25 defaults** on `semantic_stability` and `entity_consistency` — NOT the code-aware extractor, which appears to be helping code. **Do not move a threshold on this**: it identifies the suspect, not the value. Two caveats stand — n=1 for prose against n=3 for code, and response length is unmatched (prose input baseline 1717 vs 1220), so modality is not isolated from verbosity. Settle by matching response length across arms and running prose twice. |
| C1c | **De-escalation is effectively unreachable for a real agent once coherence floors — measured, not inferred.** Composite reduces to `0.35·(1−coherence) + 0.10·depth + 0.25·min(1, signals/4)`, exact against every row of three keyed runs. With coherence at 0.0 the first term is permanently 0.35, so leaving HIGH (hold 0.55) requires **≤1 signal for 3 consecutive turns** — two signals is 0.575. Across keyed runs 4, 5 and 6 there are **84 post-escalation worker turns; 4 had ≤1 signal, and never two consecutively.** `entity_consistency` fired 34/36 and `semantic_stability` 29/36 in run 6. At ~5% of turns being quiet enough, three consecutive is ~0.01%: it does not happen at any run length. | **open — new, evidence-backed** | This is no longer "the scenario didn't give the mechanism its calm". A real model's responses fire 2+ CDD signals per turn as a matter of course, so the calm the hysteresis requires is not a thing real traffic produces. The decision is whether that is intended. Options, all of them loosenings and none obviously right: raise the per-level hold margin so 2 signals sits below it; count calm on a *rate* rather than a consecutive run; or accept that de-escalation requires healing (which R3 withdrew as unsettable globally, and R5 supersedes). **Do not "fix" this by tuning v3's prompts** — four prompt revisions across runs 3-6 each moved which signal fired without changing that two fire. |
| C2 | **HIGH and CRITICAL levels** | **HIGH live-confirmed, CRITICAL open** | HIGH reached keylessly at turn 5 on 2026-08-09, then **live against a real model on keyed run 2** (ELEVATED turn 8, HIGH turn 10, `drift_worker` the pressure handle at both) — see E6. CRITICAL was deliberately tuned out of reach in both. |
| C3 | **Five campaign findings** — pulse streak field, evidence-count shrink, lease expiry mid-deliberation, config generation guard, CRITICAL suspension | open | All five need adversarial conditions a clean run cannot produce. |
| C4 | **Reviewer separation of duties** (`allowed_actions: ["AGENT_SEND"]` only) | open | Never live-confirmed: no reviewer has yet *attempted* a tool call, so the absence of blocks proves nothing. |

## D. Test-harness debt

| # | Item | Status | Note |
|---|---|---|---|
| D1 | **30 suites still use the inline one-shot stub port pick** (`grep -rl 'RANDOM % 20000'`, re-counted 2026-08-09). The hardened launcher is shared (`tests/helpers/stub_launch.sh`); **22 suites now use it** (re-counted 2026-08-16), 30 still inline. The original note said one — that was true when written and is not now. | open | Expect more intermittent CI red. Repoint on touch; converting all 30 blind just repeats "freeze one guess into 29 callers" in the other direction. |
| D2 | **Tier 3 vacuity audit** — `tests/gorilla`, 62 absence-guarded assertions | parked | Older scenario tests, weakest claims. Tiers 1–2 done. |
| D3 | **Retrofit v1 and v2 to `gatelib.sh`** with per-gate negative fixtures | open | Roadmap increment. v1 has ~151 gates, v2 ~89. |
| D4 | **Keyless runs overwrite `summary.json`**, destroying a keyed record (already lost v2's 108/0/1 once) | open | Fixed in `gate_summary_json`; lands when v1/v2 are retrofitted. |
| D5 | **Keyless placeholder counts are hand-maintained and wrong** — `seq 1 155` (v1) / `seq 1 115` (v2) against real keyed totals 171 / 109 | open | Registry-driven skips fix this; same retrofit. |
| D6 | **No `PHASE\|X\|end` markers** — a phase is only ever "reached", so truncation mid-phase looks like completion | open | `phase_complete()` exists in gatelib; the `.naab` scripts must emit the end markers. |
| D7 | **v2 `summary.json` lacks the `failed` ID array** that v1 carries — a fail count with no identities cannot be triaged later | open | Same retrofit. |
| D8 | **Trust-store leak: cause now understood, fix still not applied.** The store is NOT written during a run — `~/.naab/trusted-keys` is dated Aug 3 and unchanged. What varies is that suites using `trust_setup.sh` repoint `NAAB_TRUST_STORE_DIR` while they run, so a probe issued during that window sees an empty store and succeeds, and the same probe outside it hits `INTEGRITY BLOCK`. | open | Not a leak — a missing isolation in the ~35 suites that write an unsigned `govern.json`. Repoint them at `trust_setup.sh` on touch. **Any ad-hoc probe against a dev container must isolate the store itself**, or its results are timing-dependent; this cost a wrong reading once already. |

## E. living-script v3 roadmap

Full design in the session plan file; increments 1–2 are merged.

| # | Increment | Status |
|---|---|---|
| E1 | Gate library + self-test driver (#133) | **done** |
| E2 | v2 pattern archaeology (#132, #134) | **done** |
| E3 | Per-agent routing in `agent_stub.py` | **done** |
| E4 | v3 scenario skeleton, ladder walked **keylessly** against the stub first | **done** |
| E5 | v3 gates + negative fixtures, `--self-test` green in CI | **done** — 11 gates, 11 negative fixtures, registered in `run-all-tests.sh` |
| E6 | One keyed run | **done — 6 runs; run 6 completed end to end** | Ladder escalation, the conservation invariant and S20 silence are all live-confirmed. **De-escalation IS now observed live** (2026-08-16, local Qwen2.5-0.5B via a loopback Gemini shim): HIGH->ELEVATED after three consecutive calm turns, with `deescalate_calm_turns` reading 1, 2, 3 and showing the trigger value on the firing turn. C1c's reasoning held for the keyed API runs; a slower, more drift-prone local model produced the calm window they never did. ELEVATED->NORMAL remains unobserved and is arithmetically unreachable at zero coherence — see the floor-lock entry in governance-campaign-findings.md. |

### E6 — the two keyed runs (2026-08-10)

Neither run finished. Both died on a limit that was *doing its job*, and in both
cases the first diagnosis named the wrong knob — the failure mode being two
similarly-named limits where the error text quotes one of them.

| run | died at | actual cause | first diagnosis (wrong) |
|---|---|---|---|
| 1 | 33s of telemetry, 11 of ~39 calls, mid-DRIFT_PRESSURE | `limits.timeout.global`, unset, **defaults to 30s** | `agent_dispatch.default_timeout_seconds` — that is the PER-CALL timeout and nothing reads it as a run limit |
| 2 | DRIFT_RECOVERY turn 1, 110070 tokens | per-agent `max_total_tokens`, unset, **defaults to 100000** (`governance.h:2262`) | `hard_stop.max_tokens_per_run` — set to 500000 in the committed config and never approached |

`run.sh`'s `timeout 600s` wrapper never governed anything in run 1: the engine
always died first, so the visible backstop was structurally unreachable. Both
limits are now set explicitly with `meta` entries recording the distinction.

**The messages themselves are fixed now** (`38366e5`). The per-agent budget error
names the budget that bound instead of quoting a number that fits two limits, at
both call sites — `agentSend` and `agentPropose`. Neither misdiagnosis above was
caused by a wrong value; every number printed was correct. They were caused by
the engine declining to say which thing the number belonged to, which is the
whole shape of the diagnostics work in `governance-campaign-findings.md`.

Run 1's timeout half is **not** fixed. `ErrorSanitizer` mangles
`max_tokens_per_run` into `max_<redacted>` at rendered output, so the run-level
error still does not name its key even though its source string does — recorded
under "Found in `src/`, not fixed" and pinned by `test_limit_attribution.sh`
LA-03. The `limits.timeout.global` message names no limit at all; giving it one
requires `ResourceLimiter` to retain its configured value, which it does not, and
touches the per-instruction VM dispatch macro. Separate increment, not done.

Cost model worth keeping: conversation history is resent every turn, so token
spend grows roughly with the **square** of turn count — 19 calls cost 110k
against a 36-call scenario, and `drift_worker` alone takes ~33 of those calls.

**What run 2 established before it died** — all of it live, against a real model,
none of it previously observed outside the stub:

- The ladder escalates on real drift: **ELEVATED at turn 8, HIGH at turn 10**,
  with `drift_worker` the pressure handle at both.
- The **conservation invariant holds against live traffic**: residual
  `0.000000000000` across all 16 snapshots
  (`coherence == 1 − damage + healed`).
- `prompt_compliance` (S20) fired **0 times**. The reworded prompts held against
  a real model — the earlier wording fired S20 on 35 of 36 turns, penalising the
  agent for the scenario's vocabulary rather than its behaviour.
- Zero infrastructure noise: no retries, no fallbacks, no rate-limit errors.

**Still unanswered: de-escalation live.** Run 2 died one turn into the recovery
phase, which is the only phase that could produce it.

**A gate of my own was mis-measuring while this ran.** V3-11 counted every
analyzed `CDD_TURN` after the first escalation as a "calm turn", so it reported
*"de-escalation did not fire despite 8 calm turns"* when all 8 fired 2–3 signals
each. A correct non-firing was reported as a governance failure — the mirror
image of the vacuity this harness exists to prevent, and the same root shape:
the gate's stated precondition and the thing it actually measured were not the
same predicate. Fixed to require empty `penalties_detail`.

**Increment 4b result (probed keylessly 2026-08-09, routed stub, 2 agents,
20 turns).** The ladder *does* walk: `normal → elevated` at turn 3,
`elevated → high` at turn 5, with the plan's tuning
(`check_interval_turns: 1`, elevated 0.35/2, high 0.55/3). **This is the first
time HIGH has been reached at all** — C2 above is no longer blocked on "needs
genuine drift", only on doing it live. Composite peaked at 0.70.

**Correction (same day, from the v3 run).** The probe's conclusion below —
"de-escalation is structurally unreachable" — was **wrong**, and wrong in an
instructive way. `living-script_v3` steps `high → elevated` at turn 18. The
hysteresis works; this is the first time it has been observed doing so.

The probe missed it because its recovery phase still fired ~2 signals a turn,
so composite floored at 0.567 — just above `high_threshold` 0.55. Nothing
stepped down because nothing ever dropped below a threshold, which is
indistinguishable from a mechanism that cannot fire. The lesson is the campaign's
own: *a mechanism that did not fire and a mechanism that cannot fire produce the
same observation*, and I took the stronger reading on one run.

**What is genuinely blocked is the LAST step, `elevated → normal`.** In the v3
run the handle is quiet from turn 18 to 36 — eighteen turns — with composite
pinned at exactly 0.5125, above `elevated_threshold` 0.35. With coherence
floored at 0.0 the `coherence_prox` term alone holds the composite up, and
`coherence_natural_healing` defaults to 0.0, so it never climbs back. An agent
that has once floored its coherence can return to HIGH-minus-one and no further,
for the life of the process.

The original probe notes follow, kept because the arithmetic in them is right
even though the conclusion drawn from it was too strong:

- `context_drift.coherence_natural_healing` defaults to **0.0**
  (`governance.h:1343`). With it off, coherence only ever decreases — the sole
  increase path is S22's fail→pass credit (`behavioral_sequence.cpp:1458`),
  which needs `agent.record_validation()`.
- Once coherence floored at 0.0, twelve consecutive calm turns brought composite
  down only to **0.567**, never near `elevated_threshold` 0.35. The level
  therefore cannot step down however well the agent behaves. The hysteresis
  machinery (`deescalate_sustained`, `deescalate_pressure_handle_`) is not
  reached because its input — a lower computed target — never occurs.
- Enabling healing at 0.06/turn was **not** enough: `heal_factor` is
  `1/(1 + signals_fired_this_turn)`, and ~2 signals kept firing through the calm
  phase, so healing stayed below the penalties.
- Part of that tail is `mandate_alignment`'s rolling window of 20 — a 6-turn
  drift keeps paying for roughly 20 calm turns. A v3 recovery phase of 8–10
  turns is too short by construction; it needs to outlast the window.

Two fixture bugs of my own were found and fixed on the way, both worth not
repeating: 14 identical "recovery" responses fire `response_repetition` every
turn (a recovery phase must be varied *and* on-mandate), and the first drift
fixture was so far off-mandate that coherence floored by turn 3, which destroys
the de-escalation observation before the recovery phase begins. Drift has to be
strong enough to escalate and mild enough to leave headroom.

**Open question this raises (new, not yet an item above):** is a permanently
pinned level the intended design? It is defensible — scrutiny earned should not
be cheap to shed — but combined with B1 (scrutiny does not survive a restart)
the pair is odd: unshakeable within a process, free to reset by restarting it.

**Two constraints verified in source; do not re-derive:**
- `check_interval_turns` **must be 1**. `behavioral_sequence.cpp:785` zeroes
  `signals_fired_this_turn`, and the interval gate returns at `:802` before
  anything can increment it — so every skipped turn contributes zero signal
  density *and decays* `cb_sustained`.
- Disabling CDD signals on all agents **except one** pins
  `deescalate_pressure_handle_` to that agent: a sibling with no signals caps at
  `risk 0.20 + depth 0.10 = 0.30`, below `elevated_threshold`, so it can never
  raise the level. This is what makes de-escalation testable at all.

## F. Standing rules earned the hard way

Not tasks — the things that keep being rediscovered.

- **A zero is only evidence if you can say what would have made it non-zero.**
  "Both counters read 0" does not show independence when neither gate was
  approached.
- **When you make a permissive branch strict, check what used to land in it.**
  Cost once: a fast-exit failure that would have broken every platform lacking a
  JS executor, invisible to Linux CI.
- **Count turns, not telemetry rows.** Each turn emits both a `SEMANTIC_TURN`
  and an `OUTPUT_ADMISSIBILITY_EVAL`; row counts double.
- **A fix reaching one of N callers is the default outcome, not the exception.**
  Observed at 1/29, 2/29, 2/31, and on both halves of several config pairs.
- **A gate must never assert "agent X is at level Y".** `governance_level_` is a
  single global atomic; only "the system reached Y while X was the pressure
  handle" is falsifiable.
- **A gate must exclude its own artefact from its own evidence.** `RE-10` counted
  `restrictions.crypto` to show the check ran, and the warning it was testing
  *names the rule it warns about* — so the gate counted the warning as proof of
  the thing the warning complains about. It passed under an `INTEGRITY BLOCK`
  where nothing executed at all.
- **Isolate the trust store in any ad-hoc probe.** Otherwise the result depends
  on whether some other suite happened to have `NAAB_TRUST_STORE_DIR` repointed
  at the time (D8).
