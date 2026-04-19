#!/usr/bin/env bash
# test_error_hints.sh — Tests for LLM-friendly error message hints
# Validates that common mistakes produce helpful suggestions
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
PASS=0
FAIL=0
TD=$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/error_hints_XXXXXX")

cleanup() { rm -rf "$TD"; }
trap cleanup EXIT

echo '{"version":"5.0","mode":"off"}' > "$TD/govern.json"

check() {
    local desc="$1"
    local pattern="$2"
    local file="$3"
    local output
    output=$("$NAAB" "$file" 2>&1 || true)
    if echo "$output" | grep -qi "$pattern"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "    Expected pattern: $pattern"
        echo "    Got: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Error Hint Tests ==="
echo ""

# --- Phase 1: VM dict key error ---
echo "--- Dict Key Error ---"
echo 'main { let d = {"name": "Alice"}; print(d["age"]) }' > "$TD/dict_key.naab"
check "dict[missing] shows available keys" "Available keys" "$TD/dict_key.naab"
check "dict[missing] suggests .get()" "dict.get" "$TD/dict_key.naab"
check "dict[missing] suggests .has()" "dict.has" "$TD/dict_key.naab"

# --- Phase 2: VM method-not-found ---
echo ""
echo "--- Method Not Found ---"
echo 'main { let s = "hello"; print(s.len()) }' > "$TD/str_len.naab"
check "string.len() suggests .length()" "length" "$TD/str_len.naab"

echo 'main { let s = "hello"; print(s.strip()) }' > "$TD/str_strip.naab"
check "string.strip() suggests .trim()" "trim" "$TD/str_strip.naab"

echo 'main { let s = "hello"; print(s.charAt(0)) }' > "$TD/str_charat.naab"
check "string.charAt() suggests .char_at()" "char_at" "$TD/str_charat.naab"

echo 'main { let a = [1,2,3]; print(a.len()) }' > "$TD/list_len.naab"
check "list.len() suggests .length()" "length" "$TD/list_len.naab"

echo 'main { let a = [1,2]; a.forEach(fn(x) { print(x) }) }' > "$TD/list_foreach.naab"
check "list.forEach() suggests for...in" "for.*in" "$TD/list_foreach.naab"

echo 'main { let d = {"a": 1}; print(d.len()) }' > "$TD/dict_len.naab"
check "dict.len() suggests .size()" "size" "$TD/dict_len.naab"

echo 'main { let d = {"a": 1}; print(d.setDefault("b", 2)) }' > "$TD/dict_unknown.naab"
check "dict unknown method lists available methods" "Available dict methods" "$TD/dict_unknown.naab"

# --- Phase 3: Built-in function hints ---
echo ""
echo "--- Built-in Function Hints ---"
# parseInt may be caught by variable helper or builtin — check for int()
echo 'main { let x = parseInt("42") }' > "$TD/parseint.naab"
check "parseInt() suggests int()" "int(" "$TD/parseint.naab"

# --- Phase 4: Compile-time || warning ---
echo ""
echo "--- Logical OR Hint ---"
echo 'main { let x = null; let y = x || "default"; print(y) }' > "$TD/or_hint.naab"
check "|| with string RHS hints about ??" "null coalesce" "$TD/or_hint.naab"

# --- Phase 5: Match arm { } warning ---
echo ""
echo "--- Match Arm Warning ---"
cat > "$TD/match_brace.naab" << 'EOF'
main {
    let x = 1
    let r = match x {
        1 => {"result": "one"}
        _ => {"result": "other"}
    }
    print(r)
}
EOF
check "match arm { } warns about dict literal" "dict literal" "$TD/match_brace.naab"

# --- Summary ---
echo ""
TOTAL=$((PASS + FAIL))
echo "=== Error Hint Tests: $PASS/$TOTAL passed ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
else
    exit 0
fi
