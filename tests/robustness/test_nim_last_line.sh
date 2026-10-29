#!/usr/bin/env bash
# test_nim_last_line.sh -- a Nim block's last line is printed or run, never mangled (D12)
#
# A <<nim>> block's value is what its last line prints, so nim_executor wrapped
# that line as `echo <line>`. A last line that returns nothing -- a call like
# writeFile(...) -- then failed to compile ("type mismatch", the executor's
# "Nim type error" hint), one-line or multi-line (observed on master 6055ac3c).
# The multi-line wrapper also wrote the echo at column 0, so an indented last
# line left its block. The executor now asks Nim at compile time whether the
# line can be printed (when compiles(echo(...))) and runs it as it is when it
# cannot, keeping its indentation; the line sits alone in a template, so a
# trailing comment cannot swallow a parenthesis and it is evaluated once.
#
#   NL-00  CONTROL: a one-line expression's value is the block's value (5)
#   NL-01  a one-line statement that returns nothing runs (its file is written)
#   NL-02  a multi-line block ending in such a statement runs
#   NL-03  CONTROL: an expression with a trailing comment still prints its value
#   NL-04  CONTROL: a last line with a side effect is evaluated once (counter 1)
#   NL-05  an indented last line stays in its block (a `block:` ending in
#          writeFile runs)
#   NL-06  CONTROL: languages.blocked still refuses the block (exit 3, nothing
#          written) -- the wrapper is behind the gate
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-28):
#   the last line wrapped as `echo <line>` again (the pre-fix shape)
#                                                    -> NL-01, NL-02, NL-05 red
#   the multi-line wrapper drops the line's indentation -> NL-05 red
# Red first: on master NL-01, NL-02 and NL-05 fail; the controls pass.
# Needs nim on PATH: without it every arm is reported SKIP/UNMEASURABLE (exit 0;
# CI installs no Nim, so this is measured where Nim is installed).

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_nimlast_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

ENFORCE='{"mode":"enforce","security":{"sandbox_level":"elevated"}}'

# run ID CONFIG NIM_BODY -> OUT, RC, DIR (the block's value is printed as VALUE=)
run() {
    DIR="$W/$1"; mkdir -p "$DIR"
    printf '%s\n' "$2" > "$DIR/govern.json"
    printf 'main {\n    let r = <<nim\n%s\n>>\n    print("VALUE=" + string(r))\n}\n' "$3" > "$DIR/p.naab"
    RC=0; OUT=$(cd "$DIR" && timeout 180s "$NAAB" run p.naab 2>&1) || RC=$?
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
brief() { head -c 300 <<<"$OUT" | tr '\n' ' '; }

echo "=== Nim block last line (D12) ==="

if ! command -v nim >/dev/null 2>&1; then
    # A missing toolchain is reported, not failed, as for every polyglot
    # language CI does not install (Nim is one): the skip report counts it.
    for a in NL-00 NL-01 NL-02 NL-03 NL-04 NL-05 NL-06; do echo "  SKIP [$a] nim is not on PATH -- UNMEASURABLE"; done
    exit 0
fi

run nl00 "$ENFORCE" '2 + 3'
if [ "$RC" -eq 0 ] && has "VALUE=5"; then
    pass NL-00 "CONTROL: a one-line expression's value is the block's value"
else
    fail NL-00 "a one-line expression should give 5" "rc=$RC $(brief)"
fi

run nl01 "$ENFORCE" 'writeFile("nl01.txt", "NL01")'
if [ "$RC" -eq 0 ] && [ "$(cat "$DIR/nl01.txt" 2>/dev/null)" = "NL01" ]; then
    pass NL-01 "a one-line statement that returns nothing runs"
else
    fail NL-01 "the one-line statement should run and write its file" "rc=$RC $(brief)"
fi

run nl02 "$ENFORCE" $'let msg = "NL02"\nwriteFile("nl02.txt", msg)'
if [ "$RC" -eq 0 ] && [ "$(cat "$DIR/nl02.txt" 2>/dev/null)" = "NL02" ]; then
    pass NL-02 "a multi-line block ending in a statement runs"
else
    fail NL-02 "the multi-line block should run its last statement" "rc=$RC $(brief)"
fi

run nl03 "$ENFORCE" '2 + 3 # a trailing comment'
if [ "$RC" -eq 0 ] && has "VALUE=5"; then
    pass NL-03 "CONTROL: an expression with a trailing comment prints its value"
else
    fail NL-03 "an expression with a trailing comment should give 5" "rc=$RC $(brief)"
fi

run nl04 "$ENFORCE" $'var counter = 0\nproc bump(): int =\n  inc counter\n  counter\nbump()'
if [ "$RC" -eq 0 ] && has "VALUE=1"; then
    pass NL-04 "CONTROL: the last line is evaluated once"
else
    fail NL-04 "the side-effecting last line should give 1" "rc=$RC $(brief)"
fi

run nl05 "$ENFORCE" $'block:\n  let m = "NL05"\n  writeFile("nl05.txt", m)'
if [ "$RC" -eq 0 ] && [ "$(cat "$DIR/nl05.txt" 2>/dev/null)" = "NL05" ]; then
    pass NL-05 "an indented last line stays in its block"
else
    fail NL-05 "the indented last line should run inside its block" "rc=$RC $(brief)"
fi

run nl06 '{"mode":"enforce","security":{"sandbox_level":"elevated"},"languages":{"blocked":["nim"]}}' 'writeFile("nl06.txt", "NL06")'
if [ "$RC" -eq 3 ] && [ ! -e "$DIR/nl06.txt" ]; then
    pass NL-06 "CONTROL: languages.blocked still refuses the block"
else
    fail NL-06 "a blocked language must be refused before anything runs" "rc=$RC $(brief)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
