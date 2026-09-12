#!/usr/bin/env bash
# ============================================================
# test_debug_is_tainted_parity.sh — debug.is_tainted answered "no" under the VM
#
# THE DEFECT
#
# DebugModule::checkTainted() reached the GovernanceEngine through
# g_debug_interpreter, a raw pointer set by DebugModule::setInterpreter() --
# called from src/interpreter/interpreter.cpp and from NOWHERE else. Under the
# VM, which is the DEFAULT engine, the pointer is null and the function returned
# false before consulting anything.
#
# The taint data was never missing. Both engines call markTainted() on the same
# GovernanceEngine::taint_set_ (vm.cpp, and call_dispatch.cpp/expressions.cpp on
# the tree-walker). Only the ROUTE from debug.is_tainted to that set was
# tree-walker-only.
#
# Measured on bf489f0, taint_tracking enabled with sources ["file.read"]:
#
#   let untrusted = file.read("input.txt")
#   debug.is_tainted("untrusted")   VM: false    --tree-walk: true
#
# The failure direction is the unsafe one and it is silent: a script that guards
# on `if debug.is_tainted(x) { refuse }` never refuses under the default engine.
# It is not a governance gate itself -- taint SINKS are enforced separately and
# were never affected -- but it is offered to scripts as one, and it answered
# wrongly rather than erroring.
#
# SCOPE, because the pointer has four other users. debug.env(), debug.stack(),
# debug.snapshot() and debug.trace()'s location need the tree-walker's SCOPE and
# CALL STACK, which the VM does not expose through this interface; measured on
# the same build, debug.env() returns [] and debug.stack() returns [] under the
# VM. Those are a separate and larger problem (a VM introspection API) and are
# NOT fixed here. is_tainted is the one that needs only the GovernanceEngine,
# which is engine-agnostic and already reachable via getCurrent(). DT-05 pins
# that split so this suite is not read as a claim about the other four.
#
# That deferred half is register row F46 in docs/open-investigations.md, with
# its measurements -- it is a real open finding, not a footnote to this one.
#
#   DT-01  POSITIVE CONTROL: the tree-walker reports tainted data as tainted.
#          If this fails the fixture is not tainting anything and every other
#          arm is meaningless
#   DT-02  NEGATIVE CONTROL: an untainted literal reports false, on BOTH
#          engines. Without this, "return true always" passes DT-01 and DT-03
#   DT-03  THE FIX: the VM agrees with the tree-walker on tainted data
#   DT-04  the two engines agree on both answers -- stated as parity rather
#          than as two separate absolutes, since parity is the actual claim
#   DT-05  SCOPE PIN: debug.env()/debug.stack() are still tree-walker-only.
#          Asserted so a future change that makes them work updates this
#          suite deliberately instead of silently widening what it covers
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

# Unsigned govern.json below: with any key in the ambient trust store that is an
# INTEGRITY BLOCK at exit 3 and every probe returns no marker at all.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab-debug-taint-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
cleanup() { rm -rf "$TEST_TMP"; teardown_isolated_trust; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
W="$TEST_TMP/w"; mkdir -p "$W"

printf 'SECRET-DATA\n' > "$W/input.txt"
cat > "$W/govern.json" <<'JSON_EOF'
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "taint_tracking": { "enabled": true, "level": "advisory",
    "sources": ["file.read"], "sinks": [], "sanitizers": ["sanitize_"] } }
JSON_EOF

cat > "$W/t.naab" <<'NAAB_EOF'
use file
use debug
main {
  let untrusted = file.read("input.txt")
  let clean = "literal constant"
  print("UNTRUSTED=" + debug.is_tainted("untrusted"))
  print("CLEAN=" + debug.is_tainted("clean"))
}
NAAB_EOF

# $1=engine args $2=marker -> the marker's value, or "UNMEASURED" if the program
# never got far enough to print it. A missing marker is NOT a false: it means
# the run died (config rejected, parse error, integrity block), and reporting
# that as "false" would let a broken fixture read as the defect being present.
probe() {
    local out
    out=$(cd "$W" && timeout 60s "$NAAB" ${1:-} t.naab 2>&1)
    local line
    line=$(printf '%s\n' "$out" | grep "^$2=" | head -1)
    if [ -z "$line" ]; then
        echo "UNMEASURED"
    else
        echo "${line#*=}"
    fi
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  debug.is_tainted was tree-walker-only                        |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

TW_UNTRUSTED=$(probe "--tree-walk" "UNTRUSTED")
TW_CLEAN=$(probe "--tree-walk" "CLEAN")
VM_UNTRUSTED=$(probe "" "UNTRUSTED")
VM_CLEAN=$(probe "" "CLEAN")

echo "  measured: tree-walk untrusted=$TW_UNTRUSTED clean=$TW_CLEAN"
echo "            vm        untrusted=$VM_UNTRUSTED clean=$VM_CLEAN"
echo ""

# DT-01 -- without this the fixture could be tainting nothing at all
if [ "$TW_UNTRUSTED" = "true" ]; then
    pass "DT-01" "POSITIVE CONTROL: tree-walker sees file.read output as tainted"
else
    fail "DT-01" "POSITIVE CONTROL: tree-walker sees file.read output as tainted" \
         "got '$TW_UNTRUSTED' -- the fixture taints nothing, every other arm below is void"
fi

# DT-02 -- without this, "always return true" passes DT-01 and DT-03
if [ "$TW_CLEAN" = "false" ] && [ "$VM_CLEAN" = "false" ]; then
    pass "DT-02" "NEGATIVE CONTROL: a literal is untainted on both engines"
else
    fail "DT-02" "NEGATIVE CONTROL: a literal is untainted on both engines" \
         "tree-walk='$TW_CLEAN' vm='$VM_CLEAN' -- a blanket true would pass the other arms"
fi

# DT-03 -- the fix
if [ "$VM_UNTRUSTED" = "true" ]; then
    pass "DT-03" "THE FIX: the VM sees file.read output as tainted"
elif [ "$VM_UNTRUSTED" = "UNMEASURED" ]; then
    fail "DT-03" "THE FIX: the VM sees file.read output as tainted" \
         "UNMEASURED -- the program never printed the marker; this is a broken probe, not a false"
else
    fail "DT-03" "THE FIX: the VM sees file.read output as tainted" \
         "got '$VM_UNTRUSTED' -- debug.is_tainted answers 'not tainted' for tainted data"
fi

# DT-04 -- parity is the actual claim
if [ "$VM_UNTRUSTED" = "$TW_UNTRUSTED" ] && [ "$VM_CLEAN" = "$TW_CLEAN" ]; then
    pass "DT-04" "the engines agree on both answers"
else
    fail "DT-04" "the engines agree on both answers" \
         "untrusted vm='$VM_UNTRUSTED' tw='$TW_UNTRUSTED'; clean vm='$VM_CLEAN' tw='$TW_CLEAN'"
fi

# DT-05 -- scope pin. These need interpreter scope/stack, are NOT fixed here,
# and this arm exists so that changing them is a deliberate edit to this file.
cat > "$W/s.naab" <<'NAAB_EOF'
use debug
main {
  let outer_var = "hello"
  let n = 42
  print("ENVKEYS=" + debug.inspect(debug.keys(debug.env())))
}
NAAB_EOF
vm_env=$(cd "$W" && timeout 60s "$NAAB" s.naab 2>&1 | grep "^ENVKEYS=" | head -1)
tw_env=$(cd "$W" && timeout 60s "$NAAB" --tree-walk s.naab 2>&1 | grep "^ENVKEYS=" | head -1)
if [ -z "$vm_env" ] || [ -z "$tw_env" ]; then
    fail "DT-05" "SCOPE PIN: debug.env() is still tree-walker-only" \
         "UNMEASURED -- one of the engines printed no marker (vm='$vm_env' tw='$tw_env')"
elif [ "$vm_env" = "ENVKEYS=[]" ] && [ "$tw_env" != "ENVKEYS=[]" ]; then
    pass "DT-05" "SCOPE PIN: debug.env() is still tree-walker-only (not fixed here, by design)"
else
    fail "DT-05" "SCOPE PIN: debug.env() is still tree-walker-only" \
         "vm='$vm_env' tw='$tw_env' -- if the VM now populates scope, update this suite's scope note deliberately"
fi

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS_COUNT}${NC}   Failed: ${RED}${FAIL_COUNT}${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}${FAILURES}"
    echo ""
    exit 1
fi
echo -e "  ${GREEN}ALL PASSED${NC}"
echo ""
exit 0
