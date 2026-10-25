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
# Argv-based helper: the previous inline f-strings nested double quotes
# inside {len(d["..."])}, which is a SyntaxError on Python < 3.12.
section_count() {
    python3 -c 'import json,sys; d=json.load(open("govern.json")); sys.exit(0 if len(d[sys.argv[1]]) == int(sys.argv[2]) else 1)' "$1" "$2" 2>/dev/null
}
# Counts for the STANDARD preset. Since 2026-10-24 the generator leaves out
# what the engine would not honour: a check the profile turns off (code_quality
# 23 -> 18, restrictions 10 -> 7 at standard; every code_quality/restrictions
# block is enabled by its presence), and settings no check reads (the process
# block, requirements.error_handling and naming_conventions). Test 9 is the
# behavioural half: each preset loads with no such warning and runs Hello World.
check "capabilities has 6 sub-sections"  "section_count capabilities 6"
check "code_quality has 18 sub-sections" "section_count code_quality 18"
check "restrictions has 7 sub-sections"  "section_count restrictions 7"
check "requirements has 5 sub-sections"  "section_count requirements 5"
check "limits has 6 sub-sections"        "section_count limits 6"
check "meta has 5 sub-sections"          "section_count meta 5"
check "hooks has 5 sub-sections"         "section_count hooks 5"
check "polyglot has 5 sub-sections"      "section_count polyglot 5"

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
# The condition is SINGLE-quoted so $OUTPUT is expanded inside the eval, within
# [[ ]] double quotes, instead of being spliced into the condition string. The
# previous form interpolated captured governance output into an eval'd string
# between single quotes, so any apostrophe in a message terminated the quote --
# CONTRA-013's advisory text says "NAAb's standard library" and contains
# "<<python>>", which the shell then read as a heredoc operator. The test failed
# with a bash syntax error while the thing it was checking was fine.
check "No config load error" '[[ "$OUTPUT" != *"Failed to load"* ]]'

# --- Test 10: every preset means what it says ---
# The generated file used to write "level": "off" for checks a profile turns
# off. "off" is not an enforcement level, and where a key sets only a level the
# engine enforces an unknown level at HARD -- so relaxed (30 such checks) and
# standard (14) blocked Hello World on complexity_floor, while strict and
# paranoid ran it. And every code_quality/restrictions block is enabled by its
# presence, so "enabled": false there did nothing. A generated config must load
# with none of the engine's own "this setting does not do what it says"
# warnings, and must run the program the README's Quick Start runs.
echo ""
echo "Test 10: Each preset loads cleanly and runs Hello World"
for preset in relaxed standard strict paranoid; do
    D="$TEST_DIR/p_$preset"; rm -rf "$D"; mkdir -p "$D"
    (cd "$D" && echo "" | "$NAAB_BIN" init --governance --governance-preset "$preset" > /dev/null 2>&1)
    printf 'main {\n    let name = "World"\n    print("Hello, " + name + "!")\n}\n' > "$D/hello.naab"
    HOUT=$(cd "$D" && "$NAAB_BIN" hello.naab 2>&1); HRC=$?
    check "$preset: Hello World runs (exit 0, prints its line)" \
        '[ "$HRC" -eq 0 ] && grep -q "^Hello, World!" <<<"$HOUT"'
    check "$preset: no setting the engine ignores, cannot parse or does not enforce" \
        '! grep -qE "has no effect|unknown enforcement level|not a setting the engine reads|not enforced|no check enforces" <<<"$HOUT"'
done
# Control: the warning probe sees the engine's warnings at all. A config with
# one ignored flag and one unknown level must draw both.
D="$TEST_DIR/p_control"; rm -rf "$D"; mkdir -p "$D"
printf '{"version":"4.0","mode":"enforce","code_quality":{"no_debug_artifacts":{"enabled":false},"complexity_floor":{"level":"off","min_score":10}}}\n' > "$D/govern.json"
printf 'main {\n    let name = "World"\n    print("Hello, " + name + "!")\n}\n' > "$D/hello.naab"
COUT=$(cd "$D" && "$NAAB_BIN" hello.naab 2>&1); CRC=$?
check "control: an ignored flag and an unknown level are both reported, and the level blocks" \
    'grep -q "has no effect" <<<"$COUT" && grep -q "unknown enforcement level" <<<"$COUT" && [ "$CRC" -eq 3 ]'

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
