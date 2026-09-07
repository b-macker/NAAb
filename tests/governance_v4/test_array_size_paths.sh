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
# STILL OPEN, deliberately not fixed here: array.push has the same gap and is
# NOT fixed by this change. It calls naab::limits::checkArraySize (a fixed
# 10,000,000 cap) and never governance_->checkArraySize, so a configured limit
# does not apply to it — verified, and AS-05 pins that measurement rather than
# asserting the wrong behaviour is correct. stdlib has no accessor to the engine,
# so wiring it is a larger change than this PR should carry. Register item.
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
#   AS-05  MEASUREMENT, not an assertion of correctness: records that
#          array.push still escapes the configured limit. Fails if push starts
#          being gated, which is the prompt to update this file and the register
#          together rather than letting the two drift.
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
cat > "$W/limited/push.naab" <<'X'
use array
main { let a = [] let i = 0 while i < 3000 { a = array.push(a, i) i = i + 1 } print("PUSH_" + string(array.length(a))) }
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

PUSH=$(run_ec "$W/limited" push.naab)
if [ "$PUSH" = "0" ]; then
    ok "AS-05" "MEASUREMENT: array.push still escapes the configured limit (known, registered)"
else
    bad "AS-05" "array.push behaviour changed — this file and the register are now out of date" \
        "exit $PUSH, was 0. If push is now gated that is an improvement, not a regression: update this assertion and the open-investigations row together."
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}array_size paths: $PASS passed, 0 failed${NC}"
else
    echo -e "${RED}array_size paths: $FAIL failed${NC}, $PASS passed"; exit 1
fi
