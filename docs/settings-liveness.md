# Setting liveness: does any test notice when a govern.json setting stops working?

The dead-interpreter gate asks whether each *test* can fail. This asks the same
of each *setting* in `govern-template.json`: drop it, so the engine falls back
to its default, and see whether any test that uses it notices.

Measured 2026-10-03. Full per-setting data: [`settings-liveness/results-2026-10-03.json`](settings-liveness/results-2026-10-03.json).

## How it is measured

`loadFromJson()` in `src/runtime/governance_config.cpp` is the one function
every config load passes through: file, `extends` chain, inline string, mid-run
reload. A **test build only** (`cmake -DNAAB_CONFIG_MUTATION=ON`) reads three
variables there:

| Variable | Effect |
|---|---|
| `NAAB_SETTINGS_LOG` | append every setting path the loaded config contains |
| `NAAB_DROP_SETTING` | comma-separated paths to remove before parsing (`*` matches any key at that level) |
| `NAAB_DROP_LOG` | append each drop actually performed, with the value dropped |

In a normal build none of this code exists. An environment variable that can
delete a governance setting would be a bypass, so
`tests/security/test_setting_drop_compiled_out.sh` checks the shipped build:
the variable's name is not in the binary, and a requested drop of
`capabilities.shell.enabled` leaves shell blocked. Pointed at a test build,
both arms fail, which is the proof the check can fail.

`tools/testrunner/setting_drop.py` drives it:

1. **map**: runs every shell suite twice on the test build, logging what each
   one's configs really contain. This produces a measured map of suite to
   settings, and a baseline verdict (exit status and skip-marker count) for
   each suite. A suite whose two baselines disagree is excluded as flaky.
2. **probe**: for each suite, drops groups of up to 32 of the settings it
   loads, and bisects any group whose drop changes the verdict, down to
   single settings.
3. **report**: classifies every template setting.

```bash
cmake -S . -B build-mut -DCMAKE_BUILD_TYPE=Release -DNAAB_CONFIG_MUTATION=ON
cmake --build build-mut --target naab-lang -j4
python3 tools/testrunner/setting_drop.py map   --mut-binary build-mut/naab-lang --jobs 4
python3 tools/testrunner/setting_drop.py probe --mut-binary build-mut/naab-lang --jobs 4
python3 tools/testrunner/setting_drop.py report
```

`map` took 15 min and `probe` 39 min (940 runs) on 4 cores.

## Results

| Status | Settings | Meaning |
|---|---:|---|
| **live** | **189** | dropping it changed some test's verdict |
| survives | 747 | tests load it, none noticed it dropped |
| untested | 538 | no test loads it at all |
| documentation | 19 | `rationale` / `description`: read into reports, not enforcement |

Of 1,474 enforcement settings, **189 (13%) have a test that would notice if
they broke**.

**survives is a list to triage, not a verdict that a setting is dead.** A
surviving setting is one of three things:
- dead: it does nothing;
- set to its default: the drop changes nothing by design (a sweep over
  explicit values cannot see a default);
- untested: it works, but no test checks its effect.

The JSON records which suites load each setting, as the starting point for
telling these apart.

| Section | Settings | Untested | Live | Survives |
|---|---:|---:|---:|---:|
| scanner | 310 | 116 | 0 | 194 |
| code_quality | 219 | 75 | 19 | 125 |
| polyglot_optimization | 169 | 120 | 0 | 49 |
| context_drift | 98 | 24 | 24 | 50 |
| restrictions | 79 | 23 | 11 | 45 |
| languages | 74 | 48 | 5 | 21 |
| agents | 47 | 11 | 23 | 13 |
| capabilities | 43 | 9 | 20 | 14 |
| contracts | 30 | 20 | 2 | 8 |
| requirements | 30 | 7 | 0 | 23 |
| output | 29 | 3 | 0 | 26 |
| audit | 28 | 2 | 6 | 20 |
| circuit_breaker | 25 | 3 | 14 | 8 |
| limits | 24 | 0 | 9 | 15 |
| agent_review / scorers / orchestra / environments / governance_baseline | 31 | 31 | 0 | 0 |

## Controls

- **Positive control.** The settings known to be load-bearing all come out
  live, each noticed by a named suite:
  - `capabilities.shell.enabled` by `test_sandbox_engine_parity.sh`;
  - `mode` by `test_r13_fixes.sh`;
  - `capabilities.network.enabled` by `test_governance_validity.sh`;
  - `circuit_breaker.enabled` by `test_cb_masked_child.sh`;
  - `context_drift.enabled` by `test_bsd_cdd_gate.sh`;
  - `agents.*.shell_allowed` by `test_shell_content_split.sh`;
  - `capabilities.filesystem.blocked_paths` by `test_governance_authority.sh`.
- **No unmeasurable drops.** Every drop the probe requested was performed and
  logged by the engine itself.
- **No flaky suites.** No suite's two baselines disagreed. A first run showed
  one; that was the harness's own fault, below.

## Findings

### 1. `inert_keys_baseline.txt` lists live settings

`meta.inheritance.merge_arrays` is in the inert baseline, yet the probe found
it live, and the result reproduces.
- **Without the drop**, `tests/governance_v4/test_extends.sh` passes 44/44,
  twice.
- **With it dropped**, the suite fails 5, identically both times, including
  T18a ("SECRET_A blocked (from parent)") and T18d ("SECRET_A value not
  leaked").
- **What breaks:** array inheritance, so a parent config's blocked
  environment variable is lost.

**Root cause.** `test_inert_key_sweep.sh` searches for readers in every
source file *except* `src/runtime/governance_config.cpp`, to avoid counting
the parser as a reader. But that file also holds real consumers: the
`extends` merge logic, ratchet decisions and the telemetry forwarder's setup.
This contradicts the sweep's own claim that it "can only UNDER-report".

Probably live by the same mechanism (a consumer inside that file that is not
a parse line, clamp, ratchet compare or merge copy):
- `meta.inheritance.merge_strategy`;
- `agent_dispatch.default_timeout_seconds`;
- `meta.allow_agent_addition_mid_run`;
- six `telemetry_output.forward_*` / `webhook_auth_header` keys.

**Not changed here.** Removing any of these from the template as "inert" would
remove working features.

### 2. The template is missing settings that work

315 setting paths that suites load are not in `govern-template.json`.
- **260 are real settings the loader reads**, for example:
  - `agents.*.api_base`;
  - all of `circuit_breaker.output_admissibility.*`;
  - `circuit_breaker.deescalate_sustained`;
  - the newer `context_drift.signals.*`;
  - `telemetry.decision_snapshots`.
- The rest are deliberate fixtures (misspellings that test the unknown-key
  warning) or entries in maps where the user chooses the names.

### 3. Tests that set keys the engine ignores

| Key a test sets | Why it does nothing | Suites |
|---|---|---|
| `taint_sources`, `sinks` (top level), `mode: "HARD"` | Wrong location (`taint_tracking.sources` / `sinks`) and an invalid mode; the whole config is ignored | `test_container_taint_vm003.sh`, `test_taint_polyglot_vm001.sh` |
| `capabilities.process.allow_spawn` | Not a key (the real one is `spawn`, itself inert) | `test_drift_detection.sh`, `test_bsd_cdd_fixes.sh` |
| `capabilities.env_access` | Not a key | `run_differential.sh` |
| `limits.code.max_functions` | Deliberately removed (it duplicated a scanner check) | `test_multiagent_governance.sh`, `run_differential.sh` |
| `meta.require_signature` | Deliberately removed (V-SC-008) | `test_hivemind_governed.sh` |
| `restrictions.max_string_length` | Wrong place (only `languages.python`) | `test_hivemind_governed.sh` |
| `scanner.code_quality.complex_boolean_expr.max_operators` | Not read | `test_multiagent_governance.sh` |

The two taint tests fail against the dead interpreter, but that does not mean
they test taint. Taint tracking is off by default, and enforce mode upgrades
the sandbox to `standard`, which refuses `env.get` itself. Their broad greps
("denied", "governance") matched that sandbox refusal, so taint tracking was
never reached: an outer gate masking the inner one.

## Limits

- **Single `.naab` test files are not covered.** The naab phase's own
  `govern.json` files are not covered, so "untested" may be slightly high.
- **Masking.** Group bisection can in principle miss a setting whose effect
  another setting in the same group masks. That shows as "survives", which is
  why survivors are triage material rather than a verdict.
- **Per-agent and per-function settings are grouped by field name.**
  `agents.reviewer.model` and `agents.planner.model` are both counted as
  `agents.*.model`.
- **Two harness defects were found and fixed during the measurement:**
  - the build check's own `govern.json` sat in a parent directory of every
    suite, and 35 suites discovered it;
  - SKIP markers in coloured output went uncounted (#276).

  The results above are from the corrected runs.
