# Changelog

All notable changes to NAAb will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed -- Bundle 19 PR 4: module boundaries, template clean-up, test debt (migration notes)
- **An importer reaches only the functions a module exports, on both engines (A216).** On the VM a read of a module's unexported function from code outside that module is refused; on the tree-walker `m.hidden()`, `import {hidden}` and the import dicts refuse it. A module with no `export` at all keeps every function public, as before.
  - **Migration:** `export` the function the importer calls.
- **A module's exported variable keeps its taint across the import, on both engines (A224).** A module dict holding a tainted export is tainted as a whole, a named import is decided per name, and on the tree-walker only the importer's binding is marked. Before, a tainted value exported by a module reached the importer's sinks unflagged.
  - **Migration:** sanitize the value where the taint policy expects it, in the module or before the sink.
- **A built-in method call's result on the VM takes its arguments' run-time taint (A217)**, as the static rule and every other call route already did: `d.merge(tainted)` is tainted.
- **A TypeScript block that ends in a statement has the value null (A221)**, as a JavaScript block does; its printed output is unchanged.
  - **Migration:** end the block with an expression when its value is used.
- **Template keys nothing parses are gone from both templates and `naab-lang init` (A175(b), decision 6).** Of 140 such keys, 48 that ask for a refusal, detection, limit, isolation, integrity check or evidence record that nothing provides now draw a load warning (`"... is not read -- nothing enforces it."`) when a config sets them to a value that asks for it; 94 leave quietly; 4 are read by another reader (the scanner, the reload, `must_differentiate`) and stay. A config written by an older `init` still loads with no unknown-key line: the retired keys are known (`tests/governance_v4/template_retired_keys.txt`).
  - **Migration:** none needed. A warning names a setting that never did anything; remove it, or use the check its hint names.
- **The template's `agent_review` names agents its own `agents` block defines (A29(e))**: a `reviewer` agent and the `security_review` scorer. It named agents the template did not define.
- **The template says intent findings stay advisory without a `function_intents` map (U-016).**

### Fixed -- Bundle 19 PR 4
- **A reply on which the reality checkpoint fired carries `reality_checkpoint` and its notice again, and `agent.pipeline()` passes that pressure to the next stage (A229).** Since #49 the drift check recorded the checkpoint at one turn and the reply asked about the next, so no reply ever carried the field: the next stage's `state.inherited_pressure`, its `Governance pressure:` header line and its `upstream_provenance.pressure` were never set. A fired checkpoint now reports the sustained count it fired at; it read 0.
  - **Migration:** a pipeline's later stages now inherit the earlier stage's pressure, so with `reality_checkpoint` at its default level (soft) a later stage can be refused sooner than before.
- **The debug module's snapshot, watch and timer tables are under one lock** (DV-07): concurrent `async fn`s wrote them unguarded. Under ThreadSanitizer, DV-07's program drew 166 and 161 race reports in 2 runs with the lock removed and none in 3 with it; its earlier one-call-each form drew none even without the lock, so it now loops.
- **78 test suites that write an unsigned `govern.json` no longer depend on the keys the machine trusts (D8).** With any key trusted, an unsigned config is an integrity block, so 50 of them failed on a machine with one key installed. Each calls the new `isolate_trust_store` (`tests/helpers/trust_setup.sh`), which needs no EXIT trap; `test_coverage_visibility.sh` CV-02 is now absolute.
- **Test arms that could not fail.** The pipefail guard also flags a command piped into a status-verdict filter (`python3 -c ... sys.exit`, `jq -e`, A223); `test_scanner_false_positives.sh` is registered, and its T6, T7 and T8 (which could not fail) were fixed.
- Tests: `tests/governance_v4/test_module_export_taint.sh`, `tests/vm/test_module_exports_only.sh`, `tests/vm/test_typescript_value.sh` (TV-06..TV-10), `tests/governance_v4/test_a64_untested_paths.sh` (A64's untested functions: 13 arms, 17 mutants in its header), `tests/governance_v4/test_checkpoint_response.sh` (A229: 6 arms, 4 mutants), new arms in `test_no_effect_settings.sh` (NE-06..NE-11), `test_template_coverage.sh` (TC-06/TC-07), `test_agent_review_settings.sh` (AR-17), the taint tables and `test_debug_vm_scope.sh`. `tools/protmap/unprobed.txt` reasons re-checked (D15).
- Recorded while doing it, open: A225 (a JavaScript block whose last line does not end in a bracket, after a statement), A226 (a block's printed output loses its last newline when it also returns a value), A227 (`no_secrets.entropy_check` is read by no check), A228 (a dict-field call reaches the tree-walker's async branch ASYNC-001 calls unreachable), A230 (`await` takes a whole sum as its operand, so `"A=" + await f + await g` concatenates a future on the VM).

### Fixed -- a request a model receives carries text that is not UTF-8 (A209; migration note)
- **One byte that is not UTF-8 no longer ends the run before a request is sent.** A Latin-1 byte in a prompt, a tool's result, a guidance section, a tool's description, history the program wrote or the program's own source made the request builder throw (exit 1, `[json.exception.type_error.316]`), and the model received nothing; a valid result cut by `tool_result_max_chars` inside a multi-byte character did the same, and a dict result holding the byte reached the model as `Error: failed to serialize tool result`. The byte is now sent as U+FFFD, and the cut lands on a character boundary.
- **Agent review now reviews a program whose source holds such a byte.** Every reviewer call failed on it, so under the default `fail_policy: "open"` the program ran unreviewed.
  - **Migration:** such a program can now be refused by its review. A scan now judges the text as sent, with U+FFFD in place of each invalid byte, so a value at the edge of a length-bounded pattern can now be refused (`password = "abcdef<byte>"` matches the 8-or-more Hardcoded Password pattern under `no_secrets`). A cut tool result can be up to three bytes shorter than `tool_result_max_chars`.
- Tests: `tests/governance_v4/test_request_utf8.sh`. Recorded while fixing it: A211 (a program can write an agent handle's history, which no scan reads; open).

### Changed -- Bundle 19 PR 3: language, tooling, VM correctness (migration notes)
- **`codegen.run()` treats text equal to a model output as tainted, whatever route brought it (A161).** It looks the code up in the model-output ledger when the argument carries no taint, so a plain `agent.send()` reply passed to `codegen.run()` is refused unless `codegen.allow_tainted_code` is set; before, only a reply after a tool loop was.
  - **Migration:** a program that runs model-written code through codegen sets `"codegen": {"allow_tainted_code": true}`, deliberately.
- **`no_hardcoded_results` reads `check_dict_status_fields` and `check_perfect_scores` (U-014).** Both defaulted to true, were written by the template and `init`, and were read by nothing. A block returning a constant success dict (`return {"status": "success"}`, every value a literal) or setting a score-like name to a constant 100, 100.0 or 1.0 is now flagged at the check's level. Measured on the repository's 1,698 polyglot blocks first: 2 flagged, both constant scores in test fixtures.
  - **Migration:** compute the status or score from the input, or set the key to `false` in `no_hardcoded_results`.
- **`custom_patterns` on a simple `code_quality` check add to its built-in patterns instead of replacing them (A105).** Seven checks: `no_temporary_code`, `no_simulation_markers`, `no_dead_code`, `no_debug_artifacts`, `no_unsafe_deserialization`, `no_sql_injection`, `no_path_traversal`. `patterns` still replaces.
  - **Migration:** a config that used `custom_patterns` to REPLACE the built-ins uses `patterns`; one that meant to extend them gets the built-ins back (no config in this repository set either).
- **VM arithmetic between a float and a non-number is a type error (A215).** `"a" * 2.5` printed 20 and `[1] - 1.5` printed 33.5 (the value's handle read as a number), exit 0; the tree-walker already refused them.
  - **Migration:** a program that produced such a value now stops with the type error the int forms already gave.
- **On the tree-walker, `d.merge(x)` returns a new dict and leaves `d` unchanged (A69)**, as the VM always did.
  - **Migration:** a program that relied on `merge` changing its receiver assigns the result (`d = d.merge(x)`).
- **A `for` over a range on the VM is not array growth (A66).** `for i in 0..n` counts without building the list, so `limits.array_size` no longer refuses the loop; a range VALUE (`let xs = 0..n`) is still capped.
- **`naab-lang check` binds what imports write and exactly the VM's prelude (A188).** `use`/`import` aliases are known names, `use codegen` and local modules check, and `string + any` / `string * int` are strings; a module the VM's prelude lacks, used without `use` (`regex.match` with no `use regex`), is now refused by `check`, as the run refuses it.
  - **Migration:** add the `use` line `check` asks for.
- **An object or array argument of a tool call reaches the NAAb function as a dict or a list (F52)**, not as JSON text, read within the 64-level nesting bound; a deeper call is refused with an error result.
  - **Migration:** a tool that called `json.parse()` on such an argument uses it directly.
- **`debug.env()`, `debug.stack()`, `debug.snapshot()` and `debug.trace()` answer on the VM (F46)**, from its frames; they returned `{}`, `[]`, nothing and `<repl>`. What `debug.env()` and `debug.compare()` return carries the taint of any tainted local they read.
  - **Migration:** under a taint sink, printing `debug.env()` output while a tainted local is in scope is refused, as printing the local is.
- **The display sanitizer leaves the engine's own help text alone (A39(a)).** A `Rule:` line, and a `Help:` or `Example:` section, keep their values; the secret shapes are still redacted everywhere, and a program's own values outside those sections still are.

### Fixed -- Bundle 19 PR 3
- **`x |> mod.fn(a)` means `mod.fn(x, a)` on the VM (A67)**, and meets every gate the direct call meets; the README's own `"hello world" |> string.upper() |> string.split(" ")` failed. The tree-walker still refuses a member target.
- **A line break after a dict `:`, a `let`/`const` `=`, a parameter default `=`, an enum `=`, a struct field `:` or a destructuring `=` parses (A190).**
- **A one-line JavaScript block with two statements returns its last expression (A179)**, and a one-line statement has the value null.
- **A TypeScript block's value is no longer also printed (D18).**
- **Block search works (A170):** the FTS5 index no longer returned nothing, re-indexing no longer duplicates results, an index in the old format is rebuilt when opened, and a failed query is reported instead of returning nothing.
- **`debug.is_tainted("x")` answers from the value with `debug` from the prelude, and for a computed name (A219)**; it answered false for a tainted local there.
- **The scanner's `missing_imports` reads code, not comments or string text (A218).**
- **`agent_review` warns at load when it is enabled with no detection agents, or its `scorer` names no enabled `scorers` entry (A29(d)).** What runs is unchanged.
- Tests: `tests/parser/test_line_breaks.sh`, `tests/cli/test_check_modules.sh`, `tests/cli/test_blocks_search.sh`, `tests/cli/test_error_redaction.sh`, `tests/vm/test_float_nonnumber.sh`, `tests/vm/test_pipeline_module.sh`, `tests/vm/test_range_loop.sh`, `tests/vm/test_typescript_value.sh`, `tests/vm/test_debug_vm_scope.sh`, `tests/governance_v4/test_tool_loop_reply.sh`, `tests/governance_v4/test_hardcoded_status_score.sh`, `tests/governance_v4/test_custom_patterns_add.sh`, `tests/scanner/test_missing_imports_code_only.sh`. Recorded while building it, open: A216 (each engine's import exposes unexported functions), A217 (the VM's `merge` method drops its argument's run-time taint), A220, A221, A222, A223, A224 (a module's exported variable loses its taint across the module boundary, on both engines).

### Documentation -- Bundle 19 PR 3
- `docs/API_REFERENCE.md`: regex patterns are ECMAScript (`[^\]]` is any character but `]`); a POSIX class such as `[[:alpha:]]` works only where the pattern has no lookaround or backreference (A222).

### Changed -- Bundle 19 PR 2: init, a run past 10,000 results, plugins that cannot load (migration notes)
- **`naab-lang init --governance-preset standard`, `strict` and `paranoid` write taint tracking and sequence detection on (A75)**, as plain `naab-lang init` already did; `relaxed` turns them on only with `--taint`. `init` and the generated file now say what the standard sandbox refuses -- shell blocks, `env.get()`, reading outside this directory, home and the temp directory -- and that `"security": {"sandbox_level": "elevated"}` allows them.
  - **Migration:** a project generated with a preset now refuses a taint flow, such as file contents passed into a `<<python>>` block, unless a `sanitize_`/`validate_`/`escape_` function cleans it; regenerate with `--governance-preset relaxed`, or edit `taint_tracking`, if that is not wanted.
- **`naab-lang init` writes nothing it was not asked for (A176).** An existing `naab.toml` is kept unless `--force`; `init --help` and `-h` print init's usage; an argument init does not know, a flag missing its value, and an unknown preset name refuse (exit 1) and write nothing. A preset name is read without case.
  - **Migration:** a script that ran `init` with a misspelt flag (`--preset strict`) got a standard config and now gets exit 1; use `--governance-preset`.
- **Under enforce, every governance plugin loads before the program runs, and one that is missing or does not load refuses the run (exit 4), naming it (A192(b)).** A plugin rule that gives no verdict -- its function is missing, it throws, it returns no dict with `"pass"` -- counts as failed at the rule's own level instead of being skipped. Audit and off keep the warning.
  - **Migration:** fix or restore a plugin file the run reports; a `hard` rule whose function throws now refuses what it checks.
- **The program may not change a governance plugin's helpers, or put a lookalike where a plugin looks for one (A192(a)).** Every file a plugin reads while it loads, every place its `use`/`import` looked before the file it found (its own folder, `naab_modules/` up the tree, `~/.naab/modules`), and a new `.naab` file directly in a plugin's folder are refused to program writes (HARD); reads stay allowed.
  - **Migration:** write program output elsewhere than a plugin's folder and the `naab_modules/` entries its imports name.

### Fixed -- Bundle 19 PR 2
- **A run past 10,000 check results keeps its own counts (A193).** The quality gate, the summary and `Governance: PASS|REFUSED` lines, the dashboard and the blocked check now count the results the memory cap evicted: an advisory recorded early used to stop failing `advisory_violations > 0`, and the run ended `Governance: PASS`.
- **A run past 10,000 check results no longer slows down sharply (A177):** the cap compacts in batches of 1,000. 20,000 governed calls hit the 30-second run limit before; they now take about a second (unoptimised build).
- **Telemetry writes every check result once, or says it could not (A107).** It resumed from a position in the result list, which the cap moves, so results recorded between two writes were skipped; it resumes by record number now, and `GovernanceCheckSummary.results_not_written` counts results that left memory before a write.
- **A token budget near `INT_MAX` is reached (A110 residual).** The run's call and token totals and the per-agent token counts are 64-bit; a count shown to the program stops at `INT_MAX` instead of wrapping negative.

### Changed -- Bundle 19 PR 1: security and silent no-ops (migration notes)
- **The run's governance now judges the compile phase too (A116).** A governance plugin's own calls made while the program compiles -- `env.get` of a blocked variable, for one -- are refused as they already were while it runs. A plugin file itself is read as the operator's: a lease, a capability manifest or a `blocked_paths` entry does not stop the engine loading it.
  - **Migration:** a plugin whose rule reads something the project withholds from the program now ends the run at compile; give the plugin what it needs some other way.
- **A config that loaded may now be refused at load (exit 4), naming the key:** a `capabilities.functions` entry that is not an object, such as `"default": []`, which used to restrict nothing (A187); and, anywhere in the config, an `enabled` that is not `true` or `false`, a `level` that is not a level name, or a `limits` value that is not a number -- each was dropped to its default, leaving the check off or the limit gone (A111). Only a key the loader reads as that setting is judged: a function, agent or output-contract field the user named `level` or `enabled` is a name and still loads. A limits `rationale` is text and still loads. Over the repository's 189 configs, only two written to be invalid are affected.
  - **Migration:** write `"enabled": true`, not `"true"`; a level as a string (`"hard"`); a limit as a number.
- **An environment now applies the check levels it sets (A185).** `environments.<name>` with `code_quality.no_placeholders.level: "hard"` (or the nested form) raises that check, and turns it on if the file leaves it off; it used to be dropped in silence. An environment can only raise a level: a lower one is ignored with a warning, as a lower mode already was. Any key an environment cannot change now draws a warning naming it. A mid-run reload that drops an environment's raise is refused.
  - **Migration:** a run with `--env` whose environment names check levels now enforces them.
- **`capabilities.network.allowed_ports` now binds the program's connections (A87):** `http.*`, URL imports and embedded Python's sockets. It was read and applied to nothing.
- **A governance hook starts from the environment NAAb started with (A53).** A bare command name is found on that `PATH`, and the hook no longer sees variables the program set. `blocked_read` values still reach it, as before.
- **The variables that hold the engine's keys are withheld from the program (A33(b)):** the ones `telemetry.tamper_evidence.hmac_key_env`, `audit.tamper_evidence.hmac_key_env` and `audit.provenance.signing_key_env` name. `env.get` on one is a HARD block, and no child process receives it.
  - **Migration:** a program that read one of these variables is now refused; it should not need the engine's keys.
- **`capabilities.env_vars.read: false` is a HARD block at every env function (A183).** It used to reach the sandbox first, whose refusal a `try` caught, so the run ended PASS. Under audit the refusal is now recorded.
- **The top level has its own function-capability entry (A186).** A `"main"` entry in `capabilities.functions` grants the main block and module loading (`use mod` is a top-level FS_READ); `"default"` still applies without one, and the advisory now proposes a `"main"` entry rather than widening `"default"`.

### Fixed
- **The program can no longer write a governance plugin file (A192).** `file.write` over the plugin govern.json names ran, and the next run skipped the rule the plugin had enforced. Every program write to a plugin file is now refused (HARD); reading it is still allowed.
- **Under `extends`, a child's `dangerous_calls` or `main_block` level no longer lowers its parent's (A191).** A child's `"level": "advisory"` ran `eval()` its parent refused at `hard`: the floor raised one copy of the setting and the check read another.
- **A banned `Function(` no longer refuses JavaScript's `function` keyword (A184)**, in `banned_functions` and `restrictions.dangerous_calls.blocklist_extra`. `new Function(...)`, `Function(...)` and an alias of it are still refused, and other languages still match without case.
- **The `regex` module no longer refuses ordinary patterns (A189)** such as `([a-z]+ )*`, `(\s*,\s*)+` or `(a|ab)*c.*d`: the matcher is linear for them since A89. A slow pattern is still stopped by the time limit and the step budget.
- **`debug.is_tainted("x")` answers correctly on the default engine (F35)** for a local or captured variable; it answered false for tainted data. A name computed at run time still reads false there (recorded).
- **The version check runs with the suite (A181).** `tests/self-audit/test_version_consistency.sh`, which checks that every file declaring NAAb's version agrees, that both binaries print it and that `scripts/bump-version.sh` reaches every declaration, ran nowhere since 2.0.0; `run-all-tests.sh` runs it now. It reads the files with Windows line endings removed, so it measures the committed text on every runner, and a run that cannot measure fails.

### Documentation
- `docs/GOVERNANCE-LIMITS.md` §8, "What a Restart Forgets": what governance state lives only in the process (B1, documented by decision rather than persisted); §7.6 and the summary table no longer say `--timeout` can extend a governance timeout, which A79 fixed.

### Changed -- one workflow publishes a release (A180; migration note)
- **A release page is published once, with its notes and every file.** `release.yml` now makes the signed binary, the SBOM and the provenance file itself (they were `supply-chain.yml`'s jobs). Its release step runs after every other job and publishes all 17 files in one step, and it refuses to publish when one is missing. Before, the supply-chain workflow created and published the page seconds after the tag, with no notes and none of the four packages: v2.0.2's page was public that way for 26 minutes, and v2.0.0's build failed and left one published. `supply-chain.yml` now only scans and may not write to the repository, and `fuzz-nightly.yml` gets the read-only token every other workflow has. Test: `tests/self-audit/test_release_publisher.sh`.
  - **Migration:** from v2.0.3, the cosign certificate of `naab-lang.sig` names `.github/workflows/release.yml@refs/tags/vX.Y.Z` (v2.0.2 and earlier: `supply-chain.yml`). A verification that pins the workflow must accept the new name. `SECURITY.md` now shows a check that accepts both and no other repository; the command it showed before accepted a signature made by any GitHub repository's workflow (A182).

### Changed -- claims as evidence (Bundle 20 PR 5; migration note)
- **S19 (`claim_result_reconciliation`, on by default) reads a reply's claims with one claim reader (A202)**, `include/naab/claim_reader.h`, instead of looking words up in the reply's keyword set. That set dropped "done" (a stop word), split a snake_case tool name into words so such a tool was never checked, read "not successful" as success, counted "could" as a failure word, and counted a reply that named a tool with no status word as accurate. The reader skips code, quotes, questions, conditionals and infinitives, applies negation ("no errors" says it worked) and reads a modal as a prediction; a tool counts only when the reply names it and says one thing about it.
  - **Migration:** coherence changes for tool-using agents: S19 now fires where it could not (a snake_case tool the reply misreports, a negated claim), and stops counting replies that claim nothing. `context_drift.thresholds.claim_accuracy_min` was parsed and read by nothing; it is now warned at load and left out of the template.
- **S24 `claim_validation` (off by default):** a reply whose claims were only positive ("fixed", "the tests pass", "this should work") is charged when the next `agent.record_validation()` fails -- a flat penalty, never baseline-absorbed, beside S22's charge for the failure itself; a reply that admitted the failure is not. A failed validation after the handle's last send is scored at exit. Its latch is consumed whether or not S24 is on.
- **`AGENT_CLAIM` telemetry**: each claim a scored reply makes (at most 16 per reply), with its kind, direction, negation and byte span, never the text. **`VALIDATION_RECORDED`** gains `turn`, `settles` (the claim ids it settles: the latest reply's, once), `claims_only_positive` and `contradicts`.
- **`naab-gov claims FILE|-`** prints what the claim reader makes of a reply (report only), and **`naab-gov replay` checks claims** (a seventh check): ids against their own fields, each settlement against earlier claims, the contradiction re-derived, the reply against an `AGENT_RESPONSE` or the transcript, and, with `--transcript`, the reader re-run on the reply's text.
- **A negative CDD weight is warned at load:** the signal reads on and never pays.

### Fixed -- Bundle 20 PR 5
- **Evidence is written when text is not UTF-8 (A207).** One byte that is not UTF-8 -- a Latin-1 comment in a refused function, a playbook saved in a legacy encoding, a file name -- made the evidence writers throw: a run lost its telemetry, its JSON report and its SARIF, an advisory-only run ended exit 1 instead of 0, a HARD refusal left the next run's history nothing to remember, and a guidance pull with telemetry on ended the run. Every evidence writer (telemetry, the audit trail, the transcript, the JSON and SARIF reports, the run-history carry, the baselines) now passes text through one helper, `include/naab/utf8_text.h`, which replaces each invalid byte with U+FFFD; a guidance delivery's `recorded_sha256` uses the same helper, so the recorded text and its hash agree. Tests: `tests/governance_v4/test_evidence_utf8.sh`, `tests/unit/utf8_text_test.cpp`.

### Added -- did guidance help? Measured after the run, never decided at it (Bundle 20 PR 4; migration note)
- **`naab-gov efficacy TELEMETRY`** reports, per guidance section, trigger and model, what followed each delivery to an agent over the next three analysed drift turns of its handle: the triggering signal paid again (`recurred`), did not (`quiet`), the run ended first, the window spanned two models, or another intervention landed in it (`undetermined`). Report only; exit 0 a report, 2 UNMEASURABLE (nothing verifies, nothing was delivered), 4 usage. The next run's start judges the previous run's deliveries the same way, carries per-section totals in the run history, and its end-of-run guidance block names a section whose trigger came back after every judged delivery. Nothing at runtime chooses guidance by these outcomes.
- **`guidance.holdout`** (0..1, default 0, fixed when the run starts) withholds that fraction of would-be agent deliveries, recorded as `GUIDANCE_WITHHELD` reason `holdout`, so a delivered and a held-out arm can be compared; the draw is reproducible from the run id. Without a holdout the report says causation is UNMEASURABLE.
- **A `history` guidance source** (`{"type": "history"}`): one section per rule earlier runs refused, named for the rule, with which functions were refused, when and at which level, and which later passed. The automatic violation route finds it by the rule's name.
- **`naab-gov replay` checks guidance** (a sixth check): each delivery's recorded text against its hash, each reply against its delivery, `trigger_recurred` against the reply's drift turn, and the holdout draws.
- **Telemetry:** `GUIDANCE_DELIVERED` gains `recorded_sha256` (the text as recorded: the writer removes absolute path prefixes, so it can differ from `sha256`, the text as delivered), `section_sha256` and `section_key`; a candidate skipped for its cooldown is `GUIDANCE_WITHHELD` reason `cooldown`; `CDD_TURN` and `GUIDANCE_RESPONSE` gain `model`, the model that answered.
  - **Migration:** a reader that counts `GUIDANCE_WITHHELD` events as policy withholding should filter on `reason`: `cooldown` and `holdout` are new reasons and are not withheld by policy. The template's guidance sources list gains `{ "type": "history" }` (the template's guidance block is off).

### Fixed -- Bundle 20 PR 4
- **After a tool loop, `agent.send()`'s `trace` named no model, provider or key, read one attempt and 0 ms, and `agent.usage()`'s latency stayed 0** (A199): the loop's reply replaced the send's record. The trace is the send's own call again. Tests: `tests/governance_v4/test_guidance_efficacy.sh`, `tests/unit/guidance_efficacy_test.cpp`; the stub answers by request path (`path_routes`, `test_stub_routing.sh` SR-10).

### Changed -- run history acts: code an earlier run refused is checked again where it is met (Bundle 20 PR 3; migration note)
- **A function an earlier run refused, moved unchanged into an imported module, is checked there under the rule that refused it**, at that rule's level in the current govern.json. A module's functions otherwise skip the heuristic checks (oversimplification, the complexity floor, the cosmetic-sanitizer half of `no_incomplete_logic`); only the remembered rule runs, and only for a function whose code fingerprint matches and that has at least 12 tokens or the same name. Its telemetry row names the run and the row that refused it (`history_ref`).
  - **Migration:** a program that passed by importing a function an earlier run of the same project refused may now be refused; fix the function. A project with no history (telemetry or its chain off, or a fresh checkout) is unchanged.
- **A refused run names precedent:** up to two lines per refused rule, `-- Precedent for RULE: fn F (FILE) was refused in run A and passed in run B --`, on stderr before the verdict, from earlier repairs at the same or a stricter level. A rule switched off or lowered is never shown as a precedent.
- **A `must_produce` contract refusal is remembered** for a function defined in the entry program. When it later passes, telemetry and `naab-gov history` say whether the change that made it pass inserted a golden case's input and expected value as literals (`golden_case`). That note is a fact, never a verdict, and is not printed by the run. Test: `tests/governance_v4/test_refusal_memory.sh`.

### Added -- run history: what earlier runs refused, and what became of it (Bundle 20 PR 2; migration note)
- **A run remembers what earlier runs of the project refused.** With `history.enabled` (default true) and telemetry with its chain on, `naab-lang run` reads what earlier runs refused from their telemetry and records, per refused function, whether this run refused it again or repaired it -- a repair is a pass of the same rule for the same function at the same or a stricter level, so switching the rule off or lowering it is never one. A refusal of a function-body check names its subject on its telemetry row (function, file, a hash of its text and a code fingerprint), the refused text is kept in `.naab_cache/`, and each run's telemetry carries a chained summary the next run reads. History changes no verdict in this release. The end of a run prints `-- History: N refusal(s) remembered from earlier runs, M repaired in this run --` before its verdict when there is history; `naab-gov history TELEMETRY` shows it in full. Tests: `tests/governance_v4/test_run_history.sh`, `tests/governance_v4/test_subject_identity.sh`.
- **`naab-lang init` turns telemetry and its chain on for every preset**, and writes `"history": {"enabled": true}`.
  - **Migration:** a project made by `naab-lang init` now writes `telemetry.jsonl` and `.naab_cache/` beside its govern.json; a project whose telemetry or chain is off has no history and nothing changes for it. A refusal row carries four new fields (`subject_function`, `subject_file`, `subject_sha256`, `subject_fp`), and the complexity floor records a pass where it judged a function and found it met.

### Changed -- the evidence a run's history will stand on (Bundle 20 PR 1; migration note)
- **The webhook's credential is withheld from the program like the chain keys (A33(b)).** Bundle 19 PR 1 withholds the variables named by `telemetry.tamper_evidence.hmac_key_env`, `audit.tamper_evidence.hmac_key_env` and `audit.provenance.signing_key_env`; the one named by `telemetry.webhook_auth_env` -- sent as the `Authorization` header when telemetry is forwarded -- is now withheld the same way: `env.get` refuses it (HARD), and child processes and embedded Python's `os.environ` do not receive it. A program that read it could post forged events to the operator's webhook. Test: `tests/governance_v4/test_key_env_withheld_routes.sh` (all four variables over each route).
  - **Migration:** a program that read this variable itself must stop; it was the operator's secret.
- **A tool may not record a validation, reset or commit for the agent that is running it (A197).** `agent.send()` and `agent.propose()` already refused this; `agent.record_validation()`, `agent.reset()` and `agent.commit()` did not, so a tool the model called could record a pass for its own agent, and a tool that reset its own agent crashed the process. A tool acting on another handle, and the script after the turn, are unaffected. Test: `tests/governance_v4/test_tool_self_grading.sh`.
- **The governance baseline is protected like the calibration file (A196).** With `governance_baseline` enabled, the program may read `governance_baseline.path` and may not write or delete it (`governance.state_files`, HARD). A program that rewrote it before the end of the run hid its own regression. Saving it stays `--governance-baseline-save`. The state-file refusal no longer says the file holds scoring weights.

### Fixed -- Bundle 20 PR 1
- **A check result recorded after the run's last report write reaches telemetry and the reports (A195).** A HARD refusal made after it -- a `must_produce` contract runs after the program -- was printed and missing from telemetry and the JSON report, and a clean run never wrote the after-run audit's results, the contracts' passes or the baseline check. Every fatal exit now writes the reports, and a clean run writes them once more at its end; telemetry resumes by record number (A107), so nothing is written twice. The stderr `Telemetry: N events written` line prints the run's running total. Test: `tests/governance_v4/test_telemetry_after_cap.sh`.

## [2.0.2] - 2026-11-19

Covers `v2.0.1..v2.0.2`: 38 merged pull requests (#350 to #387), Bundles 8 to 18 and settings-liveness step 3. Every command under Added is new in this release; an entry further down that changes or fixes one of them records work done before the release, and the Added entry describes the command as it ships.

### Upgrading from 2.0.1
Most changes only make governance refuse more. These are the ones an existing project can notice. Each is described in full below, with its remedy.
- **A config that loaded may now be refused at load (exit 4):** a restriction written in a type the loader does not read, such as `"enabled": "no"` or a nested list (A111); an `agents` block written as a list (A100); a host entry holding `*` (A147); a `custom_rules` pattern that does not compile (A131); an `extends` child whose allow list has nothing in common with its parent's (A98); an import allowlist for a language whose imports cannot be listed, or an unknown import mode (A175).
- **Settings that did nothing now enforce:** the twelve keys of settings-liveness step 3, including a 10-second `tool_timeout_seconds` default for every agent with tools and the refusal of an undated signature under `trust.require_fresh_signature` (A175); `limits.timeout.per_block`, `languages.per_language.<lang>.timeout` and `limits.code.max_lines_per_block` (A2); `custom_rules` on NAAb code at `naab-lang run` (A131). 25 settings that enforce nothing now draw a load warning.
- **Refusals a program could catch now end the run:** a refused `codegen.run()` at `codegen.level`, default `hard` (A88), and a rate-limit refusal, HARD (A133).
- **Overrides that loosened no longer do:** an `extends` child runs under its parent's restrictions (A98); an environment can only raise the mode and turn a gate on (A95(b), A130); a misspelled `security.sandbox_level` under enforce no longer leaves the run unrestricted (A94); a mid-run reload is compared on every key the loader reads, and an edit of a key with no single direction (hook commands, retry tuning, state-file paths) is refused (A104); `--approve --expiry` may shorten the configured lifetime, never extend it (A175).
- **Checks read more:** taint tracking follows branches, matches, callbacks, struct fields, captured variables, every bound variable and sinks that name a NAAb function (A117, A120, A127, A129, A141); `must_derive_from` needs every listed parameter in the function's code, where a comment or a plain string used to do (A134); an output contract applies to a bare `return` (A114); every completed behavioural sequence is enforced (A113); `no_secrets` reads credential macros and `restrictions.crypto` the CryptoAPI constants (A148); the governance voice's and agent review's texts are scanned before they are shown (A166). A governed program may not write `.naab_cache/` (A169).
- **Some false refusals are gone:** the VM no longer flags a clean value for the name of a tainted one (A121); `no_apologetic_language` matches whole words (A149); and an execution contract for a function the program neither defines nor imports is skipped and named, where it blocked every program in the project (A134).
- **Output changed:** the summary and closing lines say `warned` / `refused`, a refused run ends with `-- Governance: REFUSED (rule) --` (A39(c), A174), and check records carry `disposition` (A172); `--verify-telemetry-chain` exits 1 on a line over 1 MiB or nested deeper than 64 levels (A163); the `path` module returns `/` on Windows.
- **Elsewhere:** a child process that runs node 24 needs `"runtime": {"memory_limit": 600}` (A145); prerequisite checks run only at `naab-lang run` and `--attest` (A122).

### Added
- **`naab-gov heal TELEMETRY --config GOVERN.JSON`: what a run's own record says could be stricter (Bundle 15).** Report only, with no model: it proposes one kind of edit, a level key the config writes as `"advisory"` raised to `"soft"`, where that key's rule fired at least `--min` times (default 3) in the run. It writes and signs nothing; a person makes the edit and signs it outside the run. It proposes only from evidence that holds: `naab-gov replay` on the same file must verify the chain of every run and report no decision DIVERGED, and a run counts only when its recorded `config_fingerprint` is the given config's and it accepted no mid-run reload. Each proposal is asked of `naab-gov ratchet` and kept only when the ratchet accepts it and names the key as tightened. Named and not proposed: a repeated rule whose level the file does not write (a level key added mid-run is refused by the ratchet), a `level` the engine does not read, and an edit the ratchet refuses (a shorthand level such as `"no_debug_artifacts": "advisory"`). Exit 0 nothing to propose, 1 a proposal, 2 UNMEASURABLE (the evidence did not hold, or no run matched), 4 usage. Test: `tests/governance_v4/test_gov_heal.sh`.
- **`naab-gov patch-check BASE PATCHED`: the gate a patch written by an agent must pass before a person looks at it (Bundle 15).** It compares two directory trees, with no model anywhere, and exits 0 accepted, 1 refused, 2 UNMEASURABLE, 4 usage. It refuses a patch that adds, changes or removes a governance file (any `govern.json`, `*.sig`, `*.manifest.json`, anything under a `trusted-keys` directory, a file the base config's `extends` chain or `governance_plugins` name, a project context file such as `CLAUDE.md`, or anything under the new `patch_check.protected_paths`); that changes or removes an existing test file (a base file under `patch_check.test_paths`, default `tests/` and `test/`; adding one is allowed); or that adds or changes a symbolic link. Where the base config has contract clauses and a `.naab` file changed, it also runs `naab-gov contracts` on each changed program, by default (the owner's decision, 2026-11-15), and refuses a contract counterexample, a governance block or a parse failure the base did not have; the search runs the patched program's functions in a governed worker, and is never run when the patch touched a governance file. `--no-run-contracts` leaves it out (`--run-contracts` is accepted and changes nothing). Printed and deciding nothing: a lexical assertion-site count before and after, what the draft manifest newly lists, and the taint flow counts. UNMEASURABLE when the base holds no test file, when the config has contract clauses and `--no-run-contracts` was given, or when the contract search could not measure a contracted function of a changed program or gave no report for it (on Windows the search has no worker process, so there such a patch is refused or reads UNMEASURABLE, never "accepted"). The config read is the base tree's, never the patch's. A person still reviews and merges. Test: `tests/governance_v4/test_gov_patch_check.sh`.
- **`naab-gov explain FILE`: the deterministic findings, then a model's explanation that decides nothing (Bundle 15).** It prints what `naab-gov manifest infer` and `naab-gov taint` say about the program and, with `--telemetry FILE`, the sequence patterns the run matched (`BSD_MATCH` events, read through the bounds `naab-gov replay` uses). Its exit status is fixed at that point: 0 every section was gathered, 2 UNMEASURABLE (a section could not be), 4 usage. Only then, when `output.explainer` names an entry of `agents`, that agent is asked once to word the findings; the reply is printed under a label saying a model wrote it, records no check result, writes no file and cannot change the status; its control characters are shown as `?`. The model is sent the findings only, never the program: names, paths and hosts that are not plain (whitespace, more than 96 characters) are withheld, and of a `BSD_MATCH` event only the pattern name, level, step count and path are read -- its `events` text, which a program or a model wrote, never is. Findings that match the secret patterns are not sent; a reply that matches the secret patterns (unless the config sets `no_secrets` false) or, under `no_pii`, the PII patterns is withheld. `--no-model` asks nobody; `--json` keeps the explanation apart from the findings. New key `output.explainer` (default empty: no model is asked). `naab-gov manifest infer --stdout` prints the draft and writes nothing. Test: `tests/governance_v4/test_gov_explain.sh`.
- **The measurement extended (Bundle 13 PR 3): the checks lint applies to NAAb code, ten more languages, a second config.** `tools/govmeasure/naab_lint.py` lints every `.naab` file with its polyglot block bodies emptied, to count what the 15 lint-only checks flag on NAAb text (A132); `semgrep_tests.py` turns the Semgrep rules repository's own test files into labelled cases for Python, JavaScript, shell and seven more languages (fetched at run time and pinned to a commit; under their licence never committed, uploaded or quoted); the scorer gains `--group source|category` and `--any-rule-only` (for a config with hard-level checks, where a hard block ends the scan). Report only: no check changed. Results in `docs/governance-measurement.md`; A149 recorded.
- **A measurement of what the block checks catch, against labels made outside the project (Bundle 13).** `tools/govmeasure` sends labelled code to `naab-gov check` and reports, per category and rule, how many flawed and how many sound cases were flagged, with 95% intervals: NIST's Juliet C/C++ suite (each file's flawed and fixed variant) and EvalPlus's published model-written solutions, labelled by running their hidden tests. Report only: it runs nightly, changes no verdict, and reports UNMEASURABLE rather than a rate when the checker or a control fails. The first results are in `docs/governance-measurement.md`.
- **`naab-gov replay TELEMETRY`: checks recorded decisions against their recorded evidence (Bundle 14).** It re-executes nothing. It reads a telemetry file and reports, per decision, MATCH, DIVERGED or UNMEASURABLE: each run's hash chain (the same check as `--verify-telemetry-chain`, with the same key source, or `--hmac-key`); each output-admissibility decision against its own coherence, threshold, baseline state and configured actions, and against the coherence its snapshot and the turn's `CDD_TURN` record; each analysed `CDD_TURN`'s coherence against the previous one and the penalties and adjustments it lists; each `TRANSCRIPT_REF` against the transcript (`--transcript FILE`); and each execution event attributed to a model reply against the `AGENT_RESPONSE` it names. Exit 1 when anything diverged; otherwise 2 when anything is unmeasurable -- evidence that is not in the files given, a chain sealed with a key that was not given, a line over the size or nesting bound, a file with nothing to check -- and 0 only when every decision checked matches. `--json` for the report as JSON. It loads no govern.json and runs nothing. The scorer is not re-run, so a penalty that should not have been charged is not seen.
- **Provenance for generated code (Bundle 14).** When telemetry is on, `CODEGEN_EXEC` and `POLYGLOT_EXEC` record `code_sha256`, the SHA-256 of the code that ran, and `code_attribution`: `model_output_match` when that exact text equals something a model wrote in the same run, otherwise `unattributed`. On a match the event names the model (`origin_model`), the hash of the request body as it was sent (`origin_request_sha256`), the hash of the model's whole reply (`origin_content_sha256`), the agent and turn, and how the text was obtained (`origin_derivation`: the reply, the reply as `agent.send()` returned it, a proposal candidate, a string argument of a tool call, or `agent.extract_code()` output). The match is equality of text, not a claim about where the code came from: code the program edits before running reads `unattributed`, and a text a model and the program both wrote reads as a match. Hashes only -- no model text is written. There is no new setting. `AGENT_RESPONSE` gains `content_sha256` (the whole reply) and `request_sha256`; its `content_hash`, which covers the first 500 characters, is unchanged, so anything that compares it across versions still matches (A160).
- **`naab-gov diff OLD NEW`: what an edit of govern.json allows and refuses (Bundle 13).** It loads each file as a run would, from the file's own directory, and judges the same requests under both with the engine's own gates: reading and writing each environment variable either file names, what a child process receives, each path and host either file names, the shell, every language, and the secret and PII scans, without and with `--governance-override`. It prints the requests that are newly allowed and newly refused, then the settings as a mid-run reload of the first file by the second would judge them (what it would refuse, and every other change). `--env` and `--agent-id` select the environment and role, `--json` prints the report as JSON. Report only: exit 0 when nothing differs, 1 when something does, 4 for a file that does not load. It lists what it does not judge as requests (taint, contracts, limits and others are compared as settings only).
- **`naab-gov taint FILE`: a static report of where tainted data reaches a sink, implicit flows included (Bundle 12).** It reads FILE under the govern.json a run would find (or `--config PATH`) and lists each flow of tainted data to a configured sink. A **value** flow is data computed from a source, the kind the run's taint check judges. A **control** flow is a sink reached, or a value set, only on a path tainted data chose. Examples: `if t == "x" { http.get(A) }`, a loop over tainted data, code after a `return` on a branch over it, the right side of `&&`. No run-time check judges control flows. It is report only: it refuses nothing and runs none of the program, so the flows can be measured before any is enforced (loading the config runs none of its `prerequisites`: they run only where a program runs, A122). What it cannot follow is counted, and a report that found nothing with something left unfollowed exits 2 (UNMEASURABLE), not 0 (0 no flow, 1 flows, 4 a usage or config error). `--json` gives the same report as data. `tools/taintflow/measure.py` runs it over a tree of programs under a fixed policy, and `tools/taintflow/crosscheck.py` compares it, line by line, with what a run of each test program flags under its own config; the measurement is in `docs/taint-flow-report.md`, and its findings are A120-A122 in `docs/open-investigations.md`.
- **A signed capability manifest per program, and `"manifest": {"required": true}` (Bundle 11).** `naab-gov manifest infer PROGRAM` drafts `<stem>.manifest.json` beside the program from what it shows statically: the actions its stdlib calls need (as the run's own gates classify them), the languages of its polyglot blocks and of `process.run` inline code, the hosts and files its literal arguments name, the agents it creates, and every NAAb module it imports, pinned by SHA-256. What it cannot see -- a computed path, host or agent name, the effects inside a polyglot block -- it lists as a note. A person reviews the draft and signs it with a trust-store key (`naab-gov manifest sign`); `naab-gov manifest check PROGRAM` says whether a run would accept it. With `manifest.required` a run needs that manifest, signed by a trusted Ed25519 key (no unsigned mode, no legacy HMAC), for exactly the program's name and text, or it is refused (exit 3) before anything runs; the run is then narrowed to it like a lease, on every load and reload: an action, language, host, file, agent or module it does not list is refused (HARD), the program cannot read or rewrite the manifest, and an `extends` parent's requirement holds in the child. `--lint-only` is not asked for one. Polyglot code is bounded by the sandbox only: a subprocess language runs where the listed files and hosts do not reach (CONTRA-013), and without `SHELL_EXEC` it is refused.
- **`contracts.<fn>.requires` / `ensures`: preconditions and postconditions written as NAAb expressions (Bundle 11).** `"requires": ["n >= 0", "len(items) > 0"]` must hold for the arguments when the function is entered (after its defaults); `"ensures": ["result >= 0"]` must hold for the returned value, named `result`, and the arguments as they were on entry. The VM checks both at every call -- direct calls, callbacks, `async fn` -- and a clause that is false, not a boolean, or cannot be evaluated is enforced at the contract's level (else `contracts.level`), like any other contract breach. A clause may read the function's inputs and nothing else: operators, indexing, member access, lists, dicts and `len()`/`type()`/`typeof()`. A clause that calls a function, assigns, interpolates a string or runs a polyglot block refuses the config (exit 4), as does a name that is not a parameter; at run time a clause still cannot call a program-defined `len` or read a member of a module a parameter holds. Checked by the default engine only; `--tree-walk` (audit/off) evaluates none and prints a note saying so. A mid-run reload cannot add, edit or remove a clause (`contracts.functions` is fixed for the run).
- **`naab-gov contracts FILE`: a counterexample search for those clauses (Bundle 11).** For each function FILE declares with a clause, it generates inputs from `--seed` (the simplest first, the clauses' own numbers and their neighbours included), sets aside the ones that do not meet `requires`, and reports the first that meets them and still breaks a clause -- the function's `ensures`, or a clause of a function it calls. Opt-in, and separate from `naab-gov check`, because it RUNS the functions: in a `naab-lang` worker, under the project's govern.json, with `main` not run. Exit 0 none found, 1 a counterexample, 2 UNMEASURABLE (nothing was tested, or the worker did not report in full), 3 a governance block, 4 a config or usage error. POSIX only, like the other worker-based commands.
- **A behavioural-sequence pattern lint, and a bounded state-machine explorer (Bundle 10).** The config loader now reports a pattern that can never fire (a step with no matcher) and a pattern subsumed by another (identical steps, an equal-or-wider window at an equal-or-stricter level) -- report-only, and sound by construction, so no pattern that fires is ever called redundant. `tools/governance_explore/explore.py` (report-only) drives the real engine over every bounded ordering of propose / commit / sibling-send / proposer-send and checks that no commit lands at CRITICAL and that the governance level never de-escalates; `test_governance_statemachine.sh` is its depth-3 CI smoke.
- **`naab-gov ratchet OLD NEW`: would a mid-run reload of OLD by NEW be accepted?** Exit 0 accepted, 1 refused (each violation named; a NEW whose signature does not verify is refused too, unless `--ratchet-only`), 4 a config error. It asks the engine's own reload decision (`evaluateReload()`, which the reload itself now calls), after loading OLD as a run would, with `--env`, `--agent-id` and `--lease-grant` laid over it as at startup (with no `--env`, `governance.default_env` applies, as in a run). `--json` prints the decision.
- **A property check of "governance only tightens"** (`naab_ratchet_property`, Bundle 8): generated config pairs through the real reload, with random startup overlays, judged by the engine's own gates; also that an agent role and a capability lease only narrow, and that `--sandbox-level`, `--timeout` and `--memory-limit` never raise what `govern.json` configures. It runs in the suite and, at a larger budget, in CI. Its first runs found the holes below.
- **Two fuzzers and a ThreadSanitizer CI job (Bundle 9).** `tests/fuzzing/model_channel` sends generated model replies and tool calls through the agent stub -- empty, huge, malformed, odd encodings, broken tool pairing -- and requires every run to end in exit 0, 3 or 4 with its evidence written and its telemetry chain intact. `tests/fuzzing/config` mutates a restrictive govern.json and requires it to load or exit 4, never crash, and never load weaker than written, judged by probes that try what it forbids. The `ThreadSanitizer (concurrency suites)` job (`tests/tsan/run_tsan.sh`) runs the async, agent-batch and polyglot-concurrency suites under TSan and fails on any report its suppressions file does not give a reason for. `tests/tsan/schedule_perturb.sh` (report-only) reruns those suites pinned to one and two CPUs; its first run found that the test stub logged a request after replying to it, so a suite could read its key log out of order (A112, fixed in the stub).

### Changed -- messages and records that match what happened (Bundle 18: A171-A174, A39(c); migration note)
- **A refusal records, and names, the line it happened on (A171).** A check made while the program ran took its file and line from a note that the output-contract check (run at every function return, contract or not) and the polyglot-block check set and never put back, while the runtime gates -- filesystem, environment, network, a taint sink -- set none. A refused `file.read` on line 9 was recorded in the JSON report, SARIF and telemetry at the line of the last `return` or polyglot block, or 0; its message printed no `At:` line at all (`At: in function 'get'` inside a function). Now every check with no explicit location is located at the running program's current instruction, every explicit location is scoped (the unscoped setter is gone), and the refusal message says `At: line 9` (`At: line 4, in function 'get'`). The tree-walker (audit and off only) locates its standard-library calls the same way.
- **With `telemetry.deduplicate_checks` on, two refusals are no longer merged into one row** because a function returned before them (A171): a refusal located by the running program is a runtime event, which de-duplication never collapses.
- **Every check record says what happened to the action: `disposition` (A172).** Telemetry rows, the JSON report's results and SARIF result properties carry `passed`, `refused`, `warned`, `overridden`, `approved` or `audit_only`; the JSON summary counts them (`summary.dispositions`). Telemetry's `result` is unchanged -- still `block` for any failed check, including one that warned, was overridden, or ran under audit -- so existing readers keep working.
- **The summary and the closing line count what happened (A39(c)).** `[governance] Summary` reads `P passed, W warned, R refused`, adding `overridden`, `approved` or `audit-only` counts when there are any, and its rows say `WARN`, `OVERRIDDEN`, `AUDIT ONLY` or `REFUSED` (was `BLOCKED`). The closing line keeps `-- Governance: PASS` and states what the run let through and the after-run audit's result: `-- Governance: PASS (2 warnings; after-run audit: 0 findings) --` (was `(N static, M runtime, 0 violations)`, where "static" counted the runtime gates too and "0 violations" counted only the audit -- after two overridden refusals the run printed "2 blocked" and then "PASS ... 0 violations"). The `--governance-dashboard` counts use the same words.
- **A refused run ends with its summary and `-- Governance: REFUSED (rule) --` (A174)**, on stderr, after the `Error:` on stdout; it used to stop at the error. Exit codes are unchanged.
- **The user guide says where messages go (A173):** a refusal, like every error that stops a program, prints on stdout as `Error: ...`; warnings and the closing lines go to stderr.
  - **Migration:** a script that reads the closing lines must match the new wording: `warned`/`refused` (not `warnings`/`blocked`) in the summary, `after-run audit: 0 findings` (not `0 violations`) in the PASS line, and `-- Governance: REFUSED (` on a refused run. A report reader that counted failed results by level can read `disposition` to tell a refusal from an override or an audit-only record.

### Changed -- twelve settings that loaded and did nothing now work (settings-liveness step 3, PR 2; migration note)
A config that already sets one of these gets its behaviour from this release. Each was parsed, shown in the template and read by nothing (A175).
- **`limits.code.max_total_polyglot_lines`**: the program's polyglot code may total at most this many lines, each distinct block counted once (a block in a loop is one block). The block that crosses it is refused (HARD, exit 3).
- **`limits.timeout.total_polyglot`** (seconds): the time all polyglot blocks of a run may spend together. The block that would cross it is stopped when the time left runs out and refused (HARD); a block started with none left is refused before it runs. Audit mode reports and stops nothing.
- **`limits.memory.total_mb`** (and the bare `"limits": {"memory": N}`): caps the run's memory budget, which child processes get. It lowers the budget `runtime.memory_limit` (default 512) sets and never raises it. Like `runtime.memory_limit`, it is applied when the run starts: a mid-run reload may lower it (raising it is refused), and the lower value applies from the next run.
- **`restrictions.dangerous_calls.blocklist_extra`**: names the project adds to the dangerous calls, matched as literal text without case, like `banned_functions`, at the dangerous-calls level.
- **The import allowlist** (`restrictions.imports` with `"mode": "allowlist"` and `"allowed": {"python": [...]}`, or `languages.per_language.<lang>.imports` with `"mode": "allowlist"` and `"allowed": [...]`): every import a block of that language writes must be listed (an entry admits the modules nested under it: `os` admits `os.path`); an import whose name the block computes is refused, because it cannot be checked. The blocked lists still apply. Python, JavaScript, TypeScript, Go and Ruby; an allowlist for another language, the `"any"` key, a restrictions allowlist naming no language, or a mode other than `"blocklist"`/`"allowlist"` refuses the config (exit 4).
- **`agents.<name>.tool_timeout_seconds`** (default 10): a tool call that runs past it is stopped and refused (HARD, exit 3). This applies to every agent with tools, including one that does not set the key: a tool that needs longer must say so. Audit mode reports and stops nothing.
- **`agent_dispatch.max_retries_per_run`**: once the run has used that many retries, a failing provider call returns its error instead of trying again (send, tool loop and `agent.propose()`).
- **`approval.default_expiry_hours`**: the lifetime `naab-lang --approve` grants. `--expiry` may shorten it and no longer extend it; a config set to 0 grants an approval that has already expired. A govern.json that does not load now refuses the approval (exit 4).
- **`trust.require_fresh_signature`**: a verified signature that carries no signing time -- a legacy HMAC signature, or an Ed25519 one made before signing times were recorded -- is refused like one that does not verify (exit 3), whatever `stale_signature_level` says. Re-sign with `--sign-governance`.
- A mid-run reload may not loosen the new lists: removing a `blocklist_extra` entry, or admitting an import the running allowlist refuses, is a ratchet violation.

### Changed -- settings that enforce nothing leave the template and `naab-lang init` (settings-liveness step 3; migration note)
- **25 settings that look like protection and enforce nothing now draw a load warning**, and neither template copy nor `naab-lang init` writes them any more (the owner's decision, 2026-11-16). They are: `limits.memory.per_block_mb`, `limits.execution.total_executions`, `limits.data.input_size`, the `restrictions.code_injection` LDAP, XPath and template-injection switches (no check detects any of them), four `polyglot.parallel` / `persistent_runtime` limits plus `persistent_runtime.max_memory_per_session_mb`, `polyglot.variable_binding.max_bound_variables`, four `polyglot.output` options, three `restrictions.polyglot_output` options, `audit.log_events.checks_passed` / `checks_failed` (the audit trail records no check result; the telemetry file records each one), `audit.provenance.record_decisions` / `record_proof_objects`, and `api.tls_cert` / `tls_key`. The REST server has no TLS, and the template used to say "set tls_cert and tls_key for HTTPS". Where a working setting does the job, the warning names it. The settings are still parsed, so the reload ratchet and `extends` treat them as before, and nothing that ran before is refused. **Migration:** a config that sets one prints `"<setting>" is parsed but not enforced -- no check reads it.` Remove the setting, or use the setting the warning names. If you set `api.tls_cert` or `tls_key`, your API was never served over TLS: put it behind a TLS-terminating proxy.
- **45 settings that guard nothing** (output formatting, loader options, `polyglot_optimization` and check sub-options, `limits.rate.cooldown_on_limit_ms`, the `requirements.naming_conventions` block, and switches that fail closed, such as `code_quality.no_secrets.allowlist` and `trust.check_revocation`) are out of the template and init too. A config that sets one still loads without a warning.
- The nine settings with a clear meaning that were left in the template (`limits.memory.total_mb`, `limits.timeout.total_polyglot`, `limits.code.max_total_polyglot_lines`, the import allowlists, `restrictions.dangerous_calls.blocklist_extra`, `agents.<name>.tool_timeout_seconds`, `trust.require_fresh_signature`, `approval.default_expiry_hours`, `agent_dispatch.max_retries_per_run`) work in this release: see "twelve settings that loaded and did nothing now work" above. Test: `tests/governance_v4/test_no_effect_settings.sh`.

### Changed -- what the engine's own model calls wrote is scanned (A166, A169; migration note)
- **The governance voice's guide and everything agent review's models write are scanned before they are shown, as every other model output is.** The voice's guide was scanned for secrets only when the config turned `no_secrets` on; agent review's texts -- each finding's category and description, the rejected findings' hints, its own voice guide -- were never scanned, and were printed, passed into every report and written to `.naab_cache`. Each of these models is shown the whole script, so a key the script holds could come back in any of them. Now each is withheld when it matches the secret patterns (unless the config sets `"no_secrets": false`) or, under `no_pii`, the PII patterns, and the run says so. A withheld finding still counts exactly as before: the review's zone, score and level are worked out first. **Migration:** a project that wants these texts printed unscanned sets `"no_secrets": false`, which also switches off the scan of every agent reply.
- **A governed program may no longer write `.naab_cache/`**, the directory beside govern.json where agent review and the governance voice keep the results they read back on the next run (A169). Creating it, writing, copying or deleting a file in it, and writing through a link into it are refused (exit 3); reading it is still allowed. The cache's own integrity check is keyed from govern.json and the script, which a program can know, so a program that could write there could plant a review that found nothing. **Migration:** a program that kept its own files in `.naab_cache/` must use another directory.

### Removed -- test programs nothing ran (A58)
- Nine standalone test programs in the default build target, and the `naab_testing` library, are gone: four were placeholders that only returned success, four tested files or syntax that no longer exist, and one could not fail. The profiler, block registry and block search index tests that still had a subject are GoogleTest cases in `naab_unit_tests` now, which CI runs. Moving the search test found **A170**: `naab-lang blocks search <text>` has never found anything (recorded, not changed).

### Changed -- refusals are governance blocks, and three limits enforce (A88, A168, A2; migration note)
- **A refused `codegen.run()` is a governance block, not an error the program can catch.** Every codegen rule (`codegen.enabled`, the language lists, `max_code_size_bytes`, `max_code_lines`, the cumulative caps, nesting depth, the tainted-code rule, the agent action) is now decided by the engine at `codegen.level` (default `hard`): the refusal is recorded, and at `hard` or `soft` the run ends with exit 3 whatever the program's `try/catch` does. Before, each was a plain error: a `try/catch` swallowed it, the run ended "Governance: PASS" with nothing recorded, and `codegen.level` did nothing. At every level the refused code still never runs. **Migration:** a program that catches a codegen refusal and carries on now stops; set `codegen.level` to `"detect"` to keep the refusal catchable, or `"advisory"` to record it as a warning. A `codegen.run_strict()` failure of code that did run is unchanged.
- **`agent.create()` past `exposure_tracking.max_unique_agents` goes through the engine too**, at `exposure_tracking.level` (default `advisory`: a recorded warning; `hard` ends the run with exit 3). The agent is not created at any level.
- **`limits.timeout.per_block`, `languages.per_language.<lang>.timeout` and `limits.code.max_lines_per_block` now enforce.** Each was read by code nothing called, so a block ran as long and as large as it liked under any of them. A polyglot block (inline, `.exec()`, or `codegen.run()`) now runs under the language's own `timeout`, else `limits.timeout.per_block`, in seconds: it is interrupted at the limit and the overrun is a HARD block (exit 3). A block with more lines than the language's own `max_lines`, else `limits.code.max_lines_per_block`, is a HARD block before it runs. In audit mode both are reported and nothing is stopped. **Migration:** a config that carried these keys without effect now applies them -- `naab-lang init` writes `per_block: 30` and a line limit, and the template lists per-language timeouts of 10 to 60 seconds; raise or remove a value that is too small for your blocks (0 or absent means no limit).
- **A shell, node or ruby block is interrupted at the run's deadline.** Their executor waited on its own 30-second bound and never asked for the deadline around it, so a block ran to its end under `--timeout` or a block limit and was only refused afterwards.

### Changed -- two block checks read more (A148; migration note)
- **`code_quality.no_secrets` reads a credential held in a C macro.** `#define DB_PASSWORD "..."` passed, because every assignment pattern wanted `name = "value"`. A macro is now flagged when its name ends in a credential word (PASSWORD, PASSWD, SECRET, TOKEN, API_KEY, PRIVATE_KEY, ACCESS_KEY, SECRET_KEY, CRYPTO_KEY, ENCRYPTION_KEY, SIGNING_KEY) and its value is one string literal of 8 or more characters with no whitespace.
- **`restrictions.crypto` reads the Windows CryptoAPI constants.** `CALG_MD5` passed, because the check looks for `md5` as a whole word. The named constants are now flagged: `CALG_MD2`, `CALG_MD4`, `CALG_MD5`, `CALG_SHA`, `CALG_SHA1`, `CALG_DES`, `CALG_3DES`, `CALG_3DES_112`, `CALG_DESX`, `CALG_RC2`, `CALG_RC4`, `CALG_RC5`. A config that lists its own `weak_hashes` or `weak_ciphers` gets a constant only for an algorithm its list names (`des` covers the 3DES and DESX constants). `rand()` is not flagged.
  - **Migration:** both are tightenings. A C or C++ block (or, for secrets, any text the check reads: the program, a prompt, a tool result, a model's reply) that holds such a macro or constant is now refused at the check's level where the check is on. Measured before shipping: neither fires on any of this repository's 1,698 polyglot blocks. To keep a block that trips the macro rule, read the value from the environment instead of defining it; to keep one that trips a constant, use a stronger algorithm, or list the algorithms you do want refused in `weak_hashes` / `weak_ciphers`.
- **`code_quality.no_path_traversal` is unchanged, and its reach is now written down** in the template: it reads a literal `../` outside string literals only. What keeps a program inside its directory is `capabilities.filesystem`.

### Fixed
- **The telemetry chain of an `agent.batch` run verifies (A164).** Each event's `prev_hash` is read from the telemetry file's tail, and a writer released the file lock before its sealed line was flushed, so the next writer -- another batch thread, or another process sharing the file -- chained onto the same predecessor. `--verify-telemetry-chain` then reported BREAK on a file nobody had edited (7 of 7 runs of one batch test). A sealed line is now written and flushed before either lock is released, and the webhook receives a batch run's events in the file's order.
- **On Windows, processes sharing one telemetry or transcript file take turns (A165).** There was no file lock there at all (`flock` is POSIX), so two processes could seal events onto the same predecessor and an untouched file failed `--verify-telemetry-chain`. The writers now take a `LockFileEx` lock on Windows, as they take `flock` elsewhere.
- **`--verify-telemetry-chain` is bounded, and a file it cannot read does not pass (A163).** A line over 1 MiB or nested deeper than 64 levels is reported `NOT READ` and fails the verification (a deeply nested line used to end it with a segmentation fault). A file whose lines are all unreadable exits 1; it used to print `CORRUPT` for each and exit 0.
  - **Migration:** a script that verifies telemetry holding a line over 1 MiB now gets exit 1 with `NOT READ` for that line.
- **`no_apologetic_language` matches words, not pieces of words (A149, the owner's decision of 2026-11-11).** Its patterns had no word boundary, so `oops` matched inside `Loops` and the leading `I` inside `API`: a block whose comment read `# Loops over the items` was refused. Each pattern now starts and ends at a word boundary; `whoops` is matched as its own word, so every apology flagged before is still flagged.
  - **Migration:** none needed. A block or file that was refused only for a word containing `oops`, or for `API didn't check` and the like, now passes this check.
- **`naab-gov lint` says which findings a run does not enforce (A132).** Lint applies 15 checks to the whole file that a run applies inside polyglot blocks only. A finding of one of them is now followed by a note naming the rules. No verdict or exit code changes.
- **`--verify-telemetry-chain` no longer aborts on a `hash` field that is not a string (A163).** A telemetry line such as `{"hash": 1, ...}` ended the verifier with an uncaught exception; it is now reported `CORRUPT`, exit 1, like a line that is not JSON.
- **A host entry holding `*` refuses the config (A147, the owner's decision of 2026-11-11).** `capabilities.network.allowed_hosts` and `blocked_hosts` entries are names, not patterns: an entry covers that host and every sub-domain of it. An entry written `*.example.com` was compared with its star, which no host name contains, so it matched nothing -- in `blocked_hosts` it blocked nothing, silently. A config with a `*` in a host entry now exits 4 and the message names the plain form.
  - **Migration:** write `example.com` where the config has `*.example.com` (it covers `example.com` and its sub-domains).
- **TypeScript blocks run on node 24 under a memory budget (A146).** Under any memory budget the executor started `tsx` with `--jitless --no-expose-wasm`; node 24 has no `--expose-wasm` flag and refuses to start ("bad option: --no-expose-wasm"), so every `<<typescript>>` block failed there. It now passes `--jitless` alone and removes the one warning node 22 prints about it from the block's stderr.
- **A child process that runs node 24 needs a larger memory budget than the default (A145, the owner's decision of 2026-11-10).** Node 24 maps its ~512 MB JIT code range writable, and the child's data limit counts that, so under the default 512 MB budget `process.run("node", ...)` (and a TypeScript block, whose `tsx` launcher is node) dies with "Fatal process out of memory: Failed to reserve virtual memory for CodeRange", and a `runtime_versions` pin for typescript cannot read tsx's version and fails closed. Node 22 was not affected. The default stays 512 MB. A pin whose version check fails now says so and quotes the runtime ("its version check did not complete (it ended abnormally: Fatal process out of memory: ...)"); it used to say the pin "can never be satisfied as written".
  - **Migration:** a project that runs node 24 in a child process sets `"runtime": {"memory_limit": 600}` (or more) in govern.json.
- **An environment can only turn a gate on (A130, Bundle 17, the owner's decision of 2026-11-09).** `--env NAME` (or `governance.default_env`) applied `quality_gate.enabled`, `governance_baseline.enabled` and `governance_baseline.fail_on_regression` as written, so an environment's `"false"` switched off a gate the file turned on: a run the quality gate failed (exit 2) passed under `--env dev`, exit 0, and the template's own `development` environment did exactly that. An environment's `"true"` still turns each gate on; any other value over a gate the file turned on is ignored, with a warning naming the environment and the key. The template's `development` environment is removed.
  - **Migration:** a project that switched a gate off for one environment now keeps it on there. Leave the gate off in the file and turn it on in the environments that need it (`"quality_gate.enabled": "true"`).
- **Custom rules apply to NAAb code when it runs, and none is silently dropped (A131, Bundle 17).** A `custom_rules` entry for `"naab"` (or for every language) was applied by `naab-gov lint` and to polyglot blocks, never to the program at `naab-lang run`: the same file was refused by lint and ran under "Governance: PASS". The run now checks the entry file and every imported module, on both engines, and names the matched line. It reads the NAAb code only: a polyglot block's body is checked in the block's own language, as before, so a match there counts once. A pattern the old ReDoS heuristic called unsafe used to be skipped with a warning; every custom rule now compiles with the linear-time matcher, which refuses (scan limit, HARD) rather than hangs on a pattern it cannot run in linear time, so the rule applies. A pattern that does not compile refuses the config (exit 4, the rule named) instead of loading without it.
  - **Migration:** a project whose custom rule matched its own NAAb code now meets it at the rule's level when the program runs. Fix a rule that does not compile, or the project no longer loads.
- **A rate-limit refusal is a HARD block (A133, Bundle 17, the owner's decision of 2026-11-09).** `limits.rate.max_stdlib_calls_per_second`, `max_file_ops_per_second` and `max_polyglot_per_second` refused a call with a catchable runtime error that recorded nothing: 35 refusals caught in a `try` ended "Governance: PASS", exit 0. They now go through `enforce()` at HARD -- recorded, uncatchable, exit 3 -- like the other limits, on both engines and in `codegen.run()`.
  - **Migration:** a program that caught a rate-limit refusal and carried on now stops. Raise the limit if the rate is intended.
- **Execution and derivation contracts judge what they name (A134, Bundle 17, the owner's decisions of 2026-11-09).** A `must_produce`/`must_vary`/... contract for a function the program neither defines nor imports blocked the run (HARD, after it had finished), so one project-wide contract list blocked every other program in the project; such a contract is now skipped and named ("Execution contracts not run: ..."). A missing argument is padded by the parameter's declared type in the contract's `params` (an untyped one stays an empty list). And `must_derive_from` passed when any one listed parameter appeared anywhere in the body, comments and strings included; now every listed parameter must appear in the function's code -- a comment does not count, and a string counts only through what it interpolates.
  - **Migration:** a function whose `must_derive_from` lists several parameters and uses only some, or names them only in a comment or a string, is now refused at the contract's level. A contracted function renamed in the source is no longer caught by the program that lost it (name-keyed contracts never saw a rename). `must_derive_from` checks that each parameter appears in the function's code, not that it flows into `return_key` (A144).
- **A taint sink can name a NAAb function (A141, Bundle 17, the owner's decision of 2026-11-09).** `taint_tracking.sinks` matched only the names the engine checks itself (a stdlib call, `<lang>_exec`, `print`, variable assignment), so a sink naming a program's own function -- `post`, or `net.post` from an imported module -- loaded without a word and did nothing: a tainted value reached it under `level: hard`, exit 0. A call into a NAAb function is now checked under the name it is written by (and a struct method called on an instance under its type's name), at the sink's level, and `naab-gov taint` reports the flow.
  - **Migration:** a project whose sinks list names one of its own functions now meets `taint_tracking` there. Sinks match by prefix, as they always have: an entry such as `post` also names `post_message`.
- **`naab-gov contracts` keeps a typed parameter's cases in its type (A142, Bundle 17).** The random half put integers in any parameter, so a `string` parameter was tried with 8 strings and then integers that threw before reaching the code; "no counterexample found" rested on 8 cases. The type comes from the contract's `params` hint, else the program's own annotation (`fn f(s: string)`). An untyped parameter is searched as before, with the same cases for the same seed.
- **A reload resolves `extends` (A135, Bundle 17).** A mid-run reload, and `naab-gov ratchet`, read an `extends` child alone, so every inherited setting looked removed and a project using `extends` could not reload at all -- even a reload of the file by itself was refused. The reload now merges the chain as startup does, with the parent floor applied, before the ratchet compares. A chain that does not load (a parent missing, unsigned or refused) refuses the reload; the run continues under the configuration it has, and `naab-gov ratchet` exits 1.
- **`naab-verify-audit` verifies the audit trail the engine writes (A136, Bundle 17).** It was written for an older logger's format and reported every untouched engine trail as TAMPERED, exit 2. It now checks each entry's hash against its content and each `prev_hash` against the entry before: VALID exit 0, TAMPERED exit 2 with the line named, and NOT VERIFIABLE exit 3 for a trail with no chain (`audit.tamper_evidence` off), never VALID. Entries cut from the end cannot be detected from the file alone, and the tool says so. The older format still verifies as before.
- **`export fn` is a function to `@intent` and `naab-gov contracts` (A137, Bundle 17).** A `/// @intent` above `export fn` was dropped, so intent validation asked the function for the intent it had; and `naab-gov contracts` could not find an exported contracted function ("not declared in this file", exit 2). Run-time `requires`/`ensures` always applied to it.
- **CONTRA-013 fires only while a separate-process language can run (A138, Bundle 17).** It said path rules do not reach subprocess languages whenever a path rule was set below `restricted`, including with shell disabled and with every subprocess language blocked -- its own remedy did not silence it. It now asks the gates: shell disabled refuses every separate-process language; otherwise the language lists decide. In-process: JavaScript, SQL, and Python where the build embeds it.
- **The template and `naab-gov init` no longer write settings that do nothing (A139, Bundle 17).** The template loaded with 14 config warnings: keys nothing enforces (`capabilities.process`, `filesystem.blocked_extensions`, `limits.data.string_length`/`nesting_depth`, `requirements.error_handling`), `"enabled": false` on four checks that a block's presence switches on, a sequence pattern whose steps matched no event (so enabling detection from the template replaced the built-in patterns with one that never fired), an `orchestra` section nothing reads, and REST key scopes the server does not know. It now loads with none. A top-level `scopes` block (per-file exemptions) was written by the template and by `naab-gov init` and read by nothing, so a project believed files exempt while every check applied; both stop writing it, and the loader warns when a config has one. It is not built: a per-file switch-off would be a way out of a check.
  - **Migration:** none needed for enforcement -- nothing removed was applied. Delete `scopes` from a config to silence the warning.
- **Under `extends`, a child can no longer loosen its parent's security settings, under either merge strategy (A98, Bundle 8, the owner's decision of 2026-11-06).** The merge let a child allow what its parent refused: under `child_wins` its own value replaced the parent's, and under `parent_wins` its own non-empty list still replaced or widened the parent's -- environment reads and writes, child-process withholding, file reads and writes, languages, hosts and the response scans (measured over generated parent/child pairs; only the mode was a floor). Now, after the merge, a switch is allowed only when both configs allow it, a blocked list is the union of both, an allow list keeps only what both admit, and every check is on when either turns it on, at the stricter level. The same holds for the sandbox level (the merge never read it: a parent's `restricted` reached no child), `integrity.blocked_flags`, codegen (enabled only when both enable it) and the per-language banned lists. A child can still tighten, and still set what is not a security setting (limits, outputs, telemetry, agents).
  - **Migration:** a child that relied on overriding its parent's restriction now runs under the parent's. Two allow lists that admit nothing in common refuse the config at load (exit 4, the key named): narrow the child's list to entries the parent allows. A child's `allowed_paths` entry inside a parent's `blocked_paths` entry no longer opens it. A child no longer enables codegen its parent does not; enable it in the parent too.
- **An environment can only raise the mode, never lower it (A95(b), Bundle 8, the owner's decision of 2026-11-06).** `--env NAME` (or `governance.default_env`) used to replace govern.json's mode whatever the direction: an environment at `audit` over an enforce file ran an audit run that read what the file refused, and let `--tree-walk` run; one at `off` over an audit file blocked the run instead. A lower mode is now ignored, with a warning naming the environment.
  - **Migration:** a project whose environment sets a mode below its file's, such as the template's former `development: {"mode": "audit"}` over `"mode": "enforce"`, now runs at the file's mode. To audit in development, set the file's mode to `audit` and raise it in the stricter environments.
- **The VM no longer flags a clean value for the name of a tainted one (A121(a), (b), Bundle 12).** The compiler kept one flat map of tainted names, so a catch binding, a match binding, a loop variable, a parameter, a destructured name or an inner `let` read the state of the outer variable of that name: `let e = env.get("HOME")` made `catch (e) { env.set_var("K", string(e)) }` a violation whatever was thrown, and a function's local reached another function's parameter. A declaration now starts its own entry and the outer one comes back when it leaves scope; the shared taint rule does the same for the names a catch or match arm binds, so the tree-walker and `naab-gov taint` (which over-reported a try expression's catch binding, a match binding and a list pattern) changed with it. Separately, the VM's check of a variable bound into a polyglot block also looked the variable's NAME up in a run-wide set, and wrote it there whenever it flagged one, so after one refused and caught `<<shell[clean]>>` every later variable named `clean` was flagged; it now judges each bound value by its own taint. Values that are tainted are still refused, under both taint origins (the table rows say so). A nested function that returns a variable reassigned clean before the call is still flagged: an accepted over-report, pinned (A121(c)).
  - **Migration:** none needed; fewer false refusals. A program that caught such a refusal at `detect` level now takes the success path.
- **Array callbacks work inside an `async fn` (A124, Bundle 12).** `array.map_fn`, `filter_fn`, `reduce_fn` and `find` stopped the async fn with "requires function evaluator", and `array.sort` / `sorted` with a comparator returned the default order. The async VM now has the evaluator, with the callback-argument taint `main{}` has, and `sort`/`sorted` refuse a comparator they cannot call instead of ignoring it.
  - **Migration:** a program that relied on `array.sort(xs, cmp)` inside an `async fn` returning the default order now gets the comparator's order.
- **An `async fn` that returns a variable it captured keeps the variable's taint (A129, Bundle 12).** The async call copied each captured variable for its thread without the taint the run kept for it while the variable was still live, so a value only the run knew was tainted (a callback's result, for instance) came back clean from `await`. Under `taint_tracking` it now meets the sink check as it does returned from a nested function.
  - **Migration:** a program that returns tainted data from an `async fn` through a captured variable and sends the result to a sink now meets `taint_tracking` at its configured level.
- **A tainted `print` inside a `match` arm names its own line (A121(d)).** The VM's `print` check read the next instruction's line, which inside a match arm is the match's.
- **The cross-block gate judges every bound variable (A127, Bundle 12).** With `taint_tracking.gate_cross_block`, a block bound `[clean, tainted]` after an earlier block produced output passed, while `[tainted, clean]` was refused: the flow was judged by its first variable only. A flow is now unsanitized when any bound value is tainted.
  - **Migration:** a program under `gate_cross_block` that binds tainted data after a clean variable into a later block is now refused at `cross_block_level`.
- **The `path` module returns `/` as its separator on every host (Bundle 16, the owner's decision of 2026-11-07).** On Windows it returned `std::filesystem`'s native form, which mixed separators: `path.join("/usr", "local", "bin")` was `/usr\local\bin`, and `path.normalize("/foo/../bar")` was `\bar` (observed on the Windows runner), so one program built paths that read differently on every host. `join`, `dirname`, `basename`, `extension`, `normalize` and `resolve` now return the generic form (`D:/a/b` on Windows); on Linux and macOS nothing changes. What a path means stays the host's: `path.is_absolute("/usr/bin")` is still false on Windows, which needs a drive. `tests/stdlib/test_stdlib_path.naab` had failed on Windows since it was written; the runner counted it as a missing executor until A123's guard stopped excusing every Windows failure.
  - **Migration:** a Windows program that compared a path result with a backslash string, or handed one to a tool that reads `/` as an option, now gets forward slashes.
- **The engine's own commands no longer read the program's stdin, and prerequisites run only where a program runs (A122, Bundle 16).** A `command` prerequisite was started with the run's stdin, so one naming an interpreter read -- and ran -- whatever was piped to the program, which then read nothing; a hook inherited it the same way. Prerequisite checks, hooks and the runtime-version probe now start with the null device as stdin (POSIX and Windows); a child the program starts (`process.run`, polyglot blocks) still gets the program's stdin. And every govern.json load ran the prerequisites, so `naab-gov lint`, `taint`, `ratchet`, `manifest check` and `check`, `naab-lang --lint-only`, the signing commands, the C API and the REST/MCP server processes all started the commands a project names. Only `naab-lang run` (and its workers) and `naab-lang --attest` run them now; the others say they did not. `--attest` ran each check twice and now runs it once.
  - **Migration:** a prerequisite command that read stdin now reads end of input. A static review no longer reports a failing prerequisite; run the program or `naab-lang --attest` for that. `naab-gov lint` still fires an `on_violation` hook on a block it finds (A126, undecided).
- **A JavaScript block's value is its last expression whatever precedes it (A125, Bundle 16).** `function f(n) { return n * 2; }` then `f(21)` evaluated to null on both engines, because the executor read the declaration and the call as one statement. And a block whose last statement was a declaration (`const b = [...]`) never ran: it was wrapped as `return (const b = ...)`, a SyntaxError. Both now work; a block ending in a declaration or a loop has the value null.
- **Embedded Python's allowlist scrub withholds `LD_LIBRARY_PATH`, as every other child's does (A99, Bundle 16).** Its list of essential variables was a copy that kept it; it is now generated from the one list.
- **A `.naab` test that prints a failure and exits 0 now fails the suite (A123, Bundle 16).** Thirteen tests did, mostly because they expected a number from a block that returns its output as a string (a coercion removed on purpose in May); they now compare what their executors return and fail their run when an assertion fails. `run-all-tests.sh` now also reads the output of every passing `.naab` test, and one that reports a failure counts as failed; one test, pinned with its record, waits on A121.
- **Two taint flows the VM did not check (A120, Bundle 12).** A callback a standard-library or built-in method call runs (`array.map_fn`, `filter_fn`, `reduce_fn`, `.find`, `.for_each`, `.sorted` with a comparator) now gets the call's data taint on every parameter: `array.map_fn([t], fn(v) { print(v) })` printed a tainted value unflagged. And a struct field store now taints the struct, as an index store taints a dict: `s.a = t` then `print(s)` passed. Both were found by `naab-gov taint`'s cross-check with the run. The tree-walker taints no parameter at all and is unchanged (it runs only under audit and off).
  - **Migration:** a program that hands tainted data to a callback, or stores it in a struct field, and sends the result to a sink, now meets `taint_tracking` at its configured level. One tainted element taints every element's callback.
- **A runtime error inside an expression now names its line on the VM (A121(d)).** The parser built every operator, literal, member access, subscript, list and dict with no location, so an error raised there said `x.naab:0`; and a check at a standard-library call read the line of an argument three instructions back. `env.set_var("K", t ?? "x")` was refused at line 0. Each node now takes the line of its operator or opening token, and the VM reads the call's own instruction.
- **Two taint tests printed their failures and passed (A123).** `test_taint_aliasing.naab` and `test_subscript_taint.naab` printed `FAIL` for A120's struct case and exited 0, so the suite counted them as passing; they now fail the run on a failed assertion. Fourteen more did the same; thirteen are fixed (Bundle 16, below) and one waits on A121.
- **A `match` or `catch` inside a larger expression now reads its own value on the VM (A118, Bundle 12).** A match subject or binding, or a catch binding, took a stack slot counted from the declared locals only, so when the expression around it already held something -- a function being called, a left operand, earlier list items, a method's receiver -- it read or overwrote that instead. `"p" + match s { "x" => "a", _ => "b" }` gave `bx`, `print(try { throw "e" } catch (e) { e })` printed `__builtin__:print`, and `print(match ...)` failed with "Value is not callable". The tree-walker was right in every case. Measured on the code before this change: 106 of 180 cases (each operand position crossed with each construct) gave a wrong value or failed; now none do.
- **Taint now means the same thing on both engines, and a table pins it (A117, Bundle 12).** The tree-walker and the VM compiler each walked the AST with their own rule, and both ended in "anything not listed is clean". They now share one rule, in which an expression it does not name is tainted. The VM's runtime taint, which carries a value no static walk can see (a callback's result, say), now follows the same rule. Measured on the code before this change, 52 of 156 checks changed verdict (78 cases, each with the tainted value straight from a source and from a callback). A value now carries taint through:
  - a match subject or guard, a computed dict key, and a condition in a sink's argument, on both engines;
  - an if-condition, a JavaScript block's result, a value thrown into a `catch`, a taint source written straight into a sink's argument, and a value a generator yields from a source, on the tree-walker;
  - any callback a standard-library call ran, not only the last one (`array.map_fn` with one tainted callback result and a clean one after it read clean), on both engines;
  - on the VM, for a value only the runtime knew was tainted: string interpolation, a dict key, if and match conditions, an `async fn`'s arguments and result, a generator's arguments and yields, and any call's result when an argument was tainted, as the static rule always judged it (`k(t)` read clean when `k` ignored its argument).

  The VM also stopped flagging clean values: after any taint source, the next standard-library call's result was tainted, however many statements came between. And on the tree-walker, a sanitizer written straight into a sink's argument now clears taint, as its assigned form already did on both engines. `tests/governance_v4/taint_semantics.psv` is the table, one row per expression kind and operand position, and `test_taint_semantics.sh` runs every row on both engines from both origins.
  - **Migration:** a program that branches or matches on tainted data, or passes it to a function, and hands the result to a sink, now meets `taint_tracking` at its configured level.
- **A function's output contract now applies to a bare `return` too (A114).** The VM checked `return_not_null`, `return_type` and the rest only when the function returned a value; a bare `return`, or a body that ended without one, returned null past them. Both return paths now run the same checks.
  - **Migration:** a program whose contracted function returned null that way, and passed, is now refused at the contract's level.
- **A completed behavioural sequence is now enforced whatever its shape or its neighbours (A113, Bundle 10).** Four ways a pattern the operator wrote to BLOCK did not: an ADVISORY pattern listed before a HARD one that shared its final step suppressed the HARD block (one event completed at most one pattern); a single-step pattern was never checked before the call, so the action ran and only then blocked; and a partial match kept its oldest start, so a credential read, a few ordinary calls, a second read and a send slipped the gap window. The detector now advances every pattern on each event and enforces the strictest completion, catches single-step and about-to-complete patterns before the call, and keeps a bounded list of partials so a fresh start re-anchors a stalled one -- while matching stays non-overlapping (consecutive events do not cascade into overlapping matches). Every change only tightens; none lets through anything that blocked before.
- **A tool now runs on the VM its agent call came from (A93).** Every tool ran on the main VM from whatever thread the tool loop was on. So two `agent.batch` / `fan_out` workers whose models both called a tool ran on one VM stack at once: their results were wrong, and the batch result read null. A tool called from an `async fn` ran on main's stack while main was still executing, which could crash the process. A batch now lends its VM to the workers, which enter it one at a time; the model calls still run concurrently. An `async fn`'s tool runs on the `async fn`'s own VM, and function-scope capabilities attribute it to that function.
- **A restriction written in a type the loader does not read now refuses the config (A111).** `"shell": {"enabled": "no"}` (or `0`, `"off"`, `"false"`), and a `blocked_paths` or `blocked_read` entry written as a nested list, used to load without the restriction: the shell ran, and the blocked file and variable were read, under `mode: enforce`, with no warning. The same was true of a single string where `languages.allowed`/`blocked`, an agent role's lists or a function's `allowed_actions` expect a list. Such a config is now refused at load (exit 4), naming the key and the type it must be; a mid-run reload to one is refused as a config error.
  - **Migration:** a config that relied on one of these values being ignored no longer loads. Write the switch as `true`/`false` and each list as a list of strings. A single string where a `capabilities` list is expected still loads, as a one-entry list, and so do the bare forms `"shell": false`, `"network": false` and `"filesystem": "read"`. Of the 164 configs in this repository with these sections, the only one refused is a test fixture poisoned on purpose.
- **A provider's token counts can no longer un-spend a token budget (A110).** The run budget (`agent_dispatch.hard_stop.max_tokens_per_run`) and each agent's `max_total_tokens` add the counts in the response body. A reply reporting negative counts lowered the total, so a 1,000-token run kept sending; a count past `int`'s range wrapped (2^62 read as 0). Counts are now read as 64-bit and clamped to 0..10,000,000.
- **Report writers no longer read the check results while another thread records them (A86).** The JSON, SARIF, JUnit, CSV and HTML writers, telemetry and the post-run audits read `check_results_` without its lock while `async fn`s and agent workers recorded checks, which could crash the process or write a torn record. They now read a copy taken under the lock.
- **Two thread-safety defects ThreadSanitizer found on its first run (A108, A109).** The polyglot-optimization calibration was published before it was built, so a second `async fn` could read a half-built table; and the audit logger and the tamper-evident logger formatted timestamps with `gmtime()`, whose one static buffer let entries written at once carry each other's time.
- **Under enforce, a misspelled `security.sandbox_level` left the run unrestricted (A94).** The clamp to `standard` skipped any config that set a level, valid or not, so a typo kept the CLI's default. Only a real level now grants more than `standard`; a misspelled one still only warns.
- **An environment's `mode` now decides whether governance is active (A95).** `--env prod` with `mode: enforce` over a file in `mode: off` enforced nothing. And a reload could switch an active engine off without the ratchet seeing it, under an environment whose `mode` is off. Such a reload is now refused.
- **Two reloads the ratchet accepted loosened governance (A96).** With `env_vars.read: false`, dropping a name from `blocked_read` was accepted, though child processes then received it. And `detect` -> `soft` was accepted, though under `--governance-override` a soft block lets the action through. Both are refused now.
- **Two more role edits the ratchet accepted loosened governance (A101, A102).** Removing a role's last ENV action from `allowed_actions` stopped the matrix governing the environment, so the action it had denied became allowed. And removing a role's `network_allowed: false` gave the role the network back. Both are refused now, as removing `shell_allowed: false` already was.
- **An `agents` block written as a list now refuses the config (A100).** The loader reads only the object form, keyed by role name, so a list loaded no roles at all and said nothing; it then warned, and now (Bundle 16, the owner's decision) refuses the config (exit 4), for the legacy `agent_roles` key too, by the same rule as A111.
  - **Migration:** write the roles block as an object keyed by role name, `"agents": {"worker": {...}}`. No config in this repository used the list form.
- **A capability lease no longer widens what child processes receive (A97).** A lease without ENV_READ switches the child-environment scrub to allowlist mode, which dropped the project's `blocked_subprocess_vars`. A blocked name is now withheld in either mode.
- **A mid-run reload can no longer loosen any of the 773 govern.json keys the ratchet did not compare (A104, Bundle 8).** Every key the loader reads is now classified, and the ones a reload must not loosen carry a declared order the engine enforces: the run budgets (`agent_dispatch.hard_stop.*`), `limits.array_size` / `call_depth`, `context_drift.enabled`, the circuit-breaker and exposure thresholds, `restrictions.polyglot_output`, every presence-enabled code_quality block, and the rest. Keys with no single tighter direction refuse any mid-run change. Measured on 2.0.1 with `naab-gov ratchet`: the loosening of 463 of the 475 declared keys was accepted.
  - **Migration:** a reload is now refused when it adds or removes a declared key that has no declared default; when it edits a `fixed` key (hook commands, retry tuning, state-file paths) or an existing `governance_plugins` entry; or when it rewrites a check's shorthand into an equivalent object form. Make such edits between runs. The violation names the key and its declared order.
- **A refused mid-run reload no longer changes the JSON depth limit (A106).** The loader applied `limits.data.max_json_depth` process-wide while parsing, and a reload parses before it decides, so a rejected reload to a deeper limit took effect anyway.
- **The codegen ratchet read 0 as the tightest value of five caps where 0 means no cap (A103).** `max_code_size_bytes`, `max_code_lines` and the three cumulative caps 50 -> 0 were accepted as tightenings, and `timeout_seconds` -> 0 (the 15 s fallback) was too.
- **A list could empty itself while two engines ran at once (A92).** The cycle collector marks from one VM's stack and globals but sweeps every value in the process, and clears unreachable containers it takes for cycles. A list another VM was using looked like one, so a program running an `async fn` computed with empty lists: observed, main's own list read 0 elements in 6 of 6 runs while main looped beside an async fn, and an async fn's list argument read empty in about 96% of calls while main collected. The collector now runs only when its engine is the only one executing in the process, and defers otherwise. An `async fn` therefore delays cycle collection until it finishes. `--gc-stats` now reports the engine that ran: collections run, deferred, and values collected. On the default engine it used to print the idle tree-walker's count, which was always 0.

## [2.0.1] - 2026-10-31

The first 2.x release with packages. v2.0.0 was tagged, but its macOS build failed and the release step that attaches the packages and notes never ran. Everything in [2.0.0] below applies to 2.0.1, including "Upgrading from 1.x". From 2.0 on, the Releases page carries 2.x only; every earlier version remains as a git tag.

### Fixed
- **macOS builds again.** `src/runtime/resource_limits.cpp` converted `pthread_t` to an integer with a `static_cast`, which does not compile where `pthread_t` is a pointer (macOS). It now produces the value CPython's `PyThread_get_thread_ident()` returns for the same thread, which the Python timeout hook compares it with: the address on macOS, the same integer as before on Linux.

### Added
- **A macOS build check in CI** (`ci.yml`): the release's macOS build steps plus a smoke test. Nothing built on macOS between v1.8.0 and the v2.0.0 tag, so the release was the first to find this.

## [2.0.0] - 2026-10-30

> Covers `v1.8.0..v2.0.0`: 387 commits on master, 256 of them merged pull
> requests. This section was not kept up to date between #165 and the README
> review of 2026-10-24. "Highlights since #165" below is a short list of
> user-visible features from that period, not a complete record;
> `git log v1.8.0..v2.0.0` is the complete record. 1.5.0 through 1.8.0 have no
> section in this file; their tags remain, and `git log` between them is their
> record.

### Upgrading from 1.x
A major version because these change what an existing project sees. Each is described in full below, with its remedy.
- **Two defaults now block** (Bundle 6, migration note below): built-in sequence patterns enforce at their own levels, and what a model writes is scanned for secrets. Each has a one-line opt-out that restores the old behaviour exactly.
- **Under an enforce `govern.json` every program runs on the default engine, in a CLI or worker process.** `--tree-walk` and the REPL are refused (exit 4), and REST `/execute` runs each request in a worker process.
- **`--no-governance` waives only the "no govern.json found" error.** It no longer switches off parts of a config that is found (A78), and a config that is found but does not load refuses the run (A84). A flag may tighten `govern.json`'s run limits and sandbox level, never loosen them (A79).
- **Every regular expression is matched by `naab::rx`** (A89): RE2 for patterns it reads exactly as before, QuickJS's libregexp for lookaround and back-references. Patterns are translated to keep the old dialect; a run of the whole test suite that also matched every input of 4 KB or less with the old matcher found no difference. A governance scan that runs past its step budget refuses the block (exit 3) instead of passing it.

### Changed -- defaults (Bundle 6; migration note)
Two defaults now block where they used to report or stay silent. Each has a one-line opt-out, and each opt-out is the old behaviour exactly.
- **Built-in sequence patterns enforce at their own level by default.** With `behavioral_sequences.enabled: true` and no `patterns` list, `default_pattern_enforcement` now defaults to `"declared"`: each built-in pattern blocks at the level it declares (most are SOFT: exit 3, `--governance-override` lets it through; `config_tampering` is HARD). It shipped `"observe"` (every built-in at advisory) in #182, because correcting the built-ins' step names switched them on for the first time. What changes for you:
  - `credential_harvesting` (an `env.get` of a name holding key/token/secret/password, then a network call within 5 events) now refuses the network call. A program that reads its own API key and calls out is that shape. Write `"default_pattern_enforcement": "observe"` to keep the old behaviour, or list your own `patterns`, which replace the built-ins.
  - A config that lists `patterns` is unaffected: listed patterns replace the built-ins and carry their own levels.
  - A reload from `"declared"` to `"observe"` is a ratchet violation, as before; so a running program cannot opt out mid-run.
- **New built-in pattern `credential_exfiltration`**: `env.get` of a credential-named variable, then `crypto.base64_encode`/`hex_encode`, then `agent.send` or an `http` call, at most 5 events apart, SOFT. No built-in covered a credential sent to a MODEL (`credential_harvesting` ends at a network call, and `agent.send` is not one). The README's example config wrote it as a user pattern.
- **What a model writes is scanned for secrets unless the config says not to.** An agent reply, each `agent.propose()` candidate and the arguments of a tool call the model makes are checked against the secret patterns when `govern.json` does not set `no_secrets` at all; a hit ends the run (HARD, exit 3), and the reply never reaches the program. `"code_quality": {"no_secrets": false}` (or `"restrictions": {"no_secrets": false}`) turns it off. The program's own source, its prompts and tool results stay opt-in, as before. A reload that writes `false` over the default is a ratchet violation. An `extends` child that wants it off must say so itself: a parent's `false` is not inherited, because absent in the child means the new default.

### Changed
- **Under an enforce `govern.json`, programs run on the default engine only.**
  - `--tree-walk` is refused (exit 4) before anything runs. That holds when the config's mode is `enforce`, and also when `--env` or `governance.default_env` would make it `enforce`. Audit and off still run the tree-walker.
  - REST `/execute` now runs each request in its own `naab-lang run` worker process on the default engine, as `naab-lang mcp` does. One request's state, data or crash no longer reaches the next or the server.
    - A server whose directory resolves to no `govern.json` refuses requests (exit_code 4), as a file run does, unless it was started with `--no-governance`.
    - On Windows `/execute` answers 501 ("not available on this platform yet"), like `naab-lang mcp`.
    - `naab-lang --no-governance api PORT` now parses the port.
  - The REPL (`naab-lang` with no arguments, `--repl`, code piped into either, and the `naab-repl*` binaries) does not start beside a `govern.json` in mode enforce or audit, or beside one that does not load (exit 4, as a file run). It loads no `govern.json` at all (A80): beside an enforce config, code piped into `naab-lang` read an environment variable the config blocks, and exited 0. It points to `naab-lang run`.
- **The README is restructured around what NAAb is for.** It led with the governance engine but spent over half its length on a language tour and a stdlib reference, described sequence detection in six places, and put the Quick Start at line 141. It now runs: the pitch and the hero, the Quick Start, how it works, what it stops (one table with the demo), what it does not stop, configuration, features in brief, the language in one example. The hero is a shipped program, `examples/quickstart/agent_task.naab` with its `govern.json`, that a visitor runs with `GEMINI_API_KEY=placeholder` -- the send is refused before any request is made. The language tour, polyglot details and the stdlib table moved to `docs/language-tour.md`, with new examples for SQL parameters, persistent runtimes, `naab_return()` and `-> JSON`. `tests/self-audit/test_readme_claims.sh` reads both files and now also checks the prose: the four enforcement levels, the protected policy files, shell-off, `CONTRA-013`, the MCP tool list, the quoted error messages, the pattern and signal counts, Go and PHP headers, live guidance, and response secret scanning.
- **`USER_GUIDE.md` is rewritten as a guide to working in a project.** The old guide described a different language: `import "stdlib" as std`, a block registry with "24,483 blocks", `use BLOCK-PY-09145`, a `~/naab-repl` binary, and 45 code fragments with no `main`, several of which could not parse. The new one goes from `naab-lang init` to modules, other languages and the policy, and says what a new project's `govern.json` and sandbox allow (writes, environment variables and shell blocks need `security.sandbox_level: "elevated"`). Every program in it is a complete file, and `tests/self-audit/test_readme_claims.sh` RC-22 runs each one in a fresh `naab-lang init` project with only the `govern.json` change its section shows, checking its exit code, promised values and output. It lists what does not work yet. Writing it found the defects fixed below (A71-A74, A70(a)).
- **`==` and `!=` compare lists, dictionaries and structs by value** (A72). Both engines handled only null, bools, numbers and strings and called every container unequal, so `a == a` was false for an array and `x == []` could never be true. Lists now compare element by element, dictionaries key by key, structs by type and fields, with the scalar rules unchanged (`5 == 5.0`, no conversion between types). One helper for both engines, `include/naab/value_equality.h`. Test: `tests/vm/test_value_equality.sh`.
- **README and demo checked against the binary.** Every command, config example and code block in `README.md` was run against the current build, and the claims that did not hold were corrected:
  - The opening example shows the engine's real output, and says that sequence detection needs a pattern you write and `behavioral_sequences.enabled`.
  - The CI report flags are `--governance-sarif PATH`, `--governance-junit PATH` and `--governance-report PATH` (before or after the file name since the A70(a) fix below). The old `naab-lang app.naab --governance-report sarif > results.sarif` wrote a JSON report to a file named `sarif` and the program's output to `results.sarif`.
  - The config example uses keys the loader reads (`restrictions.code_injection`, `restrictions.shell_injection`, `limits.code.max_lines_per_block`, `limits.timeout.global`). Four of its keys were read by nothing.
  - The signing commands work as written: `--keygen PATH` needs the directory to exist and writes `PATH` and `PATH.pub`.
  - Response secret scanning is described as opt-in (`code_quality.no_secrets`), BSD's 16 built-in patterns as advisory unless declared, and CDD as 23 signals.
  - Spread and rest syntax (`[...a]`, `fn(...args)`), `const` immutability and `bolo.report` are no longer listed: none of them exists.
  - The pipeline example pipes into user functions; piping into a standard-library function does not work yet (A67).
  - Added: TypeScript and SQL blocks, the MCP server, live guidance, propose/commit, output admissibility, run budgets, the `collections` module, and a "what it does not contain" note pointing at the protection map.
  - `tests/self-audit/test_readme_claims.sh` runs the README's examples and the demo, so a claim that stops holding fails the suite.
- **The demo blocks each scene for the reason it narrates.** `demo/governance_demo.sh` now requires exit 3 and the scene's own rule; scene 6 used to be blocked by the sandbox rather than by taint tracking, and scene 7 (all issues fixed) failed on two of its own checks. Re-recorded.
- **The no-govern.json error names the real requirement.** It said `--require-governance set`, a flag nobody had passed, because requiring a govern.json is the default. It now says so and points at `naab-lang init`. The `--no-governance` help line no longer claims to disable the engine (it has not since #244).

### Fixed
- **A long input can no longer crash or stall the governor** (A89). libstdc++'s `std::regex` recurses once per character, so a long enough model reply, program literal or contract pattern ended the process with a segfault (exit 139), leaving no verdict and no report; some patterns also took minutes on a long line. Every regular expression now goes through `naab::rx` (see Upgrading). The vendored QuickJS carries a backport of QuickJS 2024's regexp step limit (`external/patches/`), and the checks over polyglot blocks, function bodies, whole files, PII and model output run under one step budget. The dangerous-alias and comment co-occurrence patterns were rewritten to run in linear time without changing what they match. `--timeout` now also stops a `<<javascript>>` block stuck in a catastrophic regular expression.
- **`restrictions.dangerous_calls` no longer refuses an anonymous JavaScript function** (A91). Its patterns are matched without case, so `\bFunction\s*\(`, meant for the `Function()` constructor, also matched every `function (`. A match spelled exactly as the reserved word `function` is now skipped. Every other spelling, including an alias such as `FUNCTION(...)`, is still refused, and the rest of the table still ignores case (PHP's `SYSTEM(...)` is refused).
- **The telemetry webhook receives the whole run** (F53). Every CLI exit drains the forwarder, and `RunStart`, `RunEnd` and `GovernanceCheckSummary` are forwarded, so the forwarded chain can be verified.
- **A refused tool call is answered beside its call** (A44). A refusal used to send only a result, a request both providers reject.
- **The reasons reach MCP callers, editors and reports** (A43). A blocked `naab_propose` reports its findings, passed through the secret and PII scan; `findings` keeps the start and the end of long output; naab-lsp diagnostics name their rule again; a static HARD block on the default engine writes its reports.
- **The governance voice prints its guide once**, and a guide matching the secret or PII patterns is withheld (A40).
- **JavaScript and thrown values in VM `async fn`s** (A61, A59(a)). A `<<javascript>>` block inside an `async fn` runs; a value thrown in one reaches the `await`'s `catch` as that value, with its taint; a closure returned from one stays valid.
- **Nim and TypeScript blocks** (D12, D13). A Nim block's last line is printed when it has a value. TypeScript blocks run at `elevated`, where they ran out of memory.
- **Reported lines** (A76). Every match-level finding (secrets, placeholders, PII, incomplete logic) is printed and recorded at the line of the match.
- **Embedded Python's sockets answer to `capabilities.network`'s host lists** (A35). A `<<python>>` block's `socket.connect` was judged on the network capability alone, so `allowed_hosts`/`blocked_hosts` did not reach it, and `sendto`/`sendmsg` were not judged at all: with `network.enabled: false`, a UDP datagram left through an import the source check cannot see. Names are now judged at the lookup (`getaddrinfo`, `gethostbyname`) and at a connect given a name; when an allowed name is looked up under an `allowed_hosts` list, the engine resolves it too and admits those addresses, and a connect or send to any other address is refused unless an IP entry admits it. Alternate IPv4 spellings (`2130706433`, `127.1`) and the IPv4-mapped IPv6 form are judged as the address they reach. AF_UNIX sockets and runs with no `govern.json` are unchanged. Test: `tests/governance_v4/test_python_host_lists.sh`.
- **Threads a `<<python>>` block spawns are governed like the block** (A85). The embedded-Python audit policy read the block's sandbox and engine from thread-local state, so a `threading.Thread` the block started ran with neither: it read and deleted a `blocked_paths` file and ran `os.system` with shell disabled while the block's own thread was refused, and -- once the host lists reached Python sockets -- connected where `allowed_hosts` refused the block and sent UDP with network disabled. On such a thread the policy now uses the run's sandbox and engine, the same ones the block runs under. Tests: `tests/governance_v4/test_python_thread_policy.sh`, `tests/governance_v4/test_python_host_lists.sh` HL-15..HL-18.
- **`process.run` answers to a role's shell gate** (A54). A role with `shell_allowed: false` (or no `SHELL_EXEC` in its action matrix, or a function without `SHELL_EXEC`) ran any command through `process.run` while its `<<shell>>` blocks were refused. Test: `tests/governance_v4/test_process_run_role_gate.sh`.
- **`restrictions.information_disclosure` covers `ps -A`, `ps -e`, `ps -o pid -e` and `process.run`** (A17 F8a). `block_process_listing` matched only `ps aux` and `ps -ef` in source, and `process.run("ps", ["aux"])` was never checked. One scan now covers both, recognising every option that lists all processes, under either name (`ps`, `ps.exe`); `ps -p PID` still runs. `block_system_info_leak` reaches `process.run` too. Test: `tests/governance_v4/test_info_disclosure_commands.sh`.
- **`crypto.base64_encode`/`base64_decode`/`hex_encode`/`hex_decode` answer to `restrictions.data_exfiltration`** (F19), as `base64.b64encode(` and `binascii.hexlify(` in a block already did, at the restriction's level. Test: `tests/governance_v4/test_native_encoding_exfil.sh`.
- **A risk budget that overshoots zero is exhausted, not unlimited** (A68). Two charges on one turn took a budget of 1 to -1, the "no budget" sentinel, and every later send was admitted; `agent.environment()` reported -1. The remaining budget now stops at 0. Test: `tests/governance_v4/test_risk_budget_overshoot.sh`.
- **`agent_review` no longer fails open on an unrecognised setting** (A29). `fail_policy` is compared without case, and a value other than `open`/`closed` is treated as `closed`, with a warning. An enforcement zone level other than `advisory`/`soft` blocks as hard: an unknown string with a warning, `detect` and `approval_required` because nothing can catch a block before the program runs. Before, every value but `hard`/`soft` ran as advisory while the banner printed the configured string. Under `fail_strategy: "continue"`, a failed agent the review continues past is now reported. Test: `tests/governance_v4/test_agent_review_settings.sh`.
- **A tainted assignment meets a `variable.assign` taint sink on the default engine** (A65). The check looked the variable's name up in the engine's shared taint set, which the VM writes only for globals and only afterwards, so a local's assignment passed. Test: `tests/governance_v4/test_taint_assign_sink.sh`.
- **`naab.toml` values that read as security settings say they are not applied** (A28). `[polyglot] python = false` and `[features] sandbox_mode = false` changed nothing, silently. The manifest stays package metadata; `naab-lang run` and `naab-lang manifest check` now name the `govern.json` key that does the job (`languages.blocked`, `security.sandbox_level`). Test: `tests/cli/test_manifest_unapplied.sh`.
- **A `govern.json` that is found and does not load refuses the run, even with `--no-governance`** (A84). A parse error, an empty file, an `extends` that does not resolve or an array over the element cap read as "no govern.json found": the refusal named no config, and `--no-governance` ran the program with none of the policy (observed: a blocked environment variable printed, exit 0, on both engines). Such a run now exits 4 and says the config did not load; an integrity failure keeps exit 3. REST `/check` answers 400 instead of a verdict, `naab-gov check` exits 4, and the REPL does not start. Test: `tests/cli/test_malformed_config.sh`.
- **The default engine writes the audit trail's polyglot entries and counts every runtime `.exec()`** (A81). Only the tree-walker did either: on the VM, `audit.log_events.polyglot_executed` and `polyglot_timing` wrote nothing, and a persistent runtime's `.exec()` was not counted against `limits.execution.polyglot_blocks`, so three `js.exec()` calls ran under a HARD limit of 2. With the tree-walker refused under enforce, an enforce run had recorded no polyglot execution at all. `.exec()` entries and messages now name the call's own line; seven other VM sites still read the line three instructions back (A82, open).
- **Only the private key that signed `govern.json` unlocks a flag the owner locked** (A77). The check read the `NAAB_SIGNING_KEY` file as a public key: the owner's private key never worked, and with a signature written without a timestamp (still accepted) the public key, which anyone can read, unlocked `integrity.blocked_flags`. Test: `tests/security/test_flag_authority.sh`.
- **`--no-governance` no longer switches off parts of a `govern.json` that is found** (A78). With a config found, it still skipped the sandbox's capability sync (shell, network, environment), the enforce-mode default sandbox, the post-run audit, the `must_produce` contracts and the check for a deleted `govern.json`. It now only waives the requirement when no `govern.json` is found, as `--help` already said.
- **A flag may only tighten `govern.json`'s run limits and sandbox level** (A79). `--timeout` and `--memory-limit` can lower a configured limit, not raise it; an explicit 30-second limit counts as configured; an inherited (`extends`) limit applies as written; the tree-walker reads both limits; the limits reach child processes whether or not the sandbox level changed; and under `enforce` a `--sandbox-level` above `standard` is lowered to `standard` unless `govern.json` sets one.
- **Default parameter values on the VM** (A71). The default engine never applied them: a parameter left out of a call read whatever the stack held next, so `fn c(x, y = 10)` gave `c(1)` = 2 and `fn d(n = 5) { let t = 100; return n }` returned 100. Defaults are now evaluated on every call, in order, in the callee's scope, as on the tree-walker, and a required parameter after a defaulted one is still required. Test: `tests/vm/test_default_params.sh`.
- **The `standard` sandbox refused a new file at the top of the project** (A73). `file.write("out.txt", ...)` and `file.exists("missing.txt")` stopped the program with a sandbox violation while `data/out.txt` was accepted: a bare name for a file that did not exist yet was never made absolute. Test: `tests/security/test_sandbox_relative_path.sh`.
- **A `govern.json` run time limit below 30 seconds was raised to 30** (A74). The configured limit now applies as written. `--help` described `--timeout` as a per-block timeout. Removing the floor exposed that an `extends` merge never reached the run's limit (it set `limits.timeout.global` but not the copy the run reads); an inherited limit now applies as written. Test: `tests/cli/test_timeout_config.sh`.
- **Seven run options worked only after the file name** (A70(a)): `--governance-report`, `--governance-sarif`, `--governance-junit`, `--governance-record-baselines`, `--governance-check-baselines`, `--memory-limit` and `--allow-network`. Written first they were taken for the command ("Unknown command"). Test: `tests/cli/test_flag_position.sh`, whose FP-S arm fails on the next option added to one parse loop only.
- `--help` lists `naab-lang init`'s `--taint` and `--languages`.
- **A project made with `naab-lang init` could not run hello world.** With the default `standard` preset (and `relaxed`), the generated `govern.json` set `complexity_floor` to `"level": "off"`. `"off"` is not a level, so the check ran at hard and refused `main` for a complexity score below 10: the README's own hello world exited 3. More generally the generator wrote checks its profile meant to switch off in forms the loader ignores or enforces anyway (`"enabled": false` on a check that a present block switches on, `"level": "off"`), plus keys nothing reads. It now leaves out what its profile turns off, and each preset runs hello world with no "has no effect" or "not a setting" warning (`tests/cli/test_init_governance.sh` Test 10).
- **Optional chaining on null on the VM** (`a?.b`, `a?.m()`). The null path popped one value too many, so the expression took the value below it: `print(u?.name)` failed with "Value is not callable", and `let n = u?.name` set `n` to the print builtin. Wrong since optional chaining was added; the tree-walker was correct. Test: `tests/vm/test_optional_chaining.sh`, plus a differential probe.
- **Rule and setting names are no longer redacted in error messages.** The error sanitizer took any word starting with `token`, `secret`, `credential` or `auth` for a secret, so a rule named `credential_exfiltration` printed as `<redacted>` and `max_tokens_per_run` as `max_<redacted>`. A separator (`:`, `=`, whitespace or a quote) between the keyword and the value is now required. `token=value` without quotes is now redacted, which it was not before.

### Highlights since #165 (not a complete list)
- `naab-lang mcp`: governed execution over the Model Context Protocol, one worker process per call (#307).
- Live guidance: project knowledge from markdown, SQLite, https, command and MCP sources, delivered when governance observes the matching event (#309, #310, #312, #316).
- `<<sql>>` blocks: in-process SQLite with no filesystem reach, variables bound as parameters (#291). TypeScript blocks run through `tsx`.
- Function-scope capabilities (`capabilities.functions`): which actions a function may take, intersected down the call stack.
- `capabilities.shell.enabled: false` refuses every language that runs as a separate process (#298).
- The protection map: per language and forbidden action, which layer stops it (#296).
- The run's evidence files are protected from the governed program (#314), and a reload cannot move or switch them off (#317).
- `limits.array_size` applies at every way an array grows (#335).
- `string.slice`, and one implementation of slice/substring/replace for both engines (#262).

### Added
- **Output admissibility: the `undetermined` verdict** (`circuit_breaker.output_admissibility.on_undetermined`)
  - A gate that passes because it has no basis to judge now says so. Inside the adaptive baseline window every statistical signal's penalty is suppressed, so coherence sits at its ceiling and the threshold comparison passes on a number that was never a judgement.
  - `OUTPUT_ADMISSIBILITY_EVAL` gains `result: "undetermined"`, `baseline_state` (`calibrating`/`complete`/`disabled`) and `on_undetermined`; `OUTPUT_INADMISSIBLE` gains `undetermined`; `response.admissibility.undetermined` exposes it to scripts on both directions.
  - One-directional by design — it marks the PASS direction only. Objective signals (S1 `circular_actions`, S21 `response_repetition`) are exempt from baseline absorption and charge from turn 1, so the gate can still rule OUT while calibrating.
  - Policy: `"pass"` (default, no behaviour change), `"quarantine"`, `"block"`. Ratchet-enforced (`pass` < `quarantine` < `block`).
  - Undetermined holds never advance the quarantine streak: `adaptive_baseline_window` and `max_quarantine_streak` both default to 5, so counting them would terminate every agent on its last calibrating turn regardless of behaviour.
- **Behavioral Sequence Detection (BSD)** — FSM-based multi-step attack pattern matching
  - Configurable via `behavioral_sequences` in govern.json. Your patterns replace the built-in set (17 patterns today, enforcing at their own levels unless `default_pattern_enforcement` is `"observe"`; advisory until Bundle 6)
  - Detail globs for per-argument matching (e.g., `env.get:*SECRET*|*TOKEN*`)
  - Decay/expiry — events older than N turns or N seconds don't match
  - Pre-execution blocking — dangerous sequences blocked before the final step runs
- **Context Drift Detection (CDD)** — coherence monitoring for LLM agent conversations
  - Tracks declared intent vs observed actions across agent turns
  - Signals: repeated failures, circular actions, scope creep, contradictions
  - Configurable via `context_drift` in govern.json with per-signal weights and coherence threshold
- **VM taint lineage tracking** — every tainted value carries a source-to-sink chain
  - Preserved across `OP_RETURN` frame eviction
  - Per-language taint sinks (`python_exec`, `go_exec`, etc.)
  - Makes governance violations actionable — users see exactly where taint originated
- **govern-template.json sync** — 15 missing keys added, 11 dead fields annotated with comments explaining what replaced them, template restructured into 12 logical groups
- BSD/CDD dashboard telemetry — `--governance-dashboard` now shows BSD events processed, patterns matched, and CDD coherence state

### Fixed
- **Zero-warning build** — root-cause analysis and fix for all 20 build warnings across 8 files
  - `executeWithReturn()` missing exit code check — garbage stdout parsed as return value on subprocess failure
  - Dead code from refactors: `last_line_idx` (cpp_executor_adapter), `blocks_found` (block_registry), `DEFAULT_JSON_DEPTH` (json_impl)
  - Sign conversions in backward loops and timeout parsing (main.cpp, agent_impl, json_result_parser)
  - `std::result_of` → `std::invoke_result_t` migration (deprecated C++17, removed C++20)
- BSD/CDD correctness — 6 post-review findings (detail_glob matching, pre-execution abort, hard enforcement)
- `behavioral_sequence.cpp` moved to `naab_security` CMake library (fixed linker errors on Sanitizers, Windows, and CodeQL CI)
- Shell polyglot test T6 skipped on Windows (no shell executor on that platform)
- Stale regex docs synced with actual patterns
- Struct helper error message improved

### Security
- **V-RCE-014**: C++ polyglot `#include` path traversal bypass — relative paths (`../../../etc/shadow`) not blocked by `isCppSourceSafe()` (self-audit v14)

## [1.4.0] - 2026-04-30

### Added
- **`use agent` stdlib module** — governed LLM conversations with multi-provider support
  - `agent.create(name)` / `agent.send(handle, message)` / `agent.run(handle, message)` — full conversation lifecycle
  - `agent.messages(handle)` / `agent.usage(handle)` — conversation history and token tracking
  - Anthropic (Claude) and Gemini provider support with automatic API format translation
  - Per-agent governance: `max_turns`, `max_tokens`, `max_total_tokens`, `system_prompt`
  - Per-agent sandbox config: `allowed_paths`, `blocked_paths`, `shell_allowed` (advisory, pending tool execution)
  - Server-side `AgentTracker` — turn/token enforcement immune to handle mutation attacks
  - Output content filtering: `checkSecrets()` (18 patterns) and `checkPii()` (5 patterns) on all LLM responses
  - Tool-use response blocking — agents cannot invoke function calls (defense-in-depth until tool loop is implemented)
  - Forged handle rejection — unknown handle IDs blocked at send time
- `\x` hex escape (`\x1b` etc.) and `\e` ESC escape in string literals
- **Gate 11b**: main{} body hash — blocks modification without re-baselining
- Banned function alternatives in governance errors (eval, Function, exec, compile)
- Blocked import alternatives in governance errors (os, subprocess, shutil, socket, ctypes, pickle)
- `array.get(index)` safe access — returns null instead of throwing on out-of-bounds
- `format_timestamp()` stdlib helper
- `PROJECT_SETUP.md` — end-to-end project bootstrapping guide (keys, signing, govern.json)
- `CLAUDE.md` — internal reference for Claude Code (build, test, architecture, conventions)
- 5 governance hardening fixes from naab-15 Synapse adversarial analysis
- 5 systemic runtime enforcement fixes for LLM code generation

### Fixed
- Dict.get() hint dedup — no longer spams repeated hints in loops
- Taint labels show "argument N of 'func()'" instead of cryptic `__argN__`
- Struct-to-dict conversion error now suggests `to_dict()` or field access
- CLI arg passthrough: NAAb flags no longer consumed after script filename
- `--sandbox-level` and `--gc-stats` flags restored after passthrough regression
- `env.get_args()` returns empty in VM — args provider not wired to VM
- `allowed_paths` now takes priority over `blocked_paths`
- Sanitizer warning deduplication
- Unsigned governance mode warnings removed (broke CI tests)
- `validate_` sanitizer taint clearing + `file.append` registered as taint sink

### Security
- Agent output filtering blocks secrets (API keys, tokens) and PII (SSN, CC numbers) in LLM responses
- Server-side agent tracking prevents governance bypass via handle dict mutation
- Bypass flags removed from all governance error messages (11 files) — enforced by `test_error_msg_leaks.sh`
- 10 governance gap fixes from context-review Gemini session scanning
- 15+ DX improvements from context-review analysis (better error guidance without leaking internals)

## [1.1.0] - 2026-04-23

### Added
- **Ed25519 trust-anchored governance signing (V-SC-009)** — asymmetric signing replaces HMAC-SHA256
  - `--keygen [path]` generates Ed25519 keypair, installs public key to trust store
  - `--signing-key <path>` specifies private key for signing operations
  - `--trust-key <pubkey.pem>` installs a public key to the trust store
  - `--list-keys` displays trusted key fingerprints
  - External trust store at `~/.naab/trusted-keys/` (outside project sandbox)
  - Signature format: `ed25519:<base64>` prefix for Ed25519, `hmac:<hex>` for tagged HMAC
  - Legacy HMAC backward compatibility preserved with deprecation warning
- **CryptoUtils Ed25519 primitives** — `ed25519Keygen`, `ed25519Sign`, `ed25519Verify`, `ed25519Fingerprint`, `toBase64`, `fromBase64`
- **TrustStore class** — `trust_store.h/cpp` for managing Ed25519 public keys

### Security
- **V-SC-009: Trust store keys + missing .sig = BLOCK** — attacker sandboxed to project directory cannot bypass governance by deleting `.sig` files when trusted keys exist in `~/.naab/trusted-keys/`
- **V-SC-009: HMAC .sig + trust store = BLOCK** — forces re-signing with Ed25519 when trust store is active
- **V-SC-009: ed25519Verify rejects private keys** — only public keys accepted for verification
- **V-SC-009: readSigningKey bounded read** — lstat + S_ISREG + 8KB cap prevents abuse
- **V-SC-009: .sig file read capped at 4KB** — prevents memory abuse via crafted signature files
- **V-SC-009: TOCTOU eliminated** — .sig opened directly, branch on is_open() instead of exists-then-open
- **V-SC-009: Private key written via writeFileSecure()** — O_CREAT|O_WRONLY|O_EXCL with mode 0600
- `NAAB_SIGNING_KEY` added to blocked env vars (env_impl.cpp) and subprocess scrubbing (subprocess_helpers.cpp, persistent_process_executor.cpp)

### Changed
- `signFile()` prefers Ed25519 (NAAB_SIGNING_KEY) over HMAC (NAAB_GOVERN_KEY) with deprecation warning
- `verifySignatureImpl()` unified verification replaces duplicated logic in verifyFileSignature/verifyContentSignature
- Owner-awareness messages updated: ~15 occurrences changed from HMAC-specific to generic signing references
- govern-template.json updated: integrity comment changed from HMAC to Ed25519, `require_signature` field removed

## [1.0.0] - 2026-04-14

### Added
- array.map()/filter()/reduce() aliases (no _fn suffix needed)
- Module name aliases (fs→file, os→env, re→regex) with "Did you mean?" hints
- spawn/wait/wait_all → async function hints for common concurrency patterns
- Package integrity verification (SHA-256 hashes in naab.lock)
- Unpinned git dependency security warnings

### Fixed
- Tree-walker timeout on Android/Termux (global_shutdown_ set from timer thread)
- Package manager shell injection (system()→fork/execvp)
- Package manager silent failures (installAll reporting with summary)
- Subcommand flag parsing (naab install --verbose no longer treated as package name)
- 6 parser expect() messages upgraded with examples and suggestions

### Security
- V-PKG-001: Shell injection in extractTarball eliminated (fork/execvp)
- V-PKG-002: SHA-256 integrity hash verification for downloaded packages
- parseSpec validates against shell metacharacters (';|`$()&)

## [0.9.0] - 2026-04-09

### Added
- **String interpolation** — `f"Hello {name}"` and `"Hello ${name}"` syntax
- **for-in with index** — `for (i, item) in arr { }` binds both index and value
- **switch keyword** — alias for `match` expression
- **--quiet/-q flag** — suppresses governance info messages for scripting use
- **--strict flag** — alias for `--strict-types`
- **Multi-line error context** — parser errors show 2-3 source lines with caret
- **Parser error recovery** — reports multiple errors instead of stopping at first
- **Top-level runtime declarations** — `runtime py = python.start()` at file scope
- **Pre-built release binaries** — Linux (x86_64, aarch64), macOS (arm64), Windows (x86_64)
- **INSTALL.md** — installation guide (binary download and build from source)
- **naab-gov CLI** — standalone governance linting and scanning tool

### Fixed
- **SQLite NULL column crash** — `safeColumnText()` guards all `sqlite3_column_text()` calls
- **Thread pool double-init race** — replaced double-check pattern with `std::call_once`
- **Module cache thread safety** — added `std::shared_mutex` for concurrent access
- **popen FILE* leak** — wrapped in RAII `unique_ptr<FILE, pclose>`
- **Governance telemetry file leak** — RAII wrapper for fopen/flock/fclose
- **Debugger source locations** — `CallFrame.source_location` now uses real file:line info

### Security
- 60+ security fixes across R5-R24 audit rounds
- Windows subprocess hardening (Job Objects, handle narrowing, Ctrl-C handler)
- Lockfile HMAC-SHA256 signatures, fail-closed verification
- REST API per-request timeout, body size limits, constant-time auth
- Environment variable injection blocking (LD_PRELOAD, PATH, etc.)

## [0.8.1] - 2026-04-03

### Fixed
- **Windows CI** — `vm.cpp` `OP_POLYGLOT` handler preamble/postamble now guarded with `#ifdef HAVE_PYBIND11`, fixing Python `-> JSON` blocks in subprocess mode (Windows/no pybind11)
- **Version sync** — all version strings across configs, docs, tests, and tooling synchronized to 0.8.1
- **CI hardening** — `ci.yml` now runs full `run-all-tests.sh` suite with pybind11 installed

## [0.8.0] - 2026-04-02

### Added
- **Windows portability** — full MSYS2/MinGW64 build support, `platform_win32.cpp`, `CreateProcess` subprocess executor, GitHub CI for Windows
- **LSP enhancements** — codeAction, workspaceSymbol, rename refactoring (Sprint 8.3)
- **Deterministic builds** — `naab.lock` lockfile via `--lock`/`--lock-check`/`--lock-path` flags (Sprint 8.4)
- **Enterprise governance** — exit codes (0-4), quality gates, CWE/OWASP tags, baselines, environment selector, runtime versions
- **New stdlib modules** — `log`, `uuid`, `validate`, `process`
- **libnaab** static library and `naab-gov` standalone CLI

## [0.7.0] - 2026-03-30

### Added
- **Bytecode VM** — stack-based bytecode compiler and virtual machine, now the default execution engine
  - Computed goto dispatch (GCC/Clang) for fast opcode handling
  - NaN-boxing integer fast paths for arithmetic without unboxing
  - Constant folding optimization pass in the compiler
  - 47 VM bugs fixed during development and hardening
  - Use `--tree-walk` flag to fall back to the legacy AST interpreter
- **Agent Governance Runtime** — multi-agent role enforcement for govern.json
  - `--agent-id <name>` CLI flag to identify the executing agent
  - `--governance-dashboard` flag for governance summary output to stderr
  - `agent_roles` section in govern.json for per-agent path and language restrictions
  - Telemetry JSONL output for agent execution tracking
- **Governance Plugins** — extend governance with custom NAAb-based check functions
  - `governance_plugins` array in govern.json references external .naab files
  - Each plugin defines rules with `id`, `function`, `trigger`, `level`, and help text
- **Package Manager** — dependency management for NAAb projects
- **Multi-Agent Integration Tests** — end-to-end tests for agent governance scenarios

### Changed
- VM is now the default execution engine (`global_use_vm = true`); tree-walker available via `--tree-walk`
- NaN-boxing migration complete — zero `fromLegacy()`/`toLegacy()` calls remain outside core naab_val.cpp/h

### Fixed
- 47 VM bugs across phases 1-16 (control flow, closures, polyglot dispatch, governance integration)
- Integer overflow in array indexing with large indices
- Parallel polyglot race conditions in VM execution path
- Gorilla test #12 fixes for edge-case governance interactions

### Performance
- VM 50ms vs Tree-walker 395ms (87% faster, ~8x speedup) on standard benchmarks
- NaN-boxing eliminates heap allocation for int/double/bool/null values

## [0.6.0] - 2026-03-21

### Added
- **f-string syntax** — `f"hello {name}"` as shorthand for `"hello ${name}"`
  - Bare `{expr}` in f-strings is equivalent to `${expr}` in regular strings
  - Regular strings still require `${}` — bare `{` is literal
  - Supports arbitrary expressions: `f"result: {2 + 3}"`
  - `${}` also works inside f-strings for backward compatibility
- 6 f-string tests in `test_stdlib_string.naab` (44 → 50 assertions)
- **Pattern matching enhancements** — guard clauses, binding patterns, array destructuring
  - Guard clauses: `n if n > 0 => "positive"` — condition checked after pattern match
  - Binding patterns: `other => f"got: {other}"` — identifier binds the subject value
  - Array destructuring: `[x, y] => f"point: {x}, {y}"` — match and destructure arrays
  - Literal array elements must match exactly: `[0, 0] => "origin"`
  - Combined: `[x, y] if x + y > 25 => "far"` — destructure + guard
- 9 match enhancement tests in `test_control_flow.naab` (39 → 48 assertions)
- **Interfaces / traits** — declare interface contracts and validate struct implementations
  - `interface Printable { fn to_string() -> string }` — define required method signatures
  - `struct Point implements Printable { ... }` — declare interface compliance
  - Multiple interfaces: `struct Box implements Printable, Measurable { ... }`
  - Method validation: missing methods produce clear error messages with examples
  - Duck typing via `fn StructName.method(instance)` free functions
  - `StructName.method(instance)` call syntax for method dispatch
- 10 interface tests in `test_interfaces.naab`
- **Generators / yield** — generator functions with lazy iteration via `yield` keyword
  - `yield value` inside a function makes it a generator (auto-detected)
  - `for x in generator_fn(args)` iterates over yielded values
  - Fibonacci, filtering, flattening patterns all work
  - `break` in for-in loop stops iteration early
  - Empty generators (no yields executed) produce zero iterations
  - Nested yield: generators can yield from nested loops
- 12 generator tests in `test_generators.naab`
- **Stdlib completion** — new functions across file, http, path, and io modules
  - `file.copy(src, dst)`, `file.move(src, dst)`, `file.size(path)` — file operations
  - `file.basename(path)`, `file.dirname(path)`, `file.extension(path)` — path info from file module
  - `http.head(url)`, `http.patch(url, body)` — additional HTTP methods
  - `io.input(prompt?)` — read line from stdin with optional prompt
  - New `path` module: `join`, `dirname`, `basename`, `extension`, `resolve`, `is_absolute`, `normalize`, `exists`
- **REPL** — interactive Read-Eval-Print Loop
  - `naab-lang` with no arguments starts the REPL; also `naab-lang --repl`
  - Expressions auto-print: `3 + 4` shows `7`
  - Variables, functions, structs persist across lines
  - Multi-line input: open braces/brackets continue to next line (`...` prompt)
  - Meta-commands: `.help`, `.clear` (reset environment), `.exit`
  - Top-level declarations (`fn`, `struct`, `enum`, `use`, `import`) work correctly
  - History saved to `~/.naab_history`
- **URL imports** — import NAAb modules directly from URLs
  - `import "https://example.com/lib.naab" as mylib` — download and cache
  - Cache stored in `~/.naab/cache/` with content-hashed filenames
  - Cached modules are reused across runs (no re-download)
  - Clear error messages on download failure

### Fixed
- **BUG-E1: Type checking bypass via `callFunction()`** — pipeline (`|>`), stdlib callbacks (`array.filter_fn`), lambda calls, and method dispatch now enforce parameter type annotations. Previously only direct `fn(args)` syntax checked types.
- **BUG-E1b: Generator parameter type bypass** — generator functions now validate parameter types at creation time, before eager body execution.
- **BUG-E2: Generator exception safety** — if a generator body throws, interpreter state (`active_generator_`, `current_env_`, `returning_`) is now properly restored via try/catch. Previously, an exception mid-generation corrupted state for subsequent operations.

### Hardening
- **229 new edge-case assertions** across 9 test files targeting bypass paths, state consistency, and cross-feature interactions:
  - `test_type_enforcement.naab` (41 tests) — variable/param/return type checking, nullable, union types, struct types, error messages
  - `test_type_bypass.naab` (25 tests) — pipeline, lambda, callback, method dispatch type enforcement (BUG-E1 verification)
  - `test_generator_state.naab` (17 tests) — exception recovery, nested generators, reuse independence, break/continue state
  - `test_interface_invariants.naab` (25 tests) — multi-struct, multi-interface, error paths, method as callback, f-string interaction
  - `test_match_hardened.naab` (30 tests) — guard evaluation order, destructuring edge cases, guard+destructure combos, nested match
  - `test_fstring_hardened.naab` (26 tests) — expression types, special values, mixed syntax, complex interpolation chains
  - `test_stdlib_path.naab` (30 tests) — all 8 path functions, edge cases (root, hidden files, no extension), error handling
  - `test_stdlib_file_new.naab` (20 tests) — copy/move/size/basename/dirname/extension with real filesystem operations
  - `test_v060_interactions.naab` (15 tests) — typed generators, interface+generator, match+generator, f-string everywhere, full feature pipeline

## [0.5.3] - 2026-03-21

### Added
- **Optional chaining (`?.`)** — safely access properties/methods of potentially null objects
  - `user?.name` returns null if user is null, otherwise returns `user.name`
  - `obj?.method()` returns null if obj is null, otherwise calls the method
  - Chains: `a?.b?.c` propagates null through the chain
  - Combines with `??`: `user?.name ?? "default"` for fallback values
  - Works with structs, dicts, arrays, strings — any member/method access
- 9 optional chaining tests in `test_structs_enums.naab` (25 → 34 assertions)
- **Destructuring assignment** — extract values from arrays and dicts into variables
  - Array: `let [a, b, c] = [1, 2, 3]` — positional extraction
  - Dict: `let {name, age} = get_user()` — key-based extraction
  - Partial: extra array elements ignored, missing dict keys become null
  - Value semantics preserved (destructured values are copies)
  - Taint propagation supported for governance
- 8 destructuring tests in `test_closures_scope.naab` (30 → 38 assertions)
- **Spread/rest operator (`...`)** in array destructuring
  - `let [first, ...rest] = [1, 2, 3, 4]` — `rest` becomes `[2, 3, 4]`
  - `...rest` must be the last element in the pattern
  - Empty rest produces `[]` when array is exactly consumed
  - Value semantics preserved (rest array is an independent copy)
- 5 spread tests added to `test_closures_scope.naab` (38 → 43 assertions)
- **For loop destructuring** — iterate with pattern matching
  - `for [key, val] in dict { }` — destructure key-value pairs from dicts
  - `for [a, b, c] in array_of_arrays { }` — destructure nested arrays
  - `for [first, ...rest] in rows { }` — spread in loop patterns
- 5 for-loop destructuring tests in `test_control_flow.naab` (34 → 39 assertions)
- **Dict dot-methods `entries()` and `merge()`** — added to primary CallExpr dispatch path
  - `d.entries()` returns array of `[key, value]` pairs
  - `d.merge(other)` merges another dict into `d`, overwriting existing keys
  - `for [k, v] in d.entries() { }` — full destructuring support
- 6 dict method tests in `test_structs_enums.naab` (34 → 40 assertions)
- **Null coalescing assignment (`??=`)** — assign only if variable is currently null
  - `x ??= "default"` assigns `"default"` only when `x` is null
  - Falsy values (`false`, `0`, `""`) are NOT null — assignment is skipped
  - Supports variables and member access targets (`obj.field ??= val`)
  - Desugars to `x = x ?? val` — no new AST node needed
- 6 nullish assignment tests in `test_operators_matrix.naab` (62 → 68 assertions)
- **Compound subscript assignment** — `arr[i] += val`, `dict["key"] -= val`
  - All 5 compound operators work: `+=`, `-=`, `*=`, `/=`, `%=`
  - `??=` also works on subscripts: `arr[0] ??= "default"`
  - Supports both literal and variable indices
- 7 compound subscript tests in `test_operators_matrix.naab` (68 → 75 assertions)
- **`in` containment operator** — check membership in dicts, arrays, and strings
  - `if "key" in dict { }` — check if key exists in dict
  - `if item in array { }` — check if item is in array
  - `if "sub" in string { }` — check if substring exists
  - Returns boolean, works with `!` for negation: `if !("x" in d) { }`
- 8 `in` operator tests in `test_operators_matrix.naab` (75 → 83 assertions)
- **String repetition** — `"abc" * 3` → `"abcabcabc"`
  - Commutative: `3 * "abc"` also works
  - `"x" * 0` → `""`, `"" * 5` → `""`
- 5 string repeat tests in `test_operators_matrix.naab` (83 → 88 assertions)
- **`not in` operator** — readable negation of containment check
  - `if 99 not in arr { }` — equivalent to `!(99 in arr)` but more readable
  - Works with arrays, dicts, and strings
  - Desugars to `UnaryExpr(Not, BinaryExpr(In, ...))` at parse time
- 6 `not in` tests in `test_operators_matrix.naab` (88 → 94 assertions)
- **Array/string slicing** — `arr[start:end]` syntax for extracting sub-sequences
  - `arr[1:3]` returns elements at indices 1 and 2 (end exclusive)
  - `str[0:5]` returns substring from index 0 to 4
  - Negative indices: `arr[-2:5]` counts from end
  - Empty slice when start >= end: `arr[3:3]` → `[]`
  - Desugars to `__slice(collection, start, end)` builtin
- 8 slicing tests in `test_operators_matrix.naab` (94 → 102 assertions)

## [0.5.2] - 2026-03-21

### Added
- **Exhaustive Robustness Test Suite** — 8 test files, 308 assertions covering core language correctness
  - `test_stdlib_array.naab` (40) — all 16 array functions + empty/error edge cases
  - `test_stdlib_string.naab` (44) — all 19 string functions + format/fmt + boundary/error cases
  - `test_stdlib_math_json.naab` (49) — math (15), json (all 6), regex (all 12) including advanced
  - `test_operators_matrix.naab` (62) — all 17 operators × type matrix, truthiness, ??, |>, string comparison
  - `test_closures_scope.naab` (30) — capture, factory, nested, shadow, IIFE, higher-order
  - `test_control_flow.naab` (34) — nested break/continue, return in loops/try, match, empty iter
  - `test_structs_enums.naab` (25) — struct create/modify, enum match, value semantics
  - `test_stdlib_env_time.naab` (33) — env (all 12 fns), time (all 12 fns) including parse_datetime
- **Meta-Test Validation Suite** — 5-layer system proving test assertions are genuine
  - Layer 1: Static integrity audit (7 checks: no trivial assertions, balanced counts, no orphans)
  - Layer 2: Mutation testing — 45 deliberately wrong assertions, all correctly detected
  - Layer 3: Sensitivity testing — 33 tests proving outputs depend on inputs
  - Layer 4: Coverage verification — cross-references stdlib C++ source against test usage (100% stdlib, 17/17 operators, 11/11 patterns)
  - Layer 5: Runtime count verification — manifest-based output validation for all 8 test files
  - `run_meta_tests.sh` — master runner for all 5 layers
- **Loop iteration governance enforcement** — `checkLoopIterations()` wired into all 5 loop paths (range-inclusive, range-exclusive, dict, list, while)
- Robustness tests integrated into `run-all-tests.sh` with governance enabled

### Fixed
- **String lexicographic comparison** — `<`, `<=`, `>`, `>=` now compare strings lexicographically instead of converting to float (which always returned false)
- NAAb division always returns float (10/3 = 3.333, not 3) — test expectations corrected
- Functions without explicit return yield last evaluated value, not null — test corrected

### Changed
- Test suite: 340 → 358 tests (0 unexpected failures)
- Stdlib coverage: 75% → 100% (92/92 functions tested, all 7 modules at FULL coverage)
- Robustness suite: 86 → 477 assertions (86 original + 313 exhaustive + 45 mutations + 33 sensitivity)

## [0.5.1] - 2026-03-20

### Fixed
- Float serialization in polyglot bindings — `serializeValueForLanguage()` used `std::to_string()` (6 decimals), now uses `%.15g` matching `Value::toString()` (FIX-1)
- Dict key escaping in polyglot serialization — keys with `"`, `\`, `\n` now properly escaped across all 7 language paths (FIX-2)
- Subscript assignment taint propagation — `dict["k"] = tainted` and `arr[0] = tainted` now mark container as tainted (FIX-3)
- Struct field assignment taint propagation — `obj.field = tainted` now marks struct as tainted (FIX-4)
- Dot-notation method mutation taint — `arr.push(tainted)`, `dict.put("k", tainted)` now propagate taint at 4 code locations (FIX-5)
- Stale mono test assertion — float concatenation test updated from `"3.140000"` to `"3.14"` (FIX-6)
- Documentation: math.pi/math.e gotcha removed (both cases work), float format gotcha updated
- Catch-throw taint restore — exception paths in catch blocks now restore outer taint state (BUG-1)
- ForStmt taint from sources — `for item in env.get("PATH")` now marks loop variable as tainted (BUG-2)
- ReturnStmt taint collapsed to unified `checkRhsTainted()` helper — 33 lines to 3 (BUG-3)
- Parallel polyglot all-language taint — was shell-only, now checks python_exec, go_exec, etc. (BUG-4)
- `checkRhsTainted()` nested CallExpr guard removed + consume-once semantics — `let x = arr[get_tainted()]` detects inner return taint without false-positiving on subsequent literals (BUG-5 + FIX-A)
- Deduplicated polyglot sink-type mapping into `checkPolyglotBoundVarTaint()` helper (FIX-D)

### Changed
- Governance regression suite: 319 → 339 tests (split categories: passes, error behavior, missing executor)
- Mono exhaustive test: 292 → 293 PASS (0 FAIL)
- Test runner categories split: error behavior tests vs missing executor tests vs skip files
- Real taint e2e integration test added (12 scenarios exercising full taint pipeline)

## [0.5.0] - 2026-03-19

### Added
- **Advisory Noise Reduction** — governance output reduced from 100+ lines to 3-5
  - Grouped duplicate call warnings (DX-6): deferred, deduplicated, configurable threshold
  - Grouped polyglot try/catch warnings (DX-7): compact multi-function summary
  - `emitAdvisory()` centralized output with `output.max_advisories` cap (default 15)
  - Suppression summary: "... and N more advisories suppressed"
- **Configurable advisory thresholds** in govern.json:
  - `code_quality.duplicate_calls` — enabled, threshold (default 3), max_entries (default 5)
  - `code_quality.polyglot_try_catch` — enabled, max_entries (default 3)
  - `output.max_advisories` (default 15, 0 = unlimited), `output.advisory_summary`
- **15 DX Governance Improvements** (DX-1 through DX-15):
  - Taint matching changed from substring to prefix (DX-1)
  - All-language polyglot binding taint checks — python_exec, go_exec, etc. (DX-2)
  - MemberExpr sanitizer detection — module.sanitize_foo() clears taint (DX-3)
  - Reserved name warnings — result_, returning_, etc. (DX-4)
  - Unused binding detection within same scope block (DX-5)
  - Duplicate call detection in function bodies (DX-6)
  - Try/catch polyglot advisory warning (DX-7)
  - Scope pattern validation with glob matching (DX-8)
  - Nim JSON error helper for -> JSON blocks (DX-9)
  - Type-safe binding hints for Go/Nim complex types (DX-10)
  - Cross-language hallucination detection — +5 Nim, +3 Go patterns (DX-11)
  - Per-language polyglot_output taint — "polyglot_output:python" syntax (DX-12)
  - JSON roundtrip waste detection (DX-13)
  - Missing executor install guidance per language (DX-14)
  - govern.json schema validation with sanitizer false positive warnings (DX-15)
- **Exhaustive Governance Test Suite** — 197 edge-case tests across 19 files
  - 10 categories: sinks, sources, expressions, async, modules, degraded, recursive, polyglot, interactions, docs
  - Chaos testing: taint washing, silent swallow, direct bypass, concurrent taint, GC stress
- **C++ Native Scanner** (`naab-lang --scan`) — 139 checks across 11 source files
  - 6 categories: redundancy, code_quality, complexity, style, security, language-specific
  - Language modules: Python (14), JavaScript (12), C++ (12), Go (9), Rust (10), NAAb (11)
  - Text + JSON + SARIF output, govern.json configurable
- **GC Cycle Detector Fix** (BUG-10) — env_stack_ tracks all live environments during GC

### Fixed
- Optimization hints verbose 10-line block printed at all enforcement levels — now only for "hard"
- MatchExpr taint propagation — match arms now checked via expressionContainsTaint
- AwaitExpr taint propagation — async return taint chain fixed with lastReturnWasTainted check
- 7 critical review followup fixes: assignment/return/for-stmt await paths, dead code removal
- Polyglot return in functions (BUG-1) — returning_ flag checked in CompoundStmt loop

### Changed
- govern-template.json synced to v4.0 (was v3.0 at root level)
- Governance regression suite: 208 → 339 tests (100% pass rate)

## [0.4.0] - 2026-03-18

### Added
- **Governance v4.0 — Taint Tracking** — variable-level tracking of untrusted data from
  sources (env.get, io.read_line, file.read, polyglot_output) through to sinks (shell_exec,
  file.write, http.*, env.set_var) with configurable enforcement (hard/soft/advisory)
  - Propagation through string concatenation, interpolation, function returns, for-loop iterators
  - Sanitizer functions clear taint (validate_*, sanitize_*, escape_*, int(), float())
  - Name collision fix: taint cleared on variable redeclaration with clean value
  - Thread-safe taint_set_ with mutex for async/parallel execution
  - Taint state saved/restored around module loading to prevent cross-module leaks
- **Cross-Module Input Contracts** — parameter type validation at function call sites
  via govern.json `contracts.functions.*.params` (format: `"name:type"`)
  - `validate_inputs: true` enables input contract checking
  - Checked on both callFunction() and direct call dispatch paths
- **Enhanced Audit Trail** — JSONL logging with tamper-evident hash chains
  - `polyglot_timing`: execution duration per polyglot block
  - `taint_decisions`: every taint mark/clear/block decision logged
  - `contract_checks`: every contract pass/fail logged with details
  - Parallel polyglot blocks now logged in audit trail
- **Dead Conditional Scanner** — regex-based detection for impossible conditions
  - `len(x) < 0`, contradictory comparisons, null access after null check, type impossibilities
  - Data flow analysis: range tracking from math.max/min/clamp/for-loops
- **Configurable Scanner** — govern.json `scanner` section with per-check enable/disable
- **17 Edge Case Bugs Fixed** during exhaustive hardening:
  - Assignment path taint tracking (BUG-A)
  - Expression tree taint propagation for string concat/interpolation (BUG-B/C)
  - Function return taint via lastReturnTainted flag (BUG-D)
  - For-loop iterator taint from tainted iterables (BUG-E)
  - HTTP URL taint sink check / SSRF prevention (BUG-F)
  - env.set_var() taint sink check (BUG-G)
  - Async interpreter governance propagation (BUG-I)
  - Parallel polyglot bound variable taint check (BUG-J)
  - Await resolution return contract check (BUG-K)
  - taint_set_ mutex for thread safety (BUG-N)
  - Module loading taint state isolation (BUG-O)
  - Parallel polyglot audit logging (BUG-P)
  - Direct call path contract audit logging (BUG-Q)

### Changed
- govern.json template updated with v4.0 sections (taint_tracking, enhanced contracts, audit)
- FutureValue now stores func_name for return contract checking at await resolution

### Tests
- 49 governance v4 tests across 12 test files (all passing)
- 208/208 regression suite (100% pass rate)

## [0.3.1] - 2026-03-05

### Added
- **Project Context Awareness** — governance reads project files to supplement `govern.json`
  - Layer 1: LLM instruction files (CLAUDE.md, .cursorrules, copilot-instructions.md, gemini.md)
  - Layer 2: Linter/formatter configs (.editorconfig, .eslintrc, .prettierrc, biome.json)
  - Layer 3: Package manifests (package.json, go.mod, Cargo.toml, pyproject.toml)
  - Opt-in (`"project_context": { "enabled": true }` in govern.json), off by default
  - govern.json always overrides — project context supplements, never conflicts
  - Each layer independently toggleable, dry-run mode, surgical rule suppression
  - Extracted language preferences feed into polyglot optimization scoring
  - Manifest detection is advisory-only — never blocks new languages
  - Full extraction report with source file + line number for every rule
  - 29 edge case tests covering conflicts, priority_source, code block skipping, and more
- Nim, Zig, Julia polyglot executors (3 new languages, 15 total)
- Governance v3.0 — 50+ checks, 13 config sections, per-language rules, custom rules engine
- Polyglot optimization system — 7 detection layers, 205+ patterns, task-language scoring
- Empirical profiling and calibration for governance suggestions
- Cross-language consensus verification

## [0.2.0] - 2025-02-24

### Added
- Pattern matching with `match` expressions
- Async/await support for concurrent function execution
- Dedicated Go executor for polyglot blocks
- REPL tab completion (keywords, stdlib, user symbols, dot notation)
- Lambda expressions with closure capture
- If expressions (`let x = if cond { a } else { b }`)
- Parallel polyglot execution with dependency analysis
- Persistent sub-runtime contexts (`runtime` keyword)
- JSON sovereign pipe (`naab_return()`, `-> JSON` header)
- Native data binding for polyglot blocks (Ruby, PHP, compiled langs)
- Debug module (`debug.inspect`, `debug.type`)
- "Did you mean?" suggestions with fuzzy matching
- Common mistake helper errors (~35 patterns)
- Polyglot error mapping with source location
- Polyglot timeout configuration
- CI workflows: Build & Test, Sanitizers (ASan/UBSan), CodeQL, Supply Chain, Release

### Fixed
- Pipeline operator evaluates right side lazily
- Polyglot `>>` delimiter only closes at line start
- JavaScript type conversion in polyglot blocks
- Python large integer handling
- Python indentation preservation in polyglot blocks
- Thread-safe temp files for C++, Rust, C# executors
- Copy-on-assignment semantics for arrays and dicts
- Break/continue validation outside loops
- Assignment-in-condition detection

## [0.1.0] - 2024-12-01

### Added
- Core language: variables, functions, control flow, loops
- Polyglot block execution (Python, JavaScript, Rust, C++, C#, Shell, Ruby, PHP)
- Standard library: array, string, math, time, env, file, io, json, http, csv, regex, crypto
- REPL with linenoise (arrow keys, history, Ctrl+R search)
- Block registry with SQLite storage
- Error reporting with source locations
