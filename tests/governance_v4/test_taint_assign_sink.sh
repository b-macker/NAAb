#!/usr/bin/env bash
# ============================================================
# test_taint_assign_sink.sh -- the "variable.assign" taint sink on the VM
# judges the value being assigned, not a name in the engine's shared taint
# set (A65 remainder, Bundle 3)
#
# The VM marks the right-hand side tainted on its shadow stack
# (OP_GOV_TAINT_MARK), then OP_GOV_TAINT_CHECK_ASSIGN asked checkTaintedSink()
# whether the TARGET NAME was in the engine's shared taint set. The VM writes a
# name into that set only for a global, and only after the check, so the
# lookup found nothing for a local the first time it was assigned (traced from
# vm.cpp's OP_SET_GLOBAL / OP_DEFINE_GLOBAL, the only markTainted() sites for
# a name); and for a name another thread assigned clean, another thread's clear
# could land between mark and lookup (the shape A65 fixed for call arguments).
# The check now passes the taint the VM already holds to
# checkTaintedValueAtSink(), which shares checkTaintedSink()'s decision code.
#
#   TA-00  CONTROL: the same source reaching a file.write sink is refused --
#          the taint config is live on this program
#   TA-01  a local in main assigned from a taint source is refused at the
#          assignment when variable.assign is a sink
#   TA-02  a local inside a function, likewise
#   TA-03  CONTROL: variable.assign not among the sinks -- the assignment runs
#   TA-04  CONTROL: a sanitized right-hand side -- the assignment runs
#   TA-05  CONTROL: an untainted right-hand side -- the assignment runs
#   TA-06  INSTRUMENT: TA-00's refusal, run from a directory whose name holds
#          "RAN", still reads as refused. The engine prints the program's path,
#          so a substring test for the program's marker read a refusal as a run
#          whenever mktemp's random suffix spelled RAN (A205).
#
# The program's marker is matched as a whole line (print("RAN") prints it on
# its own line), never as a substring of the output.
#
# Kill table (each mutant rebuilt, this suite run):
#   the check back to checkTaintedSink(var_name, ...)   TA-01, TA-02
#   refused()/ran() back to a substring test for RAN     TA-06 (harness mutant,
#                                                       no rebuild)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }

echo "=== taint: the variable.assign sink on the VM ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-taintassign-XXXXXX")"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
export NAAB_TA_PROBE="probe-value"

# run NAME SINKS_JSON PROGRAM -> OUT RC
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }, "taint_tracking": { "enabled": true, "level": "hard", "sources": ["env.get"], "sinks": %s, "sanitizers": ["sanitize_"] } }\n' "$2" > "$d/govern.json"
    printf '%s\n' "$3" > "$d/p.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" run p.naab 2>&1)"; RC=$?
}
# The marker is a whole line: the output also holds the program's path, and a
# path can contain the letters RAN.
marker()  { grep -qx 'RAN' <<<"$OUT"; }
refused() { [ "$RC" = 3 ] && ! marker && [[ "$OUT" == *"Taint tracking violation"* ]]; }
ran()     { [ "$RC" = 0 ] && marker; }

run ta0 '["file.write"]' 'use env
use file
main {
    let v = env.get("NAAB_TA_PROBE")
    file.write("out.txt", v)
    print("RAN")
}'
if refused; then ok "TA-00" "control: the source reaching file.write is refused"
else bad "TA-00" "the taint config does not reach this program -- every arm below is unmeasured" "rc=$RC $OUT"; fi

run ta1 '["variable.assign"]' 'use env
main {
    let v = env.get("NAAB_TA_PROBE")
    print("RAN")
}'
if refused; then ok "TA-01" "a local in main assigned from a taint source is refused"
else bad "TA-01" "a tainted assignment to a local passed the variable.assign sink" "rc=$RC $OUT"; fi

run ta2 '["variable.assign"]' 'use env
fn grab() {
    let inner = env.get("NAAB_TA_PROBE")
    return 1
}
main {
    let r = grab()
    print("RAN")
}'
if refused; then ok "TA-02" "a local inside a function, likewise"
else bad "TA-02" "a tainted assignment inside a function passed the sink" "rc=$RC $OUT"; fi

run ta3 '["file.write"]' 'use env
main {
    let v = env.get("NAAB_TA_PROBE")
    print("RAN")
}'
if ran; then ok "TA-03" "control: variable.assign not a sink -- the assignment runs"
else bad "TA-03" "the assignment was refused with no assignment sink" "rc=$RC $OUT"; fi

run ta4 '["variable.assign"]' 'use env
fn sanitize_value(s) { return "clean" }
main {
    let v = sanitize_value(env.get("NAAB_TA_PROBE"))
    print("RAN")
}'
if ran; then ok "TA-04" "control: a sanitized right-hand side runs"
else bad "TA-04" "a sanitized assignment was refused" "rc=$RC $OUT"; fi

run ta5 '["variable.assign"]' 'main {
    let v = "plain"
    print("RAN")
}'
if ran; then ok "TA-05" "control: an untainted right-hand side runs"
else bad "TA-05" "an untainted assignment was refused" "rc=$RC $OUT"; fi

# TA-06: the same refusal from a directory whose name holds the marker's letters.
run ta6_RAN_dir '["file.write"]' 'use env
use file
main {
    let v = env.get("NAAB_TA_PROBE")
    file.write("out.txt", v)
    print("RAN")
}'
if [[ "$OUT" == *ta6_RAN_dir* ]] && refused; then
    ok "TA-06" "instrument: a refusal printed beside a path holding RAN still reads as refused"
elif [[ "$OUT" != *ta6_RAN_dir* ]]; then
    bad "TA-06" "the output does not name the program's directory, so this arm measures nothing" "rc=$RC $OUT"
else
    bad "TA-06" "a path holding RAN made a refusal read as a run" "rc=$RC $OUT"
fi

echo ""
echo "  Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
