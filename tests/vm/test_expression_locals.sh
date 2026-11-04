#!/usr/bin/env bash
# A118 (Bundle 12): a value an EXPRESSION declares as a local -- a match
# subject, a match binding, a catch binding -- reads and writes its own slot,
# whatever the enclosing expression already holds on the stack.
#
# The VM compiler gave these locals the slot locals.size(), counted from the
# declared locals only, while the stack also held the enclosing expression's
# temporaries (a callee, a left operand, earlier arguments). So the local was
# the temporary: print(match ...) wrote the result over print and failed "Value
# is not callable", "p" + match ... read "p" as the subject, and a catch binding
# under print read print itself.
#
# Generated from tests/vm/expression_locals.psv: every CONTEXT (an operand
# position) crossed with every INNER construct. A case passes when
#   VM(construct) == VM(literal)  -- the oracle: the same program with the
#                                    construct replaced by its value, which
#                                    declares no expression local
#   TW(construct) == TW(literal)  -- and the same on the tree-walker (each
#                                    engine against its own oracle: they print
#                                    a struct differently)
# and every program finished (a DONE marker). A program that does not finish
# is a failure for the construct run, UNMEASURABLE for the oracle run.
#
# Measured against master 528fbb59 (the binary built before the fix): 106 of
# the 180 cases fail, every one on the VM (the tree-walker is right in all), and
# every control context (alone, a left operand, an assigned value) passes.
#
# Kill table (each mutant applied alone, built and run; the rows it turned red):
#   A1  interpolation's held text and string()   q_interp
#   A2  member assignment's held value           q_member_target
#   A3  index assignment's held value            q_index_target
#   A4  index assignment's held object           q_index_target
#   A5  pipeline into a call: callee             ctx_pipe_call
#   A6  pipeline into a name: callee             ctx_pipe
#   A7  subscript's held object                  q_subscript, q_member_target
#   A8  binary operator's left operand           ctx_bin_right, ctx_match_arm
#   A9  optional-chaining method's receiver      ctx_optional
#   A10 method call's receiver                   ctx_method, ctx_stdlib2
#   A11 a call's callee                          ctx_call1, ctx_call2, ctx_print, ctx_nested, ...
#   A12 a call's earlier arguments               ctx_call2, ctx_nested, ...
#   A13 a dict's earlier keys and values         ctx_dict2, ctx_dict_key
#   A14 a list's earlier elements                ctx_list2
#   A15 a range's start                          q_range
#   A16 a struct literal's instance              ctx_struct2
#   A17 catch binding reserves no slots          every in_catch / in_catch_use case under a temporary
#   A18 match subject reserves no slots          every match construct under a temporary
#   A19 a pattern's held subject copy            ctx_pattern (a call in a pattern)
# A twentieth count, a list pattern's literal element, was removed as dead: the
# compiler compares that element only when it is a literal, which declares no
# local.
#
# Batch screen (one process per engine and side, not four per case): the
# ~730 separate runs cost 29 s here and minutes on build-windows, where a
# process start goes through MSYS. Every case without a prelude first runs
# once in a BATCH -- one program per engine for the constructs and one for the
# literals, each case in its own function -- and a case the batch PASSES is
# decided there. Every other case (a batch mismatch, a batch that stopped
# before reaching it, and each case with a prelude) is run on its own, exactly
# as before, and that run decides. So a batch can only ever pass a case early;
# it cannot fail one. That is sound only if the batch never passes a case the
# single run fails, which was measured: EL_MODE=batch-only against master
# 528fbb59 passes none of the 106 cases the single runs fail (most of its
# batches stop at their first crash, so the mutants, which mostly give wrong
# values without one, are the stronger check), and each of the 19 mutants
# turns red every row it turned red with single runs only (A11, A17 and A18
# also ctx_pattern rows: that context was added after the first mutant run;
# observed 2026-11-04, one run each). HC-3 and HC-4 are
# the batch path's own controls (its comparison can fail; a case that stops
# the program passes nothing after it).
#   EL_MODE=batch-only  report the batch verdicts, run nothing singly (measurement)
#   EL_MODE=single      run every case singly (the harness before the batch)
#
# Usage: bash tests/vm/test_expression_locals.sh [path/to/naab-lang]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-${NAAB:-$REPO/build/naab-lang}}"
TABLE="$SCRIPT_DIR/expression_locals.psv"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       /'; }
skip() { SKIP=$((SKIP+1)); echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    echo "UNMEASURABLE: naab-lang not built"; exit 1
fi
[ -f "$TABLE" ] || { echo "FAIL: $TABLE missing"; exit 1; }

# No govern.json anywhere above this directory: --no-governance waives the
# requirement, and governance plays no part in what is measured.
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
EL_MODE="${EL_MODE:-}"
case "$EL_MODE" in ""|batch-only|single) ;; *) echo "FAIL: EL_MODE must be batch-only or single"; exit 1 ;; esac

PROGRAM_HEAD='use string
struct P { a: string, b: string }
fn f1(a) { return a }
fn f2(a, b) { return a + b }
fn call0(f) { return f() }
'

# naab ENGINE FILE -> the program's output (stdout and stderr)
naab() {
    if [ "$1" = tw ]; then (cd "$T" && timeout 60 "$NAAB" --no-governance --tree-walk "$2" 2>&1)
    else (cd "$T" && timeout 60 "$NAAB" --no-governance "$2" 2>&1); fi
}

# run ENGINE PRELUDE STATEMENT -> OUT (the lines between R: and DONE), DONE
run() {
    local engine="$1" prelude="$2" stmt="$3" raw
    stmt="${stmt//@@/$'\n    '}"
    stmt="${stmt//@PIPE@/|>}"
    prelude="${prelude//@PIPE@/|>}"
    printf '%s%s\nmain {\n    let s = "x"\n    let n = 1\n    %s\n    print("R:")\n    print(r)\n    print("DONE")\n}\n' \
        "$PROGRAM_HEAD" "$prelude" "$stmt" > "$T/p.naab"
    if [ "$engine" = tw ]; then
        raw="$(cd "$T" && timeout 30 "$NAAB" --no-governance --tree-walk p.naab 2>&1)"
    else
        raw="$(cd "$T" && timeout 30 "$NAAB" --no-governance p.naab 2>&1)"
    fi
    DONE=$(grep -cx "DONE" <<<"$raw" | tr -d ' ')
    [ "$DONE" -gt 0 ] && DONE=1
    OUT="$(sed -n '/^R:$/,/^DONE$/p' <<<"$raw" | grep -vx 'R:\|DONE')"
    RAW="$raw"
}

echo "=== harness controls ==="
HC_OK=1
for engine in vm tw; do
    run "$engine" "" 'let r = "lit"'
    if [ "$DONE" -eq 1 ] && [ "$OUT" = "lit" ]; then ok "HC-1/$engine" "a program runs and its value is read back"
    else bad "HC-1/$engine" "the harness must read a value back (done=$DONE out=$OUT)" "$RAW"; HC_OK=0; fi
done
run vm "" 'let r = "a"'; a_out="$OUT"
run vm "" 'let r = "b"'
if [ "$a_out" != "$OUT" ]; then ok "HC-2" "two different values read back differently (the comparison can fail)"
else bad "HC-2" "the harness reads every value the same"; HC_OK=0; fi

NC=$(grep -c '^C|' "$TABLE"); NI=$(grep -c '^I|' "$TABLE")
NQ=$(grep -c '^Q|' "$TABLE"); NJ=$(grep -c '^J|' "$TABLE")
if [ "$NC" -lt 20 ] || [ "$NI" -lt 8 ] || [ "$NQ" -lt 5 ] || [ "$NJ" -lt 4 ]; then
    bad "TS-N" "only $NC/$NQ contexts and $NI/$NJ constructs read from the table"
fi

# collect CONTEXT_TAG CONSTRUCT_TAG: every context crossed with every construct
CID=(); CPRE_I=(); CSTMT_I=(); CPRE_L=(); CSTMT_L=(); CWHY=()
collect() {
    local ctx inn cid prelude stmt cwhy iid inner lit
    ctx="$(grep "^$1|" "$TABLE" | tr -d '\r')"
    inn="$(grep "^$2|" "$TABLE" | tr -d '\r')"
    while IFS='|' read -r _ cid prelude stmt cwhy; do
        [ -z "$cid" ] && continue
        [ "$prelude" = "-" ] && prelude=""
        while IFS='|' read -r _ iid inner lit; do
            [ -z "$iid" ] && continue
            CID+=("$cid/$iid"); CWHY+=("$cwhy")
            CPRE_I+=("${prelude//X/$inner}"); CSTMT_I+=("${stmt//X/$inner}")
            CPRE_L+=("${prelude//X/$lit}");   CSTMT_L+=("${stmt//X/$lit}")
        done <<<"$inn"
    done <<<"$ctx"
}
collect C I
collect Q J

# batch_program SIDE INDEX... -> one program: each case in its own function,
# its value printed between R:k and E:k (SIDE is I for the constructs, L for
# the literals)
batch_program() {
    local side="$1" k stmt; shift
    printf '%s' "$PROGRAM_HEAD"
    for k in "$@"; do
        if [ "$side" = I ]; then stmt="${CSTMT_I[$k]}"; else stmt="${CSTMT_L[$k]}"; fi
        stmt="${stmt//@@/$'\n    '}"; stmt="${stmt//@PIPE@/|>}"
        printf 'fn el_case_%s() {\n    let s = "x"\n    let n = 1\n    %s\n    print("R:%s")\n    print(r)\n    print("E:%s")\n}\n' "$k" "$stmt" "$k" "$k"
    done
    printf 'main {\n'
    for k in "$@"; do printf '    el_case_%s()\n' "$k"; done
    printf '}\n'
}

# parse_batch OUTPUT -> "k<TAB>value" for each case that reached its E:k
parse_batch() {
    awk '/^R:[0-9]+$/ { k = substr($0, 3); buf = ""; inb = 1; n = 0; next }
         inb && $0 == ("E:" k) { print k "\t" buf; inb = 0; next }
         inb { buf = (n++ ? buf "\037" $0 : $0) }' <<<"$1"
}

# screen NAME INDEX... -> sets SCREENED[k]=1 for each case the batch passes:
# both engines reached it on both sides, and each engine's construct value
# equals its literal value
declare -A SCREENED=()
screen() {
    local name="$1" k side engine; shift
    declare -A got=()
    for side in I L; do
        batch_program "$side" "$@" > "$T/b_${name}_$side.naab"
        for engine in vm tw; do
            while IFS=$'\t' read -r k v; do got["$engine$side$k"]="$v"; done \
                < <(parse_batch "$(naab "$engine" "b_${name}_$side.naab")")
        done
    done
    for k in "$@"; do
        [ -n "${got[vmI$k]+x}" ] && [ -n "${got[vmL$k]+x}" ] && [ -n "${got[twI$k]+x}" ] && [ -n "${got[twL$k]+x}" ] || continue
        [ "${got[vmI$k]}" = "${got[vmL$k]}" ] && [ "${got[twI$k]}" = "${got[twL$k]}" ] && SCREENED[$k]=1
    done
}

# Harness controls of the batch path, through the same screen(): a construct
# whose value differs from its literal must not pass (HC-3), and a case that
# stops its program must not pass, nor any case after it, while a case before
# it still does (HC-4).
if [ "$HC_OK" -eq 1 ] && [ "$EL_MODE" != single ]; then
    base=${#CID[@]}
    CSTMT_I+=('let r = "a"' 'let r = "a"' 'let r = call0(0)'       'let r = "a"')
    CSTMT_L+=('let r = "b"' 'let r = "a"' 'let r = "a"'         'let r = "a"')
    screen hc "$base" $((base+1)) $((base+2)) $((base+3))
    if [ -z "${SCREENED[$base]+x}" ]; then ok "HC-3" "the batch does not pass a construct whose value differs from its literal"
    else bad "HC-3" "the batch passed a case whose value differs"; HC_OK=0; fi
    if [ -n "${SCREENED[$((base+1))]+x}" ] && [ -z "${SCREENED[$((base+2))]+x}" ] && [ -z "${SCREENED[$((base+3))]+x}" ]; then
        ok "HC-4" "a case that stops the batch passes nothing from there on; the case before it passes"
    else bad "HC-4" "batch markers: before=${SCREENED[$((base+1))]:-no} stopping=${SCREENED[$((base+2))]:-no} after=${SCREENED[$((base+3))]:-no}"; HC_OK=0; fi
    for k in "$base" $((base+1)) $((base+2)) $((base+3)); do unset "SCREENED[$k]"; done
    CSTMT_I=("${CSTMT_I[@]:0:$base}"); CSTMT_L=("${CSTMT_L[@]:0:$base}")
fi

N=${#CID[@]}
if [ "$HC_OK" -eq 1 ] && [ "$EL_MODE" != single ]; then
    idx=()
    for ((k = 0; k < N; k++)); do [ -z "${CPRE_I[$k]}" ] && [ -z "${CPRE_L[$k]}" ] && idx+=("$k"); done
    screen cases "${idx[@]}"
fi

echo "=== contexts x constructs: $N cases ==="
BATCHED=0; SINGLE=0
for ((k = 0; k < N; k++)); do
    id="${CID[$k]}"; cwhy="${CWHY[$k]}"
    [ "$HC_OK" -ne 1 ] && { skip "$id" "harness controls failed"; continue; }
    if [ -n "${SCREENED[$k]+x}" ]; then
        BATCHED=$((BATCHED+1)); ok "$id" "$cwhy (batch)"; continue
    fi
    if [ "$EL_MODE" = batch-only ]; then
        bad "$id" "not passed by the batch -- $cwhy"; continue
    fi
    SINGLE=$((SINGLE+1))
    # Each engine is judged by its own oracle: the engines print some values
    # differently (a struct), which is not what this measures.
    run vm "${CPRE_L[$k]}" "${CSTMT_L[$k]}"; want_vm="$OUT"; wd_vm=$DONE
    run tw "${CPRE_L[$k]}" "${CSTMT_L[$k]}"; want_tw="$OUT"; wd_tw=$DONE
    if [ "$wd_vm" -ne 1 ] || [ "$wd_tw" -ne 1 ]; then skip "$id" "the oracle (literal) program did not run: $(grep -m1 -i error <<<"$RAW")"; continue; fi
    run vm "${CPRE_I[$k]}" "${CSTMT_I[$k]}"; vm_done=$DONE; vm_out="$OUT"; vm_raw="$RAW"
    run tw "${CPRE_I[$k]}" "${CSTMT_I[$k]}"; tw_done=$DONE; tw_out="$OUT"; tw_raw="$RAW"
    if [ "$vm_done" -eq 1 ] && [ "$vm_out" = "$want_vm" ] && [ "$tw_done" -eq 1 ] && [ "$tw_out" = "$want_tw" ]; then
        ok "$id" "$cwhy"
    else
        bad "$id" "vm want [$want_vm] got(done=$vm_done) [$vm_out]; tw want [$want_tw] got(done=$tw_done) [$tw_out] -- $cwhy" \
            "--- vm
$(tail -3 <<<"$vm_raw")
--- tw
$(tail -3 <<<"$tw_raw")"
    fi
done
echo "  decided by the batch: $BATCHED, run singly: $SINGLE"

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$SKIP"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
