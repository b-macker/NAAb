#!/usr/bin/env bash
# ============================================================
# test_inert_limits.sh — limits.data keys that are parsed and never enforced
#
# WHAT WAS FOUND
#
# limits.data.string_length, nesting_depth and dict_size are parsed, recorded
# in explicitly_set (so they take part in the ratchet and in inheritance) and
# clamped -- then read by nothing. Their only readers are
# GovernanceEngine::checkStringLength / checkNestingDepth / checkDictSize,
# which have zero call sites anywhere in src/: defined once, declared once in
# governance.h, never invoked. An operator can set a HARD data limit, watch it
# survive validation, and get no enforcement whatsoever.
#
# Found by sweeping every enforce()/recordPass() rule name back to its
# enclosing method and asking which methods nothing calls -- the same shape as
# CONTRA-007, which iterated a set its own loader had already emptied.
#
# WHY THE CHECKS WERE NOT SIMPLY WIRED IN
#
# They enforce at HARD: exit 3, uncatchable. 32 config sites in this repo
# already set these keys, including both govern-template.json copies and
# tests/gorilla/naab-32/phases/phase1-hardening.json, which sets dict_size 50
# and nesting_depth 8 -- tight enough to block ordinary data. Wiring them in
# would start hard-blocking configs that pass today. That is a behaviour change
# wearing a bug fix's clothes, and it is the third time in this campaign that
# the safe-looking direction turned out to be one.
#
# THE CONTROLS ARE LD-04 THROUGH LD-07, AND THEY ARE NOT DECORATION
#
# Three sibling keys in the same block ARE enforced, by paths that have nothing
# to do with the dead methods:
#   output_size     polyglot.cpp:718 enforces it directly
#   array_size      mirrored to rules_.max_array_size, which has live consumers
#   max_json_depth  calls setMaxJsonDepth(), read by json_impl.cpp
# A warning that fired on the whole block would satisfy LD-01..03 completely,
# so those three are what make LD-01..03 mean anything.
#
# max_json_depth deserves its own note: it is documented as an alias for
# nesting_depth and writes the same struct field, but it ALSO sets the live
# JSON depth limit. So two spellings of one documented setting behave
# differently -- one enforces, one does nothing. That is why LD-03's warning
# names it, and why LD-08 uses it as the positive half of the comparison.
#
# LD-08 IS THE SUBSTANTIVE GATE. The others check that a string is printed.
# LD-08 checks that the string is TRUE: a tiny string_length does not block a
# long string, while a tiny max_json_depth does block deep JSON. Without it
# this suite would pass against a warning that libelled a working check.
#
# EVERY GATE HAS A DEMONSTRATED FAILURE CASE
#
#   F1   warning suppressed                        -> LD-01 LD-02 LD-03
#   F2   warn on every key in the block            -> LD-04 LD-05 LD-06
#   F3   nesting_depth stops naming max_json_depth -> LD-03
#   F4'  setMaxJsonDepth made a no-op              -> LD-08
#   F5   warn on block presence, not on a key      -> LD-07
#
# F4' is worth explaining. The first attempt at degrading LD-08 made
# string_length also set the JSON depth, on the theory that this "wired the
# check in" -- but LD-08's inert half tests a STRING, not JSON, so nothing was
# enforced and LD-08 passed. The degradation that works breaks the LIVE half
# instead: with setMaxJsonDepth a no-op, both sides of the comparison become
# absences and LD-08 fails, which is exactly what it should do. A gate that
# compares two no-ops measures nothing.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../helpers/gatelib.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ ! -x "$NAAB" ]; then
    echo "  test_inert_limits.sh: SKIPPED — build/naab-lang not found"
    exit 0
fi

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
W="${_SYSTMP}/inert-limits-$$"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
mkdir -p "$W"

echo "=== limits.data: parsed, ratcheted, and unenforced ==="

gate_init "inert-limits"
gate_def LD-01 WARN    "string_length warns that nothing enforces it"
gate_def LD-02 WARN    "dict_size warns that nothing enforces it"
gate_def LD-03 WARN    "nesting_depth warns, and names the spelling that works"
gate_def LD-04 CONTROL "output_size stays silent — polyglot.cpp enforces it"
gate_def LD-05 CONTROL "array_size stays silent — mirrored to a live field"
gate_def LD-06 CONTROL "max_json_depth stays silent — json_impl.cpp enforces it"
gate_def LD-07 CONTROL "no warning when limits.data is absent"
gate_def LD-08 TRUTH   "the warning is true — inert key does not block, live key does"

run_limits() {  # $1 = JSON object for limits.data, $2 = script
    cat > "$W/govern.json" << EOF
{"version": "4.0", "limits": {"data": $1}}
EOF
    (cd "$W" && "$NAAB" "${2:-t.naab}" 2>&1)
}

warned_for() {  # $1 = key, $2 = output
    echo "$2" | grep -q "limits.data.$1\" is parsed but not enforced"
}

cat > "$W/t.naab" << 'EOF'
main { print("ok") }
EOF

# --- LD-01..LD-03: the inert keys warn ------------------------------------
i=0
for key in string_length dict_size nesting_depth; do
    i=$((i + 1)); id="LD-0$i"
    out="$(run_limits "{\"$key\": 8}")"
    if warned_for "$key" "$out"; then
        if [ "$key" = nesting_depth ] && ! echo "$out" | grep -q 'max_json_depth'; then
            fail "$id" "nesting_depth warned but did not name max_json_depth" \
                 "an operator told only that their key is dead cannot find the one that works"
        else
            pass "$id" "$key warns that nothing enforces it"
        fi
    else
        fail "$id" "$key did not warn" "an inert HARD limit looks configured and enforces nothing"
    fi
done

# --- LD-04..LD-06: siblings in the same block that ARE enforced ------------
i=3
for key in output_size array_size max_json_depth; do
    i=$((i + 1)); id="LD-0$i"
    out="$(run_limits "{\"$key\": 4096}")"
    if warned_for "$key" "$out"; then
        fail "$id" "$key was wrongly reported as inert" \
             "this key has a live enforcement path; the warning is firing on the block"
    else
        pass "$id" "$key stays silent — it is enforced elsewhere"
    fi
done

# --- LD-07: silence when the block is absent -------------------------------
OUT7="$(run_limits '{}')"
if echo "$OUT7" | grep -q 'is parsed but not enforced'; then
    fail LD-07 "warned with no keys set" "the warning fires on block presence, not on a key"
else
    pass LD-07 "no warning when limits.data is absent"
fi

# --- LD-08: is the warning telling the truth? ------------------------------
# Inert side: string_length 4 must NOT block a much longer string.
cat > "$W/long.naab" << 'EOF'
main {
    let s = "abcdefghijklmnopqrstuvwxyz0123456789"
    print(s)
}
EOF
INERT_OUT="$(run_limits '{"string_length": 4}' long.naab)"
INERT_BLOCKED=1
echo "$INERT_OUT" | grep -q 'abcdefghijklmnopqrstuvwxyz' && INERT_BLOCKED=0

# Live side: max_json_depth 1 must block deeply nested JSON, proving the
# comparison is between two reachable code paths rather than two no-ops.
cat > "$W/deep.naab" << 'EOF'
use json
main {
    try {
        let d = json.parse("{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":1}}}}}")
        print("PARSED_OK")
    } catch (e) {
        print("PARSE_BLOCKED")
    }
}
EOF
LIVE_OUT="$(run_limits '{"max_json_depth": 1}' deep.naab)"

if [ "$INERT_BLOCKED" -eq 0 ] && echo "$LIVE_OUT" | grep -q 'PARSE_BLOCKED'; then
    pass LD-08 "the warning is true — inert key does not block, live key does"
elif [ "$INERT_BLOCKED" -ne 0 ]; then
    fail LD-08 "string_length DID block — the key is not inert after all" \
         "the warning would be libelling a working check"
else
    fail LD-08 "max_json_depth did not block deep JSON" \
         "cannot show the comparison is between two reachable paths; LD-01..03 unproven"
fi

gate_print_summary
gate_exit
