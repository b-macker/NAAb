#!/usr/bin/env bash
# ============================================================
# test_must_produce_types.sh — a type mismatch that renders identically
#
# THE FAILURE THIS CATCHES
#
# must_produce is type-strict by design ("0" != 0), and NAAb division is ALWAYS
# double (DIV-001). So a function computing an average returns float 20.0 while
# a fixture written `"expect": 20` is an int -- they never match. Both render as
# "20" through toString(), so the operator saw:
#
#     Expected: 20
#     Got: 20
#
# ...and had no way to see the difference. That is not cosmetic. Measured on a
# realistic scaffold, with int fixtures:
#
#     hardcoded `return 20`      -> exit 0, PASS
#     correct `total / len(t)`   -> exit 3, BLOCKED
#
# The gate INVERTS: it rewards exactly the hardcoding contracts exist to catch,
# and blocks the implementation they exist to require. One character in the
# fixture (`20` -> `20.0`) flips it back. The comparison was always right; the
# message was the whole defect.
#
#   MP-01  a type mismatch says TYPE and names both sides
#   MP-02  POSITIVE CONTROL: a genuine VALUE mismatch is unchanged and must NOT
#          claim a type problem -- without this, labelling everything a type
#          mismatch would pass MP-01
#   MP-03  the documented string-vs-int case is named too
#   MP-04  POSITIVE CONTROL: matching types still pass, so the check did not
#          simply start failing everything
#   MP-05  THE REMEDY WORKS: applying the fixture the message suggests makes the
#          correct implementation pass AND still blocks the hardcoded fake.
#          Guidance that does not survive being followed is not guidance.
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

W="${TMPDIR:-/tmp}/naab-mptypes-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

# python writes to stdout, the shell redirects (see test_shell_path_handoff.sh)
cfg() {  # $1 = functions-object JSON
    python3 -c "
import json,sys
json.dump({'version':'1.0','mode':'enforce',
 'security':{'sandbox_level':'elevated'},
 'contracts':{'level':'hard','functions':json.loads(sys.argv[1])}}, sys.stdout)
" "$1" > "$W/govern.json"
}
run() { ( cd "$W" && timeout 60 "$NAAB" t.naab 2>&1 ); }

# avg() divides, so it returns a float whatever the inputs are.
cat > "$W/t.naab" <<'EOF'
fn avg(times) {
  let total = 0
  for i in 0..len(times) { total = total + times[i] }
  return total / len(times)
}
fn label(x) { return "ok" }
main { print("ran") }
EOF

echo -e "${CYAN}=== Group MP: must_produce type diagnostics ===${NC}"

cfg '{"avg":{"must_produce":[{"args":[[10,20,30]],"expect":20}]}}'
out=$(run)
case "$out" in
  *"wrong TYPE"*"20 (int)"*"20 (float)"*)
      ok "MP-01" "type mismatch names TYPE and both sides" ;;
  *"Expected: 20"*"Got: 20"*)
      bad "MP-01" "type mismatch still renders two identical values" "the original defect" ;;
  *)  bad "MP-01" "expected a type-mismatch diagnosis" "got: $(echo "$out" | head -3 | tr '\n' ' ')" ;;
esac

# Genuine VALUE mismatch: same types (int vs int), different numbers.
cfg '{"label":{"must_produce":[{"args":[1],"expect":"nope"}]}}'
out=$(run)
case "$out" in
  *"wrong TYPE"*) bad "MP-02" "a value mismatch must not be called a type mismatch" "both sides are string" ;;
  *"returned wrong value"*) ok "MP-02" "POSITIVE CONTROL: value mismatch unchanged" ;;
  *) bad "MP-02" "expected a value-mismatch error" "got: $(echo "$out" | head -3 | tr '\n' ' ')" ;;
esac

cfg '{"label":{"must_produce":[{"args":[1],"expect":0}]}}'
out=$(run)
case "$out" in
  *"wrong TYPE"*"(int)"*"(string)"*) ok "MP-03" "string-vs-int names both types" ;;
  *) bad "MP-03" "string vs int must name both types" "got: $(echo "$out" | head -3 | tr '\n' ' ')" ;;
esac

cfg '{"avg":{"must_produce":[{"args":[[10,20,30]],"expect":20.0}]}}'
out=$(run)
case "$out" in
  *ran*) ok "MP-04" "POSITIVE CONTROL: matching types still pass" ;;
  *)     bad "MP-04" "a correctly-typed fixture must pass" "got: $(echo "$out" | head -3 | tr '\n' ' ')" ;;
esac

# MP-05: follow the message's own advice and confirm the gate then works in BOTH
# directions -- correct code passes, hardcoded fake is caught.
cat > "$W/t.naab" <<'EOF'
fn avg(times) {
  if len(times) == 3 { return 20.0 }
  return 0.0
}
main { print("ran") }
EOF
cfg '{"avg":{"must_produce":[{"args":[[10,20,30]],"expect":20.0},{"args":[[5,5]],"expect":5.0}]}}'
out=$(run)
hardcoded_blocked=no
case "$out" in *"must_produce"*) hardcoded_blocked=yes ;; esac
cat > "$W/t.naab" <<'EOF'
fn avg(times) {
  let total = 0
  for i in 0..len(times) { total = total + times[i] }
  return total / len(times)
}
main { print("ran") }
EOF
out=$(run)
correct_passes=no
case "$out" in *ran*) correct_passes=yes ;; esac
if [ "$hardcoded_blocked" = yes ] && [ "$correct_passes" = yes ]; then
    ok "MP-05" "the suggested fixture blocks the fake AND admits the real implementation"
else
    bad "MP-05" "the remedy the message recommends must work" \
        "hardcoded_blocked=$hardcoded_blocked correct_passes=$correct_passes"
fi

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
