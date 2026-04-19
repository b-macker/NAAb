#!/usr/bin/env bash
# test_tier4_design.sh — Tier 4 language design tests
# Tests || value return, optional new, use "./path" in VM
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
PASS=0
FAIL=0
TD=$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/tier4_test_XXXXXX")

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
    if echo "$output" | grep -qF "$expected"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        echo "    Expected: $expected"
        echo "    Got: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Tier 4 Language Design Tests ==="
echo ""

# --- Phase 1: || returns values (JS-style) ---
echo "--- || Value Return ---"

cat > "$TD/or_null.naab" << 'EOF'
main {
    let x = null
    let y = x || "default"
    print(y)
}
EOF
check_output "null || 'default' returns 'default'" "default" "$TD/or_null.naab"

cat > "$TD/or_truthy.naab" << 'EOF'
main {
    let x = "hello"
    let y = x || "world"
    print(y)
}
EOF
check_output "'hello' || 'world' returns 'hello'" "hello" "$TD/or_truthy.naab"

cat > "$TD/or_false.naab" << 'EOF'
main {
    let x = false
    let y = x || 42
    print(y)
}
EOF
check_output "false || 42 returns 42" "42" "$TD/or_false.naab"

cat > "$TD/or_zero.naab" << 'EOF'
main {
    let x = 0
    let y = x || "fallback"
    print(y)
}
EOF
check_output "0 || 'fallback' returns 'fallback'" "fallback" "$TD/or_zero.naab"

# --- Phase 1b: && returns values ---
echo ""
echo "--- && Value Return ---"

cat > "$TD/and_both.naab" << 'EOF'
main {
    let x = 1 && 2
    print(x)
}
EOF
check_output "1 && 2 returns 2" "2" "$TD/and_both.naab"

cat > "$TD/and_null.naab" << 'EOF'
main {
    let x = null && "hello"
    print(x)
}
EOF
check_output "null && 'hello' returns null" "null" "$TD/and_null.naab"

cat > "$TD/and_false.naab" << 'EOF'
main {
    let x = false && 42
    print(x)
}
EOF
check_output "false && 42 returns false" "false" "$TD/and_false.naab"

# --- Boolean context still works ---
echo ""
echo "--- Boolean Context Backward Compat ---"

cat > "$TD/or_if.naab" << 'EOF'
main {
    let a = false
    let b = true
    if a || b {
        print("works")
    }
}
EOF
check_output "if a || b still works" "works" "$TD/or_if.naab"

cat > "$TD/and_if.naab" << 'EOF'
main {
    let a = true
    let b = true
    if a && b {
        print("works")
    }
}
EOF
check_output "if a && b still works" "works" "$TD/and_if.naab"

# --- VM parity ---
echo ""
echo "--- VM || Parity ---"

check_output "VM: null || 'default'" "default" "$TD/or_null.naab"
check_output "VM: 1 && 2" "2" "$TD/and_both.naab"

echo ""
echo "--- Tree-walker || Parity ---"
check_output "TW: null || 'default'" "default" "$TD/or_null.naab" "--tree-walk"
check_output "TW: 1 && 2" "2" "$TD/and_both.naab" "--tree-walk"

# --- Phase 2: new optional for structs ---
echo ""
echo "--- Optional new ---"

cat > "$TD/struct_no_new.naab" << 'EOF'
struct Point {
    x: int
    y: int
}
main {
    let p = Point { x: 10, y: 20 }
    print(p.x)
    print(p.y)
}
EOF
check_output "struct without new — x" "10" "$TD/struct_no_new.naab"
check_output "struct without new — y" "20" "$TD/struct_no_new.naab"

cat > "$TD/struct_with_new.naab" << 'EOF'
struct Point {
    x: int
    y: int
}
main {
    let p = new Point { x: 10, y: 20 }
    print(p.x)
    print(p.y)
}
EOF
check_output "struct with new still works — x" "10" "$TD/struct_with_new.naab"
check_output "struct with new still works — y" "20" "$TD/struct_with_new.naab"

# --- Phase 3: use "./path" in VM ---
echo ""
echo "--- use './path' in VM ---"

cat > "$TD/mymod.naab" << 'EOF'
fn greet(name) {
    return "Hello, " + name
}
EOF

cat > "$TD/use_path.naab" << 'EOF'
use "./mymod" as mymod
main {
    print(mymod.greet("World"))
}
EOF
check_output "use './path' works in VM" "Hello, World" "$TD/use_path.naab"

# use without as — auto-alias from filename
cat > "$TD/use_no_alias.naab" << 'EOF'
use "./mymod"
main {
    print(mymod.greet("Auto"))
}
EOF
check_output "use './path' without as — auto-alias" "Hello, Auto" "$TD/use_no_alias.naab"

# --- and/or keyword value return ---
echo ""
echo "--- and/or Keyword Value Return ---"

cat > "$TD/or_keyword.naab" << 'EOF'
main {
    let x = null or "default"
    print(x)
}
EOF
check_output "or keyword returns value" "default" "$TD/or_keyword.naab"

cat > "$TD/and_keyword.naab" << 'EOF'
main {
    let x = 1 and 2
    print(x)
}
EOF
check_output "and keyword returns value" "2" "$TD/and_keyword.naab"

# --- Summary ---
echo ""
TOTAL=$((PASS + FAIL))
echo "=== Tier 4 Tests: $PASS/$TOTAL passed ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
else
    exit 0
fi
