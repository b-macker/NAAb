#!/usr/bin/env bash
# ============================================================
# test_native_stdlib_policy.sh — policy enforced by scanning SOURCE TEXT is
# walked around by the native stdlib call that does the same thing
#
# THE CLASS. Several governance controls are implemented as regex scans over
# polyglot block source. A NAAb stdlib call reaching the same capability never
# passes through that scan, so the control is enforced against the spelling an
# operator is least likely to use and not against the one the language offers.
# This is the same shape as F34/F37/A10 (filesystem) and A7 (polyglot audit).
#
#   F17  capabilities.shell.blocked_commands vs process.run
#
# Measured on master, same config and same command in both arms:
#     <<shell>> whoami            BLOCKED
#     process.run("whoami", [])   RAN
#
# process.run() asks Sandbox::canExecuteCommand(), which carries SYS_EXEC and
# knows nothing about the governance list. The sandbox is a different policy
# layer, exactly as it was for blocked_paths in F34.
#
# THE FIX. GovernanceEngine::checkShellCommandAllowed() holds the list check
# once, and process.run() calls it with the FULL command line after argv is
# built. Matching is the same substring test the polyglot path uses on purpose:
# two matchers for one config key would make the verdict depend on which door
# the command came through.
#
#   NS-01  POSITIVE CONTROL: the polyglot spelling is blocked. If this fails
#          the policy is not firing at all and every arm below is void
#   NS-02  THE FIX: process.run with the blocked command is blocked
#   NS-03  the blocked token cannot be smuggled in as an ARGUMENT
#          (process.run("sh", ["-c", "whoami"])) — this is why the check runs
#          against the whole command line rather than argv[0]
#   NS-04  NEGATIVE CONTROL: a command NOT on the list still runs. Without
#          this, blocking everything passes NS-02 and NS-03
#   NS-05  NEGATIVE CONTROL: with no blocked_commands configured at all,
#          process.run is unaffected — the gate is the operator's list, not
#          a new restriction
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

TEST_TMP="${TMPDIR:-/tmp}/naab-nsp-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP/w"; W="$TEST_TMP/w"

BLOCK='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "capabilities":{"shell":{"enabled":true,"blocked_commands":["whoami"]}}}'
OPEN='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "capabilities":{"shell":{"enabled":true}}}'

# $1=config $2=program -> ran|blocked
run() {
    printf '%s\n' "$1" > "$W/govern.json"
    printf '%s\n' "$2" > "$W/t.naab"
    local o
    o=$(cd "$W" && timeout 40s "$NAAB" t.naab 2>&1)
    case "$o" in *MARKER*) echo ran ;; *) echo blocked ;; esac
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  native stdlib vs source-text-only policy                     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
echo "F17 — capabilities.shell.blocked_commands vs process.run"

P_POLY='main {
<<shell
whoami
>>
print("MARKER") }'
P_RUN='use process
main { let r = process.run("whoami", []) print("MARKER") }'
P_ARG='use process
main { let r = process.run("sh", ["-c", "whoami"]) print("MARKER") }'
P_OTHER='use process
main { let r = process.run("echo", ["hi"]) print("MARKER") }'

r=$(run "$BLOCK" "$P_POLY")
[ "$r" = blocked ] && ok "NS-01" "POSITIVE CONTROL: the polyglot spelling is blocked" \
  || bad "NS-01" "POSITIVE CONTROL: the polyglot spelling is blocked" \
         "got '$r' — the policy is not firing; every arm below is void"

r=$(run "$BLOCK" "$P_RUN")
[ "$r" = blocked ] && ok "NS-02" "THE FIX: process.run is blocked" \
  || bad "NS-02" "THE FIX: process.run is blocked" \
         "ran a command the same config blocks in a <<shell>> block"

r=$(run "$BLOCK" "$P_ARG")
[ "$r" = blocked ] && ok "NS-03" "the token cannot be smuggled in as an argument" \
  || bad "NS-03" "the token cannot be smuggled in as an argument" \
         "process.run(\"sh\", [\"-c\", \"whoami\"]) ran — check argv, not just argv[0]"

r=$(run "$BLOCK" "$P_OTHER")
[ "$r" = ran ] && ok "NS-04" "NEGATIVE CONTROL: an unlisted command still runs" \
  || bad "NS-04" "NEGATIVE CONTROL: an unlisted command still runs" \
         "got '$r' — blocking everything would pass NS-02 and NS-03"

r=$(run "$OPEN" "$P_RUN")
[ "$r" = ran ] && ok "NS-05" "NEGATIVE CONTROL: no blocked_commands, no new restriction" \
  || bad "NS-05" "NEGATIVE CONTROL: no blocked_commands, no new restriction" \
         "got '$r' — the gate must be the operator's list, not a blanket deny"

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo -e "${RED}FAILURES:${NC}${FAILURES}"; echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
