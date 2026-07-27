#!/usr/bin/env bash
# ============================================================
# test_try_handler_leak.sh — break/continue out of a try must close its
# exception handler, and must not close one it does not own.
#
# Compiler::visit(BreakStmt) and visit(ContinueStmt) emitted OP_POPN for locals
# but no OP_TRY_END, so jumping out of a try body leaked its handler onto the
# VM's handler stack permanently. A leaked handler is not inert: a later throw
# with no enclosing try resolves against it, re-enters the dead catch block and
# falls THROUGH it, re-executing statements that already ran. Observed before
# the fix, on scenario L2:
#
#     BEFORE_UNCAUGHT
#     STALE_CATCH_REACHED e=should-be-uncaught     <- dead handler swallowed it
#     BEFORE_UNCAUGHT                              <- statement re-executed
#     Error: Uncaught exception: should-be-uncaught
#
# Two things make that severe rather than cosmetic: an exception that should
# terminate the run is absorbed, and any side effect between the stale catch_ip
# and the leak point fires twice — file writes, agent sends, telemetry.
#
# The tree-walker is correct throughout, so it is the oracle here rather than a
# hand-written expectation. Every scenario asserts VM output == tree-walk
# output, which is a stronger check and cannot drift with either engine.
#
# The over-pop cases (E1-E3) matter more than the leak cases. Emitting an
# OP_TRY_END for a try that ENCLOSES the loop would destroy a handler that is
# still live, turning a leak into a wrongly-uncaught exception. They exist so
# the fix cannot be "close everything on the way out".
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${TRYLEAK_TMP:-${_SYSTMP}/tryleak-$$}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

cleanup() { [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

# Both engines run the same source with governance off. Error TEXT differs
# between engines by design (ERR-TEXT-001 in docs/engine-divergences.md), so
# the comparison is over the program's own prints, not the runtime's wording.
run_both() {  # $1=id  $2=source
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/t.naab"
    ( cd "$d" && "$NAAB" --no-governance t.naab 2>/dev/null | grep -E '^[A-Z0-9_]+=' ) > "$d/vm.txt" || true
    ( cd "$d" && "$NAAB" --no-governance --tree-walk t.naab 2>/dev/null | grep -E '^[A-Z0-9_]+=' ) > "$d/tw.txt" || true
    if diff -q "$d/vm.txt" "$d/tw.txt" >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

report() {  # $1=id $2=description
    local d="$TEST_TMP/$1"
    if run_both "$1" "$3"; then
        pass "$1" "$2"
    else
        fail "$1" "$2" "VM: $(tr '\n' ' ' < "$d/vm.txt") | tree-walk: $(tr '\n' ' ' < "$d/tw.txt")"
    fi
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  try handler lifetime across break / continue                 |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ---- L1: break out of try, then an unrelated try/catch still works ----
report L1 "break out of try leaves later try/catch intact" '
fn helper() { return 42 }
main {
    let i = 0
    while i < 3 { i = i + 1
        try { if i == 1 { break } } catch (e) { print("BAD1=" + string(e)) }
    }
    try { throw "boom" } catch (e) { print("K=" + string(helper())) print("E=" + string(e)) }
    print("END=1")
}'

# ---- L2: the original corruption — throw with NO enclosing try ----
report L2 "throw outside any try is not absorbed by a leaked handler" '
main {
    let i = 0
    while i < 3 { i = i + 1
        try { if i == 1 { break } } catch (e) { print("STALE=" + string(e)) }
    }
    print("BEFORE=1")
    throw "should-be-uncaught"
    print("AFTER=1")
}'

# ---- L3: continue out of try, repeatedly ----
report L3 "continue out of try does not accumulate handlers" '
main {
    let i = 0
    let n = 0
    while i < 5 { i = i + 1
        try { if i < 4 { continue } n = n + 1 } catch (e) { print("STALE=" + string(e)) }
    }
    print("N=" + string(n))
    print("BEFORE=1")
    throw "uncaught-after-continues"
    print("AFTER=1")
}'

# ---- L4: nested trys inside the loop, break from the inner one ----
report L4 "break from a nested try closes both handlers" '
main {
    let i = 0
    while i < 3 { i = i + 1
        try {
            try { if i == 1 { break } } catch (e) { print("INNER=" + string(e)) }
        } catch (e) { print("OUTER=" + string(e)) }
    }
    print("BEFORE=1")
    throw "uncaught-after-nested"
    print("AFTER=1")
}'

# ---- E1: OVER-POP GUARD — try ENCLOSING the loop must survive a break ----
report E1 "a try enclosing the loop still catches after an inner break" '
main {
    try {
        let i = 0
        while i < 3 { i = i + 1
            if i == 1 { break }
        }
        throw "caught-by-enclosing"
    } catch (e) { print("CAUGHT=" + string(e)) }
    print("END=1")
}'

# ---- E2: OVER-POP GUARD — enclosing try, break out of an INNER try ----
report E2 "enclosing try survives a break out of an inner try" '
main {
    try {
        let i = 0
        while i < 3 { i = i + 1
            try { if i == 1 { break } } catch (e) { print("INNER=" + string(e)) }
        }
        throw "still-caught-by-enclosing"
    } catch (e) { print("CAUGHT=" + string(e)) }
    print("END=1")
}'

# ---- E3: OVER-POP GUARD — break inside a CATCH block ----
# The handler for that try is already gone by the time the catch runs, on both
# the fall-through path and the exception path. Emitting one here would pop an
# enclosing handler that is still live.
report E3 "break inside a catch block does not close an enclosing handler" '
main {
    try {
        let i = 0
        while i < 3 { i = i + 1
            try { throw "inner" } catch (e) { if i == 1 { break } }
        }
        throw "enclosing-must-still-catch"
    } catch (e) { print("CAUGHT=" + string(e)) }
    print("END=1")
}'

# ---- E4: return out of a try (runtime cleanup path, must be unaffected) ----
report E4 "return out of a try still cleans its handler" '
fn f(x) {
    let i = 0
    while i < 3 { i = i + 1
        try { if i == 1 { return "early" } } catch (e) { print("STALE=" + string(e)) }
    }
    return "late"
}
main {
    print("R=" + f(1))
    print("BEFORE=1")
    throw "uncaught-after-return"
    print("AFTER=1")
}'

echo ""
echo "────────────────────────────────────────"
echo -e "Passed:  ${GREEN}${PASS_COUNT}${NC}"
echo -e "Failed:  ${RED}${FAIL_COUNT}${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}${FAILURES}"
    exit 1
fi
exit 0
