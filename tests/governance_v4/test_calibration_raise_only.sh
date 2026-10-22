#!/usr/bin/env bash
# ============================================================
# test_calibration_raise_only.sh — a governed program may only RAISE its scoring weights (A60)
#
# THE DEFECT
#
# With `scoring` and `scoring_calibration` enabled, a calibration weight
# replaces the configured weight of a rule. governance.calibrate() is a call
# the governed PROGRAM makes, so calibrate(rule, 0) zeroed the weight of the
# program's own violations and it passed the cumulative risk gate it was
# failing (exit 2 -> exit 0). Writing the calibration file before the first
# violation did the same without calling calibrate at all. Measured at
# dd7744fd; the template called the feature "operator-driven".
#
# THE FIX (owner's decision, 2026-10-22: raise-only)
#
# calibrate() refuses a weight below the rule's current weight -- its override
# if it has one, else the highest configured weight over the rule's contexts --
# and the program may not write the calibration file (reads stay allowed).
# Lowering a weight is the operator's: they edit the file outside the run.
#
#   CR-00  CONTROL: ten advisory violations reach the risk gate (exit 2,
#          score 30 = 10 x default weight 3) -- the gate this is about fires
#   CR-01  calibrate(rule, 0) is refused (success false, `refused` set) and
#          the run still exits 2 at score 30
#   CR-02  calibrate(rule, 5) -- a raise -- is accepted: score 50 (the
#          success-expecting control; a refuse-everything gate fails it)
#   CR-03  an operator-written file setting weight 0 is loaded: exit 0. The
#          operator route still works
#   CR-04  over an operator override of 0, calibrate(rule, 1) is a raise and
#          is accepted: score 10. The floor is the CURRENT weight
#   CR-05  a contextual rule weight sets the floor: with rule:category = 10,
#          calibrate(rule, 5) is refused and calibrate(rule, 10) accepted
#   CR-06  file.write of the calibration file: refused (exit 3,
#          governance.state_files) and the operator's file is unchanged
#   CR-07  file.read of the same file: allowed (protection is write-only)
#   CR-08  with scoring_calibration off, the same path is an ordinary file:
#          the write lands (protection follows what the engine reads)
#   CR-09  a relative path resolves beside the telemetry file, as the engine
#          loads it: logs/cal.json is refused, ./cal.json is an ordinary file
#   CR-10  embedded Python open(file, "w") is refused too (same decision
#          point); UNMEASURABLE where Python is not embedded
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

# Every govern.json below is unsigned; with a key in the ambient trust store an
# unsigned config is an INTEGRITY BLOCK at exit 3, the code a refusal arm reads.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/naab-cal-raise-$$"
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo "  PASS [$1] $2"; }
# Detail on the FAIL line: run-all-tests.sh's summary keeps only PASS/FAIL/Result lines.
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
cleanup() { rm -rf "$TEST_TMP"; teardown_isolated_trust; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
W="$TEST_TMP/w"

echo "=== test_calibration_raise_only.sh ==="

if [ ! -x "$NAAB" ]; then
    skip "CR-ALL" "naab-lang not built"; echo "Result: 0 passed, 0 failed, 1 skipped"; exit 0
fi

RULE="code_quality.no_placeholders"
OPERATOR_ZERO="{\"version\": 1, \"overrides\": {\"$RULE\": {\"weight\": 0, \"reason\": \"operator\"}}}"

# $1 = extra JSON members (may be empty), $2 = calibration enabled (true|false), $3 = calibration path
cfg() {
    printf '{"mode": "enforce", "security": {"sandbox_level": "elevated"},
 "capabilities": {"filesystem": {"mode": "write"}},
 "scoring": {"enabled": true, "yellow_threshold": 3, "red_threshold": 8%s},
 "code_quality": {"no_placeholders": {"enabled": true, "level": "advisory"}},
 "codegen": {"enabled": true, "allowed_languages": ["python"]},
 "scoring_calibration": {"enabled": %s, "auto_save": false, "path": "%s"}%s}\n' \
        "${4:-}" "${2:-true}" "${3:-cal.json}" "${1:-}"
}

# Ten runtime advisories: codegen.run of a placeholder comment.
VIOLATE='    for i in 0..10 {
        codegen.run("python", "# TODO: finish\nx = 1")
    }
    print("DONE")'

# $1 = config, $2 = program body (inside main), [$3 = file to pre-write, $4 = its content]
# Sets RC and OUT.
run() {
    rm -rf "$W"; mkdir -p "$W/logs"
    printf '%s\n' "$1" > "$W/govern.json"
    printf 'use codegen\nuse governance\nuse file\nmain {\n%s\n}\n' "$2" > "$W/t.naab"
    [ -n "${3:-}" ] && printf '%s\n' "$4" > "$W/$3"
    OUT=$(cd "$W" && timeout 120s "$NAAB" run t.naab 2>&1); RC=$?
}
score_of() { grep -oE 'risk score [0-9]+' <<<"$OUT" | head -1 | grep -oE '[0-9]+$'; }

# ---- CR-00 control --------------------------------------------------------------
run "$(cfg)" "$VIOLATE"
c_score=$(score_of)
if [ "$RC" = 2 ] && [ "$c_score" = 30 ] && grep -q '^DONE' <<<"$OUT"; then
    pass "CR-00" "ten advisories reach the risk gate (exit 2, score 30)"
else
    fail "CR-00" "the control did not reach the gate -- nothing below can be read" "rc=$RC score=${c_score:-none}"
    echo "Result: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"; exit 1
fi

# ---- CR-01 lowering refused -----------------------------------------------------
run "$(cfg)" "    let r = governance.calibrate(\"$RULE\", 0, \"lower it\")
    print(\"CAL=\" + string(r.get(\"success\")) + \" REFUSED=\" + string(r.get(\"refused\") != null))
$VIOLATE"
if [ "$RC" = 2 ] && [ "$(score_of)" = 30 ] && grep -q 'CAL=false REFUSED=true' <<<"$OUT"; then
    pass "CR-01" "calibrate(rule, 0) is refused and the gate still fires at score 30"
else
    fail "CR-01" "the program lowered its own weight" "rc=$RC score=$(score_of) $(grep -o 'CAL=[a-z]* REFUSED=[a-z]*' <<<"$OUT")"
fi

# ---- CR-02 raise accepted -------------------------------------------------------
run "$(cfg)" "    let r = governance.calibrate(\"$RULE\", 5, \"raise it\")
    print(\"CAL=\" + string(r.get(\"success\")))
$VIOLATE"
if [ "$RC" = 2 ] && [ "$(score_of)" = 50 ] && grep -q 'CAL=true' <<<"$OUT"; then
    pass "CR-02" "calibrate(rule, 5) is a raise and is accepted (score 50)"
else
    fail "CR-02" "a raise was not accepted" "rc=$RC score=$(score_of) $(grep -o 'CAL=[a-z]*' <<<"$OUT")"
fi

# ---- CR-03 operator file still loads --------------------------------------------
run "$(cfg)" "$VIOLATE" "cal.json" "$OPERATOR_ZERO"
if [ "$RC" = 0 ] && grep -q '^DONE' <<<"$OUT"; then
    pass "CR-03" "an operator-written weight of 0 is loaded: the run passes the gate"
else
    fail "CR-03" "the operator's calibration file was not applied" "rc=$RC score=$(score_of)"
fi

# ---- CR-04 the floor is the current weight --------------------------------------
run "$(cfg)" "    let r = governance.calibrate(\"$RULE\", 1, \"raise over operator zero\")
    print(\"CAL=\" + string(r.get(\"success\")))
$VIOLATE" "cal.json" "$OPERATOR_ZERO"
if [ "$RC" = 2 ] && [ "$(score_of)" = 10 ] && grep -q 'CAL=true' <<<"$OUT"; then
    pass "CR-04" "over an operator override of 0, calibrate(rule, 1) is a raise (score 10)"
else
    fail "CR-04" "the floor was not the current override" "rc=$RC score=$(score_of) $(grep -o 'CAL=[a-z]*' <<<"$OUT")"
fi

# ---- CR-05 a contextual weight sets the floor -----------------------------------
CTX=", \"rule_weights\": {\"$RULE:code_quality\": 10}"
run "$(cfg "" true cal.json "$CTX")" "    let a = governance.calibrate(\"$RULE\", 5, \"under the contextual weight\")
    let b = governance.calibrate(\"$RULE\", 10, \"at it\")
    print(\"LOW=\" + string(a.get(\"success\")) + \" EQ=\" + string(b.get(\"success\")))"
if grep -q 'LOW=false EQ=true' <<<"$OUT"; then
    pass "CR-05" "with rule:category = 10, a weight of 5 is refused and 10 accepted"
else
    fail "CR-05" "the contextual weight did not set the floor" "rc=$RC $(grep -o 'LOW=[a-z]* EQ=[a-z]*' <<<"$OUT")"
fi

# ---- CR-06 program write of the file refused ------------------------------------
run "$(cfg)" "    file.write(\"cal.json\", \"{\\\"version\\\": 1, \\\"overrides\\\": {\\\"$RULE\\\": {\\\"weight\\\": 0}}}\")
    print(\"WROTE\")
$VIOLATE" "cal.json" '{"version": 1, "overrides": {}}'
if [ "$RC" = 3 ] && ! grep -q '^WROTE' <<<"$OUT" && grep -q 'governance.state_files\|governance state file' <<<"$OUT" \
   && grep -q '"overrides": {}' "$W/cal.json"; then
    pass "CR-06" "file.write of the calibration file is refused (exit 3) and the operator's file is unchanged"
else
    fail "CR-06" "the program wrote the calibration file" "rc=$RC $(head -c 120 "$W/cal.json" 2>/dev/null)"
fi

# ---- CR-07 reads allowed --------------------------------------------------------
run "$(cfg)" "    print(\"READ=\" + file.read(\"cal.json\"))" "cal.json" '{"version": 1, "overrides": {}, "marker": "OPERATOR_FILE_7731"}'
if [ "$RC" = 0 ] && grep -q 'READ=.*OPERATOR_FILE_7731' <<<"$OUT"; then
    pass "CR-07" "file.read of the calibration file is allowed"
else
    fail "CR-07" "reading the calibration file was refused" "rc=$RC"
fi

# ---- CR-08 calibration off: an ordinary file ------------------------------------
run "$(cfg "" false)" "    file.write(\"cal.json\", \"plain\")
    print(\"WROTE\")"
if [ "$RC" = 0 ] && grep -q '^WROTE' <<<"$OUT" && grep -q '^plain' "$W/cal.json"; then
    pass "CR-08" "with calibration off the same path is an ordinary file"
else
    fail "CR-08" "a file the engine does not read was protected" "rc=$RC"
fi

# ---- CR-09 the protected file is the one the engine loads -----------------------
TEL=', "telemetry": {"enabled": true, "output_file": "logs/tel.jsonl"}'
run "$(cfg "$TEL")" "    file.write(\"cal.json\", \"not the engine file\")
    print(\"ROOT_OK\")
    file.write(\"logs/cal.json\", \"engine file\")
    print(\"LOGS_WROTE\")"
if [ "$RC" = 3 ] && grep -q '^ROOT_OK' <<<"$OUT" && ! grep -q '^LOGS_WROTE' <<<"$OUT" && [ ! -f "$W/logs/cal.json" ]; then
    pass "CR-09" "beside a telemetry file, logs/cal.json is refused and ./cal.json is an ordinary file"
else
    fail "CR-09" "protection and loading disagree on the file" "rc=$RC root=$(grep -c '^ROOT_OK' <<<"$OUT") logs=$(grep -c '^LOGS_WROTE' <<<"$OUT")"
fi

# ---- CR-10 embedded Python reaches the same decision point ----------------------
rm -rf "$W"; mkdir -p "$W"
printf 'main { let v = <<python\n1+1\n>>\nprint(string(v)) }\n' > "$W/probe.naab"
if [ "$(cd "$W" && "$NAAB" --no-governance probe.naab 2>/dev/null | tail -1)" != "2" ]; then
    skip "CR-10" "Python is not embedded in this build (a separate process does not reach path policy, CONTRA-013)"
else
    run "$(cfg)" "    let r = <<python
try:
    with open(\"cal.json\", \"w\") as f:
        f.write(\"x\")
    out = \"PY_WROTE\"
except PermissionError:
    out = \"PY_REFUSED\"
out
>>
    print(string(r))" "cal.json" '{"version": 1, "overrides": {}}'
    if grep -q 'PY_REFUSED' <<<"$OUT" && grep -q '"overrides": {}' "$W/cal.json"; then
        pass "CR-10" "embedded Python open(file, \"w\") is refused"
    else
        fail "CR-10" "embedded Python wrote the calibration file" "rc=$RC $(grep -o 'PY_[A-Z]*' <<<"$OUT" | head -1)"
    fi
fi

echo "Result: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
