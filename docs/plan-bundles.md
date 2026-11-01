# NAAb bundle plan

The project owner's ordered plan for governance work. It was recorded on 2026-10-30, and its statuses were brought up to date on 2026-10-31.

A **bundle** is one branch, one pull request and one validated push. **Confirm a bundle's scope with the owner before starting it.** A finding made along the way is recorded in `docs/open-investigations.md` in the same pull request, and is added to a bundle only by the owner's decision.

## Status (2026-11-01)

| Bundle | State | Where |
|---|---|---|
| 1-5 | merged | git log, CHANGELOG |
| 6: crash-proof governor, then the planned defaults | merged | #347 (`41a3b3c3`) |
| 7: release | merged | #348 (`ca07ae12`) tagged v2.0.0; its macOS release build failed, so #349 (`735fa217`) released 2.0.1 |
| 8: "governance only tightens", proved | merged: PR 1 #352 (`76c871cf`), PR 2 #353 (`fc9b8099`) | Bundle 8 below |
| 9: adversarial inputs and concurrency | in progress, one PR (the default rule): every part built and in local validation; nothing pushed | Bundle 9 below |
| 10-15 | not started | |

Outside the bundles: A92 (the cycle collector cleared lists another engine was using) was fixed in #350 (`96a129b1`). The bundle plan itself was recorded in #351 (`86fc33c9`).

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

## Bundle 12 -- taint as information flow

- **One taint semantics.** Unknown AST nodes fail CLOSED; today `expressionContainsTaint()` returns false for them.
- **A static information-flow check** (noninterference) for the NAAb fragment.
- **Collapse the two implementations**, or generate the parity tests from the semantics.

## Bundle 13 -- measure what governance catches; help operators

- **A labelled corpus** of good and bad LLM-written code: llm_bridge runs plus `tools/langconform/corpus_diff.py` blocks.
- **Per-check precision and recall**, report-only in CI first.
- **`naab-gov diff OLD NEW`:** which requests a config edit newly allows or refuses (built on Bundle 8).
- **Settings liveness follow-up:** 747 settings survive removal, and 538 are untested.

## Bundle 14 -- replay and provenance

- **Deterministic replay:** rebuild a run's governance decisions from the transcript, telemetry and decision snapshots (`naab-gov replay`).
- **Provenance for generated code:** model, prompt hash and code hash per codegen or polyglot block, chained into telemetry.

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
  - A88: codegen refusals are catchable, and `codegen.level` is inert;
  - from Bundle 8: A98, A95(b), A100 (refuse or warn), A99 -- see Bundle 8's progress;
  - the 14 compiled-but-unrun test programs, plus the unlinked `naab_testing` library.
- **Open:** A66, A75(a)(b), A76(e), A70(b)(c), A67, B1, A16/F11/F12, A53, A33(b), D18, and the A64 test debt.
- **Paused:** the guidance bypass filter.
