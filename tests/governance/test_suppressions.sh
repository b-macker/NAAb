#!/bin/bash
# Test: Governance pattern suppressions for incomplete_logic
# Verifies that suppressions in govern.json prevent false positives per-file
set -e

NAAB="$(dirname "$0")/../../build/naab-lang"
WORK_DIR="$(mktemp -d)"
trap "rm -rf $WORK_DIR" EXIT

PASS=0
TOTAL=0

# Code that triggers error_sentinel pattern
SENTINEL_CODE='fn validate_id(id) {
    if id == null {
        return "invalid-id"
    }
    return id
}
main {
    let r = validate_id("abc")
    print(r)
}'

# --- T1: Without suppression, violation fires ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/govern.json" <<'EOF'
{
    "mode": "enforce",
    "code_quality": {
        "no_incomplete_logic": "soft"
    }
}
EOF
echo "$SENTINEL_CODE" > "$WORK_DIR/validators.naab"
if "$NAAB" "$WORK_DIR/validators.naab" 2>&1 | grep -qi "incomplete logic"; then
    echo "  PASS: T1: Without suppression, violation fires"
    PASS=$((PASS+1))
else
    echo "  FAIL: T1: Violation should fire without suppression"
fi

# --- T2: With suppression for this file, violation is suppressed ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/govern.json" <<'EOF'
{
    "mode": "enforce",
    "code_quality": {
        "no_incomplete_logic": {
            "enabled": true,
            "level": "SOFT",
            "suppressions": ["validators.naab"]
        }
    }
}
EOF
if "$NAAB" "$WORK_DIR/validators.naab" 2>&1 | grep -qi "incomplete logic"; then
    echo "  FAIL: T2: Suppression should prevent violation"
else
    echo "  PASS: T2: Suppression prevents violation for matching file"
    PASS=$((PASS+1))
fi

# --- T3: Suppression does NOT apply to other files ---
TOTAL=$((TOTAL+1))
echo "$SENTINEL_CODE" > "$WORK_DIR/other.naab"
if "$NAAB" "$WORK_DIR/other.naab" 2>&1 | grep -qi "incomplete logic"; then
    echo "  PASS: T3: Suppression does not apply to non-matching file"
    PASS=$((PASS+1))
else
    echo "  FAIL: T3: Other files should still get violations"
fi

# --- T4: Wildcard suppression works ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/govern.json" <<'EOF'
{
    "mode": "enforce",
    "code_quality": {
        "no_incomplete_logic": {
            "enabled": true,
            "level": "SOFT",
            "suppressions": ["*.naab"]
        }
    }
}
EOF
if "$NAAB" "$WORK_DIR/other.naab" 2>&1 | grep -qi "incomplete logic"; then
    echo "  FAIL: T4: Wildcard suppression should prevent violation"
else
    echo "  PASS: T4: Wildcard suppression works"
    PASS=$((PASS+1))
fi

echo "  test_suppressions.sh: $PASS/$TOTAL PASSED"
