#!/usr/bin/env bash
# ============================================================
# test_optional_chaining.sh — `a?.b` and `a?.m()` on a null `a` must give null
# on the VM, and must leave the stack as they found it.
#
# Compiler::visit(MemberExpr) and the optional branch of visit(CallExpr)
# compiled `a?.b` as DUP, JUMP_IF_NULL, ..., and on the null path a POP of
# "the null dup". But OP_JUMP_IF_NULL already consumes the value it tests
# when it jumps (it pushes it back only when it is NOT null), so that POP
# removed the ORIGINAL null as well, one value too many. The expression then
# left nothing on the stack, and whatever sat below it was taken as its value.
# Observed before the fix (README review, 2026-10-24): `print(u?.name)` with
# u = null failed with "Value is not callable" (the print builtin was popped
# as the argument), and `let n = u?.name` made n the print builtin itself.
# The tree-walker gave null both times. The opcode has had that meaning since
# the VM's first commit (1dd08e83); optional chaining arrived later
# (c2d9bb66) with the extra POP, and null coalescing (`??`), written against
# the same opcode, has always been correct.
#
# Why the existing test missed it: tests/robustness/test_structs_enums.naab
# T18.6 does `let r = n?.name; if r == null`. Inside a function the value
# below the expression is the local `n`, which is null, so r read null by
# coincidence.
#
# Every scenario asserts a hand-written expectation on BOTH engines (so a
# change that broke both the same way still fails) and that the two agree.
# The L arms are the stack check: a local declared AFTER the expression must
# keep its own value. The C arms are the non-null controls: the fix must not
# touch the path where the object exists.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

# A missing or stand-in binary would make both engines print nothing; ask the
# binary to identify itself first.
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/optchain-XXXXXX")"
trap '[ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"' EXIT

PASS_COUNT=0; FAIL_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

# $1=id $2=description $3=expected output (one KEY=value line) $4=source
check() {
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    printf '%s\n' "$4" > "$d/t.naab"
    local vm tw
    vm=$(cd "$d" && "$NAAB" --no-governance t.naab 2>&1 | grep -E '^[A-Z0-9_]+=' )
    tw=$(cd "$d" && "$NAAB" --no-governance --tree-walk t.naab 2>&1 | grep -E '^[A-Z0-9_]+=' )
    if [ "$vm" = "$3" ] && [ "$tw" = "$3" ]; then
        pass "$1" "$2"
    else
        fail "$1" "$2" "expected '$3' | VM: '${vm:-<nothing>}' | tree-walk: '${tw:-<nothing>}'"
    fi
}

echo ""
echo "=== optional chaining on null (VM vs tree-walker) ==="

check OC-01 "u?.name on null, passed straight to a call" "A=null" '
main {
    let u = null
    print("A=" + string(u?.name))
}'

check OC-02 "u?.name on null, stored in a local" "N=null" '
main {
    let u = null
    let n = u?.name
    print("N=" + string(n))
}'

check OC-03 "d?.get() on null" "M=null" '
main {
    let d = null
    print("M=" + string(d?.get("k")))
}'

check OC-04 "u?.a?.b chain on null" "C=null" '
main {
    let u = null
    print("C=" + string(u?.a?.b))
}'

check OC-05 "u?.name ?? fallback on null" "D=none" '
main {
    let u = null
    print("D=" + (u?.name ?? "none"))
}'

check OC-L1 "locals around u?.x keep their values (member)" "L=1,null,2" '
fn f(u) {
    let a = 1
    let r = u?.x
    let b = 2
    return string(a) + "," + string(r) + "," + string(b)
}
main {
    print("L=" + f(null))
}'

check OC-L2 "locals around u?.get() keep their values (method)" "L=1,null,2" '
fn f(u) {
    let a = 1
    let r = u?.get("x")
    let b = 2
    return string(a) + "," + string(r) + "," + string(b)
}
main {
    print("L=" + f(null))
}'

# ---- controls: the non-null path is unchanged ----
check OC-C1 "u?.name on a dict returns the field" "P=Ann" '
main {
    let u = {"name": "Ann"}
    print("P=" + u?.name)
}'

check OC-C2 "d?.get() on a dict calls the method" "Q=v" '
main {
    let d = {"k": "v"}
    print("Q=" + d?.get("k"))
}'

check OC-C3 "locals around u?.x keep their values when u exists" "L=1,5,2" '
fn f(u) {
    let a = 1
    let r = u?.x
    let b = 2
    return string(a) + "," + string(r) + "," + string(b)
}
main {
    print("L=" + f({"x": 5}))
}'

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
[ "$FAIL_COUNT" -eq 0 ]
