#!/usr/bin/env bash
# ============================================================
# test_block_failure_parity.sh -- a polyglot block that fails, fails the
# program, in every language
#
# Most executors stop the program when a block fails (exit 1). Four did not,
# on both engines and in both forms:
#   - typescript/ts, php (GenericSubprocessExecutor): the exit status was
#     never read -- a missing `tsx`, a PHP fatal error or a Python exception
#     became the block's value (usually null) under exit 0;
#   - cpp: a compile error was printed and the block became null; a failed
#     STATEMENT block's bool was ignored;
#   - csharp/cs: an outer catch(std::exception) returned null for every
#     exception, its own compile error and GovernanceHardError included.
# And php never ran as PHP at all unless the block wrote `<?php` itself: the
# engines added the opener only when they injected bindings, so `echo 1 + 1;`
# "returned" the string "echo 1 + 1;" and file_put_contents() wrote nothing.
#
# The language list comes from the binary (codegen.supported_languages()), so
# an executor registered tomorrow is held to the same contract.
#
#   BF-00  CONTROL: valid sql, javascript and php blocks run (exit 0) on both
#          engines -- without it every arm below passes for an engine that
#          fails everything
#   BF-01  every registered language: a broken EXPRESSION block exits 1 and
#          the code after it does not run, both engines
#   BF-02  the same for a broken STATEMENT block
#   BF-03  php without `<?php` runs as PHP: `echo 1 + 1;` yields 2, and a
#          statement block writes its file
#   BF-03c CONTROL: a PHP template (text before a <?php tag) is left as
#          written -- the opener is added only when the block has none
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== a failed polyglot block fails the program ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    skip BF-00 "naab-lang not built -- UNMEASURABLE"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
# elevated: under the enforce default (standard) subprocess languages are
# refused by the sandbox, which would fail every block for the wrong reason.
printf '%s\n' '{"mode":"enforce","security":{"sandbox_level":"elevated"}}' > "$W/govern.json"

run() {  # $1 = engine flag or "", $2 = program text; sets OUT, RC
    printf '%s\n' "$2" > "$W/p.naab"
    OUT="$(cd "$W" && "$NAAB" ${1:+$1} p.naab --timeout 60 2>&1)"; RC=$?
}

langs="$(mkdir -p "$W/reg" && printf 'use codegen\nmain {\n    for x in codegen.supported_languages() { print(x) }\n}\n' > "$W/reg/r.naab" \
         && cd "$W/reg" && "$NAAB" r.naab --no-governance 2>/dev/null | tr -d '\r')"
if ! printf '%s\n' "$langs" | grep -qx sql; then
    bad BF-00 "could not list the registered languages" "$langs"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 1
fi

# --- BF-00: controls ---
c0=""
for eng in "" --tree-walk; do
    e=${eng:-vm}; e=${e#--}
    run "$eng" $'main {\n    let a = <<sql\nSELECT 41 + 1 AS v\n>>\n    print("SQL_OK")\n}'
    [ $RC -eq 0 ] && [[ "$OUT" == *SQL_OK* ]] || c0+=" sql/$e(rc=$RC)"
    if printf '%s\n' "$langs" | grep -qx javascript; then
        run "$eng" $'main {\n    let a = <<javascript\n1 + 1\n>>\n    print("JS=" + a)\n}'
        [ $RC -eq 0 ] && [[ "$OUT" == *"JS=2"* ]] || c0+=" javascript/$e(rc=$RC)"
    fi
done
[ -z "$c0" ] && ok BF-00 "valid blocks run on both engines" \
             || bad BF-00 "a valid block failed -- every arm below would prove nothing" "$c0"$'\n'"$OUT"

# --- BF-01 / BF-02: every registered language, both forms, both engines ---
# The marker is assembled at run time: the tree-walker's error report quotes
# the surrounding source, so a literal "AFTER_EXPR" would appear in the output
# without the line ever running.
f1=""; f2=""; n=0
while read -r l; do
    [ -n "$l" ] || continue
    n=$((n+1))
    for eng in "" --tree-walk; do
        e=${eng:-vm}; e=${e#--}
        run "$eng" "main {
    let r = <<$l
@@@ !!! not valid in any language
>>
    print(\"AFTER_\" + \"EXPR\")
}"
        [ $RC -eq 1 ] && [[ "$OUT" != *AFTER_EXPR* ]] || f1+=" $l/$e(rc=$RC)"
        run "$eng" "main {
    <<$l
@@@ !!! not valid in any language
>>
    print(\"AFTER_\" + \"STMT\")
}"
        [ $RC -eq 1 ] && [[ "$OUT" != *AFTER_STMT* ]] || f2+=" $l/$e(rc=$RC)"
    done
done <<< "$langs"
[ -z "$f1" ] && ok BF-01 "a broken expression block stops the program in all $n registered languages" \
             || bad BF-01 "a broken expression block was swallowed" "$f1"
[ -z "$f2" ] && ok BF-02 "a broken statement block stops the program in all $n registered languages" \
             || bad BF-02 "a broken statement block was swallowed" "$f2"

# --- BF-03: php runs as php without writing <?php ---
if ! printf '%s\n' "$langs" | grep -qx php || ! command -v php >/dev/null 2>&1; then
    skip BF-03 "php not registered or not installed -- UNMEASURABLE"
else
    f3=""; f3c=""
    for eng in "" --tree-walk; do
        e=${eng:-vm}; e=${e#--}
        rm -f "$W/php_ran.txt"
        run "$eng" $'main {\n    let r = <<php\necho 1 + 1;\n>>\n    print("PHP=" + r)\n    <<php\nfile_put_contents("php_ran.txt", "x");\n>>\n}'
        { [ $RC -eq 0 ] && [[ "$OUT" == *"PHP=2"* ]] && [ -f "$W/php_ran.txt" ]; } || f3+=" $e(rc=$RC)"
        run "$eng" $'main {\n    let t = <<php\nHello <?php echo "T"; ?>\n>>\n    print("TPL=" + t)\n}'
        { [ $RC -eq 0 ] && [[ "$OUT" == *"TPL=Hello T"* ]]; } || f3c+=" $e(rc=$RC)"
    done
    [ -z "$f3" ] && ok BF-03 "php blocks run as PHP without <?php (value 2, file written)" \
                 || bad BF-03 "a php block without <?php did not run as PHP" "$f3"$'\n'"$OUT"
    [ -z "$f3c" ] && ok BF-03c "a PHP template keeps its leading text" \
                  || bad BF-03c "a PHP template was altered" "$f3c"$'\n'"$OUT"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
