#!/usr/bin/env bash
# A125 (Bundle 16): a JavaScript block's value is its last expression, whatever
# statements precede it.
#
# The executor (src/runtime/js_executor.cpp) wraps multi-line JavaScript in a
# function and returns the last statement. When the code ended in a bracket it
# looked for the last statement's start at a top-level ';' only. A function
# declaration ends in '}', so `function f(n) { ... }` then `f(21)` read as one
# statement, started with `function`, and the block returned undefined -- null
# in NAAb -- on both engines. The same code with the call's result assigned to
# a variable returned 42. And a block whose last statement was a declaration
# was wrapped as `return (const b = ...)`, a SyntaxError, so it never ran.
# (CLAUDE-TEMPLATE.md's item 46 is about a different shape, a multi-line last
# expression, which works since 7b82a8d5: JL-C3.)
#
# Each arm prints the block's value and compares it with the value the same
# computation gives as one expression, on the VM and on the tree-walker.
#
#   JL-00  CONTROL: a one-line block returns its value on both engines -- the
#          harness reads a value at all
#   JL-01  a function declaration, then a call
#   JL-02  the same, the call ending in ';'
#   JL-03  a var, a function declaration, then a call
#   JL-04  a function declared over three lines, then a call
#   JL-05  two declarations, then a call
#   JL-06  a function given a callback (tests/analyzer/semantic_analyzer_test.naab)
#   JL-07  a recursive factorial(15) (tests/comprehensive/test_all_languages_stress.naab)
#   JL-08  a block whose last statement is a multi-line declaration runs: null
#          (every path wrapped it as `return (const b = ...)`, a SyntaxError)
#   JL-09  the same with a one-line declaration last (the last-line path)
#   JL-10  `var b = function(x) {...}` then `(41)` is one statement (no
#          semicolon is inserted before `(`): null, never 41 -- the split must
#          not cut where JavaScript does not end a statement
#   Controls that already worked and must not change:
#   JL-C1  the call's result assigned, the name on the last line
#   JL-C2  an arrow function
#   JL-C3  a multi-line object literal as the last expression (7b82a8d5)
#   JL-C4  a block that ends in a for loop is a statement: null
#   JL-C5  a block that ends in if/else is a statement: null
#
# Red first (observed 2026-11-06, master 01fb3552): JL-01..JL-07 return null on
# both engines, JL-08..JL-10 fail with a SyntaxError (exit 1); JL-00 and
# JL-C1..JL-C5 pass. JL-10 is what the newline rule is for: with it removed,
# the block returns 41 (see the kill table).
#
# Kill table (each mutant alone, rebuilt, this suite re-run; observed 2026-11-06):
#   M50 compiles() always false                -> JL-01..JL-07
#   M51 no split in the `;`-found branch       -> JL-03
#   M52 no split in the no-`;` branch          -> JL-01, -02, -04..-07
#   M53 newline boundaries not restricted      -> JL-10 (returns 41)
#   M54 declarations not statements            -> JL-08, JL-09, JL-10
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -6 | sed 's/^/       /'; }

echo "=== A JavaScript block's value is its last expression ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    echo "  FAIL naab-lang not built (UNMEASURABLE, not a pass)"; exit 1
fi
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-jslast.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

# arm ID EXPECTED WHY JS-LINES...: the block's lines, one per argument
arm() {
    local id="$1" want="$2" why="$3"; shift 3
    {
        printf 'main {\n    let v = <<javascript\n'
        local l; for l in "$@"; do printf '%s\n' "$l"; done
        printf '>>\n    print("VALUE=" + string(v))\n}\n'
    } > "$W/$id.naab"
    local eng got rc bad_eng=""
    for eng in vm tw; do
        local flag=""; [ "$eng" = tw ] && flag="--tree-walk"
        got=$(cd "$W" && timeout 30 "$NAAB" --no-governance $flag "$id.naab" 2>&1); rc=$?
        got=$(grep -o 'VALUE=.*' <<<"$got" | head -1 | tr -d '\r')
        [ "$rc" -eq 0 ] && [ "$got" = "VALUE=$want" ] || bad_eng="$bad_eng $eng:rc=$rc:${got:-none}"
    done
    if [ -z "$bad_eng" ]; then ok "$id" "$why: $want on both engines"
    else bad "$id" "$why: expected $want" "$bad_eng"; fi
}

arm JL-00 42 "CONTROL: a one-line block" '21 * 2'
arm JL-01 42 "a function declaration, then a call" \
    'function f(n) { return n * 2; }' 'f(21)'
arm JL-02 42 "the same, the call ending in a semicolon" \
    'function f(n) { return n * 2; }' 'f(21);'
arm JL-03 42 "a var, a declaration, then a call" \
    'var a = 1;' 'function f(n) { return n + a; }' 'f(41)'
arm JL-04 42 "a function declared over three lines, then a call" \
    'function f(n) {' '    return n * 2;' '}' 'f(21)'
arm JL-05 42 "two declarations, then a call" \
    'function g(n) { return n + 1; }' 'function f(n) { return g(n) * 2; }' 'f(20)'
arm JL-06 84 "a function given a callback" \
    'function process(callback) { return callback(42); }' 'process(x => x * 2);'
arm JL-07 1307674368000 "a recursive factorial(15)" \
    'function factorial(n) {' '    if (n <= 1) return 1;' '    return n * factorial(n - 1);' '}' 'factorial(15)'
arm JL-C1 42 "CONTROL: the result assigned, the name last" \
    'function f(n) { return n * 2; }' 'var r = f(21);' 'r'
arm JL-C2 42 "CONTROL: an arrow function" \
    'const f = (n) => n * 2;' 'f(21)'
arm JL-C3 '{"a":1,"b":2}' "CONTROL: a multi-line object literal last" \
    'const a = 1;' 'JSON.stringify({' '    a: a,' '    b: 2' '});'
arm JL-C4 null "CONTROL: a block ending in a for loop is a statement" \
    'var s = 0;' 'for (var i = 0; i < 3; i++) {' '    s += i;' '}'
arm JL-C5 null "CONTROL: a block ending in if/else is a statement" \
    'var s = 1;' 'if (s > 0) {' '    s = 2;' '} else {' '    s = 3;' '}'
arm JL-08 null "a block whose last statement is a declaration runs (it was a SyntaxError)" \
    'var a = 1;' 'const b = [' '    a, 2' ']'
arm JL-09 null "the same, a one-line declaration last" \
    'var a = 1;' 'let b = a + 1'
# A newline before `(` is not a statement boundary: this is ONE statement, an
# immediately invoked function assigned to b, and the block's value is null. A
# split at that newline would compile and return 41 -- other code.
arm JL-10 null "a newline before ( continues the statement (an IIFE): not split" \
    'var b = function(x) { return x + 1; }' '(41)'

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
