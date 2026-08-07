#!/usr/bin/env bash
# test_orphan_kill_rt003.sh — V-RT-003: subprocess must be killed (not orphaned) on timeout
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

TMPDIR_TEST="${HOME}/.naab"
mkdir -p "$TMPDIR_TEST"

echo "=== test_orphan_kill_rt003.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: script spawns a long-running subprocess; naab times out; child must die
# ---------------------------------------------------------------------------
echo "[T1] Subprocess killed (not orphaned) when naab times out"

# A unique duration, so the process we look for is OURS. The original code
# declared a MARKER variable with a comment explaining exactly why one was
# needed, then never used it: both pgrep and pkill matched the generic
# "sleep 30". Three consequences, all live:
#   - vacuous PASS — if the child never spawned, nothing matches and the orphan
#     check reports success without a subprocess ever having existed;
#   - spurious FAIL — any unrelated `sleep 30` on the machine fails the test;
#   - collateral damage — the failure path ran `pkill -f "sleep 30"`, killing
#     other users' and other suites' processes on a shared runner.
# The fix is to observe the specific PID rather than a command pattern: it makes
# the control possible (we can assert the child DID start) and it makes both the
# check and the cleanup incapable of touching anything that is not ours.
UNIQ_SLEEP="30.$(( $$ % 997 ))"
SCRIPT_T1="${TMPDIR_TEST}/rt003_t1_$$.naab"
cat > "$SCRIPT_T1" <<NAAB
use process
main {
    // This long sleep would become an orphan if we don't kill it on timeout
    let r = process.run("sleep", ["$UNIQ_SLEEP"])
    print("done")
}
NAAB

# Run naab in the background so the child can be observed while it is alive.
"$NAAB" "$SCRIPT_T1" --no-governance --timeout 2 >/dev/null 2>&1 &
NAAB_PID=$!

# Control: the subprocess must actually start, or the orphan check below is
# asserting that something which never existed was cleaned up.
CHILD_PIDS=""
for _ in $(seq 1 40); do
    CHILD_PIDS=$( (pgrep -f "sleep $UNIQ_SLEEP" 2>/dev/null || true) | tr '\n' ' ')
    [ -n "${CHILD_PIDS// /}" ] && break
    sleep 0.1
done

wait "$NAAB_PID" 2>/dev/null || true
sleep 0.5   # let kill/reap settle

if [ -z "${CHILD_PIDS// /}" ]; then
    skip "subprocess never started — nothing could be orphaned, kill-on-timeout not exercised"
else
    orphan_count=0
    for p in $CHILD_PIDS; do
        kill -0 "$p" 2>/dev/null && orphan_count=$((orphan_count + 1))
    done
    if [[ "$orphan_count" -eq 0 ]]; then
        ok "no orphan sleep process after timeout (child $CHILD_PIDS started and was reaped)"
    else
        fail "found $orphan_count orphan sleep process(es) after naab timeout: $CHILD_PIDS"
        # Only ever kill PIDs we observed ourselves.
        for p in $CHILD_PIDS; do kill -9 "$p" 2>/dev/null || true; done
    fi
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
echo "Results: ${PASS}/${TOTAL} passed${SKIP:+, ${SKIP} skipped}"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
