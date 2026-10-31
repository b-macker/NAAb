# NAAb bundle plan

The project owner's ordered plan for governance work. It was recorded on 2026-10-30, and its statuses were brought up to date on 2026-10-31.

A **bundle** is one branch, one pull request and one validated push. **Confirm a bundle's scope with the owner before starting it.** A finding made along the way is recorded in `docs/open-investigations.md` in the same pull request, and is added to a bundle only by the owner's decision.

## Status (2026-10-31)

| Bundle | State | Where |
|---|---|---|
| 1-5 | merged | git log, CHANGELOG |
| 6: crash-proof governor, then the planned defaults | merged | #347 (`41a3b3c3`) |
| 7: release | merged | #348 (`ca07ae12`) tagged v2.0.0; its macOS release build failed, so #349 (`735fa217`) released 2.0.1 |
| 8: "governance only tightens", proved | in progress | |
| 9-15 | not started | |

Outside the bundles: A92 (the cycle collector cleared lists another engine was using) was fixed in #350 (`96a129b1`).

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
  - the 14 compiled-but-unrun test programs, plus the unlinked `naab_testing` library.
- **Open:** A66, A75(a)(b), A76(e), A70(b)(c), A67, B1, A16/F11/F12, A53, A33(b), D18, and the A64 test debt.
- **Paused:** the guidance bypass filter.
