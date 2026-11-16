#!/usr/bin/env bash
# ============================================================
# test_check_disposition.sh -- every check record says what happened to the
# action (A172)
#
# THE DEFECT (observed on 58796deb, docs/open-investigations.md A172)
#
# Telemetry recorded every failed check as "result": "block" -- an advisory
# that let the run go on, a soft refusal the operator overrode, a refusal in
# audit mode that only logged -- and no field said whether the action ran.
# The JSON report and SARIF said the same through the level alone.
#
# THE FIX (the owner's decision, 2026-11-16): a field "disposition" on every
# record -- passed / refused / warned / overridden / approved / audit_only --
# in telemetry, the JSON report (with summary counts) and SARIF properties.
# "result" stays as it was, for existing readers.
#
# ARMS (each read from telemetry AND the JSON report unless named)
#   DS-00  control: the record is read at all -- a failed check's telemetry
#          row has "result": "block" (true before the fix too)
#   DS-01  advisory              -> warned      (exit 0)
#   DS-02  hard refusal          -> refused     (exit 3)
#   DS-03  soft, overridden      -> overridden  (exit 0, the action ran)
#   DS-04  audit mode            -> audit_only  (exit 0, the action ran)
#   DS-05  detect, caught        -> refused     (the program went on)
#   DS-06  escalated advisory    -> warned, then refused (exit 3)
#   DS-07  a passing check       -> passed
#   DS-08  the JSON summary counts the dispositions
#   DS-09  SARIF carries the disposition in its result properties
#   DS-10  compatibility: "result" is still "block" for a warned record
#   Not measured: "approved" (needs a signed approval token; no arm).
#
# KILL TABLE (mutants, each reverted alone and rebuilt; observed 2026-11-16)
#   M1 telemetry writes no disposition            DS-01..DS-07 (DS-10 stays green)
#   M2 the override branch leaves "refused"       DS-03, DS-08, DS-09
#   M3 the audit branch leaves "refused"          DS-04
#   M4 escalation leaves "warned"                 DS-06
#   M5 JSON report entry has no disposition       DS-01..DS-07
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/naab-check-disposition-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2 (UNMEASURABLE)"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "${TEST_TMP:?}"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB (UNMEASURABLE)"; exit 1; }

echo "=== test_check_disposition.sh (A172) ==="

# arm DIR MODE CONFIG_BODY PROGRAM [args...]
arm() {
    local d="$1" mode="$2" cfg="$3" prog="$4"; shift 4
    mkdir -p "$d"
    cat > "$d/govern.json" <<EOF
{"version":"4.0","mode":"$mode","security":{"sandbox_level":"elevated"},
 "telemetry":{"enabled":true,"output_file":"tele.jsonl"},
 $cfg}
EOF
    printf '%s' "$prog" > "$d/p.naab"
    (cd "$d" && rm -f tele.jsonl r.json r.sarif &&
     VAR_ONE=1 timeout 60 "$NAAB" run p.naab "$@" \
        --governance-report r.json --governance-sarif r.sarif > out.txt 2> err.txt)
    RC=$?
}

# disp DIR RULE SOURCE [FIELD] -- the field (default disposition) of each row of
# RULE (passing and failing) from telemetry (tele) or the JSON report (json).
disp() {
    python3 - "$1" "$2" "$3" "${4:-disposition}" <<'EOF'
import json, sys, os
d, rule, src, field = sys.argv[1:]
out = []
try:
    if src == "json":
        for r in json.load(open(os.path.join(d, "r.json")))["results"]:
            if r["rule"] == rule:
                out.append(str(r.get(field, "-")))
    else:
        for l in open(os.path.join(d, "tele.jsonl"), encoding="utf-8"):
            e = json.loads(l)
            if e.get("rule_name") == rule and e.get("event_type") in ("RuleViolation", "GovernanceCheck"):
                out.append(str(e.get(field, "-")))
except (OSError, ValueError, KeyError) as ex:
    print("UNREADABLE:" + type(ex).__name__); sys.exit()
print(" ".join(out))
EOF
}

# expect ID DESC DIR RULE WANT [WANT_RC]
expect() {
    local id="$1" desc="$2" d="$3" rule="$4" want="$5" want_rc="${6:-}"
    if [ -n "$want_rc" ] && [ "$RC" != "$want_rc" ]; then
        fail "$id" "$desc: exit $RC, expected $want_rc (the fixture did not do what the arm needs)" \
            "$(head -2 "$d/out.txt" | tr '\n' ' ')"
        return
    fi
    local t j; t="$(disp "$d" "$rule" tele)"; j="$(disp "$d" "$rule" json)"
    case "$t$j" in *UNREADABLE*) skip "$id" "$desc: record unreadable (tele='$t' json='$j', rc=$RC)"; return ;; esac
    if [ "$t" = "$want" ] && [ "$j" = "$want" ]; then
        pass "$id" "$desc ($want)"
    else
        fail "$id" "$desc: telemetry '$t', JSON '$j', expected '$want'"
    fi
}

# ---------- DS-01 advisory / DS-00 / DS-10 ----------
W="$TEST_TMP/ds01"
arm "$W" enforce '"code_quality":{"no_placeholders":{"enabled":true,"level":"advisory"}}' \
$'fn f() {\n    // TODO: later\n    return 1\n}\nmain {\n    print(f())\n}\n'
R="$(disp "$W" code_quality.no_placeholders tele result)"
case "$R" in
    *block*) pass "DS-00" "control: the failed check's telemetry row is read (result '$R')" ;;
    *)       fail "DS-00" "no failed no_placeholders row read (result '$R', rc=$RC) -- every other arm is unmeasured" ;;
esac
expect "DS-01" "advisory -> warned" "$W" code_quality.no_placeholders "warned passed" 0
R="$(disp "$W" code_quality.no_placeholders tele result | cut -d' ' -f1)"
if [ "$R" = "block" ]; then
    pass "DS-10" "compatibility: \"result\" is still \"block\" for a warned record"
else
    fail "DS-10" "\"result\" for a warned record is '$R', expected 'block' (existing readers)"
fi

# ---------- DS-02 hard refusal ----------
W="$TEST_TMP/ds02"
arm "$W" enforce '"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}' \
$'use file\nmain {\n    let k = file.read("vault/x.txt")\n}\n'
expect "DS-02" "hard refusal -> refused" "$W" capabilities.filesystem.path "refused" 3

# ---------- DS-03 soft, overridden ----------
W="$TEST_TMP/ds03"
arm "$W" enforce '"capabilities":{"env_vars":{"allowed_read":["HOME"]}}' \
$'use env\nmain {\n    let x = env.get("VAR_ONE")\n    print("RAN:" + x)\n}\n' --governance-override
if grep -q 'RAN:1' "$W/out.txt"; then
    expect "DS-03" "soft, overridden -> overridden (and the read ran)" "$W" capabilities.env_vars.allowed_read "overridden" 0
else
    fail "DS-03" "the overridden read did not run (rc=$RC) -- fixture" "$(head -2 "$W/out.txt" | tr '\n' ' ')"
fi

# ---------- DS-04 audit ----------
W="$TEST_TMP/ds04"
arm "$W" audit '"capabilities":{"env_vars":{"allowed_read":["HOME"]}}' \
$'use env\nmain {\n    let x = env.get("VAR_ONE")\n    print("RAN:" + x)\n}\n'
if grep -q 'RAN:1' "$W/out.txt"; then
    expect "DS-04" "audit mode -> audit_only (and the read ran)" "$W" capabilities.env_vars.allowed_read "audit_only" 0
else
    fail "DS-04" "the audited read did not run (rc=$RC) -- fixture" "$(head -2 "$W/out.txt" | tr '\n' ' ')"
fi

# ---------- DS-05 detect, caught ----------
W="$TEST_TMP/ds05"
arm "$W" enforce '"taint_tracking":{"enabled":true,"level":"detect","sources":["env.get"],"sinks":["file.write"]},"capabilities":{"filesystem":{"mode":"write"}}' \
$'use env\nuse file\nmain {\n    try {\n        file.write("o.txt", env.get("HOME"))\n    } catch (e) {\n        print("CAUGHT")\n    }\n    print("WENT_ON")\n}\n'
if grep -q 'WENT_ON' "$W/out.txt" && grep -q 'CAUGHT' "$W/out.txt" && [ ! -e "$W/o.txt" ]; then
    expect "DS-05" "detect, caught -> refused (the write did not happen; the program went on)" "$W" taint_tracking.sink_violation "refused" 0
else
    fail "DS-05" "the detect-level refusal was not caught, or the write happened (rc=$RC) -- fixture" "$(head -2 "$W/out.txt" | tr '\n' ' ')"
fi

# ---------- DS-06 escalated advisory ----------
W="$TEST_TMP/ds06"
arm "$W" enforce '"code_quality":{"no_placeholders":{"enabled":true,"level":"advisory"}},"advisory_escalation":{"enabled":true,"soft_after":2}' \
$'fn f() {\n    // TODO: one\n    return 1\n}\nfn g() {\n    // TODO: two\n    return 2\n}\nmain {\n    print(f() + g())\n}\n'
expect "DS-06" "escalated advisory -> warned, then refused" "$W" code_quality.no_placeholders "warned refused" 3

# ---------- DS-07 a passing check ----------
W="$TEST_TMP/ds07"
arm "$W" enforce '"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}' \
$'use file\nmain {\n    let k = file.exists("ok.txt")\n    print("DONE")\n}\n'
T="$(disp "$W" capabilities.filesystem tele)"; J="$(disp "$W" capabilities.filesystem json)"
case "$T|$J" in
    passed*"|"passed*) pass "DS-07" "a passing check -> passed" ;;
    *) fail "DS-07" "a passing check: telemetry '$T', JSON '$J' (rc=$RC)" ;;
esac

# ---------- DS-08 summary counts, DS-09 SARIF ----------
W="$TEST_TMP/ds03"
S="$(python3 - "$W" <<'EOF'
import json, sys, os
try:
    print(json.dumps(json.load(open(os.path.join(sys.argv[1], "r.json")))["summary"].get("dispositions"), sort_keys=True))
except (OSError, ValueError, KeyError) as ex:
    print("UNREADABLE")
EOF
)"
case "$S" in
    *'"overridden": 1'*) pass "DS-08" "the JSON summary counts dispositions ($S)" ;;
    *) fail "DS-08" "JSON summary dispositions: '$S'" ;;
esac
P="$(python3 - "$W" <<'EOF'
import json, sys, os
try:
    for run in json.load(open(os.path.join(sys.argv[1], "r.sarif")))["runs"]:
        for r in run["results"]:
            if r["ruleId"] == "capabilities.env_vars.allowed_read":
                print(r.get("properties", {}).get("disposition", "-"))
except (OSError, ValueError, KeyError):
    print("UNREADABLE")
EOF
)"
if [ "$P" = "overridden" ]; then
    pass "DS-09" "SARIF result properties carry the disposition"
else
    fail "DS-09" "SARIF disposition '$P', expected 'overridden'"
fi

echo
echo "=== $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable ==="
[ "$FAIL_COUNT" -eq 0 ] && [ "$SKIP_COUNT" -eq 0 ]
