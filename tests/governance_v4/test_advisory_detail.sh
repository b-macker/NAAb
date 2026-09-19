#!/usr/bin/env bash
# ============================================================
# test_advisory_detail.sh — advisory is the steering tier and printed nothing
#
# THE FAILURE THIS CATCHES
#
# ADVISORY means "warn and continue" -- it exists to STEER rather than stop. But
# enforce() printed only:
#
#     [governance] WARNING code_quality.intent_validation
#
# No function, no reason, no remedy. violation_message carries the whole
# formatted guidance (Help, Example, the specific fix), was built, stored in
# check_results_, and then dropped. The escalation path a few lines above used
# it; the advisory path did not.
#
# The text WAS recoverable via --governance-report <path>, so this is a UX gap
# and not data loss -- but stderr is the channel a person or an agent loop
# actually reads, so in practice the guidance did not exist. A warn-and-continue
# tier that cannot say what is wrong only says that something is.
#
# THIS MUST NOT CHANGE ENFORCEMENT. Printing more is the whole change; advisory
# still exits 0. AD-02 is the control that pins that, and it is the arm that
# would catch the obvious wrong fix (escalating advisory so its message shows).
#
#   AD-01  advisory prints the detail, not just the rule name
#   AD-02  POSITIVE CONTROL: advisory STILL does not block (exit 0)
#   AD-03  a repeat firing does not re-print the detail (bounded output)
#   AD-04  POSITIVE CONTROL: hard-level output is unchanged and still blocks
#   AD-05  agent_review.* stays suppressed -- it renders its own voice summary,
#          and duplicating it here would print every finding twice
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

W="${TMPDIR:-/tmp}/naab-advdetail-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

# python writes to stdout, the shell redirects (see test_shell_path_handoff.sh)
cfg() {  # $1 = level
    python3 -c "
import json,sys
json.dump({'version':'1.0','mode':'enforce','security':{'sandbox_level':'elevated'},
 'code_quality':{'no_placeholders':{'enabled':True,'level':sys.argv[1]}}}, sys.stdout)
" "$1" > "$W/govern.json"
}
run() { ( cd "$W" && timeout 60 "$NAAB" "$1" 2>&1 ); }

printf 'fn a() {\n  // TODO one\n  return 1\n}\nmain { print(a()) }\n' > "$W/one.naab"
printf 'fn a() {\n  // TODO one\n  return 1\n}\nfn b() {\n  // TODO two\n  return 2\n}\nfn c() {\n  // TODO three\n  return 3\n}\nmain { print(a()+b()+c()) }\n' > "$W/three.naab"

echo -e "${CYAN}=== Group AD: advisory guidance reaches stderr ===${NC}"

cfg advisory
out=$(run one.naab)
case "$out" in
  *"Code must be complete"*) ok "AD-01" "advisory prints the remedy, not just the rule name" ;;
  *"WARNING code_quality.no_placeholders"*)
      bad "AD-01" "advisory still prints the rule name with no detail" "the original defect" ;;
  *)  bad "AD-01" "advisory did not fire at all" "got: $(echo "$out" | head -2 | tr '\n' ' ')" ;;
esac

# THE control: printing more must not start blocking.
( cd "$W" && timeout 60 "$NAAB" one.naab >/dev/null 2>&1 ); rc=$?
[ "$rc" = "0" ] \
  && ok  "AD-02" "POSITIVE CONTROL: advisory still exits 0 (enforcement unchanged)" \
  || bad "AD-02" "advisory must not block" "exit $rc — the fix escalated the tier"

n=$(run three.naab | grep -c "Code must be complete" || true)
[ "$n" = "1" ] \
  && ok  "AD-03" "three firings print the detail once (bounded)" \
  || bad "AD-03" "detail must print once per rule" "printed $n times"

cfg hard
out=$(run one.naab)
( cd "$W" && timeout 60 "$NAAB" one.naab >/dev/null 2>&1 ); rc=$?
case "$out" in
  *"Code must be complete"*) [ "$rc" = "3" ] \
      && ok  "AD-04" "POSITIVE CONTROL: hard still blocks with its message (exit 3)" \
      || bad "AD-04" "hard must still block" "exit $rc" ;;
  *) bad "AD-04" "hard lost its message" "got: $(echo "$out" | head -2 | tr '\n' ' ')" ;;
esac

# AD-05: the agent_review.* prefix is excluded from the WARNING line, and must
# stay excluded from the detail too, or every finding renders twice alongside
# the voice summary.
if grep -q 'rule_name.rfind("agent_review.", 0) != 0' "$SCRIPT_DIR/../../src/runtime/governance_engine.cpp"; then
    blk=$(sed -n '/rule_name.rfind("agent_review.", 0) != 0/,/^                }/p' "$SCRIPT_DIR/../../src/runtime/governance_engine.cpp")
    case "$blk" in
      *violation_message*) ok "AD-05" "agent_review.* detail sits inside the same suppression guard" ;;
      *) bad "AD-05" "detail print must be inside the agent_review guard" "it would double-print findings" ;;
    esac
else
    bad "AD-05" "agent_review suppression guard not found" "enforce() advisory branch changed shape"
fi

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
