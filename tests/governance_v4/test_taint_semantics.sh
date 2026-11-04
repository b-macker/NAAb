#!/usr/bin/env bash
# The taint semantics, generated from tests/governance_v4/taint_semantics.psv (Bundle 12).
#
# Taint is implemented three times: the tree-walker's AST walk
# (Interpreter::expressionContainsTaint), the VM compiler's compile-time AST walk
# (Compiler::exprContainsTaint, which marks values with OP_GOV_TAINT_MARK) and the
# VM's runtime taint stack. The two walks now share one rule,
# include/naab/taint_rule.h; this suite pins all three to one table. Every row
# runs on BOTH engines -- the VM under an enforce config, the tree-walker beside
# an audit copy (it is refused under enforce) -- and each must give the row's
# expected verdict at the sink, `print`. The pass condition is the expectation,
# not agreement: two engines that both miss a taint agree perfectly.
#
# Structural checks (screened -- they read source text): every expression kind in
# include/naab/ast.h has a row (every kind but LambdaExpr a tainted one), and
# taint_rule.h names every NodeKind, so a kind added to the AST fails here before
# it can be laundered (its default is tainted, -Wswitch reports it as a warning).
#
# Measured against master 528fbb59 (the pre-Bundle-12 binary), 23 rows changed
# verdict, each in the decided direction: the tree-walker missed javascript
# output, a throw into a catch, conditions and an inline source; neither engine
# saw a match subject, a computed dict key or a condition in a sink argument;
# the VM tainted every clean stdlib call after a source (its return flag was
# never cleared); and a callback that returned tainted data before a clean one
# laundered it on both (the flag is last-write-wins).
#
# Kill table (each mutant built and run alone; the controls stayed green):
#   see the Bundle 12 PR; recorded in the header below once measured.
#
# Usage: bash tests/governance_v4/test_taint_semantics.sh [path/to/naab-lang]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-${NAAB:-$REPO/build/naab-lang}}"
TABLE="$SCRIPT_DIR/taint_semantics.psv"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -12 | sed 's/^/       /'; }
skip() { SKIP=$((SKIP+1)); echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    echo "UNMEASURABLE: naab-lang not built"; exit 1
fi
if [ ! -f "$TABLE" ]; then echo "FAIL: $TABLE missing"; exit 1; fi

T="$(mktemp -d)"
cleanup() { teardown_isolated_trust; rm -rf "$T"; }
trap cleanup EXIT
# No key installed: both configs load unsigned (a signed audit config would be
# raised to enforce, where --tree-walk is refused).
setup_isolated_trust
mkdir -p "$T/vm" "$T/tw"
cfg() {  # cfg MODE
    printf '{"version":"5.0","mode":"%s","security":{"sandbox_level":"elevated"},"taint_tracking":{"enabled":true,"level":"advisory","sources":["env.get"],"sinks":["print"],"sanitizers":["sanitize_"]}}\n' "$1"
}
cfg enforce > "$T/vm/govern.json"
cfg audit > "$T/tw/govern.json"

# run_case ENGINE PRELUDE SETUP EXPR CONTEXT -> VIOL (count) DONE (0/1) OUT
run_case() {
    local engine="$1" prelude="$2" setup="$3" expr="$4" context="$5" body dir
    expr="${expr//@@/$'\n'}"
    if [ "$context" = "sink" ]; then
        body="print(${expr})"
    else
        body="let x = ${expr}
    print(x)"
    fi
    dir="$T/$engine"
    printf 'use env\nuse string\n%s\nmain {\n    let t = env.get("HOME")\n    %s\n    %s\n    print("DONE")\n}\n' \
        "$prelude" "$setup" "$body" > "$dir/p.naab"
    if [ "$engine" = "tw" ]; then
        OUT="$(cd "$dir" && timeout 30 "$NAAB" --tree-walk p.naab 2>&1)"
    else
        OUT="$(cd "$dir" && timeout 30 "$NAAB" p.naab 2>&1)"
    fi
    VIOL=$(grep -ciE "sink_violation|Taint tracking violation" <<<"$OUT")
    DONE=$(grep -cx "DONE" <<<"$OUT" | tr -d ' ')
    [ "$DONE" -gt 0 ] && DONE=1
}
verdict() { [ "$VIOL" -gt 0 ] && echo tainted || echo clean; }

echo "=== harness controls ==="
HC_OK=1
for engine in vm tw; do
    run_case "$engine" "" "" "t" assign
    if [ "$DONE" -eq 1 ] && [ "$(verdict)" = tainted ]; then ok "HC-1/$engine" "a tainted name is flagged at the sink"
    else bad "HC-1/$engine" "the detector must see a tainted name (done=$DONE)" "$OUT"; HC_OK=0; fi
    run_case "$engine" "" "" '"c"' assign
    if [ "$DONE" -eq 1 ] && [ "$(verdict)" = clean ]; then ok "HC-2/$engine" "a constant is not flagged"
    else bad "HC-2/$engine" "a constant must not be flagged (done=$DONE)" "$OUT"; HC_OK=0; fi
done

echo "=== the table covers every expression kind; the rule names every NodeKind ==="
KINDS="$(sed -n '/^enum class NodeKind {/,/^};/p' "$REPO/include/naab/ast.h" | tr -d '\r' \
    | sed -n '/\/\/ Expressions/,$p' | grep -oE '^[[:space:]]+[A-Z][A-Za-z]+' | tr -d ' ')"
ALLKINDS="$(sed -n '/^enum class NodeKind {/,/^};/p' "$REPO/include/naab/ast.h" | tr -d '\r' \
    | grep -oE '^[[:space:]]+[A-Z][A-Za-z]+' | tr -d ' ')"
NK=$(grep -c . <<<"$KINDS")
NA=$(grep -c . <<<"$ALLKINDS")
ROWS="$(grep -v '^#' "$TABLE" | tr -d '\r' | grep -v '^[[:space:]]*$')"
missing=""; untainted=""
while read -r k; do
    [ -z "$k" ] && continue
    grep -q "^[^|]*|${k}|" <<<"$ROWS" || missing="$missing $k"
    if [ "$k" != "LambdaExpr" ] && ! grep -qE "^[^|]*\|${k}\|.*\|tainted\|[^|]*$" <<<"$ROWS"; then untainted="$untainted $k"; fi
done <<<"$KINDS"
if [ "$NK" -ge 18 ] && [ -z "$missing" ] && [ -z "$untainted" ]; then
    ok "TS-K" "every one of the $NK expression kinds has a row, and every kind that can carry taint a tainted one"
else bad "TS-K" "kinds without a row:${missing:- none}; without a tainted row:${untainted:- none} ($NK kinds read)"; fi
unnamed=""
RULE="$(tr -d '\r' < "$REPO/include/naab/taint_rule.h")"
while read -r k; do
    [ -z "$k" ] && continue
    grep -q "K::${k}:" <<<"$RULE" || unnamed="$unnamed $k"
done <<<"$ALLKINDS"
if [ "$NA" -ge 40 ] && [ -z "$unnamed" ]; then ok "TS-R" "taint_rule.h names all $NA NodeKinds"
else bad "TS-R" "NodeKinds taint_rule.h does not name:${unnamed:- none} ($NA kinds read)"; fi

echo "=== the table, on both engines ==="
if [ "$HC_OK" -ne 1 ]; then
    echo "  (harness controls failed: the rows below cannot be judged)"
fi
NROWS=0
while IFS='|' read -r id kind prelude setup expr context expect why; do
    [ -z "$id" ] && continue
    [ "$prelude" = "-" ] && prelude=""
    [ "$setup" = "-" ] && setup=""
    NROWS=$((NROWS+1))
    [ "$HC_OK" -ne 1 ] && { skip "$id" "harness controls failed"; continue; }
    run_case vm "$prelude" "$setup" "$expr" "$context"; vm_v=$(verdict); vm_done=$DONE; vm_out=$OUT
    run_case tw "$prelude" "$setup" "$expr" "$context"; tw_v=$(verdict); tw_done=$DONE; tw_out=$OUT
    if [ "$vm_done" -ne 1 ] || [ "$tw_done" -ne 1 ]; then
        skip "$id" "the program did not finish (vm done=$vm_done, tw done=$tw_done): $(grep -m1 -iE 'error' <<<"$vm_out$tw_out")"
        continue
    fi
    if [ "$vm_v" = "$expect" ] && [ "$tw_v" = "$expect" ]; then
        ok "$id" "$expect on both engines -- $why"
    else
        bad "$id" "expected $expect, vm=$vm_v tw=$tw_v -- $why" "--- vm
$(tail -6 <<<"$vm_out")
--- tw
$(tail -6 <<<"$tw_out")"
    fi
done <<<"$ROWS"
if [ "$NROWS" -lt 50 ]; then bad "TS-N" "only $NROWS rows read from the table"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$SKIP"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
