# CLAUDE.md audit — claims checked against commit `fecb13e7`

**Audited commit:** `fecb13e700e7de268b220baffc990452a1b95d67` (main, 2026-10-03, #276).
Re-check a finding by checking out that SHA. Line numbers `Lnnn` refer to CLAUDE.md **at that commit**.

**Report only.** Nothing in CLAUDE.md or the code was changed. Corrections are
deferred until the concurrent CLAUDE.md edits settle. Section B lists findings to
investigate, not text to change quietly.

## Conditions (configured, not observed)

- **Machine:** Claude Code cloud container, Linux x86-64, 4 cores. Not CI, not Windows.
- **Build:** `-DCMAKE_BUILD_TYPE=Release`, to match CI. CLAUDE.md's own default
  differs (see A3). cmake reported ✓ pybind11, ✓ QuickJS, ✓ OpenSSL 3.0.13, ✓ Rust.
- **Full suite:** run alone, with nothing else executing (15m53s). Probes ran afterwards,
  or between runs, with `HOME` pointed at a scratch dir. The suite installs keys into
  `~/.naab/trusted-keys` mid-run, and an unsigned probe config would otherwise get
  an integrity block.
- **History:** the clone was shallow (50 commits). It was unshallowed to 1247 commits
  before any history claim below.

**Tiers:**
- **screened:** grep/heuristic only.
- **traced:** followed through source to the point of effect.
- **verified:** reproduced with the binary, with a control that produced the opposite outcome in the same harness.

---

## A. Clear-cut text fixes (wrong name, path, count, line, signature)

| # | Where | CLAUDE.md says | Actually (at `fecb13e7`) | Evidence | Tier |
|---|---|---|---|---|---|
| A1 | L314 | "Agent telemetry event types (47)" | **49**. Missing `AGENT_KEY_THROTTLED` (agent_impl.cpp:3089, added b05b7732 #184, 2026-09-01) and `VALIDATION_SCORED_AT_EXIT` (governance_engine.cpp:3253, c0ffcdf0 #269). The second is described in CLAUDE.md prose (S22) but is absent from the list. | Ran the grep CLAUDE.md itself gives and diffed it against the list: 2 in code not in the list, 0 in the list not in code. No non-literal `writeAgentTelemetry(` call sites exist. | verified |
| A2 | L23, L35 | "441 tests, 0 unexpected failures"; "~374 pass, ~51 error-behavior, ~12 needs-tree-walk" | **447 total**, 381 pass, 52 error-behavior, 12 needs-tree-walk, 2 missing-executor. 1 unexpected failure, caused by A4. | Observed: `run-all-tests.sh` summary in this container (Release, pybind11 on). Pass and missing-executor counts are platform-dependent. | observed |
| A3 | L12 | "Debug (default): `cmake ..`" | No default is set. `CMAKE_BUILD_TYPE` is empty, which is not Debug: no `-g`, no `-O`, and `_FORTIFY_SOURCE=2` **is** defined because the type ≠ "Debug" (CMakeLists.txt:101). CLAUDE.md's own L45 says this correctly ("a default local build has no CMAKE_BUILD_TYPE"), so L12 and L45 contradict each other. | CMakeLists.txt has no `set(CMAKE_BUILD_TYPE …)`. | traced |
| A4 | L9, L16, L23 | Build with `make naab-lang -j4`, then `bash run-all-tests.sh` → 0 unexpected failures | Following those two lines literally yields **1 unexpected failure** (`test_r22_fixes.sh`: "naab-gov not built … UNMEASURABLE, not a pass"). At least 3 more arms SKIP as UNMEASURABLE for the same reason (suite log: `SKIP: naab-gov not built`, `[OS-00]`, `[CI-00]`) and still count as passed. After `make naab-gov`, `test_r22_fixes.sh` passes 9/9. The build line needs `naab-gov` too. | Failure observed, then cleared by building `naab-gov`. | verified |
| A5 | L87 | "stdlib/ 23 modules (*_impl.cpp)" | **25 modules registered** (stdlib.cpp:437–463): the 23 `_impl.cpp` files plus `io` and `collections`, both implemented in stdlib.cpp. `io` is the module the L429 gotcha says the filesystem gate missed; it is absent from this list. | `modules_[...]` registrations | traced |
| A6 | L262 | "12 language executors (Python, JS, Go, Rust, C++, C#, Nim, Shell, Ruby, PHP, Julia, Zig)" | **TypeScript is also registered**: `typescript` and `ts` → `GenericSubprocessExecutor("tsx {}")` (main.cpp:356–358). That makes 13 languages across 19 registered names. PHP and TS have no `*_executor.cpp` of their own. | main.cpp:290–362 | traced |
| A7 | L266 | `src/runtime/subprocess_helpers.h/cpp` | The header is `include/naab/subprocess_helpers.h`. Only the .cpp is in src/runtime. | `ls` | traced |
| A8 | L215 | "`OutputContract` struct in `governance_config.h`" | No `governance_config.h` exists. The struct is at `include/naab/governance.h:2644`. | grep | traced |
| A9 | L220 | "`ProviderResponse.thinking_reported`" | No type named `ProviderResponse` has ever existed in src/ or include/ (`git log -S'ProviderResponse' -- src include` is empty). The name comes from commit message dbf55a44. The field is `AgentResponse::thinking_reported` (include/naab/agent_provider.h:32). | history + grep | traced |
| A10 | L151 | `GovernanceHardError` "governance.h, ~line 2467" | governance.h:**3024** | grep | traced |
| A11 | L208 | `governance.health()` returns `{verdict, coherence, governance_epoch, mode, zone, active, consecutive_passes, observation_count, integrity_verified, ...}` | It returns exactly: `verdict, active, total_checks, consecutive_passes, refusal_count, bsd_connected, cdd_connected, telemetry_connected, transcript_connected, degradation_reasons, governance_epoch, coherence`. **`mode`, `zone`, `observation_count` and `integrity_verified` are not returned**; they belong to other functions in governance_impl.cpp. History: never true. The claim arrived in 3d9e5b68, and `result["zone"]` was never set in health(). | Ran `print(governance.health())`. Positive control: the fields that do exist printed. | verified |
| A12 | L218 | "S8-S23 names match in both tables" | **False for S19.** The telemetry label is `claim_result` (`signalName()`, behavioral_sequence.cpp:2694), but the config key is `claim_result_reconciliation`. Copying `claim_result` from `penalties_detail` into `context_drift_signals` overrides nothing. Also stale on the same line: "the only symptom is one `Warning: unknown context_drift_signals key "..."` line". The warning now names the canonical key ("…that is the telemetry label; the config key is "claim_result_reconciliation" (signal NOT overridden)"). The code comment at governance_config.cpp:~2840 lists "four" divergent names and misses S19 too. Never true: `claim_result` dates from a3e5a1dd (07-06), and the sentence was written in cd4a40a5 (07-25). | Probe: `claim_result:false` → warning naming the canonical key. Controls: `circular` gave the documented-divergence warning, and the canonical key was accepted silently. | verified |
| A13 | L231 | S20 refusal indicators: "16 words" | **17**: focused, focus, redirect, scope, mandate, sorry, cannot, assigned, task, objective, unable, outside, beyond, instead, back, remind, reminder. None is removed by `kStopWords`. | behavioral_sequence.cpp:2220–2225 | traced |
| A14 | L257, L312 | `telemetry.forwarding` with `webhook_url`, `siem_url`, `batch_size`, `flush_interval_ms` | **No `forwarding` object, no SIEM URL, no flush interval.** The real keys sit flat under `telemetry`: `webhook_url`, `webhook_auth_header`, `webhook_auth_env`, `forward_batch_size`, `forward_timeout_ms`, `forward_retry_count`, `forward_buffer_max`, `forward_shutdown_drain_ms`. "SIEM" is the same webhook. The schema first appeared in docs commit 2a1775a2 (2026-06-11) and was never parsed (`git log -G'contains\("forwarding"\)' -- src` is empty), so it was never built. It is also repeated in docs/security-decisions.md:93, BRANCH-REPORT.md and PROJECT_SETUP.md. **Behavioural consequence: see B2.** | governance_config.cpp:2526–2565 | verified (B2) |
| A15 | L258 | REST: "`api.auth` section with `keys` array, each key has `id`, `key` (or `key_env`), `permissions` (`read`/`write`/`admin`)" | It is **`api.keys[]`** (no `auth` level). Each entry is `{key, name, scopes}`, the scopes are `execute`/`check`/`blocks`/`stats` (rest_api.cpp:533–537), and there is no `key_env`. Same docs commit 2a1775a2, never built; also in BRANCH-REPORT.md. **Behavioural consequence: see B1.** | governance_config.cpp:410–422, governance.h:2782 | verified (B1) |
| A16 | L382, L380 | CLI table: `--no-governance` "Skip governance (dev only)"; `--governance-report` / `--governance-sarif` listed without an argument | **Decided in #244 (7cc189db):** `--no-governance` no longer disables a discovered govern.json. Its only remaining job is waiving the "no govern.json found" requirement (`global_require_governance` defaults true). `--governance-report <path>` and `--governance-sarif <path>` require a path (main.cpp:1246–1248). | Probe: a blocked `file.read` exits 3 in all 5 placements of `--no-governance` (before/after the script, with/without `run`). | verified |
| A17 | L143 | `diff_runner.py` runs with `--no-governance`, "so taint is switched off there by construction" | The outcome still holds, but the mechanism is stale. After #244 the flag does not switch governance off. Taint is off because the corpus runs with cwd = the file's directory and discovery walks up to `tests/govern.json`, which is `mode: off`. If that file's mode changes, taint turns on in the differential suite. | Observed: `[governance] Loaded: …/tests/govern.json (mode: off)` from a corpus file run exactly as diff_runner.py runs it. | traced |
| A18 | L419–L421 | Present-tense line anchors for code since fixed: "`tamper_evident_logger.cpp:501` reads `if (...)`", "`rest_api.cpp:164`, `:194`, `:451` all `_exit(3)`", "`agent_provider.cpp:117` and `telemetry_forwarder.cpp:155` both set CAINFO…" | Each paragraph later says "Fixed". The anchors now point at other code: :501 is the F31 comment; rest_api.cpp has no `_exit` call; the Termux path is at agent_provider.cpp:131 and telemetry_forwarder.cpp:161, behind `exists()`. A reader landing on the line sees code that does not match the sentence. Suggest past tense or a commit anchor instead of a line. | sed/grep | traced |
| A19 | L170 | "would have broken all 12 `living-script_v2` roles" | Ambiguous: `examples/living-script_v2/src/govern.json` has **13** agents, and also had 13 when the sentence was written (4589acd3). It is 12 only if `operator` is not counted. | config | traced |
| A20 | L325 | "22 hook points in `agentSend()`" | Low confidence; the counting rule is unspecified. `if (transcript_active` occurs 21× in the file at the commit that wrote "22" (06509d3b) and 26× now (25 within `agentSend`). The number has probably grown by about 5. | grep at both commits | screened |
| A21 | L328 | "all `localtime_r` calls use `localtime_s` guards (3 sites in agent_impl.cpp)" | **4** sites: 1394, 1644, 5185, 7091. | grep | traced |

---

## B. Behavioural claims that are false (findings to investigate, not text to change quietly)

Most of these say a protection is weaker than documented, the direction most prone to
overclaiming. So each one rests on a matched control in the same harness that produced
the opposite outcome.

### B1. Following CLAUDE.md's REST auth schema leaves `/api/v1/execute` unauthenticated (L258)
- **Verified, one run per arm.** Ran `naab-lang api <port>` in a dir with a govern.json.
  - **Documented** `api.auth.keys[{id,key,permissions}]`: POST `/api/v1/execute` returns **200 with no key**, and 200 with the key.
  - **Parsed** `api.keys[{name,key,scopes}]` (control): **401 with no key**, 200 with the key.
- The server binds `0.0.0.0` and logged no warning.
- **Why it is silent (traced):** the unknown-key validator iterates **top-level keys only**
  (governance_checks.cpp:6789 `VALID_TOP_KEYS`, loop at :6847). A nested `api.auth` or
  `telemetry.forwarding` is never reported, although `meta.schema_validation.warn_unknown_keys` defaults true.
- **Reachability:** anyone configuring REST auth from CLAUDE.md, PROJECT_SETUP.md or
  BRANCH-REPORT.md. No config in the tree uses `api.auth`.
- **History:** never built (A15). The text needs fixing either way. The open question is
  whether the engine should refuse or warn on a nested unknown key under `api`.

### B2. Telemetry forwarding cannot be the mitigation CLAUDE.md names for a missing `RunEnd` (L257, L312, L316)
1. **The documented schema forwards nothing (verified).** I used a local HTTP listener, with a program that emits `GOVERNANCE_HEALTH_QUERY` live and then sleeps 3 s.
   - `telemetry.forwarding.webhook_url` → **0 POSTs**.
   - Flat `telemetry.webhook_url` (control) on the identical program → 1 POST.
2. **Even when configured correctly, the run anchors are never forwarded.**
   - *Traced:* `RunStart` is written inside `chainPrevLocked()` and `RunEnd` inside `emitRunEnd`, both via `checkedWrite` only (governance_reports.cpp:257–300 and 315–333). Neither reaches `fwd->enqueue` (the enqueue sites are :631, :723, :1594, :1665, :1773).
   - *Observed, n=1:* the file held RunStart, GOVERNANCE_HEALTH_QUERY and RunEnd. The webhook received only the health query, although RunStart was written about 2 s before exit.
   - So a SIEM never sees any `RunEnd`, and the "advisory warning for a final run with no `RunEnd` (crash-indistinguishable; SIEM forwarding is the mitigation)" cannot be resolved from the forwarded stream.
3. **The final drain never runs on a clean exit either (traced only).**
   - `TelemetryForwarder::shutdown()` is called only from `~GovernanceEngine()`.
   - The CLI run path ends in `_exit(0)` (main.cpp:2910) while `shared_governance` (main.cpp:1463) is still in scope.
   - docs/open-investigations.md F53 records this only for the HARD-block `_exit(3)`; it applies to every CLI exit.
   - Whether events are actually lost is a race with the worker thread. My one short run still forwarded its single live event, so I have not demonstrated loss.

### B3. A PII match in an agent response is not a HARD block (L214)
- **Verified, one run per arm.** Used an agent stub; the script wraps `agent.send` in try/catch.
  - `no_pii` enabled, response containing `123-45-6789`: the response is **returned to the script**, exit 0, with only `[governance] WARNING code_quality.no_pii … [ADVISORY]`.
  - Control: `no_secrets` with an AWS key in the response → exit 3, uncatchable.
  - Clean response → returned normally.
- `NoPiiConfig.level` defaults to `ADVISORY` (governance.h:576). The sentence "When enabled it is a HARD block (exit 3, uncatchable), not advisory" is true only for `no_secrets` at its default level.
- **History:** never true for PII. ADVISORY dates from bcf647a0 (Feb); the sentence is from 252e9d5b (#124, Aug). Its "Verified:" clause only tested `no_secrets`.
- Not tested: non-default `no_secrets` levels.

### B4. The test CLAUDE.md cites for per-sighting entity scoring never runs its assertions (L163)
- `test_challenge_fail_path.sh` **Group I**: I-01, I-02 and I-03 print `SKIP … No entity challenge fired — staging did not reach step-up`.
- **I-04** sits inside the same skipped `else` branch and never executes.
- The suite still prints `ALL PASSED`. This happened in **4 of 4 runs** here (once in the full suite, three times standalone).
- docs/governance-campaign-findings.md:161–162 cites the same Group I and I-04 as the regression tests for `7c6320b` and `8a6b00d`.
- **Mechanism, verified (one run per arm):**
  - Group I's config does not set `adaptive_baseline_enabled`. The default flipped to `true` in e7f63246 (#167, 2026-08-23), after Group I was written (7c6320bf, 2026-07-25).
  - Inside the 5-turn baseline window, S15's firings are absorbed: `penalties_detail` is empty and pressure stays at `0.0000`. The level never reaches ELEVATED within the fixture's 6 sends.
  - The same config with `adaptive_baseline_enabled: false` produces **4 entity challenges, `keyword_ratio=1.000000`, `expected_keyword_mode=per_sighting_best`**.
- **Classification:** the test regressed at #167; the feature itself behaved correctly in my one control run.
- The skip is deterministic logic, not platform. CI logs were not inspected.

### B5. The ReDoS crash class is live by default, outside the tables the gotcha names (L412)
- **Verified, n=1 per arm.** A `<<python>>` block containing a single line of about 40 KB (also 200 KB) **segfaults the interpreter (exit 139) on both engines** under a plain `{"mode":"enforce"}` config. No verdict is rendered.
- **Controls:**
  - A 100-character line passes (rc 0).
  - A real `subprocess.call(…, shell=True)` is HARD-blocked (rc 3).
- **gdb, about 78k frames:** `std::regex` `_M_dfs` recursion ← `analyzer::SyntacticAnalyzer::detectImports` (src/analyzer/syntactic_analyzer.cpp; patterns such as `\w+\.\w+\s*\(`) ← `ComprehensiveTaskDetector::analyze` ← `checkPolyglotOptimization` ← `checkPolyglotBlock` ← `VM::run`.
- The path is on by default: `polyglot_optimization.enabled = true` (governance.h:2401).
- My first attribution, `DANGEROUS_PATTERNS_DB`'s `.*` entries, was **wrong**. The crash reproduced with `dangerous_calls` absent and with no `subprocess.call(` literal in the input.
- CLAUDE.md's rule ("never use an unbounded `.*` / `[\s\S]*` between two required literals", scoped to three tables) describes the hazard too narrowly. Under libstdc++, **any unbounded repetition over a long run** recurses per character.
- Not tested: whether `codegen.run` and inline `process.run` code reach the same analyzer. L413 says inline code goes through `checkPolyglotBlock()`, so it probably does (screened).

### B6. The `api_base` loopback exception is a string-prefix check (L276)
- **Verified, n=1.**
  - `"api_base": "http://127.0.0.1@192.0.2.2:PORT"` is accepted (no "api_base ignored" warning). The agent's request, **with its `x-goog-api-key` header**, arrived over plaintext HTTP at a listener bound to 192.0.2.2 (`Host: 192.0.2.2:PORT`).
  - Control: `http://192.0.2.2:PORT` is rejected with the warning and never contacted.
- The check is `rfind("http://127.0.0.1", 0) == 0` and similar (governance_config.cpp:2732–2735). By the same logic `http://localhost.example.com/` would pass (screened; not run, needs DNS).
- So "https only, except loopback" is false at the edge. The impact is an API key sent in cleartext to a non-loopback host whenever an attacker or an LLM can influence `api_base` in govern.json.

### B7. The error-message security rule is broader than what is enforced (L343)
- L343 says error messages must "NEVER leak … config key paths, or governance internals" and that `test_error_msg_leaks.sh` "enforces this".
- The test is a **45-entry deny-list over 19 files** (bypass flags, three `taint_tracking.*` keys, some internals).
- Governance errors print config key paths by design:
  - Observed: `Rule: code_quality.no_secrets = <redacted>` and "This is enforced by governance code_quality.no_secrets".
  - Traced: `checkShellAllowed()` prints `agents.<id>.allowed_actions` and "Add SHELL_EXEC to the allowed_actions list to permit shell execution".
- Either the rule should be narrowed to what is intended, or the engine violates it. That is a decision, not a text tweak.

### B8. (minor) Not every `dead_keys` read goes through `isKeyDead()` (L160)
- `agent.dispatch_status()` iterates `s_dispatch.dead_keys` directly (agent_impl.cpp:7290–7293).
- Consequence (traced, not run): a key past `key_retry_after_seconds` that has not been re-consulted is still reported as dead. `isKeyDead()` erases it only when called.

---

## C. Incidental (not CLAUDE.md claims; found on the way, not investigated)

- The `no_secrets` error help text recommends `env.get_var("YOUR_KEY_NAME")`. `env_impl.cpp:483` rejects `get_var` as an unknown function. Observed in the probe output; traced.
- Suites that report SKIP for reasons this build contradicts, yet count as passed:
  - "No executor found for language: js" (the registered name is `javascript`).
  - "OpenSSL not compiled in" (cmake: ✓ OpenSSL 3.0.13).
  - "Python not available" SKIPs on a pybind11 build.
  - These look like broken probes (screened).
- `baselines.auto_record`: `b.json` was never written on either engine (n=1 each). Two facts are traced: `saveBaselines()` is called only from `~GovernanceEngine()`, which the CLI's `_exit(0)` skips; and `checkBaseline()` is called only from the tree-walker (polyglot.cpp:777). **No positive control**, so it is unmeasured whether recording or persistence is what fails.

---

## D. Checked and holding (so the next pass need not redo them)

**Confirmed by running something** (the agent stub, a probe, or a suite):
- 874 leak checks, 0 fail.
- 580 unit tests pass, and the 9 `known_failures.txt` exclusions still fail for their recorded reason.
- `test_transcript.sh`: 28 assertions.
- `naabfuzz selftest`: 25 OK.
- Division `7/2` = `3.5` on both engines; `math.abs` → float; `-2147483648` → float; errors print to stdout with an `Error:` prefix.
- On both engines: bare dict keys, `??` inside match arms, `"hello".slice(-3)` / `string.slice` = `llo`, and `array.sorted` being non-mutating.
- The tree-walker rejects `arr.sorted()`; the VM accepts it.
- A top-level `const` is a parse error.
- A malformed govern.json exits 4.
- The sandbox upgrade log line, verbatim.
- A SOFT block exits 3 without the override and 0 with `--governance-override`.
- Every event carries `run_id`, and `--verify-telemetry-chain` verifies a clean file.

**Confirmed against source:**
- 23 CDD signals.
- Every listed default (traced against initialisers and the parser):
  - `adaptive_baseline_enabled` true, window 5;
  - `max_challenge_failures` 1, `max_quarantine_streak` 5, `on_undetermined` "pass";
  - thresholds 0.4 / 0.6 / 0.8, `deescalate_sustained` 2;
  - `no_secrets` / `no_pii` false, `thinking_budget` -1;
  - `rate_normalized_floor` 0.5, `validation_recovery_amount` 0.075;
  - `vocab_contraction_agent_events_only` true, `exclude_infrastructure_errors` true;
  - pool size 6, extends depth 5 / "replace", temporal_coupling ADVISORY.
- All 16 signal weights and thresholds S5–S23.
- S11 and S19 windows of 20.
- `kStopWords` has 55 entries.
- 19 scanner checks.
- 3 `infrastructure:` sites.
- 9 env access points.
- The VM's dispatch loop + 3 inner `GovernanceHardError` rethrows. Two more sit in `callBuiltinFunction`; that is not a contradiction.
- `flushGroupedAdvisories` at 4 catch sites.
- The RLIMIT_DATA / AS ×4 ≥ 2 GB containment.
- Propose temperature +0.15, capped at 1.5.
- The persona-override text.
- The S12 EMA gate.
- `consecutive_passes` advances only in `computePulseVerdict`.
- The 3-turn suppression after recovery.
- Evidence-ratchet entries.
- `telemetry.transcript` location.
- `cdd_snapshot` sites.
- The CamelCase engine events.
- `content_hash` is computed over the first 500 chars.
- `key_health` fields.
- The `trace` and `usage` fields.
- The AGENT_SEND dispatch-table list.
- The template `researcher` has empty `allowed_actions` and `shell_allowed: false`.

**Every cited `Test:` file** exists and is registered: governance_v4 through the glob sweep, the rest by name. Every one printed `ALL PASSED` except `test_r22_fixes.sh` (A4) and `test_signal_contract.sh`, which is intentionally in the sweep skip-list and is not cited as passing.

---

## E. Blind spots — what this audit did not establish

- **Identifier existence** (about 1,000 backticked names) was a word-level grep over src/include/tests/tools, so it is screened. A name that exists can still be misdescribed.
- **Config keys:** I checked that each leaf is parsed as a quoted key. Nesting was traced only for the keys in A and B.
- **Live-API measurements** were not re-measured: runs 7–23, "31/40 vs 0/40", "VC-08 20→5", "repo-sentinel round 7". They are covered only insofar as their cited suites pass.
- **Windows/MSYS claims** were not checked. CI logs were not inspected.
- **Suite "ALL PASSED" lines** were taken at face value except where SKIP lines were visible. Of the SKIP lines in the log, I traced only Group I (B4). Others may hide the same shape.
- **Re-tracing:**
  - B1, B2(1), B3 and B6: each rests on one run per arm plus its control.
  - B4: 4 runs plus the mechanism control.
  - B5: 3 sizes on the VM, 1 on the tree-walker, and the backtrace.
  - The adversarial pass changed two things. It dropped a planned "VM rethrow count is stale" item (the claim holds), and it corrected B5's attribution away from `DANGEROUS_PATTERNS_DB`.
