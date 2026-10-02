#!/usr/bin/env bash
# ============================================================
# test_agent_harness_example.sh -- examples/agent_harness is locked, runs
# under its lock, and every governance setting it relies on actually bites.
#
# The example's govern.json was written first and signed; the harness was then
# built to fit it. A setting that parses but does nothing reads exactly like
# one that works, so each setting the harness depends on gets an arm that
# BREAKS the rule and must be refused. Every refusal-expecting arm is
# meaningless unless the unmodified harness passes under the SAME signed
# config, so AH-01 is the control and each arm states what it changed.
#
# Each variant is a copy of the example, patched, re-signed with a key made
# for this test (the run's trust store holds only that key), and run on the
# local stub with NAAB_SIGNING_KEY removed from the environment.
#
#   lock
#   AH-L1  the committed signature verifies against keys/harness-signing.pub
#          (tools/lock_check.sh: ok; a STALE signature is reported, not failed
#          -- it is the configured authority decay, not tampering)
#   AH-L2  one edited byte in govern.json breaks the lock
#   AH-L3  a valid signature with an old timestamp is classified STALE, not
#          broken (positive control for AH-L1's tolerance: stale is detected)
#   AH-L4  run.sh refuses to run a copy whose lock is broken
#   AH-11  a locked flag (--tree-walk) on the signed config is an INTEGRITY
#          BLOCK; AH-01 is the same config without the flag
#
#   run
#   AH-01  control: the harness runs end to end under the signed config
#   AH-02  contracts.must_call: run_step without record_validation -> HARD
#   AH-03  taint: agent output written unsanitized inside write_report -> block
#   AH-03c control: the same write with taint tracking off succeeds
#   AH-04  capabilities.functions: the pure verifier reads a file -> HARD
#   AH-05  languages.blocked: a <<python>> block -> HARD
#   AH-06  no_secrets: a worker response carrying an AWS-shaped key -> HARD
#   AH-07  output_contract: worker JSON without "defects" -> refused
#   AH-08  ground truth: an invented defect fails its step, is recorded
#          (VALIDATION_RECORDED passed=false) and charges coherence (S22)
#   AH-09  tool allowlist: a registered tool govern.json does not list is
#          blocked and never executed
#   AH-12  contracts.must_produce: a plan check that accepts every plan fails
#          its golden test
#   AH-13  contracts.must_produce: a verifier that accepts every defect fails
#          its golden test (dict-valued expectation)
#   AH-14  code_quality.complexity_floor: a hollow verifier is refused (the
#          checks that would catch it first are switched off in this arm only)
#   AH-15  code_quality.intent_validation: a declared intent the code does not
#          carry out is refused
#   AH-16  capabilities.filesystem.blocked_paths: the harness may not write
#          its own telemetry file
#   AH-17  capabilities.network: http.* is a HARD block even for a function
#          granted NET_CONNECT
#   AH-18  prerequisites: a required environment variable that is missing
#          stops the run before any model call
#   AH-10  key check: tools/check_govern_keys.py passes on the shipped config
#          and FAILS on a misspelled key (positive control)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
EX="${AH_EXAMPLE:-$REPO/examples/agent_harness}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/agent-harness-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2"; }

echo "=== examples/agent_harness: locked, runs, and every relied-on setting bites ==="

RUN_ARMS="AH-01 AH-02 AH-03 AH-03c AH-04 AH-05 AH-06 AH-07 AH-08 AH-09 AH-11 AH-12 AH-13 AH-14 AH-15 AH-16 AH-17 AH-18"
ALL="AH-L1 AH-L2 AH-L3 AH-L4 $RUN_ARMS AH-10"
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
unset NAAB_SIGNING_KEY

# One key for every variant, trusted in this test's isolated store only.
TKEY="$TEST_TMP/test-signing.pem"
"$NAAB" --keygen "$TKEY" >/dev/null 2>&1 && "$NAAB" --trust-key "$TKEY.pub" >/dev/null 2>&1 \
    || { for id in $ALL; do skip "$id" "could not create a test signing key (UNMEASURABLE)"; done
         echo ""; echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"; exit 0; }

has() { case "$OUT" in *"$1"*) return 0 ;; *) return 1 ;; esac; }

# ---------------------------------------------------------------- lock ----
lock_copy() {  # $1 = name; copies the example (with its signature and key)
    L="$TEST_TMP/$1"; rm -rf "$L"; mkdir -p "$L"
    cp -r "$EX/src" "$EX/tools" "$EX/keys" "$EX/workspace" "$EX/fixtures" "$EX/run.sh" "$L/"
}
LOCK_OUT="$(NAAB="$NAAB" NAAB_REPO="$REPO" "$EX/tools/lock_check.sh" 2>&1)"; LOCK_RC=$?
case "$LOCK_RC" in
    0) pass "AH-L1" "the committed signature verifies against keys/harness-signing.pub" ;;
    2) pass "AH-L1" "the committed signature is intact but STALE -- re-sign with tools/sign.sh ($(head -1 <<<"$LOCK_OUT"))" ;;
    3) skip "AH-L1" "lock check unmeasurable: $LOCK_OUT" ;;
    *) fail "AH-L1" "the committed govern.json does not verify -- it was edited after signing" "$LOCK_OUT" ;;
esac

lock_copy l2
sed -i.bak 's/"timeout": 600 }/"timeout": 601 }/' "$L/src/govern.json" && rm -f "$L/src/govern.json.bak"
if cmp -s "$EX/src/govern.json" "$L/src/govern.json"; then
    skip "AH-L2" "the edit did not apply (fixture moved) -- UNMEASURABLE"
else
    L2OUT="$(NAAB="$NAAB" NAAB_REPO="$REPO" "$L/tools/lock_check.sh" 2>&1)"; L2RC=$?
    if [ "$L2RC" -eq 1 ] && [[ "$L2OUT" == LOCK\|broken* ]]; then
        pass "AH-L2" "one edited value in govern.json breaks the lock"
    else
        fail "AH-L2" "an edited govern.json still passes the lock check (rc=$L2RC)" "$L2OUT"
    fi
    # AH-L4 run.sh must refuse the broken copy before running anything.
    R4OUT="$(cd "$L" && NAAB="$NAAB" NAAB_REPO="$REPO" ./run.sh --stub 2>&1)"; R4RC=$?
    if [ "$R4RC" -ne 0 ] && [[ "$R4OUT" == *"refusing to run"* ]] && [ ! -d "$L/out" ]; then
        pass "AH-L4" "run.sh refuses a copy whose lock is broken, before creating a run"
    else
        fail "AH-L4" "run.sh ran (or tried to) with a broken lock (rc=$R4RC)" "$(tail -3 <<<"$R4OUT")"
    fi
fi

# AH-L3: sign the copy with the test key, timestamped 90 days ago. The engine
# signs content + ":" + timestamp, so openssl can produce a VALID old signature.
lock_copy l3
cp "$TKEY.pub" "$L/keys/harness-signing.pub"
if command -v openssl >/dev/null 2>&1; then
    OLD=$(( $(date +%s) - 90*86400 ))
    { cat "$L/src/govern.json"; printf ':%s' "$OLD"; } > "$TEST_TMP/l3.payload"
    if SIG=$(openssl pkeyutl -sign -rawin -inkey "$TKEY" -in "$TEST_TMP/l3.payload" 2>/dev/null | base64 | tr -d '\n') && [ -n "$SIG" ]; then
        printf 'ed25519:%s:%s' "$SIG" "$OLD" > "$L/src/govern.json.sig"
        L3OUT="$(NAAB="$NAAB" NAAB_REPO="$REPO" "$L/tools/lock_check.sh" 2>&1)"; L3RC=$?
        if [ "$L3RC" -eq 2 ] && [[ "$L3OUT" == LOCK\|stale* ]]; then
            pass "AH-L3" "a valid signature older than max_signature_age_days is classified STALE, not broken"
        else
            fail "AH-L3" "an old valid signature was not reported stale (rc=$L3RC)" "$L3OUT"
        fi
    else
        skip "AH-L3" "openssl cannot sign Ed25519 here (UNMEASURABLE)"
    fi
else
    skip "AH-L3" "openssl not available (UNMEASURABLE)"
fi

# ----------------------------------------------------------------- run ----
# run_variant NAME PATCH_PY [FLAGS]
# Copies the example into a fresh directory, applies PATCH_PY (python that may
# edit H = harness source, F = stub fixture dict, G = govern dict), points
# every agent at a fresh stub, re-signs with the test key, runs.
# Sets RC, OUT (stdout+stderr), D (run dir).
run_variant() {
    D="$TEST_TMP/$1"; rm -rf "$D"; mkdir -p "$D/out"
    cp "$EX/src/harness.naab" "$EX/src/govern.json" "$D/"
    cp -r "$EX/workspace" "$EX/fixtures" "$D/"
    python3 - "$D" "$2" <<'PY'
import json, re, sys
d, patch = sys.argv[1], sys.argv[2]
H = open(d + "/harness.naab").read()
F = json.load(open(d + "/fixtures/stub_responses.json"))
G = json.load(open(d + "/govern.json"))
H0 = H
exec(patch)
if re.search(r"(^|\n)\s*H\s*=", patch) and H == H0:
    sys.exit("patch did not change the harness")
open(d + "/harness.naab", "w").write(H)
json.dump(F, open(d + "/fixtures/stub_responses.json", "w"), indent=1)
json.dump(G, open(d + "/govern.json", "w"), indent=1)
PY
    [ $? -eq 0 ] || { RC=-2; OUT="patch did not apply"; return 1; }
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
    NAAB_SIGNING_KEY="$TKEY" "$NAAB" --sign-governance "$D/govern.json" >/dev/null 2>&1 \
        || { RC=-3; OUT="could not sign"; kill "$STUB_PID" 2>/dev/null; STUB_PID=""; return 1; }
    # shellcheck disable=SC2086
    (cd "$D" && timeout 120 "$NAAB" ${3:-} harness.naab > out/stdout.txt 2> out/stderr.txt); RC=$?
    kill "$STUB_PID" 2>/dev/null; STUB_PID=""
    OUT="$(cat "$D/out/stdout.txt" "$D/out/stderr.txt")"
    return 0
}

# The worker's step-2 answer, so arms can replace it.
W2_IDX=3

CONTROL_OK=0
if run_variant ah01 ""; then
    if [ "$RC" -eq 0 ] && has "STEP|1|passed=true" && has "STEP|2|passed=true" && has "STEP|3|passed=true" \
       && has "VERDICT|APPROVED" && [ -s "$D/out/report.json" ]; then
        CONTROL_OK=1
        pass "AH-01" "control: the harness runs end to end under the signed config (3 steps verified, APPROVED, report written)"
    else
        fail "AH-01" "harness did not complete (rc=$RC) -- every refusal arm below is void" "$(tail -8 <<<"$OUT")"
    fi
else
    skip "AH-01" "variant setup failed: $OUT (UNMEASURABLE)"
fi

if [ "$CONTROL_OK" -ne 1 ]; then
    for id in $RUN_ARMS; do [ "$id" = AH-01 ] || skip "$id" "control failed; a refusal here would prove nothing"; done
else

expect_block() {  # ID DESCRIPTION NEEDLE [RC]
    local want_rc="${4:-3}"
    if [ "$RC" -lt 0 ]; then skip "$1" "variant setup failed: $OUT (UNMEASURABLE)"; return; fi
    if [ "$RC" -eq "$want_rc" ] && has "$3"; then pass "$1" "$2"
    else fail "$1" "did not bite (rc=$RC, wanted $want_rc + '$3')" "$(grep -m3 -iE 'error|block|violation' <<<"$OUT")"; fi
}

run_variant ah11 "" "--tree-walk"
expect_block "AH-11" "a locked flag (--tree-walk) on the signed config is an INTEGRITY BLOCK" "flag '--tree-walk' is locked"

run_variant ah02 'H = H.replace("    agent.record_validation(worker, passed, detail)\n", "")'
expect_block "AH-02" "run_step without record_validation is a HARD block" "must call 'record_validation'"

# The unsanitized write goes INSIDE write_report, which holds FS_WRITE and still
# calls sanitize_report -- so neither the function capability nor the must_call
# contract can be what refuses it. Only taint can.
RAW='H = H.replace("    let text = sanitize_report(json.stringify(report))\n", "    file.write(\"out/raw_report.json\", json.stringify(report))\n    let text = sanitize_report(json.stringify(report))\n")'
run_variant ah03 "$RAW"
if [ "$RC" -ne 0 ] && [ "$RC" -ge 0 ] && [ ! -f "$D/out/raw_report.json" ] && has "aint"; then
    pass "AH-03" "unsanitized agent output written to a file is refused (taint)"
else
    fail "AH-03" "taint did not bite (rc=$RC, file written: $([ -f "$D/out/raw_report.json" ] && echo yes || echo no))" "$(grep -m2 -i 'taint\|error' <<<"$OUT")"
fi
# Control: with taint OFF the same write must go through, or AH-03 could be
# refusing for another reason. (Narrowing `sources` is NOT a control:
# agent_impl.cpp taints every send/propose/commit result, tool result and tool
# argument whenever taint tracking is on, whatever the source list says.)
run_variant ah03c "$RAW
G['taint_tracking']['enabled'] = False"
if [ "$RC" -eq 0 ] && [ -f "$D/out/raw_report.json" ]; then
    pass "AH-03c" "control: with taint tracking off the same write succeeds, so taint is what refuses AH-03"
else
    fail "AH-03c" "the write is refused even with taint off (rc=$RC) -- AH-03 proves nothing" "$(grep -m2 -i 'taint\|error' <<<"$OUT")"
fi

run_variant ah04 'H = H.replace("fn verify_claims(claims, truth) {\n", "fn verify_claims(claims, truth) {\n    let peek = file.read(\"fixtures/task.json\")\n")'
expect_block "AH-04" "the pure verifier (default entry, no actions) reading a file is a HARD block naming it" "Undeclared action in 'verify_claims'"

run_variant ah05 'H = H.replace("main {\n", "main {\n    let probe = <<python\n1 + 1\n>>\n", 1)'
expect_block "AH-05" "a <<python>> block is refused (HARD)" 'Language "python" is blocked'

run_variant ah06 'F["routes"]["AH-WORKER"]["responses"][1]["content"] = "{\"step\": 1, \"files\": [\"README.md\"], \"defects\": [], \"note\": \"key AKIAIOSFODNN7EXAMPLE\"}"'
expect_block "AH-06" "a worker response carrying an AWS-shaped key ends the run (HARD)" "no_secrets"

run_variant ah07 "F['routes']['AH-WORKER']['responses'][$W2_IDX]['content'] = '{\"step\": 2, \"files\": [\"inventory.py\"]}'"
if [ "$RC" -ne 0 ] && [ "$RC" -ge 0 ] && has "output contract violation" && ! has "STEP|2|"; then
    pass "AH-07" "worker JSON missing a required field is refused before the harness uses it"
else
    fail "AH-07" "output contract did not bite (rc=$RC)" "$(grep -m2 -i 'contract\|STEP|2' <<<"$OUT")"
fi

run_variant ah08 "F['routes']['AH-WORKER']['responses'][$W2_IDX]['content'] = '{\"step\": 2, \"files\": [\"inventory.py\"], \"defects\": [{\"file\": \"inventory.py\", \"line\": 14, \"explanation\": \"invented\"}]}'"
VREC=$(python3 - "$D/out/telemetry.jsonl" <<'PY'
import json, sys
fails, charged = 0, 0
try:
    for l in open(sys.argv[1]):
        e = json.loads(l)
        if e.get("event_type") == "VALIDATION_RECORDED" and e.get("config_name") == "worker" and e.get("passed") == "false":
            fails += 1
        if e.get("event_type") == "CDD_TURN" and e.get("config_name") == "worker" and "validation_outcome" in (e.get("penalties_detail") or ""):
            charged += 1
except OSError:
    pass
print(fails, charged)
PY
)
read -r VF VC <<<"$VREC"
if has "STEP|2|passed=false" && [ "${VF:-0}" -ge 1 ] && [ "${VC:-0}" -ge 1 ]; then
    pass "AH-08" "an invented defect fails its step, is recorded (passed=false x$VF) and charges coherence (x$VC)"
else
    fail "AH-08" "ground truth did not reach governance (fails=$VF charged=$VC rc=$RC)" "$(grep -m3 'STEP|\|Error' <<<"$OUT")"
fi

run_variant ah09 'H = H.replace("    let planner = agent.create(\"planner\")", "    agent.register_tool(\"delete_file\", read_file, {\"description\": \"Delete a file.\", \"parameters\": {\"path\": {\"type\": \"string\"}}})\n    let planner = agent.create(\"planner\")")
F["routes"]["AH-WORKER"]["responses"][0] = {"tool_calls": [{"name": "delete_file", "args": {"path": "inventory.py"}}]}'
TOOLS=$(python3 - "$D/out/telemetry.jsonl" <<'PY'
import json, sys
blocked = executed = 0
try:
    for l in open(sys.argv[1]):
        e = json.loads(l)
        if e.get("event_type") == "AGENT_TOOL_BLOCKED":
            blocked += 1
        if e.get("event_type") == "AGENT_TOOL_CALL" and "delete_file" in json.dumps(e):
            executed += 1
except OSError:
    pass
print(blocked, executed)
PY
)
read -r BLOCKED EXEC <<<"$TOOLS"
if [ "${BLOCKED:-0}" -ge 1 ] && [ "${EXEC:-0}" -eq 0 ]; then
    pass "AH-09" "a registered tool missing from govern.json tools[] is blocked and never executed"
else
    fail "AH-09" "tool allowlist did not bite (blocked=$BLOCKED executed=$EXEC rc=$RC)"
fi

run_variant ah12 'H = H.replace("    let problems = []\n    let steps = plan.get(\"steps\")\n", "    let problems = []\n    let steps = plan.get(\"steps\")\n    if steps != null { return problems }\n", 1)'
expect_block "AH-12" "a plan check that accepts every plan fails its golden test" "must_produce: 'plan_problems' returned wrong value"

run_variant ah13 'H = H.replace("            let matched = false\n", "            let matched = true\n", 1)'
expect_block "AH-13" "a verifier that accepts every defect fails its golden test" "must_produce: 'verify_claims' returned wrong value"

run_variant ah14 'import re
H = re.sub(r"fn verify_claims\(claims, truth\) \{.*?\n\}\n", "fn verify_claims(claims, truth) {\n    let known_files = truth.get(\"files\")\n    let rejected = []\n    return {\"passed\": rejected.length() == 0, \"verified\": known_files.length(), \"rejected\": rejected}\n}\n", H, flags=re.S)
del G["contracts"]["functions"]["verify_claims"]["must_produce"]
G["code_quality"]["intent_validation"]["enabled"] = False
G["code_quality"]["no_incomplete_logic"]["enabled"] = False'
expect_block "AH-14" "a hollow verifier is refused by the complexity floor" "The verifier must contain real logic"

run_variant ah15 'G["code_quality"]["intent_validation"]["function_intents"]["load_json"] = "compress archives and upload them to remote storage"'
expect_block "AH-15" "a declared intent the code does not carry out is refused" "Intent mismatch on 'load_json'"

run_variant ah16 'H = H.replace("    file.write(\"out/report.json\", text)\n", "    file.write(\"out/report.json\", text)\n    file.write(\"out/telemetry.jsonl\", \"\")\n")'
expect_block "AH-16" "the harness may not write its own telemetry file" "File path blocked by governance: out/telemetry.jsonl"

run_variant ah17 'H = H.replace("use array\n", "use array\nuse http\n").replace("fn load_json(path) {\n", "fn load_json(path) {\n    let ping = http.get(\"http://127.0.0.1:9/\")\n")
G["capabilities"]["functions"]["load_json"]["allowed_actions"].append("NET_CONNECT")'
expect_block "AH-17" "http.* is a HARD block even for a function granted NET_CONNECT" "Network access is not allowed"

run_variant ah18 'G["prerequisites"]["checks"][0]["name"] = "AH_REQUIRED_BUT_UNSET"'
expect_block "AH-18" "a missing required environment variable stops the run" "Prerequisite failed: env_var 'AH_REQUIRED_BUT_UNSET'"

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
