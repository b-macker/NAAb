#!/usr/bin/env bash
# test_error_redaction.sh -- the display sanitizer leaves the engine's own help
# alone and keeps redacting values (A39(a), the owner's decision 8).
#
# ErrorSanitizer::redactValues() replaced every `name = "value"` and
# `key: 'value'` with <redacted>, and the fatal catch sites run every message
# through it, so the fix an error message teaches was damaged (observed on
# d32f8603): a HARD block printed `Rule: code_quality.no_placeholders =
# <redacted>`, and the `sprintf` hint printed `let msg = <redacted> + name +
# "!"`. Now the two generic value patterns (and QUOTED_VALUE, a keyword
# pattern) are skipped in a "Help:" or "Example:" section (to the next blank
# line) and on the "Rule:" line; the undefined-name hints of both engines are
# put under a "Help:" heading. The secret shapes (API keys, emails, card
# numbers, addresses) still apply to every line.
#
#   ER-01   a HARD block's Rule line shows its value.
#   ER-02   the sprintf hint's example keeps its code (VM); ER-02tw the
#           tree-walker's copy of the hint table (a name read as a variable),
#           outside any govern.json.
#   ER-03   CONTROL: a value the program put in its own message, outside any
#           help section, is still redacted.
#   ER-04   CONTROL: a key-shaped value in a "Help:" section the PROGRAM wrote
#           is still redacted (the secret shapes apply in every section).
#
# Kill table (mutants of src/runtime/error_sanitizer.cpp and the hint sites):
#   E1 no engine-section rule (every line redacted)    -> ER-01, ER-02
#   E2 sections skip the secret shapes too             -> ER-04
#   E3 the VM hint not under a heading                 -> ER-02
#   E4 the tree-walker hint not under a heading        -> ER-02tw
#   E5 the Rule line not exempt                        -> ER-01

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_redact.XXXXXX")"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -12 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

mkdir -p "$W/gov" "$W/free"
printf '{"version":"3.0","mode":"enforce","code_quality":{"no_placeholders":{"enabled":true,"level":"hard"}}}\n' > "$W/gov/govern.json"
printf 'fn work(x) {\n    // TODO: finish this\n    return x\n}\nmain {\n    print(work(1))\n}\n' > "$W/gov/p.naab"
OUT=$(cd "$W/gov" && "$NAAB" run p.naab 2>&1); RC=$?
if [ "$RC" -eq 3 ] && grep -qF 'Rule: code_quality.no_placeholders = "hard"' <<<"$OUT"; then
    ok ER-01 "a HARD block's Rule line shows its value"
else bad ER-01 "expected Rule: code_quality.no_placeholders = \"hard\" (rc=$RC)" "$OUT"; fi

printf 'main {\n    let name = "x"\n    let s = sprintf("Hello %%s", name)\n    print(s)\n}\n' > "$W/free/s.naab"
OUT=$(cd "$W/free" && "$NAAB" run s.naab --no-governance 2>&1); RC=$?
if [ "$RC" -ne 0 ] && grep -qF 'let msg = "Hello " + name + "!"' <<<"$OUT" && grep -q '^  Help:$' <<<"$OUT"; then
    ok ER-02 "the sprintf hint's example keeps its code (VM)"
else bad ER-02 "the example was redacted or the hint missing (rc=$RC)" "$OUT"; fi
# The tree-walker reports a CALLED unknown name through its own (already
# headed) "Undefined function" message; the hint table is its variable lookup.
printf 'main {\n    let f = sprintf\n    print(f)\n}\n' > "$W/free/v.naab"
OUT=$(cd "$W/free" && "$NAAB" run v.naab --no-governance --tree-walk 2>&1); RC=$?
if [ "$RC" -ne 0 ] && grep -qF 'let msg = "Hello " + name + "!"' <<<"$OUT"; then
    ok ER-02tw "the sprintf hint's example keeps its code (tree-walker)"
else bad ER-02tw "the example was redacted or the hint missing (rc=$RC)" "$OUT"; fi

printf 'main {\n    let p = "hunter2hunter2"\n    throw "bad config: password = \\"" + p + "\\""\n}\n' > "$W/free/t1.naab"
OUT=$(cd "$W/free" && "$NAAB" run t1.naab --no-governance 2>&1); RC=$?
if [ "$RC" -ne 0 ] && ! grep -q 'hunter2hunter2' <<<"$OUT" && grep -q 'redacted' <<<"$OUT"; then
    ok ER-03 "CONTROL: a value in the program's own message is still redacted"
else bad ER-03 "the value was shown (rc=$RC)" "$OUT"; fi

printf 'main {\n    let k = "abcdefghij0123456789"\n    throw "oops\\n\\n  Help:\\n  - token: " + k\n}\n' > "$W/free/t2.naab"
OUT=$(cd "$W/free" && "$NAAB" run t2.naab --no-governance 2>&1); RC=$?
if [ "$RC" -ne 0 ] && ! grep -q 'abcdefghij0123456789' <<<"$OUT" && grep -q '^  Help:$' <<<"$OUT"; then
    ok ER-04 "CONTROL: a key-shaped value in a program-written Help section is still redacted"
else bad ER-04 "the key-shaped value was shown (rc=$RC)" "$OUT"; fi

echo
echo "error redaction: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
