#!/bin/bash
# Test: return_keys_non_empty and return_keys_non_null contract enforcement
# Verifies that governance blocks functions returning dicts with null/empty values for required keys.
set -e

NAAB="$(dirname "$0")/../../build/naab-lang"
WORK_DIR="$(mktemp -d)"
trap "rm -rf $WORK_DIR" EXIT

PASS=0
TOTAL=0

# --- T1: return_keys_non_empty blocks empty array value ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/govern.json" <<'EOF'
{
    "mode": "enforce",
    "contracts": {
        "level": "hard",
        "functions": {
            "process_data": {
                "return_type": "dict",
                "return_keys": ["items", "count"],
                "return_keys_non_empty": true
            }
        }
    }
}
EOF
cat > "$WORK_DIR/test.naab" <<'EOF'
fn process_data() {
    return {"items": [], "count": 0}
}
main {
    let r = process_data()
    print("done")
}
EOF
if ! "$NAAB" "$WORK_DIR/test.naab" 2>&1 | grep -q "return_keys_non_empty"; then
    echo "  FAIL: T1: return_keys_non_empty should block empty array"
else
    echo "  PASS: T1: return_keys_non_empty blocks empty array value"
    PASS=$((PASS+1))
fi

# --- T2: return_keys_non_empty blocks null value ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/test2.naab" <<'EOF'
fn process_data() {
    return {"items": null, "count": 5}
}
main {
    let r = process_data()
    print("done")
}
EOF
if ! "$NAAB" "$WORK_DIR/test2.naab" 2>&1 | grep -q "return_keys_non_empty"; then
    echo "  FAIL: T2: return_keys_non_empty should block null value"
else
    echo "  PASS: T2: return_keys_non_empty blocks null value"
    PASS=$((PASS+1))
fi

# --- T3: return_keys_non_empty blocks empty dict value ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/test3.naab" <<'EOF'
fn process_data() {
    return {"items": {"nested": true}, "count": {}}
}
main {
    let r = process_data()
    print("done")
}
EOF
if ! "$NAAB" "$WORK_DIR/test3.naab" 2>&1 | grep -q "return_keys_non_empty"; then
    echo "  FAIL: T3: return_keys_non_empty should block empty dict"
else
    echo "  PASS: T3: return_keys_non_empty blocks empty dict value"
    PASS=$((PASS+1))
fi

# --- T4: return_keys_non_empty passes with substantive values ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/test4.naab" <<'EOF'
fn process_data() {
    return {"items": [1, 2, 3], "count": 3}
}
main {
    let r = process_data()
    print("done")
}
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/test4.naab" 2>&1)
if echo "$OUTPUT" | grep -q "return_keys_non_empty"; then
    echo "  FAIL: T4: return_keys_non_empty should pass with real values"
else
    echo "  PASS: T4: return_keys_non_empty passes with substantive values"
    PASS=$((PASS+1))
fi

# --- T5: return_keys_non_null blocks null but allows empty array ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/govern.json" <<'EOF'
{
    "mode": "enforce",
    "contracts": {
        "level": "hard",
        "functions": {
            "get_result": {
                "return_type": "dict",
                "return_keys": ["data", "errors"],
                "return_keys_non_null": true
            }
        }
    }
}
EOF
cat > "$WORK_DIR/test5.naab" <<'EOF'
fn get_result() {
    return {"data": [], "errors": []}
}
main {
    let r = get_result()
    print("done")
}
EOF
# non_null should allow empty arrays (they're not null)
OUTPUT=$("$NAAB" "$WORK_DIR/test5.naab" 2>&1)
if echo "$OUTPUT" | grep -q "return_keys_non_null"; then
    echo "  FAIL: T5: return_keys_non_null should allow empty array (not null)"
else
    echo "  PASS: T5: return_keys_non_null allows empty array (not null)"
    PASS=$((PASS+1))
fi

echo "  test_return_keys_non_empty.sh: $PASS/$TOTAL PASSED"
