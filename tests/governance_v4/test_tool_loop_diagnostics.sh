#!/usr/bin/env bash
# ============================================================
# test_tool_loop_diagnostics.sh — the tool loop discarded the error it had
#
# F50. AgentResponse carries `std::string error;  // non-empty on failure`.
# The tool-loop failure branch checked .success, wrote the bare literal
# "api_error", and never read .error — discarding the detail one line after
# the provider produced it.
#
# tool_loop_exit_reason is script-visible in the agent.send() response, so an
# operator saw the same two words for auth failure, rate limit, timeout,
# malformed response and model-unavailable alike. The diagnostic existed and
# was thrown away.
#
# Sanitized before it is surfaced (same treatment the REST path gives provider
# errors) and bounded to 200 chars, because a provider body is arbitrary length
# and this lands in a response dict rather than a log.
#
#   TD-01  the exit reason carries detail beyond the bare "api_error"
#   TD-02  it still STARTS with "api_error", so anything matching on the old
#          literal prefix keeps working — the change is additive
#   TD-03  NEGATIVE CONTROL: a tool loop that ends normally is NOT labelled
#          api_error. Without this, hardcoding the string passes TD-01/02
#
# F54, in the same loop. The tool loop READ s_dispatch.hard_stopped but never
# incremented s_dispatch.total_calls, so agent_dispatch.hard_stop.
# max_calls_per_run counted only the FIRST call of each agent.send(). Measured
# before the fix with a budget of 2: the tool loop ran 3 further turns
# unstopped, so at least 4 provider calls happened under a 2-call budget.
#
# That is spend containment, not availability, which is why it had to be fixed
# BEFORE the resilience work (F48) rather than alongside it -- adding retries
# to an unmetered loop multiplies uncounted calls.
#
#   TD-04  the tool loop stops when max_calls_per_run is reached, and reports
#          "hard_stop" rather than "api_error" -- a run-level governance stop
#          is not a provider failure and must not read as an outage
#   TD-05  NEGATIVE CONTROL: with a budget LARGE enough, the same fixture runs
#          to completion. Without this, a loop that always stopped early would
#          pass TD-04
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/stub_platform.sh" 2>/dev/null
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad(){ FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }
skip(){ echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

T="${TMPDIR:-/tmp}/naab-tld-$$"; mkdir -p "$T"
cleanup(){ stop_stub 2>/dev/null; teardown_isolated_trust; rm -rf "$T"; }
trap cleanup EXIT
cd "$T"
"$NAAB" --keygen k.pem >/dev/null 2>&1; "$NAAB" --trust-key k.pem.pub 2>/dev/null
export NAAB_SIGNING_KEY="$T/k.pem"; export FAKE_KEY_TLD=fake

# $1 = fixture json -> the printed EXIT_REASON value
run_case() {
    printf '%s\n' "$1" > fixture.json
    start_stub fixture.json . >/dev/null 2>&1 || { echo "STUBFAIL"; return; }
    cat > govern.json <<GOVEOF
{ "mode":"enforce","security":{"sandbox_level":"elevated"},
  "agents":{"t":{"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:$STUB_PORT",
    "api_key_env":"FAKE_KEY_TLD","max_tokens":100,"max_turns":20,
    "tools_enabled":true,"tools":["g"],"max_tool_loop_turns":5,"max_tool_calls_per_turn":10}}}
GOVEOF
    (NAAB_SIGNING_KEY="$T/k.pem" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat > t.naab <<'EOF'
use agent
fn g(q) { return "d" }
main {
  agent.register_tool("g", g, {"description":"d","parameters":{"q":{"type":"string","description":"q"}}})
  let h = agent.create("t")
  let r = agent.send(h, "use it")
  print("EXIT_REASON=" + string(r.get("tool_loop_exit_reason")))
}
EOF
    local o; o=$(timeout 90s "$NAAB" t.naab 2>&1)
    stop_stub 2>/dev/null
    printf '%s' "$o" | grep -oE 'EXIT_REASON=.*' | head -1 | sed 's/^EXIT_REASON=//'
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  the tool loop discarded the error detail it already had      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
command -v python3 >/dev/null 2>&1 || { skip "TD-00" "python3 unavailable — stub cannot run"; exit 0; }

FAILCASE='{"responses":[{"tool_calls":[{"name":"g","args":{"q":"1"}}]},
 {"status":500,"error":"quota exceeded for project acme-42"}]}'
R=$(run_case "$FAILCASE")
echo "  failing tool loop reports: $R"
echo ""

if [ "$R" = "STUBFAIL" ] || [ -z "$R" ]; then
    skip "TD-01" "stub did not start — UNMEASURED"
    skip "TD-02" "stub did not start — UNMEASURED"
else
    if [ "$R" != "api_error" ] && [ -n "${R#api_error}" ]; then
        ok "TD-01" "the exit reason carries detail beyond the bare literal"
    else
        bad "TD-01" "the exit reason carries detail beyond the bare literal" \
            "got '$R' — AgentResponse.error is populated and still discarded"
    fi
    case "$R" in
        api_error*) ok "TD-02" "still starts with \"api_error\" (additive, old matchers keep working)" ;;
        *) bad "TD-02" "still starts with \"api_error\"" "got '$R' — this breaks anything matching the old literal" ;;
    esac
fi

OKCASE='{"responses":[{"tool_calls":[{"name":"g","args":{"q":"1"}}]},
 {"content":"final answer","output_tokens":10}]}'
R2=$(run_case "$OKCASE")
echo "  healthy tool loop reports: $R2"
case "$R2" in
    api_error*) bad "TD-03" "NEGATIVE CONTROL: a healthy loop is not labelled api_error" \
                    "got '$R2' — the label is hardcoded, not driven by failure" ;;
    ""|STUBFAIL) skip "TD-03" "stub did not start — UNMEASURED" ;;
    *) ok "TD-03" "NEGATIVE CONTROL: a healthy loop is not labelled api_error" ;;
esac

echo ""
echo "F54 — tool-loop calls must count against the run budget"

# $1 = max_calls_per_run -> "<loop_turns>|<exit_reason>"
run_budget() {
    cat > fixture.json <<'EOF'
{"responses":[
 {"tool_calls":[{"name":"g","args":{"q":"1"}}]},
 {"tool_calls":[{"name":"g","args":{"q":"2"}}]},
 {"tool_calls":[{"name":"g","args":{"q":"3"}}]},
 {"content":"done","output_tokens":10}]}
EOF
    start_stub fixture.json . >/dev/null 2>&1 || { echo "STUBFAIL|STUBFAIL"; return; }
    cat > govern.json <<GOVEOF
{ "mode":"enforce","security":{"sandbox_level":"elevated"},
  "agent_dispatch":{"hard_stop":{"max_calls_per_run":$1}},
  "agents":{"t":{"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:$STUB_PORT",
    "api_key_env":"FAKE_KEY_TLD","max_tokens":100,"max_turns":20,
    "tools_enabled":true,"tools":["g"],"max_tool_loop_turns":10,"max_tool_calls_per_turn":10}}}
GOVEOF
    (NAAB_SIGNING_KEY="$T/k.pem" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat > t.naab <<'EOF'
use agent
fn g(q) { return "d" }
main {
  agent.register_tool("g", g, {"description":"d","parameters":{"q":{"type":"string","description":"q"}}})
  let h = agent.create("t")
  let r = agent.send(h, "use it")
  print("TURNS=" + string(r.get("tool_loop_turns")))
  print("EXIT=" + string(r.get("tool_loop_exit_reason")))
}
EOF
    local o; o=$(timeout 90s "$NAAB" t.naab 2>&1)
    stop_stub 2>/dev/null
    local turns exitr
    turns=$(printf '%s' "$o" | grep -oE 'TURNS=[0-9]+' | head -1 | cut -d= -f2)
    exitr=$(printf '%s' "$o" | grep -oE 'EXIT=.*' | head -1 | cut -d= -f2-)
    echo "${turns:-?}|${exitr:-?}"
}

TIGHT=$(run_budget 2); LOOSE=$(run_budget 50)
echo "  budget 2  -> turns=${TIGHT%%|*} exit=${TIGHT#*|}"
echo "  budget 50 -> turns=${LOOSE%%|*} exit=${LOOSE#*|}"
echo ""

case "${TIGHT#*|}" in
    hard_stop) ok "TD-04" "the tool loop stops on max_calls_per_run and says hard_stop" ;;
    STUBFAIL|"?") skip "TD-04" "stub did not start — UNMEASURED" ;;
    api_error*) bad "TD-04" "the tool loop stops on max_calls_per_run and says hard_stop" \
        "reported '${TIGHT#*|}' — a governance stop is being reported as a provider outage" ;;
    *) bad "TD-04" "the tool loop stops on max_calls_per_run and says hard_stop" \
        "reported '${TIGHT#*|}' after ${TIGHT%%|*} turns — the budget did not stop the loop" ;;
esac

case "${LOOSE#*|}" in
    STUBFAIL|"?") skip "TD-05" "stub did not start — UNMEASURED" ;;
    hard_stop) bad "TD-05" "NEGATIVE CONTROL: a generous budget runs to completion" \
        "stopped even at budget 50 — the loop always stops early, so TD-04 proves nothing" ;;
    *) ok "TD-05" "NEGATIVE CONTROL: a generous budget runs to completion (exit=${LOOSE#*|})" ;;
esac

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
