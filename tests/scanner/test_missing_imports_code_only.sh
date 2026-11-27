#!/usr/bin/env bash
# test_missing_imports_code_only.sh -- the scanner's missing_imports reads
# NAAb code, not comments or string text (A218, U-012).
#
# redundancy.missing_imports searched every line for `<module>.`, so a comment
# or a string naming a stdlib module the file does not `use` was reported as a
# use: a file whose only mentions were `// Parse with json.parse later` and
# print("call uuid.v4 to make an id") got five findings (observed on
# d32f8603). The usage search now reads each line with its comments (#, //,
# /* */) and string text blanked; the code inside a string's ${...} is kept,
# since it runs. The `use` lines are read as before.
#
#   MI-00  CONTROL: an unimported module used in code is flagged.
#   MI-01  a module named in a comment (#, //, /* */) is not flagged.
#   MI-02  a module named in a string ("...", '...', an escaped quote) is not.
#   MI-03  a module used inside a string's ${...} IS flagged.
#   MI-04  a use after a string containing # is flagged (the # is string text).
#   MI-05  a use after a block comment closes on the same line is flagged.
#   MI-06  CONTROL: with `use <module>` the same code is not flagged.
#
# Not changed, recorded: the check treats only `debug` as auto-imported,
# while the VM's prelude makes thirteen modules available without `use`
# (naab/stdlib_prelude.h).
#
# Kill table (mutants of src/scanner/checks_redundancy.cpp, run by hand):
#   S1 the usage search reads the raw lines        -> MI-01, MI-02
#   S2 interpolated code blanked as string text    -> MI-03
#   S3 strings not recognised                      -> MI-02, MI-04
#   S4 a block comment never ends on its line      -> MI-05
#
# The text report has two layouts (one finding, several), and the first
# version of MI-05 asserted a message the grouped layout never prints, so it
# passed on d32f8603 too; it now asserts the line list.

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_mimp.XXXXXX")"
trap 'rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -10 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

# scan ID -> LINES ("" when not flagged); RC; OUT. The text report groups a
# rule's findings and lists their lines ("Lines: 2, 5").
N=0
scan() {
    N=$((N+1)); printf '%s\n' "$1" > "$W/f$N.naab"
    OUT=$(cd "$W" && "$NAAB" --scan "f$N.naab" naab 2>&1); RC=$?
    # Several findings: "redundancy.missing_imports (N occurrences)" then
    # "Lines: 2, 5"; one finding: "Line 3 [SOFT] redundancy.missing_imports".
    LINES=$(grep -A1 'redundancy.missing_imports (' <<<"$OUT" | sed -n 's/^ *Lines: //p')
    [ -z "$LINES" ] && LINES=$(sed -n 's/^ *! Line \([0-9]*\) .*redundancy\.missing_imports.*/\1/p' <<<"$OUT" | head -1)
    FLAGGED=0; grep -q 'redundancy.missing_imports' <<<"$OUT" && FLAGGED=1
}
# A scan that printed no report at all is UNMEASURABLE, not "not flagged".
measured() { grep -qi 'issue\|scan\|---' <<<"$OUT"; }

scan $'main {\n    let id = uuid.v4()\n    print(id)\n}'
if [ "$FLAGGED" = 1 ] && grep -q "'uuid' is used but module 'uuid' is not imported" <<<"$OUT"; then
    ok MI-00 "CONTROL: an unimported module used in code is flagged"
else bad MI-00 "the code use was not flagged (rc=$RC) -- every 'not flagged' arm is UNMEASURABLE" "$OUT"; exit 1; fi

scan $'// Parse the reply with json.parse later\n# see regex.match for the pattern\n/* csv.read handles quoting */\nmain {\n    print("ok")\n}'
if measured && [ "$FLAGGED" = 0 ]; then ok MI-01 "modules named in #, // and /* */ comments are not flagged"
else bad MI-01 "a comment was read as a use (lines: ${LINES:-none})" "$OUT"; fi

scan $'main {\n    print("call uuid.v4 to make an id")\n    print(\'use crypto.sha256 for hashes\')\n    let s = "say \\"math.pi\\" twice"\n}'
if measured && [ "$FLAGGED" = 0 ]; then ok MI-02 "modules named in strings (double, single, escaped quote) are not flagged"
else bad MI-02 "a string was read as a use (lines: ${LINES:-none})" "$OUT"; fi

scan $'main {\n    let id = "x-y"\n    print("id=${regex.replace(id, "-", "")}")\n}'
if [ "$FLAGGED" = 1 ] && grep -q "'regex' is used" <<<"$OUT"; then
    ok MI-03 "a module used inside \${...} is flagged (the interpolated code runs)"
else bad MI-03 "interpolated code was not read (lines: ${LINES:-none})" "$OUT"; fi

scan $'main {\n    let a = "has # not a comment" + file.read("x")\n}'
if [ "$FLAGGED" = 1 ] && grep -q "'file' is used" <<<"$OUT"; then
    ok MI-04 "a use after a string containing # is flagged"
else bad MI-04 "the # inside a string ended the line (lines: ${LINES:-none})" "$OUT"; fi

# The report prints one message per rule, so the mention (line 2) and the
# use (line 3) are told apart by the line list, not by the message.
scan $'main {\n    /* a note about http.get\n    */ let c = path.join("a", "b")\n}'
if [ "$FLAGGED" = 1 ] && [ "$LINES" = "3" ]; then
    ok MI-05 "a use after a block comment closes is flagged; the comment's mention is not"
else bad MI-05 "block comment handling (lines: ${LINES:-none})" "$OUT"; fi

scan $'use uuid\nmain {\n    let id = uuid.v4()\n    print(id)\n}'
if measured && [ "$FLAGGED" = 0 ]; then ok MI-06 "CONTROL: with use uuid the same code is not flagged"
else bad MI-06 "an imported module was flagged" "$OUT"; fi

echo
echo "missing_imports reads code: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
