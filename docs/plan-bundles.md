# NAAb bundle plan

The project owner's ordered plan for governance work. It was recorded on 2026-10-30, and its statuses were brought up to date on 2026-10-31.

A **bundle** is one branch, one pull request and one validated push. **Confirm a bundle's scope with the owner before starting it.** A finding made along the way is recorded in `docs/open-investigations.md` in the same pull request, and is added to a bundle only by the owner's decision.

## Status (2026-11-10)

| Bundle | State | Where |
|---|---|---|
| 1-5 | merged | git log, CHANGELOG |
| 6: crash-proof governor, then the planned defaults | merged | #347 (`41a3b3c3`) |
| 7: release | merged | #348 (`ca07ae12`) tagged v2.0.0; its macOS release build failed, so #349 (`735fa217`) released 2.0.1 |
| 8: "governance only tightens", proved | merged: PR 1 #352 (`76c871cf`), PR 2 #353 (`fc9b8099`). Follow-up merged, #364 (`314bccfd`): A95(b), A98 | Bundle 8 below |
| 9: adversarial inputs and concurrency | merged | #354 (`7a4c1c50`); A108-A112 |
| 10: state machines and automata | merged | #355 (`f9337bdf`); A113 |
| 11: contracts and manifests for LLM-written code | merged, two PRs by the owner's decision (2026-11-01). PR 1 #356 (`703fa6c4`): requires/ensures as NAAb expressions checked by the VM at the call boundary, and the opt-in counterexample search `naab-gov contracts` in a worker; A114. PR 2 #357 (`528fbb59`): the signed capability manifest; A115, A116 | Bundle 11 below |
| 12: taint as information flow | merged (the control-flow bar waits on Bundle 13); two PRs by the owner's decision (2026-11-04), then follow-ups. PR 1 (merged, #358): one taint semantics -- one static rule both AST walks share, unknown kinds fail closed, a suite generated from the semantics table; A117. PR 2 (merged, #360): the static noninterference check, an opt-in report, `naab-gov taint`; measured, A120-A122. Follow-up PR 1 (merged, #361, `01fb3552`): A120 and A121(d) fixed, A122(b), A123 and A124 recorded. PR 3 merged, #363 (`169d6600`): A121(a)-(c), A124, A127, A129. The control-flow bar waits on Bundle 13 | Bundle 12 below |
| 16: honest tests and the engine's own child processes | merged, #362 (`977e7a40`): A123 (13 of 14 tests and the guard; one pinned for Bundle 12 PR 3), A125, A122, A99, A100 fixed; A126 recorded for the owner | Bundle 16 below |
| 17: what Gemini's SentinelGrid report found | merged, #365 (`795b5885`), one PR, scope by the owner's decisions of 2026-11-09: A130, A131, A133-A139, A141, A142 fixed; A132, A140, A143, A144 and F-14 recorded | Bundle 17 below |
| 13: measure what governance catches; help operators | in progress, three PRs by the owner's decision (2026-11-10). PR 1 built: `naab-gov diff`; A147 recorded | Bundle 13 below |
| 14: replay and provenance | in progress, two PRs by the owner's decision (2026-11-11). PR 1 built, #369: provenance for generated code; A160-A162 recorded | Bundle 14 below |
| 15 | not started; order 17, 13, 14, 15 (the owner, 2026-11-09) | |

Outside the bundles: A92 (the cycle collector cleared lists another engine was using) was fixed in #350 (`96a129b1`). A145 and A146 (node 24: the child memory budget, TypeScript's flags, and the runtime-pin message) were fixed in #366 (`97dab417`). The bundle plan itself was recorded in #351 (`86fc33c9`).

Bundle 8 is two PRs, by the owner's decision of 2026-10-31: PR 1 holds parts 1 and 2 (the `naab-gov ratchet` seam, the property harness, the same property for the overlays, and the CI job); PR 2 classifies the unclassified ratchet keys. The harness measures `extends` without asserting it, because a child can loosen its parent under both merge strategies today: A98 is the owner's decision. Progress and findings are under Bundle 8 below.

## Standing rules (every bundle)

- **Never loosen governance or add an escape route.**
  - The ratchet only tightens.
  - `govern.json` is primary.
  - Fail closed.
- **Do the history pass before any change** (`.claude/skills/investigation-gates`, `docs/investigation-method.md`). Close whole classes, not single instances.
- **Test red first, with controls.**
  - Revert-check every mutant, and keep a kill table in each suite header.
  - Report PASS / FAIL / UNMEASURABLE.
- **Never write literal secrets or PII**; build them at runtime from fragments.
- **No force-push** (use merge commits). **No model identifiers** in commits or PRs.
- **One validated push per bundle.**
  - Match every CI result to its head SHA.
  - Merge on the owner's go.
  - Restart the branch from master after a merge.
- **Record every finding in `docs/open-investigations.md`**, in the same PR.
- **Before trusting a master-vs-branch comparison, check how each suite takes its binary:** the first argument, the `NAAB` variable, or a hard-coded `build/naab-lang`.

## Bundle 6 -- crash-proof governor, then the planned defaults (merged, #347)

- **A89 first:** every `std::regex` over text the governed side controls was to move to a matcher that does not recurse per character. That text is:
  - model replies and tool results;
  - program text;
  - `output_contract.regex_checks`;
  - program-supplied patterns (orchestra, the regex module);
  - the 11 config-built sites.
  - Shipped as `naab::rx`: RE2 where it reads a pattern exactly as before, and QuickJS libregexp otherwise.
- A built-in BSD pattern: env -> encode -> `agent.send` | `http.post`.
- `default_pattern_enforcement: "declared"` became the default.
- `no_secrets` is on by default for agent replies.
- A migration note, docs, and claims-suite updates.

## Bundle 7 -- release (merged; tag only on the owner's go)

## Bundle 8 -- "governance only tightens", proved

- **Seam: `naab-gov ratchet OLD NEW`.** Today the reload ratchet is private to `governance_config.cpp`.
- **Property harness:**
  - generate config pairs over small alphabets (paths, env names, hosts, levels, modes);
  - for every pair the ratchet accepts, no probe request may become allowed, judged by the engine's own decision functions;
  - a failure prints the counterexample (the config pair and the request).
- **The same property for:**
  - `extends` merge;
  - CLI flag overlays;
  - lease grants;
  - role overlays;
  - startup overlays at reload.
- **A CI job runs it**, the consumer #240 lacked.
- **Optional:** an offline Z3 proof of the per-list probe-completeness lemmas.
- **Then classify the unclassified ratchet keys** (773 in `tests/governance_v4/ratchet_unclassified_baseline.txt`, counted 2026-10-31), declaring an order per key.

**Progress (2026-11-01).** Both PRs are merged: PR 1 #352 (`76c871cf`), PR 2 #353 (`fc9b8099`).

| Part | State |
|---|---|
| Seam | built: `naab-gov ratchet OLD NEW` calls `GovernanceEngine::evaluateReload()`, the decision `reloadIfChanged()` itself makes, after starting OLD as a run does (role, environment including `governance.default_env`, lease) |
| Property harness | built: `naab_ratchet_property` (`tests/property/ratchet_property.cpp`), suite `tests/governance_v4/test_ratchet_property.sh` |
| `extends` merge | measured, not asserted (A98, below); only the mode floor is asserted |
| CLI flag overlays | asserted, exhaustively: sandbox level, timeout, memory limit (`include/naab/run_limits.h`) |
| Lease grants, role overlays | asserted, with liveness controls (a role and a lease must each change some verdict) |
| Startup overlays at reload | asserted for role, environment and lease (RG-07 checks each one reaches the seam's decision) |
| CI job | built: a "Ratchet property" step in Build & Test, a new seed each run |
| Z3 proof (optional) | not started |
| PR 2: classify the 773 keys | merged (#353): every key classified (the unclassified baseline is empty); 475 declared orders compiled into the engine and enforced by `evaluateReload()` (`checkDeclaredOrderRatchet()`); unclear keys `fixed`, by the owner's decision (2026-10-31); suite `tests/governance_v4/test_declared_order_ratchet.sh` |

**What PR 1 found.** Each finding has a row in `docs/open-investigations.md`.
- **Fixed in PR 1**, and every fix only tightens:
  - A94: a misspelled `sandbox_level` lifted the enforce clamp;
  - A95(a)(c): an environment's mode did not change whether governance was active;
  - A96: two loosening reloads were accepted (child-process `blocked_read`, `detect` -> `soft`);
  - A97: a lease dropped the project's subprocess blocklist;
  - A100: an `agents` list was ignored without a warning;
  - A101: removing a role's last ENV action was read as a tightening;
  - A102: deleting a role's `network_allowed: false` re-granted the network.
- **Open, waiting on the owner** (none is added to a bundle without the owner's decision):
  - A98: what `extends` should promise. A child can loosen its parent under both merge strategies, `parent_wins` included;
  - A95(b): whether an environment with `mode: off` should switch governance off. Today governance stays on and blocking, so switching it off would loosen today's behaviour;
  - A100: whether a non-object `agents` block should be refused (exit 4) instead of warned about;
  - A99: embedded Python's allowlist scrub keeps `LD_LIBRARY_PATH`, which every other child's scrub withholds. It is traced, not measured. Removing it could break Python's own subprocesses on Termux, so it waits for a measurement there.

**Follow-up decided (the owner, 2026-11-06)**, as its own PR after Bundle 16 and Bundle 12 PR 3. It completes the part this bundle left measured but not asserted, "the same property for `extends` merge":
- **A95(b): an environment may raise the mode, never lower it.** A lower mode in an environment is ignored, with a load warning that names it. Over an enforce file nothing changes (the run still blocks); over an audit file the run gets audit, instead of today's accidental blocking. No tracked config sets an environment's mode below its file's (observed 2026-11-06: none of 404 tracked JSON files sets one to `off`).
- **A98: the `extends` floor holds for security settings under both strategies**, `parent_wins` and `child_wins`: blocked lists unioned, allowlists intersected (an empty intersection refused at load, never read as unrestricted), switches allowed only when both allow, the stricter level by refusal rank. A child may still tighten, and still change settings that are not security settings. The property harness then ASSERTS the `extends` property instead of measuring it. All 6 tracked `extends` configs use `child_wins` with `replace` (observed 2026-11-06), so this is the case that changes them.
- A99 and A100 moved to Bundle 16.

**Progress (2026-11-09).** The follow-up is built. A95(b): an environment's mode applies only when stricter than the file's, one rule (`modeAfterEnvironment()`) for startup, every reload, the `--tree-walk` refusal and the REPL gate. Wider than the decision's note said: an environment at `audit` over an enforce file DID lower it (observed), and is now refused too. A98: `applyExtendsFloor()` after every merge; the property harness found two loosenings the first version left (a child's allowed path inside its parent's blocked path; a parent's advisory PII check raised to soft at load, compared as advisory) and asserts `extends` now. One finding on the way, recorded for the owner's decision: A130 (an environment can still switch the quality gate and the governance baseline off).

**What PR 2 found.** Each finding has a row in `docs/open-investigations.md`.
- **Fixed in PR 2**, each tightening only:
  - A103: the codegen ratchet read 0 (no cap) as the tightest value of five caps, and the 0 timeout as 0 s instead of the 15 s fallback;
  - A106: a reload the ratchet refused still raised the JSON depth limit, which the loader sets process-wide while it parses;
  - A104: the 773 keys themselves. Most were live settings a signed reload could loosen: run budgets, the `limits.array_size` / `call_depth` aliases, `context_drift.enabled`, `restrictions.polyglot_output`, presence-enabled check blocks, breaker and exposure thresholds, and an environment's `quality_gate.enabled`. On master the probe's loosenings were accepted for 463 of 475 declared keys.
- **Open, waiting on the owner:**
  - A105: a non-empty `custom_patterns` on a simple code_quality check replaces its built-in patterns (observed: one added pattern let a `# for now` block run). The reload side is closed (the lists are `fixed`); making `custom_patterns` append at startup is a behaviour change to schedule.

## Bundle 9 -- adversarial inputs and concurrency

- **Model-channel fuzzer**, through `tests/helpers/agent_stub.py`:
  - replies and tool calls: huge, empty, malformed JSON, odd encodings, broken call/result pairing;
  - the invariant: never a crash or hang; exit 0, 3 or 4, with evidence written.
- **Config fuzzer:** a random `govern.json` loads or exits 4. It never crashes, and never loads weaker than written.
- **Concurrency:**
  - a ThreadSanitizer CI job over the async, agent-batch and polyglot-concurrency suites;
  - fix A86: the report writers read `check_results_` without its lock;
  - **A93** (added 2026-10-31 by the owner's decision): `agent.batch` may run two tools in the main VM at once.
    - Reproduce it first.
    - The result decides the fix: a lock around the tool call, or each tool run on its own VM.
    - The ThreadSanitizer job guards the fix.
- **Schedule exploration**, if ThreadSanitizer is quiet.

**Progress (2026-11-01, one PR).** Each finding has a row in `docs/open-investigations.md`.
- **A93** reproduced first: two batched tools on one VM stack gave wrong results and a null batch result, and an `async fn`'s tool ran on main's stack while main executed. The reproduction decided the fix: a tool runs on the VM its agent call came from, and `agent.batch` LENDS its VM to the workers, which enter it one at a time. A VM per tool was rejected: it drops the dispatcher's frames, which loosens function-scope capabilities (`test_tool_thread_vm.sh` TV-03). With the loan's lock removed, the TSan build reports 143 data races in one run of that suite, so the job guards the fix.
- **A86** fixed: the report writers, telemetry and the post-run audits read a copy of `check_results_` taken under its lock. A unit test is the instrument; a CLI probe did not reproduce it in 20 runs.
- **The ThreadSanitizer job** (`tests/tsan/run_tsan.sh`, CI "ThreadSanitizer (concurrency suites)") runs the suites in `tests/tsan/suites.txt` and the matching unit tests. A planted race is its positive control. Its first run found **A108** (the calibration flag published before the data) and **A109** (`gmtime()` in two loggers), both fixed; it is quiet since.
- **The model-channel fuzzer** (`tests/fuzzing/model_channel`) found **A110**: negative token counts un-spent the run budget, and counts past `int`'s range wrapped. Fixed by clamping at the parse sites.
- **The config fuzzer** (`tests/fuzzing/config`) found **A111**: a capability restriction in a type its parser does not read loaded without the restriction (27 of 200 seeds). Fixed by refusing such a config (exit 4). It reaches only capability restrictions, so other sections are unmeasured.
- **Schedule exploration** is perturbation, not enumeration (no `rr` here): `tests/tsan/schedule_perturb.sh` reruns the same suites pinned to one and two CPUs. Report-only. Its first run (3 rounds) found **A112**, a defect in the agent stub rather than the engine: CK-11 failed in 1 of 3 one-CPU rounds because the stub logged a request after replying. Fixed, with `test_stub_routing.sh` SR-08 as its guard.

## Bundle 10 -- state machines and automata

- **BSD pattern emptiness check at load** ("can this pattern ever fire?"), plus subsumption.
- **Bounded exhaustive exploration of the governance state machines**, against the real engine via the stub:
  - level escalation and de-escalation;
  - pulse;
  - quarantine streak;
  - lease;
  - propose/commit.
- **Safety properties**, for example:
  - at CRITICAL no commit lands;
  - a calm sibling never de-escalates another handle's level;
  - an undetermined hold never advances the streak.

**Progress (A113, this PR).** The history pass over the matcher found four ways a completed pattern went unenforced (owner approved fixing all four): one event completed at most one pattern (an advisory listed first suppressed a hard block); the pre-check skipped single-step patterns; and a stalled partial kept its oldest start. Fixed in `behavioral_sequence.cpp` -- every pattern advanced per event, strictest enforced, single-step pre-checked, a bounded partial list re-anchoring a stall, matching still non-overlapping (`test_bsd_enforcement_completeness.sh`, 3 mutants). The **emptiness + subsumption** lint is a report-only load warning, sound by construction (`governance_config.cpp`, `test_bsd_pattern_lint.sh`). The **bounded explorer** is `tools/governance_explore/explore.py` (report-only; depth 4 = 173 orderings, clean), checking no-commit-at-CRITICAL and no-de-escalation; `test_governance_statemachine.sh` is the depth-3 CI smoke. Pulse, quarantine-streak and lease safety properties already have dedicated suites (`test_undetermined_gate.sh` Group E, `test_propose_commit.sh` Group J); the explorer covers the orderings they do not.

## Bundle 11 -- contracts and manifests for LLM-written code

- **`contracts.<fn>.requires` / `ensures`** in `govern.json`, checked at VM call boundaries at the configured level.
- **Counterexample search in `naab-gov check`:**
  - generate inputs that satisfy `requires`;
  - report one that breaks `ensures`;
  - use a deterministic seed;
  - Z3 later, for pure integer functions.
- **A signed capability manifest per program:**
  - static effect inference: languages, effects, hosts, files, agents;
  - approved by a person, enforced at runtime as an allowlist;
  - polyglot code stays bounded by the sandbox only (CONTRA-013), and the manifest says so.

**Decisions (the owner, 2026-11-01):** two PRs -- requires/ensures plus the counterexample search, then the manifest. Clauses are **NAAb expressions** (not a declarative form), compiled and run by the VM. The search is **opt-in and runs in a worker** (`naab-gov contracts FILE`, a `naab-lang run --contract-search` worker under the project's govern.json), so `naab-gov check` stays static. The manifest is **a signed sidecar plus a govern.json switch**: `naab-gov manifest infer FILE` drafts `<program>.manifest.json`, a person signs it with a trust-store key, `manifest.required: true` refuses a run whose manifest is missing, unsigned or for other program text, and the approved manifest narrows the run like a lease. Two more (2026-11-02): the manifest **pins every imported NAAb module** by path and SHA-256 (an unlisted or changed module is refused at import), and it is read **only under `manifest.required`** (a manifest beside a program that does not require one changes nothing).

## Bundle 12 -- taint as information flow

- **One taint semantics.** Unknown AST nodes fail CLOSED; today `expressionContainsTaint()` returns false for them.
- **A static information-flow check** (noninterference) for the NAAb fragment.
- **Collapse the two implementations**, or generate the parity tests from the semantics.

**Decisions (the owner, 2026-11-04):** two PRs -- the one semantics with generated parity tests, then the static check. The implementations are **kept and pinned, not collapsed**: the parity tests are generated from one semantics table. There turned out to be **three** implementations, not two: the tree-walker's AST walk, the VM compiler's compile-time AST walk (`Compiler::exprContainsTaint()`, which marks values with `OP_GOV_TAINT_MARK`) and the VM's runtime taint stack; the two AST walks **share one rule** (`include/naab/taint_rule.h`). **Conditions count**: a tainted if-condition, match subject, pattern or guard taints the result, on both engines (the VM's compile-time walk already counted an if-condition; nothing loosens). The VM's **stale return flag** (every clean stdlib call after a taint source read as tainted) **is fixed**. The static check (PR 2) is an **opt-in report** first, `naab-gov taint FILE`: nothing is refused at run time until its false-positive rate has been measured.

**Progress (2026-11-04).** PR 1 merged (#358): the shared rule, the table `tests/governance_v4/taint_semantics.psv` and the suite generated from it, `tests/governance_v4/test_taint_semantics.sh`; its findings are A117. Running every row a second time with the tainted value coming from a callback, which only the VM's runtime stack carries, showed the runtime dropping taint the static rule keeps (A117(f)); PR 1 brings the runtime to the same rule, since "one semantics on both engines" is the decision. Found on the way: A118, a VM wrong-value bug in match and catch bindings inside larger expressions. **Added to Bundle 12 by the owner's decision (2026-11-04)**, as its own PR after PR 1, before the static check: a value the static report reasons about must first be the value the VM computes. Fixed in that PR: the compiler counts the temporaries an expression holds and gives a match or catch local slots above them (`tests/vm/test_expression_locals.sh`).

**Progress (2026-11-05).** A118's fix merged (#359). PR 2 built: `naab-gov taint FILE`, report only, with the analysis in the engine's library so the integration step is wiring (the owner, 2026-11-05: "do the report and then do the real logic and integration after"). Measured over the tree (`docs/taint-flow-report.md`): 1,241 programs, 78 control flows from 24 decision points, of which 23 are a program acting on its own input as designed and 1 is an over-approximation; on 61 test programs run under their own configs, 270 of the 279 lines the runs flag are value flows in the report, and every difference is explained. The cross-check found A120 (two explicit flows the run misses, one of them in two existing tests), A121 (the run's false alarms by name, and a lost line) and A122 (a `command` prerequisite reads the run's stdin). Before any integration: the owner's bar for control flows (a proposal is in the report doc), following imported modules (4,855 calls not followed), and which bundle takes A120-A122.

**Progress (2026-11-05, after #360).** The owner's direction: "Do what's decided and let's revisit the others after". Follow-up PR 1: A120 fixed in the VM (a callback gets its call's data taint; a struct field store taints the struct), A121(d) fixed as the wider class it turned out to be (every operator, literal, member access and subscript had no source location, and the VM read a call's line three instructions back), the two tests that carried A120(b) made to fail their run, A122(b) (hooks inherit stdin too) and A123 (sixteen `.naab` tests print failures and exit 0) recorded. For the revisit: A122's fix, A121(a)-(c) (each loosens what the run flags), A123's triage and guard, the control-flow bar, the Bundle 8 leftovers (A95(b), A98, A99, A100) and Bundle 13's scope.

**Decisions (the owner, 2026-11-06).** Follow-up PR 1 merged (#361, `01fb3552`). **PR 3**, after Bundle 16:
- **A121(a): fix it.** The compiler's compile-time taint names (`tainted_vars_`) get scopes, so every declaration -- a `let`, a parameter, a catch binding, a match binding, a loop variable -- starts its own entry. It lessens what the run flags, so it lands with two rows per shape: the clean case is not flagged, and the tainted case is still refused under both taint origins.
- **A121(b): reduce first, then decide.** The minimal forms do not reproduce it; the original program does.
- **A121(c): keep it flagged**, pinned in the table as an accepted over-report: a nested function reads a variable whose taint at call time the compiler cannot know.
- **A124: fix it**, so A120(a)'s callback taint also reaches callbacks inside an `async fn`.
- `test_chaos_module_boundary.naab` (A123) is settled with A121.

**Progress (2026-11-08).** PR 3 built. A121(b) reduced and brought to the owner, who decided to judge a bound variable by its value: the trigger was an earlier REFUSED bound-variable sink of the same name, whose name the VM wrote into the run-wide taint set and never cleared. A121(a) fixed in two halves (the compiler's names scoped with its locals; the shared rule shadows catch and match binders, which also fixed the same over-report in `naab-gov taint` and on the tree-walker). A121(c) pinned. A124 fixed. The chaos test's pin removed. Two findings on the way, each by the owner's decision: A127 (the cross-block gate judged a flow by its first bound variable) fixed in this PR; A128 (the VM's PHP binding never parses) recorded only. A third, found while killing mutants and fixed by the owner's decision: A129 (an `async fn` returning a captured variable dropped its run-time taint). And A121(d) had one site left (the VM's `print` check read the next instruction's line), fixed with it.

**The control-flow bar** stays report-only. Nothing refuses a control flow until (1) imported modules are followed (4,855 calls are not followed today); (2) it is measured against Bundle 13's labelled corpus, with a stated catch rate and none of the 23 benign decision points refused; (3) only the narrow shape is refused -- an outbound sink reached under tainted control, with arguments that differ by branch or in a loop over tainted data; (4) a declared exception comes with a ratchet entry, so none can be added mid-run; (5) the key is opt-in and defaults to report. So it follows Bundle 13.

## Bundle 16 -- honest tests and the engine's own child processes (merged, #362)

Added by the owner's decision (2026-11-06). It runs before Bundle 13 because Bundle 13 measures what governance catches, and that measurement is only as good as tests that fail when they fail.

- **A123: every `.naab` test fails its run when it fails.**
  - Fix the 14 that print a failure and exit 0. Triage (observed 2026-11-06, each run as `run-all-tests.sh` runs it): 8 expect ints from stdout-based executors, a coercion `b44018d4` removed on purpose; 1 checks a message `7b82a8d5` renamed; 1 expects module-relative paths, where the engine resolves against the working directory by design; 1 runs with no govern.json and expects a standard sandbox; 2 wait on A125; 1 (`test_chaos_module_boundary.naab`) is settled with A121 in Bundle 12 PR 3.
  - A guard in `run_test`: a `.naab` test that exits 0 but prints a failure marker counts as a failure. Controls: a planted silent failure is caught, and a test printing `FAIL: 0` passes.
- **A125: a JavaScript block that declares a function and ends with a call returns `null`**, on both engines. Fix the executor's wrapping.
- **A122: the engine's own child processes.**
  - Prerequisite commands and hooks (POSIX and Windows) start with an empty stdin. Children the program starts (`process.run`, polyglot executors) keep inheriting it: that is the program's stdin to hand on.
  - Prerequisites run only under `naab-lang run` and its workers. The static `naab-gov` subcommands report them as not run, the rule guidance connectors already follow (`allowExternalGuidance()`). Correct the help text and CLAUDE.md, which call those subcommands static.
- **A99: one copy of the essential child-environment names.** Embedded Python's list is generated from `essentialChildEnvVars()`, so it stops keeping `LD_LIBRARY_PATH` under the allowlist. The Termux check is for the owner's machine; a failure there fails closed.
- **A100: a non-object `agents` block refuses the config (exit 4)**, by A111's rule for a restriction in a type its parser does not read. None of the 80 tracked configs with a roles block uses the list form.

**Order** (the owner, 2026-11-06): Bundle 16, then Bundle 12 PR 3, then Bundle 8's follow-up (A95(b), A98).

**Progress (2026-11-06).** Built as one PR; each item's record is in `docs/open-investigations.md`.
- A123: the 13 tests fail their run when they fail (each made red once); `path_resolution` and `test_python_os_block` became shell suites, because a `.naab` test cannot choose its working directory or sandbox level. The `run_test` guard is live: over the whole `.naab` phase it flagged only the one pinned test, `test_chaos_module_boundary.naab`, which waits on A121.
- A125 was two defects in one function: the split at the last statement, and declarations wrapped as expressions (a block ending in one never ran).
- A122: the decided fix, plus the runtime-version probe (the same class: the engine's own child). The static commands that load a config -- signing, the C API, the server processes -- no longer run prerequisites either.
- A99, A100: as decided; A100's refusal also covers the legacy `agent_roles` key.
- **The `path` module's Windows contract (A123), decided (the owner, 2026-11-07): always `/`.** The guard's first honest build-windows run failed `tests/stdlib/test_stdlib_path.naab`, which the old Python-warning rule had excused since it was written; the module returned `std::filesystem`'s mixed native form there. Every path function now returns `/` on every host; `is_absolute()` keeps the host's meaning.
- **Found and not decided: A126** -- `naab-gov lint` fires the project's `on_violation` hook, the hook half of A122(c). Pinned (`test_engine_child_stdin.sh` ES-07), the owner's call.

## Bundle 17 -- what Gemini's SentinelGrid report found (merged, #365; ran before Bundle 13)

Added by the owner's decision (2026-11-09: "Ok bundle 17 first"). Gemini built a project on NAAb and reported 42 findings; all 42 were checked on master `314bccfd`, twice (26 reproduced by running, 12 confirmed in code, 2 do not reproduce, 2 checked in part). This bundle takes the holes and the bugs; the precision findings wait for Bundle 13, which measures them.

- **A130: an environment only turns a gate on.** The owner (2026-11-09): "only raise govern.json's mode, never lower it", applied to `quality_gate.enabled`, `governance_baseline.enabled` and `governance_baseline.fail_on_regression`.
- **A131 (F-34, F-05): custom rules.** Applied to NAAb code at run time, not only by lint; a pattern compiles with the crash-proof matcher and is enforced, or refuses the config.
- **A133: rate limits are HARD**, through `enforce()` (the owner: "HARD, like other limits"). F-14 (string methods not counted as stdlib calls) recorded open.
- **A134 (F-32, F-41, F-42): contracts.** A contract for a function the program does not have is skipped and named (the owner); typed padding; `must_derive_from` needs every listed parameter, in code (the owner).
- **A135 (F-04, F-21, F-28): a reload resolves `extends`.**
- **A136 (F-27): `naab-verify-audit` verifies the engine's audit trail.**
- **A137 (F-36, F-08): `export fn`** for `@intent` and `naab-gov contracts`.
- **A138 (F-03): CONTRA-013** fires only while a separate-process language can run.
- **A139 (F-01, F-02, F-06, F-26): template and `init` drift**, including the unused `scopes` block (warned at load, not built).

**Added from Gemini's updated report (the owner, 2026-11-09):** A141 (F-33, wider than reported: a sink naming a NAAb function was never checked by the run either -- now enforced at the call and reported) and A142 (F-09: the contract search keeps a typed parameter's cases in its type). Recorded, not built: A143 (F-35, output baselines; a design call) and A144 (`must_derive_from` checks presence, not derivation into `return_key`).

**Not taken, recorded:** A132 -- `naab-gov lint` applies 15 whole-file checks a run does not (the owner: custom rules now, the rest after Bundle 13 measures their precision); A140 -- the rest of the report, and the remedies in it that would loosen governance (a test-context exemption, per-file scopes, crediting `let` names to intent, not scanning comments for secrets), which are not to be taken as written.

## Bundle 13 -- measure what governance catches; help operators

- **A labelled corpus** of good and bad LLM-written code: llm_bridge runs plus `tools/langconform/corpus_diff.py` blocks.
- **Per-check precision and recall**, report-only in CI first.
- **`naab-gov diff OLD NEW`:** which requests a config edit newly allows or refuses (built on Bundle 8).
- **Settings liveness follow-up:** 747 settings survive removal, and 538 are untested.

**Decisions (the owner, 2026-11-10).**

- **The corpus is redefined.** Neither source above comes with labels: `corpus_diff.py`'s blocks carry no verdicts, and the llm_bridge replies were directed by us. Labels come from outside instead:
  - security labels from benchmarks other people built (NIST Juliet, OWASP Benchmark, PrimeVul; SecretBench needs an access request);
  - gaming labels from executed tests (LLM-written solutions judged by EvalPlus's hidden tests, plus llm_bridge runs that recorded a validation result);
  - the `corpus_diff.py` blocks stay as the false-alarm baseline.
  - The security benchmarks are human-written code; the LLM-written part is EvalPlus's published outputs and our llm_bridge runs.
- **The measurement runs nightly, report-only**, not on every PR. Datasets are downloaded at run time and pinned by hash, never committed.
- **Three PRs:** PR 1 `naab-gov diff`; PR 2 the scorer with its controls and a pilot on Juliet and EvalPlus; PR 3 the A132 decision from PR 2's numbers, the other benchmarks, and the settings follow-up if wanted.
- **Not answered by the external benchmarks:** Bundle 12's control-flow bar. It needs measurement on NAAb programs (`tools/taintflow`) and stays its own item.

**Progress.** PR 1 (built 2026-11-10): `naab-gov diff OLD NEW` judges each request under both files with the engine's own gates, through the probes the Bundle 8 harness uses (now shared, `include/naab/policy_probes.h`), and reports the settings as the reload decision does (`evaluateReload()`). Report only. Its first trial run found A147 (a `*.name` host entry matches nothing), recorded for the owner.

## Bundle 14 -- replay and provenance

- **Deterministic replay:** rebuild a run's governance decisions from the transcript, telemetry and decision snapshots (`naab-gov replay`).
- **Provenance for generated code:** model, prompt hash and code hash per codegen or polyglot block, chained into telemetry.

**Scope (the owner, 2026-11-11), two PRs, provenance first.**
- **PR 1, provenance** (built, #369): `CODEGEN_EXEC` and `POLYGLOT_EXEC` carry the SHA-256 of the code run and whether that exact text equals a model output of the run; on a match, the model, the hash of the full request as sent and the hash of the whole reply. Attribution is exact hash equality, otherwise `unattributed`: nothing looser, because a guess does not belong in an audit record. No new key: the fields follow telemetry, so there is nothing to switch off mid-run. `AGENT_RESPONSE.content_hash` (first 500 characters) is kept as it is; the whole reply's hash is a new field, `content_sha256` (A160). Findings recorded, not fixed: A161, A162.
- **PR 2, `naab-gov replay FILE`**: a consistency check, not a re-execution -- the decision snapshots hold digests, not the scorer's inputs, so re-running the scorer would mean building a second one. Docs and help text say it "checks recorded decisions against their recorded evidence". Per decision MATCH / DIVERGED / UNMEASURABLE. Exit: any DIVERGED is 1; none diverged but anything UNMEASURABLE is 2, never 0; 4 a usage error. Report only: it runs no program, prerequisite or hook and fetches nothing.
  - **Keyed chains:** it follows the existing chain verifier's path for a file sealed with an HMAC key and adds no new place that reads the key; a keyed chain it cannot verify without the key is UNMEASURABLE.
  - **Telemetry and transcripts are untrusted input** (they carry model-written text): what it reads per line is bounded, and that text is never executed or pattern-matched without limits.
- **Not built:** re-execution of the scorer from the transcript (decide after PR 2 measures what comes back UNMEASURABLE), any enforcement that reads replay output, provenance by taint lineage, Bundle 15's telemetry healing.

## Bundle 15 -- AI on deterministic rails (expert reviewers, patch agents, telemetry healing)

- **Reviewers** explain deterministic findings (the manifest, taint paths, BSD matches); they never decide.
- **Patch agents:**
  - governed by the ratchet and S22 `evidence_count` (no shrinking tests);
  - may not touch `govern.json` or test assertions;
  - verified by the Bundle 8 and 11 checks; a human merges.
- **Telemetry healing:**
  - log content is untrusted data, because it carries model text;
  - mitigations only tighten;
  - built on Bundle 14 replay.

## Not bundled (open, the owner's call)

- **Waiting on the owner:**
  - A147: a `blocked_hosts` or `allowed_hosts` entry written `*.name` matches no host;
  - A88: codegen refusals are catchable, and `codegen.level` is inert;
  - the 14 compiled-but-unrun test programs, plus the unlinked `naab_testing` library.
- **Open:** A66, A75(a)(b), A76(e), A70(b)(c), A67, B1, A16/F11/F12, A53, A33(b), D18, and the A64 test debt.
- **Paused:** the guidance bypass filter.
