#!/bin/bash
# Usability fixes based on Gemini LLM chat session analysis
# V-GOV-025 (path canonicalization), error messages, import hint, polyglot JSON wording

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

echo "=== Gemini Session Usability Fixes ==="
echo ""
echo "--- V-GOV-025: Path canonicalization ---"

# T1: governance_engine.cpp uses canonAndNorm (not just normSep) for path matching
grep -q 'canonAndNorm' "$SRC/runtime/governance_engine.cpp"
check $? "canonAndNorm helper exists in governance_engine.cpp"

# T2: All 4 path comparison sites use canonAndNorm
COUNT=$(grep -c 'canonAndNorm(bp)\|canonAndNorm(ap)' "$SRC/runtime/governance_engine.cpp")
if [ "$COUNT" -ge 4 ]; then
    check 0 "All path comparisons use canonAndNorm ($COUNT sites)"
else
    check 1 "Expected 4+ canonAndNorm calls, found $COUNT"
fi

# T3: canonAndNorm applies weakly_canonical
grep -A5 'canonAndNorm' "$SRC/runtime/governance_engine.cpp" | grep -q 'weakly_canonical'
check $? "canonAndNorm uses weakly_canonical"

# T4: Runtime — relative allowed_paths work with file operations
WORK_DIR=$(mktemp -d "$TMPDIR/naab_gov025_XXXXXX")
mkdir -p "$WORK_DIR/output"
cat > "$WORK_DIR/govern.json" << 'GOVEOF'
{
    "version": "1.0.0",
    "mode": "enforce",
    "capabilities": {
        "filesystem": {
            "mode": "restricted",
            "allowed_paths": ["./output"]
        }
    }
}
GOVEOF
cat > "$WORK_DIR/test.naab" << 'NAABEOF'
import "file" as file;
main {
    file.write("./output/test.txt", "hello from governance");
    let content = file.read("./output/test.txt");
    print(content);
}
NAABEOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test.naab 2>&1)
if echo "$OUTPUT" | grep -q "hello from governance"; then
    check 0 "Relative ./output in allowed_paths matches ./output/test.txt"
else
    check 1 "Relative path matching failed: $OUTPUT"
fi
rm -rf "$WORK_DIR"

echo ""
echo "--- Enhanced governance error messages ---"

# T5: Error message includes "Resolved to:" for path mismatch diagnostics
grep -q 'Resolved to:' "$SRC/runtime/governance_engine.cpp"
check $? "Governance error shows resolved path"

# T6: Error message includes "Allowed (resolved):"
grep -q 'Allowed (resolved):' "$SRC/runtime/governance_engine.cpp"
check $? "Governance error shows resolved allowed paths"

echo ""
echo "--- Import hint fix ---"

# T7: Import hint no longer says "not 'import'"
if grep -q "not 'import'" "$SRC/parser/error_hints.cpp"; then
    check 1 "Import hint still says \"not 'import'\""
else
    check 0 "Import hint no longer says \"not 'import'\""
fi

# T8: Import hint shows both import and use as valid
grep -q 'import.*path.*as mod' "$SRC/parser/error_hints.cpp"
check $? "Import hint shows valid import syntax"

# T9: Import hint shows use for stdlib
grep -q 'use math' "$SRC/parser/error_hints.cpp"
check $? "Import hint shows use for stdlib"

# T10: looksLikeJavaScriptImport checks next token type
grep -q 'next_token->type' "$SRC/parser/error_hints.cpp"
check $? "JS import detection checks next token type"

echo ""
echo "--- Polyglot -> JSON error wording ---"

# T11: Error says "output on stdout" not "return value"
grep -q 'expected JSON output on stdout' "$SRC/interpreter/polyglot.cpp"
check $? "Polyglot JSON error says 'output on stdout'"

# T12: Error no longer says "expected a JSON return value"
if grep -q 'expected a JSON return value' "$SRC/interpreter/polyglot.cpp"; then
    check 1 "Still says 'return value'"
else
    check 0 "No longer says 'return value'"
fi

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
