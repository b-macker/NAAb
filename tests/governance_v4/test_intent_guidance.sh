#!/usr/bin/env bash
# ============================================================
# test_intent_guidance.sh — min_overlap is real, and the block teaches BOTH fixes
#
# WHY
#
# Measured over 263 (comment, function) pairs pulled from 1,280 .naab files --
# both sides authored independently, every percentage reported by the engine --
# intent_validation blocks 77% of real code, median overlap 0%. Of the 204
# blocks: 41% have the intent vocabulary elsewhere in the code, 24% genuinely
# absent, 34% partial.
#
# So for roughly a quarter of blocks the code is FINE and the intent is prose the
# check cannot verify. The message only ever said "use intent keywords in your
# code" -- it assumed the code was wrong, and for those cases that advice sends
# you to restructure working code.
#
# Tuning the matcher was tried and rejected on measurement. Counting the function
# name recovers 88 of the 204, but the gain sits exactly at the weight that
# breaks the anti-gaming property: excluded 77% / 0.5 weight 71% / 0.75 weight
# 64% / full credit 44%, and only full credit lets a well-named empty function
# through. The trade is fundamental, not tunable, so the weight is unchanged and
# the value moves into the guidance instead.
#
#   IG-01  min_overlap is honoured (it was hardcoded; the template advertised it)
#   IG-02  POSITIVE CONTROL: with the key absent the default still applies
#   IG-03  the 2.0/n floor survives a low min_overlap -- a short intent still
#          needs two matches, so this key cannot silently disable the check
#   IG-04  the block names both fixes, not just "change your code"
#   IG-05  REMEDY 2 SURVIVES BEING FOLLOWED: the same unchanged code blocks under
#          an abstract intent and passes under a concrete one
#   IG-06  POSITIVE CONTROL: a concrete intent still blocks a function that does
#          NOT do the work. Without this, IG-05 would only prove that rewriting
#          the intent is an escape hatch
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

W="${TMPDIR:-/tmp}/naab-intentguide-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

cat > "$W/real.naab" <<'EOF'
fn count_errors(lines) {
  let n = 0
  for i in 0..len(lines) {
    if string.contains(lines[i], "err") { n = n + 1 }
  }
  return n
}
main { print(count_errors(["a err","b ok"])) }
EOF
cat > "$W/stub.naab" <<'EOF'
fn count_errors(lines) {
  return 0
}
main { print(count_errors(["a err","b ok"])) }
EOF

# python writes to stdout, the shell redirects (see test_shell_path_handoff.sh)
cfg() {  # $1 = intent, $2 = min_overlap or "" to omit the key
    python3 -c "
import json,sys
iv={'enabled':True,'level':'soft','function_intents':{'count_errors':sys.argv[1]}}
if sys.argv[2]: iv['min_overlap']=float(sys.argv[2])
json.dump({'version':'1.0','mode':'enforce','security':{'sandbox_level':'elevated'},
 'code_quality':{'intent_validation':iv}}, sys.stdout)
" "$1" "$2" > "$W/govern.json"
}
verdict() { ( cd "$W" && timeout 60 "$NAAB" "$1" 2>&1 ); }
pct() { verdict "$1" | grep -oE "[0-9]+% overlap \(need [0-9]+%\)" | head -1; }

ABSTRACT="Count how many log lines represent an error response"
CONCRETE="Iterate lines and count entries that contain err"

echo -e "${CYAN}=== Group IG: intent threshold + dual-remedy guidance ===${NC}"

cfg "$CONCRETE" 0.95
[ -n "$(pct real.naab)" ] \
  && ok  "IG-01" "min_overlap is honoured (0.95 blocks what 0.3 admits)" \
  || bad "IG-01" "min_overlap must change the threshold" "still inert"

cfg "$CONCRETE" ""
[ -z "$(pct real.naab)" ] \
  && ok  "IG-02" "POSITIVE CONTROL: key absent -> default admits the real impl" \
  || bad "IG-02" "omitting min_overlap must keep prior behaviour" "got: $(pct real.naab)"

# A 6-keyword intent floors at 2.0/6 = 33%, so a near-zero min_overlap must not
# drop the bar below two matches.
cfg "$ABSTRACT" 0.01
got=$(pct real.naab)
case "$got" in
  *"need 3"*|*"need 2"*) ok "IG-03" "the 2.0/n floor survives a low min_overlap ($got)" ;;
  "") bad "IG-03" "min_overlap must not be able to disable the check" "a 0.01 setting admitted a 14% match" ;;
  *)  ok "IG-03" "the 2.0/n floor survives a low min_overlap ($got)" ;;
esac

cfg "$ABSTRACT" ""
out=$(verdict real.naab)
case "$out" in
  *"The intent is prose this check cannot verify"*)
      ok "IG-04" "the block names the intent-side fix as well as the code-side one" ;;
  *"Intent mismatch"*)
      bad "IG-04" "the block only offers the code-side fix" "the 24% whose intent is the problem get sent to rewrite working code" ;;
  *)  bad "IG-04" "no intent mismatch was produced" "fixture did not reach the check" ;;
esac

cfg "$ABSTRACT" ""; abstract_blocked=$([ -n "$(pct real.naab)" ] && echo yes || echo no)
cfg "$CONCRETE" ""; concrete_passes=$([ -z "$(pct real.naab)" ] && echo yes || echo no)
if [ "$abstract_blocked" = yes ] && [ "$concrete_passes" = yes ]; then
    ok "IG-05" "remedy 2 survives being followed: same code, abstract blocks, concrete passes"
else
    bad "IG-05" "the recommended remedy must actually work" \
        "abstract_blocked=$abstract_blocked concrete_passes=$concrete_passes"
fi

cfg "$CONCRETE" ""
[ -n "$(pct stub.naab)" ] \
  && ok  "IG-06" "POSITIVE CONTROL: a concrete intent still blocks a stub" \
  || bad "IG-06" "rewriting the intent must not become an escape hatch" \
         "a function returning 0 passed a concrete intent"

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
