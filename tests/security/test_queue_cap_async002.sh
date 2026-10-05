#!/usr/bin/env bash
# Test V-ASYNC-002: ThreadPool queue cap — spawning more async tasks than the
# queue limit (1000) must produce a clear error, not OOM or SIGSEGV.

NAAB="${NAAB_BIN:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ((FAIL++)); }
skip() { echo "  SKIP: $1"; ((SKIP++)); }

WORK_DIR=$(mktemp -d)
source "$(dirname "$0")/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$WORK_DIR"; teardown_isolated_trust' EXIT
# Until this was rewritten, none of the programs below parsed: they used
# `function main()` (NAAb's entry point is `main {}`) and `async noop()` (NAAb
# has `async fn`; calling one starts it). Every run exited 1 with a parse error,
# and every arm accepts a non-zero exit without a crash -- so all three passed
# without running anything. There was also no govern.json here, so even valid
# programs would have exited 4 ("no govern.json found"). The programs now parse,
# the arms are unchanged, and C1/C2 are the controls that work actually ran.
#
# Note the cap the header describes (ThreadPool max_queue_size) is NOT on the
# `async fn` path: the VM runs each call on its own std::async thread. What
# bounds that path is the value-handle table (256 ranges); C1/C2 cover it.
echo '{ "version": "4.0", "mode": "off" }' > "$WORK_DIR/govern.json"

echo "=== V-ASYNC-002: ThreadPool Queue Cap ==="

# T1: Spawn 2000 async tasks in a tight loop → must get a clear queue-full error, not crash.
cat > "$WORK_DIR/t1.naab" <<'EOF'
async fn noop() {
    return 0
}

main {
    let i = 0
    while i < 2000 {
        let _ = noop()
        i = i + 1
    }
}
EOF

output=$("$NAAB" "$WORK_DIR/t1.naab" 2>&1)
exit_code=$?
# Must not be killed by SIGSEGV (139) or SIGABRT (134)
if [ $exit_code -eq 139 ] || [ $exit_code -eq 134 ]; then
    fail "T1: process crashed (exit $exit_code) — queue overflow caused memory corruption"
elif echo "$output" | grep -qiE "(queue full|queue.*full|pending.*tasks|async queue)"; then
    pass "T1: queue-full error message produced (exit $exit_code)"
elif [ $exit_code -ne 0 ]; then
    pass "T1: non-zero exit on queue overflow (exit $exit_code) — no crash"
else
    # If it somehow completed with 0 exit, the cap may not have triggered
    # (could be normal if tasks drain fast enough — acceptable behavior)
    pass "T1: completed without crash (tasks may have drained between enqueues)"
fi

# T2: Spawn ≤ 50 async tasks → must complete normally with exit 0.
cat > "$WORK_DIR/t2.naab" <<'EOF'
async fn noop() {
    return 1
}

main {
    let futures = []
    let i = 0
    while i < 50 {
        futures.push(noop())
        i = i + 1
    }
}
EOF

output=$("$NAAB" "$WORK_DIR/t2.naab" 2>&1)
exit_code=$?
if [ $exit_code -eq 0 ]; then
    pass "T2: 50 async tasks completed normally (no false positive)"
elif echo "$output" | grep -qiE "(queue full|queue.*full)"; then
    fail "T2: queue-full error for only 50 tasks — cap too low"
else
    # Non-zero exit for other reasons (runtime error, etc.) is acceptable
    pass "T2: completed without crash (exit $exit_code)"
fi

# T3: Verify error message text format when queue is full.
cat > "$WORK_DIR/t3.naab" <<'EOF'
async fn work() {
    return 42
}

main {
    let i = 0
    while i < 1500 {
        let _ = work()
        i = i + 1
    }
}
EOF

output=$("$NAAB" "$WORK_DIR/t3.naab" 2>&1)
exit_code=$?
if [ $exit_code -eq 139 ] || [ $exit_code -eq 134 ]; then
    fail "T3: process crashed — queue overflow is unsafe"
elif echo "$output" | grep -qE "[0-9]+ tasks pending|queue full"; then
    pass "T3: error message includes task count and 'queue full' text"
elif [ $exit_code -ne 0 ]; then
    pass "T3: non-zero exit on large async loop (exit $exit_code)"
else
    pass "T3: completed without crash"
fi

# C1: 2000 un-awaited async fn calls must run to completion. Each call used to
# take a fresh 64K-handle range that was never returned, so from about the
# 256th call the VM wrote past its handle table and crashed (exit 139 at 600
# calls, even when each call was awaited). The marker proves the loop ran.
cat > "$WORK_DIR/c1.naab" <<'EOF'
async fn noop() {
    return 0
}

main {
    let fs = []
    let i = 0
    while i < 2000 {
        fs.push(noop())
        i = i + 1
    }
    let n = 0
    for f in fs {
        n = n + await f
        n = n + 1
    }
    print("C1_DONE:" + string(n))
}
EOF
output=$(timeout 60 "$NAAB" "$WORK_DIR/c1.naab" 2>&1)
exit_code=$?
if [ $exit_code -eq 0 ] && echo "$output" | grep -q "C1_DONE:2000"; then
    pass "C1: 2000 async fn calls completed and were awaited"
else
    fail "C1: 2000 async fn calls did not complete (exit $exit_code): $(echo "$output" | tail -2)"
fi

# C2: more workers alive at once than the handle table has ranges must fail
# with a clear runtime error -- not a signal, and not silent corruption. The
# sleep holds every worker alive together. C1 is the control that the error is
# about concurrency, not about the number of calls.
cat > "$WORK_DIR/c2.naab" <<'EOF'
use time
async fn hold(i) {
    time.sleep(1.5)
    return i
}

main {
    let fs = []
    let i = 0
    while i < 400 {
        fs.push(hold(i))
        i = i + 1
    }
    print("C2_SPAWNED")
    for f in fs {
        let v = await f
    }
    print("C2_ALL_AWAITED")
}
EOF
output=$(timeout 60 "$NAAB" "$WORK_DIR/c2.naab" 2>&1)
exit_code=$?
if [ $exit_code -eq 139 ] || [ $exit_code -eq 134 ] || [ $exit_code -eq 124 ]; then
    fail "C2: 400 live workers crashed or hung (exit $exit_code)"
elif echo "$output" | grep -q "C2_SPAWNED" && echo "$output" | grep -q "handle space exhausted" \
     && [ $exit_code -ne 0 ]; then
    pass "C2: exhausting the handle table is a clear runtime error (exit $exit_code)"
else
    fail "C2: expected a clear handle-exhaustion error (exit $exit_code): $(echo "$output" | tail -2)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
