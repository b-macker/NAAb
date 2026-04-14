#!/bin/bash
# Fixes from Gemini naab-6 session
# Governance max_array_size on literals, async syntax hints

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

echo "=== naab-6 Session Fixes ==="
echo ""
echo "--- Fix 1: Governance max_array_size on literals ---"

# T1: Source — VM OP_LIST has governance checkArraySize
grep -A5 'OP_LIST' "$SRC/vm/vm.cpp" | grep -q 'governance_->checkArraySize'
check $? "VM OP_LIST has governance array size check"

# T2: Source — expressions.cpp ListExpr has governance check
grep -A10 'visit(ast::ListExpr' "$SRC/interpreter/expressions.cpp" | grep -q 'governance_->checkArraySize'
check $? "Tree-walker ListExpr has governance array size check"

# T3: Source — expressions.cpp DictExpr has governance check
grep -A10 'visit(ast::DictExpr' "$SRC/interpreter/expressions.cpp" | grep -q 'governance_->checkArraySize'
check $? "Tree-walker DictExpr has governance array size check"

# T4: Runtime — array literal blocked when governance max_array_size exceeded
WORK_DIR=$(mktemp -d "$TMPDIR/naab_arr_gov_XXXXXX")
mkdir -p "$WORK_DIR"
cat > "$WORK_DIR/govern.json" << 'GOVEOF'
{
  "version": "1.0.0",
  "mode": "enforce",
  "limits": {
    "array_size": 5
  }
}
GOVEOF
cat > "$WORK_DIR/test.naab" << 'EOF'
main {
    try {
        let arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        print("NO ERROR: array created with " + len(arr) + " elements")
    } catch (e) {
        print("CAUGHT: array size blocked")
    }
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -qi "array size\|size.*exceed\|CAUGHT"; then
    check 0 "Array literal blocked by governance max_array_size=5"
else
    check 1 "Expected array size error: $OUTPUT"
fi
rm -rf "$WORK_DIR"

echo ""
echo "--- Fix 2: Async syntax hints ---"

# T5: Source — parser has async block hint
grep -q 'async blocks.*async function' "$SRC/parser/parser.cpp"
check $? "Parser has async block hint"

# T6: Runtime — async { } gives helpful error
WORK_DIR=$(mktemp -d "$TMPDIR/naab_async_XXXXXX")
cat > "$WORK_DIR/test.naab" << 'EOF'
main {
    async {
        print("hello")
    }
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -q "async function"; then
    check 0 "async { } gives async function hint"
else
    check 1 "Expected async function hint: $OUTPUT"
fi
rm -rf "$WORK_DIR"

# T7: Runtime — async taskName() gives helpful error
WORK_DIR=$(mktemp -d "$TMPDIR/naab_async2_XXXXXX")
cat > "$WORK_DIR/test.naab" << 'EOF'
function myTask() {
    print("hello")
}
main {
    async myTask()
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -q "async function myTask"; then
    check 0 "async myTask() gives async function declaration hint"
else
    check 1 "Expected async function myTask hint: $OUTPUT"
fi
rm -rf "$WORK_DIR"

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
