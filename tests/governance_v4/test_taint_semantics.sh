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
# Measured against master 528fbb59 (the binary built before Bundle 12): 52 of
# the 156 row checks fail, each in the direction the table decides (A117). The
# tree-walker missed javascript output, a throw into a catch, conditions, an
# inline source and a generator yielding a source; neither engine saw a match
# subject, a computed dict key or a condition in a sink argument; the VM tainted
# every clean stdlib call after a source (its return flag was never cleared); a
# callback that returned tainted data before a clean one laundered it on both
# (the flag is last-write-wins); and under the callback origin the VM's runtime
# dropped taint through interpolation, dict keys, conditions, async calls,
# generators and any call whose body ignored its tainted argument.
#
# Kill table: each mutant applied alone, built and run; the rows it turned red
# (every other row and every control stayed green). M1-M12 were run before the
# callback origin existed, so they list source-origin rows only.
#   The static rule (include/naab/taint_rule.h) and its callers:
#   M1  if-condition dropped from the rule      if_cond, if_cond_sink
#   M2  match subject dropped                   match_subject
#   M3  match guard dropped                     match_guard
#   M4  throw carries nothing                   throw_catch (tw; the VM carries a thrown value's taint itself)
#   M5  a polyglot block is clean               inline_js_sink (tw; the VM marks polyglot output at run time)
#   M6  dict keys dropped                       dict_key
#   M7a a source call is undecided              10 rows on the vm or tw (lit_interp, call_source_inline, dict_key, ...)
#   M7b a sanitizer call is undecided           call_sanitized_inline
#   M8  VM return flag not cleared per call     call_clean_after_source, call_clean_after_gap, call_math_clean (vm)
#   M9  VM no compile-time mark on arguments    if_cond_sink (vm)
#   M10a VM callback accumulator not set        cb_last_clean, cb_nested (vm)
#   M10b tw callback accumulator not set        cb_last_clean, cb_nested (tw)
#   M11 rule fails open on an unknown kind      tests/unit/taint_rule_test.cpp (UnknownKind..., StatementKind...)
#   M12 tw parallel path does not mark          inline_js, inline_js_bound (tw)
#   The VM runtime, under the callback origin unless noted:
#   R1  module-as-function drops arg taint      lit_interp, call_string_module
#   R2  OP_DICT ignores keys                    dict_key, dict_key_only
#   R3  condition not saved                     if_cond, if_cond_sink, if_nested, if_inner_cond, match_subject_binding
#   R4  pattern/guard not added                 match_guard, match_list_guard
#   R5  saved condition not applied             the if rows and match_subject, match_guard, match_list_guard
#   R6  handler restore (run()'s catch)         if_cond_unwind_await (both origins: the leak over-taints)
#   R6b handler restore (OP_THROW)              if_cond_unwind (both origins)
#   R7  async args go untainted                 await_arg, await_ignores
#   R8  async return taint not read             await_arg, await_ignores
#   R9  generator args do not taint             yield_gen, yield_ignores
#   R10 yielded taint not recorded              yield_captured (both origins)
#   R11 call result ignores argument taint      call_userfn_ignores, call_lambda_ignores, await_ignores
#   R12 sanitizer return not cleared            the four sanitizer rows, both origins
#   R13 null result ignores argument taint      call_userfn_null
#   R14 tw yield taint not recorded             yield_captured (tw, both origins)
#   R15 if-expression emits no condition op     if_cond, if_cond_sink, if_nested, if_inner_cond
#   R16 callNaabFunction() condition restore    SURVIVES: every throw a NAAb catch receives also passes a
#                                               handler restore (R6/R6b); it alone matters for a native
#                                               caller that survives a callback's throw -- an agent tool
#                                               loop does (traced, not run: this table has no agent stub)
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

# Two origins for the tainted value t, and every row runs under both:
#   source    t comes straight from the source call, which the VM compiler's
#             walk sees, so the VM marks t at compile time AND at run time;
#   callback  t is a callback's result, which no static walk can see, so on the
#             VM only the runtime taint stack carries it. A kind the runtime
#             drops is laundered under this origin alone.
ORIGIN_EXPR_source='env.get("HOME")'
ORIGIN_EXPR_callback='array.map_fn([1], fn(v) { return env.get("HOME") })[0]'
ORIGIN=source

# run_case ENGINE PRELUDE SETUP EXPR CONTEXT -> VIOL (count) DONE (0/1) OUT
run_case() {
    local engine="$1" prelude="$2" setup="$3" expr="$4" context="$5" body dir origin_var origin_expr
    origin_var="ORIGIN_EXPR_$ORIGIN"; origin_expr="${!origin_var}"
    expr="${expr//@@/$'\n'}"
    if [ "$context" = "sink" ]; then
        body="print(${expr})"
    else
        body="let x = ${expr}
    print(x)"
    fi
    dir="$T/$engine"
    printf 'use env\nuse string\nuse array\n%s\nmain {\n    let t = %s\n    %s\n    %s\n    print("DONE")\n}\n' \
        "$prelude" "$origin_expr" "$setup" "$body" > "$dir/p.naab"
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
for ORIGIN in source callback; do
for engine in vm tw; do
    run_case "$engine" "" "" "t" assign
    if [ "$DONE" -eq 1 ] && [ "$(verdict)" = tainted ]; then ok "HC-1/$engine/$ORIGIN" "a tainted name is flagged at the sink"
    else bad "HC-1/$engine/$ORIGIN" "the detector must see a tainted name (done=$DONE)" "$OUT"; HC_OK=0; fi
    run_case "$engine" "" "" '"c"' assign
    if [ "$DONE" -eq 1 ] && [ "$(verdict)" = clean ]; then ok "HC-2/$engine/$ORIGIN" "a constant is not flagged"
    else bad "HC-2/$engine/$ORIGIN" "a constant must not be flagged (done=$DONE)" "$OUT"; HC_OK=0; fi
done
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
for ORIGIN in source callback; do
echo "--- t from: $ORIGIN"
while IFS='|' read -r id kind prelude setup expr context expect why; do
    [ -z "$id" ] && continue
    [ "$prelude" = "-" ] && prelude=""
    [ "$setup" = "-" ] && setup=""
    [ "$ORIGIN" = source ] && NROWS=$((NROWS+1))
    id="$id/$ORIGIN"
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
done
if [ "$NROWS" -lt 50 ]; then bad "TS-N" "only $NROWS rows read from the table"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$SKIP"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
