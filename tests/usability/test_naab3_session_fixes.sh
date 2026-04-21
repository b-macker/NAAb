#!/bin/bash
# Usability fixes from Gemini naab-3 session analysis
# -> JSON placement hint, Go auto-wrapping, () binding error

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

echo "=== naab-3 Session Usability Fixes ==="
echo ""
echo "--- Fix 1: -> JSON placement hint ---"

# T1: Parser has ARROW token check near INLINE_CODE
grep -q "tok.type == lexer::TokenType::ARROW" "$SRC/parser/parser.cpp"
check $? "Parser checks ARROW token for hint"

# T2: Hint mentions 'block header'
grep -q 'block header' "$SRC/parser/parser.cpp"
check $? "Hint explains -> JSON goes in block header"

# T3: Runtime — >> -> JSON gives helpful error
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/naab_json_hint_XXXXXX")
cat > "$WORK_DIR/test.naab" << 'EOF'
main {
    let x = <<python
print("hello")
>> -> JSON;
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -q "block header"; then
    check 0 ">> -> JSON error shows placement guidance"
else
    check 1 "Expected hint about block header: $OUTPUT"
fi
rm -rf "$WORK_DIR"

echo ""
echo "--- Fix 2: Go auto-wrapping improvements ---"

# T4: Go wrapper detects func main()
grep -q 'func main()' "$SRC/runtime/go_executor.cpp"
check $? "Go wrapper detects user func main()"

# T5: Go wrapper has grouped import block
grep -q 'import (' "$SRC/runtime/go_executor.cpp"
check $? "Go wrapper uses grouped import block"

# T6: Go wrapper auto-detects time package
grep -q 'time\.' "$SRC/runtime/go_executor.cpp"
check $? "Go wrapper auto-imports time package"

# T7: Go wrapper auto-detects encoding/json
grep -q 'encoding/json' "$SRC/runtime/go_executor.cpp"
check $? "Go wrapper auto-imports encoding/json"

# T8: Go wrapper filters user import lines
grep -q 'isImportLine' "$SRC/runtime/go_executor.cpp"
check $? "Go wrapper filters user import lines"

echo ""
echo "--- Fix 3: () binding error ---"

# T9: Lexer rejects () for polyglot binding with helpful error
grep -q 'square brackets.*not parentheses' "$SRC/lexer/lexer.cpp"
check $? "Lexer has () binding error with guidance"

# T10: Runtime — <<python(x) gives clear error
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/naab_paren_XXXXXX")
cat > "$WORK_DIR/test.naab" << 'EOF'
main {
    let x = 10;
    let y = <<python(x)
print(x)
>>;
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -q "square brackets"; then
    check 0 "<<python(x) gives clear error about using [x]"
else
    check 1 "Expected bracket hint: $OUTPUT"
fi
rm -rf "$WORK_DIR"

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
