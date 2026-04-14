#!/bin/bash
# Usability fixes from Gemini naab-4 session
# Rust variable injection bug — let statements at module scope

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

echo "=== naab-4 Session Usability Fixes ==="
echo ""
echo "--- Rust variable injection fix ---"

# T1: Source — polyglot.cpp has Rust-specific fn main() injection
grep -q 'language == "rust"' "$SRC/interpreter/polyglot.cpp" | head -1
grep -A5 'language == "rust"' "$SRC/interpreter/polyglot.cpp" | grep -q 'fn main()'
check $? "polyglot.cpp handles Rust fn main() for var injection"

# T2: Source — vars injected AFTER fn main() opening brace
grep -q 'brace_pos' "$SRC/interpreter/polyglot.cpp"
check $? "Rust vars injected after fn main() brace"

# T3: file.exists() already in stdlib
grep -q '"exists"' "$SRC/stdlib/file_impl.cpp"
check $? "file.exists() already implemented"

# T4: file.create_dir() already in stdlib
grep -q '"create_dir"' "$SRC/stdlib/file_impl.cpp"
check $? "file.create_dir() already implemented"

# T5: file.mkdir() alias exists
grep -q '"mkdir"' "$SRC/stdlib/file_impl.cpp"
check $? "file.mkdir() alias exists"

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
