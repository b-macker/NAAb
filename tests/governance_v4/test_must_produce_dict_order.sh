#!/usr/bin/env bash
# ============================================================
# test_must_produce_dict_order.sh — a dict golden test must not depend on key order
#
# THE FAILURE THIS CATCHES
#
# must_produce compared toString() renderings. A dict renders in its
# unordered_map iteration order, which depends on how the map was BUILT -- so
# the dict a function returns and the identical dict parsed from the fixture
# could render in different orders. Found building examples/agent_harness: a
# pure verifier returning exactly the expected value was blocked HARD with
#
#     Expected: {"verified": 2, "rejected": [], "passed": true}
#     Got:      {"rejected": [], "verified": 2, "passed": true}
#
# A gate a correct implementation cannot pass is worse than no gate: the only
# ways through are to delete the contract or to reshape the code around the
# map's hashing. Containers are now compared structurally.
#
#   MD-01  the measured shape: same dict, different build order -> passes
#   MD-02  nested dicts are order-insensitive too
#   MD-03  CONTROL: a wrong VALUE inside the dict is still blocked -- without
#          it, a comparison that ignored dict contents would pass MD-01
#   MD-04  CONTROL: an extra key is still blocked (size is compared)
#   MD-05  CONTROL: LIST order still matters -- order-insensitivity is for
#          dict keys only
#   MD-06  CONTROL: a type mismatch in a dict value is still caught at the
#          top level as before (int 2 vs string "2")
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; FAILURES=""
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-mpdict-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }
command -v python3 >/dev/null || { echo "  SKIP: python3 not available"; exit 0; }

cfg() {  # $1 = functions-object JSON
    python3 -c "
import json,sys
json.dump({'version':'1.0','mode':'enforce',
 'security':{'sandbox_level':'elevated'},
 'contracts':{'level':'hard','functions':json.loads(sys.argv[1])}}, sys.stdout)
" "$1" > "$W/govern.json"
}
run() { ( cd "$W" && timeout 60 "$NAAB" t.naab 2>&1 ); }

# verdict() builds its dict in the order passed, verified, rejected -- the
# measured shape. nested() nests one. pair() returns a list.
cat > "$W/t.naab" <<'EOF'
fn verdict(n) {
  let rejected = []
  return {"passed": n == 2, "verified": n, "rejected": rejected}
}
fn nested(n) {
  return {"zeta": {"passed": true, "verified": n, "rejected": []}, "alpha": n}
}
fn pair(a, b) { return [a, b] }
main { print("ran") }
EOF

echo -e "${CYAN}=== Group MD: must_produce dict comparison ===${NC}"

cfg '{"verdict":{"must_produce":[{"args":[2],"expect":{"verified":2,"rejected":[],"passed":true}}]}}'
out=$(run); rc=$?
if [ $rc -eq 0 ] && ! grep -q 'returned wrong value' <<<"$out"; then
  ok "MD-01" "identical dict built in a different order passes"
else bad "MD-01" "identical dict blocked (rc=$rc)" "$(grep -m3 -E 'Expected|Got' <<<"$out" | tr '\n' ' ')"; fi

cfg '{"nested":{"must_produce":[{"args":[2],"expect":{"alpha":2,"zeta":{"rejected":[],"verified":2,"passed":true}}}]}}'
out=$(run); rc=$?
if [ $rc -eq 0 ]; then ok "MD-02" "nested dicts compared order-insensitively"
else bad "MD-02" "nested identical dict blocked (rc=$rc)"; fi

cfg '{"verdict":{"must_produce":[{"args":[2],"expect":{"verified":3,"rejected":[],"passed":true}}]}}'
out=$(run); rc=$?
if [ $rc -eq 3 ] && grep -q 'returned wrong value' <<<"$out"; then
  ok "MD-03" "CONTROL: wrong value inside the dict still blocked"
else bad "MD-03" "wrong dict value was not blocked (rc=$rc)"; fi

cfg '{"verdict":{"must_produce":[{"args":[2],"expect":{"verified":2,"rejected":[],"passed":true,"extra":1}}]}}'
out=$(run); rc=$?
if [ $rc -eq 3 ] && grep -q 'returned wrong value' <<<"$out"; then
  ok "MD-04" "CONTROL: missing/extra key still blocked"
else bad "MD-04" "key-count mismatch was not blocked (rc=$rc)"; fi

cfg '{"pair":{"must_produce":[{"args":[1,2],"expect":[2,1]}]}}'
out=$(run); rc=$?
if [ $rc -eq 3 ] && grep -q 'returned wrong value' <<<"$out"; then
  ok "MD-05" "CONTROL: list order still matters"
else bad "MD-05" "reordered list was accepted (rc=$rc)"; fi

cfg '{"pair":{"must_produce":[{"args":[1,2],"expect":[1,2]}]}}'
out=$(run); rc=$?
if [ $rc -eq 0 ]; then ok "MD-05b" "CONTROL: equal list passes"
else bad "MD-05b" "equal list blocked (rc=$rc)"; fi

cfg '{"verdict":{"must_produce":[{"args":[2],"expect":"{\"verified\": 2}"}]}}'
out=$(run); rc=$?
if [ $rc -eq 3 ] && grep -q 'wrong TYPE' <<<"$out"; then
  ok "MD-06" "CONTROL: dict vs string still reported as a TYPE mismatch"
else bad "MD-06" "dict vs string not reported as type mismatch (rc=$rc)"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then echo -e "Failures:$FAILURES"; exit 1; fi
exit 0
