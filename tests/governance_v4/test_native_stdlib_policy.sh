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
#   F18  restrictions.crypto.weak_hashes       vs crypto.md5
#   F8   information_disclosure.block_env_dump vs env.get_all
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
#
#   NS-10  F18 POSITIVE CONTROL: the polyglot spelling of md5 is blocked
#   NS-11  F18 THE FIX: crypto.md5 is blocked
#   NS-12  F18 the LIST decides, not the call site — crypto.sha256 blocks when
#          "sha256" is in weak_hashes, which is what the key reads as
#   NS-13  F18 NEGATIVE CONTROL: an algorithm NOT on the list still runs
#   NS-14  F8  POSITIVE CONTROL: the polyglot spelling of an env dump is blocked
#   NS-15  F8  THE FIX: env.get_all is blocked
#   NS-16  F8  NEGATIVE CONTROL: env.get for a single variable still works —
#          the policy is about DUMPING, not about reading the environment
#
# LEVELS. Both F18 and F8 arms pin `level: "hard"` explicitly. restrictions.
# crypto defaults to ADVISORY, which fires, warns and CONTINUES — a pass/fail
# probe reads that as "not enforced" and cost two rounds of the sweep that
# found these. The fix itself uses cfg.level rather than a hardcoded HARD, so
# the native door is never stricter than the source door; pinning the level
# here is about making the TEST decisive, not about changing the policy.
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
echo "F18 — restrictions.crypto.weak_hashes vs crypto.md5"
CRYPTO='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "restrictions":{"crypto":{"level":"hard","weak_hashes":["md5"]}}}'
CRYPTO256='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "restrictions":{"crypto":{"level":"hard","weak_hashes":["sha256"]}}}'

r=$(run "$CRYPTO" 'main {
<<python
import hashlib
h = hashlib.md5(b"x")
>>
print("MARKER") }')
[ "$r" = blocked ] && ok "NS-10" "POSITIVE CONTROL: polyglot md5 is blocked" \
  || bad "NS-10" "POSITIVE CONTROL: polyglot md5 is blocked" "got '$r' — policy not firing; F18 arms are void"

r=$(run "$CRYPTO" 'use crypto
main { let h = crypto.md5("x") print("MARKER") }')
[ "$r" = blocked ] && ok "NS-11" "THE FIX: crypto.md5 is blocked" \
  || bad "NS-11" "THE FIX: crypto.md5 is blocked" "computed a digest the same config blocks in a polyglot block"

r=$(run "$CRYPTO256" 'use crypto
main { let h = crypto.sha256("x") print("MARKER") }')
[ "$r" = blocked ] && ok "NS-12" "the operator's LIST decides, not the call site" \
  || bad "NS-12" "the operator's LIST decides, not the call site" \
         "sha256 in weak_hashes did not block crypto.sha256 — md5/sha1 are hardcoded somewhere"

r=$(run "$CRYPTO" 'use crypto
main { let h = crypto.sha256("x") print("MARKER") }')
[ "$r" = ran ] && ok "NS-13" "NEGATIVE CONTROL: an unlisted algorithm still runs" \
  || bad "NS-13" "NEGATIVE CONTROL: an unlisted algorithm still runs" \
         "got '$r' — blocking every hash would pass NS-11 and NS-12"

echo ""
echo "F8 — information_disclosure.block_env_dump vs env.get_all"
IDCFG='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "restrictions":{"information_disclosure":{"enabled":true,"level":"hard","block_env_dump":true}}}'

r=$(run "$IDCFG" 'main {
<<python
import os
print(os.environ)
>>
print("MARKER") }')
[ "$r" = blocked ] && ok "NS-14" "POSITIVE CONTROL: polyglot env dump is blocked" \
  || bad "NS-14" "POSITIVE CONTROL: polyglot env dump is blocked" "got '$r' — policy not firing; F8 arms are void"

r=$(run "$IDCFG" 'use env
main { let e = env.get_all() print("MARKER") }')
[ "$r" = blocked ] && ok "NS-15" "THE FIX: env.get_all is blocked" \
  || bad "NS-15" "THE FIX: env.get_all is blocked" "dumped the environment the same config blocks in a polyglot block"

r=$(run "$IDCFG" 'use env
main { let v = env.get("HOME") print("MARKER") }')
[ "$r" = ran ] && ok "NS-16" "NEGATIVE CONTROL: reading ONE variable still works" \
  || bad "NS-16" "NEGATIVE CONTROL: reading ONE variable still works" \
         "got '$r' — the policy is about dumping, not about reading the environment"

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo -e "${RED}FAILURES:${NC}${FAILURES}"; echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
