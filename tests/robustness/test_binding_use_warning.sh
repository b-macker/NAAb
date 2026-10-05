#!/usr/bin/env bash
# ============================================================
# test_binding_use_warning.sh -- "bound variable is never used" fires only
# for bindings that really are unused
#
# The tree-walker warns when a <<lang[x]>> binding never appears in the block.
# It searched a copy of the block with every string literal removed -- right
# for its sibling check (a name inside a string is not a use of an UNBOUND
# NAAb variable), wrong here: most languages read a bound value INSIDE a
# string, so a block that used its binding was told it never did:
#   shell "$x"   python f"{x}"   ruby "#{x}"   php "$x"
# The use check now searches the code as written. (The VM emits neither
# binding warning; these arms are tree-walker only.)
#
#   BW-01  a binding referenced only inside an interpolating string draws no
#          warning, per language (each UNMEASURABLE where not registered)
#   BW-01c CONTROL: the block really received the value -- without it BW-01
#          passes for a block that never ran
#   BW-02  CONTROL: a binding the block never mentions still warns -- without
#          it BW-01 passes for a build that dropped the warning entirely
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== bound-variable use warning ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    skip BW-01 "naab-lang not built -- UNMEASURABLE"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
printf '%s\n' '{"mode":"enforce","security":{"sandbox_level":"elevated"}}' > "$W/govern.json"
mkdir -p "$W/reg"
printf 'use codegen\nmain {\n    for x in codegen.supported_languages() { print(x) }\n}\n' > "$W/reg/r.naab"
REG=" $(cd "$W/reg" && "$NAAB" r.naab --no-governance 2>/dev/null | tr -d '\r' | tr '\n' ' ') "

run() {  # $1 = program; sets OUT, RC (tree-walker: the engine that warns)
    printf '%s\n' "$1" > "$W/p.naab"
    OUT="$(cd "$W" && "$NAAB" --tree-walk p.naab --timeout 60 2>&1)"; RC=$?
}

measured=0
for spec in 'shell|echo "v=$who"|v=BOUNDVAL' \
            'python|f"v={who}"|v=BOUNDVAL' \
            'ruby|"v=#{who}"|v=BOUNDVAL' \
            'php|echo "v=$who";|v=BOUNDVAL'; do
    IFS='|' read -r lang code want <<< "$spec"
    if [[ "$REG" != *" $lang "* ]]; then
        skip "BW-01/$lang" "no $lang executor registered -- UNMEASURABLE"; continue
    fi
    run "main {
    let who = \"BOUNDVAL\"
    let r = <<$lang[who]
$code
>>
    print(\"R=\" + r)
}"
    if [[ "$OUT" != *"R=$want"* ]]; then
        skip "BW-01/$lang" "the $lang block did not return the bound value here (exit $RC) -- UNMEASURABLE"; continue
    fi
    measured=$((measured+1))
    ok "BW-01c/$lang" "the block read the bound value ($want)"
    case "$OUT" in
        *"Bound variable 'who' is never used"*) bad "BW-01/$lang" "a binding used inside a string was reported unused" "$OUT" ;;
        *) ok "BW-01/$lang" "no false 'never used' warning for $code" ;;
    esac
done
[ $measured -gt 0 ] || skip BW-01 "no interpolating language measurable here -- UNMEASURABLE"

# BW-02: the warning still fires for a binding that is really unused.
if [[ "$REG" == *" sql "* ]]; then
    run $'main {\n    let spare = 1\n    let r = <<sql[spare]\nSELECT 7 AS v\n>>\n    print("DONE")\n}'
    case "$OUT" in
        *"Bound variable 'spare' is never used"*) ok BW-02 "an unused binding still warns" ;;
        *) bad BW-02 "an unused binding drew no warning -- BW-01 would prove nothing" "$OUT" ;;
    esac
else
    skip BW-02 "no sql executor registered -- UNMEASURABLE"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
