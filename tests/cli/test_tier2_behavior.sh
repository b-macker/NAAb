#!/usr/bin/env bash
# test_tier2_behavior.sh — Tier 2 behavior fixes
# Tests match arm block detection and polyglot single-line detection
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
PASS=0
FAIL=0
TD=$(mktemp -d "${TMPDIR:-/tmp}/tier2_test_XXXXXX")

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

check_output() {
    local desc="$1"
    local expected="$2"
    local file="$3"
    local output
    output=$("$NAAB" "$file" 2>&1 || true)
    if echo "$output" | grep -q "$expected"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "    Expected: $expected"
        echo "    Got: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Tier 2 Behavior Tests ==="
echo ""

# --- Match arm block detection ---
echo "--- Match Arm Block Detection ---"

cat > "$TD/match_block_let.naab" << 'EOF'
main {
    let x = 1
    let r = match x {
        1 => { let y = 2; y }
        _ => 0
    }
}
EOF
check "match => { let ... } gives clear error" "expressions, not blocks" "$TD/match_block_let.naab"

cat > "$TD/match_block_if.naab" << 'EOF'
main {
    let x = 1
    let r = match x {
        1 => { if true { 1 } }
        _ => 0
    }
}
EOF
check "match => { if ... } gives clear error" "expressions, not blocks" "$TD/match_block_if.naab"

cat > "$TD/match_block_for.naab" << 'EOF'
main {
    let x = 1
    let r = match x {
        1 => { for i in [1] { print(i) } }
        _ => 0
    }
}
EOF
check "match => { for ... } gives clear error" "expressions, not blocks" "$TD/match_block_for.naab"

cat > "$TD/match_block_return.naab" << 'EOF'
main {
    let x = 1
    let r = match x {
        1 => { return 42 }
        _ => 0
    }
}
EOF
check "match => { return ... } gives clear error" "expressions, not blocks" "$TD/match_block_return.naab"

# Dict literal in match arm should still work
cat > "$TD/match_dict.naab" << 'EOF'
main {
    let x = 1
    let r = match x {
        1 => {"result": "one"}
        _ => {"result": "other"}
    }
    print(r)
}
EOF
check_output "match => {dict} still works" "result" "$TD/match_dict.naab"

# Simple expression match arm
cat > "$TD/match_expr.naab" << 'EOF'
main {
    let x = 2
    let r = match x {
        1 => "one"
        2 => "two"
        _ => "other"
    }
    print(r)
}
EOF
check_output "match => expr still works" "two" "$TD/match_expr.naab"

# --- Single-line polyglot detection ---
echo ""
echo "--- Single-Line Polyglot Detection ---"

echo 'main { let x = <<python print(42) >> }' > "$TD/single_line.naab"
check "single-line <<python ... >> gives error" "not supported" "$TD/single_line.naab"

cat > "$TD/multi_line.naab" << 'EOF'
main {
    let x = <<python
print(42)
>>
    print(x)
}
EOF
check_output "multi-line polyglot still works" "42" "$TD/multi_line.naab"

# --- Summary ---
echo ""
TOTAL=$((PASS + FAIL))
echo "=== Tier 2 Tests: $PASS/$TOTAL passed ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
else
    exit 0
fi
