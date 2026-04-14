#!/bin/bash
# Usability + security fixes from Gemini naab-5 session
# print() taint sink, max_json_depth governance, C-style for hint, {} hint

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

SRC="$SCRIPT_DIR/../../src"

echo "=== naab-5 Session Fixes ==="
echo ""
echo "--- Fix 1: print() taint sink ---"

# T1: Source — call_dispatch.cpp has taint check before print
grep -q 'checkExpressionTaintedSink' "$SRC/interpreter/call_dispatch.cpp" && \
grep -B5 'func_name == "print"' "$SRC/interpreter/call_dispatch.cpp" | grep -q 'Built-in' && \
grep -A10 'Built-in functions' "$SRC/interpreter/call_dispatch.cpp" | grep -q 'checkExpressionTaintedSink'
check $? "Tree-walker print() has taint sink check"

# T2: Source — vm.cpp has taint check for print in builtin call path
grep -q 'Taint sink check for print' "$SRC/vm/vm.cpp" && \
grep -q 'print-arg' "$SRC/vm/vm.cpp"
check $? "VM print/println has taint sink check"

echo ""
echo "--- Fix 2: max_json_depth governance ---"

# T3: Source — governance_config.cpp parses max_json_depth
grep -q 'max_json_depth' "$SRC/runtime/governance_config.cpp"
check $? "governance_config.cpp parses max_json_depth"

# T4: Source — json_impl.cpp uses dynamic depth from limits
grep -q 'getMaxJsonDepth' "$SRC/stdlib/json_impl.cpp"
check $? "json_impl.cpp uses governance-configured depth"

# T5: Runtime — json.stringify respects governance depth limit
WORK_DIR=$(mktemp -d "$TMPDIR/naab_json_depth_XXXXXX")
mkdir -p "$WORK_DIR"
cat > "$WORK_DIR/govern.json" << 'GOVEOF'
{
  "version": "1.0.0",
  "mode": "enforce",
  "limits": {
    "data": {
      "max_json_depth": 3
    }
  }
}
GOVEOF
cat > "$WORK_DIR/test.naab" << 'EOF'
use json

main {
    let deep = {"a": {"b": {"c": {"d": 1}}}}
    try {
        let s = json.stringify(deep)
        print("NO ERROR: " + s)
    } catch (e) {
        print("CAUGHT: depth exceeded")
    }
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -q "depth exceeded"; then
    check 0 "json.stringify blocked by governance max_json_depth=3"
else
    check 1 "Expected depth exceeded: $OUTPUT"
fi
rm -rf "$WORK_DIR"

echo ""
echo "--- Fix 3: C-style for loop hint ---"

# T6: Source — parser detects C-style for loop keywords
grep -q 'Python-style for loops' "$SRC/parser/parser.cpp"
check $? "Parser has C-style for loop hint"

# T7: Runtime — for (let i = 0; ...) gives helpful error
WORK_DIR=$(mktemp -d "$TMPDIR/naab_for_XXXXXX")
cat > "$WORK_DIR/test.naab" << 'EOF'
main {
    for (let i = 0; i < 10; i = i + 1) {
        print(i)
    }
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -q "range"; then
    check 0 "for (let i ...) gives range() hint"
else
    check 1 "Expected range hint: $OUTPUT"
fi
rm -rf "$WORK_DIR"

echo ""
echo "--- Fix 4: Polyglot {} hint ---"

# T8: Source — polyglot error handler has {} brace hint
grep -q 'wrap polyglot code in { }' "$SRC/interpreter/polyglot.cpp"
check $? "Polyglot error handler has {} brace hint"

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
