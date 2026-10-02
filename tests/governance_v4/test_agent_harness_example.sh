#!/usr/bin/env bash
# ============================================================
# test_agent_harness_example.sh -- examples/agent_harness runs, and every
# governance setting it relies on actually bites.
#
# The example ships a govern.json that a builder is meant to trust. A setting
# that parses but does nothing reads exactly like one that works, so each one
# the harness depends on gets an arm that BREAKS the rule and must be refused.
# Every refusal-expecting arm is meaningless unless the unmodified harness
# passes, so AH-01 is the control and each arm states what it changed.
#
#   AH-01  control: the skeleton runs end to end on the stub (exit 0, three
#          steps verified, verdict APPROVED, report written)
#   AH-02  contracts: run_step without record_validation -> HARD block
#   AH-03  taint: agent output written to a file without the sanitizer -> block
#          (inside write_report, so capabilities and contracts cannot be the
#          reason it is refused)
#   AH-03c control: the same write with taint tracking off succeeds
#   AH-04  function capabilities: a pure helper (default entry, no actions)
#          that reads a file -> HARD block naming the function
#   AH-05  language blocklist: a <<python>> block -> HARD block
#   AH-06  no_secrets: a worker response carrying an AWS-shaped key -> HARD
#   AH-07  output contract: worker JSON without "defects" -> refused
#   AH-08  ground truth: a claimed defect not in truth.json -> step fails AND
#          governance records it (VALIDATION_RECORDED passed=false, and S22
#          charges coherence on the worker's next analysed turn)
#   AH-09  tool allowlist: the model calls a registered tool that govern.json
#          does not list -> blocked, never executed
#   AH-10  key check: tools/check_govern_keys.py passes on the shipped config
#          and FAILS on a config with a misspelled key (positive control)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
EX="$REPO/examples/agent_harness"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/agent-harness-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2"; }

echo "=== examples/agent_harness: runs, and every relied-on setting bites ==="

ALL="AH-01 AH-02 AH-03 AH-03c AH-04 AH-05 AH-06 AH-07 AH-08 AH-09 AH-10"
IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=1
if [ "$IS_WINDOWS" -eq 1 ] || ! command -v python3 >/dev/null 2>&1; then
    for id in $ALL; do skip "$id" "agent stub unsupported here (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"; exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "${TEST_TMP:?}"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
export GEMINI_API_KEY="stub-key"

# run_variant NAME PATCH_PY
# Copies the example into a fresh run directory, applies PATCH_PY (python that
# may edit H = harness source, F = stub fixture dict, G = govern dict), points
# every agent at a fresh stub, runs. Sets RC, OUT (stdout+stderr), D (run dir).
run_variant() {
    D="$TEST_TMP/$1"; mkdir -p "$D/out"
    cp "$EX/src/harness.naab" "$EX/src/govern.json" "$D/"
    cp -r "$EX/workspace" "$EX/fixtures" "$D/"
    python3 - "$D" "$2" <<'PY'
import json, sys
d, patch = sys.argv[1], sys.argv[2]
H = open(d + "/harness.naab").read()
F = json.load(open(d + "/fixtures/stub_responses.json"))
G = json.load(open(d + "/govern.json"))
exec(patch)
open(d + "/harness.naab", "w").write(H)
json.dump(F, open(d + "/fixtures/stub_responses.json", "w"), indent=1)
json.dump(G, open(d + "/govern.json", "w"), indent=1)
PY
    STUB_PID=""
    if ! start_stub "$D/fixtures/stub_responses.json" "$D/out"; then RC=-1; OUT=""; return 1; fi
    python3 - "$D/govern.json" "$STUB_PORT" <<'PY'
import json, sys
p, port = sys.argv[1], sys.argv[2]
cfg = json.load(open(p))
for a in cfg["agents"].values():
    a["api_base"] = "http://127.0.0.1:" + port
json.dump(cfg, open(p, "w"), indent=1)
PY
    (cd "$D" && timeout 120 "$NAAB" harness.naab > out/stdout.txt 2> out/stderr.txt); RC=$?
    kill "$STUB_PID" 2>/dev/null; STUB_PID=""
    OUT="$(cat "$D/out/stdout.txt" "$D/out/stderr.txt")"
    return 0
}

has() { case "$OUT" in *"$1"*) return 0 ;; *) return 1 ;; esac; }

# The worker's step-2 answer, so arms can replace it.
W2_IDX=3

# ---- AH-01 control ----
CONTROL_OK=0
if run_variant ah01 ""; then
    if [ "$RC" -eq 0 ] && has "STEP|1|passed=true" && has "STEP|2|passed=true" && has "STEP|3|passed=true" \
       && has "VERDICT|APPROVED" && [ -s "$D/out/report.json" ]; then
        CONTROL_OK=1
        pass "AH-01" "control: the skeleton runs end to end (3 steps verified, APPROVED, report written)"
    else
        fail "AH-01" "skeleton did not complete (rc=$RC) -- every refusal arm below is void" "$(tail -5 "$D/out/stdout.txt")"
    fi
else
    skip "AH-01" "stub failed to start (UNMEASURABLE)"
fi
if [ "$CONTROL_OK" -ne 1 ]; then
    for id in AH-02 AH-03 AH-03c AH-04 AH-05 AH-06 AH-07 AH-08 AH-09; do skip "$id" "control failed; a refusal here would prove nothing"; done
else

# ---- AH-02 contracts.functions.run_step.must_call ----
run_variant ah02 'H = H.replace("    agent.record_validation(worker, passed, detail)\n", "")'
if [ "$RC" -eq 3 ] && has "run_step" && has "record_validation"; then
    pass "AH-02" "run_step without record_validation is a HARD block"
else
    fail "AH-02" "contract did not bite (rc=$RC)" "$(grep -m2 -i 'contract\|error' <<<"$OUT")"
fi

# ---- AH-03 taint_tracking ----
# The unsanitized write goes INSIDE write_report, which holds FS_WRITE and still
# calls sanitize_report -- so neither the function capability nor the must_call
# contract can be what refuses it. Only taint can.
run_variant ah03 'H = H.replace("    let clean = sanitize_report(json.stringify(report))\n", "    file.write(\"out/raw_report.json\", json.stringify(report))\n    let clean = sanitize_report(json.stringify(report))\n")'
if [ "$RC" -ne 0 ] && [ ! -f "$D/out/raw_report.json" ] && has "aint"; then
    pass "AH-03" "unsanitized agent output written to a file is refused (taint)"
else
    fail "AH-03" "taint did not bite (rc=$RC, file written: $([ -f "$D/out/raw_report.json" ] && echo yes || echo no))" "$(grep -m2 -i 'taint\|error' <<<"$OUT")"
fi

# ---- AH-03c control: the same write with taint tracking OFF must go through,
# or AH-03 could be refusing for some other reason. (Removing agent.send from
# `sources` is NOT a control: agent_impl.cpp taints every send/propose/commit
# result, tool result and tool argument whenever taint tracking is on, whatever
# the source list says -- verified while writing this test.)
run_variant ah03c 'H = H.replace("    let clean = sanitize_report(json.stringify(report))\n", "    file.write(\"out/raw_report.json\", json.stringify(report))\n    let clean = sanitize_report(json.stringify(report))\n")
G["taint_tracking"]["enabled"] = False'
if [ "$RC" -eq 0 ] && [ -f "$D/out/raw_report.json" ]; then
    pass "AH-03c" "control: with taint tracking off the same write succeeds, so taint is what refuses AH-03"
else
    fail "AH-03c" "the write is refused even with taint off (rc=$RC) -- AH-03 proves nothing" "$(grep -m2 -i 'taint\|error' <<<"$OUT")"
fi

# ---- AH-04 capabilities.functions (default grants nothing) ----
run_variant ah04 'H = H.replace("fn verify_claims(claims, truth) {\n", "fn verify_claims(claims, truth) {\n    let peek = file.read(\"fixtures/task.json\")\n")'
if [ "$RC" -eq 3 ] && has "verify_claims"; then
    pass "AH-04" "a pure helper (default entry, no actions) that reads a file is a HARD block naming it"
else
    fail "AH-04" "function capability did not bite (rc=$RC)" "$(grep -m2 -i 'capabilit\|error' <<<"$OUT")"
fi

# ---- AH-05 languages.blocked ----
run_variant ah05 'H = H.replace("main {\n", "main {\n    let probe = <<python\n1 + 1\n>>\n", 1)'
if [ "$RC" -eq 3 ] && has 'Language "python" is blocked'; then
    pass "AH-05" "a <<python>> block is refused (HARD)"
else
    fail "AH-05" "language blocklist did not bite (rc=$RC)" "$(grep -m2 -i 'language\|error' <<<"$OUT")"
fi

# ---- AH-06 code_quality.no_secrets on responses ----
run_variant ah06 'F["routes"]["AH-WORKER"]["responses"][1]["content"] = "{\"step\": 1, \"files\": [\"README.md\"], \"defects\": [], \"note\": \"key AKIAIOSFODNN7EXAMPLE\"}"'
if [ "$RC" -eq 3 ] && has "no_secrets"; then
    pass "AH-06" "a worker response carrying an AWS-shaped key ends the run (HARD)"
else
    fail "AH-06" "response secret scan did not bite (rc=$RC)" "$(grep -m2 -i 'secret\|error' <<<"$OUT")"
fi

# ---- AH-07 output_contract ----
run_variant ah07 "F['routes']['AH-WORKER']['responses'][$W2_IDX]['content'] = '{\"step\": 2, \"files\": [\"inventory.py\"]}'"
if [ "$RC" -ne 0 ] && has "output contract violation" && ! has "STEP|2|"; then
    pass "AH-07" "worker JSON missing a required field is refused before the harness uses it"
else
    fail "AH-07" "output contract did not bite (rc=$RC)" "$(grep -m2 -i 'contract\|STEP|2' <<<"$OUT")"
fi

# ---- AH-08 ground truth -> governance ----
run_variant ah08 "F['routes']['AH-WORKER']['responses'][$W2_IDX]['content'] = '{\"step\": 2, \"files\": [\"inventory.py\"], \"defects\": [{\"file\": \"inventory.py\", \"line\": 14, \"explanation\": \"invented\"}]}'"
VREC=$(python3 - "$D/out/telemetry.jsonl" <<'PY'
import json, sys
fails, charged = 0, 0
for l in open(sys.argv[1]):
    e = json.loads(l)
    if e.get("event_type") == "VALIDATION_RECORDED" and e.get("config_name") == "worker" and e.get("passed") == "false":
        fails += 1
    if e.get("event_type") == "CDD_TURN" and e.get("config_name") == "worker" and "validation_outcome" in (e.get("penalties_detail") or ""):
        charged += 1
print(fails, charged)
PY
)
read -r VF VC <<<"$VREC"
if has "STEP|2|passed=false" && [ "${VF:-0}" -ge 1 ] && [ "${VC:-0}" -ge 1 ]; then
    pass "AH-08" "an invented defect fails its step, is recorded (passed=false x$VF) and charges coherence (x$VC)"
else
    fail "AH-08" "ground truth did not reach governance (fails=$VF charged=$VC)" "$(grep -m3 'STEP|' <<<"$OUT")"
fi

# ---- AH-09 tool allowlist (dual gate) ----
run_variant ah09 'H = H.replace("    let planner = agent.create(\"planner\")", "    agent.register_tool(\"delete_file\", read_file, {\"description\": \"Delete a file.\", \"parameters\": {\"path\": {\"type\": \"string\"}}})\n    let planner = agent.create(\"planner\")")
F["routes"]["AH-WORKER"]["responses"][0] = {"tool_calls": [{"name": "delete_file", "args": {"path": "inventory.py"}}]}'
BLOCKED=$(grep -c '"event_type":"AGENT_TOOL_BLOCKED"\|"event_type": "AGENT_TOOL_BLOCKED"' "$D/out/telemetry.jsonl" 2>/dev/null)
EXEC=$(python3 - "$D/out/telemetry.jsonl" <<'PY'
import json, sys
n = 0
for l in open(sys.argv[1]):
    e = json.loads(l)
    if e.get("event_type") == "AGENT_TOOL_CALL" and "delete_file" in json.dumps(e):
        n += 1
print(n)
PY
)
if [ "${BLOCKED:-0}" -ge 1 ] && [ "${EXEC:-0}" -eq 0 ]; then
    pass "AH-09" "a registered tool missing from govern.json tools[] is blocked and never executed"
else
    fail "AH-09" "tool allowlist did not bite (blocked=$BLOCKED executed=$EXEC rc=$RC)"
fi

fi  # control ok

# ---- AH-10 key check, with a positive control ----
KC="$EX/tools/check_govern_keys.py"
KOUT="$(python3 "$KC" "$EX/src/govern.json" --repo "$REPO" 2>&1)"; KRC=$?
python3 - "$EX/src/govern.json" "$TEST_TMP/typo.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
c["agents"]["worker"]["max_tool_call_per_turn"] = 4   # misspelled on purpose
json.dump(c, open(sys.argv[2], "w"))
PY
TOUT="$(python3 "$KC" "$TEST_TMP/typo.json" --repo "$REPO" 2>&1)"; TRC=$?
if [ "$KRC" -eq 0 ] && [ "$TRC" -eq 1 ] && [[ "$TOUT" == *"max_tool_call_per_turn"* ]]; then
    pass "AH-10" "shipped govern.json has no unread keys, and a misspelled key is caught"
else
    fail "AH-10" "key check wrong (shipped rc=$KRC, typo rc=$TRC)" "$KOUT | $TOUT"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
