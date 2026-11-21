#!/usr/bin/env bash
# The check-result cap (V-GOV-024, MAX_CHECK_RESULTS = 10,000) and what a run
# past it used to lose: speed (A177), telemetry rows (A107) and its own counts
# (A193). Bundle 19 PR 2.
#
#   CR-00  one place appends a check result (pushCheckResultLocked), so every
#          result gets the record number telemetry resumes by (structural;
#          control: the pattern finds that one site)
#   CR-01  20,000 governed calls finish well inside the run limit, and take
#          less than 10x the time of 5,000 (linear is 4x; master: 20,000 hit
#          the 30 s limit at 0.6 s for 5,000)
#   CR-02  an advisory recorded before 12,000 passes still fails
#          `advisory_violations > 0` (exit 2), and the summary still counts
#          it (master: exit 0, "Governance: PASS", no summary line); controls: the same advisory below the cap fails the gate, and
#          past the cap without the advisory the gate passes (exit 0)
#   CR-03  past the cap, every result recorded is a telemetry row or counted
#          in results_not_written, and the chain verifies; control: the
#          probe counts rows at all
#
# Kill table (mutants of the engine, run by hand from a copy of the tree):
#   K1 compact at every record again              -> CR-01
#   K2 the quality gate ignores the evicted tally  -> CR-02
#   K3 the summary ignores the evicted tally       -> CR-02 (summary)
#   K3b the PASS line ignores the evicted tally    -> CR-02 (PASS line)
#   K4 results_not_written not written             -> CR-03
#   K5 a second push_back site added               -> CR-00
# Not covered here: the preflight-skip branch of the compaction (F8) -- no
# public route makes a preflight result in a unit or suite run.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/naab_crcap.XXXXXX")"
trap 'rm -rf "$TEST_TMP"; teardown_isolated_trust' EXIT

PASS=0; FAIL=0; UNM=0
pass() { echo "  PASS [$1] $2"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "        $3"; FAIL=$((FAIL + 1)); }
unm()  { echo "  UNMEASURABLE [$1] $2"; UNM=$((UNM + 1)); }

echo "=== check-result cap: speed, telemetry, counts (A177, A107, A193) ==="

# --- CR-00 ---------------------------------------------------------------
sites="$(grep -rn 'check_results_\.\(push_back\|emplace_back\|insert\)' "$REPO/src" "$REPO/include" || true)"
n_sites=$(grep -c . <<<"$sites")
if [ "$n_sites" -eq 1 ] && grep -q 'include/naab/governance.h' <<<"$sites"; then
    pass CR-00 "one site appends a check result, the helper in governance.h"
elif [ "$n_sites" -eq 0 ]; then
    fail CR-00 "the pattern found no append site at all, so it measures nothing"
else
    fail CR-00 "a check result is appended outside pushCheckResultLocked(): it gets no record number" "$sites"
fi

mkcfg() {  # DIR KIND (plain | gate | telemetry): written by a JSON writer
    # The config goes out on stdout and the shell writes the file: a native
    # Python (build-windows) cannot open an MSYS path handed to it.
    mkdir -p "$1"
    python3 -I - "$2" > "$1/govern.json" <<'PYCFG'
import json, sys
c = {"version": "4.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
     "capabilities": {"filesystem": {"mode": "read", "blocked_paths": ["vault/"]}}}
if sys.argv[1] == "gate":
    # shell off: CONTRA-013, the advisory every shell-enabled config records,
    # would otherwise be the one advisory the no-advisory control has.
    c["capabilities"]["shell"] = {"enabled": False}
    c["capabilities"]["functions"] = {"main": {"allowed_actions": ["FS_READ"]},
                                      "noisy": {"allowed_actions": []}}
    c["quality_gate"] = {"enabled": True, "conditions": [
        {"metric": "advisory_violations", "operator": ">", "threshold": 0}]}
if sys.argv[1] == "telemetry":
    c["telemetry"] = {"enabled": True, "output_file": "tele.jsonl",
                      "tamper_evidence": {"enabled": True, "algorithm": "sha256"}}
sys.stdout.write(json.dumps(c, indent=1) + "\n")
PYCFG
}
loop_prog() {  # FILE N WITH_ADVISORY(0|1)
    {
        echo 'fn noisy() {'
        echo '    return file.exists("a.txt")'
        echo '}'
        echo 'main {'
        [ "$3" = 1 ] && echo '    let x = noisy()'
        echo '    let i = 0'
        echo "    while i < $2 {"
        echo '        file.exists("b.txt")'
        echo '        i = i + 1'
        echo '    }'
        echo '    print("LOOP_DONE")'
        echo '}'
    } > "$1"
}

# --- CR-01 ---------------------------------------------------------------
D="$TEST_TMP/cr01"; mkcfg "$D" plain
loop_prog "$D/p5.naab" 5000 0; loop_prog "$D/p20.naab" 20000 0
t0=$(date +%s%N); O5=$(cd "$D" && timeout 120 "$NAAB" p5.naab 2>&1); R5=$?; t1=$(date +%s%N)
O20=$(cd "$D" && timeout 120 "$NAAB" p20.naab 2>&1); R20=$?; t2=$(date +%s%N)
ms5=$(( (t1 - t0) / 1000000 )); ms20=$(( (t2 - t1) / 1000000 ))
if [ "$R5" -ne 0 ] || ! grep -q LOOP_DONE <<<"$O5"; then
    unm CR-01 "the 5,000-call control did not run here (rc=$R5): $(tail -2 <<<"$O5" | tr '\n' ' ' | cut -c1-160)"
elif [ "$R20" -ne 0 ] || ! grep -q LOOP_DONE <<<"$O20"; then
    fail CR-01 "20,000 governed calls did not finish (rc=$R20, ${ms20} ms)" "$(tail -2 <<<"$O20" | tr '\n' ' ' | cut -c1-200)"
elif [ "$ms20" -gt $(( 10 * (ms5 > 100 ? ms5 : 100) )) ]; then
    fail CR-01 "20,000 calls took ${ms20} ms against ${ms5} ms for 5,000: more than 10x"
else
    pass CR-01 "20,000 governed calls ${ms20} ms, 5,000 ${ms5} ms"
fi

# --- CR-02 ---------------------------------------------------------------
D="$TEST_TMP/cr02"; mkcfg "$D" gate
loop_prog "$D/small.naab" 10 1; loop_prog "$D/big.naab" 6000 1; loop_prog "$D/clean.naab" 6000 0
OS=$(cd "$D" && timeout 120 "$NAAB" small.naab 2>&1); RS=$?
OB=$(cd "$D" && timeout 120 "$NAAB" big.naab 2>&1); RB=$?
OC=$(cd "$D" && timeout 120 "$NAAB" clean.naab 2>&1); RC=$?
if [ "$RS" -ne 2 ] || ! grep -q "Quality gate FAILED: advisory_violations" <<<"$OS"; then
    unm CR-02 "control: below the cap the advisory does not fail the gate here (rc=$RS), so the arm cannot judge the cap: $(grep -m2 -iE 'error|gate' <<<"$OS" | tr '\n' ' ' | cut -c1-200)"
elif [ "$RC" -ne 0 ] || ! grep -q LOOP_DONE <<<"$OC"; then
    fail CR-02 "control: past the cap with no advisory the gate should pass (rc=$RC)" "$(grep -m2 -iE 'gate|error' <<<"$OC" | tr '\n' ' ' | cut -c1-200)"
elif [ "$RB" -ne 2 ] || ! grep -q "Quality gate FAILED: advisory_violations > 0 (actual: 1)" <<<"$OB"; then
    fail CR-02 "past the cap the evicted advisory no longer fails the gate (rc=$RB)" "$(grep -E 'Quality gate|Governance:' <<<"$OB" | tr '\n' ' ' | cut -c1-200)"
elif ! grep -qE "Summary \(mode: enforce\): 1200[0-9] passed, 1 warned, 0 refused" <<<"$OB"; then
    fail CR-02 "the gate fails, but the summary does not count the evicted results" "$(grep -E 'Summary|Governance:' <<<"$OB" | tr '\n' ' ' | cut -c1-200)"
elif ! grep -q "Governance: PASS (1 warning; after-run audit" <<<"$OB"; then
    fail CR-02 "the closing PASS line does not count the evicted warning" "$(grep -E 'Governance:' <<<"$OB" | cut -c1-200)"
else
    pass CR-02 "past the cap an advisory recorded first still fails the gate (exit 2) and is counted (1 warned beside 12,000+ passes); controls: below the cap exit 2, no advisory exit 0"
fi

# --- CR-03 ---------------------------------------------------------------
D="$TEST_TMP/cr03"; mkcfg "$D" telemetry
# The control run: the same program with 10 iterations records 20 loop
# results plus whatever the run records outside the loop (K); a run that
# writes everything has no results_not_written, so its rows are 20 + K.
DC="$TEST_TMP/cr03c"; mkcfg "$DC" telemetry
loop_prog "$DC/p.naab" 10 0
(cd "$DC" && timeout 60 "$NAAB" p.naab >/dev/null 2>&1)
loop_prog "$D/p.naab" 6000 0
OT=$(cd "$D" && timeout 120 "$NAAB" p.naab 2>&1); RT=$?
if [ "$RT" -ne 0 ] || [ ! -s "$D/tele.jsonl" ]; then
    unm CR-03 "the run wrote no telemetry here (rc=$RT): $(tail -2 <<<"$OT" | tr '\n' ' ' | cut -c1-160)"
else
    count_rows() {  # FILE, read by the shell and handed over as bytes
        python3 -I -c '
import json, sys
rows = notw = 0
for line in sys.stdin.buffer.read().decode("utf-8", "replace").splitlines():
    try: e = json.loads(line)
    except Exception: continue
    t = e.get("event_type")
    if t in ("GovernanceCheck", "RuleViolation"): rows += 1
    if t == "GovernanceCheckSummary": notw += int(e.get("results_not_written", 0))
print(rows, notw, rows + notw)
' < "$1" | tr -d '\r'
    }
    read -r ROWS NOTW TOTAL < <(count_rows "$D/tele.jsonl")
    if [ -s "$DC/tele.jsonl" ]; then read -r CROWS CNOTW _ < <(count_rows "$DC/tele.jsonl"); else CROWS=0; CNOTW=0; fi
    EXPECT=$(( 12000 + CROWS - 20 ))
    VOUT=$(cd "$D" && "$NAAB" --verify-telemetry-chain tele.jsonl 2>&1); VRC=$?
    if [ "$CROWS" -lt 20 ] || [ "$CNOTW" -ne 0 ]; then
        unm CR-03 "control: the 10-call run did not write its 20 loop rows (rows=$CROWS, not_written=$CNOTW)"
    elif [ "${ROWS:-0}" -eq 0 ]; then
        fail CR-03 "the probe counted no check rows, so it measures nothing" "$(head -c 300 "$D/tele.jsonl")"
    elif [ "$NOTW" -eq 0 ]; then
        fail CR-03 "12,000 results, at most 11,000 in memory, and no result reported as not written (rows=$ROWS)"
    elif [ "$TOTAL" -ne "$EXPECT" ]; then
        fail CR-03 "rows + results_not_written = $TOTAL, not the $EXPECT results the run records" "rows=$ROWS not_written=$NOTW control_rows=$CROWS"
    elif [ "$VRC" -ne 0 ]; then
        fail CR-03 "the telemetry chain does not verify" "$(tail -3 <<<"$VOUT" | tr '\n' ' ' | cut -c1-200)"
    else
        pass CR-03 "$EXPECT results: $ROWS telemetry rows + $NOTW reported not written; the chain verifies"
    fi
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
