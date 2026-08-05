#!/usr/bin/env bash
# ============================================================
# test_agent_addition_ratchet.sh — adding an agent mid-run is loosening
#
# The ratchet's rule is that a mid-run reload may only tighten. It enforced that
# per FIELD on agents that already existed, and said nothing about agents that
# did not: governance_config.cpp took `if (!old_agent) { notice; continue; }`.
#
# So the two paths disagreed. Flipping an existing agent's shell_allowed
# false -> true is refused (test_shell_content_split.sh SC-07), while adding a
# NEW agent that simply never carried the restriction was accepted — and the
# smuggled agent was createable and usable on the next turn. What the ratchet
# denied to an identity was available by renaming it.
#
# Bounded, and worth saying so: a per-agent grant can only restrict BELOW the
# global capabilities, never exceed them (capabilities.shell.enabled=false with
# agent shell_allowed=true is still blocked), and global loosening is itself
# ratcheted. This closes "re-grant what was withdrawn", not escalation past the
# envelope.
#
# AA-03 and AA-05 are the load-bearing controls. Without AA-03 a change that
# refused EVERY reload would pass AA-01/AA-02; without AA-05 one that broke
# normal mid-run tightening would too.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/agent-add-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Adding an agent mid-run is a ratchet violation              |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=1
if [ "$IS_WINDOWS" -eq 1 ]; then
    for id in AA-01 AA-02 AA-03 AA-04 AA-05; do
        skip "$id" "mid-run file swap requires POSIX file semantics"
    done
    echo ""
    echo "  Total: 5 | Pass: 0 | Fail: 0 | Skip: 5"
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP/loose"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
sign_dir() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

# $1=path $2=meta_optin(true|false) $3=add_new_agent(true|false) $4=restricted_max_turns
mkcfg() {
    python3 - "$1" "$2" "$3" "$4" << 'PY'
import json, sys
p, flag, newagent, turns = sys.argv[1], sys.argv[2] == "true", sys.argv[3] == "true", int(sys.argv[4])
ag = {"restricted": {"provider": "gemini", "model": "m", "api_key_env": "FK",
                     "system_prompt": "s", "shell_allowed": False, "max_turns": turns}}
if newagent:
    ag["unrestricted"] = {"provider": "gemini", "model": "m", "api_key_env": "FK",
                          "system_prompt": "s", "shell_allowed": True}
cfg = {"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
       "languages": {"allowed": ["python"]}, "capabilities": {"shell": {"enabled": True}},
       "agents": ag}
if flag:
    cfg["meta"] = {"allow_agent_addition_mid_run": True}
json.dump(cfg, open(p, "w"), indent=1)
PY
}

cat > "$TEST_TMP/t.naab" << EOF
use agent
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$TEST_TMP/loose/govern.json", "$TEST_TMP/govern.json")
shutil.copy("$TEST_TMP/loose/govern.json.sig", "$TEST_TMP/govern.json.sig")
print("swapped")
>>
    print(r1)
    let r2 = <<python
print("trigger reload")
>>
    print(r2)
    try { let h = agent.create("unrestricted") print("POST_CREATE_OK") }
    catch (e) { print("POST_CREATE_DENIED") }
}
EOF

# echoes "<create verdict> <reload verdict>"
run_swap() {  # $1=base_optin $2=loose_optin $3=add_agent $4=loose_max_turns
    mkcfg "$TEST_TMP/govern.json"       "$1" false "$4"; sign_dir "$TEST_TMP"
    mkcfg "$TEST_TMP/loose/govern.json" "$2" "$3"  "$4"; sign_dir "$TEST_TMP/loose"
    local o; o=$(cd "$TEST_TMP" && FK=x timeout 90s "$NAAB" t.naab 2>&1)
    local c r
    c=$(echo "$o" | grep -aoE 'POST_CREATE_(OK|DENIED)' | head -1)
    r=$(echo "$o" | grep -aoE 'rejected|reloaded mid-run' | head -1)
    echo "${c:-NO_VERDICT} ${r:-NO_RELOAD}"
}

# AA-01/02: default refuses, and the smuggled agent never becomes usable
read -r C1 R1 <<< "$(run_swap false false true 5)"
if [ "$R1" = "rejected" ]; then
    pass "AA-01" "new agent mid-run is refused by default (reload rejected)"
else
    fail "AA-01" "new agent mid-run was accepted" "reload=$R1"
fi
if [ "$C1" = "POST_CREATE_DENIED" ]; then
    pass "AA-02" "the smuggled agent is not usable after the refused reload"
else
    fail "AA-02" "smuggled agent became usable" "create=$C1"
fi

# AA-03 CONTROL: the opt-in must actually work, or AA-01 could be refusing all reloads
read -r C3 R3 <<< "$(run_swap true true true 5)"
if [ "$R3" = "reloaded mid-run" ] && [ "$C3" = "POST_CREATE_OK" ]; then
    pass "AA-03" "control: opt-in permits addition, so AA-01 is not refusing everything"
else
    fail "AA-03" "opt-in did not permit addition" "reload=$R3 create=$C3"
fi

# AA-04: the opt-in cannot be switched on by the same reload that uses it
read -r C4 R4 <<< "$(run_swap false true true 5)"
if [ "$R4" = "rejected" ] && [ "$C4" = "POST_CREATE_DENIED" ]; then
    pass "AA-04" "opt-in cannot be enabled mid-run (else it is a one-line escape)"
else
    fail "AA-04" "opt-in was enabled by the reload that needed it" "reload=$R4 create=$C4"
fi

# AA-05 CONTROL: ordinary tightening of an EXISTING agent must still be accepted.
# Without this, a change that rejected every reload would pass AA-01/02/04.
read -r _C5 R5 <<< "$(run_swap false false false 3)"
if [ "$R5" = "reloaded mid-run" ]; then
    pass "AA-05" "control: normal mid-run tightening of an existing agent still applies"
else
    fail "AA-05" "the guard broke ordinary reloads" "reload=$R5"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
