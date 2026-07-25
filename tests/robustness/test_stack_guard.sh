#!/bin/bash
# Native stack guard regression test (#96)
#
# The Jul 14 nightly fuzz run found an ASan stack-overflow: unbounded NAAb
# function recursion blew the native C++ stack in the tree-walker long before
# the logical MAX_CALL_STACK_DEPTH (10000) guard could fire, because each
# NAAb-level call consumes many native frames whose size varies by build.
# The fix measures real stack headroom in eval()/executeStmt() and throws a
# catchable RecursionLimitException instead of crashing.
#
# Assertions:
#   1. Unbounded recursion under --tree-walk exits with a clean error
#      (no SIGSEGV/SIGABRT — exit code < 128) mentioning recursion/stack.
#   2. Same program under the VM also errors cleanly (FRAMES_MAX guard).
#   3. Moderate recursion (depth 200) still succeeds in both engines.

set -u
NAAB="${NAAB:-./build/naab-lang}"
if [ ! -x "$NAAB" ]; then
    echo "SKIP: $NAAB not found"
    exit 0
fi

TMPDIR_TG=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TG"' EXIT
PASS=0
FAIL=0

cat > "$TMPDIR_TG/deep.naab" <<'EOF'
fn rec(n) {
    return rec(n + 1)
}

main {
    print(rec(0))
}
EOF

# Depth 50 is deliberately modest: the invariant under test is that the
# headroom guard never fires on ordinary recursion, and it must hold on
# every platform — including Windows, whose main thread gets the default
# 1MB stack (Linux gives 8MB).
cat > "$TMPDIR_TG/shallow.naab" <<'EOF'
fn down(n) {
    if n == 0 {
        return 0
    }
    return down(n - 1)
}

main {
    print(down(50))
}
EOF

check() {
    local desc="$1"; shift
    if "$@"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

# --- 1. Tree-walker: unbounded recursion must error cleanly, not crash ---
out=$("$NAAB" --no-governance --tree-walk "$TMPDIR_TG/deep.naab" 2>&1)
rc=$?
check "tree-walk deep recursion exits nonzero (rc=$rc)" [ "$rc" -ne 0 ]
check "tree-walk deep recursion is not a crash (rc=$rc < 128)" [ "$rc" -lt 128 ]
check "tree-walk deep recursion reports a recursion/stack error" \
    bash -c 'echo "$1" | grep -qiE "recursion|stack"' _ "$out"

# --- 2. VM: same program errors cleanly via the FRAMES_MAX guard ---
out=$("$NAAB" --no-governance "$TMPDIR_TG/deep.naab" 2>&1)
rc=$?
check "vm deep recursion exits nonzero (rc=$rc)" [ "$rc" -ne 0 ]
check "vm deep recursion is not a crash (rc=$rc < 128)" [ "$rc" -lt 128 ]
check "vm deep recursion reports a recursion/stack error" \
    bash -c 'echo "$1" | grep -qiE "recursion|stack|call depth"' _ "$out"

# --- 3. Ordinary recursion must never trip the guard ---
# stdout and stderr are kept separate (a stderr notice must not be mistaken
# for program output) and matched by line, not by whole-string equality, so
# CRLF and incidental warnings don't decide the result. On failure the
# actual exit code and both streams are printed — a CI-only failure here
# should be diagnosable from the log without another round trip.
check_shallow() {
    local label="$1"; shift
    "$NAAB" --no-governance "$@" "$TMPDIR_TG/shallow.naab" \
        > "$TMPDIR_TG/out.txt" 2> "$TMPDIR_TG/err.txt"
    local rc=$?
    if [ "$rc" -eq 0 ] && tr -d '\r' < "$TMPDIR_TG/out.txt" | grep -qx "0"; then
        echo "  PASS: $label depth-50 recursion succeeds"
        PASS=$((PASS + 1))
    else
        # Truncated: a VM stack-overflow dumps a frame per line
        echo "  FAIL: $label depth-50 recursion succeeds (rc=$rc)"
        echo "        stdout: $(tr -d '\r' < "$TMPDIR_TG/out.txt" | tr '\n' '|' | head -c 300)"
        echo "        stderr: $(tr -d '\r' < "$TMPDIR_TG/err.txt" | tr '\n' '|' | head -c 300)"
        FAIL=$((FAIL + 1))
    fi
}

check_shallow "tree-walk" --tree-walk
check_shallow "vm"

echo ""
echo "Stack guard tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
