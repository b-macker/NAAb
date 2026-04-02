#!/bin/bash
# Tests GC CLI flags: --gc-threshold N and --gc-stats
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0

check() {
    local desc="$1"
    local condition="$2"
    if eval "$condition"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

# Create temp dir in home (Termux has no /tmp)
TMPDIR_NAAB="${HOME}/.naab_gc_test_$$"
mkdir -p "$TMPDIR_NAAB"
cleanup() { rm -rf "$TMPDIR_NAAB"; }
trap cleanup EXIT

# Simple program that allocates several strings
TMPFILE="${TMPDIR_NAAB}/alloc.naab"
cat > "$TMPFILE" << 'NAAB'
main {
    let count = 0
    for i in 0..200 {
        let s = "item_" + i
        count = count + 1
    }
    print(count)
}
NAAB

echo "=== GC Flag Tests ==="
echo ""

# --gc-stats output appears on stderr, stdout shows program output
OUTPUT=$("$NAAB_BIN" --gc-stats "$TMPFILE" 2>&1)
check "--gc-stats flag accepted (no unknown-flag error)" '! echo "$OUTPUT" | grep -q "Unknown flag"'
check "--gc-stats does not break execution" 'echo "$OUTPUT" | grep -q "200"'

# When --gc-stats is set, the [GC] line appears in stderr
GC_OUTPUT=$("$NAAB_BIN" --gc-stats "$TMPFILE" 2>&1 1>/dev/null)
# stderr should include the [GC] stats line
COMBINED=$("$NAAB_BIN" --gc-stats "$TMPFILE" 2>&1)
check "--gc-stats prints [GC] line" 'echo "$COMBINED" | grep -q "\[GC\]"'
check "--gc-stats shows Allocations tracked" 'echo "$COMBINED" | grep -q "Allocations tracked"'
check "--gc-stats shows Collections run" 'echo "$COMBINED" | grep -q "Collections run"'

# --gc-threshold sets a low threshold; execution should still succeed
OUTPUT=$("$NAAB_BIN" --gc-threshold 100 "$TMPFILE" 2>&1)
check "--gc-threshold 100 flag accepted" '! echo "$OUTPUT" | grep -q "Unknown flag"'
check "--gc-threshold 100 execution succeeds" 'echo "$OUTPUT" | grep -q "200"'

# --gc-threshold with --gc-stats: low threshold means more collections
COMBINED=$("$NAAB_BIN" --gc-threshold 50 --gc-stats "$TMPFILE" 2>&1)
check "--gc-threshold 50 + --gc-stats works" 'echo "$COMBINED" | grep -q "200"'
check "--gc-threshold 50 + --gc-stats shows [GC] line" 'echo "$COMBINED" | grep -q "\[GC\]"'

# --gc-stats on post-run position (after filename) also works
OUTPUT=$("$NAAB_BIN" "$TMPFILE" --gc-stats 2>&1)
check "--gc-stats after filename works" 'echo "$OUTPUT" | grep -q "\[GC\]"'

# --gc-threshold after filename
OUTPUT=$("$NAAB_BIN" "$TMPFILE" --gc-threshold 200 2>&1)
check "--gc-threshold after filename accepted" '! echo "$OUTPUT" | grep -q "Unknown flag"'

echo ""
echo "GC Flag Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
