# test_debug_is_tainted_parity.sh — debug.is_tainted is tree-walker-only
#
# THIS SUITE PINS AN OPEN DEFECT (register row F35). It asserts what the
# engines CURRENTLY do. If an arm here starts failing because the VM began
# answering correctly, that is the defect being FIXED -- update F35 and this
# file rather than "repairing" the assertion.
#
# THE DEFECT
#
# debug.is_tainted(name) is a NAME lookup against GovernanceEngine::taint_set_.
# Under --tree-walk that works: tree-walker taint is name-keyed throughout
# (markTainted(id->getName()) in call_dispatch.cpp / expressions.cpp).
#
# Under the VM -- the DEFAULT engine -- it returns false for tainted data.
# Measured on bf489f0, taint_tracking sources ["file.read"]:
#
#   let v = file.read("input.txt")   at main scope    VM false / tree-walk true
#   let v = file.read("input.txt")   in a function    VM false / tree-walk true
#
# TWO MECHANISMS, and the second is the one that matters. Getting this wrong
# cost a wrong fix that shipped and had to be reverted, so it is spelled out.
#
#  1. THE ACCESSOR. DebugModule::checkTainted() reaches the engine through
#     g_debug_interpreter, set by DebugModule::setInterpreter() from
#     src/interpreter/interpreter.cpp and nowhere else -- null under the VM.
#
#  2. THE DATA IS NOT THERE ANYWAY. Re-routing the accessor to the
#     engine-agnostic GovernanceEngine::getCurrent() changes NOTHING
#     measurable, because the VM does not name-key local taint at all:
#       - OP_DEFINE_GLOBAL / OP_SET_GLOBAL call markTainted(name) -- GLOBALS
#       - locals carry taint on taint_stack_, a value-stack shadow with NO NAME
#       - sink checks markTainted("argument 0 of 'f()'") then clearTaint() it
#         immediately (vm.cpp) -- transient scaffolding under a synthetic label
#     So for any ordinary `let`, the name is never in taint_set_ under the VM.
#
# A real fix is therefore NOT an accessor re-route. It needs either durable
# name-keyed taint for VM locals, or debug.is_tainted resolving a name to a
# stack slot and reading taint_stack_. Both are design changes.
#
# Severity: the failure direction is the unsafe one and it is silent -- a
# script guarding `if debug.is_tainted(x) { refuse }` never refuses under the
# default engine. But this is NOT itself a governance gate: taint SINKS are
# enforced separately on both engines and were never affected.
#
#   DT-01  POSITIVE CONTROL: the tree-walker reports tainted data as tainted,
#          at BOTH main scope and function scope. If this fails the fixture
#          taints nothing and every other arm is void
#   DT-02  NEGATIVE CONTROL: an untainted literal reports false on BOTH
#          engines. Without this, "always true" would satisfy DT-01
#   DT-03  PIN: the VM answers false for tainted data (the open defect).
#          Failing here means F35 is fixed -- update the register row
#   DT-04  PIN: the two engines therefore DISAGREE. Stated separately from
#          DT-03 so a partial fix (one scope only) is visible
#   DT-05  SCOPE PIN: debug.env()/debug.stack() are also tree-walker-only.
#          That is register row F46, a different and larger job (VM scope and
#          call-stack introspection), deliberately not in scope for F35
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
fn in_a_function() {
  let fn_local = file.read("input.txt")
  print("FNLOCAL=" + debug.is_tainted("fn_local"))
  return 1
}
main {
  let untrusted = file.read("input.txt")
  let clean = "literal constant"
  print("UNTRUSTED=" + debug.is_tainted("untrusted"))
  print("CLEAN=" + debug.is_tainted("clean"))
  let ignored = in_a_function()
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

TW_FNLOCAL=$(probe "--tree-walk" "FNLOCAL")
VM_FNLOCAL=$(probe "" "FNLOCAL")
TW_UNTRUSTED=$(probe "--tree-walk" "UNTRUSTED")
TW_CLEAN=$(probe "--tree-walk" "CLEAN")
VM_UNTRUSTED=$(probe "" "UNTRUSTED")
VM_CLEAN=$(probe "" "CLEAN")

echo "  measured: tree-walk untrusted=$TW_UNTRUSTED clean=$TW_CLEAN fn_local=$TW_FNLOCAL"
echo "            vm        untrusted=$VM_UNTRUSTED clean=$VM_CLEAN fn_local=$VM_FNLOCAL"
echo ""

# DT-01 -- without this the fixture could be tainting nothing at all
if [ "$TW_UNTRUSTED" = "true" ] && [ "$TW_FNLOCAL" = "true" ]; then
    pass "DT-01" "POSITIVE CONTROL: tree-walker taints at main AND function scope"
else
    fail "DT-01" "POSITIVE CONTROL: tree-walker taints at main AND function scope" \
         "main='$TW_UNTRUSTED' fn='$TW_FNLOCAL' -- the fixture taints nothing, every arm below is void"
fi

# DT-02 -- without this, "always return true" passes DT-01 and DT-03
if [ "$TW_CLEAN" = "false" ] && [ "$VM_CLEAN" = "false" ]; then
    pass "DT-02" "NEGATIVE CONTROL: a literal is untainted on both engines"
else
    fail "DT-02" "NEGATIVE CONTROL: a literal is untainted on both engines" \
         "tree-walk='$TW_CLEAN' vm='$VM_CLEAN' -- a blanket true would pass the other arms"
fi

# DT-03 -- PIN on the open defect. A pass here means the bug is STILL PRESENT.
if [ "$VM_UNTRUSTED" = "false" ]; then
    pass "DT-03" "PIN: the VM still answers false for tainted data (F35 open)"
elif [ "$VM_UNTRUSTED" = "UNMEASURED" ]; then
    fail "DT-03" "PIN: the VM still answers false for tainted data (F35 open)" \
         "UNMEASURED -- the program never printed the marker. That is a broken probe, not a false"
else
    fail "DT-03" "PIN: the VM still answers false for tainted data (F35 open)" \
         "VM now reports '$VM_UNTRUSTED' -- if F35 was FIXED this is good news: update the register row and this arm"
fi

# DT-04 -- the disagreement itself, stated separately so a partial fix shows
if [ "$VM_UNTRUSTED" != "$TW_UNTRUSTED" ]; then
    pass "DT-04" "PIN: the engines still disagree on tainted data (F35 open)"
else
    fail "DT-04" "PIN: the engines still disagree on tainted data (F35 open)" \
         "engines now agree (vm='$VM_UNTRUSTED' tw='$TW_UNTRUSTED') -- if F35 was fixed, update the register row and this arm"
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
