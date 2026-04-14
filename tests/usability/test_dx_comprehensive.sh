#!/bin/bash
# Comprehensive DX fixes test — cross-session + R35 report
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
PASS=0; FAIL=0; TOTAL=0

check() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "0" ]; then
        PASS=$((PASS + 1))
        echo "  PASS: T$TOTAL - $2"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: T$TOTAL - $2"
    fi
}

make_work_dir() {
    local d=$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_dx_XXXXXX")
    cat > "$d/govern.json" << 'GOVEOF'
{"version":"1.0.0","mode":"off"}
GOVEOF
    echo "$d"
}

echo "=== Comprehensive DX Fixes ==="
echo ""

# --- Fix 1: import "file" without as ---
echo "--- Fix 1: import without 'as' shows both styles ---"
WORK_DIR=$(make_work_dir)
cat > "$WORK_DIR/test.naab" << 'EOF'
import "somefile"
main { print("hi") }
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -q "two import styles" && echo "$OUTPUT" | grep -q "Built-in modules"
check $? "import without 'as' shows both import styles + module list"
rm -rf "$WORK_DIR"

# --- Fix 2: Module aliases ---
echo ""
echo "--- Fix 2: Module name aliases ---"
WORK_DIR=$(make_work_dir)
cat > "$WORK_DIR/test.naab" << 'EOF'
use fs
main { print("hi") }
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -qi "did you mean.*file\|use file"
check $? "use fs shows 'Did you mean: use file'"

cat > "$WORK_DIR/test.naab" << 'EOF'
use re
main { print("hi") }
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -qi "did you mean.*regex\|use regex"
check $? "use re shows 'Did you mean: use regex'"
rm -rf "$WORK_DIR"

# --- Fix 3: spawn/wait hints ---
echo ""
echo "--- Fix 3: spawn/wait/wait_all hints ---"
WORK_DIR=$(make_work_dir)
cat > "$WORK_DIR/test.naab" << 'EOF'
main { spawn() }
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -q "async function"
check $? "spawn shows async function hint"

cat > "$WORK_DIR/test.naab" << 'EOF'
main { wait_all() }
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -q "await"
check $? "wait_all shows await hint"
rm -rf "$WORK_DIR"

# --- Fix 4: array.map/filter/reduce aliases ---
echo ""
echo "--- Fix 4: array.map/filter/reduce aliases ---"
WORK_DIR=$(make_work_dir)
cat > "$WORK_DIR/test.naab" << 'EOF'
use array
main {
    let arr = [1, 2, 3]
    let doubled = array.map(arr, fn(x) { return x * 2 })
    print(doubled)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -q "\[2, 4, 6\]"
check $? "array.map() works as alias for map_fn"

cat > "$WORK_DIR/test.naab" << 'EOF'
use array
main {
    let arr = [1, 2, 3, 4, 5]
    let evens = array.filter(arr, fn(x) { return x % 2 == 0 })
    print(evens)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -q "\[2, 4\]"
check $? "array.filter() works as alias for filter_fn"
rm -rf "$WORK_DIR"

# --- Fix 6: Parser expect() upgrades ---
echo ""
echo "--- Fix 6: Parser expect() upgrades ---"
WORK_DIR=$(make_work_dir)

# T8: match => hint
cat > "$WORK_DIR/test.naab" << 'EOF'
main {
    let x = 1
    match x {
        1 : "one"
    }
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -q "Match arms use"
check $? "match ':' instead of '=>' shows hint"

# T9: try without catch hint
cat > "$WORK_DIR/test.naab" << 'EOF'
main {
    try {
        print("hello")
    }
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
echo "$OUTPUT" | grep -q "catch"
check $? "try without catch shows example"
rm -rf "$WORK_DIR"

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
