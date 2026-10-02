# agent_harness — a governed multi-agent harness skeleton

A starting point for building a real agent harness on NAAb. It is a pipeline
that runs end to end, a `govern.json` tailored to it with every setting
explained, and a local stub so that it runs without an API key. Every setting
it relies on has a test that shows the setting refusing something.

```
plan ──► work (tool loop, per step) ──► verify ──► review ──► judge ──► report
planner      worker + 2 tools           script    2 critics    judge    sanitized file
```

| Stage | Who | Mechanism |
|---|---|---|
| plan | `planner` | `agent.propose(…, 3)` → `orchestra.select_admissible` → `agent.commit`. Three candidates are generated, one admissible candidate is selected, and only that one enters history. |
| work | `worker` | `agent.send` with tools `list_files` / `read_file` (read-only, confined to `workspace/`), once per plan step. |
| verify | the script | `verify_claims()` checks every claimed file and defect line against `fixtures/truth.json`, then `agent.record_validation(worker, passed, detail)` feeds the result to CDD signal S22. The verdict comes from ground truth, not from another model. |
| review | 2 × `critic` | `agent.fan_out` + `orchestra.consensus_vote`. |
| judge | `judge` | Final verdict, JSON with a regex-checked `verdict` field. |
| report | the script | `sanitize_report()` → `file.write("out/report.json")`. |

## Layout

```
agent_harness/
├── run.sh                    run in a fresh out/run-<ts>-<pid>/ directory
├── src/harness.naab          the pipeline (ONE file — see "Build rules")
├── src/govern.json           the governance, commented section by section
├── workspace/                what the worker audits (3 planted defects)
├── fixtures/task.json        the task
├── fixtures/truth.json       ground truth for verify_claims()
├── fixtures/stub_responses.json  scripted model, routed per role
└── tools/check_govern_keys.py    flags govern.json keys the engine never reads
```

## Running

```bash
# from the repo root, after building build/naab-lang
examples/agent_harness/run.sh --stub     # deterministic, no key
examples/agent_harness/run.sh --live     # real Gemini; needs GEMINI_API_KEY
```

A green stub run prints:

```
PLAN|steps=3
STEP|1|passed=true
STEP|2|passed=true
STEP|3|passed=true
REVIEW|APPROVED
VERDICT|APPROVED
REPORT|bytes=…
```

Each run's directory holds `report.json`, `telemetry.jsonl` (hash-chained;
check it with `naab-lang --verify-telemetry-chain`), `transcript.jsonl`
(every prompt, response and tool call), `stdout.txt` and `stderr.txt`. Runs
never share files. Telemetry appends by design, so two runs writing to one
file would make per-run counts cumulative.

`--stub` changes exactly one thing in the run's copy of `govern.json`: it
sets `api_base` on every agent to the local stub. It also points
`NAAB_TRUST_STORE_DIR` at a temporary directory, so the unsigned copy does
not trip an integrity block. Every other setting is the one `--live` uses.
The stub routes requests by the `AH-<ROLE>` token at the start of each
system prompt, so keep those tokens if you edit the prompts.

**Live with a trust store installed:** sign the config first
(`NAAB_SIGNING_KEY=<private key> naab-lang --sign-governance src/govern.json`;
`run.sh --live` copies the `.sig` sidecar into the run directory). An unsigned config under
trusted keys is an INTEGRITY BLOCK by design.

**Checking keys after any edit to govern.json:**

```bash
python3 examples/agent_harness/tools/check_govern_keys.py examples/agent_harness/src/govern.json
```

The engine ignores unknown keys without saying so.
`meta.schema_validation.warn_unknown_keys` is parsed but never consulted, so
setting it does not help. A misspelled key looks exactly like a working one
until something fails to happen. The checker reports any key that the parser
for its section never mentions. A key it accepts is only *read*, not proven
to have an effect. Effects are proven by the behavioural test below.

## govern.json, setting by setting

Every section in `src/govern.json` carries a `_comment_*` key. This table
maps each setting to the part of the harness it protects. The **Caveat**
column is measured behaviour (on `b0a7c8d`), and some of it is not what the
key names suggest.

| Section | Setting | Protects | Caveat |
|---|---|---|---|
| `security` | `sandbox_level: standard` | No absolute paths, no fork or exec. | **Not** `restricted`: that level removes FS_READ outright, and `filesystem.mode` cannot give it back. The harness has to read its fixtures. |
| `languages` | `blocked`: every registered language | No polyglot code anywhere. At `standard`, an in-process `<<python>>` block can read paths that `file.read()` is denied. | `languages.allowed: []` means *unrestricted* (empty = unused), so use `blocked`. CONTRA-013 still prints an advisory, because it only consults `sandbox_level`. That is noise here. |
| `capabilities.shell` | `enabled: false` | Removes SYS_EXEC at the sandbox layer. This is the only containment that also stops a renamed binary. | — |
| `capabilities.filesystem` | `allowed_paths` workspace/fixtures/out | Stdlib file access is confined to those three directories. | Applies to NAAb's stdlib only, not to polyglot code (hence `languages`). |
| `capabilities.functions` | `level: hard`, `default: []`, one entry per function | Least privilege per function. Permissions are **intersected down the call stack**, so a narrow caller cannot be widened by the helper it calls. | `main {}` is not on the function stack and always resolves to `default`. No entry name targets it. Keep `main` to orchestration and put every effect in a named function. A renamed function falls back to `default` and gets nothing. |
| `taint_tracking` | sinks `file.write`/`append`, sanitizer `sanitize_` | Model output reaches disk only through `sanitize_report`. | Agent output is tainted whenever taint is on, whatever `sources` says. A dict holding *any* tainted value is tainted as a whole. Taint through a function **argument** is enforced on the VM (the default engine) but missed by `--tree-walk`. Run the harness on the VM. |
| `code_quality` | `no_secrets`, `no_pii` at hard | A response carrying a secret or PII ends the run. | OFF unless enabled. There is no response secret scanning by default. |
| `contracts` | `must_call` per function | `run_step` must report ground truth, `write_report` must sanitize, `make_plan` must select and commit. | A regex on the function's body text, not transitive. Moving the call into a helper fails the contract. That is intended. |
| `agents.*.output_contract` | JSON, required fields, `verdict` regex | Malformed worker, critic or judge output is refused before the script parses it. | Only `format: "json"` is validated. It applies on `agent.send`, not on `propose`/`commit`. A violation is a **catchable** error. |
| `agents.worker` | `tools`, tool budgets, `allowed_actions` incl. `TOOL_EXEC` | Tools are dual-gated: they must be listed here *and* registered in the script. Per-turn and per-loop budgets apply. | A tool registered but not listed is blocked (`AGENT_TOOL_BLOCKED`). |
| `agents.planner` | `propose_candidates_max: 3`, `standing_lease_turns` | Enables propose/commit, with commit gated by the lease. | 0 (the default) disables propose. |
| `agent_dispatch.hard_stop` | 60 calls / 300k tokens | A run-level spend ceiling, whatever the script does. | — |
| `exposure_tracking` | `max_unique_agents: 5` | No unbounded agent creation. | Counts **handles**, not roles: 1 + 1 + 2 critics + 1 = 5. |
| `behavioral_sequences` | `enabled` | Built-in exfiltration, shell-escape and cross-agent-relay patterns. | **Also starves CDD when off.** With this disabled, context drift scores nothing. |
| `context_drift` | every turn, baseline window 3 | 23 drift signals per agent turn, including S22 (ground truth). | Single-turn roles (planner commit, critics, judge) never leave calibration. A pass for them is labelled *undetermined* (see below). |
| `circuit_breaker.output_admissibility` | quarantine below 0.60, streak 3, corroboration 2 | Quarantines low-coherence responses and keeps them out of history. Ends an agent that keeps producing them, but only on corroborated evidence. | — |
| `circuit_breaker.step_up_*` | contextual challenges, 2-strike | Re-verifies a drifting agent. | — |
| `advisory_escalation` | `soft_after: 3` | A repeated advisory becomes a HARD block. | The count is per **rule name across all agents**, not per agent. |
| `governance_health` | `check_after_turns: 5` | Pulse verdict and health warnings. | At the default of 10, the 7-turn stub run never ran the check. |
| `telemetry` | chain, decision snapshots, transcript | Replayable, tamper-evident evidence of every decision. | The transcript holds full prompts and responses. Treat it as sensitive. |

Settings deliberately **not** set include `context_drift.signals.response_degenerate`
(default off; the judge is terse by design) and `on_undetermined` (default
`pass`; see limits). Absent settings take the engine defaults documented in
`CLAUDE.md`. The checker found 145 keys in `govern-template.json` that no
parser reads, so do not assume a key from the template does anything.

## Proof: `tests/governance_v4/test_agent_harness_example.sh`

Each arm patches a copy of the example and asserts that governance refuses
the patched harness. The arms marked *control* show the unpatched harness
works, so a refusal cannot pass simply because the fixture is broken.

| Arm | Change | Expected |
|---|---|---|
| AH-01 *control* | none | The green run above, rc 0. |
| AH-02 | delete `record_validation` from `run_step` | contract violation, rc 3 |
| AH-03 | write the report unsanitized inside `write_report` | taint violation |
| AH-03c *control* | same, taint disabled | write succeeds (AH-03 was taint, not something else) |
| AH-04 | `file.read` inside the pure `verify_claims` | `Undeclared action … FS_READ` |
| AH-05 | add a `<<python>>` block | language blocked |
| AH-06 | worker response carries an AWS key | `no_secrets`, rc 3 |
| AH-07 | worker JSON missing `defects` | output contract violation |
| AH-08 | worker claims a defect that is not there | `STEP|2|passed=false`, `VALIDATION_RECORDED passed=false`, S22 penalty |
| AH-09 | model calls a registered-but-unlisted tool | `AGENT_TOOL_BLOCKED`, no call executed |
| AH-10 | key checker on the shipped config and on a typo | pass / flags the typo |

## Build rules

These rules keep the harness honest as you replace the stubbed parts.

1. **Ground truth, not opinion.** A step passes because `verify_claims`
   checked it against known facts, never because a model said so. When you
   swap in a real task, write the truth fixture *first*.
2. **Do not tune to the stub.** The stub's answers are fixtures for the
   *governance* path. Make a real model pass the same checks, and do not
   loosen a check until it passes. If a check is wrong, change it with a
   failing test that shows why.
3. **Do not loosen governance to make a stage run.** If a setting refuses
   something, find out why first. Every caveat in the table above was found
   that way. Widening a capability entry is a governance change. Review it
   as one.
4. **One file.** Function-body checks (contracts, capability entries) are
   skipped for code loaded as a module. Splitting the harness into imports
   quietly exempts the moved code.
5. **Every effect in a named function.** `main` runs under `default`.
6. **Every new governed behaviour gets an arm** in the test above, with a
   control, because an arm that only expects refusal also passes when the
   fixture is broken.
7. **Run `check_govern_keys.py` after every govern.json edit.**

`BUILD:` comments in `harness.naab` mark where the skeleton is thin: plan
validation (`make_plan` should record a validation for the planner too) and
real redaction in `sanitize_report`.

## Acceptance criteria for a built harness

- `run.sh --stub` is green, and every arm of the test passes.
- `run.sh --live` completes on a real model with `STEP|*|passed=true` for
  the planted defects, and with a **wrong** truth fixture it reports
  `passed=false`. The second half shows the verifier is load-bearing.
- `naab-lang --verify-telemetry-chain out/run-*/out/telemetry.jsonl` is clean.
- No `capabilities.functions` entry grants more than its function uses.

## Honest limits

- **Single-turn roles are not judged by CDD.** Planner, critics and judge
  take one or two turns, which is fewer than the baseline window, so their
  output-admissibility passes are *undetermined* (`GOVERNANCE_HEALTH_WARNING`
  says so). Their protection is the output contracts, ground-truth
  verification and the two-critic consensus. Setting
  `output_admissibility.on_undetermined: "quarantine"` would hold every one of
  their responses.
- **CONTRA-013 prints an advisory on every run.** It is noise for this config
  (languages are blocked), but the engine does not know that.
- **Taint is VM-only for function-argument flows** (see the table). The
  default engine is the VM.
- **The stub tests the governance path, not model quality.** Only `--live`
  says anything about whether a real model can do the task.
