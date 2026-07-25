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

cat > "$TMPDIR_TG/shallow.naab" <<'EOF'
fn down(n) {
    if n == 0 {
        return 0
    }
    return down(n - 1)
}

main {
    print(down(200))
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

# --- 3. Moderate recursion still works in both engines ---
out=$("$NAAB" --no-governance --tree-walk "$TMPDIR_TG/shallow.naab" 2>&1)
check "tree-walk depth-200 recursion succeeds" bash -c '[ "$1" = "0" ]' _ "$out"
out=$("$NAAB" --no-governance "$TMPDIR_TG/shallow.naab" 2>&1)
check "vm depth-200 recursion succeeds" bash -c '[ "$1" = "0" ]' _ "$out"

echo ""
echo "Stack guard tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
