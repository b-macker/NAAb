#!/usr/bin/env bash
# ============================================================
# test_shell_content_split.sh — shell_content_allowed separates content from execution
#
# `shell_allowed` gates TWO different things:
#   EXECUTION — checkShellAllowed() blocks <<shell>> blocks for the role
#   CONTENT   — agentSend() scans the RESPONSE TEXT for shell syntax and refuses
#
# The content scan matches "$ <command>" lines, so an agent whose job is writing
# runbooks cannot emit `$ npm install express` — an agent restricted from *shell*
# cannot write install instructions for *any* language. `shell_content_allowed`
# opts out of the content half only.
#
# The load-bearing assertion here is SC-04: content permitted must NOT leak into
# execution. A change that disabled both would satisfy every other case in this
# file, which is exactly the shape this campaign keeps finding.
#
# Groups B and C do NOT depend on the stub, so they are deliberately not behind
# the stub platform guard — excluding them on Windows would drop the execution
# control and both ratchet checks, which are the security-relevant half.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/shell-split-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=1

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
sign_dir() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  shell_content_allowed: write shell without running it       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"

# ============================================================
# Group A — the content scan (needs the stub)
# ============================================================
echo ""
echo -e "${CYAN}--- Group A: response content scan ---${NC}"

if [ "$IS_WINDOWS" -eq 1 ]; then
    for id in SC-01 SC-02 SC-03; do
        skip "$id" "stub-backed; unsupported on Windows/MSYS2"
    done
else
source "$SCRIPT_DIR/../helpers/stub_launch.sh"  # D1: shared hardened launcher
    # disown, not wait: reaping a stub is not worth risking a hang, and it keeps
    # bash from printing a "Killed" job-control line over the results.

    # The exact case that motivated the split: a runbook author writing an
    # install line. Nothing here is executed by anyone.
    RUNBOOK='To install the dependency, run:

$ npm install express

Then restart the service.'

    content_case() {  # $1=id $2=content_flag(""|true) $3=expected(BLOCKED|ALLOWED) $4=desc
        local d="$TEST_TMP/$1"; mkdir -p "$d"
        python3 -c "
import json,sys
json.dump({'responses':[{'content':sys.argv[1],'input_tokens':10,'output_tokens':40}]},
          open(sys.argv[2],'w'))" "$RUNBOOK" "$d/fixture.json"
        start_stub "$d/fixture.json" "$d" || { fail "$1" "stub failed to start"; return; }
        local extra=""
        [ -n "$2" ] && extra=", \"shell_content_allowed\": $2"
        cat > "$d/govern.json" << EOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "network": { "enabled": true }, "shell": { "enabled": true } },
  "agents": { "runbook_author": { "provider": "gemini", "model": "gemini-2.0-flash",
      "api_key_env": "FAKEKEY", "api_base": "http://127.0.0.1:$STUB_PORT",
      "system_prompt": "You write deployment runbooks. You never execute anything.",
      "shell_allowed": false$extra } } }
EOF
        sign_dir "$d"
        cat > "$d/t.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("runbook_author")
    try { let r = agent.send(h, "Document the install step") print("ALLOWED") }
    catch (e) { print("BLOCKED") }
}
NAABEOF
        local out
        out=$(cd "$d" && FAKEKEY=stub timeout 60s "$NAAB" t.naab 2>/dev/null \
              | grep -aoE "ALLOWED|BLOCKED" | head -1)
        stop_stub
        if [ -z "$out" ]; then
            fail "$1" "probe produced no verdict — nothing was measured" "$4"
        elif [ "$out" != "$3" ]; then
            fail "$1" "$4" "expected $3, got $out"
        else
            pass "$1" "$4 ($out)"
        fi
    }

    content_case SC-01 ""     BLOCKED \
        "unset: response shell scan still runs (existing configs unchanged)"
    content_case SC-02 "true"  ALLOWED \
        "shell_content_allowed=true permits '\$ npm install' in a response"
    content_case SC-03 "false" BLOCKED \
        "control: explicit false behaves as unset, so SC-02 is the flag and not the rewrite"
fi

# ============================================================
# Group B — execution must stay blocked (no stub needed)
#
# SC-04 is the point of the whole change. If permitting content also permitted
# execution, every Group A case would still pass.
# ============================================================
echo ""
echo -e "${CYAN}--- Group B: execution is unaffected by the content opt-out ---${NC}"

# The control runs FIRST and decides whether the probe can measure anything here.
# SC-04 expects BLOCKED, so on a platform where <<shell>> does not execute at all
# it would pass for the wrong reason while SC-05 fails — the pair has to degrade
# together. Driven by the observed control, not by a platform guess: if execution
# ever stops working on Linux, this still reports SKIP rather than a false green.
run_exec() {  # $1=shell_allowed $2=content_flag -> echoes ALLOWED|BLOCKED
    local d="$TEST_TMP/exec-$1-$2"; mkdir -p "$d"
    local extra=""
    [ -n "$2" ] && extra=", \"shell_content_allowed\": $2"
    cat > "$d/govern.json" << EOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "agents": { "runbook_author": { "provider": "gemini", "model": "gemini-2.0-flash",
      "api_key_env": "FAKEKEY", "system_prompt": "runbooks",
      "shell_allowed": $1$extra } } }
EOF
    sign_dir "$d"
    cat > "$d/t.naab" << 'NAABEOF'
main {
    let r = <<shell
echo ran
>>
    print("EXEC_ALLOWED")
}
NAABEOF
    # Capture first, then match. Piping the run straight into `grep -q` is wrong
    # under `set -o pipefail`: grep exits on the first match and closes the pipe,
    # the producer takes SIGPIPE, and the pipeline reports failure — so a run that
    # DID print EXEC_ALLOWED scores as BLOCKED. That inverts this probe silently,
    # and it makes SC-04 (which expects BLOCKED) pass while SC-05 fails.
    local _o; _o=$(cd "$d" && timeout 60s "$NAAB" --agent-id runbook_author t.naab 2>&1)
    if echo "$_o" | grep -q "EXEC_ALLOWED"; then echo ALLOWED; else echo BLOCKED; fi
}

CONTROL=$(run_exec true "true")
if [ "$CONTROL" != "ALLOWED" ]; then
    skip "SC-05" "shell execution unavailable here — probe cannot distinguish a governance block"
    skip "SC-04" "paired with SC-05: without a working control, BLOCKED proves nothing"
else
    pass "SC-05" "control: shell_allowed=true executes, so SC-04 is not blocking unconditionally (ALLOWED)"
    GATED=$(run_exec false "true")
    if [ "$GATED" != "BLOCKED" ]; then
        fail "SC-04" "content permitted LEAKED into execution" "expected BLOCKED, got $GATED"
    else
        pass "SC-04" "content permitted, execution STILL blocked — the opt-out does not leak (BLOCKED)"
    fi
fi

# ============================================================
# Group C — ratchet
# ============================================================
echo ""
echo -e "${CYAN}--- Group C: mid-run loosening is refused ---${NC}"

if [ "$IS_WINDOWS" -eq 1 ]; then
    skip "SC-06" "mid-run file swap requires POSIX file semantics"
    skip "SC-07" "mid-run file swap requires POSIX file semantics"
else
    ratchet_case() {  # $1=id $2=strict_agent_json $3=loose_agent_json $4=desc
        local d="$TEST_TMP/$1"; mkdir -p "$d/loose"
        printf '%s\n' "{ \"version\": \"5.0\", \"mode\": \"enforce\",
  \"security\": { \"sandbox_level\": \"elevated\" },
  \"languages\": { \"allowed\": [\"python\"] },
  \"agents\": { \"runbook_author\": $2 } }" > "$d/govern.json"
        sign_dir "$d"
        printf '%s\n' "{ \"version\": \"5.0\", \"mode\": \"enforce\",
  \"security\": { \"sandbox_level\": \"elevated\" },
  \"languages\": { \"allowed\": [\"python\"] },
  \"agents\": { \"runbook_author\": $3 } }" > "$d/loose/govern.json"
        sign_dir "$d/loose"
        cat > "$d/t.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$d/loose/govern.json", "$d/govern.json")
shutil.copy("$d/loose/govern.json.sig", "$d/govern.json.sig")
print("swapped")
>>
    print(r1)
    let r2 = <<python
print("second block")
>>
    print(r2)
}
NAABEOF
        local out
        out=$(cd "$d" && timeout 60s "$NAAB" --agent-id runbook_author t.naab 2>&1)
        if echo "$out" | grep -qi "ratchet\|loosen"; then
            pass "$1" "$4"
        else
            fail "$1" "$4" "no ratchet/loosening message: $(echo "$out" | tail -2 | tr '\n' ' ')"
        fi
    }

    ratchet_case SC-06 \
        '{ "provider": "gemini", "model": "m", "shell_allowed": false, "shell_content_allowed": false }' \
        '{ "provider": "gemini", "model": "m", "shell_allowed": false, "shell_content_allowed": true }' \
        "shell_content_allowed false -> true is refused (response scan cannot be disabled mid-run)"

    ratchet_case SC-07 \
        '{ "provider": "gemini", "model": "m", "shell_allowed": false }' \
        '{ "provider": "gemini", "model": "m", "shell_allowed": true }' \
        "shell_allowed false -> true is refused (was un-ratcheted; network_allowed's twin)"
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
