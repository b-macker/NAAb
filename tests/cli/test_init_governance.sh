#!/bin/bash
# Integration test for naab-lang init governance generator
# Tests: presets, section completeness, flags, overwrite protection
#
# Bundle 19 PR 2 arms (A176, A75):
#   Test 6   every preset but relaxed writes taint tracking and sequence
#            detection on (A75(b)); relaxed only with --taint
#   Test 7b  an existing naab.toml is kept without --force, replaced with it
#   Test 13  --help / -h print the usage and write nothing; an argument init
#            does not know, a flag missing its value and an unknown preset
#            refuse (exit 1) and write nothing; a preset name in another case
#            is accepted (control)
#   Test 14  the summary and the file say what the standard sandbox refuses
#   Test 15  a generated standard project refuses a file.read -> python flow
#            and runs the same block on a literal (control); UNMEASURABLE
#            without embedded Python
#
# Kill table (mutants of src/cli/main.cpp init and src/cli/governance_init.cpp,
# run by hand from a copy of the tree):
#   I1  drop the naab.toml existence check                  -> Test 7b
#   I2  --help handled after the files are written          -> Test 13 (--help)
#   I3  an unknown argument ignored again                   -> Test 13 (unknown)
#   I4  an unknown preset falls back to standard again      -> Test 13 (preset)
#   I5  taint on only with --taint again (presetToConfig)   -> Test 6, Test 15
#   I6  taint on for relaxed too                            -> Test 6 (relaxed)
#   I7  the sandbox note left out of the file               -> Test 14 (file)
#   I8  the sandbox note left out of the summary            -> Test 14 (summary)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${NAAB:-${SCRIPT_DIR}/../../build/naab-lang}"
PASS=0
FAIL=0
UNM=0
TEST_DIR="${HOME}/.naab_init_test_$$"
mkdir -p "$TEST_DIR"
# Generated configs are unsigned: keep a key installed on this machine from
# turning every run into an integrity block.
source "${SCRIPT_DIR}/../helpers/trust_setup.sh"
setup_isolated_trust

cleanup() { rm -rf "$TEST_DIR"; teardown_isolated_trust; }
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
for key in version mode languages capabilities taint_tracking limits requirements restrictions code_quality contracts baselines custom_rules output audit meta hooks polyglot polyglot_optimization scanner project_context; do
    check "Has top-level '$key'" "python3 -c 'import json; d=json.load(open(\"govern.json\")); assert \"$key\" in d' 2>/dev/null"
done
# Bundle 17 (F-26): a top-level "scopes" block is read by nothing, so the test
# and example exemptions init wrote there exempted nothing. Init no longer
# writes one.
check "Has no top-level 'scopes' (read by nothing)" "python3 -c 'import json; d=json.load(open(\"govern.json\")); assert \"scopes\" not in d' 2>/dev/null"

# --- Test 4: sub-section counts ---
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
# Bundle 19 PR 4 (A175(b)): keys the loader never parses left too -- the time
# and memory capabilities, requirements other than main_block, meta's
# schema_validation / config_immutability / feature_flags, and polyglot's
# output / context_isolation / parallel / persistent_runtime blocks
# (tests/governance_v4/template_retired_keys.txt). What is left is named here.
check "capabilities has 4 sub-sections"  "section_count capabilities 4"
check "code_quality has 18 sub-sections" "section_count code_quality 18"
check "restrictions has 7 sub-sections"  "section_count restrictions 7"
check "requirements has 1 sub-section"   "section_count requirements 1"
check "limits has 6 sub-sections"        "section_count limits 6"
check "meta has 2 sub-sections"          "section_count meta 2"
check "hooks has 5 sub-sections"         "section_count hooks 5"
check "polyglot has 1 sub-section"       "section_count polyglot 1"
section_names() {
    python3 -c 'import json,sys; d=json.load(open("govern.json")); sys.exit(0 if sorted(d[sys.argv[1]]) == sorted(sys.argv[2].split(",")) else 1)' "$1" "$2" 2>/dev/null
}
check "capabilities: env_vars, filesystem, network, shell" "section_names capabilities env_vars,filesystem,network,shell"
check "requirements: main_block"                           "section_names requirements main_block"
check "meta: environment, inheritance"                     "section_names meta environment,inheritance"
check "polyglot: variable_binding"                         "section_names polyglot variable_binding"

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

# Without --taint (A75(b), the owner's decision of 2026-11-20): every preset
# but relaxed writes taint tracking and sequence detection on, as plain init
# does; relaxed only with --taint. This arm used to pin "taint disabled
# without --taint" for standard -- the opt-in 7d5cf24c built, reversed.
for preset in standard strict paranoid; do
    cd "$TEST_DIR" && rm -f govern.json
    echo "" | "$NAAB_BIN" init --governance --governance-preset "$preset" --force > /dev/null 2>&1
    check "$preset: taint tracking and sequence detection on without --taint" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"taint_tracking\"][\"enabled\"] == True
assert d[\"behavioral_sequences\"][\"enabled\"] == True
' 2>/dev/null"
done
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset relaxed --force > /dev/null 2>&1
check "relaxed: taint tracking and sequence detection off without --taint" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"taint_tracking\"][\"enabled\"] == False
assert d[\"behavioral_sequences\"][\"enabled\"] == False
' 2>/dev/null"
cd "$TEST_DIR" && rm -f govern.json
echo "" | "$NAAB_BIN" init --governance --governance-preset relaxed --taint --force > /dev/null 2>&1
check "relaxed --taint: both on" "python3 -c '
import json; d=json.load(open(\"govern.json\"))
assert d[\"taint_tracking\"][\"enabled\"] == True
assert d[\"behavioral_sequences\"][\"enabled\"] == True
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

# --- Test 7b: an existing naab.toml is kept without --force (A176) ---
# 7d5cf24c printed "naab.toml already exists (skipped)", but the manifest
# writer opened the file for writing whether or not it existed, so a user's
# naab.toml was replaced by the template on every init.
echo ""
echo "Test 7b: naab.toml overwrite protection"
cd "$TEST_DIR" && rm -f govern.json naab.toml
printf 'name = "mine"\n' > naab.toml
echo "" | "$NAAB_BIN" init --governance-preset standard > /dev/null 2>&1
check "existing naab.toml kept without --force" '[ "$(cat naab.toml)" = "name = \"mine\"" ]'
check "govern.json still written beside the kept naab.toml (control)" '[ -f govern.json ]'
rm -f govern.json
echo "" | "$NAAB_BIN" init --governance-preset standard --force > /dev/null 2>&1
check "--force replaces naab.toml" 'grep -q "^\[package\]" naab.toml'

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

# --- Test 13: init writes nothing it was not asked for (A176) ---
# Each case runs in an empty directory; "writes nothing" means the directory
# is still empty afterwards.
echo ""
echo "Test 13: --help and arguments init does not know"
empty_after() {  # ARGS... -> sets RC, OUT, LEFT (files left in the directory)
    local d; d=$(mktemp -d "$TEST_DIR/a13.XXXXXX")
    OUT=$(cd "$d" && "$NAAB_BIN" init "$@" </dev/null 2>&1); RC=$?
    LEFT=$(ls -A "$d")
}
empty_after --help
check "--help: exit 0, prints the init usage, writes nothing" \
    '[ "$RC" -eq 0 ] && grep -q "Usage: naab-lang init" <<<"$OUT" && [ -z "$LEFT" ]'
empty_after -h
check "-h: exit 0, writes nothing" '[ "$RC" -eq 0 ] && [ -z "$LEFT" ]'
empty_after --preset strict
check "an unknown argument (--preset): exit 1, named, writes nothing" \
    '[ "$RC" -eq 1 ] && grep -q "does not know the argument .--preset." <<<"$OUT" && [ -z "$LEFT" ]'
empty_after --governance-preset Strickt
check "an unknown preset: exit 1, writes nothing (it used to write standard)" \
    '[ "$RC" -eq 1 ] && grep -q "unknown governance preset" <<<"$OUT" && [ -z "$LEFT" ]'
empty_after --governance-preset
check "--governance-preset with no value: exit 1, writes nothing" \
    '[ "$RC" -eq 1 ] && [ -z "$LEFT" ]'
empty_after --governance --governance-preset STRICT
check "control: a preset name in another case is accepted and written" \
    '[ "$RC" -eq 0 ] && [ -n "$LEFT" ] && grep -q "preset: strict" <<<"$(cat "$TEST_DIR"/a13.*/govern.json 2>/dev/null)"'

# --- Test 14: the sandbox's refusals are stated (A75(a)) ---
# No preset sets security.sandbox_level, so enforce runs at standard, which
# refuses shell blocks and env.get(); init used to print "Capabilities: read
# files, shell" and say nothing about either.
echo ""
echo "Test 14: the standard sandbox's refusals are stated"
D="$TEST_DIR/p14"; mkdir -p "$D"
SOUT=$(cd "$D" && "$NAAB_BIN" init --governance --governance-preset standard </dev/null 2>&1)
check "summary names the standard sandbox, its refusals and the remedy" \
    'grep -q "Sandbox: .*standard sandbox.*shell blocks.*env.get().*sandbox_level.*elevated" <<<"$SOUT"'
# Opened from its own directory: a native Python (build-windows) cannot open
# an MSYS path handed to it.
check "the file carries the same note" "(cd \"$D\" && python3 -c '
import json; d=json.load(open(\"govern.json\"))
n=d[\"_comment_sandbox\"]
assert \"shell blocks\" in n and \"env.get()\" in n and \"elevated\" in n
assert \"security\" not in d
' 2>/dev/null)"

# --- Test 15: a generated standard project refuses a taint flow (A75) ---
# file.read is a taint source and python_exec a sink in the generated file;
# shell is not usable here, because the standard sandbox refuses it first.
echo ""
echo "Test 15: a generated standard project refuses a tainted flow"
D="$TEST_DIR/p15"; mkdir -p "$D"
(cd "$D" && "$NAAB_BIN" init --governance --governance-preset standard </dev/null >/dev/null 2>&1)
echo "abc" > "$D/data.txt"
printf 'main {\n  let r = <<python\n1 + 1\n>>\n  print("PY_OK " + r)\n}\n' > "$D/probe.naab"
printf 'main {\n  let s = file.read("data.txt")\n  let r = <<python[s]\ns.upper()\n>>\n  print("FLOW_RAN " + r)\n}\n' > "$D/flow.naab"
printf 'main {\n  let s = "abc"\n  let r = <<python[s]\ns.upper()\n>>\n  print("CLEAN_RAN " + r)\n}\n' > "$D/clean.naab"
POUT=$(cd "$D" && "$NAAB_BIN" probe.naab 2>&1)
if ! grep -q "PY_OK 2" <<<"$POUT"; then
    echo "  UNMEASURABLE: embedded Python does not run here, so the python_exec flow cannot be judged"
    UNM=$((UNM + 1))
else
    FOUT=$(cd "$D" && "$NAAB_BIN" flow.naab 2>&1); FRC=$?
    COUT2=$(cd "$D" && "$NAAB_BIN" clean.naab 2>&1); CRC2=$?
    check "file.read -> python block: refused (exit 3, taint violation), the block did not run" \
        '[ "$FRC" -eq 3 ] && grep -q "Taint tracking violation" <<<"$FOUT" && ! grep -q FLOW_RAN <<<"$FOUT"'
    check "control: the same block on a literal runs" \
        '[ "$CRC2" -eq 0 ] && grep -q "CLEAN_RAN ABC" <<<"$COUT2"'
fi

# --- Summary ---
echo ""
echo "=== Results ==="
TOTAL=$((PASS + FAIL))
echo "  $PASS/$TOTAL passed"
[ "$UNM" -gt 0 ] && echo "  $UNM UNMEASURABLE"

if [ $FAIL -gt 0 ]; then
    echo "  $FAIL FAILED"
    exit 1
fi

echo "  All tests passed!"
