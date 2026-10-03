# Template vs. engine defaults — findings (report only)

**Status: report only.** Nothing in `govern-template.json`, `docs/govern-template.json`
or the engine was changed. Fixes are proposals, to follow once the parallel
template-completion work merges. Engine-default changes are proposals only — none
is recommended below.

**Question.** A user can configure a setting by copying the template's value or by
leaving the key out. Where those give different governance, which side is intended,
and why did they diverge?

**Measured on** `fecb13e` (master at session start), Linux container, Debug build.
Re-checked against master `c6a648fc`: its only new commit changes neither
template, `governance.h`, `governance_config.cpp` nor `main.cpp`, so the results
stand. Raw tables: `docs/findings/template-vs-defaults/`. The comparison tool is
code and is kept out of this PR; it is proposed separately (linked from the PR
description), report-only and not wired into CI.

---

## 1. Summary

Copying the root template and omitting keys are different governance setups for
**105 template keys** outside the illustrative content (85 scalar values and 20
example lists; the illustrative content — example agents, contracts, rules,
per-language lists, scoring tables — is another 290 keys), plus **6** more hidden
by aliases, **24 checks** the template switches on merely by containing their
block, and **6 blocks** whose text says the opposite of what they do. Most
differences are example values the template was written with and never labelled.
Five things matter more than the rest:

1. **The template turns the sandbox off.** Both copies set
   `security.sandbox_level` and `governance.sandbox_level` to `"unrestricted"`.
   With the key omitted, `mode: enforce` upgrades the sandbox to `standard`.
   **Verified**: the same program reading `/etc/hostname` and running a `<<shell>>`
   block is refused (exit 1) with the key omitted and runs (exit 0) with the
   template's value. **Drift**: the template value was written 2026-04-18 when
   unrestricted *was* the default; `52b880e5` (2026-05-15) made enforce mode fail
   closed, and the template's explicit value has opted every copy out since.
2. **The template turns off two detectors that are on by default.**
   `restrictions.obfuscation.enabled` and `restrictions.vcs_secret_extraction.enabled`
   ship `false`; the engine has `true // on by default`. Obfuscation is
   **verified** (3-signal JS block: key omitted → exit 3 citing
   `restrictions.obfuscation`; template value → exit 0, block ran);
   vcs_secret_extraction is **traced** (same honoured-flag code shape), not run.
   Both were written `false` by a "sync template with parser" documentation commit
   (`cb42af9b`, 2026-06-01) while the engine at that same commit said
   `true // on by default`.
3. **The template's secret-scrubbing for subprocesses does nothing.**
   `subprocess_scrub_mode` and `blocked_subprocess_prefixes: ["AWS_", "GITHUB_",
   "NAAB_SIGNING"]` sit at the top level; the loader only reads them under
   `capabilities.env_vars`. **Verified**: `AWS_PROBE` reaches a `<<shell>>`
   subprocess with the template's placement (identical to omitting the keys) and
   is scrubbed when the same keys are nested where the loader reads them.
4. **The template doubles the script timeout.** `limits.timeout.global: 60`
   overrides the template's own `runtime.timeout: 30`; with both omitted the CLI
   default is 30 s. **Verified**: a 40 s loop is killed at 30 s with the keys
   omitted and completes with the template's values. `naab-lang init` writes the
   same 60.
5. **Default changes reach the templates by hand, and none of the four I found
   reached both copies** — two reached neither, two only the root.
   `context_drift.adaptive_baseline_enabled` was flipped to `true`
   in #167 (`e7f63246`); neither template followed, so both still ship `false` —
   the configuration #167's own measurement (its failure-mode-map fixture) says
   false-kills correct work in 16 of 16 cells. `44cfd852` (reality checkpoint)
   and `6888073f` (CDD signals S8–S19) flipped the root template but not
   `docs/govern-template.json`, which still ships five of those as `false`. The
   sandbox change (item 1) is the fourth.

Items 1–4 and the adaptive-baseline value hold identically in both copies
(checked); the behavioural arms replicated those values in minimal configs.

**No engine default change is proposed.** Where the record says which side is
intended, it is the engine side (#1, #2, #3, #6, #7; and #13, whose decision
explicitly keeps the engine default and puts the other value in the template).
Where the record is silent — the example values — changing the engine would change
behaviour for every user who omits the key, while a template comment changes
nothing for anyone.

**Who this reaches** (checked, because a prior review of this template attached
severity to "operators copy it" without checking — `docs/investigation-method.md`):
the project website's *Project Starter Kit* (`docs/governance.html:367-385`) tells
users to `curl` the **root** template and rename it to `govern.json`, while its
*Download Template* button serves the **docs** copy. So both copies are distributed
as starter configs (documented instruction — actual usage is not observable from
here). A verbatim copy of either template exits 4 until its `extends:
./base-govern.json` line is removed (observed for both copies), so everyone who
uses one edits it — the values below are what survives that edit.
No test loads either template; `test_d1_reconciliation.sh` A06 only greps the root
copy for key names.

---

## 2. Method, provenance and evidence tiers

**Instrument (authored by me; the system it measures is the real loader).** A
C++ dumper, generated from clang's AST of `governance.h`, prints every scalar field
reachable from `GovernanceRules` (166 structs; the only opaque field is the derived
`custom_rules[].compiled_pattern`). It loads configs through
`GovernanceEngine::loadFromString()` — `loadFromJson()` + `enforceMinimumLevels()`,
the same parse `loadFromFile()` runs — linked against naab-lang's own libraries.

For every template node (2,106 in the root copy, 1,641 leaves) the config
"template minus that node" is loaded and its parsed rules diffed against the full
template. Three further passes cover what one deletion cannot:

| pass | what it separates | root result |
|---|---|---|
| deletion | template value ≠ value with key omitted (section kept) | 468 leaves differ (53 are `rationale`/`description`/`message` text) |
| perturbation | no-diff leaf: live-and-equal vs reaches no rules field | 626 live and equal; 547 reach no rules field |
| isolation | no-effect leaf alone in an empty config: shadowed by a sibling vs unread | 7 shadowed, 221 unread, 9 array-indexed (not isolated), 310 scanner (separate pass) |
| alias masking | live, no-diff leaf whose field still differs from the empty config | 6 (found the sandbox row — see §8) |

Every row also records the **empty-config** value, because a loader fallback
(`.value("k", x)`, object-form defaults) can make "key omitted, section kept"
differ from "section omitted" — five rows in §5.3 do.

**Positive controls.** The dumper reports `adaptive_baseline_enabled` default
`true` and a one-key config flips it (the tool's run script refuses to report
without this). Each behavioural verification ran matched arms on the identical program
with the omitted-key arm required to fire.

**Tiers used below.** **verified** = behaviour run with matched arms and a control
that fired; **observed** = parsed-rules value from the harness; **traced** = read
through source to the point of effect, not run.

**Provenance.** Counts are outputs of my harness over the real loader
(authored instrument, observed system). Behavioural results are observed under
configs I wrote (configured). History is `git log -S/-G/-L` and `git blame` on the
template line and the engine initialiser; quoted commit messages are the record,
not my inference.

**Blind spots (the list is a floor, not a census):**
- keys resolved through `getenv()` at load (`*_key_env`, `webhook_auth_env`,
  `signing_key_env`) look inert because the variables are unset here — 4 such keys
  are in the "unread" set and **are read** (control: `hmac_key_env` changes the
  parsed key once the variable is set);
- process globals set by the loader outside `GovernanceRules` — exactly one exists,
  `naab::limits::setMaxJsonDepth` (found by enumerating the loader's side effects;
  added by hand in §5.2);
- post-load logic in `main.cpp` — I read the sandbox upgrade and the
  `runtime.timeout`/`memory_limit`/`gc_threshold` sentinels; other `main.cpp`
  consumers of the rules were not traced;
- raw-JSON readers — the `scanner` section (compared separately, §6);
- consumer-side substitutions for empty lists (a consumer may fall back to a
  built-in list when the template's list is omitted) — not traced per list;
- explicitness: copying the template marks 127 keys `explicitly_set`, which only
  matters under `extends` (an explicitly set child key overrides the parent even
  when equal to the default) — not compared;
- per-agent defaults are compared only for keys the template's two example agents
  carry;
- template comments were searched (`_comment*` keys, "default"/"recommend"), and
  `docs/` for the divergent values; a rationale recorded elsewhere could be missed.

---

## 3. Findings table — rows that change enforcement or claim protection that is absent

Direction: **looser** = copying the template weakens governance relative to
omitting the key. *Dormant* = the enclosing feature ships disabled in the template,
so the value takes effect only once a user enables that feature.

| # | Setting | Template | Key omitted | Effect / tier | History | Class | Proposed fix |
|---|---|---|---|---|---|---|---|
| 1 | `security.sandbox_level` and alias `governance.sandbox_level` | `"unrestricted"` (both, both copies) | absent → `standard` under `mode: enforce` (`main.cpp:258`) | **looser**, live. **Verified** (`/etc` read + shell: omitted → refused, exit 1; template → allowed, exit 0) | Template value since `56f25210` (2026-04-18), when unrestricted was the engine default (opt-in sandbox since `88442367`). `52b880e5` (2026-05-15) "enforce mode now defaults to standard sandbox"; template not updated. Both spellings carry the same value, which is why single-key deletion missed it (§8). | **Drift** — engine changed | Delete both keys from both copies (or write `"standard"`), with a comment: absent = `standard` under enforce; `elevated` is needed for absolute paths. Tightening for new copies. |
| 2 | `restrictions.obfuscation.enabled` (+`level`) | `false` (`level: "hard"`) | `true` (`level: SOFT`) | **looser**, live. **Verified** (3-signal JS block: omitted → exit 3 `restrictions.obfuscation`; template → exit 0) | Engine `true // on by default` since `9d7b3b8a` (2026-05-20). Template block first written by `cb42af9b` (2026-06-01, "sync govern-template.json with parser — 30+ missing keys added") as `false`; the engine at that commit already said on-by-default. Never matched. | **Born mismatched** in a documentation commit. Intent undetermined (no comment, message describes documenting keys); evidence leans drift | `enabled: true`. Keep `level: "hard"` only with a comment saying it is a recommended stricter level. |
| 3 | `restrictions.vcs_secret_extraction.enabled` (+`level`) | `false` (`"hard"`) | `true` (SOFT) | **looser**, live. **Traced** (`governance_checks.cpp:5855` gate honours `enabled`; `test_restrictions_enabled_key.sh` lists it as a key-honouring sibling); not run | Engine on-by-default since `174b92e3` (2026-05-05). Template: same `cb42af9b` block. | Same as #2 | Same as #2. |
| 4 | `subprocess_scrub_mode`, `blocked_subprocess_prefixes` `["AWS_","GITHUB_","NAAB_SIGNING"]`, `allowed_/blocked_subprocess_vars` | at the **top level** | loader reads them only under `capabilities.env_vars` (`governance_config.cpp:899`) | Template claims scrubbing it does not do; effect = omitted. **Verified** (`AWS_PROBE` reaches a `<<shell>>` subprocess under the template placement; nested placement scrubs it) | Placed at top level by `cb42af9b` (2026-06-01); the loader has only ever read the nested form. | **Authoring error** (misplaced), never effective | Move the four keys into `capabilities.env_vars`. Tightening. |
| 5 | `limits.timeout.global` (overrides `runtime.timeout`) | `60` (and `runtime.timeout: 30`, shadowed) | script timeout 30 s (CLI default; `runtime.timeout == 30` is treated as unset, `main.cpp:2019`) | **looser**, live. **Verified** (40 s loop: omitted → "Execution timeout" at 30 s; template → completes at 40 s) | `limits.timeout.global: 60` since the v3.0 template (`92a8fc10`, 2026-03-02); the `!= 30` sentinel and `std::max(cli, config)` mean govern.json can raise but never lower the 30 s CLI value (already noted in `open-investigations.md`). Also written by `naab-lang init`. | **Example value, undocumented**; template contradicts itself (two timeouts, the larger wins) | Pick one value and delete the other key; if 60 is intended, say so in a comment. |
| 6 | `context_drift.adaptive_baseline_enabled` | `false` | `true` | Different failure mode, dormant (CDD disabled in template). Observed | Template written `false` by `a2dde369` (2026-06-03) when the engine default **was** `false`. #167 (`e7f63246`, 2026-08-23) flipped the default and touched neither template. #167's message: an explicit `false` reproduces FALSE_KILL[varied,verbose] in 16 of 16 cells of its failure-mode map (that commit's authored fixture). `context_growth` (S12) and `persona_fingerprint` (S17) also depend on it (CLAUDE.md). | **Drift** — engine changed | Delete the key or write `true`, both copies. |
| 7 | `docs/govern-template.json` only: `context_drift.signals.{semantic_stability, mandate_alignment, response_quality, thinking_collapse}`, `context_drift.reality_checkpoint.enabled` | `false` (docs copy); root copy `true` | `true` | Dormant (CDD disabled). Observed | `44cfd852` (2026-06-24) flipped `reality_checkpoint.enabled` in the engine and the root template; `6888073f` (2026-06-25) flipped the 12 semantic signals in the engine and the root template. Neither touched the docs copy. | **Drift** — partial propagation | Sync the docs copy (better: generate it from the root, §7). |
| 8 | `taint_tracking.level` | `"soft"` | `"hard"` | **looser** once taint is enabled; dormant in the template. Traced. `naab-lang init` writes `soft` **and enables taint**, so it is live on that path | Template `b1dab53c` (2026-03-19), one day after the engine introduced `"hard"` (`03a7cae8`, 2026-03-18). | **Born mismatched**, intent undetermined | `"hard"`, or a comment saying soft is recommended and why. |
| 9 | `polyglot_optimization.enforcement_level` | `"advisory"` | `"soft"` | **looser**, live (section on by default). Traced (consumer `governance_reports.cpp`) | Both introduced in `92a8fc10` (2026-03-02) with different values. | **Example value, undocumented** | Align, or comment. |
| 10 | `code_quality.drift_detection.{max_comment_only_ratio, min_hollow_export_complexity, max_function_gain, min_complexity_baseline}` | `0.8`, `3`, `0`, `0` | `0.7`, `5`, `0.5`, `10` | Mixed: the first two **looser**, the last two stricter (`0` gain = any added function trips; `0` baseline = no trivial-function skip). Dormant (drift detection off). Observed; consumers traced (`governance_engine.cpp:5640-5818`) | Engine `3b423c8f`/`59f0b789` (2026-04-22/23). Template values written by documentation-sync commits `dbb1e4eb` (2026-05-17) and `cb42af9b` (2026-06-01). | **Born mismatched** in documentation commits | Align to the engine values. |
| 11 | `code_quality.complexity_floor.check_polyglot` | `false` | `true` | **No effect** — nothing reads it. Traced: the only `.check_polyglot` consumer is `drift_detection`'s namesake (`governance_engine.cpp:5828`), which is also why the leaf-name inert sweep cannot list it | Template set `false` by `2abb6864` (2026-03-16) as a workaround in the same commit that removed the broken polyglot floor check. | **Workaround left behind** | Delete from the template; the field itself is a candidate for the inert register. |

## 4. Findings table — value mismatches that do not loosen

| # | Setting(s) | Template → omitted | Effect | History | Class | Proposed fix |
|---|---|---|---|---|---|---|
| 12 | `context_drift.weights.{circular, scope_creep, repeated_failure, vocabulary_contraction}`; `context_drift.fingerprint_window`; `behavioral_sequences.window_size` | 0.15→0.10, 0.10→0.15, 0.10→0.05, 0.10→0.15; 10→20; 100→200 | Mixed tuning, dormant (CDD and BSD off). Observed | Engine values from `e928c1a7` (2026-05-17; vocabulary `02ef8446`). `e928c1a7` also wrote them into the docs template **matching the engine**; `c95a54b6` (2026-05-27, "sync website templates with latest root versions") deleted them; `8728852a` (2026-05-28) re-added the block to the root template with different numbers and no rationale; `37846e49` copied root to docs. The same values are in `docs/CLAUDE-TEMPLATE.md:1163-1208`; BSD `100` is also written by `naab-lang init`. | **Drift** — a matching copy was lost and re-authored differently | Restore the engine values (both templates and `CLAUDE-TEMPLATE.md`). |
| 13 | `context_drift.coherence_natural_healing` | 0.02 → 0.0 | Dormant. Measured inert at 0.02 by a prior campaign (`open-investigations.md` C1b) | `docs/security-decisions.md`: "the engine default remains 0.0 (opt-in) with 0.02 as the recommended template value." | **Deliberate, documented — but not in the template** | Add a template comment citing that decision. |
| 14 | `telemetry.tamper_evidence.{algorithm, chain_genesis}`; `telemetry.forward_retry_count`, `forward_shutdown_drain_ms` | `"hmac-sha256"`→`"sha256"`; `""`→`"NAAB-GOVERNANCE-GENESIS"`; 3→2; 3000→5000 | Dormant (tamper evidence off; forwarding needs `webhook_url`). The verifier takes a file's first `prev_hash` as genesis, so `""` is accepted; whether an empty genesis weakens its legacy-restart detection is traced only (`governance_reports.cpp:381-386`), not run | Engine `bcf647a0` (algorithm, genesis), `f229dd9b`/`11964c4f` (2026-06-05; drain bounded at 5000 ms for the M2 hang). Template written by `4d38ae0e` (2026-06-06), an env_vars commit. | **Born mismatched** | Align to the engine values. |
| 15 | Levels: `no_oversimplification` hard→SOFT, `no_incomplete_logic` hard→SOFT, `no_hallucinated_apis` soft→ADVISORY, `no_apologetic_language` soft→ADVISORY, `intent_validation.missing_level` soft→ADVISORY (dormant) | stricter | Observed | Levels from the v3.0 template (`92a8fc10`); `missing_level` from `b99bed26` ("govern-template sync"). | **Example values (stricter), undocumented** | Comment as recommended levels — the user's own bar: fine if the comment says so. |
| 16 | Stricter examples with a reader: `capabilities.env_vars.write` false→true; `audit.level` basic→none and `audit.log_events.{polyglot_timing, taint_decisions, contract_checks}` true→false; `restrictions.resource_abuse.enabled` true→false; `trust.max_signature_age_days` 90→0; `polyglot_optimization.profiling.enabled` true→false; `limits.data.max_json_depth` 20→64 (`json.parse` depth, a process global outside the dump — traced, §2); 12 of the 20 other `limits.*` keys, those not in the inert set (e.g. `execution.call_depth` 100→0 = unlimited). Dormant: `exposure_tracking.{max_autonomous_actions 50, max_unique_agents 10, coherence_floor 0.3}`→0, `agent_review.cache`, `telemetry.output_file`, `telemetry.transcript.output_file` | stricter | Observed; "has a reader" is traced by consumer grep, not verified per key (`limits` split taken from the inert register) | Mostly born with the v3.0 template (`92a8fc10`) or in the same commit as the engine field (`922041ec` cache, `e00f78b8` signature age, `af478432` exposure). | **Example values, undocumented** | Comment each as "default X; template recommends Y". |
| 17 | Mismatched but **inert** (no reader): `capabilities.network.allow_raw_sockets` false→true; `code_quality.no_secrets.suspicious_variable_names.enabled` true→false; `polyglot_optimization.language_diversity.max_single_language_percent` 80→70; `polyglot.parallel.*` and `polyglot.persistent_runtime.*` (5); 8 `limits.*` keys (the 6 in `inert_keys_baseline.txt` plus `dict_size`, `string_length`, which the loader itself warns about); `agents.*.stop_reason_action` ""→"end" | none today | Traced (no reader outside the loader/generator) | Mostly v3.0. | Inert either way; becomes a real mismatch the day the key is wired | Mark RESERVED in the template (it already does this for `capabilities.process/time/memory`) or delete. |

## 5. Presence semantics — differences no single key shows

### 5.1 The template switches on 24 checks that are off by default
These checks are enabled by the **presence of their block**, not by a value
(observed: deleting each block flips its `enabled` field to `false`, and each is
`false` in the empty config — 24 of 24):
`code_quality.{no_secrets, no_placeholders, no_hardcoded_results, no_pii,
no_temporary_code, no_simulation_markers, no_mock_data, no_dead_code,
no_debug_artifacts, no_unsafe_deserialization, no_sql_injection, no_path_traversal,
complexity_floor, encoding, no_oversimplification, no_incomplete_logic,
no_hallucinated_apis}` (17), the `no_secrets.entropy_check` sub-block (1), and
`restrictions.{dangerous_calls, shell_injection, privilege_escalation, imports,
code_injection, crypto}` (6). The six blocks of §5.2 are the same mechanism with
contradicting text and are not counted here. The template exists to show every
option, so this is **deliberate by
construction** — but no comment says that a block's presence turns the check on,
or that omitting it is the default (off). The one place it is said,
`complexity_floor.weights`' comment, shows the pattern. **Fix:** one comment line
at the top of `code_quality` and `restrictions`.

### 5.2 Six blocks say `"enabled": false` and are turned ON
Observed in the loader's own warnings when the template loads:
`requirements.error_handling` (the `level` key enables it),
`requirements.naming_conventions`, `code_quality.no_hardcoded_urls`,
`code_quality.no_hardcoded_ips`, `code_quality.no_apologetic_language`,
`code_quality.max_complexity`. Copying the template gives these six checks ON; the
template text says off; omitting the block gives off.
**History:** decided engine-side in `e1be9c62` (#154, `open-investigations.md`
A1b) — honouring the flag was rejected as a loosening, a warning was added instead,
and the template text was left as it was.
**Class:** template text stale against a recorded decision.
**Fix — needs a choice:** (a) delete the six `"enabled": false` lines: behaviour of
copies unchanged, text becomes true (**recommended**, no loosening); (b) delete the
six blocks: copies match the defaults, but new copies stop enforcing six checks —
a loosening that should be decided explicitly, not slipped in.

### 5.3 Three configurations, not two
For these, "key omitted but its block kept" differs from "block omitted":

| Setting | Template | Key omitted, block kept | Block omitted |
|---|---|---|---|
| `code_quality.no_placeholders.level` | soft | **HARD** (object-form default) | check off |
| `code_quality.no_hardcoded_results.level` | advisory | **HARD** | check off |
| `requirements.main_block.level` | soft (with `enabled: true`) | **off** — `level` is the trigger, `enabled` is ignored | off |
| `code_quality.intent_validation.enabled` | false | **true** | false |
| `telemetry.tamper_evidence.enabled` | false | **true** | false |

Not drift: these are loader fallbacks (`parseEnforcementLevel` defaults an object
form to HARD; `intent_validation`/`tamper_evidence` default on when their block is
present). They mean "I copied the block and trimmed it" is a third setup. **Fix:**
a template comment per block; no engine change proposed (aligning the fallbacks
with the initialisers would loosen the first two rows).

## 6. Scanner section (`scanner.*`, read as raw JSON by `scanner.cpp`)

Every numeric option in the template equals the default at its `getNumOption()`
call site, and every check the template names exists. The differences:
5 checks disabled by the template and enabled by default (`redundancy.single_use_variable`,
`style.inconsistent_quotes`, `security.hardcoded_ip`,
`lang_rules.naab.hardcoded_return_value`, `lang_rules.naab.dict_key_schema_check`);
65 per-check `level`s (hard/advisory) where the scanner's fallback is a uniform
`"soft"`; `scan.exclude_patterns` and two option lists (73 rows,
`root_scanner.tsv`; identical for the docs copy). Traced (static comparison by my script). In the
auto-run (`interpreter.cpp:1102`) the level only classifies the printed summary —
nothing blocks on it there — so consequence is low. Note the section's presence
itself turns on the per-run auto-scan; with no `scanner` section there is none.
Written by `47820bdd`/`cf12a57b` (2026-03-15/16, "scanner checks to match
implementation"). **Class:** example per-check levels, undocumented. **Fix:** a
comment that unlisted checks run at `soft` and that the levels shown are
recommendations.

## 7. The mechanism

- **Nothing ties template values to engine defaults.** Four default changes,
  four propagation outcomes: `52b880e5` (sandbox) — neither copy; `44cfd852`
  (reality checkpoint) and `6888073f` (CDD signals) — root only; `e7f63246`
  (adaptive baseline) — neither. No test loads a template or compares a value.
- **Two copies, both distributed.** They carry different key sets (the root has
  `telemetry.transcript`, `circuit_breaker.coherence_correction_*` and
  `mandate_reinforcement_*` that the docs copy lacks; the docs copy has
  `circuit_breaker.output_admissibility` and `signals.prompt_compliance` that the
  root lacks — the parallel completion work's territory, not compared here), and
  the docs copy carries five stale values (#7). A third partial copy of the CDD
  values lives in `docs/CLAUDE-TEMPLATE.md`.
- **Documentation commits author values.** `cb42af9b`, `dbb1e4eb`, `4d38ae0e`,
  `b99bed26` set out to add missing keys and wrote values different from the
  engine's — including the two disabled detectors (#2, #3) and the misplaced scrub
  keys (#4). A key added to "document the parser" should carry the parser's default
  unless a comment says otherwise.

**Proposed guard (for the follow-up, not built here):** a test that runs the
deletion + masking passes of the comparison tool over both copies
and fails on any field difference not listed in a *deviation register*, each entry
naming the template comment that justifies it. It would have failed on all four
flips above, and on every doc-sync commit that wrote a non-default value. Plus an
equality check between the two copies, or generate the docs copy from the root.

## 8. Self-audit (what I got wrong on the way, and what is still open)

- **Alias masking hid the worst row on the first pass.** The template writes
  `sandbox_level` twice with the same value; deleting either one alone changes
  nothing, so the deletion pass reported "matches default". Found by an added pass
  comparing each live field with the empty-config value (6 masked rows; §2).
  Any row-by-row deletion audit has this hole wherever two spellings agree.
- **Env-indirected keys read as "unread".** `*_env` keys resolve through
  `getenv()` at load, so with the variables unset every value looked inert. Four
  such keys are in the unread set; one was confirmed live with a control. They are
  UNMEASURABLE by this harness, not inert.
- **A process global sat outside the dump** (`max_json_depth`); found by
  enumerating the loader's side effects, not by the harness.
- **My first sandbox probe could not discriminate** — it read a file under
  `/tmp`, which `standard` allows. That exact trap is already written up in
  `open-investigations.md` A13; I repeated it, caught it because both arms passed,
  and switched to `/etc/hostname` + a shell block.
- **The first draft of this report understated its own counts** — "about 70
  settings" (the artifact says 105 keys) and "22 checks" (24). Both were written
  from memory and caught by recounting from the saved tables.
- **Reachability was checked late.** I measured before asking who uses the
  template; the website instruction (§1) was found afterwards. It supports the
  premise; it does not measure usage.
- **Re-tracings.** Behavioural verification: 4 rows (#1, #2, #4, #5), one run each
  with matched arms — single samples, deterministic paths. The packaged tool
  reproduced every count of the exploratory run from a cold start (reproducibility,
  not an independent re-trace). Everything else is observed or traced at the
  parsed-rules level, which says the configuration differs, not that a given
  program behaves differently.
- **Direction of error.** The method under-reports (masking, env indirection,
  globals, untraced `main.cpp` consumers), so the counts are a floor. The
  classification of "born mismatched" rows as drift-leaning is a judgement from
  commit messages that describe documenting keys; what would settle each one is the
  author's statement of intent — none was found in `docs/`.
- **Not covered:** `naab-lang init`'s config is a third setup with its own
  differences (`docs/findings/template-vs-defaults/init_leaf_diffs.tsv`, 141 rows). It
  does **not** carry rows #1–#4 or #6, but shares #5 (timeout 60), #8 (taint
  `soft`, and enables taint), and the BSD window from #12.

## 9. Proposed template edits (for after the merge — none applied)

| Priority | Edit | Direction for new copies |
|---|---|---|
| 1 | Remove `sandbox_level` from `governance` and `security` (both copies), comment the enforce-mode default | tightening |
| 1 | `restrictions.obfuscation.enabled` and `vcs_secret_extraction.enabled` → `true` | tightening |
| 1 | Move the four subprocess-scrub keys into `capabilities.env_vars` | tightening (makes the stated policy real) |
| 1 | `adaptive_baseline_enabled` → `true` or delete (both copies) | recalibration, per #167 |
| 2 | One timeout: delete `runtime.timeout` or set `limits.timeout.global` to 30 | tightening if 30 |
| 2 | Sync `docs/govern-template.json` with the root (#7) | restores engine defaults |
| 2 | Delete the six `"enabled": false` lines (§5.2 option a) | none |
| 2 | `taint_tracking.level` → `"hard"` or comment | tightening when enabled |
| 3 | Align #10, #12, #14 to engine values; delete #11 | mixed, dormant |
| 3 | Comments for #9, #13, #15, #16, §5.1, §5.3, §6; RESERVED markers for #17 | none |

## Reproduce

With the separate tool PR checked out and naab-lang built (needs clang++ and
python3):

```bash
bash tools/template_defaults/run.sh                          # root copy
bash tools/template_defaults/run.sh docs/govern-template.json # docs copy
```
Outputs: `leaf_diffs.tsv` (template / key omitted / empty-config value per field),
`masked.tsv`, `isolate.json` (shadowed / unread / skipped / scanner), `scanner.tsv`
-- the files saved in `docs/findings/template-vs-defaults/` (`isolate.json` saved
as `*_no_rules_effect.json`). The behavioural arms (§3 #1, #2, #4, #5) are small
programs under a two-line `govern.json`; their configs are given in the rows and
were run with an isolated `HOME` so no trusted key in `~/.naab` could turn every
arm into an integrity block.
