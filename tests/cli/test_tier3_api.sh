#!/usr/bin/env bash
# test_tier3_api.sh — Tier 3 API consistency tests
# Tests env.set() alias, VM method parity, and cross-engine consistency
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
PASS=0
FAIL=0
TD=$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/tier3_test_XXXXXX")

cleanup() { rm -rf "$TD"; }
trap cleanup EXIT

echo '{"version":"5.0","mode":"off"}' > "$TD/govern.json"

check_output() {
    local desc="$1"
    local expected="$2"
    local file="$3"
    local flags="${4:-}"
    local output
    output=$("$NAAB" $flags "$file" 2>&1 || true)
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

check_error() {
    local desc="$1"
    local pattern="$2"
    local file="$3"
    local flags="${4:-}"
    local output
    output=$("$NAAB" $flags "$file" 2>&1 || true)
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

echo "=== Tier 3 API Consistency Tests ==="
echo ""

# --- Phase 1: env.set() alias ---
echo "--- env.set() Alias ---"

cat > "$TD/env_set.naab" << 'EOF'
use env
main {
    env.set("NAAB_TEST_T3_KEY", "hello_tier3")
    print(env.get("NAAB_TEST_T3_KEY"))
}
EOF
check_output "env.set() works as alias for env.set_var()" "hello_tier3" "$TD/env_set.naab"

cat > "$TD/env_set_var.naab" << 'EOF'
use env
main {
    env.set_var("NAAB_TEST_T3_KEY2", "still_works")
    print(env.get("NAAB_TEST_T3_KEY2"))
}
EOF
check_output "env.set_var() still works" "still_works" "$TD/env_set_var.naab"

# --- Phase 2: VM string methods ---
echo ""
echo "--- VM String Methods ---"

cat > "$TD/str_pad_left.naab" << 'EOF'
main {
    let s = "hi"
    print(s.pad_left(5))
    print(s.pad_left(5, "0"))
}
EOF
check_output "string.pad_left() default" "   hi" "$TD/str_pad_left.naab"
check_output "string.pad_left() with char" "000hi" "$TD/str_pad_left.naab"

cat > "$TD/str_pad_right.naab" << 'EOF'
main {
    let s = "hi"
    let r = s.pad_right(5, ".")
    print(r)
    print(r.length())
}
EOF
check_output "string.pad_right()" "hi..." "$TD/str_pad_right.naab"

cat > "$TD/str_find.naab" << 'EOF'
main {
    let s = "hello world"
    print(s.find("world"))
    print(s.find("xyz"))
}
EOF
check_output "string.find() found" "^6" "$TD/str_find.naab"
check_output "string.find() not found" "^-1" "$TD/str_find.naab"

# --- Phase 3: VM list methods ---
echo ""
echo "--- VM List Methods ---"

cat > "$TD/list_find.naab" << 'EOF'
main {
    let nums = [1, 2, 3, 4, 5]
    let result = nums.find(fn(x) { return x > 3 })
    print(result)
}
EOF
check_output "list.find() with predicate" "^4" "$TD/list_find.naab"

cat > "$TD/list_find_null.naab" << 'EOF'
main {
    let nums = [1, 2, 3]
    let result = nums.find(fn(x) { return x > 10 })
    print(result)
}
EOF
check_output "list.find() returns null when not found" "null" "$TD/list_find_null.naab"

cat > "$TD/list_foreach.naab" << 'EOF'
main {
    let nums = [10, 20, 30]
    nums.forEach(fn(x) { print(x) })
}
EOF
check_output "list.forEach() works" "^10" "$TD/list_foreach.naab"

cat > "$TD/list_for_each.naab" << 'EOF'
main {
    let nums = [10, 20, 30]
    nums.for_each(fn(x) { print(x) })
}
EOF
check_output "list.for_each() alias works" "^10" "$TD/list_for_each.naab"

cat > "$TD/list_insert.naab" << 'EOF'
main {
    let nums = [1, 3, 4]
    nums.insert(1, 2)
    print(nums)
}
EOF
check_output "list.insert() works" "1, 2, 3, 4" "$TD/list_insert.naab"

# --- Phase 4: Tree-walker parity ---
echo ""
echo "--- Tree-Walker Parity ---"

cat > "$TD/tw_insert.naab" << 'EOF'
main {
    let nums = [1, 3, 4]
    nums.insert(1, 2)
    print(nums)
}
EOF
check_output "list.insert() works in tree-walker" "1, 2, 3, 4" "$TD/tw_insert.naab" "--tree-walk"

cat > "$TD/tw_pad.naab" << 'EOF'
main {
    let s = "hi"
    print(s.pad_left(5, "0"))
    print(s.pad_right(5, "."))
}
EOF
check_output "string.pad_left() in tree-walker" "000hi" "$TD/tw_pad.naab" "--tree-walk"
check_output "string.pad_right() in tree-walker" "hi..." "$TD/tw_pad.naab" "--tree-walk"

# --- Summary ---
echo ""
TOTAL=$((PASS + FAIL))
echo "=== Tier 3 Tests: $PASS/$TOTAL passed ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
else
    exit 0
fi
