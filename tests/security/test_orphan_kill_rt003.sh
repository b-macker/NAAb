#!/usr/bin/env bash
# test_orphan_kill_rt003.sh — V-RT-003: subprocess must be killed (not orphaned) on timeout
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

TMPDIR_TEST="${HOME}/.naab"
mkdir -p "$TMPDIR_TEST"

echo "=== test_orphan_kill_rt003.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: script spawns a long-running subprocess; naab times out; child must die
# ---------------------------------------------------------------------------
echo "[T1] Subprocess killed (not orphaned) when naab times out"

# Use a unique marker string so we can identify OUR sleep child even if other
# sleeps are running on the system.
MARKER="naab_orphan_test_$$"
SCRIPT_T1="${TMPDIR_TEST}/rt003_t1_$$.naab"
cat > "$SCRIPT_T1" <<NAAB
use process
main {
    // This long sleep would become an orphan if we don't kill it on timeout
    let r = process.run("sleep", ["30"])
    print("done")
}
NAAB

# Run naab with a 2s timeout; the script will block on process.run("sleep 30")
"$NAAB" "$SCRIPT_T1" --no-governance --timeout 2 >/dev/null 2>&1 || true
# Give a moment for kill/reap to settle
sleep 0.5

# Check no "sleep 30" process is left running (orphan check)
# We use pgrep to look for "sleep" with arg "30".
# Note: pgrep exits 1 when no matches found (= success for us). Wrap in a subshell
# with || true so set -euo pipefail doesn't abort the script on no-match exit code.
orphan_count=$( (pgrep -f "sleep 30" 2>/dev/null || true) | wc -l | tr -d ' ')
if [[ "$orphan_count" -eq 0 ]]; then
    ok "no orphan sleep process after timeout"
else
    fail "found $orphan_count orphan 'sleep 30' process(es) after naab timeout"
    # Kill them to not pollute the system
    pkill -f "sleep 30" 2>/dev/null || true
fi
rm -f "$SCRIPT_T1"

echo ""

# ---------------------------------------------------------------------------
# T2: subprocess completes before timeout → exit 0, no error
# ---------------------------------------------------------------------------
echo "[T2] Normal subprocess (completes within timeout) exits cleanly"
SCRIPT_T2="${TMPDIR_TEST}/rt003_t2_$$.naab"
cat > "$SCRIPT_T2" <<'NAAB'
use process
main {
    let r = process.run("echo", ["hello"])
    print("done")
}
NAAB

# Use || ec=$? to safely capture naab's exit code under set -euo pipefail.
# If naab exits non-zero, we capture the code and report a failure rather than
# aborting the test script before printing any result.
ec=0
out=$("$NAAB" "$SCRIPT_T2" --no-governance --timeout 10 2>&1) || ec=$?
if [[ "$ec" -eq 0 ]]; then
    ok "normal subprocess completed cleanly (exit 0)"
elif echo "$out" | grep -qi "timeout\|orphan\|killed"; then
    fail "subprocess was killed/timed out: ${out:0:120}"
else
    fail "normal subprocess failed (exit $ec): ${out:0:120}"
fi
rm -f "$SCRIPT_T2"

echo ""
TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
