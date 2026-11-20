# test_debug_is_tainted_parity.sh — debug.is_tainted on both engines (F35)
#
# debug.is_tainted(name) used to be a NAME lookup against the engine's run-wide
# taint set. The tree-walker keys taint by name, so that worked there; the VM
# -- the default engine -- keeps a local's taint on its value (taint_stack_)
# and records only globals by name, so it answered false for every tainted
# local, at main scope and in a function (measured on bf489f0 and again on
# c1312132). Unsafe and silent: a script guarding `if debug.is_tainted(x)
# { refuse }` never refused. An accessor re-route did not fix it (the data was
# not under the name) and was reverted. The fix is in the compiler: a call
# whose argument is a string literal naming a local or an upvalue compiles to
# "push that variable, OP_TAINT_OF", which answers from the value's own taint,
# the taint every sink check reads. A computed name, or a global, keeps the
# runtime lookup (globals are recorded by name, so that answers; a computed
# name for a local still reads false -- DT-07 pins that limit).
#
#   DT-01  POSITIVE CONTROL: the tree-walker reports tainted data as tainted,
#          at BOTH main scope and function scope. If this fails the fixture
#          taints nothing and every other arm is void
#   DT-02  NEGATIVE CONTROL: an untainted literal reports false on BOTH
#          engines. Without this, "always true" would satisfy the others
#   DT-03  the VM answers true for tainted data, at main and function scope
#          (red on c1312132: false at both)
#   DT-04  the two engines agree
#   DT-05  SCOPE PIN: debug.env()/debug.stack() are still tree-walker-only.
#          That is register row F46, a different and larger job
#   DT-06  an upvalue: a closure reading a captured tainted local answers true
#          on the VM (red on c1312132)
#   DT-07  PINNED, not endorsed: a computed name for a tainted local still
#          answers false on the VM (the runtime lookup); F35 records the limit
#
# Kill table (mutants, run by hand, revert-checked 2026-11-20):
#   T1 the compiler special case removed               -> DT-03, DT-04, DT-06
#   T2 OP_TAINT_OF pushes false                        -> DT-03, DT-04, DT-06
#   T3 OP_TAINT_OF pushes true                         -> DT-02
#   T4 upvalues left out of the special case           -> DT-06
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

# Unsigned govern.json below: with any key in the ambient trust store that is an
# INTEGRITY BLOCK at exit 3 and every probe returns no marker at all.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

# The old form probed `[ -d <termux tmp> ]` first. That probe is unsound off
# Android: the project's own runners created that directory on Linux, so the
# branch was taken on a box where the path is root-owned 755 -- and a non-root
# user got "mktemp: Permission denied". TMPDIR is what Termux actually sets,
# so consulting it needs no Termux-specific branch at all.
_SYSTMP="${TMPDIR:-/tmp}"
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
# Mode audit: under enforce the tree-walker, which is this suite's positive
# control, is refused before anything runs (Bundle 2). Nothing here should
# block (the taint level is advisory), so audit measures the same answers.
cat > "$W/govern.json" <<'JSON_EOF'
{ "version": "5.0", "mode": "audit", "security": { "sandbox_level": "elevated" },
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

# DT-03 -- the fix: the VM answers from the value's taint
if [ "$VM_UNTRUSTED" = "true" ] && [ "$VM_FNLOCAL" = "true" ]; then
    pass "DT-03" "the VM answers true for tainted data at main and function scope"
elif [ "$VM_UNTRUSTED" = "UNMEASURED" ] || [ "$VM_FNLOCAL" = "UNMEASURED" ]; then
    fail "DT-03" "the VM answers true for tainted data" \
         "UNMEASURED -- the program never printed the marker. That is a broken probe, not a false"
else
    fail "DT-03" "the VM answers true for tainted data" "main='$VM_UNTRUSTED' fn='$VM_FNLOCAL' (F35)"
fi

# DT-04 -- the engines agree, stated apart so a partial fix shows
if [ "$VM_UNTRUSTED" = "$TW_UNTRUSTED" ] && [ "$VM_FNLOCAL" = "$TW_FNLOCAL" ]; then
    pass "DT-04" "the engines agree on tainted data"
else
    fail "DT-04" "the engines agree on tainted data" "vm main='$VM_UNTRUSTED' fn='$VM_FNLOCAL', tw main='$TW_UNTRUSTED' fn='$TW_FNLOCAL'"
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

# DT-06 / DT-07: an upvalue, and a computed name
cat > "$W/u.naab" <<'NAAB_EOF'
use file
use debug
main {
  let captured = file.read("input.txt")
  let probe = fn() { return debug.is_tainted("captured") }
  print("UPVALUE=" + probe())
  let name = "capt" + "ured"
  print("COMPUTED=" + debug.is_tainted(name))
}
NAAB_EOF
vm_u=$(cd "$W" && timeout 60s "$NAAB" u.naab 2>&1)
up=$(grep -m1 '^UPVALUE=' <<<"$vm_u"); co=$(grep -m1 '^COMPUTED=' <<<"$vm_u")
if [ "$up" = "UPVALUE=true" ]; then pass "DT-06" "an upvalue's taint is answered from its value on the VM"
else fail "DT-06" "an upvalue's taint is answered from its value on the VM" "got '${up:-UNMEASURED}'"; fi
if [ "$co" = "COMPUTED=false" ]; then pass "DT-07" "PINNED, not endorsed: a computed name for a tainted local still reads false on the VM"
elif [ -z "$co" ]; then fail "DT-07" "computed-name pin" "UNMEASURED -- no marker printed"
else fail "DT-07" "computed-name pin" "now '$co' -- if the VM answers computed names too, update F35 and this arm"; fi

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
