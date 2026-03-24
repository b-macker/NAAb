#!/bin/bash
# Integration test for naab-lang init governance generator
# Tests: presets, section completeness, flags, overwrite protection

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0
TEST_DIR="${HOME}/.naab_init_test_$$"
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

echo "=== NAAb Init Governance Tests ==="
echo ""

# --- Test 1: Non-interactive preset creates valid JSON ---
echo "Test 1: Non-interactive standard preset"
cd "$TEST_DIR" && rm -f govern.json naab.toml
echo "" | "$NAAB_BIN" init --governance-preset standard --taint --force > /dev/null 2>&1
check "govern.json created" "[ -f govern.json ]"
check "naab.toml created" "[ -f naab.toml ]"
check "Valid JSON" "python3 -c 'import json; json.load(open(\"govern.json\"))' 2>/dev/null"

# --- Test 2: All 4 presets generate valid JSON ---
echo ""
echo "Test 2: All 4 presets"
for preset in relaxed standard strict paranoid; do
    cd "$TEST_DIR" && rm -f govern.json
    echo "" | "$NAAB_BIN" init --governance --governance-preset "$preset" --force > /dev/null 2>&1
    check "$preset preset valid JSON" "python3 -c 'import json; json.load(open(\"govern.json\"))' 2>/dev/null"
done

# --- Test 3: Section completeness ---
echo ""
echo "Test 3: Section completeness"
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset standard --taint --force > /dev/null 2>&1

# Check all required top-level keys
for key in version mode languages capabilities taint_tracking limits requirements restrictions code_quality contracts baselines custom_rules scopes output audit meta hooks polyglot polyglot_optimization scanner project_context; do
    check "Has top-level '$key'" "python3 -c 'import json; d=json.load(open(\"govern.json\")); assert \"$key\" in d' 2>/dev/null"
done

# --- Test 4: capabilities has 7 sub-sections ---
echo ""
echo "Test 4: Sub-section counts"
check "capabilities has 7 sub-sections" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert len(d[\"capabilities\"]) == 7, f\"got {len(d[\"capabilities\"])}\"
' 2>/dev/null"
check "code_quality has 23 sub-sections" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert len(d[\"code_quality\"]) == 23, f\"got {len(d[\"code_quality\"])}\"
' 2>/dev/null"
check "restrictions has 10 sub-sections" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert len(d[\"restrictions\"]) == 10, f\"got {len(d[\"restrictions\"])}\"
' 2>/dev/null"
check "requirements has 7 sub-sections" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert len(d[\"requirements\"]) == 7, f\"got {len(d[\"requirements\"])}\"
' 2>/dev/null"
check "limits has 6 sub-sections" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert len(d[\"limits\"]) == 6, f\"got {len(d[\"limits\"])}\"
' 2>/dev/null"
check "meta has 5 sub-sections" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert len(d[\"meta\"]) == 5, f\"got {len(d[\"meta\"])}\"
' 2>/dev/null"
check "hooks has 5 sub-sections" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert len(d[\"hooks\"]) == 5, f\"got {len(d[\"hooks\"])}\"
' 2>/dev/null"
check "polyglot has 5 sub-sections" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert len(d[\"polyglot\"]) == 5, f\"got {len(d[\"polyglot\"])}\"
' 2>/dev/null"

# --- Test 5: Language selection ---
echo ""
echo "Test 5: Language selection"
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset standard --languages python,shell,go --force > /dev/null 2>&1
check "languages.allowed has 3" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"languages\"][\"allowed\"] == [\"python\", \"shell\", \"go\"]
' 2>/dev/null"
check "per_language has go" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert \"go\" in d[\"languages\"][\"per_language\"]
' 2>/dev/null"
check "scanner has go lang_rules" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert \"go\" in d[\"scanner\"][\"lang_rules\"]
' 2>/dev/null"

# --- Test 6: Taint flag ---
echo ""
echo "Test 6: Taint flag"
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset standard --taint --force > /dev/null 2>&1
check "taint_tracking.enabled is true" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"taint_tracking\"][\"enabled\"] == True
' 2>/dev/null"

# Without --taint
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset standard --force > /dev/null 2>&1
check "taint disabled without --taint" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"taint_tracking\"][\"enabled\"] == False
' 2>/dev/null"

# --- Test 7: No overwrite without --force ---
echo ""
echo "Test 7: Overwrite protection"
cd "$TEST_DIR" && rm -f govern.json
echo "existing" > govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset standard > /dev/null 2>&1
check "Existing file not overwritten" "[ \"\$(cat govern.json)\" = 'existing' ]"

# --- Test 8: --force overwrites ---
echo "" | "$NAAB_BIN" init --governance --governance-preset standard --force > /dev/null 2>&1
check "File overwritten with --force" "python3 -c 'import json; json.load(open(\"govern.json\"))' 2>/dev/null"

# --- Test 9: --no-governance skips govern.json ---
echo ""
echo "Test 9: --no-governance flag"
cd "$TEST_DIR" && rm -f govern.json naab.toml
echo "" | "$NAAB_BIN" init --no-governance > /dev/null 2>&1
check "naab.toml created" "[ -f naab.toml ]"
check "govern.json NOT created" "[ ! -f govern.json ]"

# --- Test 10: --governance skips naab.toml ---
echo ""
echo "Test 10: --governance only flag"
cd "$TEST_DIR" && rm -f govern.json naab.toml
echo "" | "$NAAB_BIN" init --governance --governance-preset standard --force > /dev/null 2>&1
check "govern.json created" "[ -f govern.json ]"
check "naab.toml NOT created" "[ ! -f naab.toml ]"

# --- Test 11: Preset differences ---
echo ""
echo "Test 11: Preset level differences"
# Paranoid should have hard levels
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset paranoid --force > /dev/null 2>&1
check "paranoid: no_secrets is hard" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"code_quality\"][\"no_secrets\"][\"level\"] == \"hard\"
' 2>/dev/null"
check "paranoid: audit level is detailed" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"audit\"][\"level\"] == \"detailed\"
' 2>/dev/null"

# Relaxed should have advisory levels
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset relaxed --force > /dev/null 2>&1
check "relaxed: no_secrets is advisory" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"code_quality\"][\"no_secrets\"][\"level\"] == \"advisory\"
' 2>/dev/null"
check "relaxed: audit level is basic" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"audit\"][\"level\"] == \"basic\"
' 2>/dev/null"

# --- Test 12: Generated govern.json loads without errors ---
echo ""
echo "Test 12: Governance engine loads generated config"
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset standard --force > /dev/null 2>&1
cat > test_load.naab << 'NAAB'
fn compute_stuff(x) {
    let result = x * 2
    if result > 100 {
        result = 100
    }
    for i in 0..5 {
        result = result + i
    }
    return result
}
main {
    let val = compute_stuff(42)
    print(val)
}
NAAB
OUTPUT=$("$NAAB_BIN" test_load.naab 2>&1 || true)
check "No config load error" "! echo '$OUTPUT' | grep -q 'Failed to load'"

# --- Summary ---
echo ""
echo "=== Results ==="
TOTAL=$((PASS + FAIL))
echo "  $PASS/$TOTAL passed"

if [ $FAIL -gt 0 ]; then
    echo "  $FAIL FAILED"
    exit 1
fi

echo "  All tests passed!"
