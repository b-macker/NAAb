#!/bin/bash
# Regression tests for governance decision rationale
# Validates that rationale text from govern.json flows into all report formats
# and the dashboard output.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0
TEST_DIR="${HOME}/.naab_rationale_test_$$"
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

echo "=== NAAb Rationale Report Tests ==="
echo ""

MARKER="TEST_RATIONALE_MARKER_7x9q"

# --- Test 1: Rationale appears in all report formats ---
echo "--- Rationale in Reports ---"

SARIF_OUT="$TEST_DIR/test.sarif"
JUNIT_OUT="$TEST_DIR/test.xml"
JSON_OUT="$TEST_DIR/test.json"

cat > "$TEST_DIR/govern.json" <<GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "code_quality": {
    "no_placeholders": {
      "enabled": true,
      "level": "soft",
      "rationale": "$MARKER"
    }
  }
}
GOVEOF

# Use a polyglot block to trigger no_placeholders check at runtime.
# --governance-override allows soft blocks to complete so reports are generated.
cat > "$TEST_DIR/rationale_test.naab" <<'NAABEOF'
main {
    let x = <<python
# TODO: fix this placeholder
result = 42
>>
    print(x)
}
NAABEOF

# Run with all report flags + dashboard + override (soft blocks continue)
"$NAAB_BIN" "$TEST_DIR/rationale_test.naab" \
    --governance-sarif "$SARIF_OUT" \
    --governance-junit "$JUNIT_OUT" \
    --governance-report "$JSON_OUT" \
    --governance-dashboard \
    --governance-override \
    > "$TEST_DIR/stdout.txt" 2> "$TEST_DIR/stderr.txt" || true

check "JSON report exists" "[ -s '$JSON_OUT' ]"

check "JSON report contains rationale" \
    "grep -q '$MARKER' '$JSON_OUT'"

check "JUnit report contains rationale" \
    "grep -q '$MARKER' '$JUNIT_OUT'"

check "SARIF report contains rationale" \
    "grep -q '$MARKER' '$SARIF_OUT'"

check "Dashboard stderr contains rationale" \
    "grep -q '$MARKER' '$TEST_DIR/stderr.txt'"

# --- Test 2: Backward compat — no rationale fields ---
echo ""
echo "--- No-Rationale Backward Compat ---"

NORATIONAL_DIR="$TEST_DIR/no_rationale"
mkdir -p "$NORATIONAL_DIR"

cat > "$NORATIONAL_DIR/govern.json" <<'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "code_quality": {
    "no_placeholders": {
      "enabled": true,
      "level": "soft"
    }
  }
}
GOVEOF

# Same polyglot placeholder trigger, with override so report is written
cat > "$NORATIONAL_DIR/compat_test.naab" <<'NAABEOF'
main {
    let x = <<python
# TODO: placeholder
result = 1
>>
    print(x)
}
NAABEOF

COMPAT_JSON="$NORATIONAL_DIR/report.json"
"$NAAB_BIN" "$NORATIONAL_DIR/compat_test.naab" \
    --governance-report "$COMPAT_JSON" \
    --governance-override \
    > /dev/null 2>&1 || true

check "No-rationale govern.json produces report" \
    "[ -s '$COMPAT_JSON' ]"

check "No-rationale report does not contain marker" \
    "! grep -q '$MARKER' '$COMPAT_JSON'"

# --- Results ---
echo ""
echo "=== Results: $PASS/$((PASS + FAIL)) passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
