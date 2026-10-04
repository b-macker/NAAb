# Findings: the 75 inert config keys — wire, remove, or keep

**Status:** report only. No template or code is changed by this document.
**Snapshot:** `fecb13e` (findings), re-checked unchanged at `master` `c6a648fc`.
The template, loader, engine, checks, header and baseline are byte-identical
between the two.
**Date:** 2026-10-03.
**Subject:** every entry in `tests/governance_v4/inert_keys_baseline.txt`
(75 keys; `test_inert_key_sweep.sh` 2/2 at the snapshot, so the baseline is
current).

## How to read this

Every claim carries a tier:

| tag | tier | meaning |
|---|---|---|
| **[V]** | verified | made to happen, with a control in the same run that had to behave |
| **[T]** | traced | followed through the source to the point of effect, no run |
| **[S]** | screened | a grep or format check; false positives expected |

Every run below is **configured**: my fixtures, on a Release build of `fecb13e`,
in one cloud container. Nothing here is observed in the wild. The reproduction
script at the end re-runs every [V] row.

History was read on a **full** clone (1247 commits, 2026-01-07 onward).
Earlier write-ups (`docs/open-investigations.md` A2) worked from a shallow
112-commit clone and said whether keys were ever wired "cannot be answered from
this repository". It can now.

Each recommendation is one of: **WIRE** (point at the intended check and its
call site), **REMOVE** from the template (keep parsing for compatibility, add a
load warning), or **KEEP** (stated reason).

## Summary

1. **10 of the 75 are live.** The sweep excludes the loader from its consumer
   search, and these ten are consumed *inside* the loader. Six are the
   telemetry forwarding keys the baseline header already names. The other four
   are `agent_dispatch.default_timeout_seconds` [V], which the header does not
   mention, plus `meta.allow_agent_addition_mid_run`,
   `meta.inheritance.merge_arrays` and `merge_strategy` [T]. KEEP.
2. **No key was ever wired and later lost** [T, full history]. 51 of the 75
   arrived together in `bcf647a0` (2026-02-24). `92a8fc10` then shipped the
   template as "898 lines with all configurable options": the schema was wider
   than the implementation from day one. The single historical read outside
   the loader was a local variable assigned and never used, deleted the same
   day for an unused-variable warning (`babc6a88`).
3. **None of the 75 warns at load** [V]. Loading the full root template prints
   15 warnings and none names a baseline key. The warnings that exist cover
   keys *outside* the baseline. So "keep the warning" has nothing to keep:
   every REMOVE below needs a new warning.
4. **Three existing load warnings are wrong.**
   - `"limits.data.dict_size" is parsed but not enforced`: false since #160
     wired that key [V].
   - The warnings for `requirements.error_handling` and
     `requirements.naming_conventions` describe a check that is ON. Neither
     check exists [T].
5. **Recommendation: 6 WIRE, 10 KEEP, 59 REMOVE.** All six WIRE candidates
   either change nothing at their shipped values or are opt-in, and all of them
   tighten.
6. **The most dangerous removals:** `api.tls_cert` / `api.tls_key` (the REST
   server has no TLS, so API keys travel in plaintext while the operator
   believes HTTPS is on) and the `code_injection` LDAP/XPath/template trio
   (shipped `true`, matching nothing [V]).

## Decision table

Template membership at the snapshot:
- root `govern-template.json`: 74 of 75 (missing only `meta.allow_agent_addition_mid_run`);
- `docs/govern-template.json`: 72 of 75;
- `docs/book/verification/govern.json`: 17 of 75.

"Introduced" is the first commit adding the key's leaf to `src/`/`include/`;
"ever worked" comes from `git log -S` on the leaf, with every non-loader diff
line read.

### KEEP — live; the sweep cannot see their consumer

| Keys (JSON path) | Introduced | Ever worked | Evidence |
|---|---|---|---|
| `telemetry.forward_batch_size`, `forward_buffer_max`, `forward_retry_count`, `forward_timeout_ms`, `forward_shutdown_drain_ms`, `webhook_auth_header` | `f229dd9b`, `11964c4f` | yes, still | The loader hands them to `TelemetryForwarder` as `fwd_cfg.*` [T]. `test_telemetry_forward.sh` passes 4/4 (pass only, not mutation-checked). |
| `agent_dispatch.default_timeout_seconds` | `e46868c9` | yes, still | Copied into each agent's `timeout_seconds` (`governance_config.cpp:3032`), which libcurl uses (`agent_provider.cpp:301`, `:358`, `:614`) [T]. `agent.environment()` reports **60 / 17 / 33** for key absent / `17` / `17` plus explicit agent `timeout: 33` [V]. **Not mentioned in the baseline header.** |
| `meta.allow_agent_addition_mid_run` | `252e9d5b` (#124) | yes | Ratchet in the loader [T]. `test_agent_addition_ratchet.sh` passes 5/5. The only baseline key in no template. |
| `meta.inheritance.merge_arrays`, `merge_strategy` | `bcf647a0` (parse); read since `f229dd9b` | yes, since `f229dd9b` | `mergeRules()` (`governance_config.cpp:4886`, `:4936`) [T]. `test_extends.sh` passes 44/44, including `parent_wins`. |

### WIRE — details in the next section

| Keys | Introduced | Ever worked | Evidence |
|---|---|---|---|
| `trust.require_fresh_signature` | `e00f78b8` | never; no spec was ever written | Staleness runs only `if (signed_at > 0 …)` (`governance_engine.cpp:5309`), so timestamp-less signatures never go stale [T]. |
| `approval.default_expiry_hours` | `8ef3cbfc` | never | `--approve` hardcodes `int expiry_hours = 24` (`main.cpp:891`) [T]. |
| `audit.log_events.checks_failed` | `bcf647a0` | never | Siblings are wired (`governance_reports.cpp:141`–`181`); no audit entry exists for any check result [T]. |
| `agent_dispatch.max_retries_per_run` | `4fc8742e` | never (parse and merge only) | The counter exists (`agent_impl.cpp:138`); no budget compares against it [T]. |
| `polyglot_optimization.verification.max_verification_time_ms` | `627560f8` | never; siblings read | Re-execution at `governance_reports.cpp:2870` has no bound of its own [T]. |
| `code_quality.no_apologetic_language.scan_strings` | `bcf647a0` | never | With `scan_strings: true`, an apology in a string exits 0; the same text as a comment exits 3 [V]. |

### REMOVE — phantom checks the template advertises

| Keys | Introduced | Evidence | Notes |
|---|---|---|---|
| `requirements.error_handling.require_try_catch`, `require_catch_body` | `bcf647a0` | **The whole check is missing, not just these two options.** Its only reader is `requiresErrorHandling()` (`governance_engine.cpp:160`), which has had zero callers from `bcf647a0` to HEAD. `git log -S` shows only its introduction and a move (`c6c778bf`) [T]. A HARD config against code with no try/catch exits 0. | That 0 has **no positive control**: a dead interpreter prints the same 0 (see reproduction). The trace is the evidence. Remove the whole `requirements.error_handling` block. |
| `requirements.naming_conventions.check_naab_code`, `check_polyglot_code` | `bcf647a0` | No naming checker has ever existed. Outside the loader, only the struct field is referenced, at every commit [T]. HARD `snake_case` against `BadFunctionName`, `BadVariableName` and `BadPythonName` exits 0. | No control, as above. The scanner's style checks (`src/scanner/checks_style.cpp`) are the live equivalent for NAAb code. |
| `restrictions.code_injection.block_ldap_injection`, `block_template_injection`, `block_xpath_injection` | `bcf647a0` | No pattern exists anywhere [T]. All three `true` at HARD: a block with LDAP, XPath and Jinja injection shapes exits **0**. The identical program with `block_dynamic_code_gen: true` exits **3** on its `eval(` [V]. | Do **not** wire as-is: all three default to `true` in the struct (`governance.h:458`–`468`). Wiring would apply HARD patterns to all **30** in-tree configs carrying a `code_injection` block (including the 3 template copies; 19 under `tests/gorilla`), not just the 6 that spell the keys. To wire later, first flip those struct defaults to `false`; that changes no behaviour today, because nothing reads them. |
| `code_quality.no_hardcoded_results.check_dict_success_fields` | `bcf647a0` | No dict-success pattern is in `HARDCODED_RESULT_PATTERNS_DB` (`governance_engine.cpp:793`) [T]. | |

### REMOVE — switches over behaviour that is always on (wiring could only weaken)

| Keys | Evidence |
|---|---|
| `trust.check_revocation`, `trust.check_key_expiry` | `TrustStore` skips revoked and expired keys unconditionally and never reads governance (`trust_store.cpp:101`) [T]. Already decided not to wire: `docs/GOVERNANCE-LIMITS.md` §7.1, "left alone". The template's `true` is accurate, but it invites setting `false`, which disables nothing. Warn when `false`. |
| `meta.schema_validation.warn_unknown_keys`, `suggest_corrections` | With both `false`, the loader still prints `Unknown key "limit" — did you mean "limits"?` (`governance_checks.cpp:6856`). A run with both keys absent prints the same [V]. Wiring would only let operators silence a typo diagnostic on a security config. |
| `code_quality.no_hardcoded_results.check_return_true_false` | The `return True/False #…` patterns always run (`governance_engine.cpp:796`) [T]. |
| `code_quality.no_apologetic_language.scan_comments_only` | The dispatcher hands the check source with string literals removed and comments kept (`governance_checks.cpp:6651`), so the behaviour is already "not strings" [V, via the `scan_strings` arms]. |
| `code_quality.no_mock_data.ignore_in_test_context` | The check never consults any test context, and none is defined (`governance_checks.cpp:1101`) [T]. The shipped `true` describes a relaxation that never happens. Wiring it would loosen. |

### REMOVE — `limits.*` (already recorded in A2 and `GOVERNANCE-LIMITS.md` §7)

| Keys | Evidence | Notes |
|---|---|---|
| `limits.code.max_total_polyglot_lines`, `limits.execution.total_executions`, `limits.memory.total_mb`, `limits.memory.per_block_mb`, `limits.timeout.total_polyglot` | One program, all five at `1`: 18 polyglot lines, 2 blocks, about 2.6 s of polyglot time, about a 16 MB allocation. It exits **0**. The same program with only the live sibling `limits.execution.polyglot_blocks: 1` added exits **3** [V]. `memory_limit_mb` (the `total_mb` mirror) feeds `getMemoryLimitMB()`, which has never had a caller in the whole history [T]. | `total_polyglot` is "deliberately kept" (§7.3), but that decision is about keeping the **key**, not advertising it. Wiring it needs a cumulative polyglot timer in both engines and in codegen, plus a level, and 20 in-tree configs set values as low as 20 s. A2 recommended deleting `total_executions` (redundant with `polyglot_blocks`); #157 documented it instead, so that is still open. |
| `limits.rate.cooldown_on_limit_ms` | `RateLimiter` has nowhere to hold a cooldown, and breaches throw rather than wait (A2) [T]. | A2 recommended deleting it. Still open. |
| `limits.data.input_size` | The real cap is the 100 MB `MAX_INPUT_STRING`, checked at `lexer.cpp:70`, not this key; the template says 1,000,000 [T]. | |

### REMOVE — features that were never built

| Keys | Introduced | Evidence |
|---|---|---|
| `api.tls_cert`, `api.tls_key` (struct `tls_*_path`) | `f229dd9b` | The REST server is a plain `httplib::Server` (`rest_api.cpp:32`), with no SSL server anywhere [T]. **Dangerous direction:** an operator who sets a certificate believes the API is HTTPS while `X-API-Key` travels in plaintext. A warning is the minimum. Refusing to start `naab-lang api` when either key is set would be honest. |
| `audit.log_events.checks_passed` | `bcf647a0` | As `checks_failed`, but it defaults to `true`, so wiring it would write every passed check to every audited config's log. Passes already reach telemetry as `GovernanceCheck` events. Flip the struct default before any future wiring. |
| `audit.provenance.record_decisions`, `record_proof_objects` | `bcf647a0` | Siblings `record_attestations` and `sign_records` are wired (`governance_reports.cpp:481`). "Proof object" exists nowhere in `src/` [T]. |
| `code_quality.max_complexity.max_parameters` | `bcf647a0` | Siblings `max_lines_per_block` and `max_nesting_depth` are read; this one is not [T]. Wiring needs per-language parameter parsing of source text. The scanner's `long_parameter_list` covers NAAb code. |
| `meta.environment.allow_cli_override`, `env_prefix` | `bcf647a0` | CLI precedence is hardcoded tighten-only (#241, #244) [T]. Wiring `allow_cli_override: true` would contradict #244, and an environment-variable override channel would bypass the config signature. |
| `meta.feature_flags.experimental_checks`, `verbose_parsing` | `bcf647a0` | Nothing reads them [T]. |
| `output.errors.show_help`, `show_examples`, `show_code_context`, `max_errors_per_rule`, `max_total_errors`; `output.formatting.unicode_symbols`; `output.summary.show_passing` | `bcf647a0` | Presentation only. Siblings `max_advisories`, `advisory_summary`, `voice` and `file_output` are wired [T]. `docs/book/chapter21.md` shows two of them as working. |
| `polyglot.output.max_output_lines`, `require_json_pipe`, `require_naab_return`, `validate_encoding` | `bcf647a0` | The live knobs are `restrictions.polyglot_output.format: "json"` and `limits.data.output_size` [T]. `validate_encoding: true` promises a check that does not exist. |
| `restrictions.polyglot_output.validate_json`, `require_structured` | `bcf647a0` | The live knob is `format: "json"` → `checkPolyglotOutput()` at HARD (`governance_engine.cpp:2958`) [T]. |
| `polyglot.parallel.max_parallel_blocks`, `timeout_per_block` | `bcf647a0` | Concurrency is a hardcoded `ThreadPool(2)` (`polyglot_async_executor.cpp:50`); the template says 4. Wiring 4 would *raise* concurrency [T]. |
| `polyglot.persistent_runtime.max_sessions`, `session_timeout`, `max_memory_per_session_mb` | `bcf647a0` | Persistent runtimes exist in both engines (`vm.cpp:3302`, `interpreter.cpp:1381`) and nothing bounds them [T]. `max_sessions` is the strongest future WIRE candidate in the whole REMOVE set, but it needs an enforcement-level decision first; the other two need time and memory accounting that does not exist. |
| `polyglot.variable_binding.max_bound_variables` | `bcf647a0` | Sibling `require_explicit` is live (`governance_checks.cpp:6534`). The template's `0` promises nothing. Low priority. |
| `polyglot_optimization.language_diversity.min_languages`, `max_single_language_percent` | `92a8fc10` | Nothing reads them [T]. `docs/polyglot/optimization_guide.md:395` says "Require 3+ languages", so that guide needs fixing too. |
| `polyglot_optimization.helper_errors.show_alternative_language`, `fuzzy_match_threshold` | `92a8fc10` | **The one historical read:** `bool show_alternative = …` was assigned and never used, then deleted in `babc6a88` ("fixes unused variable warnings") the same day. It never worked [T]. |
| `polyglot_optimization.ai_guidance.include_in_errors`, `suggest_refactoring`, `show_benchmarks` | `92a8fc10` | Nothing reads them [T]. |
| `polyglot_optimization.calibration.auto_calibrate` | `7db975e4` | Sibling `calibration.enabled` is live (`governance_reports.cpp:1939`). Wiring would launch benchmark subprocesses unasked [T]. |

## The six WIRE recommendations

Each needs, in the same change:
- a mid-run reload ratchet entry (tighten-only), unless noted;
- a test that fails when the wiring is removed;
- a success-expecting control arm.

These rules come from `docs/plan-engine-observability.md`.

1. **`trust.require_fresh_signature`.**
   - **Proposed spec:** when `true`, a governance signature that carries no
     signing timestamp fails verification at `stale_signature_level`.
   - **Why it matters:** the staleness check only runs
     `if (signed_at > 0 && …)` (`governance_engine.cpp:5309`). Two kinds of
     signature carry no timestamp: HMAC signatures, still written whenever no
     Ed25519 key is configured (`:5145`, "legacy … deprecated"), and Ed25519
     signatures made before `e00f78b8` (2026-05-23). For both,
     `max_signature_age_days` never applies.
   - **What it does not mean:** the timestamp *is* covered by the Ed25519
     signature (`content + ":" + timestamp`, `:5125`), so stripping a timestamp
     breaks verification rather than evading staleness.
   - **Call sites:** the staleness block at `:5306`–`:5335`, and the HMAC path
     (`:5366` onward).
   - **Effect on existing configs:** 20 of 21 in-tree configs that set it say
     `false`. The one `true` (`examples/agent_harness`) has a timestamped
     Ed25519 `.sig` [S, format only].

2. **`approval.default_expiry_hours`.**
   - **Spec:** `--approve` takes its default lifetime from the discovered
     `govern.json`. In line with #244 ("a flag may tighten"), `--expiry` may
     shorten that default but not extend it.
   - **Call site:** `main.cpp:891`. The `--approve` path needs config
     discovery.
   - **Why:** today `--expiry` is a CLI flag with no working govern.json
     equivalent, against the CLAUDE.md rule "govern.json is primary".
   - **Effect on existing configs:** none. The template's 24 equals the
     hardcoded 24, and no other config sets the key.

3. **`audit.log_events.checks_failed`.**
   - **Spec:** each failed check appends a `check_failed` audit entry when this
     key is `true` and `audit.level` is not `"none"`.
   - **Call site:** inside `enforce()`, beside the existing `approval_used` and
     `override` audit calls (`governance_engine.cpp:1317`, `:1344`). Follow the
     shape of the wired siblings (`governance_reports.cpp:141`–`181`).
   - **Effect on existing configs:** `audit.level` defaults to `"none"`
     (`governance_reports.cpp:79`), so only audited configs change, and no exit
     code changes. 8 test scripts reference the audit file [S] and need
     re-running.

4. **`agent_dispatch.max_retries_per_run`.**
   - **Spec:** once the run has made N retries across all agent calls, a
     failing call returns its error instead of retrying; `0` means unlimited.
   - **Call sites:** the retry checks (`agent_impl.cpp:2921`, `:3130`). The
     counter already exists (`:138`). Follow the hard-stop pattern of
     `chargeCallBudget()` (`:318`).
   - **Effect on existing configs:** the template says `0`. Only
     `examples/agent_harness` sets the key (20).

5. **`polyglot_optimization.verification.max_verification_time_ms`.**
   - **Spec:** each consensus re-execution runs under
     `ScopedTimeout(ms, Scope::Local)`. A language that times out counts as not
     participating, never as drift.
   - **Call site:** `verifyPolyglotResult()` (`governance_reports.cpp:2803`),
     wrapping the call at `:2870`.
   - **Effect on existing configs:** verification is opt-in
     (`verification.enabled` defaults to `false`).

6. **`code_quality.no_apologetic_language.scan_strings`.**
   - **Spec:** when `true`, the check scans unstripped source.
   - **Call site:** pick `code` vs `stripped` at `governance_checks.cpp:6651`.
   - **Effect on existing configs:** none. Every in-tree value is `false`
     (the root and `docs/` template copies, and three test configs).

## Corrections to the existing record

1. **`requirements.error_handling` is a missing check, not "a live check with
   two ignored options".** That wrong description appears in the baseline
   header (category C), in `docs/open-investigations.md` A2 and in
   `docs/GOVERNANCE-LIMITS.md` §7.3. All say "the check runs off a different
   field". That field's only reader has no callers (above).
2. **Wrong load warnings.**
   - `warnInertLimit("dict_size")` (`governance_config.cpp:1006`) has been
     false since #160 added enforcement at `governance_checks.cpp:6477`. In
     one run, a 3-entry dict exits 3 against `dict_size: 2` while the warning
     prints; the 2-entry control exits 0 [V].
   - `warnEnableNeedsLevel(eh, "requirements", "error_handling")`
     (`governance_config.cpp:1088`) and
     `warnIgnoredEnableFlag(nc, "requirements", "naming_conventions")`
     (`:1103`) describe checks that do not exist [T].
3. **A2's "whether enforcement was ever removed cannot be answered"** was a
   shallow-clone artifact. Answer: no baseline key was wired and later lost.
4. **The baseline header's list of false positives omits
   `agent_dispatch.default_timeout_seconds`**, which is live [V].
5. **Open from A2:** delete `cooldown_on_limit_ms` and `total_executions`.
   #157 documented them instead, and both still ship in the template.
6. **`GOVERNANCE-LIMITS.md` §7.1 cites `lexer.cpp:69`** for `MAX_INPUT_STRING`.
   The call is at `:70`.

This document does not edit any of those files. Bringing code, docs and tests
back into agreement is follow-up work.

## What the baseline cannot see (the template's inert surface is larger than 75)

The sweep lists only keys assigned as `rules_.X.Y = …` whose leaf name appears
nowhere outside the loader. Inert keys outside that shape:

- **Unread siblings:** `code_quality.max_complexity.max_local_variables`,
  `max_cyclomatic_complexity`, `max_cognitive_complexity` (leaf-name
  collisions); `requirements.naming_conventions.variables`, `functions`,
  `level` (the whole block is unread).
- **Never parsed:** four `no_hardcoded_results` flags that `naab-lang init`
  writes and the loader never parses: `check_return_none_null`,
  `check_return_empty_collections`, `check_dict_status_fields`,
  `check_perfect_scores`.
- **Already documented in `GOVERNANCE-LIMITS.md` §7:**
  `limits.execution.parallel_blocks`, `limits.timeout.per_block`,
  `limits.code.max_nesting_depth`.
- **Already warned:** `capabilities.process.*`,
  `capabilities.filesystem.allow_hidden_files`, `allow_absolute_paths`,
  `blocked_extensions`.
- **Listed in `examples/agent_harness/README.md`:** `allowed_extensions`,
  `max_file_size`, `max_files`, top-level `scopes`,
  `per_language.*.imports.mode: "allowlist"`.

A template cleanup scoped to the 75 will leave all of these in place.

## Limits of this report

- **History blind spot:** the history pass searched leaf names. A consumer
  reading a renamed field would be invisible. The one mirrored field
  (`limits.memory.total_mb` → `memory_limit_mb` → `getMemoryLimitMB()`) was
  checked separately.
- **Traced only, not run:** trust/signing behaviour, agent retries, REST TLS,
  persistent runtimes, test context and approval-token expiry.
- **Nulls without a control:** the `naming_conventions` and `error_handling`
  nulls; the trace carries those claims.
- **One correction already made:** during the investigation I first wrote that
  `no_apologetic_language` scans the whole block, and that honouring the
  shipped values would loosen it. That came from tracing to the check function
  rather than its call site, and the controlled run above corrects it.
- **Independent re-tracings:** one per key, plus one adversarial pass, which
  found that error and left the phantom-check claims standing.
- **Direction of error:** the recommendations lean toward removing keys from
  the template. Removal cannot weaken enforcement, because no removed key
  enforces anything. Every WIRE is tightening only.
- **Snapshot:** another session was editing `govern-template.json` in
  parallel. Its changes were not visible on any remote branch at the time of
  writing; re-check the template counts against it.

## Reproduction

Run from anywhere inside a checkout with `build/naab-lang` built.

Results at the snapshot:
- **Real binary:** six PASS and two NULL(0).
- **`BIN=/bin/true`** (an interpreter that does nothing): no PASS. Five
  UNMEASURABLE and one FAIL, and the two NULL arms print the same `0`. That
  is why they are labelled as having no control.

```bash
#!/usr/bin/env bash
# Each arm prints PASS (finding reproduced), FAIL (did not reproduce) or
# UNMEASURABLE (the arm's control did not behave, so its probe says nothing).
set -u
REPO=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "run inside the NAAb checkout"; exit 2; }
BIN="$REPO/build/naab-lang"
[ -x "$BIN" ] || { echo "build first: $BIN not found"; exit 2; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

run() {  # NAME CONFIG_JSON PROGRAM -> RC, OUT
  mkdir -p "$W/$1"
  printf '%s' "$2" > "$W/$1/govern.json"
  printf '%s\n' "$3" > "$W/$1/prog.naab"
  if ! python3 -c 'import json,sys; json.load(sys.stdin)' < "$W/$1/govern.json"; then
    RC=99; OUT="fixture is not valid JSON"; return
  fi
  OUT=$(cd "$W/$1" && FAKE_KEY_INERT_AUDIT=x timeout 60 "$BIN" prog.naab 2>&1); RC=$?
}
verdict() { printf '%-13s %s\n' "$1" "$2"; }

# 1. limits.* aggregates: probe must exit 0, live sibling must exit 3
LIM_PROG='main {
    let a = <<python
import time
t = time.time()
while time.time() - t < 1.3:
    pass
x = 1
y = 2
x + y
>>
    let b = <<python
import time
t = time.time()
while time.time() - t < 1.3:
    pass
data = [0] * 2000000
40
>>
    print("RESULT", a + b)
}'
LIM='"version":"5.0","mode":"enforce","limits":{"code":{"max_total_polyglot_lines":1},"memory":{"total_mb":1,"per_block_mb":1},"timeout":{"total_polyglot":1},"rate":{"cooldown_on_limit_ms":100000},"execution":{"total_executions":1'
run lim_ctl "{$LIM,\"polyglot_blocks\":1}}}" "$LIM_PROG"; CTL=$RC
run lim_probe "{$LIM}}}" "$LIM_PROG"
if [ "$CTL" != 3 ]; then verdict UNMEASURABLE "limits: control polyglot_blocks:1 exited $CTL, not 3"
elif [ "$RC" = 0 ]; then verdict PASS "limits: 5 aggregate keys at 1 -> exit 0; live sibling -> exit 3"
else verdict FAIL "limits: probe exited $RC (a key may now be enforced)"; fi

# 2. code_injection ldap/xpath/template: probe 0, eval control 3
INJ_PROG='main {
    let r = <<python
def ldap_q(conn, user):
    return conn.search_s("dc=x", 2, "(uid=" + user + ")")
def xp(tree, name):
    return tree.xpath("//user[name=" + name + "]")
def tpl(user_input):
    from jinja2 import Template
    return Template(user_input).render()
def dyn(s):
    return eval(s)
7
>>
    print("RAN", r)
}'
INJ='{"version":"5.0","mode":"enforce","restrictions":{"code_injection":{"level":"hard","block_ldap_injection":true,"block_xpath_injection":true,"block_template_injection":true,"block_sql_injection_patterns":false,"block_command_injection":false,"block_dynamic_code_gen":'
run inj_ctl "${INJ}true}}}" "$INJ_PROG"; CTL=$RC
run inj_probe "${INJ}false}}}" "$INJ_PROG"
if [ "$CTL" != 3 ]; then verdict UNMEASURABLE "code_injection: eval control exited $CTL, not 3"
elif [ "$RC" = 0 ]; then verdict PASS "code_injection: ldap/xpath/template true at HARD -> exit 0; eval -> exit 3"
else verdict FAIL "code_injection: probe exited $RC"; fi

# 3. no_apologetic_language: comment fires, string never does
APC='{"version":"5.0","mode":"enforce","code_quality":{"no_apologetic_language":{"level":"hard","scan_comments_only":false,"scan_strings":true}}}'
run apol_ctl "$APC" 'main {
    let r = <<python
# I apologize, I did not check the result
5
>>
    print("RAN", r)
}'; CTL=$RC
run apol_probe "$APC" 'main {
    let r = <<python
msg = "I apologize, I did not check the result"
len(msg)
>>
    print("RAN", r)
}'
if [ "$CTL" != 3 ]; then verdict UNMEASURABLE "apologetic: comment control exited $CTL, not 3"
elif [ "$RC" = 0 ]; then verdict PASS "apologetic: scan_strings:true still ignores strings -> exit 0; comment -> exit 3"
else verdict FAIL "apologetic: string arm exited $RC (scan_strings may now be wired)"; fi

# 4. limits.data.dict_size is enforced, yet the loader says it is not
DCFG='{"version":"5.0","mode":"enforce","limits":{"data":{"dict_size":2}}}'
run dict_ok "$DCFG" 'main {
    let d = {"a": 1, "b": 2}
    print("RAN", d.size())
}'; OK_RC=$RC
run dict_over "$DCFG" 'main {
    let d = {"a": 1, "b": 2, "c": 3}
    print("RAN", d.size())
}'
case "$OUT" in *'"limits.data.dict_size" is parsed but not enforced'*) W_SEEN=1 ;; *) W_SEEN=0 ;; esac
if [ "$OK_RC" != 0 ]; then verdict UNMEASURABLE "dict_size: 2-entry control exited $OK_RC, not 0"
elif [ "$RC" = 3 ] && [ "$W_SEEN" = 1 ]; then verdict PASS "dict_size: enforced (exit 3) while warning 'not enforced'"
else verdict FAIL "dict_size: exit $RC, warning seen=$W_SEEN"; fi

# 5. warn_unknown_keys / suggest_corrections: false still warns
HELLO='main {
    print("RAN")
}'
run unk_ctl '{"version":"5.0","mode":"enforce","limit":{}}' "$HELLO"
case "$OUT" in *'Unknown key "limit"'*) CTL=1 ;; *) CTL=0 ;; esac
run unk_probe '{"version":"5.0","mode":"enforce","limit":{},"meta":{"schema_validation":{"warn_unknown_keys":false,"suggest_corrections":false}}}' "$HELLO"
case "$OUT" in *'did you mean "limits"'*) SEEN=1 ;; *) SEEN=0 ;; esac
if [ "$CTL" != 1 ]; then verdict UNMEASURABLE "unknown keys: control printed no warning"
elif [ "$SEEN" = 1 ]; then verdict PASS "unknown keys: both toggles false -> warning and suggestion still printed"
else verdict FAIL "unknown keys: toggles now suppress the warning"; fi

# 6. agent_dispatch.default_timeout_seconds is LIVE (absent / default / explicit)
AG() { printf '{"version":"5.0","mode":"enforce",%s"agents":{"probe":{"provider":"gemini","model":"stub-model","api_key_env":"FAKE_KEY_INERT_AUDIT","max_tokens":100,"max_turns":5,%s"system_prompt":"x"}}}' "$1" "$2"; }
AGP='use agent

main {
    let h = agent.create("probe")
    print("TIMEOUT", agent.environment(h)["limits"]["timeout_seconds"])
}'
T=""
for arm in "absent||" "dflt|\"agent_dispatch\":{\"default_timeout_seconds\":17},|" "expl|\"agent_dispatch\":{\"default_timeout_seconds\":17},|\"timeout\":33,"; do
  IFS='|' read -r name top agent <<<"$arm"
  run "to_$name" "$(AG "$top" "$agent")" "$AGP"
  T="$T $(printf '%s\n' "$OUT" | sed -n 's/^TIMEOUT //p')"
done
if [ "$T" = " 60 17 33" ]; then verdict PASS "default_timeout_seconds: live (60 / 17 / 33)"
elif [ "$T" = " 60 60 33" ]; then verdict FAIL "default_timeout_seconds: no effect"
else verdict UNMEASURABLE "default_timeout_seconds: read '$T'"; fi

# 7. Phantom checks: NULL WITHOUT A POSITIVE CONTROL (the trace is the evidence)
run naming '{"version":"5.0","mode":"enforce","requirements":{"naming_conventions":{"level":"hard","variables":"snake_case","functions":"snake_case","check_naab_code":true,"check_polyglot_code":true}}}' 'fn BadFunctionName() {
    let BadVariableName = 1
    return BadVariableName
}
main {
    let r = <<python
def BadPythonName():
    return 1
BadPythonName()
>>
    print("RAN", BadFunctionName(), r)
}'
verdict "NULL($RC)" "requirements.naming_conventions at HARD vs violating names (expect 0; no control exists)"
run errh '{"version":"5.0","mode":"enforce","requirements":{"error_handling":{"level":"hard","require_try_catch":true,"require_catch_body":true}}}' 'main {
    let r = <<python
def f():
    return int("x")
5
>>
    print("RAN", r)
}'
verdict "NULL($RC)" "requirements.error_handling at HARD, no try/catch anywhere (expect 0; no control exists)"
```

The "no baseline key warns at load" claim [V] was measured separately:
- **Setup:** copy the root template to an empty directory as `govern.json`,
  delete its `extends` key (its base file does not ship, so loading exits 4),
  and run a one-line `main { print("RAN") }`.
- **Result:** stderr held 15 `Warning:` lines, and none contained a baseline
  key's leaf name.
- **Control:** the same stderr includes the live `limits.data.*` and
  `capabilities.*` warnings, so the warning path was exercised.
