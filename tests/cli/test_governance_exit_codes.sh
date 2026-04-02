#!/bin/bash
# Tests for governance exit codes 2 (quality gate) and 3 (HARD block)
# Exit codes: 0=success 1=runtime 2=quality-gate 3=HARD-block 4=config-error

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0

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

# Temp directory (Termux: no /tmp)
TEST_DIR="${HOME}/.naab_gov_exit_test_$$"
mkdir -p "$TEST_DIR"
cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

echo "=== Governance Exit Code Tests ==="
echo ""

# ============================================================================
# Test 1: Exit code 0 — clean script, no govern.json → success
# ============================================================================
CLEAN_DIR="${TEST_DIR}/clean"
mkdir -p "$CLEAN_DIR"
cat > "${CLEAN_DIR}/ok.naab" << 'NAAB'
main { print("ok") }
NAAB

EXIT_CODE=0
"$NAAB_BIN" "${CLEAN_DIR}/ok.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "Exit 0: clean script without governance" '[ "$EXIT_CODE" = "0" ]'

# ============================================================================
# Test 2: Exit code 1 — runtime throw (not governance)
# ============================================================================
RUNTIME_DIR="${TEST_DIR}/runtime"
mkdir -p "$RUNTIME_DIR"
cat > "${RUNTIME_DIR}/throw.naab" << 'NAAB'
main { throw "runtime error" }
NAAB

EXIT_CODE=0
"$NAAB_BIN" "${RUNTIME_DIR}/throw.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "Exit 1: runtime throw without governance" '[ "$EXIT_CODE" = "1" ]'

# ============================================================================
# Test 3: Exit code 3 — HARD governance block (blocked language)
# ============================================================================
HARD_DIR="${TEST_DIR}/hard_block"
mkdir -p "$HARD_DIR"

# govern.json blocks shell language (HARD by default)
cat > "${HARD_DIR}/govern.json" << 'EOF'
{
    "version": "3.0",
    "mode": "enforce",
    "languages": {
        "blocked": ["shell"]
    }
}
EOF

# Script uses shell polyglot block — governance HARD-blocks it
cat > "${HARD_DIR}/hard_block.naab" << 'NAAB'
main {
    let r = <<shell
echo "this is blocked"
>>
    print(r)
}
NAAB

EXIT_CODE=0
"$NAAB_BIN" "${HARD_DIR}/hard_block.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "Exit 3: HARD governance block (blocked language)" '[ "$EXIT_CODE" = "3" ]'

# ============================================================================
# Test 4: Exit code 3 does NOT occur for permitted language
# ============================================================================
PERMIT_DIR="${TEST_DIR}/permit"
mkdir -p "$PERMIT_DIR"

cat > "${PERMIT_DIR}/govern.json" << 'EOF'
{
    "version": "3.0",
    "mode": "enforce",
    "languages": {
        "blocked": ["python"]
    }
}
EOF

# Script uses no polyglot at all — no governance violation
cat > "${PERMIT_DIR}/ok.naab" << 'NAAB'
main { print("permitted") }
NAAB

EXIT_CODE=0
"$NAAB_BIN" "${PERMIT_DIR}/ok.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "Exit 0: governance loaded but no violation → still 0" '[ "$EXIT_CODE" = "0" ]'

# ============================================================================
# Test 5: Exit code 2 — quality gate failure
# ============================================================================
QG_DIR="${TEST_DIR}/quality_gate"
mkdir -p "$QG_DIR"

# govern.json: custom advisory rule + quality gate that fails when any advisory fires
cat > "${QG_DIR}/govern.json" << 'EOF'
{
    "version": "3.0",
    "mode": "enforce",
    "custom_rules": [
        {
            "id": "QGATE-01",
            "name": "test_advisory_marker",
            "pattern": "QUALITY_GATE_TEST_MARKER",
            "level": "advisory",
            "enabled": true,
            "message": "Quality gate test marker detected"
        }
    ],
    "quality_gate": {
        "enabled": true,
        "conditions": [
            {
                "metric": "advisory_violations",
                "operator": ">",
                "threshold": 0
            }
        ]
    }
}
EOF

# Script with shell block containing the marker — advisory rule fires, script completes,
# then quality gate evaluates and fails → exit 2
cat > "${QG_DIR}/qgate.naab" << 'NAAB'
main {
    let r = <<shell
echo "QUALITY_GATE_TEST_MARKER"
>>
    print("done")
}
NAAB

EXIT_CODE=0
"$NAAB_BIN" "${QG_DIR}/qgate.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "Exit 2: quality gate failure (advisory_violations > 0)" '[ "$EXIT_CODE" = "2" ]'

# ============================================================================
# Test 6: Exit code 4 — invalid sandbox level (config error)
# ============================================================================
EXIT_CODE=0
"$NAAB_BIN" "${CLEAN_DIR}/ok.naab" --sandbox-level bogus > /dev/null 2>&1 || EXIT_CODE=$?
check "Exit 4: invalid sandbox-level config error" '[ "$EXIT_CODE" = "4" ]'

# ============================================================================
echo ""
echo "Governance exit code tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
