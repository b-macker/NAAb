#!/usr/bin/env bash
# ============================================================
# test_naab_comment_apostrophe.sh -- an apostrophe in a NAAb comment cannot
# hide code from NAAb's own text checks
#
# checkNaabFunctionBody() strips string literals from a function body before
# the placeholder / oversimplification / incomplete-logic checks read it. The
# stripper knew only // and /* */ as comments, but NAAb's lexer also takes
# # (Lexer::skipComment), and NAAb has single-quoted strings. So an
# apostrophe in an ordinary # comment ("# don't") opened a "string" that
# swallowed the code up to the next apostrophe. Measured before the fix: a
# // TODO between "# don't" and "# it's" ran with exit 0 under a HARD
# no_placeholders config, where the same TODO is otherwise exit 3. NAAb's
# comment forms now come from the language table (naab/language_descriptors.h).
#
#   NA-00  CONTROL: the TODO in an ordinary comment block is refused (exit 3)
#          -- the config bites at all
#   NA-01  between "# don't" and "# it's": still refused (the bypass)
#   NA-02  between "// don't" and "// it's": still refused
#   NA-03  after a "/* don't */" block comment and before "# it's": refused
#   NA-04  CONTROL: the same TODO inside a real string literal is NOT refused
#          -- strings are still stripped; without this arm a fix that stopped
#          stripping strings altogether would pass NA-01..03
# Every arm runs on the VM and on the tree-walker.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -5 | sed 's/^/       | /'; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
cat > "$W/govern.json" <<'EOF'
{
  "mode": "enforce",
  "code_quality": { "no_placeholders": { "enabled": true, "level": "hard" } }
}
EOF

# $1 = id, $2 = line before, $3 = the TODO line, $4 = line after, $5 = expected exit
probe() {
    local id="$1" before="$2" mid="$3" after="$4" want="$5" eng rc out
    printf 'fn helper() {\n    %s\n    %s\n    %s\n    return 1\n}\nmain {\n    print(helper())\n}\n' \
        "$before" "$mid" "$after" > "$W/$id.naab"
    for eng in vm tree-walk; do
        local flag=""; [ "$eng" = "tree-walk" ] && flag="--tree-walk"
        out="$(cd "$W" && "$NAAB" $flag "$id.naab" 2>&1)"; rc=$?
        if [ "$rc" -eq "$want" ]; then
            ok "$id/$eng" "exit $rc"
        else
            bad "$id/$eng" "exit $rc, expected $want" "$out"
        fi
    done
}

echo "=== NAAb comment apostrophes cannot hide code ==="
probe NA-00 "# do not change this"  "// TODO implement this properly" "# fine"          3
probe NA-01 "# don't change this"   "// TODO implement this properly" "# it's fine"     3
probe NA-02 "// don't change this"  "// TODO implement this properly" "// it's fine"    3
probe NA-03 "/* don't change */"    "// TODO implement this properly" "# it's fine"     3
probe NA-04 "# do not change this"  'let s = "TODO implement this properly"' "# fine"   0

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
