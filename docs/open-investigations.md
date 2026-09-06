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
| R5 | **Make healing relative rather than absolute.** `coherence_natural_healing` is a per-turn constant, so whether it suppresses escalation depends on the workload's damage rate — measured cliff at 0.50 for one profile and 0.15 for a lighter one (**both re-measured 2026-08-25: neither reproduced — the cliff sat between 0.20 and 0.30 on two fresh profiles; see the R3 re-examination in `docs/proposal-bounded-coherence-healing.md`**). A rate capped at a fraction of the agent's own recent damage per damaging turn would self-scale and have no cliff. | **implemented (opt-in, default off)** | Implemented 2026-08-26; 11 gates pass in `tests/governance_v4/test_relative_healing.sh`. Verified with control: `f=0.5` returns a floored agent to 1.0000/normal, while the same fixture with no healing stays floored. Scale invariance measured directly — heal/damage-rate held at 0.500 across a 2.44x change in damage magnitude. **Two design corrections, both found by running it**: the `f < 1.0` safety clamp made the proposed positive control (suppression at `f=1.5`) unreachable, so it moved to absolute-0.30; and RH-02's original failure was the acceptance FIXTURE, not the code — S17 fires every recovery turn, and only a mechanism that out-heals a live signal passes it, which is suppression. See the implementation-outcome section of the proposal. Design written up in `docs/proposal-relative-healing.md` (2026-08-25), including the regression surface this row asked for. Core property is DERIVED, not measured: recovery time works out to `n_damaging / f` clean turns, so the damage magnitude cancels and there is no cliff to find; `f < 1` is the safety invariant. The trace turned up one fact that shapes the design — damage is booked POST-CLAMP, so a floored agent books zero further damage and the rate window must span damaging turns only, or it decays to zero and recreates the trap. Gates RH-01..RH-08 specified, with RH-05 (suppression reappears at f>=1.5) as the positive control without which the no-suppression claim is unfalsifiable. |
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


##### The three delete candidates, checked before deleting

Deleting is the one recommendation that removes something, so each was traced to
the mechanism it would attach to. Two survive as deletes; the third does not.

**`limits.rate.cooldown_on_limit_ms` — delete, and the reason is structural.**
`RateLimiter` (`governance.h:2663`) is a fixed-window counter: `max_per_second`,
`window_start`, `count_in_window`, and a `check()` returning bool. There is no
field a cooldown could live in and no sleep or backoff anywhere in the tree. More
decisive than the missing wiring: on breach the callers **throw**
(`polyglot.cpp:173`), so the run ends. A cooldown is only meaningful for a
limiter that *waits*; you cannot cool down from a thrown error. Wiring this key
would not be connecting a wire, it would be replacing fail-fast with throttling.
Its default is `100`, so an operator sees a plausible value for a design that was
never built.

**`limits.execution.total_executions` — delete, redundant.** The claim was
`limits.execution.polyglot_blocks` covers it, and the trace makes that stronger
than assumed: `incrementAndCheckPolyglotBlockCount()` is called from **six**
sites spanning both engines — `vm.cpp`, `polyglot.cpp` (twice),
`call_dispatch.cpp`, `modules.cpp` — *and* `codegen_impl.cpp:360`. So the live
counter already counts every dynamic code execution including codegen, which is
exactly what "total executions" would mean.

**`limits.timeout.total_polyglot` — do NOT delete. The earlier "subsumed"
verdict was wrong.** Polyglot time is bounded at two scales, and neither is the
one this key expresses:

* per block — `max_cpu_seconds` from the **sandbox level** (hardcoded, 10 s at
  RESTRICTED), applied as `RLIMIT_CPU` and read by `js_executor.cpp:114`;
* whole run — `limits.timeout.global` → `runtime.timeout` → `main.cpp:1913`.

Cumulative time *in polyglot specifically* — "this script may run for five
minutes but must not spend more than thirty seconds in embedded code" — is
expressible by neither. The only substitute is indirect: block count (live) times
the per-block CPU bound, and the operator controls only the first, since the
second is fixed by sandbox level. So this key names a real and otherwise
unexpressible constraint. Deleting it removes intent; leave it, and either wire
it or document it as unimplemented.

Noted in passing at `main.cpp:1913`: the global timeout is applied as
`std::max(cli, config)`, so `--timeout` can **extend** a govern.json limit. The
comment says that is deliberate, but it makes the governance timeout a floor
rather than a ceiling, which is the opposite of how every other governance limit
reads.


###### The final two, exhaustively

Both were the last candidates standing after four narrowings. Tracing them
completely removes one and reclassifies the other.

**Lifecycle, both keys.** Declared once (`governance.h:270-271`, default `0`),
parsed once each (`governance_config.cpp:874-875`) with an `explicitly_set`
entry, merged across `extends` (`4594`, `4602-4605`), written into every
generated config by `naab governance init` (`max_functions: 100`,
`max_variables: 500`), and shipped in **both** copies of `govern-template.json`.
Read by nothing.

They are also **less** plumbed than their dead siblings, which is itself a
signal. The `clamp0` block covers 21 limits fields and omits both; the ratchet
covers `max_total_polyglot_lines` and `max_nesting_depth` and omits both. So a
negative value would survive parsing unclamped, and neither participates in
mid-run tightening checks.

**`limits.code.max_functions` — delete. It is redundant with a live check.**
The scanner has `function_count` (`checks_complexity.cpp:101-109`): a configurable
`max` (default 25), enabled at advisory level by the generator, reported as a
proper scanner issue with a remediation hint. It is not a dormant tool —
`main.cpp:2128` runs a **preflight scanner on every governed execution**, over the
main file *and* its imported modules, before execution so advisories survive an
exit-3 block. So the capability the dead key names is already delivered, by a key
that works, at file granularity, with better reporting. This is the same
similarly-named-sibling shape that has now produced a wrong reading five times in
this campaign.

**`limits.code.max_variables` — no equivalent anywhere, and no definition.** The
scanner's complexity family counts cyclomatic, cognitive, file length, functions,
classes, imports, returns and nested ternaries — **there is no variable count**.
Nor is there one in the analyzer profile (`syntactic_analyzer.h:18-40` has
`function_count`, `loop_count`, `max_loop_depth`, `max_function_depth`, and no
variable field), nor in the interpreter (`values_.size()` is one Environment's
scope map, a runtime binding count per scope, not a program total).

It is therefore the single key in the whole sweep that expresses something
nothing else expresses. That is not the same as a gap worth filling, because
**nobody can say what it should count**: static `let` declarations? runtime
bindings? peak live bindings across scopes? A loop declaring one `let` over a
thousand iterations is one declaration and a thousand bindings, and the two
readings differ by three orders of magnitude. The template ships `500`, a number
with no definition behind it.

**Wiring cost, if it were wanted.** Governance's static checks take
`const std::string& code` (`governance.h:2823-2829`) — **source text, not an
AST** — so any count enforced by the engine is necessarily a regex, with the usual
miscounting inside strings and comments. The AST-accurate route does not exist
either: `TypeChecker` visits every `FunctionDecl`, but `main.cpp:1699` runs it
only under `--strict-types` or `--verbose`, so a limit hosted there would not
apply to a normal run at all.

**Both were deleted.** Removed from the struct (`governance.h`), the parser and
the `extends` merge (`governance_config.cpp`), the generator
(`governance_init.cpp`) and all three operator-facing templates
(`govern-template.json`, `docs/govern-template.json`,
`docs/book/verification/govern.json`). Test fixtures under `tests/gorilla`,
`tests/e2e` and `tests/llm-ab-testing` still carry the keys; unknown keys are
ignored silently, so they are inert data, and rewriting 30+ scenario configs
would risk unrelated breakage for no behavioural gain.

`test_inert_key_sweep.sh` caught the removal on the first run — 77 entries to 75,
naming both keys — which is the guard doing exactly its job in the *good*
direction. Its failure text said "now enforced", which is wrong for a deletion;
it now names both possibilities and asks which. Baseline updated to 75.

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
| A4 | **`capability_underutilization` (S7) is inert by construction — its gate set is never written.** The firing condition tests `state.granted_capabilities.count(cap)`. `granted_capabilities` has exactly THREE occurrences in the repository: the declaration (`behavioral_sequence.h:190`), this read (`behavioral_sequence.cpp:1307`), and one read in `agent.environment()` (`agent_impl.cpp:778`). **Nothing writes it**, while its sibling `exercised_capabilities` is written on the adjacent line (`:1305`) — which is what makes this read as an oversight rather than a decision. Two further layers: the signal also **defaults to `false`**, and `agent.environment()` therefore reports `capabilities_granted` as an **empty list for every agent that has ever run**. | open | **Proven with a positive control**, not inferred. Same fixture throughout — a tool calling `env.get` at turn ≥10, tool executing 25 times: stock build → 0 firings; a scratch build seeding `granted_capabilities` → fires at turn 12; seeded build with the capability exercised from a *script* instead of a tool → 0, which separately confirms B6 blocks the script path. **The first attempt at this control FAILED and the failure was the useful part**: the signal defaults to false, so my fixture never enabled it and I had been reading a structurally-inert harness as a fact about the engine ("a sweep over explicit values cannot see a default", `investigation-method.md`). Pinned by `tests/governance_v4/test_signal_reachability.sh` SR-02/SR-03, with SR-04 as the load-bearing control — it asserts `scope_creep` DOES fire on the same fixture, without which SR-02's silence is equally satisfied by "tool events never reach CDD at all". Fix is not obviously additive: populating the set turns a signal ON for every existing config, so it needs the same deliberate call as the A2 keys. |
| A5 | **`intent_contradictions` (S4) is unreachable — every event that would arm it throws before it can be recorded.** The signal needs `state.prev_turn_blocked_caps` non-empty. That set's ONLY writer is `behavioral_sequence.cpp:1266`, inside `recordTurn`, populated from `CHECK_FAILED` events in the CURRENT turn's bucket. All six `CHECK_FAILED` emission sites (`agent_impl.cpp:3669, 3687, 3715, 3726, 3738, 3766` — the `shell_blocked` and `blocked_path` response-scan restrictions) **throw within three lines**, and `checkContextDrift` is further down the same straight-line function (`:4065`); the only intervening catches are `regex_error` at `:3685`/`:3724` and a `catch (...)` at `:3882` scoped tightly around `nlohmann::json::parse`. So the turn that produces a `CHECK_FAILED` never reaches `recordTurn`, and turn-stamping keeps that event out of every later bucket. The set is therefore always empty. | open | **Proven with a positive control**: suppressing that one throw in a scratch build makes S4 fire (turn 8) on a fixture where the next turn's tool does `file.read` — `blocked_path` maps to `filesystem`, which the tool then attempts. On the stock build the same shape gives 10 sends and 9 `CDD_TURN` events. **Read the COUNT, not the turn index**: `CDD_TURN`'s turn field runs one ahead of `RESPONSE_SCAN`'s, and reading the index is exactly how this was briefly and wrongly written off as "the ordering claim collapsed" — the blocked attempt appears as two consecutive `AGENT_RESPONSE` events with no scan and no CDD between them. Pinned by `test_signal_reachability.sh` SR-05 (which asserts the count) with SR-06 as the control, since SR-05 alone would pass for a build with CDD switched off. Fix direction unclear: emitting the event before the throw is not enough on its own, because the turn still has to reach `recordTurn`. |
| A6 | **The A2 inert-mechanism sweep cannot see either of the above.** A2 screened for *config keys parsed but enforced by nothing*. A4 and A5 are **unwritten and unrecordable state fields** — a different shape, invisible to a key-oriented scan, and both were found by accident while tracing something else (B6). That is now four for four: every inert mechanism in this repo has been found from a different direction, none by review or by the sweep built to catch them. | open | Wants a second screening pass over `DriftState`/`AgentTracker` members with the A2 discipline: for each field, does anything write it, and can the writer be reached? The two found here were the first two members examined, which is a poor sign about the base rate rather than good luck. Same caution as A2 applies — an unwritten field does NOT imply an absent feature, and no entry may be acted on without an empirical check carrying a positive control. |

## B. Known engine behaviour, decided but unfixed

| # | Item | Status | Note |
|---|---|---|---|
| B1 | **Scrutiny does not survive a process restart.** `governance_level_` is `std::atomic<int>{0}` with no persistence and no restore path. An agent that escalated gets a clean slate in the next process — observed live (v1 Aug 8: segment 1 ended `elevated`, segment 2 began `normal`). A long-running system that restarts periodically can never accumulate scrutiny. | **open — undocumented** | Not yet written up anywhere. Decide: document as a boundary, or persist. Persisting raises scope, expiry and tamper-evidence questions that should not be answered off one run. |
| B2 | **`governance.telemetry` resolves CWD-relative while `govern.json` is discovered script-dir-relative.** An operator putting the two side by side gets telemetry wherever they happened to run from. | parked | Documented. Changing it moves files for every existing user. |
| B3 | **Response secret/PII scanning defaults off.** A config that never sets `code_quality.no_secrets` / `no_pii` has *no* response scanning. | parked | Deliberate: enabling by default would start blocking responses that pass today. Worth knowing if you assumed otherwise. |
| B4 | **CRITICAL is marginally harder to reach after #129** — needs `critical_sustained` turns at or above `critical_threshold`, rather than that many above 0.70 plus one spike. | parked | Accepted trade: severity should now be caught at ELEVATED/HIGH instead. Revisit if a live run shows CRITICAL missed where it was wanted. |
| B5 | **On DEFAULT settings, `context_growth` (S12) fires permanently from ~turn 8 with no recovery path.** Its baseline is the mean of the first `adaptive_baseline_window` (5) turns and is never revisited, because the EMA that would track natural growth is gated on `adaptive_baseline_enabled` (`behavioral_sequence.cpp`: `adaptive_baseline_enabled && baseline_complete`) — **default false WHEN THIS ROW WAS MEASURED; it defaults true now.** The 29-of-29 measurement below was taken under the old default and does not describe shipped behaviour. Whether S12 still fires permanently with the EMA live is UNMEASURED — that is the open half of this row now, not the mitigation-ships-off framing. With no `context_window` either, and `context_growth_factor` at 3.0, any long single-handle conversation exceeds 3× its own opening turns and stays there. Measured on v3 keyed run 3: fired on **29 of 29 turns** from turn 8. Because composite pressure has room for only ~1 signal below `high_threshold` once coherence floors, a permanently-firing signal consumes that slot and makes de-escalation unreachable. | **closed — measured 2026-09-05; obsolete on shipped defaults** | Classified during #142 as "a v3 config gap, not an engine defect", which is true of v3 specifically — v1 and `_extended` set `context_window` 20 **and** `context_growth_factor` 5.0, and a keyless 2×2 confirms both are required (window only 25 firings, factor only 17, both 0). But that answers *what v3 did wrong*, not *whether the defaults are right*. A signal that cannot stop firing is a permanent coherence tax rather than a drift detector, and the mitigation ships off. Decide: change a default, warn at load when the combination is unwindowed + non-adaptive + factor 3.0, or document as intended. **Do not close this by pointing at v1's config.** **MEASURED 2026-09-05 — the first option happened by accident and settles the row.** `adaptive_baseline_enabled` was flipped to default true for an unrelated reason (S17's frozen warm-up baseline, #176's evidence), and that flip ungates this signal's EMA. Three arms, 40 turns, identical stub responses, S12 the only CDD signal enabled; the input-token series is AUTHORED (linear 200 + 150n, modelling a full-history conversation appending a constant per turn) and the engine's response to it is what is observed:

| arm | S12 firings | first | final coherence |
|---|---|---|---|
| adaptive OFF (pre-flip default), linear growth | **31 / 40** | turn 10 | **0.0000** |
| adaptive ON (shipped default), same series | **0 / 40** | — | **1.0000** |
| adaptive ON, flat then a 6x jump | 6 / 40 | turn 26 | 0.9892 |

Row 1 is the positive control and reproduces this row almost exactly (it recorded 29/29 from turn 8), so row 2's silence is the EMA and not a fixture that never reaches the trip point. Row 3 is the detection control and is the reason this is a fix rather than a silencing: with the baseline tracking, S12 stops taxing linear growth and still catches the sudden bloat it is named for. Mechanically: the EMA (alpha 0.1) over the last `coherence_history_size` (10) samples converges toward linear growth, so the current/baseline ratio tends to 1 and never reaches `context_growth_factor` 3.0; a step change outruns the lag and fires. Note the per-agent `max_total_tokens` default of 100000 truncates this series at turn 36 — the test raises it, and an arm that does not run its full length cannot support a claim about never stopping. Gate: `tests/governance_v4/test_context_growth_ema.sh` (CG-01 positive control, CG-02 the question, CG-03 detection control). **What is NOT claimed:** nothing here says the pre-flip behaviour was harmless, and a config that explicitly sets `adaptive_baseline_enabled: false` still has it in full. |
| B6 | **Action diversity after turn 0 is invisible to CDD.** `ev.turn` is stamped from `current_agent_turn_` (`governance_engine.cpp:6415`), written only by `setAgentContext` (6432/6438) at the START of each send, so an event raised BETWEEN send N and N+1 carries turn N — a bucket `checkContextDrift` already consumed. In a script with no registered tools the only live bucket is turn 0's. Measured: varied ops before EVERY send fired S5 on exactly the same turns as two ops once at startup, and ops introduced from turn 10 left every CDD turn byte-identical to a control that did none. | **fix exists, ships off** | `context_drift.event_feed: "since_last_check"` selects on `sequence_id > watermark` instead of `turn == N`. **`ev.turn` stamping is deliberately unchanged** — BSD reads it for pattern decay (`:442-443, 465, 500`), so re-stamping would move those windows as a side effect; selecting on sequence keeps BSD out of scope by construction. `getEventsForTurn` has exactly one caller, so no second consumer is left on a disagreeing feed. Measured, 25-turn arms: continuously-varied agent 20 charges → **0**; genuinely-narrowing agent 20 → **20** (still caught); 18 spurious advisory violations and one governance escalation eliminated. Ratcheted in BOTH directions — the change is not uniformly signed. Pinned by `tests/governance_v4/test_event_feed.sh`, where EF-01 and EF-03 are the load-bearing controls: without EF-01 there is no defect to fix, and without EF-03 "S5 stopped firing" is indistinguishable from a feed that blinded it. Still off by default: enabling it makes `scope_creep` reachable from ordinary scripts for the first time, which is a real behaviour change nobody has run at scale. |
| B7 | **S1's fingerprint cannot tell the agent's actions from the orchestrator's.** `computeFingerprint` is now restricted to `AGENT_SEND`/`AGENT_RESPONSE` events, because folding script-driven file/env/crypto events in broke it: on a fixture where every response is identical — a genuine loop — detection fell from 28/30 to 9/30 once the surrounding script varied its actions. **The ideal filter is "events this agent CAUSED"**, which would keep tool-driven `FILE_READ` (the agent asked for it) and drop only script-driven ones. That is not cheaply available: `emitEvent` stamps `agent_handle` from `current_agent_handle_`, which after the first send still holds the handle, so a script event between sends is **indistinguishable from the agent's own by handle alone**. The type filter is a conservative proxy that also discards tool-driven events. | open | **Consequence, stated not hidden:** an agent that calls the same tool identically every turn while varying its prose is a real loop S1 will now miss. The content hash covers the common case (a repeated response), not that one. Honest fix is stamping events with "raised inside an agent turn" — a `RuntimeEvent` field set between `setAgentContext` and the post-receive gates — which is larger than the change that surfaced it. Note the restriction also fixed a smaller pre-existing instance: under the default feed turn 0's bucket carried script events and cost one detection (28 vs 29 on the same fixture), so this was a category error under both feeds, not a side effect of the new one. |
| B8 | **17 of 24 BSD event-type names are silent no-ops in the documented `UPPERCASE_UNDERSCORE` form — and the SHIPPED DEFAULT PATTERNS were written that way.** `matchesStep` accepts a step name when `normalizeEventTypeName(matcher)` (lowercase, `_`→`.`) equals `eventTypeToString(event.type)`. Those disagree for most types: `eventTypeToString(TOOL_CALL)` is `tool_call`, while the enum spelling normalizes to `tool.call`. **Works:** `FILE_READ`, `FILE_WRITE`, `AGENT_SEND`, `AGENT_RESPONSE`, `ENCODE`, `DECODE`, `PROCESS_EXEC`. **Silently dead:** the other 17, including `ENV_READ`, `NET_CONNECT`, `SHELL_EXEC`, `CHECK_FAILED`, `TAINT_VIOLATION`, and all four `TOOL_*`. CLAUDE.md states both formats are accepted and gives `AGENT_SEND` as its example — one of the seven that happen to work, which is why the claim read as verified. `buildDefaultPatterns` used the enum style and `makeStep` stores it verbatim, so **10 of 16 built-ins were dead at one or more steps, 1 degraded, 5 alive** — the survivors being exactly the ones someone happened to write in lowercase. Dead included `credential_harvesting` (the detection the repository description leads with), every `tool_*` pattern, and both taint patterns. | **fixed, ships observe-first** | Measured with spelling isolated — same config, same fixture, 8 tool calls in both arms: `TOOL_CALL` fired nothing, `tool_call` fired. Default step names corrected to the canonical form. **Six of the ten dead patterns are SOFT (exit 3), so correcting the names is not a repair of working behaviour but the first time they have ever fired** — `behavioral_sequences.default_pattern_enforcement` therefore defaults to `"observe"` (all built-ins ADVISORY, visible but non-blocking); `"declared"` restores intended levels. Ratchet: `declared → observe` is a loosening violation. `credential_harvesting` is why: env var matching `*key*|*token*|*secret*` then a network call within 5 events is both the intended detection AND what every legitimate API client does — enabling it at SOFT without observation would be reckless. Pinned by `test_default_patterns.sh`; DP-04/DP-05 (observe fires-and-completes vs declared blocks) are load-bearing, since without both "observe" cannot be told from the patterns still being dead. **Confirmed by execution for 2 of the 10** (`tool_rapid_fire`, `tool_shell_escape`); the other 8 are derived from a rule verified on 8 event types in both directions and need network or taint conditions to exercise. **This row previously claimed user-defined patterns could not fire at all — that was wrong**, and the correction is kept rather than deleted. Still open: a warning for unknown step names, so the next one fails loudly at load. |
| B10 | **Four tests pinned the broken spelling, and three of them run in no suite.** `test_agent_governance_depth.sh` T40 asserted the literal string `TAINT_VIOLATION` appears near the pattern definition — a grep over SOURCE TEXT, not behaviour, so it could not tell a matching step name from a non-matching one. It was locking in a pattern that could never fire, and it is why the defect survived. Three more of the same shape sit in `tests/gorilla/naab-42/run-naab42.sh` (`FILE_WRITE`, `TOOL_CALL`, `AGENT_SEND`). All four broke on the name fix — but **`gorilla` appears nowhere in `run-all-tests.sh`**, so that suite is outside CI entirely and the sweep reported one failure while three sat latent. | in progress | **Two register claims were wrong and are corrected here.** (1) The path: `test_agent_governance_depth.sh` is in `tests/agent/`, not `tests/governance_v4/`, and it *is* registered (`run-all-tests.sh:2197`) — it is the one of the four that ran. (2) The mechanism: `SKIP_DIRS` is a red herring. `TEST_DIRS` is an explicit **allowlist** of 22 entries, so a directory is invisible by being absent from it, skipped or not. Measured 2026-09-06: **13 directories holding 575 `.naab` files are reachable by no runner**, against the 447 tests the suite reports — more test files sit outside the sweep than inside it (gorilla 491, chaos 37, governance_app 14, e2e 8, llm-ab-testing 6, scanner 5, analyzer 4, verification 3, optimization 2, `chapter verification` 2, governance_v4_degraded 1, performance 1, project_context 1). **The 13th was found by fixing the gate's own encoding bug**, which is the failure mode the gate exists for, committed by the gate itself. Its first version counted files by shelling out to `find {p}` UNQUOTED, so a directory whose name contains a space errored and returned a count of zero — and zero is skipped as "holds no tests". That hid `tests/chapter verification`: two tracked files, one of them 110KB, in a directory one character away from `tests/chapter_verification`, which IS in `TEST_DIRS`. Checked rather than assumed, because the suite log shows a `MONO_EXHAUSTIVE_TEST.naab` XFAIL and that looks at first like the file is covered after all: there are **three copies of that name with three different checksums** — `chapter_verification/`, `comprehensive/`, and the space-named one. The XFAIL is one of the two covered copies. The orphan is a divergent third, not a duplicate. It also read `run-all-tests.sh` with the locale's default encoding, which is cp1252 under MSYS2 and died on the file's box-drawing characters — the same bug that shipped once already in `test_hivemind_governed.sh`. Note `LC_ALL=C` does **not** reproduce it: PEP 538 coerces the C locale back to UTF-8, so the reproduction needs `PYTHONUTF8=0 PYTHONCOERCECLOCALE=0 LC_ALL=C`. **The source-text problem is narrower than the row implied and does not generalise to gorilla.** Of 36 gorilla runners, exactly one — `naab-42` — asserts against C++ source text: 22 of its 34 assertions, several matching *comment* strings (`Cross-agent file-based data transfer`, `Server-side governance enforcement`) and two asserting line ORDER. No other gorilla runner does it. Adopting `naab-42` as-is is not free: one of its greps has **already rotted** (`warnings += fmt::format.*Temporal coupling` no longer matches), and `pattern_count -eq 16` is an exact count that fails the next time anyone adds a BSD pattern — a tripwire on legitimate work, not on regression. **Settled:** the orphan set is now pinned by `tests/self-audit/test_coverage_visibility.sh` CV-01, which fails when a directory with `.naab` files falls outside every runner. **Still open (a judgement call, not a measurement):** whether the 573 files are adopted or deleted, and whether `naab-42`'s source-text assertions are rewritten behaviourally or dropped. A test suite nobody runs is worse than none — it reads as coverage. |

| B9 | **The pre-execution BSD path enforces without evidence or budget.** There are two enforce sites for `behavioral_sequences.<name>`: `checkBehavioralSequence` (`governance_engine.cpp:7351`) writes a `BSD_MATCH` telemetry event and calls `consumeRiskBudget(…, 1)`; `checkPreExecution` (`:7901`) does **neither**. `checkPreExecution` handles `agent.send`, `file.read`/`write`, `http.*`, `crypto` encode, `process.*` and `env.get`/`list` (`vm.cpp:1855-1870`), so most real matches take the silent path. Measured: an 8-turn agent run where the pattern fired on **every** turn produced one stderr warning, one `RuleViolation`, and **zero** `BSD_MATCH` events — instrumentation confirmed all 8 firings came from `checkPreExecution` and `checkBehavioralSequence` fired 0 times. The `BSD_MATCH` write carries a comment saying it exists so ADVISORY matches stay visible, which its sibling path defeats. | **evidence fixed; budget asymmetry recorded as deliberate** | **Fixed 2026-09-04.** `checkPreExecution` now writes `BSD_MATCH`, and BOTH sites carry a new `path` field (`pre_execution` / `completed`) so consumers can tell them apart — until now every match from that path was simply absent. **The risk-budget asymmetry is left as it was, and is now a recorded choice rather than an omission:** a pre-execution block PREVENTS the action, so charging a run-level risk budget for something that never happened is arguable either way, and changing it silently alongside an evidence fix would have buried a behaviour change inside a logging one. **Observation (b) had to be fixed FIRST, not after:** `agent.send`'s pre-check detail was the serialized HANDLE DICT, which carries `__nonce` — the HMAC tying the handle to server-side state. It already reached stderr; emitting `BSD_MATCH` would have carried it into telemetry too, so adding evidence without this would have WIDENED a leak rather than closed one. The VM now omits the argument for `agent.send` only — every other pre-checked method takes a path, URL or variable name as arg 0, which is what the detail was always for. Pinned by `tests/governance_v4/test_bsd_preexec_evidence.sh`: B9-01 the fix, B9-02 the control that the completed path is still labelled DIFFERENTLY (without it B9-01 passes for a build stamping `pre_execution` on everything), B9-03 the nonce absence WITH a positive control that details are populated at all. Negative control: removing the emission fails B9-01 with the original symptom while B9-02 still passes. **Observation (a) — double-recording at ADVISORY — remains uninvestigated.**
| B11 | **The tree-walker emits no BSD events for stdlib actions, so ~10 of 16 built-in patterns are dead under `--tree-walk`.** `emitEvent` and `checkPreExecution` have **zero** occurrences in `src/interpreter/` (vs 4 in `src/vm/`). The stdlib method-call emissions — `FILE_READ`/`FILE_WRITE`, `ENV_READ`/`ENV_WRITE`, `NET_CONNECT`, `ENCODE`/`DECODE`, `PROCESS_EXEC` — live only in `vm.cpp`. Agent, tool, codegen and taint events are emitted from `stdlib/` and `runtime/` and are engine-agnostic. **Measured**, same fixtures, VM as positive control: `/etc/passwd`+`process.run` → VM fires `sandbox_probe_escape`, tree-walk **NONE**; 2 reads + `/tmp` write + http → VM fires `data_staging`, tree-walk **NONE**; 3 × `codegen.run` → **both** fire `codegen_rapid_fire`. So BSD machinery is live under the tree-walker; only the stdlib action surface is invisible to it. Because `checkPreExecution` has no tree-walker call site, the pre-execution BLOCK path does not exist there either — including `config_tampering`, the one HARD built-in, which is what guards govern.json. | open | **Tier: 2 patterns measured dead, 1 measured alive; the other 13 derived** from which emitter file each step's event type comes from — not 16 measurements. Not vestigial: `run-all-tests.sh` classifies 12 tests as needs-tree-walk for VM-unsupported features, so operators are actively directed onto this engine. **An earlier framing of this as "the tree-walker has no behavioural detection at all" was an overstatement of a grep and is corrected here.** **ENUMERATED 2026-09-05 — the 13 derived verdicts are decidable statically, and the estimate was exactly right: 10 dead, 6 alive.** Emitter map, counting `RuntimeEventType::X` outside `behavioral_sequence.cpp`/`governance_engine.cpp`. VM-only (a site in `src/vm/vm.cpp`, none in `src/interpreter/`): `FILE_READ`, `FILE_WRITE`, `ENV_READ`, `ENV_WRITE`, `NET_CONNECT`, `SHELL_EXEC`, `PROCESS_EXEC`, `ENCODE`, `DECODE`. Engine-agnostic: `TOOL_CALL`, `TOOL_RESULT`, `CHECK_FAILED` (`agent_impl.cpp`), `CODEGEN_EXEC` (`codegen_impl.cpp`), `TAINT_VIOLATION` (`governance_checks.cpp`), `AGENT_SEND` (`agent_impl.cpp` AND `vm.cpp`). Every step must match in order, so one VM-only step kills a pattern. DEAD: `credential_harvesting`, `sandbox_probe_escape`*, `config_tampering`, `progressive_escalation`, `data_staging`*, `tool_data_exfil`, `tool_env_harvest`, `tool_shell_escape`, `codegen_exfil`, `cross_agent_file_relay` (* = the two measured). ALIVE: `taint_bypass_via_agent`, `repeated_taint_violations`, `tool_rapid_fire`, `codegen_rapid_fire`*, `codegen_after_tool`, `cross_agent_tool_chain`. **Two refinements this row did not make.** (1) "Dead" is two different failures. Seven patterns never record a step at all. But `progressive_escalation`, `tool_data_exfil` and `tool_shell_escape` record their EARLY steps normally and die only on the FINAL one — the escalation, exfil and escape steps respectively. On that engine the detector accumulates the evidence and then cannot see the act it exists to catch, which is a worse shape than silence because partial state accrues and never resolves. (2) `config_tampering` is the ONLY HARD built-in and BOTH its steps are VM-only, so under `--tree-walk` nothing in the built-in set blocks a read-then-write of govern.json — and `checkPreExecution` has exactly ONE engine call site (`vm.cpp`), so the pre-execution block path does not exist there to catch it either. **Not measured:** whether emissions in `call_dispatch.cpp` are sufficient, or whether turn stamping and event detail formats also diverge. This enumeration changes no code and is not a fix. Fix is not a one-liner: the VM emits from its method-call dispatch, and the tree-walker's equivalent path (`call_dispatch.cpp`) would need the same emissions plus the pre-execution hook, with the same event details and turn stamping — and doing so newly enables ~10 detections on that engine, so it wants the same observe-first treatment as the built-in name and glob repairs. |

## C. Never observed working (stub-only)

Passing tests, no live run has produced the triggering condition. See the
campaign doc's live-status column.

| # | Item | Status | Blocker |
|---|---|---|---|
| C1 | **De-escalation hysteresis** | **observed working (stub)** | `living-script_v3` steps `high → elevated` at turn 18 — first observation of the hysteresis firing. Only the live run remains. |
| C1b | **Confirmed again by the 2x2 (2026-08-18)** — arms at `0.03` were outcome-identical to arms at `0.0`. This row predated that experiment and would have pre-empted it; it was not consulted. **Healing is inert at every shipped rate.** 25 configs set `coherence_natural_healing`, including both templates (0.02) and living-script v1/v2 (0.03). Varying only the rate on v3: 0.0 and 0.03 give **identical** results to the quarantine; 0.25 restores `elevated → normal`. `heal_factor = 1/(1+signals)` halves an already-small number against penalties of 0.08–0.15. | **addressed; R3 withdrawn** | Bound (R1/R2), channel-agnostic booking (S1/S2) and ratchet (R4) shipped. **R3 (raise the rate) is withdrawn on evidence**: the rate at which healing suppresses escalation is damage-relative, not absolute — HIGH is lost at 0.50 in v3 and at 0.15 in the same scenario with two signals disabled. No global constant is correct. **Re-examined 2026-08-25 and UPHELD, with the cliff number corrected**: on two fresh profiles neither 0.13 nor 0.15 suppressed anything, and suppression first appeared between 0.20 and 0.30. The withdrawal now rests on the light profile suppressing ELEVATED at 0.30 — not on HIGH, which that fixture cannot test because it never reaches HIGH with healing off (a vacuous control, stated rather than glossed). See `docs/proposal-bounded-coherence-healing.md`. |
| C1a | **`elevated → normal` is unreachable once coherence floors** — **premise updated 2026-08-26 (#174)**: a behaviour-only recovery path now EXISTS but ships off. R5 relative healing (`context_drift.coherence_healing_damage_fraction`, default 0.0) returns a floored agent to 1.0000/normal at `f=0.5`, verified against a no-healing control on the same fixture. So the row is no longer "cannot", it is "can, if enabled, and only while no signal is firing" — see the S17 row below for why that last clause bites. Whether it should default on is the open decision. Original text: With `coherence_natural_healing` at its default 0.0, coherence never climbs back, and `coherence_prox` alone holds composite above `elevated_threshold` — 18 quiet turns in the v3 run left it pinned at 0.5125 vs a 0.35 threshold. | open | A design question, not obviously a defect: scrutiny earned arguably should not be cheap to shed. But pair it with B1 (a process restart clears the level outright) and the combination is odd — unshakeable within a process, free by restarting it. Both available fixes are loosenings, so this needs a decision before code. **See C1c: live evidence now generalises this from `elevated → normal` to de-escalation from ANY level.** |
| C1e | **S17's frozen warm-up baseline makes an agent pay forever for correct work — and it silently invalidated an acceptance gate.** `persona_baseline_mean` is set ONCE at baseline completion and never updated, and the test is `abs()` on a SINGLE response, so an agent whose later responses differ in keyword count from its warm-up turns fires every turn indefinitely. Measured on the `test_coherence_recovery.sh` fixture (#174): turns 18-40, `persona_fingerprint=0.0500`, every turn, alone, while the agent produced correct on-mandate work. **Exact numbers (2026-08-29, from decision snapshots):** baseline established at turn 6 as mean **7.167**, stddev **1.863**; only **2 distinct values across 40 turns** (`-1.0` then frozen), confirming the set-once guard `persona_baseline_mean < 0.0`. With `persona_deviation_factor` 2.0 the firing band is `|kw - 7.167| > 3.73`, i.e. anything below 3.4 or **above 10.9** keywords. The warm-up turns are terse ("Ledger reconcile part N") averaging 7.2 keywords; the recovery turns are richer ("quarterly totals recomputed and the balance recorded against source ledger section N") and clear 10.9 — so **the agent is punished permanently for producing MORE, scored identically to a collapse, because the test is `abs()` on a SINGLE response.** Note a hypothesis that did NOT survive measurement: the `stddev < 1.0` floor was suspected as the hair-trigger, but the observed stddev was 1.863 and never reached the floor. This evidence supports the DIRECTIONAL fix (b) over naive re-baselining (a) — re-deriving the baseline would also have to decide whether growth counts as drift, which is the actual question. **The consequence was not just noise.** It meant the acceptance gate `test_recovery_default.sh` (RD-03/RD-04, merged in #172) could only ever be passed by a mechanism that OUT-HEALS a live signal — which is suppression: absolute healing at 0.30 pins coherence at 1.0000 for 14 consecutive turns while the signal fires on every one. The gate was red for a reason nobody had checked, and its recorded justification was wrong. | **addressed (DEFAULT ON since 2026-08-29)** | Fixed in #176, default flipped after measurement. **The flip is evidence-backed, not preference:** the frozen baseline inflicts a CERTAIN, UNIVERSAL false penalty (22 of 22 post-drift turns on correct work). The cost is a BOUNDED low-rate hole, measured across 8 arms with no other signal firing in any (so S17 was measured alone) — drift in extra keywords/turn, S17 firings off->on: 0.25: 24->2 (~8% retained, the boiling-frog case); 0.5: 28->12; 1.0: 29->12; 2.0: 30->13. Detection degrades only below ~0.5 kw/turn, never reaches zero, and is flat above. The fixture was ENGINEERED as the worst case (on-mandate vocabulary so nothing else could fire and freeze the baseline), so 24->2 is an UPPER BOUND. Full suite 441/441 0 unexpected; leak 874/0. **LIVE A/B RESULTS (2026-08-29), including one that falsified a claim in this row.** (a) `living-script_extended` vs Gemini: the DEVELOPER agent, at IDENTICAL turn counts (45 v 45) with its non-S17 signals flat (circular 3->3, validation_outcome 4->4, entity_consistency 4->3), fired S17 **16 -> 3**. That is the clean evidence. The run's AGGREGATE (27->10) does NOT survive its own control: arm B was truncated by timeout (108 v 121 analyzed turns) and `semantic_stability` fell FURTHER than S17 relative to that (0.25 v 0.41 of expected), so the aggregate drop is not attributable to the fix. Temperature was not pinned (prompt omission). (b) `living-script_v3` vs a local 0.5B model, greedy + fixed seed: P2 control **perfect** — every signal byte-identical across arms, 39/39 turns. S17 fired **0 times in both arms**, so it says NOTHING about the firing rate. But it revealed what the row and the code comments got WRONG: **this is not a pure leniency change.** On uniform responses the re-derived stddev NARROWS (observed 2.67 -> 1.00, the floor), tightening the 2-sigma band from +/-5.34 to +/-2.00 keywords — a 12-keyword deviation fires with the fix ON and not OFF. The mechanism RECALIBRATES; direction is workload-dependent. **Follow-up check, which shrinks that concern:** the stddev floor of 1.0 is applied in BOTH paths (frozen establishment AND re-derivation — 2 sites), so the minimum +/-2 keyword band PREDATES the fix and is reachable without it whenever warm-up turns are uniform. The fix changes where the band is CENTRED (current behaviour vs warm-up), not how narrow it can get. The v3 comparison was a wide frozen band (varied warm-up) against a tracked narrow one — that is the warm-up being unrepresentative, i.e. the defect. The real open question, PRE-EXISTING and not caused by this change, is whether +/-2 keywords (deviation_factor 2.0 x stddev floor 1.0) is the right minimum tolerance at all. **Anything needing a persistently-firing S17 must now pin `persona_baseline_adaptive: false`** — `test_relative_healing.sh` does, because RH-10 had been relying on this defect to supply its live signal. `context_drift.thresholds.persona_baseline_adaptive` (default **true** since the 2026-08-29 flip this row's own status column records — the "default false" and "ships off" wording elsewhere in this row was written before the flip and was left standing, making the row read as self-contradictory; `governance.h`, `persona_baseline_adaptive = true`; ratcheted false->true as a loosening) re-derives the baseline from the rolling window at the end of any turn where NO OTHER signal fired. Adaptation is gated on independent evidence of health: a degrading agent trips other signals, the baseline freezes, S17 keeps paying. Excluding S17 from its own gate is load-bearing -- while S17 is the only thing firing no turn is ever clean, so the signal would hold its own baseline hostage and the fix would silently do nothing. Gates in `tests/governance_v4/test_persona_adaptive.sh`, 4/4: PF-02 (control) OFF fires 22/22 post-drift turns; PF-03 ON fires 2/22; **PF-04 (positive control) ON fires 10x during genuine drift, identical to OFF's 10x -- detection reduced by exactly zero.** Both candidate fixes named in this row were REJECTED on measurement: directional-only blinds S17 to rambling, naive re-baselining recreates the S9 problem. **Residual risk, open:** an agent whose ONLY symptom is slow persona drift, with no other signal ever firing, would be followed by the baseline -- the boiling-frog case. That was why it shipped off initially; it was flipped ON after the eight-arm measurement above, and the residual risk is accepted rather than avoided. | Known and previously recorded as deliberately-not-changed (`docs/governance-campaign-findings.md`), but that decision predates this evidence. Reopening on new evidence, not on preference. Two candidate fixes, neither traced yet: re-derive the baseline on a rolling window (S9 does this and is deliberately frozen for the opposite reason — so the asymmetry needs its own argument), or make the deviation test directional so producing MORE is not penalised like collapse. Anything here must not weaken S17's ability to catch a genuine persona shift; a positive control for that is mandatory. |
| C1f | **S5's frozen baseline is set from THREE turns, and two lines of startup code condemn a run.** Sibling of C1e, found by tracing the same "frozen baseline" shape across all four signals that hold one. `initial_entropy` is set ONCE, from the first half of the sliding window the first time it reaches `vocab_contraction_window` (6) turns — i.e. from turns 0-2 — and never updated. It is the earliest-set of the four (fixed BEFORE `baseline_complete`) and the **only one of the four not exposed in `snapshotState()`**, so unlike `persona_baseline_mean` / `thinking_baseline_mean` / `input_tokens_baseline_mean` it cannot be reconstructed from preserved evidence. Note also that `turn_types` is built from **RuntimeEventType**, not response vocabulary — the signal's name is misleading; nothing an agent *says* can move it. A pure `agent.send()` loop is `{AS, AR}` forever (entropy 1.0, and `1.0 < 0.6` is false) so S5 can never fire; add k distinct non-agent event types **in turn 0 alone** and the frozen baseline rises past the trip point: k=1 → `initial_entropy` 1.449, threshold 0.869, silent; **k=2 → 1.811, threshold 1.087, fires — and then fires every remaining turn.** Measured on shipped defaults, 40 turns, identical prompts and identical stub responses in every arm: control S5 0/40 and coherence **1.0000** at turn 39; k=3 arm S5 **35/40** and coherence **0.0000 from turn 12**, escalating `normal → elevated` at turn 12 while the control never leaves NORMAL. **Adaptive baselining does not absorb it** — S5 never fires *during* the baseline window, so its expected rate is 0 and every later firing pays full weight 0.15. With `output_admissibility` on (threshold 0.70, streak 5) the same pair diverges to: control completes 40/40; k=3 arm **terminated at turn 12 by `QUARANTINE_STREAK_EXCEEDED`** after 5 × `OUTPUT_INADMISSIBLE`. Two lines of ordinary orchestration — an env read and a base64 call before the first send — is the whole difference. | **partially addressed; ships off** | Pinned as-is by `tests/governance_v4/test_vocab_baseline.sh` (VC-00..VC-04), which asserts CURRENT behaviour so a fix reads as a deliberate gate change; VC-00 is the vacuity guard and VC-01 is the lower half of the trip point, without which VC-02 locates no boundary. **Not claimed:** no shipped example is known to be affected — `living-script_extended` does one `file.read` before its first `agent.create`, which is k=1, *below* the trip point. **Fixed 2026-09-04, default OFF.** Candidate 1 (expose `initial_entropy` in `snapshotState()`) had ALREADY shipped — this row was stale on that point, and VC-07 pins it. Candidate 2 shipped as `context_drift.thresholds.entropy_baseline_adaptive` (default false, ratcheted false->true as a loosening), mirroring `persona_baseline_adaptive`: re-derive `initial_entropy` from the current early half on any turn clean APART FROM S5 — the same self-exclusion S17 needs, because while S5 is the only signal firing no turn is ever clean and a baseline gated on total cleanliness could never move. **Measured, 25 turns, identical response stream:** startup artifact 20 firings/coherence 0.0000 -> 5 firings/0.2500; genuine narrowing (varied for 10 turns then stops, `since_last_check` feed) detected on IDENTICAL turns 20-25 with identical coherence 0.8610, while the baseline demonstrably moved (2.000 frozen vs 1.918 adaptive) — so detection cost is zero and the arm is not vacuous. Pinned by VC-08/VC-09/VC-10, where VC-10 is the vacuity guard without which 'detection unchanged' cannot be distinguished from 'the mechanism never ran'. **The residual 5 firings are structural, not a half-fix:** while turn 0 is still inside the sliding window's early half that entropy is genuinely present. **Why it ships OFF despite a measured zero detection cost** — the standard #176 used to flip S17's equivalent ON: this is ONE narrowing fixture against #176's eight arms at different drift rates, AND under the default feed the re-derived baseline converges to 1.000 where S5 can never fire again, so defaulting it on would silently retire S5 for every existing config. Defensible (under the default feed S5 can only ever fire on the startup artifact) but too large a claim for one fixture. Candidate 3 (directional comparison) remains untraced and is now unnecessary. B6 did NOT have to be settled first, as this row predicted — its fix only had to be AVAILABLE, to make the positive control expressible. **Do not re-derive the "S5 is blind under the default feed" result as a new finding.** An external defaults audit (2026-09-05) reported it as a critical coupling between `event_feed: "turn_bucket"` and `entropy_baseline_adaptive`, on the stated mechanism that the gate is `initial_entropy > 1.0` so an entropy of 1.000 fails it. That constant does not exist: the gate is `state.initial_entropy > config_->thresholds.entropy_min_initial`, default **0.5** (`governance.h`), and 1.000 clears it. The blindness is real but is the SECOND clause and has nothing to do with adaptive or with the feed — `per_turn_types` is a set per turn, so a tool-less agent is `{AS, AR}` in both halves of the window, `recent < initial * entropy_contraction_ratio` is `1.0 < 0.6`, and it is false with the flag ON, OFF, and under `since_last_check` alike (that selector changes WHICH events arrive, not their type variety — a tool-less agent raises no other types to deliver). Pinned as VC-00, the control gate of `test_vocab_baseline.sh`, since before the flag existed.
| C1d | **A compliant agent doing ordinary work floors its coherence in six turns — and it is NOT about code.** v3's DESIGN (1-3) and IMPLEMENT (4-6) are ordinary well-specified on-mandate work. Code arm (keyed runs 4/5/6): coherence 0.74/0.64/0.56 at turn 3, floored turn 7-8. **Prose arm (keyed, same config via `prose_overlay.json`, only the task differs): 0.46 at turn 3, floored turn 5** — outside the code arm's three-run spread and two turns earlier. Signals, code vs prose: `semantic_stability` 29/36 vs **35/36**, `plan_drift` 14/36 vs **29/36**, `entity_consistency` 34/36 vs 35/36. | **located; threshold decision open** | Ordinary varied work of ANY modality trips these thresholds and prose trips them harder, so the suspects are the **0.25 defaults** on `semantic_stability` and `entity_consistency` — NOT the code-aware extractor, which appears to be helping code. **Do not move a threshold on this**: it identifies the suspect, not the value. Two caveats stand — n=1 for prose against n=3 for code, and response length is unmatched (prose input baseline 1717 vs 1220), so modality is not isolated from verbosity. Settle by matching response length across arms and running prose twice. **Update (run 24 + the 2x2):** the suspects now have a MECHANISM, which moves this off "threshold value unknown". Both signals score inter-turn *variation*, so they are anti-correlated with repetition drift: measured 67/67/**0**/37% (S15) and 33/33/**12**/32% (S10) across DESIGN/IMPLEMENT/DRIFT_PRESSURE/RECOVERY — firing LESS during induced drift than during correct work — and across 8 consecutive byte-identical responses in the 2x2 neither fired once. In recovery they fire on a period of exactly 6, matching the scenario's task rotation. That is a defect in KIND, not in value, and no threshold move fixes it: raising the threshold makes them fire more on correct work, lowering it makes them blinder to repetition. See `governance-campaign-findings.md`. |
| C1c | **De-escalation is effectively unreachable for a real agent once coherence floors — measured, not inferred.** Composite reduces to `0.35·(1−coherence) + 0.10·depth + 0.25·min(1, signals/4)`, exact against every row of three keyed runs. With coherence at 0.0 the first term is permanently 0.35, so leaving HIGH (hold 0.55) requires **≤1 signal for 3 consecutive turns** — two signals is 0.575. Across keyed runs 4, 5 and 6 there are **84 post-escalation worker turns; 4 had ≤1 signal, and never two consecutively.** `entity_consistency` fired 34/36 and `semantic_stability` 29/36 in run 6. At ~5% of turns being quiet enough, three consecutive is ~0.01%: it does not happen at any run length. | **open — new, evidence-backed** | This is no longer "the scenario didn't give the mechanism its calm". A real model's responses fire 2+ CDD signals per turn as a matter of course, so the calm the hysteresis requires is not a thing real traffic produces. The decision is whether that is intended. Options, all of them loosenings and none obviously right: raise the per-level hold margin so 2 signals sits below it; count calm on a *rate* rather than a consecutive run; or accept that de-escalation requires healing (which R3 withdrew as unsettable globally, and R5 supersedes). **Do not "fix" this by tuning v3's prompts** — four prompt revisions across runs 3-6 each moved which signal fired without changing that two fire. |
| C2 | **HIGH and CRITICAL levels** | **HIGH live-confirmed, CRITICAL open** | HIGH reached keylessly at turn 5 on 2026-08-09, then **live against a real model on keyed run 2** (ELEVATED turn 8, HIGH turn 10, `drift_worker` the pressure handle at both) — see E6. CRITICAL was deliberately tuned out of reach in both. |
| C3 | **Five campaign findings** — pulse streak field, evidence-count shrink, lease expiry mid-deliberation, config generation guard, CRITICAL suspension | open | All five need adversarial conditions a clean run cannot produce. |
| C4 | **Reviewer separation of duties** (`allowed_actions: ["AGENT_SEND"]` only) | open | Never live-confirmed: no reviewer has yet *attempted* a tool call, so the absence of blocks proves nothing. |

## D. Test-harness debt

| # | Item | Status | Note |
|---|---|---|---|
| D1 | **Suites carried their own one-shot stub port pick** (`(RANDOM % 20000) + 20000`, no bind check, no retry). On a loaded runner a collision or a lingering TIME_WAIT socket leaves the stub dead and every assertion in that suite then fails for a reason unrelated to what it measures — passing locally and red in CI on the same SHA. | **fixed 2026-09-04** | All remaining callers repointed at `tests/helpers/stub_launch.sh`; **zero live inline picks remain outside the helper itself**. Full suite 441 / 0 unexpected, no `stub failed to start` in the run. **The counts in this row were stale in both directions and are recorded here because the miscount was the interesting part:** 35 files matched the string, but 13 were suites that had ALREADY converted and kept a comment naming what they replaced, and 1 was the launcher itself — leaving 21, not the 30 this row claimed. Two grouping attempts also misled before the work started: hashing the local `start_stub` bodies with comments included reported '8 distinct implementations, 5 needing individual audit', which re-hashing on CODE ONLY showed were the same logic with the prose stripped; hashing code only then reported all 21 distinct, which was an artifact of `sed` anchors matching nothing (the empty-input MD5 appeared twice). Textual similarity was the wrong instrument — the verification that works is running them, and the full suite exercises every one. One genuine outlier, `test_escalation_effectiveness.sh`, launches TWO stubs on separate port ranges with no `start_stub` function at all; the converter skipped it rather than mangling it and it was done by hand, capturing the second port as `P2` since the launcher always reports through `STUB_PORT`. A suspected PID leak there did NOT survive checking — the first stub is killed at line 166 before the second starts. |
| D2 | **Tier 3 vacuity audit** — `tests/gorilla`, 62 absence-guarded assertions | parked | Older scenario tests, weakest claims. Tiers 1–2 done. |
| D3 | **Retrofit v1 and v2 to `gatelib.sh`** with per-gate negative fixtures | open | Roadmap increment. v1 has ~151 gates, v2 ~89. |
| D4 | **Keyless runs overwrite `summary.json`**, destroying a keyed record (already lost v2's 108/0/1 once) | open | Fixed in `gate_summary_json`; lands when v1/v2 are retrofitted. |
| D5 | **Keyless placeholder counts are hand-maintained and wrong** — `seq 1 155` (v1) / `seq 1 115` (v2) against real keyed totals 171 / 109 | open | Registry-driven skips fix this; same retrofit. |
| D6 | **No `PHASE\|X\|end` markers** — a phase is only ever "reached", so truncation mid-phase looks like completion | open | `phase_complete()` exists in gatelib; the `.naab` scripts must emit the end markers. |
| D7 | **v2 `summary.json` lacks the `failed` ID array** that v1 carries — a fail count with no identities cannot be triaged later | open | Same retrofit. |
| D8 | **Trust-store leak: cause now understood, fix still not applied.** The store is NOT written during a run — `~/.naab/trusted-keys` is dated Aug 3 and unchanged. What varies is that suites using `trust_setup.sh` repoint `NAAB_TRUST_STORE_DIR` while they run, so a probe issued during that window sees an empty store and succeeds, and the same probe outside it hits `INTEGRITY BLOCK`. | in progress | Not a leak — a missing isolation. **The count was wrong and the exposure is now measured, not inferred.** The row said ~35; it is **81 of 165** suites that write an unsigned `govern.json` without repointing the store (an earlier detector undercounted by 3 more, missing a lowercase `$test_dir/govern.json`, a `$TMPDIR/gov/govern.json` and a bare relative `> govern.json`). Structural count is not exposure, so all 81 were run twice — once against an empty store, once against a store holding one key, neither arm touching `~/.naab`: **50 of 81 change verdict**, every one of them passing clean and failing poisoned. That is 16 `tests/security` suites, the whole of `tests/cli`, 7 `governance_v4`, and both `tests/property` suites. Mechanism confirmed on a minimal case rather than assumed from the pattern: an unsigned `govern.json` with any key installed is `INTEGRITY BLOCK`, exit 3, and the suite's own assertions then fail downstream of it. So 50 suites are green today only because nothing has installed a key — one `--trust-key` on a dev box breaks them all, which is how this cost a wrong reading once already. **The mechanical fix has a landmine: 42 of the 50 already set their own `trap ... EXIT`**, so the documented `trap teardown_isolated_trust EXIT` would silently clobber existing cleanup (stub servers, temp dirs). Isolate on touch, or do it as a deliberate reviewed pass — not as an add-on. **Any ad-hoc probe against a dev container must isolate the store itself**, or its results are timing-dependent. Growth is now pinned by `test_coverage_visibility.sh` CV-02. |
| D9 | **`test_prescan_canaries.sh` could destroy a developer's uncommitted work, and its revert-verification gate could not see the last three injections.** **Corrected 2026-09-03 — this row was wrong in two of its four claims, and the real defect was worse than any of them.** WRONG: (a) "`revert()` has no `cd` to repo root so it fails silently off-root" — line 28 does `cd "$LANG_DIR"` unconditionally before any injection and nothing changes CWD afterwards, so `revert()` always ran from the root; only the `2>/dev/null` half was real. WRONG: "`git checkout --` silently destroys ANY uncommitted work in those two files" — `check_clean()` was wired at 11 call sites and did `exit 1` on a dirty file. CORRECT: (b) an EXIT trap does not run on SIGKILL; (c) C7 ("File correctly reverted") runs BEFORE C8/C9/C10 inject, so the only revert gate was structurally blind to three of them. **The actual defect:** `trap cleanup EXIT` is armed at line 82 and `check_clean` exits from line 110 onward, so the guard's own `exit 1` FIRED the trap, which then ran `git checkout --` on BOTH targets unconditionally. The protection was the delivery mechanism — detecting a developer's work was immediately followed by destroying it. | **fixed** | Fixed 2026-09-03. `classify_target()` runs BEFORE the trap is armed and sorts a dirty target into two cases needing opposite handling: a diff of only `canary` lines is a leftover from an interrupted run and is reverted (this covers (b) — the EXIT trap cannot help on SIGKILL, but the NEXT run cleans up); anything else refuses while `cleanup()` does not yet exist. `cleanup()` now reverts only files `classify_target` cleared. `revert()` no longer swallows errors. C11 asserts every target is clean AFTER the last injection. **Verified on a real accident, not a fixture:** a timed-out run left `void canary_combined_p2(...)` inside a function body; the next run recognised it, reverted it, and completed 11/11. Controls: a foreign edit is refused AND survives; disabling C10's revert fails C11 while C7 still passes, which is the (c) gap shown directly. **Behaviour change:** a dirty target used to mean exit 1 → trap reverts → next run green (self-healing by eating your work). It now stays red until you commit or stash.

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
