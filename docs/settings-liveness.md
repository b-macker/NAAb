# Setting liveness: does any test notice when a govern.json setting stops working?

The dead-interpreter gate asks whether each *test* can fail. This asks the same
of each *setting* in `govern-template.json`: drop it, so the engine falls back
to its default, and see whether any test that uses it notices.

First measured 2026-10-03 ([`settings-liveness/results-2026-10-03.json`](settings-liveness/results-2026-10-03.json));
re-measured 2026-11-13 ([`settings-liveness/results-2026-11-13.json`](settings-liveness/results-2026-11-13.json)),
and again the same day with the tool fixed ([`settings-liveness/results-2026-11-13-after-tool-fix.json`](settings-liveness/results-2026-11-13-after-tool-fix.json)).
The run with the fixed tool is the next section, the first re-measurement the
one after it; everything after those describes the first run.

## Re-measured with the tool fixed (2026-11-13, #378)

The first re-measurement named three ways the tool read "no test notices"
when it could not tell. This run closes two and labels the third. Master
`084ea160` plus the tool changes; no engine check, test assertion or
template file differs. Every count is **observed** on this run.

What changed in the tool:

- **One removal hook, both readers.** `include/naab/testing/config_mutation.h`
  is called by `loadFromJson()` and by the scanner's own
  `ScannerEngine::loadConfigFromPath()`, so a removal now reaches the
  `scanner` section. `tests/security/test_setting_drop_compiled_out.sh`
  checks the shipped `naab-lang` and `naab-gov` carry neither.
- **Helpers come from the test build.** `naab-gov` is built in `build-mut`
  with the hook. `libnaab.a`, `naab-lsp` and `naab-verify-audit` are still
  the ordinary build; the tool prints that on every run.
- **broad-only is its own status.** A suite that loads 100 or more template
  settings (a whole template or example config; `--broad N`) cannot say a
  setting is tested. A survivor only such suites load is `broad-only`, the
  count October called "untested".
- `probe --settings a.b,c.d` (or `@FILE`) re-probes named settings alone,
  which removes group masking for them.
- `probe --resume` continues a stopped sweep from a checkpoint written after
  every group. This run needed it: with `naab-gov` carrying the hook,
  `test_langconform.sh` went from 0 removable settings to 1,238, and
  background work here is stopped after two hours. The sweep ran in three
  slices.

| | first run today | with the tool fixed |
|---|---:|---:|
| Units mapped / probed | 411 / 328 | 413 / 336 |
| Template settings (enforcement) | 1,534 | 1,535 |
| **live** | 310 | **318** |
| survives (a narrow suite loads it, none noticed) | 1,223 together | **268** |
| broad-only (only whole-config suites load it) | | **948** |
| unmeasurable | 1 | 1 |
| Probe runs | 1,409 | 1,558 |

- **Every setting live before is live now** (310 of 310). Eight more are:
  `codegen.max_code_lines` and `codegen.timeout_seconds` (noticed by
  `test_declared_order_ratchet.sh`, which checks them through `naab-gov`),
  `banned_functions` for cpp, javascript, php and ruby (`test_langconform.sh`,
  also through `naab-gov`), `restrictions.crypto.weak_ciphers`
  (`test_a148_widenings.sh`), and `output.explainer`, new in the template
  since (#377).
- **The scanner section is measured now, and no test notices any of it.**
  All 310 scanner settings were removed in some run: 257 are broad-only and
  53 survive (50 of them loaded by one narrow suite, 3 by two). Before, these
  rows were not evidence either way.
- **The 268 survivors are the list to work from**: a suite written for the
  area loads the setting and does not notice it gone. By section: scanner 53,
  context_drift 51, code_quality 48, restrictions 17, languages 10, agents 8,
  audit 7, circuit_breaker 7, exposure_tracking 7, taint_tracking 7,
  limits 6, governance_health 5, others 42.
- **Unmeasurable: 1**, `environments.*.quality_gate.enabled`, as before. Four
  more removals were asked for and not performed in SOME run (`version`,
  `scanner.version`, `scanner.mode`, `scanner.code_quality.empty_catch.enabled`)
  and performed in another, so they are counted as survivors.

Limits of this run:

- A survivor is still screened, not traced: group masking, a setting at its
  default, and a suite that loads a setting without asserting on it all read
  the same. `probe --settings` settles the first.
- Settings a suite checks only through `naab-lsp`, `naab-verify-audit` or
  `libnaab.a` still read as survivors (the false-"no test notices"
  direction). No known suite checks a setting only that way; that is traced
  for the three binaries' call sites, not measured.
- The 100-setting line for "broad" is authored. Eight suites are over it
  (listed in the results file). A narrow suite that loads 99 settings and
  asserts on two still makes the other 97 "survives", not "broad-only".
- The run was three slices, not one process. The checkpoint is refused for
  another map or group size (`test_setting_drop_tool.sh` ST-09, ST-10), and
  the 310 previously live settings all reappearing is the control that the
  joined result is whole.
- **Adversarial pass.** The claim judged most likely wrong was "no test
  notices any scanner setting", because the same claim was withdrawn once
  already. Looked: all 310 rows carry the value that was removed
  (`values_dropped`), so the removals were performed by the scanner's
  reader, and `test_setting_drop_compiled_out.sh` SD-05 is the arm in which a
  scanner removal on the test build changes a scan's result. One re-tracing; it changed nothing.
- **Direction of error**: unchanged. What the tool gets wrong, it gets wrong
  toward "no test notices".

## Re-measured 2026-11-13 (first run, before the tool fix)

The scanner and `naab-gov` limits this section describes are closed by the
run above.

Master `09e633e2`, the same three commands as below, 4 jobs. Every count is
**observed** on that run unless it says otherwise. Report only: no engine,
test or template file changed.

| | 2026-10-03 | 2026-11-13 |
|---|---:|---:|
| Units run | 260 | 411 |
| Template settings (enforcement) | 1,474 | 1,534 |
| **live** (a test notices it removed) | 189 (13%) | **310 (20%)** |
| not live | 1,285 | 1,224 |
| Probe runs / time | 940 / 39 min | 1,409 / 45 min |

Of the 189 settings live in October, 182 are still in the template and all
182 are still live. 79 settings moved to live (46 from survives, 33 from
untested), and 49 of the 92 settings added to the template since are live.

**The tool's "untested" count is now 0, and that is an artifact.**
`tests/governance_v4/test_nested_unknown_keys.sh` loads the whole template
(1,775 setting paths), so every setting is "loaded by a suite". The count
that means what "untested" meant in October is the first column below.

### The 1,223 survivors, sorted

| Group | Settings | What it is |
|---|---:|---|
| template-only | 453 | loaded only by the whole-template loader: no suite written for it loads it (October's "untested": 538) |
| inert by trace | 47 | classified `inert` in `tests/governance_v4/ratchet_classification.txt` |
| at the template's value | 414 | every value the probe removed equals the template's value |
| another value | 309 | some suite sets it to a value other than the template's, and none noticed it removed |
| unmeasurable | 1 | `environments.*.quality_gate.enabled`: the removal was requested and never performed |

How far each group can be trusted (all **screened**, none traced per key):

- **at the template's value is not "at its default".** The template writes
  `enabled: true` for checks whose built-in default is off, so removing such
  a key does change the rules; the group says only that no test noticed.
- **another value** says a config some suite loads sets the key to something
  other than the template's value. It does NOT say a suite set the key on
  purpose (corrected 2026-11-13, see "Follow-up checks" below): most of the
  keys checked are loaded only by suites that load a large example config
  and assert on none of them. It can also be a key another key in the same
  removed group masks.
- **inert by trace** covers only keys the ratchet table names; a key outside
  that table can be inert too.
- The ratchet table matched 359 of the survivors (kinds `ratchet`, `order`,
  `fixed`: settings with a security direction): 117 template-only, 122 at
  the template's value, 120 at another value. The 120 are listed in the
  results file (`survivor_group: "other-value"` with a `ratchet_kind`); they
  include `capabilities.network.allowed_ports` (known: A87, binds no program
  connection), the `context_drift.reality_checkpoint.*` keys, 12
  `context_drift.signals.*` switches, the five `exposure_tracking.*` limits
  and the four `hooks.*.timeout` keys.

| Section | Settings | Live | Template-only | Inert | Template value | Other value |
|---|---:|---:|---:|---:|---:|---:|
| scanner | 310 | 0 | 116 | 0 | 106 | 88 |
| code_quality | 228 | 26 | 81 | 1 | 76 | 44 |
| polyglot_optimization | 169 | 2 | 119 | 0 | 39 | 9 |
| context_drift | 120 | 35 | 34 | 0 | 10 | 41 |
| restrictions | 80 | 18 | 0 | 6 | 40 | 16 |
| languages | 74 | 6 | 0 | 3 | 53 | 12 |
| agents | 54 | 30 | 11 | 0 | 3 | 10 |
| capabilities | 43 | 21 | 9 | 0 | 10 | 3 |
| circuit_breaker | 39 | 28 | 3 | 0 | 2 | 6 |
| contracts | 32 | 8 | 17 | 0 | 0 | 7 |
| output | 29 | 2 | 1 | 13 | 11 | 2 |
| audit | 28 | 12 | 1 | 6 | 6 | 3 |
| requirements | 26 | 3 | 7 | 0 | 8 | 8 |
| limits | 22 | 10 | 0 | 0 | 3 | 9 |
| polyglot | 22 | 1 | 0 | 11 | 5 | 5 |
| governance | 21 | 6 | 11 | 0 | 1 | 3 |
| 38 smaller sections | 237 | 102 | 43 | 7 | 41 | 43 |

### Controls and limits of this run

- **Positive control: PASS.** The eight settings the first run named as
  load-bearing are all live again, each noticed by a named suite
  (`capabilities.shell.enabled`, `mode`, `capabilities.network.enabled`,
  `circuit_breaker.enabled`, `context_drift.enabled`,
  `agents.*.shell_allowed`, `capabilities.filesystem.blocked_paths`,
  `meta.inheritance.merge_arrays`).
- **No flaky suites**: no unit's two baselines disagreed.
- **scanner: 310 settings UNMEASURABLE by this tool, in both runs** (traced
  2026-11-13). `ScannerEngine::loadConfigFromPath()` (`src/scanner/scanner.cpp`)
  opens govern.json and reads its `scanner` section itself; it never passes
  through `loadFromJson()`, where the removal hook lives. A requested removal
  is performed and logged by the governance loader in the same process, so
  the tool counts it as done, while the scanner still reads the key. The
  scanner's rows in the tables above (and in October's) are not evidence
  either way; without them, 310 of 1,224 measurable settings are live (25%)
  and 913 survive.
- **Only `naab-lang` carries the hook.** A setting a suite checks through
  `naab-gov` (lint, check, diff, ratchet, taint) is loaded by a binary that
  cannot remove it, so it reads as a survivor. This is the
  false-"no test notices" direction, and it has grown since October with the
  `naab-gov` suites.
- The first run's limits (below) still apply: single `.naab` tests are not
  covered, and group removal can mask.
- The probe was run twice: a container restart stopped the first at suite
  323 of 328, with 308 live at that point.
- **Adversarial pass.** The claim judged most likely wrong was "the scanner
  section has no live setting". Looking for the scanner's reader showed the
  instrument does not reach it (above), so the claim was withdrawn rather
  than confirmed. One re-tracing; it changed the reading of 310 rows. Not
  re-traced: the per-key sorting, which is screened.
- **Direction of error.** Everything this tool can get wrong, it gets wrong
  toward "no test notices" (the scanner, `naab-gov`, masking). A setting it
  calls live was noticed by a named suite on a real run.

### Follow-up checks (2026-11-13, after the first write-up)

Made to answer what to do with the survivors. Each is **observed** unless
it says traced or undetermined; master `09e633e2`.

**The first write-up was wrong about the "another value" group.** It called
the group "the strongest lead that a test is missing: a suite bothered to
set the key". Of the 120 with a security direction, the 47 under
`capabilities`, `exposure_tracking`, `hooks`, `agents`, `codegen`, `trust`,
`governance`, `restrictions`, `languages`, `audit` and `agent_dispatch`
were listed with the suites that load them. Most are loaded only by suites
that load a whole example config and assert on none of these keys:
`test_agent_harness_example.sh`, `test_init_governance.sh`,
`test_readme_claims.sh`, `test_govern_json_fuzz.sh` and
`test_protection_map.sh`. Those keys are untested, not "tested and
unnoticed". The other 73 of the 120 were not listed.

The three groups the first write-up named:

- **`hooks.*.timeout`: no test gap.** `test_hooks.sh` sets 1 or 3 seconds;
  the default is 5 (`HookConfig::timeout`, `governance.h`, traced). With the
  key removed a hook that sleeps 60 seconds is still stopped, so the
  verdict cannot change and nothing loosens.
- **`capabilities.network.allowed_ports`: no test gap.** It binds no
  program connection (A87).
- **`exposure_tracking.*`: a real lead.** Every limit defaults to 0, which
  is unlimited (`ExposureTrackingConfig`, traced), so removal loosens.
  `tests/governance_v4/depth/test_depth_runtime.sh` loads them and did not
  notice. **Undetermined** why; the observation that would settle it is
  that suite run with one of these keys removed alone, which rules out
  masking by the rest of its group.

**Inert keys in the template: 54, not 47.** The 47 above is what the first
matcher found among the survivors. Matching the ratchet table's `inert`
lines against every template setting, with the lines that cover a whole
block, gives 54: 53 survive and 1 reads live
(`requirements.naming_conventions.enabled`, noticed by
`test_nested_unknown_keys.sh`; why is undetermined, the suite's text does
not name the key). Of the 54 lines, 18 give a trace as their reason and 36
a grep ("screened"). Finding 1 below is a grep-built inert list that held
live settings, so the 36 are leads until each is traced to its point of
effect and tried set against unset.

**The secrets macro pattern on ordinary C and C++** (A148(a), #374). The
pattern was run, as a Python regular expression with the same text and
case-insensitivity, over the C and C++ sources on this machine that hold
no credentials: `/usr/include` (3,996 files), the repository's `src/`,
`include/` and `external/` (2,203 files), 4,155 string-valued `#define`
lines in all.

| | Count |
|---|---:|
| Lines the pattern flags | 5 |
| Files they are in | 1 (`openssl/obj_mac.h`) |
| Of them, a credential | 0 |
| All-capitals names ending in a credential word (`ERR_BAD_TOKEN`) | 0 |

The five are object names (`LN_userPassword "userPassword"` and four like
it), matched because the pattern table is case-insensitive. A block is
checked by its own text, never by a header it includes, so they matter
only pasted into a block. What this does not measure: application code,
where a macro such as `ERR_BAD_TOKEN "bad_token"` would be flagged
(verified in #374) and was simply not present in these sources.
`code_quality.no_secrets.allowlist` is one of the inert keys, so the remedy
for a false alarm is renaming the macro.

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
`capabilities.filesystem.blocked_paths` leaves the path blocked. Pointed at a
test build, both arms fail, which is the proof the check can fail. (The
subject is a `file.read`, decided in the standard library; a `<<sh>>` block
cannot run on build-windows at all.)

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
cmake --build build-mut --target naab-lang naab-gov -j4
python3 tools/testrunner/setting_drop.py map   --mut-binary build-mut/naab-lang --jobs 4
python3 tools/testrunner/setting_drop.py probe --mut-binary build-mut/naab-lang --jobs 4
python3 tools/testrunner/setting_drop.py probe --resume --mut-binary build-mut/naab-lang --jobs 4   # after a stopped sweep
python3 tools/testrunner/setting_drop.py probe --settings a.b,c.d --mut-binary build-mut/naab-lang  # named settings alone
python3 tools/testrunner/setting_drop.py report
```

Since #378 the hook is `include/naab/testing/config_mutation.h`, called by
`loadFromJson()` and by the scanner's own reader, and `naab-gov` is built
with it (see the top section).

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

### 3. Tests that set keys the engine ignores (fixed)

The first version of this table named the suite that LOADED each key. Several
keys were not in those suites at all: they sat in a config file a suite's
programs discover by walking up from their own directory. The table below
names the file, and every row has since been fixed.

| Key | Why it does nothing | Where it was | Fix |
|---|---|---|---|
| `taint_sources`, `sinks` (top level), `mode: "HARD"` | Wrong location (`taint_tracking.sources` / `sinks`) and an invalid mode | `test_container_taint_vm003.sh`, `test_taint_polyglot_vm001.sh` | Rewritten to `taint_tracking.*`; exact arms now prove taint decides |
| `capabilities.process.allow_spawn` (and the rest of that block) | Nothing under `capabilities.process` is read (A16; `allowed_commands` and `spawn` included) | `test_drift_detection.sh` T51, `test_bsd_cdd_fixes.sh` | Removed |
| `capabilities.env_access` | Not a key | `tests/robustness/govern.json` (loaded via `run_differential.sh`) | Removed |
| `limits.code.max_functions` | Deliberately removed (it duplicated a scanner check) | `tests/robustness/govern.json`; `tools/agent-governance/govern.json` (loaded via `test_multiagent_governance.sh`) | Removed |
| `meta.require_signature` | Deliberately removed (V-SC-008) | `examples/hivemind_governed/src/govern.json` | Removed |
| `restrictions.max_string_length` | Not read anywhere: `max_string_length` is a struct field nothing parses or reads (`governance.h`). The earlier "only `languages.python`" was wrong | same | Removed |
| `scanner.code_quality.complex_boolean_expr.max_operators` | Not read; the scanner reads `max_conditions` | `tools/agent-governance/govern.json` | Removed |

Every fix is a deletion, never a rename to the real key. That was deliberate:
a dead key changes nothing, so deleting it changes nothing (the five affected
suites produced identical output before and after), while a rename can
LOOSEN. `max_operators: 4` renamed to `max_conditions: 4` would raise the
scanner's limit above the default of 3 that is in force today. Where an intent
to restrict had no real key to land on (`env_access`, `max_string_length`),
inventing enforcement in a test fixture is not the fix either.

Checked and NOT a defect: the shorthand forms some older configs use. The
loader accepts top-level `sandbox_level`, `capabilities.network` as a
boolean, `capabilities.filesystem` as a string and `capabilities.shell` as a
boolean (`governance_config.cpp`, the legacy block before the object forms),
and each feeds a field the engine enforces (`network_allowed`,
`filesystem_mode`, `shell_allowed`, `sandbox_level_config`).
`capabilities.env_vars.read` is a real HARD master switch. Measured, not just
traced: with `test_bsd_cdd_fixes.sh`'s shorthand config, `"network": false`
turns an `http.get` into a governance HARD block (exit 3, citing
`capabilities.network`); without it the call is refused at a different layer
(exit 1). An earlier draft of this section listed these as wrong-type values
that do nothing -- that was an assumption about the type, made without
reading which forms the loader accepts.

The two taint tests had failed against the dead interpreter, but that did not
mean they tested taint. Taint tracking is off by default, and enforce mode
upgrades the sandbox to `standard`, which refuses `env.get` itself. Their
broad greps ("denied", "governance") matched that sandbox refusal, so taint
tracking was never reached: an outer gate masking the inner one.

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
