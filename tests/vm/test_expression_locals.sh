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

# run ENGINE PRELUDE STATEMENT -> OUT (the lines between R: and DONE), DONE
run() {
    local engine="$1" prelude="$2" stmt="$3" raw
    stmt="${stmt//@@/$'\n    '}"
    stmt="${stmt//@PIPE@/|>}"
    prelude="${prelude//@PIPE@/|>}"
    printf 'use string\nstruct P { a: string, b: string }\nfn f1(a) { return a }\nfn f2(a, b) { return a + b }\nfn call0(f) { return f() }\n%s\nmain {\n    let s = "x"\n    let n = 1\n    %s\n    print("R:")\n    print(r)\n    print("DONE")\n}\n' \
        "$prelude" "$stmt" > "$T/p.naab"
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

cross() {  # cross CONTEXT_TAG CONSTRUCT_TAG
CTX="$(grep "^$1|" "$TABLE" | tr -d '\r')"
INN="$(grep "^$2|" "$TABLE" | tr -d '\r')"
echo "=== $1 contexts x $2 constructs ==="
while IFS='|' read -r _ cid prelude stmt cwhy; do
    [ -z "$cid" ] && continue
    [ "$prelude" = "-" ] && prelude=""
    while IFS='|' read -r _ iid inner lit; do
        [ -z "$iid" ] && continue
        id="$cid/$iid"
        [ "$HC_OK" -ne 1 ] && { skip "$id" "harness controls failed"; continue; }
        # Each engine is judged by its own oracle: the engines print some
        # values differently (a struct), which is not what this measures.
        run vm "${prelude//X/$lit}" "${stmt//X/$lit}"; want_vm="$OUT"; wd_vm=$DONE
        run tw "${prelude//X/$lit}" "${stmt//X/$lit}"; want_tw="$OUT"; wd_tw=$DONE
        if [ "$wd_vm" -ne 1 ] || [ "$wd_tw" -ne 1 ]; then skip "$id" "the oracle (literal) program did not run: $(grep -m1 -i error <<<"$RAW")"; continue; fi
        run vm "${prelude//X/$inner}" "${stmt//X/$inner}"; vm_done=$DONE; vm_out="$OUT"; vm_raw="$RAW"
        run tw "${prelude//X/$inner}" "${stmt//X/$inner}"; tw_done=$DONE; tw_out="$OUT"; tw_raw="$RAW"
        if [ "$vm_done" -eq 1 ] && [ "$vm_out" = "$want_vm" ] && [ "$tw_done" -eq 1 ] && [ "$tw_out" = "$want_tw" ]; then
            ok "$id" "$cwhy"
        else
            bad "$id" "vm want [$want_vm] got(done=$vm_done) [$vm_out]; tw want [$want_tw] got(done=$tw_done) [$tw_out] -- $cwhy" \
                "--- vm
$(tail -3 <<<"$vm_raw")
--- tw
$(tail -3 <<<"$tw_raw")"
        fi
    done <<<"$INN"
done <<<"$CTX"
}
cross C I
cross Q J

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$SKIP"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
