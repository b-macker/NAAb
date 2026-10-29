#!/usr/bin/env bash
# test_reported_lines.sh -- what the scanner and a per-function refusal point at (A76(a)(b))
#
# (a) The scanner empties every line of a polyglot block for its NAAb-level
#     checks (stripPolyglotBlocks), and style.multiple_blank_lines counted
#     those as blank: a 10-line file with no blank line drew "8 consecutive
#     blank lines" at its block (observed on master 6055ac3c). Block lines now
#     end a run, the block's own blank lines included -- they are its
#     language's, not NAAb's.
# (b) A per-function refusal's At: named the line the function starts on with
#     the offending text beside it: a TODO on line 9 of a function at line 7
#     read "At: line 7: // TODO ...". The function-body path now reports the
#     line the match is on (placeholders and secrets; a polyglot block's
#     location, its opening line, is unchanged).
#
#   RL-00  CONTROL: four real blank lines in NAAb code are still reported
#   RL-01  a file with a polyglot block and no blank line draws no note
#   RL-02  blank lines inside a polyglot block draw no note
#   RL-03  CONTROL: four real blank lines just before a block are still
#          reported -- the block does not swallow a real run
#   RL-04  a TODO in a function is reported at its own line (9), not the
#          function's (7)
#   RL-05  a TODO in main{} is reported at its own line
#   RL-06  a TODO inside a polyglot block in main{} is reported at its own
#          line (5): main{}'s body check reads the block's text and fires
#          first, and reported main's line (1) on master
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-29):
#   multiple_blank_lines counts block lines again           -> RL-01, RL-02 red
#   checkPlaceholders reports the base line (the pre-fix shape) -> RL-04, RL-05, RL-06 red
#   the function-body path does not pass code_starts_at_line    -> RL-04, RL-05, RL-06 red
# Red first: on master RL-01, RL-02, RL-04, RL-05 and RL-06 fail; the
# controls pass.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_replines_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

# scan ID PROGRAM -> OUT (the scanner's report for the file)
scan() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s' "$2" > "$d/p.naab"
    OUT=$(cd "$d" && timeout 60s "$NAAB" --scan p.naab 2>&1)
}
blank_note() { grep -q 'multiple_blank_lines' <<<"$OUT"; }

# run ID PROGRAM -> OUT, RC (under no_placeholders at hard)
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s\n' '{"mode":"enforce","security":{"sandbox_level":"elevated"},"code_quality":{"no_placeholders":{"enabled":true,"level":"hard"}}}' > "$d/govern.json"
    printf '%s' "$2" > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 60s "$NAAB" run p.naab 2>&1) || RC=$?
}
at_line() { grep -m1 -o 'At: line [0-9]*' <<<"$OUT" | grep -o '[0-9]*$'; }

echo "=== Reported lines (A76(a)(b)) ==="

scan rl00 $'main {\n    let a = 1\n\n\n\n\n    print(a)\n}\n'
if blank_note; then
    pass RL-00 "CONTROL: four real blank lines are reported"
else
    fail RL-00 "four blank lines in NAAb code should draw the note" "$(head -c 300 <<<"$OUT" | tr '\n' ' ')"
fi

scan rl01 $'main {\n    let r = <<python\nx = 1\ny = 2\nz = 3\nprint(x + y + z)\n>>\n    print(r)\n}\n'
if [ -n "$OUT" ] && ! blank_note; then
    pass RL-01 "a polyglot block with no blank line draws no blank-line note"
else
    fail RL-01 "a file with no blank line should draw no blank-line note" "$(grep -A1 multiple_blank_lines <<<"$OUT" | tr '\n' ' ')"
fi

scan rl02 $'main {\n    let r = <<python\nx = 1\n\n\n\n\ny = 2\nprint(x + y)\n>>\n    print(r)\n}\n'
if [ -n "$OUT" ] && ! blank_note; then
    pass RL-02 "blank lines inside a polyglot block draw no NAAb note"
else
    fail RL-02 "a block's own blank lines are its language's" "$(grep -A1 multiple_blank_lines <<<"$OUT" | tr '\n' ' ')"
fi

scan rl03 $'main {\n    let a = 1\n\n\n\n\n    let r = <<python\nx = 1\nprint(x)\n>>\n    print(r)\n}\n'
if blank_note && grep -q 'Line 3 ' <<<"$OUT"; then
    pass RL-03 "CONTROL: real blank lines before a block are still reported (at line 3)"
else
    fail RL-03 "four blank lines before a block should still be reported at line 3" "$(grep -A1 multiple_blank_lines <<<"$OUT" | tr '\n' ' ')"
fi

run rl04 $'main {\n    print(f())\n}\n\n\n\nfn f() {\n    let a = 1\n    // TODO: finish this\n    return a\n}\n'
if [ "$RC" -eq 3 ] && [ "$(at_line)" = "9" ]; then
    pass RL-04 "a TODO in a function is reported at its own line (9)"
else
    fail RL-04 "the TODO is on line 9 (the function starts on 7)" "rc=$RC at=$(at_line)"
fi

run rl05 $'main {\n    let a = 1\n    let b = 2\n    // TODO: finish main\n    print(a + b)\n}\n'
if [ "$RC" -eq 3 ] && [ "$(at_line)" = "4" ]; then
    pass RL-05 "a TODO in main{} is reported at its own line (4)"
else
    fail RL-05 "the TODO in main{} is on line 4" "rc=$RC at=$(at_line)"
fi

run rl06 $'main {\n    let a = 1\n    let r = <<python\nx = 1\n# TODO: finish the block\nprint(x)\n>>\n    print(r)\n}\n'
if [ "$RC" -eq 3 ] && [ "$(at_line)" = "5" ]; then
    pass RL-06 "a TODO inside a block in main{} is reported at its own line (5)"
else
    fail RL-06 "the TODO is on line 5" "rc=$RC at=$(at_line)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
