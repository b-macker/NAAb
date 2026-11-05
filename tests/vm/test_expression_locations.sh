#!/usr/bin/env bash
# A121(d) (Bundle 12): a runtime error inside an expression names the line it
# is on.
#
# The parser built every binary and unary operator, range, member access,
# subscript, number, bool and null literal, list and dict literal with an
# empty location (line 0), and the VM compiler emits each instruction at its
# node's line, so an error raised by one of those instructions said
# "x.naab:0". The VM also read the line of a stdlib call four instructions
# before the call (`- 4`, from when the code was bytes; it has been one word
# per instruction since 1dd08e83), so a sink check could name an argument's
# line, or 0 (test_taint_report_line.sh TL-04/TL-05). Each node now takes the
# location of its operator or opening token (calls already took their '(',
# #291), and the VM reads the call's own instruction.
#
# Each program errors on line 6, after a blank line, so a line borrowed from a
# neighbour shows as 4 or 7.
#
#   EL-00  CONTROL: a call to a value that is not a function names line 6 on
#          both builds -- the harness reads a location at all
#   EL-01  binary operator       `b - a`
#   EL-02  unary operator        `-b`
#   EL-03  comparison            `b < 2`
#   EL-04  subscript             `b[5]`
#   EL-05  member access         `b.foo`
#   EL-06  compound assignment   `b -= 1`
#   EL-07  range                 `b..3`
#
# Red first (observed 2026-11-05, the pre-change parser): EL-01..EL-07 named
# line 0; EL-00 named 6.
#
# Kill table (src/parser/parser.cpp, each mutant applied alone, built and run;
# observed 2026-11-05): `+`/`-` built with no location -> EL-01; unary minus ->
# EL-02; a comparison -> EL-03; a subscript -> EL-04; a member access -> EL-05;
# a compound assignment -> EL-06; a range -> EL-07. EL-00 stays green under all
# seven.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -6 | sed 's/^/       /'; }

echo "=== A runtime error inside an expression names its line ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    echo "  FAIL naab-lang not built (UNMEASURABLE, not a pass)"; exit 1
fi
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-exprloc.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

# arm ID EXPR WHY: the program errors at EXPR on line 6
arm() {
    local id="$1" expr="$2" why="$3" out rc line
    printf 'main {\n    let a = 1\n    let b = [1]\n    print("STARTED")\n\n    let c = %s\n    print(c)\n}\n' "$expr" > "$W/$id.naab"
    out=$(cd "$W" && timeout 20 "$NAAB" --no-governance "$id.naab" 2>&1); rc=$?
    line=$(grep -o "$id\.naab:[0-9]*" <<<"$out" | head -1 | sed 's/.*://')
    if [ "$rc" -ne 0 ] && grep -q "STARTED" <<<"$out" && [ "$line" = "6" ]; then
        ok "$id" "$why: the error names line 6"
    else
        bad "$id" "$why: expected an error at line 6" "rc=$rc line=${line:-none}
$out"
    fi
}

arm EL-00 'a()'      "CONTROL: calling a value that is not a function"
arm EL-01 'b - a'    "binary operator"
arm EL-02 '-b'       "unary operator"
arm EL-03 'b < 2'    "comparison"
arm EL-04 'b[5]'     "subscript"
arm EL-05 'a.foo'    "member access"
arm EL-07 'b..3'     "range"

# compound assignment is a statement, not an initialiser
printf 'main {\n    let a = 1\n    let b = [1]\n    print("STARTED")\n\n    b -= a\n    print(b)\n}\n' > "$W/EL-06.naab"
out=$(cd "$W" && timeout 20 "$NAAB" --no-governance EL-06.naab 2>&1); rc=$?
line=$(grep -o 'EL-06\.naab:[0-9]*' <<<"$out" | head -1 | sed 's/.*://')
if [ "$rc" -ne 0 ] && grep -q "STARTED" <<<"$out" && [ "$line" = "6" ]; then
    ok "EL-06" "compound assignment: the error names line 6"
else
    bad "EL-06" "compound assignment: expected an error at line 6" "rc=$rc line=${line:-none}
$out"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
