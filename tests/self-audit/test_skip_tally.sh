#!/usr/bin/env bash
# ============================================================
# test_skip_tally.sh -- the skip tally counts what the suites said
#
# tests/helpers/skip_tally.sh lists, per suite, every arm that reported
# SKIP/UNMEASURABLE/XFAIL, so a platform's untested surface is visible under a
# green verdict. A tally that silently counted nothing would read as "nothing
# is off", so each arm here plants known output and checks the count.
#
#   ST-01  colour-coded, plain, CRLF-terminated and each marker word are
#          counted; lowercase prose, SKIPPING, NOSKIP and zero counters
#          ("SKIP: 0", "XFAIL=0") are not
#   ST-02  a suite with no skips is not listed but is counted as a suite
#   ST-03  the TSV holds one sorted suite<TAB>line row per skip, with no CR
#   ST-04  agrees with tools/testrunner/parallel.py's count_skip_markers()
#          (the dead gate's rule) on which lines carry a marker (UNMEASURABLE without python3)
#   ST-05  a missing capture dir is reported UNMEASURABLE, not "0 skips"
#   ST-06  an empty capture dir is reported UNMEASURABLE, not "0 skips"
#   ST-07  it never fails: an unwritable TSV path still returns 0 under set -e
#   ST-08  run-all-tests.sh calls it, before the summary that can exit
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/tests/helpers/skip_tally.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/caps"

echo "=== the skip tally counts what the suites said ==="

# Five lines carry a marker; six look similar and must not count.
printf '%b' \
  '  \033[1;33mSKIP\033[0m [X-01] coloured marker\n' \
  '  SKIP [X-02] plain marker\n' \
  'SKIP [X-03] crlf terminated\r\n' \
  '  PASS [X-04] ran, then: UNMEASURABLE on this platform\n' \
  'XFAIL: known divergence\n' \
  'Results: 3 passed, 0 failed, 2 skipped\n' \
  'SKIPPING ahead to the next group\n' \
  'NOSKIP mode\n' \
  '  PASS [X-05] skip logic works\n' \
  'PASS: 4  FAIL: 0  SKIP: 0  TOTAL: 4\n' \
  'Results: 2 passed, XFAIL=0\n' > "$W/caps/alpha.log"
printf '  PASS [Y-01] clean\nResults: 1 passed, 0 failed\n' > "$W/caps/bravo.log"

out="$(skip_tally "$W/caps" "$W/out/skips.tsv")"

# --- ST-01 ---
case "$out" in
  *"5 skipped/unmeasurable arm(s) in 1 of 2 suite(s)"*) ok ST-01 "5 marker lines counted, 6 look-alikes ignored" ;;
  *) bad ST-01 "wrong count" "$out" ;;
esac

# --- ST-02 ---
case "$out" in
  *"bravo"*) bad ST-02 "a suite with no skips was listed" "$out" ;;
  *"     5  alpha"*) ok ST-02 "the clean suite is counted (2 suites) but not listed" ;;
  *) bad ST-02 "alpha's row is missing" "$out" ;;
esac

# --- ST-03 ---
tsv="$(cat "$W/out/skips.tsv" 2>/dev/null)"
rows=$(printf '%s\n' "$tsv" | grep -c "^alpha	" || true)
sorted="$(printf '%s\n' "$tsv" | LC_ALL=C sort)"
if [ "$rows" -eq 5 ] && [ "$tsv" == "$sorted" ] && [ "$tsv" == "${tsv//$'\r'/}" ]; then
    ok ST-03 "TSV: 5 rows, sorted, no CR"
else
    bad ST-03 "TSV wrong (rows=$rows)" "$tsv"
fi

# --- ST-04 ---
# Bytes go in on stdin, so no path crosses into a native python.
if command -v python3 >/dev/null 2>&1; then
    py="$(cd "$REPO/tools/testrunner" && python3 -c '
import sys, parallel
n = 0
for raw in sys.stdin.buffer.read().split(b"\n"):
    if parallel.count_skip_markers(raw):
        n += 1
sys.stdout.buffer.write(str(n).encode("ascii"))
' < "$W/caps/alpha.log" 2>&1)"
    if [ "$py" = "5" ]; then
        ok ST-04 "parallel.py's count_skip_markers() marks the same 5 lines"
    else
        bad ST-04 "parallel.py's count_skip_markers() disagrees: $py lines" "$py"
    fi
else
    skip ST-04 "python3 unavailable -- agreement with parallel.py UNMEASURABLE"
fi

# --- ST-05 / ST-06 ---
o5="$(skip_tally "$W/no-such-dir")"; r5=$?
case "$o5" in *UNMEASURABLE*) [ $r5 -eq 0 ] && ok ST-05 "missing dir: reported unmeasurable, rc 0" || bad ST-05 "rc $r5" ;;
  *) bad ST-05 "missing dir reported as a count" "$o5" ;; esac
mkdir -p "$W/empty"
o6="$(skip_tally "$W/empty")"
case "$o6" in *UNMEASURABLE*) ok ST-06 "empty dir: reported unmeasurable" ;;
  *) bad ST-06 "empty dir reported as a count" "$o6" ;; esac

# --- ST-07 ---
touch "$W/notadir"
o7="$(set -e; skip_tally "$W/caps" "$W/notadir/x/skips.tsv" 2>/dev/null; echo "RC=$?")"
case "$o7" in
  *"5 skipped/unmeasurable"*"RC=0"*) ok ST-07 "unwritable TSV path: still tallies, rc 0 under set -e" ;;
  *) bad ST-07 "the tally failed or stopped" "$o7" ;;
esac

# --- ST-08 ---
rat="$REPO/run-all-tests.sh"
call=$(grep -n 'skip_tally "\$SHELL_TEST_CAPTURE_DIR"' "$rat" | head -1 | cut -d: -f1)
summ=$(grep -n '^# Print summary' "$rat" | head -1 | cut -d: -f1)
if [ -n "$call" ] && [ -n "$summ" ] && [ "$call" -lt "$summ" ]; then
    ok ST-08 "run-all-tests.sh calls it (line $call) before the summary (line $summ)"
else
    bad ST-08 "run-all-tests.sh does not call the tally before its summary (call=${call:-none}, summary=${summ:-none})"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
