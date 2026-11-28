# NAAb bundle plan

The project owner's ordered plan for governance work. It was recorded on 2026-10-30, and its statuses were brought up to date on 2026-10-31.

A **bundle** is one branch, one pull request and one validated push. **Confirm a bundle's scope with the owner before starting it.** A finding made along the way is recorded in `docs/open-investigations.md` in the same pull request, and is added to a bundle only by the owner's decision.

## Status (2026-11-23)

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
| 13: measure what governance catches; help operators | merged, three PRs by the owner's decision (2026-11-10). PR 1 #367 (`6e2ce08b`): `naab-gov diff`. PR 2 #368 (`24cf9d80`): the scorer, its controls, the Juliet and EvalPlus pilot, and A147 fixed (refuse). PR 3 #370 (`93d2e099`): the A132 measurement, the path-traversal answer, a second profile, Semgrep labels for ten languages; A149 recorded | Bundle 13 below |
| 14: replay and provenance | two PRs by the owner's decision (2026-11-11). PR 1 merged, #369: provenance for generated code; A160-A162 recorded. PR 2 merged, #372 (`7f1db67e`): `naab-gov replay`; A163, A164 recorded | Bundle 14 below |
| 15: AI on deterministic rails | merged, four PRs and a follow-up. PR 1 #373 (`d792d943`; the owner's decision, 2026-11-12: A163 and A164 first, inside this bundle): the telemetry chain verifies under concurrent writers, and `--verify-telemetry-chain` is bounded and exits 1 on a file it cannot read. The feature work's scope was agreed on 2026-11-13 (three PRs): PR 2 #377 (`7bfd99cb`), `naab-gov explain`; PR 3 #379 (`2dc11dd9`), `naab-gov patch-check`; PR 4 #381 (`743e4ee7`), `naab-gov heal`. Follow-up #383 (`58796deb`), decided by the owner on 2026-11-15: A166 fixed (the voice and agent review scan what their models wrote), patch-check runs the contract search by default, A169's write half fixed (the program may not write `.naab_cache/`), and A58's unrun test programs cleaned up | Bundle 15 below |
| 18: messages and records that match what happened | merged, #386 (`ec009c5e`), one PR, added to the plan by the owner's decision (2026-11-16, #384): A171-A174 and A39(c) fixed | Bundle 18 below |
| 19: every open item, and seven findings from another session | five PRs, the owner's decisions 1-24 (2026-11-20). PR 1 (security and silent no-ops): A183-A187, A189, A191, A53, A33(b), A87, A111's remainder and F35 fixed, A116 decided (a) and built, A192 found and fixed, B1 documented, A188 and A190 recorded for PR 3. PR 2 (init, a run past 10,000 results, plugins): A75(a)(b), A176, A177, A107, A110's remainder, A192(a)(b) (decisions 25-26, 2026-11-21) fixed, A193 found and fixed. PR 3 (language, tooling, VM correctness): A188, A190, A179, D18, A170, A67 (VM; the tree-walker still refuses), A69, A66, A39(a), F52, A161, F46, A29(d), U-014 (A175), A105 fixed; A162 found fixed on master by A199 (#397) and dropped from it; A82 found already fixed (`80e09a9d`); A215, A218, A219 found and fixed (A215 and A218 by the owner's decisions of 2026-11-21); A216, A217, A220-A224 found and recorded open. PR 4 (template clean-up, test debt): A224, A216, A217, A221, A223, A175(b), A29(e), U-016, A1, A64's `test` rows, D8 fixed, D15 re-checked; A229 and A231 found and fixed; A225-A228 and A230 found and recorded open. PR 5 (the drift signals): B7 fixed (decision 30), A4 and A5 closed (done by #196; decision 29), V3a documented | Bundle 19 below |
| 20: run history -- what NAAb refused, what helped, what agents claimed | merged, five PRs by the owner's decisions (2026-11-21): PR 1 #394 (rebuilt after Bundle 19 PR 2, #393, which overlapped it), PR 2 #395, PR 3 #396, PR 4 #397, PR 5 #398 (`abf2c4cd`). After it, A209 in its own PR, #399 (`31c90c23`) | Bundle 20 below |
| 21: what governs this -- govern.json, CLAUDE.md and the setup files, read honestly | in progress, four PRs; scope confirmed by the owner (2026-11-23). PR 1 merged (`0bfa212c`, #400); PR 2 open | Bundle 21 below |

Outside the bundles: A92 (the cycle collector cleared lists another engine was using) was fixed in #350 (`96a129b1`). A145 and A146 (node 24: the child memory budget, TypeScript's flags, and the runtime-pin message) were fixed in #366 (`97dab417`). The bundle plan itself was recorded in #351 (`86fc33c9`). Release 2.0.2 (CHANGELOG `[2.0.2]`) carries everything merged from #350 to #387; like 2.0.1, its tag is put on the merged release commit.

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
- **Settings liveness follow-up:** 747 settings survive removal, and 538 are untested. **Re-measured 2026-11-13** (report only, `docs/settings-liveness.md`): 310 of 1,534 live (189 of 1,474 in October), 1,223 survive -- 453 loaded only by the whole-template loader, 47 inert by trace, 414 at the template's value, 309 at another value; 120 of the last group have a security direction in the ratchet table. The scanner's 310 settings are outside the instrument (the scanner reads govern.json itself): unmeasurable, not untested. **Follow-up checks (2026-11-13) and what they recommend (the owner's call, not yet decided):** most "another value" keys are set only by suites that load a whole example config and assert on none of them, so they are untested (the first write-up's "a suite bothered to set it" is corrected in the doc); 54 template settings are inert by the ratchet table, 36 of them by grep only. Recommended order: (1) fix the tool -- `naab-gov` built with the hook, the same test-only hook in the scanner's loader with the compiled-out check extended, whole-config loaders reported apart; (2) single-key re-probes, then tests first for keys whose default is off or unlimited and that refuse an action (`exposure_tracking`, the `codegen` limits, agent tool caps, `restrictions` switches, per-language limits, `trust.require_fresh_signature`); (3) trace the inert keys, then move the confirmed ones from the template to the coverage baseline, in its own PR; (4) the credential-macro pattern stays as it is (A148). **Decided by the owner (2026-11-13): that order, with (4) as written.** **Step 1 built (#378):** one removal hook for both govern.json readers (the scanner's included), `naab-gov` from the test build, `broad-only` as its own status, `probe --settings`, `probe --resume`. Re-measured with it: 318 live, 268 survive, 948 broad-only, 1 unmeasurable of 1,535; the scanner's 310 settings are measured and none is live. **Step 2 built (2026-11-14):** `tests/governance_v4/test_untested_limits.sh` gives first tests to 17 settings that refuse an action (codegen caps, call depth, per-language line caps, the obfuscation and VCS-extraction levels, three code-injection switches, two exposure limits, two tool-result caps), chosen by the owner's instruction to pick the most valuable; each is live on the test build. Found: A168. **Decided by the owner (2026-11-14): "Governance block, wire up to enforce and tighten"** -- built: the `max_unique_agents` refusal (A168) and every codegen refusal (A88, the same question) go through `enforce()` at their configured level, and the three limits that refused nothing (`limits.timeout.per_block`, `languages.per_language.<lang>.timeout`, `limits.code.max_lines_per_block`) enforce at HARD; `tests/governance_v4/test_refusals_enforced.sh`. Step 3 follows as its own PR; which keys get tests within step 2's groups, and whether any inert key leaves the template, stay the owner's call. **Step 3 traced (2026-11-16):** of 86 template settings the records flag, 4 are live, 12 stop nothing where a working mechanism exists, 25 look like protection with no clear meaning, no detector or a working setting that does the job, and 45 guard nothing (`docs/settings-liveness.md`, A175). **Decided by the owner (2026-11-16): "Mixed"** for the safety-shaped ones (make the 12 work; take the 25 out of the template and init and warn when a config sets one), **and "Move out, no warning"** for the 45. Built, merged as #385 (`148d87ae`): the warning, the removals, `test_no_effect_settings.sh`. **The 12 wired (2026-11-18, PR 2, merged as #387, `652ba8c6`)**, with two meanings asked: the tool time limit is a HARD block ("1 b") and the import allowlist is static ("2 a"); `require_fresh_signature` refuses at HARD (the owner's choice). `test_wired_settings.sh`, `test_signature_staleness.sh` ST-06..10; A179 found on the way (a one-line JavaScript block with two statements is a SyntaxError). Found and left for the owner: 143 template keys the loader never parses (A175(b)), and `naab-lang init` replacing an existing `naab.toml` (A176).

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

PR 1 merged as #367 (`6e2ce08b`). **PR 2 (built 2026-11-11):** `tools/govmeasure` -- `datasets.py` (downloads pinned by SHA-256, a mismatch refused), `juliet.py` (each NIST file split into its flawed and fixed variant, comments removed), `evalplus.py` (labels 16 models' published solutions by running EvalPlus's inputs against the reference, each in a limited child process), `govmeasure.py` (per source, category and rule: flagged bad, flagged good, Wilson intervals; UNMEASURABLE when a control fails or the checker cannot be read; the exit status never depends on a rate). First results in `docs/governance-measurement.md`, recorded as A148. The nightly job `governance-measure.yml` repeats it, report only. A147 was fixed in the same PR by the owner's decision (option b: a host entry holding `*` refuses the config). **For PR 3, from these numbers:** Juliet gives no labels for Python, JavaScript or shell, the languages most NAAb blocks are written in, and neither OWASP Benchmark (Java) nor PrimeVul (C/C++) does either; the EvalPlus label is "wrong", which is wider than "gamed".

**PR 3 (built 2026-11-11), scope by the owner's decisions of that day:** A132 measured on the repository's own `.naab` programs with a hand-read sample; Semgrep's rule test files for the languages Juliet lacks (allowed by their licence for internal measurement: fetched at run time, never committed, uploaded or quoted); PrimeVul and OWASP Benchmark dropped; the settings-liveness follow-up is its own later PR. Built: `naab_lint.py` (lint with block bodies emptied, so what fires is firing on NAAb text), `semgrep_tests.py`, a git source pinned to a commit in `datasets.py`, `--group` and `--any-rule-only` in the scorer. Findings: 344 of 1,252 NAAb files flagged on NAAb text by the 15 lint-only checks, by patterns on names and prose (A132 updated); A149 (`oops` inside `Loops`); the path-traversal zero is the check's reach; the text checks flag flawed and "must not fire" stretches alike across ten languages (A148 updated). No check changed.

**Decisions (the owner, 2026-11-11), from PR 3's numbers.** A132: all 15 stay lint-only; `banned_functions` is the one to consider at run and is undecided (its measured row shows nothing either way: the measurement config has no list for NAAb); `naab-gov lint` is to label these findings as not enforced at run. A149: fix it and the class (word boundary, a sweep of the pattern table, paired arms). A148: widen secrets to credential-named `#define` macros and add the named CryptoAPI constants; path traversal is not widened, its reach is documented. Semgrep: counts only, with no rule ids, paths or code in tables or logs; commercial use is an open legal question. **Follow-up 2 built (2026-11-12):** the A148 widenings -- a credential in a C macro (`no_secrets`) and the named CryptoAPI constants (`restrictions.crypto`), measured first (no new firing on the repository's 1,698 blocks) and shipped with a migration note; path traversal not widened, its reach written into the template. **Follow-up 1 merged (#371, `9b616789`), built 2026-11-12:** A149 fixed (word boundaries on the apologetic-language patterns; `whoops` kept) and `naab-gov lint` labels the findings a run does not enforce. **Follow-up order:** (1) A149 and the lint label, one PR; (2) the A148 widenings, each measured first on the repository's blocks, with a migration note; then the settings-liveness follow-up.

## Bundle 14 -- replay and provenance

- **Deterministic replay:** rebuild a run's governance decisions from the transcript, telemetry and decision snapshots (`naab-gov replay`).
- **Provenance for generated code:** model, prompt hash and code hash per codegen or polyglot block, chained into telemetry.

**Scope (the owner, 2026-11-11), two PRs, provenance first.**
- **PR 1, provenance** (built, #369): `CODEGEN_EXEC` and `POLYGLOT_EXEC` carry the SHA-256 of the code run and whether that exact text equals a model output of the run; on a match, the model, the hash of the full request as sent and the hash of the whole reply. Attribution is exact hash equality, otherwise `unattributed`: nothing looser, because a guess does not belong in an audit record. No new key: the fields follow telemetry, so there is nothing to switch off mid-run. `AGENT_RESPONSE.content_hash` (first 500 characters) is kept as it is; the whole reply's hash is a new field, `content_sha256` (A160). Findings recorded, not fixed: A161, A162.
- **PR 2, `naab-gov replay FILE`**: a consistency check, not a re-execution -- the decision snapshots hold digests, not the scorer's inputs, so re-running the scorer would mean building a second one. Docs and help text say it "checks recorded decisions against their recorded evidence". Per decision MATCH / DIVERGED / UNMEASURABLE. Exit: any DIVERGED is 1; none diverged but anything UNMEASURABLE is 2, never 0; 4 a usage error. Report only: it runs no program, prerequisite or hook and fetches nothing.
  - **Keyed chains:** it follows the existing chain verifier's path for a file sealed with an HMAC key and adds no new place that reads the key; a keyed chain it cannot verify without the key is UNMEASURABLE.
  - **Telemetry and transcripts are untrusted input** (they carry model-written text): what it reads per line is bounded, and that text is never executed or pattern-matched without limits.
- **PR 2 as built (2026-11-12):** five checks -- the chain (one function with `--verify-telemetry-chain`, `analyzeTelemetryChain()`, and one place that reads the key, `telemetryChainKeyFromEnvironment()`), each admissibility decision against its own fields, its snapshot and the turn's `CDD_TURN`, each analysed `CDD_TURN`'s coherence against the previous one and the listed penalties and adjustments, each `TRANSCRIPT_REF` against the transcript (`--transcript FILE`), and each attributed execution event against the `AGENT_RESPONSE` it names. Lines are read through `include/naab/bounded_jsonl.h` (1 MiB, 16 MiB for the transcript, 64 levels of nesting). What came back UNMEASURABLE on real files (observed, the 159 `.jsonl` files in the repository, all written before the fields existed): every admissibility decision (no `disposition`), every coherence row (no `coherence_adjustments`), every transcript reference (no transcript named). On telemetry harvested from eleven current suites (117 files): admissibility 105 MATCH, coherence 209 MATCH and 1 UNMEASURABLE (after a reset), provenance 10 MATCH and 6 UNMEASURABLE (tool-loop turns and proposals write no `AGENT_RESPONSE`), no decision DIVERGED; one chain DIVERGED, which is A164. Not checked, and printed in every report: governance level changes (their writers are not all traced), challenges, guidance, contracts, sequence matches.
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

**PR 1 (the owner's decision, 2026-11-12): the chain reads true before anything heals on it.** Telemetry healing is built on `naab-gov replay`, which reported every honest `agent.batch` run DIVERGED on the chain (A164), and the verifier was the one reader of telemetry still unbounded (A163).
- **A164, fixed:** a sealed line is written and flushed before the chain lock and the file lock are released, at every site that seals one; what the webhook receives is in the file's order.
- **A165, found by this PR's build-windows run and fixed in it:** Windows had no cross-process lock on the evidence files; the writers now take one (`LockFileEx`). Kept in the PR by the owner's decision (2026-11-12).
- **A163, fixed, both changes agreed by the owner:** `--verify-telemetry-chain` reads through the same bounds as `naab-gov replay` (1 MiB a line, 64 levels) and reports a line over them `NOT READ`, exit 1; a file whose lines cannot be read exits 1, not 0.
- **Not in PR 1:** reviewers, patch agents and healing themselves.

**Scope of the feature work (the owner's decisions, 2026-11-13).** Every AI output in this bundle is text beside a deterministic verdict: a model never produces a verdict, an applied config change or a merge. Three PRs, in this order.
- **PR 2, reviewers: `naab-gov explain FILE`** (merged, #377). The findings come from `naab-gov manifest infer`, `naab-gov taint` and the `BSD_MATCH` events of a telemetry file, and the exit status is fixed before `output.explainer`'s agent is asked to word them. `agent_review` is left deciding, as designed (`922041ec`) and as tightened in Bundle 3 (A29(b)); "never decide" applies to the new reviewer.
  - Corrected while scoping: a `BSD_MATCH` event's `events` field is free text and is never read; only the pattern name, level, step count and path are.
  - Found: A166 (the governance voice's guide is scanned for secrets only under `no_secrets`); fixed 2026-11-15 by the owner's decision, with agent review's texts, which were not scanned at all. Found fixing it: A169 (the review cache is writable by the program), open.
- **PR 3, patch agents: `naab-gov patch-check BASE PATCHED`** (merged, #379), a gate only; the agent that writes the patch stays outside the engine.
  - A patch may add test files and may not change or delete an existing one. Measured first (2026-11-13): a lexical assertion count is deterministic, but it fell in 14 of 296 accepted commits (vacuity audits among them) and reads a test, the same test with `assert True`, and the same test skipped as equal. The count is printed, not judged.
  - The rule is for patch agents only: 189 of the last 300 commits touching `tests/` modified or deleted an existing file.
  - As built: "governed by the ratchet" is answered the strict way -- a patch may not change a governance file at all, so there is no config change left for the ratchet to judge. The contract search refuses a new counterexample; it runs by default since the owner's decision of 2026-11-15 (it was opt-in, `--run-contracts`), and under `--no-run-contracts` a config with clauses reads UNMEASURABLE. Kept as built by the same decision: no tool runs on the patched tree once a governance file is touched, and any change to a governance file is refused rather than judged by the ratchet. A changed program no longer matches its signed manifest, so under `manifest.required` it cannot run until a person re-signs; what the draft manifest newly lists and the taint flow counts are printed and refuse nothing.
  - Not closed, and printed in every report: an added file under the test paths (a `conftest.py`, a sourced helper) can change how the existing tests run.
- **PR 4, telemetry healing: `naab-gov heal TELEMETRY`** (merged, #381), report only, no model: one fixed mapping, a repeated advisory to a stricter level, and only for a level key written in the old `govern.json`, which it takes as an input and checks against the run's `config_fingerprint`.
  - Measured first: `naab-gov ratchet` refuses a level key ADDED to a config that leaves it absent (no declared key has a default), and refuses any change to a sequence pattern (`fixed`); on the `living-script_extended` config it accepts advisory to soft for `context_drift`, `taint_tracking` and `exposure_tracking`, the rules that repeat in the committed live-run telemetry.
  - Dropped: "an observed blocked host becomes a blocked entry". No event carries a host in a field of its own; it sits in free text.
  - It refuses to propose when the chain does not verify or `naab-gov replay` reports anything DIVERGED; a mapping whose own evidence is unmeasurable is skipped and named. A person signs outside the run (`docs/security-decisions.md`: no in-process signing).
  - As built (2026-11-14): the command writes nothing, not even a proposed file; the report names the key and the value. Each proposal is asked of `naab-gov ratchet` with that one value changed and kept only when the ratchet accepts it AND names the key in a tightening notice, which is what tells a level the engine reads from an unknown `level` key (the ratchet accepts a change to a key it does not compare). A run that accepted a mid-run reload is skipped: after it, the config that produced the events is not the file given. Observed while building: the ratchet refuses a SHORTHAND level's `"advisory"` -> `"soft"` (`"no_debug_artifacts": "advisory"`) as a form change, by the Bundle 8 declaration for that key, so such a rule is named and not proposed; and the engine compares the config's modification time in whole seconds, so a swap in the same second as the load is not seen until the file changes again.
  - Not built, and said in every report: the key is found by the rule's NAME (the longest dotted prefix under which the file writes a level). That it sets that rule's level is confirmed by the ratchet reading it as a level and by its written value matching the events' level, not by tracing the check.

## Bundle 18 -- messages and records that match what happened

Added by the owner's decision (2026-11-16). Found walking one program from `govern.json` to the messages it produces, then re-checked by `docs/investigation-method.md` on `58796deb`. The re-check changed three findings and widened one; the records are A171-A174 and A39 in `docs/open-investigations.md`. **None of it changes what a run allows or refuses**: it changes what the messages, the reports and telemetry say about it.

- **A171: a refusal records the line it happened on.**
  - Today a runtime gate (file, environment, network, taint sink) never sets a location, and inherits whatever the last function return or polyglot block left behind; the message names no line.
  - Each gate records the location of its own call (the VM already has it: an ordinary runtime error prints `at <script>() [file:line]`), and every place that sets the location puts back what it found. The refusal message carries an `At:` line.
  - Telemetry de-duplication (`telemetry.deduplicate_checks`) must not treat a runtime gate's row as a source site, so two refusals are never merged into one row.
  - A guard against the next copy: the same class was patched three times before (the agent path, imported modules, A76). Candidates: a test that enumerates the gates from the binary and asserts each records its own line, or no unscoped location setter left in the engine. Decided in the bundle.
  - Then remove the workarounds: CLAUDE.md's "read a taint violation's line from its message", and `tools/taintflow/crosscheck.py`'s, once the result row's line is right.
- **A172: telemetry says whether the action ran.**
  - A failed check is recorded as `"result": "block"` whether the action was refused, ran with a warning, ran under `--governance-override`, or ran in audit mode; no field says which.
  - Add a field for the outcome; keep `result` as it is, so existing readers do not break. Its name and values are decided in the bundle, before it is built.
  - Check the readers first: `naab-gov replay`, `heal`, `explain`, the chain verifier, the webhook, `tools/llm_bridge/score.py`. Chain hashing covers the new field.
- **A174 and A39(c): the closing lines.**
  - A refused run prints its closing summary too (today it ends at the error, before or during the run).
  - The `-- Governance: PASS (...)` line is the after-run audit's verdict (pass 2, `56f25210`); its labels change to say so. Today "static" counts every check before the audit, runtime gates included, and "violations" counts the audit's findings only, so a run that printed two overrides and "2 blocked" ends with "PASS ... 0 violations".
  - The summary's "blocked" count stops counting an overridden refusal as blocked.
  - A changelog note: scripts that read these lines may need updating.
- **A173: the user guide says where messages go.** It says "the governance engine reports on stderr"; a refusal, like every fatal error, prints on stdout. Correct the sentence, and say it where the guide first shows a refusal.

**Left out, by decision:**
- Moving refusals to stderr: the MCP server (A43(b)) and the fuzz oracle read them on stdout.
- A39(a), the display filter blanking `name = "value"` in help text on the refusal path: a fix narrows where a redaction control runs, which needs its own decision (the record's option: exempt text the engine wrote).
- A39(b), one full message per rule per run: decided (`aacade58`).

**Not measured before the bundle, so measure first:** the tree-walker (audit and off only), Windows, the dangerous-call gate (the probe did not fire it), and the agent path's own location (scoped since #300, not re-checked here).

**Decisions (the owner, 2026-11-16):** the telemetry field is `disposition` (values passed / refused / warned / overridden / approved / audit_only); the guard is to remove the unscoped location setter, plus a suite with one arm per kind of runtime refusal; the closing line keeps PASS and fixes its counts.

**As built.**
- A171: the engine asks the VM running on the thread for its current instruction when no explicit location is in force; `setCheckContext()` is gone, and the six places that used it are scoped. Measured before the fix as planned: the tree-walker recorded a stale line too (CL-14) and is fixed at its one stdlib branch; the dangerous-call gate is still not exercised by an arm. A live location is never de-duplicated, which also settles the dedup half.
- A172: `disposition` in telemetry, the JSON report (with `summary.dispositions`) and SARIF; `result` unchanged.
- A174 / A39(c): as decided; the dashboard counts the same way. A finished run's PASS line reads e.g. `(2 warnings; after-run audit: 0 findings)`.
- A173: the user guide's sentence and example output.
- Suites: `test_check_locations.sh` (20), `test_check_disposition.sh` (11), `test_closing_lines.sh` (8), each red on master `f3b31246` with its controls green on both, each with a kill table. One existing arm read the old wording (`test_hivemind_governed.sh` E-02) and was moved to the new one.

## Bundle 19 -- every open item, and seven findings from another session

The owner asked (2026-11-20) for everything still open as one bundle: the open records with a clear fix or a decision to recommend, and seven findings another session reported, each re-verified on master `c1312132` beside a control. Five PRs, each from master after the previous one merges: (1) security and silent no-ops, (2) `init`, the slowdown and the evidence it loses, (3) language, tooling and VM correctness, (4) template clean-up and test debt, (5) CDD signals. Decisions 1-22 were approved before the work started and 23-24 during PR 1; PR 1's description carries the plan, with what stays out and why.

**PR 1, as built.**
- A183: `env_vars.read: false` is a HARD, recorded refusal at every env function (governance asked before the sandbox).
- A184: a banned `Function(` no longer refuses JavaScript's keyword `function`, in the one matcher `banned_functions` and `blocklist_extra` share.
- A185: an environment raises a listed check's level and turns it on, never lowers it, and names any key it cannot apply; the reload ratchet compares the table on the overlaid rules.
- A186/A187: a `"main"` entry is the top level's own; a non-object `capabilities.functions` entry refuses the config.
- A189: the regex validator's two heuristic refusals are gone (decisions 13 and 23); the time and step budgets stay.
- A191 (found building A185): an `extends` child's `dangerous_calls` or `main_block` level no longer lowers its parent's.
- A53: hooks start from the load-time environment. A33(b): the key variables are withheld. A87: `allowed_ports` binds the program's connections. A111's remainder: an ill-typed `enabled`, `level` or limit refuses the config. F35: `debug.is_tainted` answers from the value's taint on the VM.
- B1: documented (`docs/GOVERNANCE-LIMITS.md` §8). A116: the owner chose (a) -- the run's engine is current before the static phase, and plugin files are exempted explicitly while `loadPlugins()` reads them. A192 (found building it): the program may not write a plugin file.
- A188 (`naab-lang check`) and A190 (line breaks) are recorded for PR 3.
- Suites: `test_env_read_off_refusal.sh`, `test_banned_keyword_match.sh`, `test_environment_levels.sh`, `test_hook_snapshot_env.sh`, `test_key_env_withheld.sh`, `test_allowed_ports.sh`, and new arms in the extends-floor, function-capability, restriction-types, regex and taint-parity suites; each red on `c1312132` with its controls green on both, each with an observed kill table.

**PR 2, as built.**
- A176: `init` keeps an existing `naab.toml` without `--force`, prints its usage for `--help`, and refuses (writing nothing) an argument, a missing value or a preset it does not know -- a misspelt preset used to write a standard config. Found by luck on the way: `test_no_effect_settings.sh` NE-04 had passed an unknown flag and checked one config four times.
- A75: every preset but `relaxed` writes taint tracking and sequence detection on (decision 1); the summary and the file say what the standard sandbox refuses, measured.
- A177: the check-result cap compacts in batches. A107: telemetry resumes by record number and counts results that left memory unwritten. A193 (found tracing A177's readers): the quality gate and the closing lines counted only what the cap kept, so an early advisory stopped failing the gate.
- A110's remainder: 64-bit run totals, reproduced first.
- A192(a), decision 25: a plugin's helpers and the places its imports look are protected from program writes, after tracing the lookup order. A192(b), decision 26: under enforce plugins load first and one that cannot load refuses the run; a rule with no verdict counts as failed.
- Suites: `test_check_result_cap.sh`, unit `check_result_cap_test.cpp`, and new arms in `test_init_governance.sh`, `test_plugin_files.sh`, `test_reported_token_counts.sh`; each red on d7799c6d where it is a subject.

**PR 4, as built** (template clean-up and test debt; the owner's "all recommended", 2026-11-26).
- A224: a module's exported variable keeps its taint across the import, both engines. A216: an importer reaches only the functions a module exports, both engines. A217: the VM's built-in method route takes its arguments' taint. A221: a TypeScript block ending in a statement is null. The debug module's tables are under one lock. A223: the pipefail guard also flags a status-verdict filter, and the scanner false-positive suite is registered with arms that can fail.
- A175(b): of the 140 never-parsed template keys, 48 that ask for protection nothing provides are warned at load and 94 leave quietly (decision 6); 4 are read elsewhere and stay. Both templates, `naab-lang init` and five unsigned example configs drop them; `template_retired_keys.txt` is the record. A29(e), U-016 and A1 closed.
- A64: the 23 `test` rows done -- `test_a64_untested_paths.sh` (13 arms, 17 mutants killed) plus three rows already pinned by mutants elsewhere; `setInheritedPressure` found unreachable since #49 and fixed (A229, the owner's decision of 2026-11-27), and the tree-walker's async branch found reachable by a dict-field call (A228). D8 fixed: 78 suites isolate the trust store (`isolate_trust_store`, no EXIT trap), CV-02 absolute. D15: reasons re-checked.
- Found and fixed: A229, and A231 (found by the Governance Check on #402's first head: on the VM a module's code called whichever file's function of the same name was merged last, which A216 then refused), with the A216 re-import hole found fixing it. Found and recorded open: A225-A228, A230 (`await` precedence; record only, the owner's decision of 2026-11-27).

**PR 5, as built** (the drift signals; the owner's decisions 29-30 of 2026-11-27, after the plan's premises were re-checked against the code).
- A4 and A5 had been done by #196: S7's granted set is seeded and S4's blocked-then-retried capability is recorded, and removing either fails its `test_signal_reachability.sh` arm (mutant runs on 649caf69). Both closed; S7 stays off by default (decision 29).
- B7: the plan's fix (mark the agent's own events) could not by itself catch the loop the record names, because S1's fingerprint holds each reply's content hash (traced; observed: no firing on ten identical calls with identical results). Decision 30: the mark, plus S1 also fires when the last `circular_lookback` turns made the same tool calls and got the same results. `test_tool_call_loop.sh` (9 arms, 6 mutants). The mark is load-bearing under `event_feed: since_last_check` only (TC-05).
- V3a: documented in CLAUDE.md's S20 entry, no change.

## Bundle 20 -- run history: what NAAb refused, what helped, what agents claimed

Added by the owner's decision (2026-11-21), after reading the "Self-Improvement Memory" paper (arXiv 2611.18856): the paper gives an optimizer a structured memory of problem, attempt, prediction, outcome and provenance; this bundle gives that memory to the evaluator, NAAb, which coding agents iterate against until it says PASS. Each run starts with no memory of the last, so governance can be satisfied without the problem being solved.

One history, derived at `naab-lang run` start from previous runs' tamper-evident telemetry (runs whose chain verifies), with refused code stored content-addressed in the protected `.naab_cache/` and its hash inside the chained row. Three consumers:
- **Refusal memory:** how a refusal went away (repaired, moved into a module the check skips, special-cased the golden input, history reset); a remembered function found unchanged in an imported module is re-checked; precedent on the next refusal of the rule.
- **Live-guidance slice 2, as decided on 2026-10-12** (`docs/plan-live-guidance.md`, "Measuring whether it helped"): nothing at runtime decides on outcomes; an engine-written `history` source; after-run efficacy with undetermined windows; an optional holdout.
- **Claims as evidence:** a reply's claim of success or a fix is chained before the validation that settles it; S24 (off by default) charges a claim a failed validation contradicts.

**Rules:** memory only points an existing check at code or chooses engine-written facts to show -- every verdict is the real check under the current `govern.json`, so a stale or forged history can add an honest check and never invent or remove a refusal; nothing skips work because of memory; no model, embedding or retrieval anywhere; run only (not `naab-gov`, lint or the contract-search worker); a secret or PII subject is never stored; how a check was avoided is never printed to the terminal.

**Decisions (the owner, 2026-11-21):**
- Five PRs, in order: (1) evidence the history can stand on; (2) run history that records and reports and changes no verdict; (3) the parts that act; (4) guidance slice 2; (5) claims.
- A remembered function found unchanged in an imported module is refused at the rule's own level (narrowing `ae4bfb67`'s module exemption only for code NAAb already refused).
- `naab-lang init` turns telemetry and the chain on, so history is automatic for new projects.
- In scope: telemetry loss past 10,000 results (became A107, fixed by Bundle 19 PR 2, #393), A196 (the governance-baseline file), A197 (a tool grading its own handle), A202 (S19's claim reading). A33(b) (the key variables withheld from the program) was planned as PR 1's main item; Bundle 19 PR 1 (#392) withheld three of the four, and this bundle's PR 1 adds the fourth.
- Numbered 20, not 19 (2026-11-21): another session's Bundle 19, decided 2026-11-20, merged its PRs 1 and 2 (#392, #393) while this one's PR 1 was open, and overlapped it. By the owner's decision, PR 1 waited for #393 and was rebuilt from that master with only its own items; the records were renumbered from the next free ID (A195-A204).

**PR 1 -- evidence the history can stand on (merged, #394):** A33(b)'s fourth variable (`telemetry.webhook_auth_env`; #392 withheld the other three); A195 (results recorded after the last report write); A196; A197; and A205, found on its CI (one suite fixed, the class left for the owner).
**PR 2 -- run history (no verdict changes; merged, #395):** a subject on every function-body check record (A198), a code fingerprint, the content-addressed store, the chained carry and its anchor, reset detection, `naab-gov history`, `history.enabled`, `naab-lang init` writing telemetry and the chain on. **As built:** a repair is a recorded pass of the same rule, function and level or stricter (not a scan: RH-03 found the scan rule counting a function another check refused first); the fingerprint drops literal values, so a body with a secret or PII shape keeps its fingerprint while its text is never hashed or stored; two runs at once merge their carries under the telemetry lock; the `-- History:` line prints just before the verdict line, on PASS and REFUSED alike. The fingerprint drops a leading `export`: a module's text starts at its declaration line, so without it code moved into a module never matched (found writing PR 3; RH-15).
**PR 3 -- the parts that act (merged, #396):** the module re-check, precedent lines on the refused run's closing text (stderr), the golden-case fact (telemetry and the report only). **As built:** a module function re-checked for a remembered rule runs that rule's check only, at its configured level; the cosmetic sanitizer counts as `no_incomplete_logic`, whose name it refuses under; plugin rules are not re-checked, because PR 2 remembers the four function-body rules and `must_produce` only. A `must_produce` result names its subject only for a function defined in the entry program (the contract runs after the program, from its text). The golden-case comparison uses the stored refused text only while its hash still matches the recorded one, since a shell block with shell on can edit `.naab_cache/` (CONTRA-013). Writing the suite found its own vacuous arm: the first RM-02 passed on `main{}`'s complexity-floor refusal, not the module function's, and two mutants survived it; the floor now names the function, and both are killed.
**PR 4 -- guidance slice 2 (merged, #397):** the model on scored-reply records (A199), the `history` source, efficacy (one function for the report and the next run), the holdout, a replay `guidance` check (A200). The owner decided on 2026-11-22 that A200(a) is fixed here: a second hash over the text exactly as recorded, beside the delivered one. **As built:** `model` on `CDD_TURN` and `GUIDANCE_RESPONSE`; tracing A199's unverified half found the reply's trace emptied by a tool loop (model, provider, key, attempts, latency), fixed in the same PR; `recorded_sha256`, `section_sha256` and `section_key` on every delivery; a cooldown skip recorded; `include/naab/guidance_efficacy.h` judges each agent delivery over the next three analysed turns of its handle (recurred, quiet, incomplete, mixed model, or undetermined when one of the event types its table classifies as an intervention lands first), at the next run's start (totals carried per section) and in `naab-gov efficacy`; the end-of-run block names a section whose trigger always came back; `guidance.holdout` draws reproducibly from the run id (`startup` in the ratchet); the `history` source holds shape-checked facts, one section per rule; `naab-gov replay` gained a sixth check, `guidance`; the stub answers by request path, so a fallback model can be tested. Left open: a candidate cut by `max_entries` is not replaced (it would change what is delivered); A199's `OUTPUT_ADMISSIBILITY_EVAL` model, per-model baselines and the two fallback rules (decisions). Found while building it: A207 (the telemetry writer refuses text that is not UTF-8), moved to PR 5 by the owner's decision of 2026-11-22. Also recorded: A208 (two runs at once can judge one delivery twice in the carried totals; traced, open).
**PR 5 -- claims:** one claim reader shared by S19 (A202) and S24, `AGENT_CLAIM`, settlement on `VALIDATION_RECORDED`, S24, a replay `claims` check. And A207, by the owner's decision of 2026-11-22: the telemetry writer made total over text that is not UTF-8 (one helper, which `recorded_sha256` also uses), with the other free-text writers swept as one class. **As built:** `include/naab/claim_reader.h` reads a reply once (success, fixed, failure, prediction; code, quotes, questions, conditionals and infinitives skipped; negation and modals within three words; tool names whole), pinned by a 42-row table and printed by `naab-gov claims`. S19 judges a tool only when the reply names it and says one thing about it; `claim_accuracy_min` is warned at load (nothing to wire). Bundle 19's remaining PRs hold no S19 item (checked 2026-11-23: A66, A67, D18, A170, A64), so nothing overlapped. S24 keeps its own latch, consumed whether or not it is on, and is scored at exit like S22. Claims never settled are counted by `naab-gov replay`, not by a new event: the chain already holds both sides. A207 turned out wider than recorded -- the report and telemetry writers lost a refused run's evidence, and so the next run's memory of it -- and every evidence writer now goes through `include/naab/utf8_text.h`. Found while building it: A209 (the request a model receives is serialised strictly too; left for the owner) and A210 (the A6 state-field screen read its structs from stale fixed line numbers, 12 members unread on master; fixed: bounds by name).

**After the bundle -- A209 (the owner's decision of 2026-11-23, its own PR):** the request a model receives, made total over text that is not UTF-8 as PR 5 made the evidence writers. **As built:** text a scan judges is made valid where it enters the request, before the scan (the prompt, a tool's result before its cut, a guidance section at parse), and every request is serialised by `naab::text::dumpRequest()`. Wider than recorded: a valid result cut inside a character, a dict result, guidance, propose, tool declarations, history and the engine's own calls all failed, and agent review skipped such a program under its default `fail_policy`. Found while fixing it: A211 (a program can write an agent handle's history, and no scan reads it; open, the owner's call).

**Known limits, to be stated in every PR:** deleting both the telemetry file and `.naab_cache` erases history undetectably; a shell block with shell on can edit `.naab_cache` (CONTRA-013); a fresh checkout or CI without history passes what a local run refused; a trivial edit while moving code defeats the fingerprint; an `extends` child can switch telemetry off (A98 leaves it unfloored). None loosens existing governance; the module route's full fix stays the owner's open decision on module heuristics.

## Bundle 21 -- what governs this: govern.json, CLAUDE.md and the setup files, read honestly

Added to the plan by the owner (2026-11-23; scope confirmed the same day), after two proposals from another session were checked against the code: one proposed general retrieval ideas (BM25 ranking, frequency weights, graph clustering), the other a "governing knowledge layer". The owner's direction: "something that pulls info from govern.json, Claude.md, project set up files, etc.. the sources that help govern". 
What already exists (traced 2026-11-23): `project_context` (off by default) reads CLAUDE.md, gemini.md, `.cursorrules`, `.github/copilot-instructions.md`, linter configs and manifests, and turns language preferences and banned functions or imports into rules; live guidance already takes CLAUDE.md as a source (`"guidance": {"sources": ["CLAUDE.md"]}`), delivers a section named for a rule when it refuses, and protects a listed source from program writes; a rule's `rationale` reaches only the end-of-run dashboard (`Why:`, up to 3) and the reports; the MCP `naab_guidance` tool is live-guidance slice 4, unbuilt. Checking the first of these found A212 and A213, so the bundle starts by making it honest.

**Rules, beside the standing ones:** govern.json stays primary and a project document never loosens it; no self-instruction loop (a file whose text reaches an agent is a file the program may not write); a new view is report-only and runs none of the program; nothing shown to a model or a coding agent carries a detection pattern, a threshold or weight, a key variable's name, a sanitizer list or a `must_produce` golden case.

**Proposed PRs, in order:**
- **PR 1 -- `project_context` integrity: A212, A213, A214** (A214 added by the owner, 2026-11-23). An unattributed ban added to every language a block can be written in; the three style settings no rule reads reported as not enforced; the files whose text NAAb turned into a rule protected from program writes while `project_context` is on. Measured before deciding (A213): a project file the program CREATES can lift a refusal too, through language preferences and manifests feeding the language-choice check, so instead of protecting every place the loader searches (which would block a build tool writing `package.json`), the language-choice check decides with and without project preferences and refuses when either refuses -- a project file can then only add a refusal. A214: that check's HARD refusal goes through `enforce()`.
  **As built:** as above; a fourth unread style setting (`complexity.max_lines_per_block`) and A214(b) (the HARD refusal switched off by `helper_errors.enabled: false`, a setting for the hint text) were found fixing it and closed in the same PR. `tests/project_context/edge_case_tests.sh` passed and ran nowhere; it is registered. `test_reload_startup_overlays.sh` PC-02 rewrites CLAUDE.md through its operator, since the program's own write is now refused. Its first build-windows run found A232 (a CRLF file lost its `prefer X for Y` lines), fixed in the same PR; both suites now read UNMEASURABLE for the checks that need a Python block on a build without one, and the integrity suite measures its file-protection arms there.
- **PR 2 -- A211: program-written agent history.** The owner's decision (2026-11-23): the history is kept engine-side and a handle whose list differs is refused, naming `context_window` as the supported way to trim (a scan would catch a secret but not a forged earlier assistant turn; three requests carry the list, the step-up challenge among them).
  **As built:** the engine keeps each handle's history (`AgentTracker::history`) and every request is built from it; `agent.send()`, `agent.propose()` and `agent.commit()` refuse a handle whose list differs, before anything is sent -- catchable, by the owner's decision of 2026-11-27 -- naming a context window and `agent.reset()`. `agent.messages()` returns the engine's history. Measured first: only an explicit `let h2 = h` copy goes stale (functions, loops and `batch`/`fan_out`/`pipeline` share the handle), and a non-dict entry crashed with `get: wrong index for variant`; both are refused now. `test_request_utf8.sh` RU-10 read history the program wrote, and now reads it refused.
- **PR 3 -- the rule's `rationale` in the refusal message**, terminal only. History pass first on why `formatError()` leaves it out (the error-leak rule governs what a message may carry).
- **PR 4 -- `naab-gov context TARGET`**, report only: for a file, language, rule or agent role, the effective rules and where in the `extends` chain each comes from, their rationale, what `project_context` extracted and whether each extraction took effect, the matching guidance sections, and the run history's refusals and repairs. Then the MCP `naab_guidance` tool (live-guidance slice 4) on the same builder.

**Smaller, optional:** `naab-gov efficacy` lines a rewritten guidance section's tallies up by source and heading (before and after), still reading causation UNMEASURABLE without a holdout; BM25 for guidance's keyword fallback, as a measured comparison only -- labelled trigger-to-section cases, the noise controls (M-02, M-03c, M-04c, M-05c) still delivering nothing, and any short-word handling kept inside guidance, because the keyword extractor is shared with CDD.

**Not taken, with reasons (2026-11-23):** frequency weights in CDD or challenge scoring (the agent's own replies would set its yardstick, and every calibrated signal would move); bringing dropped turns back into a context window (changes what a model receives, breaks S13's window bookkeeping and Gemini's alternation, and reads program-written history, A211); auto-discovering CLAUDE.md as agent guidance (listing it already does this, and protects it); folder sources (a file the program creates in the folder would become guidance -- needs a protection decision first); failure clustering in reports (little data per project); an external retrieval service (the owner's decision). A170 (block search) is already in Bundle 19's later PRs.

## Not bundled (open, the owner's call)

- **Waiting on the owner:**
  - (done 2026-11-15, A58: the nine unrun test programs and `naab_testing` deleted, three folded into `naab_unit_tests`; `test_composition_validator.cpp` left, commented out of CMake.)
  - A170: `naab-lang blocks search <text>` finds nothing (found folding the search test); A169's remaining half, the cache HMAC keyed from public inputs.
  - (done 2026-11-21, Bundle 19 PR 2: A177, the check-result cap compacts in batches.)
- **Open:** A76(e), A70(b)(c), A16/F11/F12. (A66, A67, D18, A170 and the A64 test debt are in Bundle 19's later PRs; B1, A53 and A33(b) were in its PR 1, A75 and A177 in its PR 2.)
- **Paused:** the guidance bypass filter.
