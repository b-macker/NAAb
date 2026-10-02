# agent_harness: a governed multi-agent harness, governance first

This is a multi-agent code-audit harness built the governance-first way:

1. `govern.json` was written first, section by section from
   `govern-template.json`, keeping only settings whose effect could be traced.
2. It was then signed.
3. The harness was written last, to fit the signed config.

Every setting the harness relies on has a test that breaks the rule and
expects a refusal. The code passes on the stub with no API key.

```
plan ──► work (tool loop, per step) ──► verify ──► review ──► judge ──► report
planner      worker + 2 tools           script    2 critics    judge    redacted file
```

| Stage | Who | Mechanism |
|---|---|---|
| plan | `planner` | `agent.propose(…, 3)` → `orchestra.select_admissible` → `agent.commit`. The committed plan's shape is then checked (`plan_problems`) and the result is recorded with `agent.record_validation`. |
| work | `worker` | `agent.send` with the read-only tools `list_files` and `read_file`, once per step. |
| verify | the script | `verify_claims()` checks every claimed file and defect line against `fixtures/truth.json`. The outcome goes to governance through `agent.record_validation`, which feeds CDD signal S22. Ground truth, not opinion. |
| review | 2 × `critic` | `agent.fan_out` and `orchestra.consensus_vote`. |
| judge | `judge` | The final verdict, as JSON with a regex-checked `verdict`. |
| report | the script | `sanitize_report()` redacts and bounds the text, then `file.write("out/report.json")`. |

## The order of work, and why it matters

A harness written first tends to get a config sized to whatever the code
already does, and the config ends up describing the code instead of
constraining it. Here the order is reversed, and the engine enforces it:

1. **Write governance.** Start from `govern-template.json`, decide each
   section for this harness, and record why (`_comment_*` keys). Settings
   that have no effect were dropped (see *Considered and omitted* below).
2. **Check the keys.** Run `python3 tools/check_govern_keys.py src/govern.json`.
   The engine warns about unknown *top-level* keys only, so a misspelled
   nested key does nothing and says nothing.
3. **Sign.** Run `tools/sign.sh`. It refuses to sign a config with unread
   keys, signs, and then verifies the result the way a run will.
4. **Build to fit.** Write the code. When governance refuses something, the
   question is whether the code is wrong or the rule is. Answer it with a
   test before changing the rule, and re-sign after any change. Examples
   from building this harness:
   - A variable named `draft` is a placeholder marker, so it became
     `summary`.
   - The word "skeleton" in a comment was refused, so the code was made
     complete rather than annotated as unfinished.
   - Prose intents failed `intent_validation`, so they were rewritten as the
     concrete calls each function must make.
5. **Prove each rule bites.** See
   `tests/governance_v4/test_agent_harness_example.sh`.

## Layout

```
agent_harness/
├── run.sh                       lock check, then one fresh out/run-<ts>-<pid>/ per run
├── src/harness.naab             the pipeline (ONE file -- see Build rules)
├── src/govern.json              the governance, commented section by section
├── src/govern.json.sig          its Ed25519 signature
├── keys/harness-signing.pub     the public key that signature must verify against
├── tools/sign.sh                key check -> sign -> verify (you hold the private key)
├── tools/lock_check.sh          LOCK|ok, LOCK|stale or LOCK|broken
├── tools/check_govern_keys.py   flags govern.json keys no parser reads
├── workspace/                   what the worker audits (3 planted defects)
└── fixtures/                    task.json, truth.json (ground truth), stub_responses.json
```

## Running

```bash
# from the repo root, after building build/naab-lang
examples/agent_harness/run.sh --stub     # deterministic, no key
examples/agent_harness/run.sh --live     # real Gemini; needs GEMINI_API_KEY
```

A green run prints:

```
LOCK|ok
PLAN|steps=3
STEP|1|passed=true
STEP|2|passed=true
STEP|3|passed=true
REVIEW|APPROVED
VERDICT|APPROVED
REPORT|bytes=…
```

**The lock.** Before anything else, `run.sh` checks that `src/govern.json`
still verifies against `keys/harness-signing.pub`. It does this in a
throwaway trust store, so your own trust store cannot affect the answer.

- If the config was edited after signing, or signed with another key, the
  run does not start.
- A *stale* signature is one older than `trust.max_signature_age_days` (30
  days). It stops `--live`, where the engine refuses it anyway, and is
  reported in `--stub`.
- The run itself uses a trust store that holds exactly one key, and
  `NAAB_SIGNING_KEY` is never exported to it.

**`--stub`** changes one setting in the run's copy: `api_base` on every
agent, pointed at the local stub. Because the copy no longer matches the
committed signature, it is re-signed with a key generated for that run and
then deleted. Verification, flag locks and staleness still apply; only the
key differs. The stub routes requests by the `AH-<ROLE>` token at the start
of each system prompt.

**Taking ownership of the lock.** The committed signature was made with a
key that exists nowhere now; only the public half is in `keys/`. To own the
lock, run `tools/sign.sh`. It creates `~/.naab/agent_harness/signing-key.pem`
if needed, replaces `keys/harness-signing.pub` with your public key, and
re-signs. Do this again after every governance change and at least every 30
days. The key must live outside this directory, and `sign.sh` refuses
otherwise.

Each run directory holds:
- `report.json`;
- `telemetry.jsonl`, hash-chained (check it with
  `naab-lang --verify-telemetry-chain`);
- `transcript.jsonl`, with every prompt, response and tool call (treat it as
  sensitive);
- `governance-report.json` and `governance.sarif`;
- `stdout.txt` and `stderr.txt`.

## govern.json, setting by setting

Every section has a `_comment_*` key. The **Proof** column names the test
arm that breaks the rule. A setting with no arm is either covered by every
green run or listed as not independently measured.

| Section | Setting | What it does here | Proof |
|---|---|---|---|
| `governance`, `runtime` | dashboard, explanations, override reasons required, JSON + SARIF reports; 600 s wall clock | Config equivalents of CLI flags, so the locked file decides them. | every run |
| `integrity.blocked_flags` | `--tree-walk`, `--no-governance`, `--governance-override`, `--sandbox-level`, `--timeout`, `--env`, baseline-save flags | No command-line loosening. `--tree-walk` is locked because taint through a function argument is enforced on the VM only. The locks apply to the key holder too. | AH-11 |
| `trust` | 30 days, `hard` | A signature older than 30 days stops the run (authority decay). | AH-L3, `test_signature_staleness.sh` |
| `contradiction_detection` | `max_level: hard` | A config that contradicts itself does not run. CONTRA-013 is hard-coded to advisory and prints as noise here, because every language is blocked. | measured (audit level `full` with no file → HARD) |
| `prerequisites` | `GEMINI_API_KEY` exists, `hard` | Fails before the first model call, not on it. | AH-18 |
| `security.sandbox_level` | `standard` | No absolute paths and no fork/exec for NAAb code. Not `restricted`: that removes FS_READ outright, and `filesystem.mode` cannot give it back. | every run |
| `languages.blocked` | every registered language | No polyglot code. An in-process `<<python>>` block would read paths that `file.read()` is denied. `allowed: []` would mean *allow all*. | AH-05 |
| `codegen.enabled` | `false` | Model output is data, never code. | — |
| `capabilities.network` | `enabled: false` | `http.*` is a HARD block, even for a function granted NET_CONNECT. Agent provider calls are made by the engine and governed per agent, not by this switch (measured). | AH-17 |
| `capabilities.shell` | `enabled: false` | Removes SYS_EXEC at the sandbox layer, the only containment that also stops a renamed binary. | — |
| `capabilities.filesystem` | three `allowed_paths`; the evidence files in `blocked_paths` | Stdlib file access stays in workspace, fixtures and out, and the harness cannot rewrite its own telemetry, transcript or audit trail. govern.json, its `.sig` and the trusted keys are blocked automatically. | AH-16 |
| `capabilities.env_vars` | `read`/`write` false | NAAb code never touches the environment; the engine reads the provider key itself. At `standard` the sandbox refuses `env.get` first (exit 1), so this is defence in depth. | measured |
| `capabilities.functions` | `level: hard`, `default: []`, one entry per effectful function | Least privilege per function, **intersected down the call stack**. Pure functions (`verify_claims`, `plan_problems`, `sanitize_report`) get nothing. `main {}` runs under `default`. A renamed function falls back to `default`. | AH-04 |
| `limits` | call depth 64, 100k loop iterations, array/JSON caps, stdlib/file rate | Bounds on the program itself. Only enforced limits are set. | measured (loop) |
| `requirements.main_block` | `hard` | The program must have a `main` block. | — |
| `restrictions` | `dangerous_calls`, `information_disclosure` | The two restriction families that run on this harness's own calls; both appear in its governance report. | report |
| `taint_tracking` | sinks `file.write`/`append`, `io.write_file`; sanitizer `sanitize_`; lineage | Model output reaches disk only through `sanitize_report`. Agent output is tainted whatever `sources` says, and a dict holding any tainted value is tainted as a whole. | AH-03, AH-03c |
| `code_quality` (responses) | `no_secrets`, `no_pii` at hard | A response carrying a secret or PII ends the run. Both are OFF by default. | AH-06 |
| `code_quality` (source) | `no_placeholders`, `no_oversimplification`, `no_incomplete_logic` | Placeholder markers (including in comments), hollow bodies, swallowed errors, and a "sanitizer" with no sanitizing in it. | measured |
| `code_quality.complexity_floor` | `verify_*` needs score 25 and real branching | A hollowed-out verifier is refused. | AH-14 |
| `code_quality.intent_validation` | project intent plus one intent per function | Each function must carry out its declared intent, written as the calls it makes. | AH-15 |
| `code_quality.duplicate_calls` | threshold 3 | Advisory. It flags repeated calls even when the arguments differ (see findings). | — |
| `contracts` | `must_call`, `must_produce`, arity, return shape | `run_step` must verify and record; `make_plan` must propose, select, commit, check and record; `write_report` must sanitize. Golden tests pin `verify_claims`, `plan_problems` and `sanitize_report`, so a verifier that accepts everything fails. Tool signatures are pinned to their schemas. | AH-02, AH-12, AH-13 |
| `agents.*` | per-role prompt, budgets, `allowed_actions`, response `blocked_paths`, output contracts, retry, standing leases, `thinking_budget: 0` | Dual-gated tools, malformed output refused before parsing, no role may name the governance files. | AH-07, AH-09 |
| `agent_dispatch` | 60 calls, 300k tokens, 10 minutes, 3 consecutive failures | A run-level spend ceiling, whatever the script does. | — |
| `exposure_tracking` | 5 handles, 60 actions, coherence floor 0.3, `hard` | `max_unique_agents` counts **handles**: 1 + 1 + 2 critics + 1 = 5. | measured (a sixth create is refused) |
| `pipeline_separation` | `hard` | Unused today (no `agent.pipeline`). It is set so a pipeline added later starts out governed. | — |
| `behavioral_sequences` | enabled, cross-agent, built-in patterns | Detects exfiltration, env harvesting, shell escape and cross-agent relay. Defining any pattern would replace all the defaults. It also feeds CDD: with it off, drift scores nothing. | — |
| `temporal_coupling` | advisory | Flags suspiciously correlated agent timing. Advisory because the critics run concurrently by design. | — |
| `context_drift` | every turn, baseline window 3, absorption cap 6, `response_degenerate` on | 23 drift signals per agent turn, including S22 (ground truth). | AH-08 |
| `circuit_breaker` | quarantine at 0.60, streak 3 with corroboration 2, step-up (contextual, coherence-floor trigger), mandate reinforcement, graduated correction | Steers before blocking, and blocks on corroborated evidence. `on_undetermined: pass` is explained under *Limits*. | — |
| `advisory_escalation` | `soft_after: 3` | A repeated advisory becomes a HARD block. The count is per rule name across all agents. | — |
| `governance_health` | `check_after_turns: 5` | Pulse and instrumentation checks fire within a 7-turn run. | — |
| `quality_gate` | any hard, soft or security finding → exit 2 | A run that got to the end with findings still fails. | — |
| `audit` | `full`, `out/audit.jsonl`, hash-chained | Records overrides, reloads, attestations and taint decisions. A clean run writes nothing to it. | — |
| `telemetry` | chain, decision snapshots, transcript | Replayable, tamper-evident evidence. | every run |

### Considered and omitted

These sections or keys from `govern-template.json` are absent on purpose.
Absent settings take the engine defaults.

- **No effect for this harness (measured).**
  - The polyglot-only checks:
    - `code_quality.no_dead_code`, `no_mock_data`, `encoding`,
      `no_path_traversal`, `no_hardcoded_urls`/`ips`/`results`,
      `no_debug_artifacts`, `no_temporary_code`, `no_simulation_markers`,
      `no_unsafe_deserialization`, `no_hallucinated_apis`;
    - `restrictions.code_injection`, `privilege_escalation`,
      `data_exfiltration`, `obfuscation`, `vcs_secret_extraction`,
      `resource_abuse`;
    - `custom_rules`, which `naab-lang` applies to polyglot blocks only.

    Every language is blocked, so none of these ever runs. Each was checked
    by planting the violation in NAAb code and watching it pass.
  - `requirements.naming_conventions`: nothing reads it for NAAb code (a
    camelCase variable passes).
- **Read by nothing.**
  - `requirements.strict_types`, `no_global_state`, `documentation` and
    `version_pinning`.
  - The `.enabled` keys inside `requirements` (presence enables).
  - `languages.per_language.*`.
  - `capabilities.process.*` (the engine warns), `capabilities.time`,
    `capabilities.memory`.
  - `capabilities.filesystem.allowed_extensions`, `max_file_size`,
    `max_files`, `blocked_extensions` (warned); `allow_hidden_files` and
    `allow_absolute_paths` (warned when false).
  - The top-level `subprocess_*` keys: the real ones live under
    `capabilities.env_vars`.
  - `limits.data.string_length`, `dict_size`, `nesting_depth` (warned),
    `limits.data.input_size`, `limits.execution.total_executions`.
  - Top-level `scopes`, which is parsed by nothing.
  - `trust.require_fresh_signature`, `check_key_expiry` and
    `check_revocation`.
  - `meta.schema_validation.*`, `orchestra`, `scanner` (as a
    `govern.json` section), and the agent keys `response_format` (only
    echoed back to the script), `stream` and `stop_reason_action`.
- **Not applicable to this harness.**
  - `extends`/`meta.inheritance`: one file, signed as a unit.
  - `environments`: it can only change `mode`, report paths and the
    quality/baseline switches, and `--env` is locked.
  - `hooks`: they spawn external programs.
  - `api`, `approval`, `runtime_versions`, `polyglot*`, `baselines`,
    `project_context`, `scoring`, `scoring_calibration`.
  - `governance_baseline` and `code_quality.drift_detection`: these work
    across runs, while each run here gets a fresh directory.
  - `agent_review`: it adds LLM calls to review the script itself.
  - `governance_plugins`: these are the extension point for adding
    NAAb-source rules the engine lacks (naming, for instance). That is the
    next step if a rule is needed.

## Proof: `tests/governance_v4/test_agent_harness_example.sh`

Each run arm patches a copy of the example, re-signs it with a test key and
runs it. AH-01 is the same signed config unpatched, so a refusal elsewhere
cannot simply mean the fixture is broken.

| Arm | Change | Expected |
|---|---|---|
| AH-L1 | none | The committed signature verifies (`ok`, or `stale` once it is more than 30 days old). |
| AH-L2 | one value edited in govern.json | `LOCK\|broken` |
| AH-L3 | a valid signature dated 90 days back | `LOCK\|stale`, not broken |
| AH-L4 | `run.sh` on the broken copy | refuses before creating a run |
| AH-01 *control* | none | the green run above |
| AH-11 | `--tree-walk` | INTEGRITY BLOCK, flag locked |
| AH-02 | delete `record_validation` from `run_step` | contract violation, rc 3 |
| AH-03 / AH-03c | unsanitized write inside `write_report` / the same with taint off | refused / succeeds |
| AH-04 | `file.read` inside the pure verifier | `Undeclared action in 'verify_claims'` |
| AH-05 | a `<<python>>` block | language blocked |
| AH-06 | the worker response carries an AWS key | `no_secrets`, rc 3 |
| AH-07 | worker JSON missing `defects` | output contract violation |
| AH-08 | the worker invents a defect | step fails, `VALIDATION_RECORDED passed=false`, S22 penalty |
| AH-09 | the model calls a registered tool that is not listed | `AGENT_TOOL_BLOCKED`, never executed |
| AH-12 | the plan check accepts every plan | `must_produce` golden test fails |
| AH-13 | the verifier accepts every defect | `must_produce` golden test fails |
| AH-14 | a hollow verifier (the checks that would fire first are turned off) | complexity floor |
| AH-15 | a declared intent the code does not carry out | intent mismatch |
| AH-16 | the harness writes its own telemetry file | path blocked |
| AH-17 | `http.get` with NET_CONNECT granted | network blocked |
| AH-18 | a required environment variable is missing | prerequisite failed |
| AH-10 | key checker on the shipped config / on a typo | pass / flags the typo |

## Build rules

1. **Governance changes first, and they are signed.** Edit `src/govern.json`,
   run `tools/sign.sh`, and only then change the code that needed it.
2. **Ground truth, not opinion.** A step passes because `verify_claims`
   checked it against known facts. For a new task, write the truth fixture
   first.
3. **Do not tune to the stub.** The stub's answers exercise the governance
   path. A real model must pass the same checks.
4. **Do not loosen a rule to make a stage run.** Find out why it refused.
   Every caveat in the table above was found that way, and so were three
   engine defects.
5. **One file.** Function-body checks (contracts, capability entries,
   intents) are skipped for code loaded as a module.
6. **Every effect in a named function, every function in the config.** A
   new function needs a capability entry (or runs with none), an intent, and
   a contract if it is load-bearing.
7. **Every new rule gets an arm** in the test, alongside the AH-01 control.

## Acceptance criteria for a production harness

- `tools/lock_check.sh` prints `LOCK|ok` with **your** key.
- `run.sh --stub` is green and every arm of the test passes.
- `run.sh --live` completes on a real model with `STEP|*|passed=true` for
  the planted defects. With a deliberately wrong `truth.json` it reports
  `passed=false`, which shows the verifier is load-bearing.
- `naab-lang --verify-telemetry-chain out/run-*/out/telemetry.jsonl` is
  clean.

## Limits

- **Single-turn roles are not judged by CDD.**
  - The planner, critics and judge take one or two turns, which is fewer
    than the baseline window. Their output-admissibility passes are
    therefore *undetermined*, and `GOVERNANCE_HEALTH_WARNING` says so.
  - Their protection is the output contracts, ground truth, the plan check
    and the two-critic consensus.
  - `on_undetermined: "quarantine"` would hold every answer they give.
- **Contract checks run at the end of the run.** `must_produce` golden tests
  run after `main` returns, so a failing one exits 3 *after* the pipeline
  printed its results, and after a `Governance: PASS` banner. Read the exit
  code, not the banner.
- **Static checks are textual.** A `return` placed before the real logic
  leaves that logic in the text: the complexity floor and the
  cosmetic-sanitizer check still see it. The golden tests are what catch
  that.
- **CONTRA-013 prints on every run.** It is noise for this config, but the
  engine does not consider `languages.blocked`.
- **The stub tests the governance path, not model quality.** Only `--live`
  says whether a real model can do the task.

## Engine findings from building this

Fixed alongside this example (each with a regression test):

- **Signature age was never checked at startup.** `loadFromFile()` verified
  the signature before installing the parsed rules, so the age check read
  the empty pre-load `trust` policy. A valid 90-day-old signature under a
  30-day HARD limit loaded with exit 0.
  (`tests/governance_v4/test_signature_staleness.sh`; the old gorilla arm
  backdated the file's mtime, which the engine never reads.)
- **`must_produce` compared dicts by their printed form.** Key order depends
  on how each map was built, so a verifier that returned exactly the
  expected dict was blocked HARD. Containers are now compared structurally;
  list order and scalar comparison are unchanged.
  (`tests/governance_v4/test_must_produce_dict_order.sh`.)

Reported, not changed here:

- **Owner authority never lifts a flag lock.** `main.cpp` expects a
  signature of the form `ed25519:<b64>`, but signatures are
  `ed25519:<b64>:<timestamp>` over content plus timestamp, so the check
  never verifies. This fails closed.
- **Parsed but never read:** `trust.require_fresh_signature`,
  `check_key_expiry`, `check_revocation`; `capabilities.filesystem.allowed_extensions`,
  `max_file_size`, `max_files` (unlike their siblings, these carry no
  warning); top-level `scopes`; `limits.data.input_size`;
  `limits.execution.total_executions`; `requirements.naming_conventions` for
  NAAb code; `meta.schema_validation.warn_unknown_keys`.
- **`must_satisfy` gaps.** It compares numbers only and uses only the first
  test case. It also skips an expression it cannot parse without saying so.
- **Duplicate-call advisory ignores arguments.** `truth.get("files")` and
  `truth.get("defects")` count as one call made three times.
- **The tree-walker misses taint through function arguments.** A tainted
  value passed into a function and written there is caught on the VM but
  not by `--tree-walk`, which is why this config locks that flag.
- **`govern-template.json` has keys no parser reads, and keys in the wrong
  place.**
  - `check_govern_keys.py` reports 145 unread keys, an upper bound.
  - `project_intent`/`function_intents` sit directly under `code_quality`,
    but are read only under `code_quality.intent_validation`.
  - The `subprocess_*` keys are top-level, but are read only under
    `capabilities.env_vars`.
