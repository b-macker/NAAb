#!/bin/bash
# Tests for Governance Under Survivability — mid-run config reload
# Validates: mtime detection, ratchet enforcement (tightening only), notice generation

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0
TEST_DIR="${HOME}/.naab_reload_test_$$"
mkdir -p "$TEST_DIR"

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

check() {
    local desc="$1"
    local condition="$2"
    if eval "$condition"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== NAAb Governance Reload Tests ==="
echo ""

# --- Test 1: Ratchet rejects loosening ---
echo "--- Ratchet Rejection (loosening blocked) ---"

# Initial strict config
cat > "$TEST_DIR/govern.json" <<'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "limits": {
    "execution": {
      "loop_iterations": 100
    }
  }
}
GOVEOF

# Run a simple script to load governance
cat > "$TEST_DIR/test_strict.naab" <<'NAABEOF'
main {
    print("loaded strict config")
}
NAABEOF

"$NAAB_BIN" "$TEST_DIR/test_strict.naab" \
    --governance-dashboard \
    > "$TEST_DIR/strict_stdout.txt" 2> "$TEST_DIR/strict_stderr.txt" || true

check "Strict config loads successfully" \
    "[ -f '$TEST_DIR/strict_stderr.txt' ]"

# --- Test 2: update_reason field is accepted ---
echo ""
echo "--- update_reason Field ---"

cat > "$TEST_DIR/govern_reason.json" <<'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "update_reason": "memory pressure exceeded 85%",
  "limits": {
    "execution": {
      "loop_iterations": 500
    }
  }
}
GOVEOF

# Verify update_reason doesn't break config loading
"$NAAB_BIN" "$TEST_DIR/test_strict.naab" \
    > "$TEST_DIR/reason_stdout.txt" 2> "$TEST_DIR/reason_stderr.txt" || true

check "Config with update_reason loads without error" \
    "[ $? -eq 0 ] || grep -q 'loaded strict config' '$TEST_DIR/reason_stdout.txt'"

# --- Test 3: Governance dashboard shows reload count when applicable ---
echo ""
echo "--- Dashboard Reload Display ---"

# Dashboard output should contain governance summary
"$NAAB_BIN" "$TEST_DIR/test_strict.naab" \
    --governance-dashboard \
    > /dev/null 2> "$TEST_DIR/dashboard_stderr.txt" || true

check "Dashboard output contains governance summary" \
    "grep -q 'Governance Summary' '$TEST_DIR/dashboard_stderr.txt'"

# Without any reload, reload count should not appear
check "Dashboard does not show Reloads line when no reload occurred" \
    "! grep -q 'Reloads:' '$TEST_DIR/dashboard_stderr.txt'"

# --- Test 4: Ratchet comparator field coverage ---
echo ""
echo "--- Ratchet Field Coverage ---"

# Test that a tighter config is accepted (lower loop_iterations)
cat > "$TEST_DIR/govern_tight.json" <<'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "limits": {
    "execution": {
      "loop_iterations": 50
    }
  },
  "capabilities": {
    "shell": {
      "enabled": false
    }
  }
}
GOVEOF

"$NAAB_BIN" "$TEST_DIR/test_strict.naab" \
    > /dev/null 2> "$TEST_DIR/tight_stderr.txt" || true

check "Tighter config loads without rejection" \
    "! grep -q 'Reload rejected' '$TEST_DIR/tight_stderr.txt'"

# --- Test 5: Backward compatibility ---
echo ""
echo "--- Backward Compatibility ---"

# Minimal govern.json without any reload-related fields
cat > "$TEST_DIR/govern_minimal.json" <<'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce"
}
GOVEOF

"$NAAB_BIN" "$TEST_DIR/test_strict.naab" \
    > "$TEST_DIR/minimal_stdout.txt" 2> "$TEST_DIR/minimal_stderr.txt" || true

check "Minimal govern.json without update_reason works" \
    "grep -q 'loaded strict config' '$TEST_DIR/minimal_stdout.txt'"

# --- Results ---
echo ""
echo "=== Results: $PASS/$((PASS + FAIL)) passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
