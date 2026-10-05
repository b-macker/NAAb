#!/usr/bin/env bash
# ============================================================
# test_runtime_pin_engines.sh -- runtime_versions pins hold on every engine
# and execution path
#
# Pins shipped (5d08322c) in the commit that made the VM the default engine,
# but were checked only on the tree-walker's polyglot path. On master a
# `runtime_versions` pin was never consulted on the VM, nor through codegen,
# and a pin on a runtime whose executor reports no version (everything but
# Python and SQL) passed silently on both engines. The check now runs inside
# checkPolyglotBlock(), which every execution path calls; an unreportable
# version is reported at the pin's level, never treated as a pass.
#
# SQL is the measured runtime: it runs in-process on every platform and
# reports its version. JavaScript (in-process QuickJS) reports none.
#
#   RP-00  CONTROL: with no pin, <<sql>> and <<javascript>> run (both engines)
#   RP-01  a hard pin SQL cannot meet blocks <<sql>> (exit 3, not run), both
#          engines; RP-01c: a pin it meets lets the block run
#   RP-02  a soft pin it cannot meet blocks too (exit 3), both engines
#   RP-03  an advisory pin warns and the block runs
#   RP-04  a hard pin on javascript, which reports no version, blocks with
#          "cannot be determined" (master ran it on both engines)
#   RP-05  the pin holds through codegen.run("sql"); RP-05c: a met pin runs
#   RP-06  a pin under "sql" holds for <<sqlite>> (one runtime, two tags)
#   RP-07  text-only checks do not judge the runtime: naab-gov check reports
#          no runtime_version finding; RP-07c: the same naab-gov does apply
#          the config it was given (a block list on sql fires)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
GOV="$REPO/build/naab-gov"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== runtime_versions pins, every engine ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    skip RP-00 "naab-lang not built -- UNMEASURABLE"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT

PROG_SQL=$'main {\n    let r = <<sql\nSELECT 41 + 1 AS v\n>>\n    print("RAN")\n}'
PROG_SQLITE=$'main {\n    let r = <<sqlite\nSELECT 41 + 1 AS v\n>>\n    print("RAN")\n}'
PROG_JS=$'main {\n    let r = <<javascript\n1 + 1\n>>\n    print("RAN")\n}'
PROG_CG=$'use codegen\nmain {\n    let r = codegen.run("sql", "SELECT 1 AS v")\n    print("RAN")\n}'

mk() {  # $1 = dir, $2 = runtime_versions JSON array, $3 = program, [$4 = extra top-level JSON]
    mkdir -p "$W/$1"
    printf '{"mode":"enforce"%s,"runtime_versions":%s}\n' "${4:+,$4}" "$2" > "$W/$1/govern.json"
    printf '%s\n' "$3" > "$W/$1/p.naab"
}
run() {  # $1 = dir, $2 = "" or --tree-walk ; sets OUT and RC
    OUT="$(cd "$W/$1" && "$NAAB" ${2:+$2} p.naab --timeout 20 2>&1)"; RC=$?
}
ran() { [[ "$OUT" == *$'\nRAN'* || "$OUT" == RAN* ]]; }
pin() { printf '[{"language":"%s","required":"%s","level":"%s"}]' "$1" "$2" "$3"; }

mk c_sql '[]' "$PROG_SQL"
mk c_js  '[]' "$PROG_JS"
mk h_sql "$(pin sql '>=999' hard)" "$PROG_SQL"
mk m_sql "$(pin sql '>=0' hard)" "$PROG_SQL"
mk s_sql "$(pin sql '>=999' soft)" "$PROG_SQL"
mk a_sql "$(pin sql '>=999' advisory)" "$PROG_SQL"
mk h_js  "$(pin javascript '18' hard)" "$PROG_JS"
mk h_lite "$(pin sql '>=999' hard)" "$PROG_SQLITE"

JS_OK=1
for eng in "" --tree-walk; do
    e=${eng:-vm}; e=${e#--}
    run c_sql "$eng"
    if [ $RC -ne 0 ] || ! ran; then
        bad "RP-00/$e" "unpinned <<sql>> did not run (exit $RC) -- every arm below is void" "$OUT"
        continue
    fi
    run c_js "$eng"
    if [ $RC -eq 0 ] && ran; then ok "RP-00/$e" "with no pin, <<sql>> and <<javascript>> run"
    else ok "RP-00/$e" "with no pin, <<sql>> runs (javascript unavailable here)"; JS_OK=0; fi

    run h_sql "$eng"
    if [ $RC -eq 3 ] && ! ran && [[ "$OUT" == *"Runtime version mismatch for sql"* ]]; then
        ok "RP-01/$e" "an unmet hard pin blocks <<sql>> (exit 3, not run)"
    else bad "RP-01/$e" "an unmet hard pin did not block (exit $RC)" "$OUT"; fi
    run m_sql "$eng"
    if [ $RC -eq 0 ] && ran; then ok "RP-01c/$e" "a met hard pin lets <<sql>> run"
    else bad "RP-01c/$e" "a pin SQL satisfies blocked it (exit $RC) -- RP-01 would prove nothing" "$OUT"; fi

    run s_sql "$eng"
    if [ $RC -eq 3 ] && ! ran; then ok "RP-02/$e" "an unmet soft pin blocks (exit 3)"
    else bad "RP-02/$e" "an unmet soft pin did not block (exit $RC)" "$OUT"; fi

    run a_sql "$eng"
    if [ $RC -eq 0 ] && ran && [[ "$OUT" == *"Runtime version mismatch for sql"* ]]; then
        ok "RP-03/$e" "an unmet advisory pin warns and runs"
    else bad "RP-03/$e" "advisory pin: expected a warning and a run (exit $RC)" "$OUT"; fi

    if [ $JS_OK -eq 1 ]; then
        run h_js "$eng"
        if [ $RC -eq 3 ] && ! ran && [[ "$OUT" == *"cannot be determined"* ]]; then
            ok "RP-04/$e" "a pin on a runtime that reports no version blocks (exit 3)"
        else bad "RP-04/$e" "an unverifiable pin passed (exit $RC)" "$OUT"; fi
    else
        skip "RP-04/$e" "javascript executor unavailable -- UNMEASURABLE"
    fi

    run h_lite "$eng"
    if [ $RC -eq 3 ] && ! ran; then ok "RP-06/$e" "a pin under \"sql\" blocks <<sqlite>>"
    else bad "RP-06/$e" "a pin under sql did not hold for <<sqlite>> (exit $RC)" "$OUT"; fi
done

# codegen: enforce upgrades the sandbox to standard, which refuses codegen
# outright -- elevated lets the pin be the thing that decides.
CG='"security":{"sandbox_level":"elevated"},"codegen":{"enabled":true,"level":"hard"}'
mk g_unmet "$(pin sql '>=999' hard)" "$PROG_CG" "$CG"
mk g_met   "$(pin sql '>=0' hard)" "$PROG_CG" "$CG"
run g_met ""
if [ $RC -ne 0 ] || ! ran; then
    bad RP-05c "codegen.run(\"sql\") did not run under a met pin (exit $RC) -- RP-05 is void" "$OUT"
else
    ok RP-05c "codegen.run(\"sql\") runs under a met pin"
    run g_unmet ""
    if [ $RC -ne 0 ] && ! ran && [[ "$OUT" == *"Runtime version mismatch for sql"* ]]; then
        ok RP-05 "an unmet pin stops codegen.run(\"sql\") (exit $RC)"
    else bad RP-05 "codegen.run ignored the pin (exit $RC)" "$OUT"; fi
fi

# naab-gov check judges code text; it runs nothing, so it has no runtime.
if [ -x "$GOV" ] || [ -x "$GOV.exe" ]; then
    o7="$(printf 'SELECT 1;\n' | "$GOV" check --language sql --config-string \
        '{"mode":"enforce","runtime_versions":[{"language":"sql","required":">=999","level":"hard"}]}' 2>&1)"
    o7c="$(printf 'SELECT 1;\n' | "$GOV" check --language sql --config-string \
        '{"mode":"enforce","languages":{"blocked":["sql"]}}' 2>&1)"
    if [[ "$o7c" == *languages.blocked* ]]; then
        ok RP-07c "naab-gov check applies the config it is given"
        case "$o7" in *runtime_version*) bad RP-07 "naab-gov check judged a runtime it does not run" "$o7" ;;
            *) ok RP-07 "naab-gov check reports no runtime_version finding" ;; esac
    else
        bad RP-07c "naab-gov check did not apply a block list -- RP-07 would prove nothing" "$o7c"
    fi
else
    skip RP-07 "naab-gov not built -- UNMEASURABLE"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
