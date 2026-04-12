#!/usr/bin/env bash
# Security R31 Fix Verification Tests
# V-GOV-023: Native array governance bypass
# V-UB-001:  std::sort strict weak ordering crash
# V-DOS-011: Unbounded string allocation DoS
# V-DOS-012: ReDoS omission in regex stdlib
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
WORK_DIR="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_r31.XXXXXX")"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

cat > "$WORK_DIR/govern.json" << 'JSON'
{ "version": "4.0", "mode": "off" }
JSON

# ── V-GOV-023: Native array governance ─────────────────────────────────────

echo "=== V-GOV-023: Native array governance bypass ==="

SRC_ARRAY="$SCRIPT_DIR/../../src/stdlib/array_impl.cpp"

# Test 1: Source — checkArraySize in push
if grep -q 'checkArraySize.*push' "$SRC_ARRAY" || grep -B2 'push_back.*args\[1\]' "$SRC_ARRAY" | grep -q 'checkArraySize'; then
    pass "V-GOV-023 T1: checkArraySize guard in push"
else
    fail "V-GOV-023 T1: checkArraySize not found in push"
fi

# Test 2: Source — checkArraySize in unshift
if grep -q 'checkArraySize.*unshift' "$SRC_ARRAY" || grep -B2 'insert.*begin.*args\[1\]' "$SRC_ARRAY" | grep -q 'checkArraySize'; then
    pass "V-GOV-023 T2: checkArraySize guard in unshift"
else
    fail "V-GOV-023 T2: checkArraySize not found in unshift"
fi

# ── V-UB-001: Safe sort comparator ────────────────────────────────────────

echo "=== V-UB-001: std::sort strict weak ordering ==="

# Test 3: Sort with bad comparator doesn't crash (returns true for all)
cat > "$WORK_DIR/bad_sort.naab" << 'NAAB'
main {
    let arr = [5, 3, 1, 4, 2]
    try {
        arr.sort(fn(a, b) { return "not a number" })
        print("ERROR or SORTED")
    } catch (e) {
        print("CAUGHT: " + e)
    }
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/bad_sort.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "CAUGHT\|ERROR\|SORTED"; then
    pass "V-UB-001 T3: Bad comparator handled without crash"
else
    fail "V-UB-001 T3: Bad comparator caused unexpected behavior" "$OUTPUT"
fi

# Test 4: Sort with valid comparator still works
cat > "$WORK_DIR/good_sort.naab" << 'NAAB'
main {
    let arr = [5, 3, 1, 4, 2]
    arr.sort(fn(a, b) { return a - b })
    print(arr)
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/good_sort.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -q '\[1, 2, 3, 4, 5\]'; then
    pass "V-UB-001 T4: Valid comparator sort works correctly"
else
    fail "V-UB-001 T4: Valid comparator sort failed" "$OUTPUT"
fi

# ── V-DOS-011: String allocation bounds ───────────────────────────────────

echo "=== V-DOS-011: Unbounded string allocation ==="

# Test 5: string.repeat with huge count is rejected
cat > "$WORK_DIR/repeat_bomb.naab" << 'NAAB'
main {
    try {
        let s = string.repeat("A", 100000000)
        print("ERROR: should have thrown")
    } catch (e) {
        print("CAUGHT: " + e)
    }
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/repeat_bomb.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "maximum\|exceeds\|CAUGHT"; then
    pass "V-DOS-011 T5: string.repeat with huge count rejected"
else
    fail "V-DOS-011 T5: string.repeat NOT rejected" "$OUTPUT"
fi

# Test 6: string.pad_left with huge width is rejected
cat > "$WORK_DIR/pad_bomb.naab" << 'NAAB'
main {
    try {
        let s = string.pad_left("x", 100000000)
        print("ERROR: should have thrown")
    } catch (e) {
        print("CAUGHT: " + e)
    }
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/pad_bomb.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "maximum\|exceeds\|CAUGHT"; then
    pass "V-DOS-011 T6: string.pad_left with huge width rejected"
else
    fail "V-DOS-011 T6: string.pad_left NOT rejected" "$OUTPUT"
fi

# Test 7: Normal repeat still works
cat > "$WORK_DIR/repeat_ok.naab" << 'NAAB'
main {
    let s = string.repeat("ab", 3)
    print(s)
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/repeat_ok.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -q 'ababab'; then
    pass "V-DOS-011 T7: Normal string.repeat works"
else
    fail "V-DOS-011 T7: Normal string.repeat broken" "$OUTPUT"
fi

# ── V-DOS-012: ReDoS in regex stdlib ──────────────────────────────────────

echo "=== V-DOS-012: ReDoS omission in regex stdlib ==="

SRC_REGEX="$SCRIPT_DIR/../../src/stdlib/regex_impl.cpp"

# Test 8: regex.split uses SafeRegex
if grep -A15 'Function 7: split' "$SRC_REGEX" | grep -q 'safe_regex\|SafeRegex'; then
    pass "V-DOS-012 T8: regex.split uses SafeRegex"
else
    fail "V-DOS-012 T8: regex.split does NOT use SafeRegex"
fi

# Test 9: regex.groups uses SafeRegex
if grep -A15 'Function 8: groups' "$SRC_REGEX" | grep -q 'safe_regex\|SafeRegex'; then
    pass "V-DOS-012 T9: regex.groups uses SafeRegex"
else
    fail "V-DOS-012 T9: regex.groups does NOT use SafeRegex"
fi

# Test 10: regex.find_groups uses SafeRegex
if grep -A15 'Function 9: find_groups' "$SRC_REGEX" | grep -q 'safe_regex\|SafeRegex'; then
    pass "V-DOS-012 T10: regex.find_groups uses SafeRegex"
else
    fail "V-DOS-012 T10: regex.find_groups does NOT use SafeRegex"
fi

# Test 11: regex.is_valid uses SafeRegex
if grep -A10 'Function 11: is_valid' "$SRC_REGEX" | grep -q 'safe_regex\|SafeRegex\|analyzePattern'; then
    pass "V-DOS-012 T11: regex.is_valid uses SafeRegex"
else
    fail "V-DOS-012 T11: regex.is_valid does NOT use SafeRegex"
fi

# Test 12: regex.compile_pattern uses SafeRegex
if grep -A10 'Function 12: compile_pattern' "$SRC_REGEX" | grep -q 'safe_regex\|SafeRegex\|analyzePattern'; then
    pass "V-DOS-012 T12: regex.compile_pattern uses SafeRegex"
else
    fail "V-DOS-012 T12: regex.compile_pattern does NOT use SafeRegex"
fi

# Test 13: Only 1 raw std::regex remaining (is_valid syntax check — construction-only, no matching)
RAW_COUNT=$(grep -c 'std::regex re(' "$SRC_REGEX" || true)
if [ "$RAW_COUNT" -le 1 ]; then
    pass "V-DOS-012 T13: At most 1 raw std::regex construction (is_valid syntax check)"
else
    fail "V-DOS-012 T13: $RAW_COUNT raw std::regex constructions remain (expected <=1)"
fi

# Test 14: regex.split runtime test
cat > "$WORK_DIR/regex_split.naab" << 'NAAB'
use regex
main {
    let parts = regex.split("hello world foo", "\\s+")
    print(parts)
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/regex_split.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -q 'hello.*world.*foo'; then
    pass "V-DOS-012 T14: regex.split works correctly"
else
    fail "V-DOS-012 T14: regex.split broken" "$OUTPUT"
fi

# ── Summary ────────────────────────────────────────────────────────────────

echo ""
echo "================================"
echo "R31 Results: $PASS passed, $FAIL failed (of $((PASS+FAIL)) tests)"
echo "================================"

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
