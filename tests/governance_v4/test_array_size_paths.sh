#!/usr/bin/env bash
# ============================================================
# test_array_size_paths.sh — limits.array_size was wired into some paths only
#
# THE GAP. `limits.array_size` is a HARD gate. It was called on list literals,
# ranges and spreads (vm.cpp OP_BUILD_LIST / range / spread, expressions.cpp
# ListExpr) but NOT on list CONCATENATION, so `a = a + a` in a loop grew with no
# governance check and no internal cap until the allocator gave up. Measured
# before the fix, with limits.array_size = 1000:
#
#   list literal of 1500      exit 3, blocked          (gate live)
#   a = a + a doubling        Error: bad_alloc, exit 1  (no gate, NO governance
#                                                        verdict line at all)
#
# An operator who set that limit believed array growth was capped and had capped
# three of five paths. BOTH engines had the same gap, so tests/differential/
# could not see it — parity holds when both are wrong.
#
# NOT THE ReDoS CLASS, and the distinction matters for how this is triaged. The
# SECRET_PATTERNS crash was dangerous because it happened INSIDE checkSecrets()
# while it evaluated attacker content — the check died mid-decision and the
# input was neither blocked nor cleared. Here the allocation died in ordinary
# execution with no gate on the path, so nothing was mid-verdict. The missing
# end-of-run verdict is an audit gap, not a bypassed check.
#
# THE REST OF THE GROWTH PATHS (D10). That fix left array.push for later: it
# called only naab::limits::checkArraySize (a fixed 10,000,000 cap), stdlib had
# no accessor to the engine, and gating it changes behaviour for configs that
# set the key. AS-05 pinned the gap as a measurement. Tracing the rest found the
# same gap at every way an array grows one element at a time, on both engines:
# .push/.append/.add, .unshift, .insert, array.push, array.unshift -- and
# range(), which also built its whole list before ANY check, so not even the
# fixed cap applied to it. Measured with limits.array_size = 1000: each grew to
# 1500 and the run exited 0 under "Governance: PASS". They now answer to the
# same check as a literal, through one helper (include/naab/array_growth.h).
# The tree-walker's `0..N` is a lazy range object, not an array, so it has
# nothing to gate; the VM materialises it and was already checked -- so a
# `for i in 0..N` past the limit blocks on the VM only (A66, open).
#
#   AS-01  POSITIVE CONTROL. A list LITERAL over the limit must block (exit 3).
#          Without it, AS-02's block is equally satisfied by a config that
#          blocks everything, or by the binary refusing to run at all.
#   AS-02  THE FIX. Concatenation over the limit must block, in BOTH engines.
#   AS-03  CONTROL. Concatenation UNDER the limit must still work — a fix that
#          blocks all concatenation would pass AS-02 and be useless.
#   AS-04  CONTROL. With NO limit configured (default 0 = disabled), a large
#          concatenation must be unaffected. This is the backward-compatibility
#          assertion: the change must not start blocking runs that pass today.
#   AS-05  every one-element growth path and range() over the limit blocks
#          (exit 3, nothing after it runs), both engines
#   AS-06  CONTROL, per path: growing to EXACTLY the limit still works -- a gate
#          that blocks every push would pass AS-05
#   AS-07  CONTROL, per path: with no limit configured, growth to 3000 is
#          unaffected (backward compatibility)
#   AS-08  the block is HARD: a try/catch around the growth does not catch it
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }

[ -x "$NAAB" ] || { echo "  naab-lang not built, skipping"; exit 0; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/arraysize-XXXXXX")"
# A failed mktemp leaves $W EMPTY, and every "$W/x" write below then
# rebases onto the filesystem ROOT. That is how a stray govern.json reached /
# and silently governed every later run on the machine. Fail loudly instead.
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
cleanup() { teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W/limited" "$W/unlimited"

cat > "$W/limited/govern.json" <<'JSON'
{ "version":"4.0", "mode":"enforce",
  "security": { "sandbox_level": "elevated" },
  "limits": { "array_size": 1000 } }
JSON
cat > "$W/unlimited/govern.json" <<'JSON'
{ "version":"4.0", "mode":"enforce", "security": { "sandbox_level": "elevated" } }
JSON

python3 -c "print('main { let a = [' + ','.join(['1']*1500) + '] print(\"LIT\") }')" > "$W/limited/lit.naab"
cat > "$W/limited/concat.naab" <<'X'
main { let a = [1] let i = 0 while i < 20 { a = a + a i = i + 1 } print("CONCAT_DONE") }
X
cat > "$W/limited/small.naab" <<'X'
main { let a = [1,2,3] let b = [4,5] let c = a + b print("SMALL_" + string(c.length())) }
X
cat > "$W/unlimited/big.naab" <<'X'
main { let a = [1] let i = 0 while i < 12 { a = a + a i = i + 1 } print("BIG_" + string(a.length())) }
X

# $1=dir $2=file $3=extra-flags -> exit code, memory-bounded so an unfixed
# build fails on bad_alloc instead of taking the machine down.
run_ec() { ( cd "$1" && ulimit -v 2000000 2>/dev/null; timeout 90 "$NAAB" ${3:-} "$2" >/dev/null 2>&1 ); echo $?; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  limits.array_size: which paths does the gate actually reach? |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

LIT_VM=$(run_ec "$W/limited" lit.naab); LIT_TW=$(run_ec "$W/limited" lit.naab --tree-walk)
if [ "$LIT_VM" = "3" ] && [ "$LIT_TW" = "3" ]; then
    ok "AS-01" "POSITIVE CONTROL: a list literal over the limit blocks (exit 3, both engines)"
else
    bad "AS-01" "the configured limit does not block a list literal" \
        "VM exit $LIT_VM, tree-walk exit $LIT_TW, expected 3 each. The gate is not live, so AS-02 proves nothing — fix this before reading any other result here."
fi

CAT_VM=$(run_ec "$W/limited" concat.naab); CAT_TW=$(run_ec "$W/limited" concat.naab --tree-walk)
if [ "$CAT_VM" = "3" ] && [ "$CAT_TW" = "3" ]; then
    ok "AS-02" "concatenation over the limit blocks (exit 3, both engines)"
else
    bad "AS-02" "concatenation escapes limits.array_size" \
        "VM exit $CAT_VM, tree-walk exit $CAT_TW, expected 3 each. exit 1 means the run died on bad_alloc with no governance verdict — the pre-fix behaviour. A difference BETWEEN the two engines means the gate was added to one path only."
fi

SM=$(run_ec "$W/limited" small.naab)
if [ "$SM" = "0" ]; then
    ok "AS-03" "CONTROL: concatenation under the limit still works"
else
    bad "AS-03" "the fix blocks legitimate concatenation" \
        "exit $SM on a 5-element concat against a limit of 1000. AS-02 passing while this fails means the gate blocks everything, which is not a fix."
fi

BIG=$(run_ec "$W/unlimited" big.naab)
if [ "$BIG" = "0" ]; then
    ok "AS-04" "CONTROL: with no limit configured, a large concatenation is unaffected"
else
    bad "AS-04" "adding the gate changed behaviour for configs that set no limit" \
        "exit $BIG with limits.array_size absent (default 0 = disabled). This must stay backward compatible — it is the difference between fixing a gate and starting to block runs that pass today."
fi

# ---- AS-05..AS-08: every way an array grows, per path and per engine (D10) ----
# grow_stmt PATH -> one NAAb statement that grows `a` by one element (value i)
grow_stmt() {
    case "$1" in
        push)        echo 'a.push(i)' ;;
        append)      echo 'a.append(i)' ;;
        add)         echo 'a.add(i)' ;;
        unshift)     echo 'a.unshift(i)' ;;
        insert)      echo 'a.insert(0, i)' ;;
        mod_push)    echo 'a = array.push(a, i)' ;;
        mod_unshift) echo 'a = array.unshift(a, i)' ;;
    esac
}
# mk PATH N DIR -> writes DIR/PATH_N.naab, which grows an array to N elements
# along PATH and then prints LEN_<length>. Nothing after the growth runs if the
# growth is blocked, so a missing LEN_ line is the block's observable effect.
mk() {
    local f="$3/$1_$2.naab"
    if [ "$1" = "range" ]; then
        printf 'use array\nmain { let a = range(%s) print("LEN_" + string(array.length(a))) }\n' "$2" > "$f"
    else
        printf 'use array\nmain { let a = [] let i = 0 while i < %s { %s i = i + 1 } print("LEN_" + string(array.length(a))) }\n' \
            "$2" "$(grow_stmt "$1")" > "$f"
    fi
    echo "$f"
}
# out DIR FILE FLAGS -> stdout+stderr, then "RC=<exit>" on the last line
out() { ( cd "$1" && ulimit -v 2000000 2>/dev/null; timeout 90 "$NAAB" ${3:-} "$2" 2>&1; echo "RC=$?" ); }

for path in push append add unshift insert mod_push mod_unshift range; do
    for eng in vm tw; do
        flags=""; [ "$eng" = "tw" ] && flags="--tree-walk"
        f=$(mk "$path" 1500 "$W/limited")
        O=$(out "$W/limited" "$(basename "$f")" "$flags"); O="${O//$'\r'/}"
        rc="${O##*RC=}"
        if [ "$rc" = "3" ] && grep -q 'exceeds limit of 1000' <<<"$O" && ! grep -q 'LEN_' <<<"$O"; then
            ok "AS-05/$path/$eng" "$path past the limit blocks (exit 3)"
        else
            bad "AS-05/$path/$eng" "$path grows past limits.array_size" \
                "exit $rc, expected 3 with 'exceeds limit of 1000' and no LEN_ line. Got: $(grep -m1 -E 'LEN_|exceeds|Error' <<<"$O")"
        fi
        f=$(mk "$path" 1000 "$W/limited")
        O=$(out "$W/limited" "$(basename "$f")" "$flags"); O="${O//$'\r'/}"
        rc="${O##*RC=}"
        if [ "$rc" = "0" ] && grep -q 'LEN_1000' <<<"$O"; then
            ok "AS-06/$path/$eng" "CONTROL: $path to exactly the limit (1000) still works"
        else
            bad "AS-06/$path/$eng" "$path is refused AT the limit" \
                "exit $rc, expected 0 and LEN_1000. A gate that blocks every growth passes AS-05; this arm is what tells it apart."
        fi
        f=$(mk "$path" 3000 "$W/unlimited")
        O=$(out "$W/unlimited" "$(basename "$f")" "$flags"); O="${O//$'\r'/}"
        rc="${O##*RC=}"
        if [ "$rc" = "0" ] && grep -q 'LEN_3000' <<<"$O"; then
            ok "AS-07/$path/$eng" "CONTROL: $path with no limit configured reaches 3000"
        else
            bad "AS-07/$path/$eng" "$path changed behaviour for a config that sets no limit" \
                "exit $rc, expected 0 and LEN_3000 with limits.array_size absent (default 0 = off)."
        fi
    done
done

cat > "$W/limited/catch.naab" <<'X'
main {
    let a = []
    try {
        let i = 0
        while i < 1500 { a.push(i) i = i + 1 }
    } catch (e) {
        print("CAUGHT")
    }
    print("AFTER")
}
X
for eng in vm tw; do
    flags=""; [ "$eng" = "tw" ] && flags="--tree-walk"
    O=$(out "$W/limited" catch.naab "$flags"); O="${O//$'\r'/}"
    rc="${O##*RC=}"
    if [ "$rc" = "3" ] && ! grep -q 'CAUGHT\|AFTER' <<<"$O"; then
        ok "AS-08/$eng" "the block is HARD: a try/catch around the push does not catch it"
    else
        bad "AS-08/$eng" "the limit's block is catchable or absent" \
            "exit $rc, expected 3 with neither CAUGHT nor AFTER printed."
    fi
done

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}array_size paths: $PASS passed, 0 failed${NC}"
else
    echo -e "${RED}array_size paths: $FAIL failed${NC}, $PASS passed"; exit 1
fi
