#!/usr/bin/env bash
# test_reported_lines.sh -- what the scanner and a per-function refusal point at (A76(a)(b))
#
# (a) The scanner empties every line of a polyglot block for its NAAb-level
#     checks (stripPolyglotBlocks), and style.multiple_blank_lines counted
#     those as blank: a 10-line file with no blank line drew "8 consecutive
#     blank lines" at its block (observed on master 1c2a8765). Block lines now
#     end a run, the block's own blank lines included -- they are its
#     language's, not NAAb's.
# (b) A per-function refusal's At: named the line the function starts on with
#     the offending text beside it: a TODO on line 9 of a function at line 7
#     read "At: line 7: // TODO ...". The same class, found fixing it: PII and
#     incomplete logic named the function's line too, and the whole-file check
#     (secrets, PII, incomplete logic over the entry file and each module --
#     the one that fires first) named NO line; and the reports and SARIF read
#     the check context, not the message, so they kept the old line. Each
#     match-level finding is now printed and recorded at the line the match is
#     on. A polyglot block's location, its opening line, is unchanged.
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
#   RL-07  a secret on line 9 of a function is reported At: line 9. The
#          whole-file check fires first, and named NO line on master (`At:`
#          gave only the masked value)
#   RL-07s the same refusal's SARIF result has startLine 9 (master: none)
#   RL-08  RL-04's TODO is recorded where it is printed: SARIF startLine 9,
#          not the function's 7 (the report read the check context, which
#          named the function's line, while At: named the TODO's)
#   RL-09  PII on line 9 is reported At: line 9 (master: no line)
#   RL-10  an empty catch on line 9 is reported At: line 9 (master: no line)
#   RL-11  under SOFT with --governance-override both the whole-file and the
#          per-function findings run: a secret, an SSN and an empty catch on
#          lines 9, 10 and 11 are each recorded twice, at their own line
#          (master: no line, and the function's 7)
#
# The secret and the SSN are built at run time from fragments; the repo holds
# neither value.
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-29):
#   multiple_blank_lines counts block lines again           -> RL-01, RL-02 red
#   checkPlaceholders reports the base line (the pre-fix shape) -> RL-04, RL-05, RL-06 red
#   the function-body path does not pass code_starts_at_line    -> RL-04, RL-05, RL-06 red
#   the whole-file check passes line 0 again       -> RL-07, RL-07s, RL-09, RL-10, RL-11 red
#   findings printed at the match line but not recorded there
#                                                   -> RL-07s, RL-08, RL-11 red
#   the PII match offset is not reported (line 1)   -> RL-09, RL-11 red
#   the pattern match offset is not reported        -> RL-10, RL-11 red
#   the function-body PII/incomplete-logic calls drop the flag -> RL-11 red
#   the function-body secrets call drops the flag   -> RL-11 red
# Red first: on master RL-01, RL-02, RL-04..RL-11 fail; the controls pass.

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

FAKE_KEY="AK""IA""ABCDEFGHIJ""KLMNOP"
FAKE_SSN="123""-45-""6789"
# fn_with LINE9 [LINE10 LINE11] -> a program whose f() starts on line 7
fn_with() {
    printf 'main {\n    print(f())\n}\n\n\n\nfn f() {\n    let a = 1\n'
    printf '%s\n' "$@"
    printf '    return a\n}\n'
}
# runcfg ID CONFIG PROGRAM [ARGS...] -> OUT, RC, DIR
runcfg() {
    DIR="$W/$1"; mkdir -p "$DIR"
    printf '%s\n' "$2" > "$DIR/govern.json"
    printf '%s' "$3" > "$DIR/p.naab"
    shift 3
    RC=0; OUT=$(cd "$DIR" && timeout 60s "$NAAB" run p.naab "$@" 2>&1) || RC=$?
}
# sarif_lines FILE RULE -> each RULE result's startLine ("-" when it has none)
sarif_lines() {
    python3 -c '
import json, sys
d = json.load(sys.stdin)
out = []
for r in d["runs"][0]["results"]:
    if r.get("ruleId") == sys.argv[1]:
        for l in r.get("locations", [{}]):
            out.append(str(l.get("physicalLocation", {}).get("region", {}).get("startLine", "-")))
print(" ".join(sorted(out)))' "$2" < "$1" 2>/dev/null
}
cq() { printf '{"mode":"enforce","security":{"sandbox_level":"elevated"},"code_quality":{%s}}' "$1"; }

runcfg rl07 "$(cq '"no_secrets":{"enabled":true,"level":"hard"}')" \
    "$(fn_with "    let k = \"$FAKE_KEY\"")" --governance-sarif o.sarif
if [ "$RC" -eq 3 ] && [ "$(at_line)" = "9" ]; then
    pass RL-07 "a secret on line 9 is reported At: line 9"
else
    fail RL-07 "the secret is on line 9" "rc=$RC at=$(at_line) $(grep -m1 'At:' <<<"$OUT" | sed 's/[A-Z0-9]\{8,\}/<v>/')"
fi
L=$(sarif_lines "$DIR/o.sarif" code_quality.no_secrets)
if [ "$L" = "9" ]; then
    pass RL-07s "the refusal's SARIF result is at line 9"
else
    fail RL-07s "the SARIF result should be at line 9" "startLine(s)=${L:-<no result>}"
fi

runcfg rl08 '{"mode":"enforce","security":{"sandbox_level":"elevated"},"code_quality":{"no_placeholders":{"enabled":true,"level":"hard"}}}' \
    $'main {\n    print(f())\n}\n\n\n\nfn f() {\n    let a = 1\n    // TODO: finish this\n    return a\n}\n' --governance-sarif o.sarif
L=$(sarif_lines "$DIR/o.sarif" code_quality.no_placeholders)
if [ "$RC" -eq 3 ] && [ "$(at_line)" = "9" ] && [ "$L" = "9" ]; then
    pass RL-08 "the TODO is recorded where it is printed (At: 9, SARIF 9)"
else
    fail RL-08 "At: and SARIF should both name line 9" "rc=$RC at=$(at_line) sarif=${L:-<no result>}"
fi

runcfg rl09 "$(cq '"no_pii":{"enabled":true,"level":"hard"}')" \
    "$(fn_with "    let s = \"$FAKE_SSN\"")"
if [ "$RC" -eq 3 ] && [ "$(at_line)" = "9" ]; then
    pass RL-09 "PII on line 9 is reported At: line 9"
else
    fail RL-09 "the PII is on line 9" "rc=$RC at=$(at_line)"
fi

runcfg rl10 "$(cq '"no_incomplete_logic":{"enabled":true,"level":"hard"}')" \
    "$(fn_with '    try { a = a + 1 } catch (e) { }')"
if [ "$RC" -eq 3 ] && [ "$(at_line)" = "9" ]; then
    pass RL-10 "an empty catch on line 9 is reported At: line 9"
else
    fail RL-10 "the empty catch is on line 9" "rc=$RC at=$(at_line)"
fi

runcfg rl11 "$(cq '"no_secrets":{"enabled":true,"level":"soft"},"no_pii":{"enabled":true,"level":"soft"},"no_incomplete_logic":{"enabled":true,"level":"soft"}')" \
    "$(fn_with "    let k = \"$FAKE_KEY\"" "    let s = \"$FAKE_SSN\"" '    try { a = a + 1 } catch (e) { }')" \
    --governance-override --governance-sarif o.sarif
LS=$(sarif_lines "$DIR/o.sarif" code_quality.no_secrets)
LP=$(sarif_lines "$DIR/o.sarif" code_quality.no_pii)
LI=$(sarif_lines "$DIR/o.sarif" code_quality.no_incomplete_logic)
if [ "$RC" -eq 0 ] && [ "$LS" = "9 9" ] && [ "$LP" = "10 10" ] && [ "$LI" = "11 11" ]; then
    pass RL-11 "whole-file and per-function findings each recorded at their own line (9, 10, 11)"
else
    fail RL-11 "each finding should be recorded twice at its own line" "rc=$RC secrets=[$LS] pii=[$LP] incomplete=[$LI]"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
