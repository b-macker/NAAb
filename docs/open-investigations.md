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
| A1 | **6 of 9 contradiction checks have no test at all** — `CONTRA-001, 003, 004, 005, 008, 010`. Of the three that do, one (`007`) was dead. `CONTRA-006` does not exist; explain the gap. | **in progress** | All nine now confirmed to fire against a hand-built config (probed 2026-08-09) — none is a `CONTRA-007` repeat. Two notes: `CONTRA-001` is hardcoded SOFT, so it aborts the load and prints its description with an **empty `Rule:` field** — the pattern id never reaches the operator; and probing it turned up the `restrictions.*.enabled` finding below. **Both remaining items are now done.** `test_contradiction_coverage.sh` covers the five that had no test at all (`CONTRA-001, 003, 004, 005, 008`), each with a **negative fixture** differing only in the field its condition reads — without which "the id appears in the output" is satisfied by an engine that prints every pattern unconditionally. The other six are covered elsewhere and are not duplicated. The empty `Rule:` is fixed (A1a). Note the count is now **eleven**, not nine: `CONTRA-011` and `CONTRA-012` were added by the observability campaign. |
| A1a | **A SOFT/HARD contradiction block does not name which pattern fired.** `CONTRA-001` at SOFT prints `Rule: ` empty. The ADVISORY path prints `contradiction.CONTRA-001` correctly. | **done** | `rule_name` now goes to `formatError` as well as `enforce`. Pinned by `test_contradiction_coverage.sh` CCOV-02, which anchors on the `Rule:` line specifically — asserting the id appears *somewhere* in the output is satisfied by the description alone. CCOV-01 pins it too, because on the load-abort path that line is CONTRA-001's only machine-readable identification. |
| A1b | **`code_quality.<check>: {"enabled": false}` turned the check ON.** `loadSimpleCheck` set `config.enabled = true` for any object form and never read `obj["enabled"]`, so the explicit flag was inert and the result was the inverse of what the operator wrote. **Both copies of `govern-template.json`** ship `no_hardcoded_urls: {"enabled": false}` and `no_hardcoded_ips: {"enabled": false}`, so the shipped template enabled the two checks it documents as disabled, and every config copied from it inherited that. Nine checks route through this loader. | **done** | Found by accident again — a *negative fixture* for CONTRA-003 refused to go negative. That is three for three: every inert/inverted mechanism in this repo has been found from a different direction, none by review. **First fixed the wrong way, then corrected.** Honouring the flag is a LOOSENING — the long note at the `restrictions.*` block already rejected exactly that trade for its own six sub-blocks: every config carrying `"enabled": false` today is being enforced and would silently stop on upgrade. Honouring it in `code_quality` disabled `no_hardcoded_urls` and `no_hardcoded_ips` in **both** templates, and made the same JSON mean opposite things under `code_quality` and `restrictions` — the precise confusion that note exists to complain about. Now conformed to the precedent: `warnIgnoredEnableFlag()` is shared, enforcement is unchanged, and the silence is what got fixed. **The worst instance was found last, by finishing the enumeration rather than stopping at the first family.** `parseCodeQualityField` enables an object-form check **at HARD** when it carries no `level`, and never reads the inner flag — so `code_quality.no_secrets: {"enabled": false}` turns secret scanning ON as an uncatchable exit-3 block. Measured: block absent → exit 0; `{"enabled": false}` → **exit 3**; `true` → exit 3. Same for `no_placeholders` and `no_hardcoded_results`. A second, inverted shape sits in `requirements.main_block` and `error_handling`, where `enabled` is not read at all and `level` is the only trigger — so `{"enabled": true}` with no level leaves the requirement OFF while the operator believes they switched it on; that gets its own message (`warnEnableNeedsLevel`). `telemetry.tamper_evidence` was checked and is CORRECT — guarded by `explicitly_set`, so its flag is honoured. Coverage extended past `loadSimpleCheck` to the other object forms that force-enable (`no_pii`, `no_mock_data`, `no_apologetic_language`, `max_complexity`, `encoding`, `no_hardcoded_results`, `complexity_floor`) — `complexity_floor` uses a separate parse branch and was still silently inverted after the first fix, which `tests/governance_v4/edge/govern.json` had been trying to disable all along. Pinned by CCOV-03, whose negative fixture must OMIT the block, since that is now the only way to leave a check off. |
| A1c | **`"level"` accepts any string, and an unrecognised one silently DISABLES.** `parseEnforcementLevel` maps `hard`/`approval_required`/`soft`/`advisory`/`detect` to `{true, level}` and falls through to `return {false, HARD}` for everything else — so `"level": "off"`, `"none"`, `"warn"` or a typo turns the check off with no diagnostic. That is the same silence as A1b, in the key that A1b's own warning tells operators to reach for. | **done (warned, not honoured)** | Found while verifying A1b's warning text. Not fixed: unlike the `enabled` case there is no safe default — rejecting unknown levels is a tightening that could fail configs loading today, and defaulting them to enabled is a tightening too. Needs the same deliberate call as the A2 keys. Fixed the additive way: an unrecognised level now warns and names the five valid ones, while still disabling exactly as before — enforcement is unchanged. De-duplicated per distinct value, because three checks reach the parser by two paths and the message does not name the key, so a second identical line carries nothing. Pinned by `test_level_key_silent_disable.sh`, verified to fail when the warning is removed; LK-03 is the control (a VALID level must stay silent, or warning on everything would pass) and LK-04 separates de-duplication from blanket suppression. |
| A2 | **Systematic inert-mechanism sweep of the engine.** Beyond CONTRA: which other checks read state that is normalized away, or depend on a counter fed by a different threshold? | **swept; one shape found and now guarded** | Three sub-sweeps ran. (1) *Loader clears state a check reads* — clean; the only `.erase`/`.clear` on check-visible state is the CONTRA-007 site, which already records the overlap before erasing. (2) *`enabled` flags never set true* — clean; an initial pass flagged 11, and all 11 were false positives from alias reads (`auto& cfg = rules_.X; cfg.enabled = ...`), which is itself the lesson: a full-path grep reports working keys as dead. (3) *Keys parsed but enforced by nothing* — **77 flagged by screening, of which only a verified handful are real** — the scan excludes the loader from its consumer search, and the loader is itself a legitimate consumer whenever it copies a key into a subsystem's config struct. **Six flagged keys are live**: the telemetry forwarding set is read at `governance_config.cpp` and handed to `TelemetryForwarder`, which uses them under shorter names. Tightening the rule the other way (any loader read counts) collapses 77 to 2, because ratchet and `extends` merging read every key they guard. Neither heuristic stands alone, so **no entry may be acted on without an empirical check that includes a positive control** — a control is what caught this: the polyglot output-size keys could not be settled either way, because the known-live sibling (`limits.data.output_size`) would not fire in the test harness, so its silence proved nothing about its neighbours. **An unread key does NOT imply an absent feature** — that was claimed first and corrected on tracing, in the alarming direction. Four categories, all present: **(A) no enforcement exists** — verified empirically for `limits.code.max_total_polyglot_lines`/`max_functions`/`max_variables`, `limits.execution.total_executions`, `limits.memory.total_mb`/`per_block_mb`: all six set to `1`, a program with 40+ polyglot lines across two blocks runs to completion at exit 0 with "0 violations", against a `languages.blocked` positive control that blocks at exit 3 on the identical program; **(B) live feature, inert toggle** — `trust_policy.check_revocation` and `check_key_expiry`: `TrustStore::loadTrustedKeys` skips revoked and expired keys *unconditionally* and `trust_store.cpp` never reads the governance config, so setting them false disables nothing and wiring them could only permit WEAKENING; **(C) live check, ignored sub-option** — `requirements.error_handling.require_try_catch`/`require_catch_body`, while the check runs off `rules().require_error_handling`; **(D) absent and honest** — `require_fresh_signature` defaults false and no signature-age comparison exists. Category A is the real gap; the `block_{ldap,template,xpath}_injection` trio also has no pattern anywhere. `limits.code.max_total_polyglot_lines` is the emblem: declared, parsed, clamped, **ratcheted**, merged across `extends`, written into every generated config by `naab governance init`, shipped at 5000 in both templates — and read by nothing. More plumbing than most keys that work, which is why it reads as alive. Guarded by `test_inert_key_sweep.sh` + `inert_keys_baseline.txt`: the set is pinned, so a NEW unenforced key fails CI instead of waiting to be found by accident. Remaining: decide per key whether to wire or delete (A3b's warning applies — wiring is not additive). |
| A3 | **Which `enforce()` sites can never fire?** | **done — see the campaign doc** | The evidence-mining version of this row was wrong and is recorded as such: cross-referencing 107 rule names against 21 MB of archived runs reported 95 "never fired", including five fired by hand an hour earlier. The archive is all `living-script`, so it measures coverage, not capability. **Source reachability** is the discriminator. Result: 5 check-bearing methods with zero call sites; 3 genuinely inert config keys; 1 enforcement-without-evidence gap; 1 dead duplicate. |
| A3a | **`limits.data.output_size` enforces without producing evidence.** `polyglot.cpp:718` throws a plain `std::runtime_error` rather than calling `enforce()`, so the block yields no governance finding, no telemetry, no report entry — and is catchable by NAAb `try/catch`, unlike the HARD block it resembles. | open | Routing it through `enforce()` changes behaviour (catchable → uncatchable). Needs the same decision as C1a. |
| A3c | **The bounded-healing ledger could be over-counted without detection** — G2 inflated `coherence_damage_total` and no gate failed. | **done** | Closed by BH-08, which asserts the exact conservation invariant `coherence == 1 - damage + healed`. Note the check originally specified here (`damage ≈ (1 - min_coherence_lifetime) + healed`) was **wrong** — it only holds if all healing follows the coherence minimum. Testing it is how that was found. |
| R5 | **Make healing relative rather than absolute.** `coherence_natural_healing` is a per-turn constant, so whether it suppresses escalation depends on the workload's damage rate — measured cliff at 0.50 for one profile and 0.15 for a lighter one. A rate capped at a fraction of the agent's own recent damage per damaging turn would self-scale and have no cliff. | open | New design; needs its own trace of the regression surface. The alternative is accepting that this knob cannot be set globally and documenting it, which is the current state. |
| V3a | **S20's `prompt_alignment` denominator is the MANDATE's keyword count, not the prompt's** (`behavioral_sequence.cpp:1899`). A short, perfectly on-topic prompt against a long mandate scores low and trips gate 1 before the response is examined. v3's original prompts fired S20 on **35 of 36 turns**, including phases where the agent was behaving correctly. | open | Fixed in v3 by wording every prompt on-mandate, but the engine behaviour is unchanged and will bite any operator whose mandate is longer than their prompts. Worth deciding whether the denominator should be the prompt's keyword count, or whether the threshold should scale with mandate length. |
| A3b | **Four dead check methods remain in the tree**: `checkStringLength`, `checkNestingDepth`, `checkDictSize`, `checkCodegenAllowed`. The first three back inert keys; the fourth duplicates live enforcement in `codegen_impl.cpp`. | open | Delete, or wire behind an opt-in. Wiring is **not** additive — see the campaign doc's regression surface. |


### A2 traced properly: the `limits.*` family, key by key

The screening sweep said "77 unread". Tracing each key to the point of
enforcement — rather than to the point of *reference* — gives a different and
much more useful answer. Three distinct things were being conflated.

**Reference is not reachability.** Six check methods read a `limits.*` key and
have **zero call sites**: `getMaxLinesForLanguage`, `getTimeoutForLanguage`,
`checkNestingDepth`, `checkOutputSize`, `checkStringLength`, `checkDictSize`
(whose only apparent caller is a *comment* in `governance_config.cpp` recording
that it has none). A grep for the key finds the read and reports the key live.

**A similarly-named sibling is not the same key.** `max_lines_per_block` appears
enforced at `governance_checks.cpp:1447` — but that is
`code_quality.max_complexity.max_lines_per_block`, not
`limits.code.max_lines_per_block`. This is the third time in this campaign that a
name collision has produced a wrong reading (`max_tokens_per_run` vs
`max_total_tokens` killed two runs; `forward_batch_size` vs `batch_size` nearly
put six working keys on a defect list).

**An inert knob is not an absent protection.** Most `limits.*` keys sit on top of
enforcement that works, driven by a different source:

| key | protection | driven by |
|---|---|---|
| `limits.memory.total_mb`, `per_block_mb` | RLIMIT_AS / Job memory, real | the **sandbox level**, hardcoded (128 MB at RESTRICTED) — governance never sets it |
| `limits.data.array_size`, `dict_size`, `string_length` | throws on overflow, real | `include/naab/limits.h` **compile-time constants** (10M / 1M / 100MB) |
| `limits.data.nesting_depth` | parse depth bounded | `MAX_PARSE_DEPTH`, compile-time |

`limits.memory.total_mb` is the sharpest case and corrects an earlier claim in
this document. The loader *does* read it (`governance_config.cpp:447`) into
`rules_.memory_limit_mb`, which is why it looked wired — but `getMemoryLimitMB()`
has no callers, and `SubprocessContainment::max_memory_bytes` is populated from
`ScopedSandbox::getConfig().max_memory_mb`, never from governance. The chain runs
three links and dead-ends on the fourth.

**Live from govern.json**, verified by tracing to a reachable enforcement site:
`limits.execution.loop_iterations`, `limits.execution.polyglot_blocks`,
`limits.rate.max_polyglot_per_second`, `max_stdlib_calls_per_second`,
`max_file_ops_per_second`, and `limits.data.output_size` (via `polyglot.cpp:718`,
not via the dead `checkOutputSize`).

**A fourth conflation, and the one that produced the most wrong answers: the
loader MIRRORS nested keys into flat legacy fields, and the flat field is what
enforcement reads.** `limits.execution.call_depth` sets `rules_.max_call_depth`;
`limits.data.array_size` sets `rules_.max_array_size`; `limits.timeout.global`
(line 791) sets BOTH `rules_.timeout_seconds` and `rules_.runtime.timeout`, and
`main.cpp:1913` enforces the latter. The nested key is the modern spelling, the
flat field is the original, and a scan that excludes the loader sees the modern
name written and never read — so a wired key reads as dead. Two entries were
wrongly listed here as unenforced on exactly this basis, including
`limits.timeout.global`, which had been singled out as *the* aggregate an
operator would most reasonably assume they had. They have it.

(Note one quirk found while confirming it: `main.cpp:1913` guards on
`rules.runtime.timeout != 30`, a sentinel against the default, so configuring the
global timeout to exactly 30 seconds is a no-op.)

**Genuinely unenforced, with no protection behind them by any route** — nine
keys, and the only ones that warrant a wire-or-delete decision:

    limits.code.max_functions
    limits.code.max_variables
    limits.code.max_total_polyglot_lines
    limits.code.max_nesting_depth
    limits.execution.total_executions
    limits.execution.parallel_blocks
    limits.timeout.total_polyglot
    limits.data.input_size
    limits.rate.cooldown_on_limit_ms


#### The nine, traced individually — and only two are truly uncovered

Tracing each of the nine to the thing it would protect, rather than stopping at
"nothing reads this key", collapses the list again. Two corrections to the
framing above come first:

**"The engine never accumulates" was wrong.** `limits.execution.polyglot_blocks`
is an aggregate over the whole run, it is LIVE, and it is backed by a member
counter (`polyglot_block_count_`, incremented by
`incrementAndCheckPolyglotBlockCount()`). The per-item/aggregate split was a tidy
story that the code does not support.

**Parallel polyglot execution is real**, not a phantom feature —
`polyglot_async_executor.h`, `executePolyglotGroupParallel()`,
`group.parallel_blocks`.

| key | what actually protects it |
|---|---|
| `limits.execution.total_executions` | **redundant** — `limits.execution.polyglot_blocks` is live and counts the same executions |
| `limits.execution.parallel_blocks` | **inert knob** — concurrency is bounded by a hardcoded `ThreadPool(2)` in `polyglot_async_executor.cpp` |
| `limits.timeout.total_polyglot` | **subsumed** — the whole-run timeout (`limits.timeout.global` → `runtime.timeout` → `main.cpp:1913`) already bounds it |
| `limits.data.input_size` | **inert knob** — `limits::checkStringSize` enforces `MAX_INPUT_STRING` (100 MB) at `lexer.cpp:69` |
| `limits.code.max_nesting_depth` | **covered** — `MAX_PARSE_DEPTH` (1000) enforced at `parser.cpp:18`; the scanner also has `deep_nesting` |
| `limits.code.max_total_polyglot_lines` | **partly covered** — per block by `MAX_POLYGLOT_BLOCK_SIZE` (1 MB) and the live `code_quality.max_complexity.max_lines_per_block`; the program-wide total is unbounded |
| `limits.rate.cooldown_on_limit_ms` | **nothing, and nothing to wire it to** — no cooldown or sleep exists anywhere; the live rate limits block rather than throttle. Its default is `100`, so an operator sees a plausible non-zero value for behaviour that does not exist |
| `limits.code.max_functions` | **nothing** |
| `limits.code.max_variables` | **nothing** |

So the honest count is **two** keys where an operator gets no protection and
nothing else covers it, plus one (`max_total_polyglot_lines`) whose aggregate
half is uncovered.

`max_functions` looked cheap to wire because `profile.function_count` already
exists — but that is computed **per polyglot block, by regex over the block's
source** (`syntactic_analyzer.cpp:83-90`), for complexity scoring. It is not a
program-wide count of NAAb functions, so wiring the key to it would enforce
something other than what the key's name promises. There is no `variable_count`
field at all.

**Recommendation, revised by the trace**: delete `cooldown_on_limit_ms`,
`total_executions`, and `total_polyglot` as redundant or unimplementable-as-named;
leave `parallel_blocks`, `input_size` and `max_nesting_depth` alone but document
them as knobs over hardcoded protections (the same treatment `trust_policy` got);
and treat `max_functions` / `max_variables` as the only genuine wire-or-delete
decisions — noting that wiring them needs a program-wide counter that does not
exist today, for a limit whose value nobody has ever been able to rely on.

**On whether these were ever wired**: this clone's history is shallow — 112
commits beginning 2026-07-22 — so `git log -S` reports every one of the dead
check methods as first appearing in the second visible commit, which means only
that they predate the visible history. Whether enforcement was removed at some
point cannot be answered from this repository, and no claim either way should be
made from it.

Note what unites them: every one is an **aggregate** — a total across the
program, a whole-run timeout, a cumulative count — while every live limit is
**per-item**: one block, one value, one call, one second. The engine enforces
what it can decide at a single point and does not accumulate. That is a coherent
architectural boundary rather than scattered rot, and it means "wire them up" is
not a small change: it requires state that does not exist today.

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
| C1b | **Confirmed again by the 2x2 (2026-08-18)** — arms at `0.03` were outcome-identical to arms at `0.0`. This row predated that experiment and would have pre-empted it; it was not consulted. **Healing is inert at every shipped rate.** 25 configs set `coherence_natural_healing`, including both templates (0.02) and living-script v1/v2 (0.03). Varying only the rate on v3: 0.0 and 0.03 give **identical** results to the quarantine; 0.25 restores `elevated → normal`. `heal_factor = 1/(1+signals)` halves an already-small number against penalties of 0.08–0.15. | **addressed; R3 withdrawn** | Bound (R1/R2), channel-agnostic booking (S1/S2) and ratchet (R4) shipped. **R3 (raise the rate) is withdrawn on evidence**: the rate at which healing suppresses escalation is damage-relative, not absolute — HIGH is lost at 0.50 in v3 and at 0.15 in the same scenario with two signals disabled. No global constant is correct. See `docs/proposal-bounded-coherence-healing.md`. |
| C1a | **`elevated → normal` is unreachable once coherence floors.** With `coherence_natural_healing` at its default 0.0, coherence never climbs back, and `coherence_prox` alone holds composite above `elevated_threshold` — 18 quiet turns in the v3 run left it pinned at 0.5125 vs a 0.35 threshold. | open | A design question, not obviously a defect: scrutiny earned arguably should not be cheap to shed. But pair it with B1 (a process restart clears the level outright) and the combination is odd — unshakeable within a process, free by restarting it. Both available fixes are loosenings, so this needs a decision before code. **See C1c: live evidence now generalises this from `elevated → normal` to de-escalation from ANY level.** |
| C1d | **A compliant agent doing ordinary work floors its coherence in six turns — and it is NOT about code.** v3's DESIGN (1-3) and IMPLEMENT (4-6) are ordinary well-specified on-mandate work. Code arm (keyed runs 4/5/6): coherence 0.74/0.64/0.56 at turn 3, floored turn 7-8. **Prose arm (keyed, same config via `prose_overlay.json`, only the task differs): 0.46 at turn 3, floored turn 5** — outside the code arm's three-run spread and two turns earlier. Signals, code vs prose: `semantic_stability` 29/36 vs **35/36**, `plan_drift` 14/36 vs **29/36**, `entity_consistency` 34/36 vs 35/36. | **located; threshold decision open** | Ordinary varied work of ANY modality trips these thresholds and prose trips them harder, so the suspects are the **0.25 defaults** on `semantic_stability` and `entity_consistency` — NOT the code-aware extractor, which appears to be helping code. **Do not move a threshold on this**: it identifies the suspect, not the value. Two caveats stand — n=1 for prose against n=3 for code, and response length is unmatched (prose input baseline 1717 vs 1220), so modality is not isolated from verbosity. Settle by matching response length across arms and running prose twice. **Update (run 24 + the 2x2):** the suspects now have a MECHANISM, which moves this off "threshold value unknown". Both signals score inter-turn *variation*, so they are anti-correlated with repetition drift: measured 67/67/**0**/37% (S15) and 33/33/**12**/32% (S10) across DESIGN/IMPLEMENT/DRIFT_PRESSURE/RECOVERY — firing LESS during induced drift than during correct work — and across 8 consecutive byte-identical responses in the 2x2 neither fired once. In recovery they fire on a period of exactly 6, matching the scenario's task rotation. That is a defect in KIND, not in value, and no threshold move fixes it: raising the threshold makes them fire more on correct work, lowering it makes them blinder to repetition. See `governance-campaign-findings.md`. |
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
