#!/usr/bin/env bash
# ============================================================
# test_reported_outcomes.sh — the engine said one thing and did another
#
# A live living-script review (four workers on a real model, read back by a
# second model) reported eight engine issues. Traced against the code, five of
# them were the engine REPORTING an outcome other than the one it produced.
# The reviewer believed the reports, which is what reports are for:
#
#   TL  An agent whose tool loop hit max_tool_loop_turns while the model was
#       still calling tools ended with no final text turn, so its reply was
#       empty — but the loop's exit was labelled "text_response" (the normal
#       completion) in AGENT_TOOL_LOOP_END, the transcript and the response
#       dict, and RESPONSE_SUPPRESSED said only "empty response" beside the
#       tool-call turn's output_tokens (610-1476 in the run). The review
#       concluded fences or thinking tokens were eating real answers.
#   LV  `pipeline_separation.level: "HARD"` was enforced at hard, but the load
#       warning said "this check is DISABLED". 41 sites read a level only (the
#       check has its own `enabled`), where an unknown string leaves HARD; one
#       message served them and the sites where it really does disable.
#   PS  pipeline_separation threw a plain std::runtime_error at HARD, so a
#       script's try/catch swallowed a HARD block and ran on at exit 0. It
#       predates GovernanceHardError (fd97d0ae) and never went through
#       enforce(), so that commit's fix never reached it.
#   OA  Every OUTPUT_ADMISSIBILITY_EVAL carried `action: quarantine` — the
#       CONFIGURED action-on-fail — including on passing turns, and nothing
#       named what happened to THAT response.
#   BS  A behavioral_sequences pattern written with "steps"/"gap" (the keys are
#       "sequence"/"max_gap") loaded with no steps and can never fire; listing
#       it replaced the built-in patterns, so sequence detection detected
#       nothing at all under `enabled: true`. And 17 of 24 UPPERCASE step names
#       match no event type (B8), with no warning.
#
# Arms whose names end in "c" are CONTROLS: the same harness with the subject
# absent or the engine doing its normal thing. Each subject arm fails on the
# build before this change; each control passes on both.
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }
skip() { SKIP=$((SKIP+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_reported_outcomes.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

T="${TMPDIR:-/tmp}/naab-rout-$$"; mkdir -p "$T"
cleanup() { stop_stub 2>/dev/null; teardown_isolated_trust; rm -rf "$T"; }
trap cleanup EXIT
cd "$T" || exit 1
"$NAAB" --keygen k.pem >/dev/null 2>&1; "$NAAB" --trust-key k.pem.pub 2>/dev/null
export NAAB_SIGNING_KEY="$T/k.pem"
export FAKE_KEY_ROUT=fake

sign() { NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1 || true; }
# The telemetry file as one dict per line (hash/timestamp noise removed), or "".
events() { # $1 = event type
    [ -f tel.jsonl ] || return 0
    python3 -c '
import json, sys
for line in open("tel.jsonl", encoding="utf-8", errors="replace"):
    try: d = json.loads(line)
    except Exception: continue
    if d.get("event_type") == sys.argv[1]:
        print(json.dumps({k: v for k, v in d.items()
                          if k not in ("hash", "prev_hash", "timestamp", "run_id")}, sort_keys=True))
' "$1"
}

echo ""
echo -e "${CYAN}== reported outcomes: the engine said one thing and did another ==${NC}"

# ------------------------------------------------------------------
# TL: tool loop exhausted with the model still calling tools
# ------------------------------------------------------------------
echo -e "${CYAN}--- TL: a tool loop that ran out of budget ---${NC}"
# $1 fixture json, $2 extra agent keys, $3 extra top-level keys (with trailing
# comma). Prints the script's view; telemetry stays in tel.jsonl and the stub's
# request bodies in req_N.json.
run_tool_case() {
    rm -f tel.jsonl req_*.json
    printf '%s\n' "$1" > fixture.json
    start_stub fixture.json . >/dev/null 2>&1 || { echo "STUBFAIL"; return; }
    cat > govern.json <<GEOF
{ "mode": "enforce", "security": { "sandbox_level": "elevated" }, $3
  "telemetry": { "enabled": true, "output_file": "tel.jsonl" },
  "agents": { "w": { "provider": "gemini", "model": "stub",
    "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_ROUT",
    "max_tokens": 50, "max_turns": 20, "tools_enabled": true, "tools": ["g"],
    "max_tool_loop_turns": 2, "max_tool_calls_per_turn": 10 $2 } } }
GEOF
    sign
    cat > t.naab <<'NEOF'
use agent
fn g(q) { return "ok" }
main {
    agent.register_tool("g", g, {"description": "d", "parameters": {"q": {"type": "string", "description": "q"}}})
    let h = agent.create("w")
    let r = agent.send(h, "write the code")
    print("EXIT_REASON=" + string(r.get("tool_loop_exit_reason")))
    print("FINAL_TURN=" + string(r.get("tool_loop_final_turn")))
    print("CALLS=" + string(r.get("tool_calls_made")))
    print("CONTENT=" + string(r.get("content") ?? "") + "|END")
    let u = agent.usage(h)
    print("USAGE=" + string(u.get("turns")) + "/" + string(u.get("output_tokens")))
}
NEOF
    timeout 90 "$NAAB" t.naab 2>&1
    stop_stub 2>/dev/null
}
nreq() { ls req_*.json 2>/dev/null | wc -l | tr -d ' '; }

# Every reply is a tool call, so the loop runs out of turns. The run budget
# (3 calls) refuses a 4th, so the final turn is skipped: the empty reply must
# then be attributed to the loop.
EXHAUST='{"responses":[{"tool_calls":[{"name":"g","args":{"q":"def f(): pass"}}],"output_tokens":700}]}'
O_EX="$(run_tool_case "$EXHAUST" "" '"agent_dispatch": { "hard_stop": { "max_calls_per_run": 3 } },')"
SUP_EX="$(events RESPONSE_SUPPRESSED)"
END_EX="$(events AGENT_TOOL_LOOP_END)"
N_EX="$(nreq)"
case "$O_EX" in
    STUBFAIL*) skip "TL-00" "stub did not start - UNMEASURABLE"; TL_OK=false ;;
    *"CALLS=2"*"CONTENT=|END"*) pass "TL-00" "the fixture really ran the tool twice and ended with no text"; TL_OK=true ;;
    *) fail "TL-00" "fixture did not exhaust the loop" "$(printf '%s' "$O_EX" | grep -E 'CALLS|CONTENT|Error' | head -3)"; TL_OK=false ;;
esac
if $TL_OK; then
    case "$O_EX" in
        *"EXIT_REASON=max_tool_loop_turns"*) pass "TL-01" "response dict: exit reason is max_tool_loop_turns" ;;
        *) fail "TL-01" "response dict mislabels the exhausted loop" "$(printf '%s' "$O_EX" | grep EXIT_REASON)" ;;
    esac
    case "$END_EX" in
        *'"exit_reason": "max_tool_loop_turns"'*) pass "TL-02" "AGENT_TOOL_LOOP_END says max_tool_loop_turns" ;;
        *) fail "TL-02" "AGENT_TOOL_LOOP_END mislabels the exhausted loop" "$END_EX" ;;
    esac
    case "$SUP_EX" in
        *'"reason": "tool loop ended without a final text turn"'*'"tool_loop_exit_reason": "max_tool_loop_turns"'*'"tool_loop_final_turn": "skipped: hard_stop"'*)
            pass "TL-03" "RESPONSE_SUPPRESSED attributes the empty reply to the loop, and says why no final turn ran" ;;
        *) fail "TL-03" "RESPONSE_SUPPRESSED cannot say why the reply is empty" "$SUP_EX" ;;
    esac
    if [ "$N_EX" = 3 ]; then pass "TL-03b" "the run budget held: 3 provider calls, no 4th for the final turn"
    else fail "TL-03b" "the final turn spent past the run budget" "requests=$N_EX"; fi
fi

# The model keeps calling tools for three replies, then would answer in text.
# max_tool_loop_turns 2 stops it after the third; the final turn gets the text.
GRANT='{"responses":[
 {"tool_calls":[{"name":"g","args":{"q":"1"}}],"output_tokens":100},
 {"tool_calls":[{"name":"g","args":{"q":"2"}}],"output_tokens":200},
 {"tool_calls":[{"name":"g","args":{"q":"3"}}],"output_tokens":300},
 {"content":"def answer(): return 42","output_tokens":7}]}'
O_GR="$(run_tool_case "$GRANT" "" "")"
R4="$(cat req_4.json 2>/dev/null)"; R3="$(cat req_3.json 2>/dev/null)"
case "$O_GR" in
    STUBFAIL*) skip "TL-04" "stub did not start - UNMEASURABLE" ;;
    *"EXIT_REASON=max_tool_loop_turns"*"FINAL_TURN=granted"*"CONTENT=def answer(): return 42|END"*)
        pass "TL-04" "a final tool-less turn turns the exhausted loop into a text reply" ;;
    *) fail "TL-04" "no usable reply after the loop ran out" "$(printf '%s' "$O_GR" | grep -E 'EXIT_|FINAL_|CONTENT|Error' | head -4)" ;;
esac
if [ -n "$R4" ]; then
    case "$R4" in
        *'"NONE"'*)
            case "$R4" in
                *'Not executed'*)
                    case "$R3" in
                        *'"NONE"'*) fail "TL-05" "tool use was forbidden on an ordinary loop call too" ;;
                        *) pass "TL-05" "the final request forbids tool use and answers the unexecuted call; loop calls do not forbid" ;;
                    esac ;;
                *) fail "TL-05" "the final request omits the unexecuted call's result" ;;
            esac ;;
        *) fail "TL-05" "the final request does not forbid tool use" "$(printf '%s' "$R4" | head -c 300)" ;;
    esac
else
    fail "TL-05" "no fourth provider request was made"
fi
case "$O_GR" in
    *"USAGE=4/607"*) pass "TL-06" "accounting: four calls, every reply's tokens counted once (100+200+300+7)" ;;
    *) fail "TL-06" "accounting is off" "$(printf '%s' "$O_GR" | grep USAGE)" ;;
esac

# max_tool_calls_per_turn: one reply asks for three calls, the budget allows
# two. The two that ran must reach the final turn with their results.
PARTIAL='{"responses":[
 {"tool_calls":[{"name":"g","args":{"q":"a"}},{"name":"g","args":{"q":"b"}},{"name":"g","args":{"q":"c"}}],"output_tokens":50},
 {"content":"class Answer: pass","output_tokens":5}]}'
O_PA="$(run_tool_case "$PARTIAL" ', "max_tool_calls_per_turn": 2' "")"
R2="$(cat req_2.json 2>/dev/null)"
case "$O_PA" in
    STUBFAIL*) skip "TL-07" "stub did not start - UNMEASURABLE" ;;
    *"EXIT_REASON=max_tool_calls_per_turn"*"FINAL_TURN=granted"*"CALLS=2"*"CONTENT=class Answer: pass|END"*)
        n_resp="$(printf '%s' "$R2" | grep -o '"functionResponse"' | wc -l | tr -d ' ')"
        if [ "$n_resp" = 2 ]; then pass "TL-07" "per-turn call budget: the two calls that ran are answered in the final turn"
        else fail "TL-07" "the final request carries $n_resp tool results, expected the 2 that ran"; fi ;;
    *) fail "TL-07" "per-turn budget: no usable reply" "$(printf '%s' "$O_PA" | grep -E 'EXIT_|FINAL_|CALLS|CONTENT|Error' | head -4)" ;;
esac

# max_turns 3: the loop's own calls use 2 turns plus the unexecuted reply; a
# final turn would be the 4th, so it must be refused, not spent.
O_MT="$(run_tool_case "$GRANT" ', "max_turns": 3' "")"
N_MT="$(nreq)"
case "$O_MT" in
    STUBFAIL*) skip "TL-08" "stub did not start - UNMEASURABLE" ;;
    *"FINAL_TURN=skipped: max_turns"*)
        if [ "$N_MT" = 3 ]; then pass "TL-08" "max_turns refuses the final turn: no 4th provider call"
        else fail "TL-08" "final turn refused but a call was made anyway" "requests=$N_MT"; fi ;;
    *) fail "TL-08" "max_turns did not refuse the final turn" "$(printf '%s' "$O_MT" | grep -E 'FINAL_|EXIT_|Error' | head -3)" ;;
esac

# Control: the model answers after one tool call -> a genuine text response,
# no final turn needed.
FINISH='{"responses":[{"tool_calls":[{"name":"g","args":{"q":"x"}}]},{"content":"def f(): return 1","output_tokens":12}]}'
O_FIN="$(run_tool_case "$FINISH" "" "")"
SUP_FIN="$(events RESPONSE_SUPPRESSED)"
N_FIN="$(nreq)"
case "$O_FIN" in
    STUBFAIL*) skip "TL-01c" "stub did not start - UNMEASURABLE" ;;
    *"EXIT_REASON=text_response"*"CONTENT=def f(): return 1|END"*)
        if [ -z "$SUP_FIN" ] && [ "$N_FIN" = 2 ]; then pass "TL-01c" "CONTROL: a loop that ends in text stays text_response, no extra call, nothing suppressed"
        else fail "TL-01c" "CONTROL: a completed loop made an extra call or was suppressed" "requests=$N_FIN ${SUP_FIN}"; fi ;;
    *) fail "TL-01c" "CONTROL: a completed loop is no longer labelled text_response" "$(printf '%s' "$O_FIN" | grep -E 'EXIT_|CONTENT|Error' | head -3)" ;;
esac

# Control: an empty reply with no tool call keeps the plain reason.
EMPTY='{"responses":[{"content":"","output_tokens":0}]}'
O_EMP="$(run_tool_case "$EMPTY" "" "")"
SUP_EMP="$(events RESPONSE_SUPPRESSED)"
case "$O_EMP" in
    STUBFAIL*) skip "TL-02c" "stub did not start - UNMEASURABLE" ;;
    *)
        case "$SUP_EMP" in
            *'"reason": "empty response"'*) pass "TL-02c" "CONTROL: a model that said nothing is still reported as an empty response" ;;
            *) fail "TL-02c" "CONTROL: plain empty reply misattributed" "${SUP_EMP:-no RESPONSE_SUPPRESSED event}" ;;
        esac ;;
esac

# ------------------------------------------------------------------
# CT: an output contract is not met by saying nothing
# ------------------------------------------------------------------
# The contract block ran only for non-empty content (llm-bridge-findings open
# item 3), and it threw before the accounting commit, so a violating reply's
# turn and tokens were never counted. Project owner's decision: an empty reply
# is a violation; the throw now follows the accounting commit.
echo -e "${CYAN}--- CT: output contracts and empty replies ---${NC}"
# $1 fixture, $2 output_contract json. Prints what the script saw.
run_contract_case() {
    rm -f tel.jsonl
    printf '%s\n' "$1" > fixture.json
    start_stub fixture.json . >/dev/null 2>&1 || { echo "STUBFAIL"; return; }
    cat > govern.json <<GEOF
{ "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tel.jsonl" },
  "agents": { "w": { "provider": "gemini", "model": "stub",
    "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_ROUT",
    "max_tokens": 50, "max_turns": 20, "output_contract": $2 } } }
GEOF
    sign
    cat > ct.naab <<'NEOF'
use agent
main {
    let h = agent.create("w")
    try {
        let r = agent.send(h, "answer")
        print("DELIVERED=" + string(r.get("content")) + "|END")
    } catch (e) {
        print("CAUGHT=" + string(e))
    }
    let u = agent.usage(h)
    print("USAGE=" + string(u.get("turns")) + "/" + string(u.get("output_tokens")))
}
NEOF
    timeout 90 "$NAAB" ct.naab 2>&1
    stop_stub 2>/dev/null
}
JSON_CONTRACT='{"format": "json", "required_fields": ["a"]}'
O="$(run_contract_case '{"responses":[{"content":"","output_tokens":9}]}' "$JSON_CONTRACT")"
V="$(events CONTRACT_VIOLATION)"
case "$O" in
    STUBFAIL*) skip "CT-01" "stub did not start - UNMEASURABLE" ;;
    *"CAUGHT="*"output contract violation"*"empty response"*)
        case "$V" in
            *'"violation": "empty response"'*) pass "CT-01" "an empty reply violates a declared contract (error + CONTRACT_VIOLATION)" ;;
            *) fail "CT-01" "refused, but no CONTRACT_VIOLATION event" "$V" ;;
        esac ;;
    *) fail "CT-01" "an empty reply was handed to the script under a contract" "$(printf '%s' "$O" | grep -E 'DELIVERED|CAUGHT' | head -2)" ;;
esac
O="$(run_contract_case '{"responses":[{"content":"{}","output_tokens":9}]}' "$JSON_CONTRACT")"
case "$O" in
    STUBFAIL*) skip "CT-02" "stub did not start - UNMEASURABLE" ;;
    *"CAUGHT="*"missing required field"*"USAGE=1/9"*) pass "CT-02" "a violating reply's call is still counted (turns 1, 9 tokens)" ;;
    *) fail "CT-02" "a contract violation skipped the accounting commit" "$(printf '%s' "$O" | grep -E 'CAUGHT|USAGE' | head -2)" ;;
esac
O="$(run_contract_case '{"responses":[{"content":"{\"a\": 1}","output_tokens":9}]}' "$JSON_CONTRACT")"
case "$O" in
    STUBFAIL*) skip "CT-01c" "stub did not start - UNMEASURABLE" ;;
    *'DELIVERED={"a": 1}|END'*"USAGE=1/9"*) pass "CT-01c" "CONTROL: a reply that meets the contract is delivered and counted once" ;;
    *) fail "CT-01c" "CONTROL: a valid reply was refused or miscounted" "$(printf '%s' "$O" | grep -E 'DELIVERED|CAUGHT|USAGE' | head -2)" ;;
esac
O="$(run_contract_case '{"responses":[{"content":"def f(): pass","output_tokens":9}]}' '{"format": "text", "regex_checks": {"has_def": "def "}}')"
case "$O" in
    STUBFAIL*) skip "CT-03" "stub did not start - UNMEASURABLE" ;;
    *'output_contract.format "text" is not validated'*'DELIVERED=def f(): pass|END'*)
        pass "CT-03" "a \"text\" contract is reported as unvalidated at load (it checks nothing but emptiness)" ;;
    *) fail "CT-03" "a \"text\" contract loads silently" "$(printf '%s' "$O" | grep -iE 'warning|DELIVERED|CAUGHT' | head -3)" ;;
esac
# The empty reply's usual cause, named in the violation.
O="$(run_tool_case "$EXHAUST" ', "output_contract": {"format": "json", "required_fields": ["a"]}' '"agent_dispatch": { "hard_stop": { "max_calls_per_run": 3 } },')"
case "$O" in
    STUBFAIL*) skip "CT-04" "stub did not start - UNMEASURABLE" ;;
    *"output contract violation"*"tool loop ended without a final text turn: max_tool_loop_turns"*)
        pass "CT-04" "the violation names the exhausted tool loop as the cause of the empty reply" ;;
    *) fail "CT-04" "the violation does not say why the reply was empty" "$(printf '%s' "$O" | grep -iE 'violation|Error' | head -3)" ;;
esac

# A final turn that was granted and still said nothing is named as that.
O="$(run_tool_case "$EXHAUST" ', "output_contract": {"format": "json", "required_fields": ["a"]}' "")"
case "$O" in
    STUBFAIL*) skip "CT-05" "stub did not start - UNMEASURABLE" ;;
    *"output contract violation"*"the final tool-less turn after max_tool_loop_turns returned no text"*)
        pass "CT-05" "a granted final turn that returned nothing is reported as such, not as a missing turn" ;;
    *) fail "CT-05" "granted-but-empty final turn misreported" "$(printf '%s' "$O" | grep -iE 'violation|Error' | head -3)" ;;
esac

# ------------------------------------------------------------------
# PS: pipeline separation goes through enforce()
# ------------------------------------------------------------------
echo -e "${CYAN}--- PS: a HARD pipeline-separation block cannot be caught ---${NC}"
# $1 = pipeline_separation JSON (or empty), $2 = "same"|"distinct" configs.
run_pipe_case() {
    printf '%s\n' '{"responses":[{"content":"ok","output_tokens":5}]}' > fixture.json
    start_stub fixture.json . >/dev/null 2>&1 || { echo "STUBFAIL"; return; }
    local ps=""; [ -n "$1" ] && ps="\"pipeline_separation\": $1,"
    cat > govern.json <<GEOF
{ "mode": "enforce", "security": { "sandbox_level": "elevated" }, $ps
  "agents": {
    "a": { "provider": "gemini", "model": "stub", "api_base": "http://127.0.0.1:$STUB_PORT",
           "api_key_env": "FAKE_KEY_ROUT", "max_tokens": 50 },
    "b": { "provider": "gemini", "model": "stub", "api_base": "http://127.0.0.1:$STUB_PORT",
           "api_key_env": "FAKE_KEY_ROUT", "max_tokens": 50 } } }
GEOF
    sign
    local second="a"; [ "$2" = distinct ] && second="b"
    cat > p.naab <<EOF
use agent
main {
    let h1 = agent.create("a")
    let h2 = agent.create("$second")
    try {
        let r = agent.pipeline([h1, h2], "hi")
        print("PIPE_RAN")
    } catch (e) {
        print("PIPE_CAUGHT")
    }
    print("AFTER")
}
EOF
    timeout 90 "$NAAB" p.naab 2>&1; echo "RC=$?"
    stop_stub 2>/dev/null
}
O="$(run_pipe_case '{"enabled": true, "level": "hard"}' same)"
case "$O" in
    STUBFAIL*) skip "PS-01" "stub did not start - UNMEASURABLE" ;;
    *"PIPE_CAUGHT"*|*"AFTER"*) fail "PS-01" "a HARD separation block was caught and the script ran on" "$(printf '%s' "$O" | grep -E 'PIPE_|AFTER|RC=')" ;;
    *"RC=3"*) pass "PS-01" "HARD: uncatchable, exit 3, nothing after the pipeline ran" ;;
    *) fail "PS-01" "HARD: unexpected outcome" "$(printf '%s' "$O" | tail -3)" ;;
esac
O="$(run_pipe_case '{"enabled": true, "level": "detect"}' same)"
case "$O" in
    STUBFAIL*) skip "PS-02" "stub did not start - UNMEASURABLE" ;;
    *"PIPE_CAUGHT"*"AFTER"*"RC=0"*) pass "PS-02" "DETECT: blocks, and a script may catch it (the documented DETECT tier)" ;;
    *) fail "PS-02" "DETECT did not block catchably" "$(printf '%s' "$O" | grep -E 'PIPE_|AFTER|RC=')" ;;
esac
O="$(run_pipe_case '{"enabled": true, "level": "advisory"}' same)"
case "$O" in
    STUBFAIL*) skip "PS-03" "stub did not start - UNMEASURABLE" ;;
    *"PIPE_RAN"*"AFTER"*"RC=0"*)
        case "$O" in
            *"Pipeline separation violation"*) pass "PS-03" "ADVISORY: reported, pipeline runs" ;;
            *) fail "PS-03" "ADVISORY: the violation was not reported" ;;
        esac ;;
    *) fail "PS-03" "ADVISORY blocked" "$(printf '%s' "$O" | grep -E 'PIPE_|AFTER|RC=')" ;;
esac
O="$(run_pipe_case '{"enabled": true, "level": "hard"}' distinct)"
case "$O" in
    STUBFAIL*) skip "PS-01c" "stub did not start - UNMEASURABLE" ;;
    *"PIPE_RAN"*"AFTER"*"RC=0"*) pass "PS-01c" "CONTROL: distinct configs at HARD are not blocked" ;;
    *) fail "PS-01c" "CONTROL: a compliant pipeline was blocked" "$(printf '%s' "$O" | tail -3)" ;;
esac
O="$(run_pipe_case '' same)"
case "$O" in
    STUBFAIL*) skip "PS-02c" "stub did not start - UNMEASURABLE" ;;
    *"PIPE_RAN"*"AFTER"*"RC=0"*) pass "PS-02c" "CONTROL: without the section, same configs run" ;;
    *) fail "PS-02c" "CONTROL: blocked with no pipeline_separation configured" "$(printf '%s' "$O" | tail -3)" ;;
esac

# ------------------------------------------------------------------
# LV: an unknown level's warning states what actually happens
# ------------------------------------------------------------------
echo -e "${CYAN}--- LV: the unknown-level warning tells the truth ---${NC}"
# Level names are case-insensitive now ("HARD" is "hard", LC below), so these
# arms use a value that is not a level at all.
# Level-only site: the check is ENFORCED at hard. The behaviour half is PS-01's
# harness: if the warning says hard, it must be.
O="$(run_pipe_case '{"enabled": true, "level": "strict"}' same)"
case "$O" in
    STUBFAIL*) skip "LV-01" "stub did not start - UNMEASURABLE" ;;
    *)
        W="$(printf '%s' "$O" | grep 'unknown enforcement level "strict"' | head -1)"
        case "$O" in *"RC=3"*) enforced=true ;; *) enforced=false ;; esac
        case "$W" in
            *DISABLED*) fail "LV-01" "warning says DISABLED for a level-only key" "$W" ;;
            *"enforced at hard"*)
                if $enforced; then pass "LV-01" "level-only key: warning says hard, run is blocked at hard"
                else fail "LV-01" "warning says hard but the check did not block" "$(printf '%s' "$O" | tail -2)"; fi ;;
            *) fail "LV-01" "no truthful warning for \"strict\"" "${W:-none}" ;;
        esac ;;
esac
# Enabling site: the shorthand value is also the on/off switch, so the check
# really is DISABLED — and the program it should have blocked runs. Run on the
# tree-walker: the VM's main-block check was itself inert before this change
# (MB below), and this pair is about the WARNING, so it must not depend on it.
mkdir -p lv && cd lv
printf 'fn helper() { return 1 }\n' > nomain.naab
lv_run() { printf '{"mode":"enforce","requirements":{"main_block":%s}}\n' "$1" > govern.json; sign
           timeout 30 "$NAAB" --tree-walk nomain.naab 2>&1; echo "RC=$?"; }
O_UP="$(lv_run '"strict"')"
O_CASE="$(lv_run '"HARD"')"
O_LO="$(lv_run '"hard"')"
# The OBJECT form is read by two parsers; the later one turns the check on at
# hard. The warning has to agree with the one that wins, and say it once.
O_OBJ="$(lv_run '{"level": "strict"}')"
cd ..
case "$O_LO" in
    *"main"*"block"*) LO_BLOCKS=true ;;
    *) LO_BLOCKS=false ;;
esac
if ! $LO_BLOCKS; then
    fail "LV-02c" "CONTROL: requirements.main_block \"hard\" did not block a program without main" "$(printf '%s' "$O_LO" | tail -2)"
else
    pass "LV-02c" "CONTROL: \"hard\" blocks a program without a main block"
    case "$O_UP" in
        *"requires a main"*) fail "LV-02" "\"strict\" was enforced at a site the warning calls disabled" "$(printf '%s' "$O_UP" | tail -2)" ;;
        *'unknown enforcement level "strict"'*DISABLED*) pass "LV-02" "enabling site: warning says DISABLED and the check did not run" ;;
        *) fail "LV-02" "no DISABLED warning where the check is disabled" "$(printf '%s' "$O_UP" | head -2)" ;;
    esac
fi
N_WARN="$(printf '%s\n' "$O_OBJ" | grep -c 'unknown enforcement level "strict"')"
case "$O_OBJ" in
    *DISABLED*) fail "LV-04" "object form: a DISABLED warning for a check that is enforced" "$(printf '%s' "$O_OBJ" | grep 'unknown enforcement' | head -2)" ;;
    *"enforced at hard"*"requires a main"*)
        if [ "$N_WARN" = 1 ]; then pass "LV-04" "object form read by two parsers: one warning, it says hard, and the check blocks"
        else fail "LV-04" "object form: $N_WARN warnings for one value"; fi ;;
    *) fail "LV-04" "object form: warning and outcome do not agree" "$(printf '%s' "$O_OBJ" | grep -E 'unknown enforcement|requires a main|RC=' | head -3)" ;;
esac
# Keep-default site (`if (en) level = lv`): neither disabled nor hard.
printf '{"mode":"enforce","contracts":{"level":"strict"}}\n' > govern.json; sign
printf 'main { print("RAN") }\n' > c.naab
O="$(timeout 30 "$NAAB" c.naab 2>&1)"
case "$O" in
    *'unknown enforcement level "strict"'*"default level applies"*) pass "LV-03" "keep-default site: warning says the default level applies" ;;
    *) fail "LV-03" "keep-default site misreported" "$(printf '%s' "$O" | grep -i 'level' | head -2)" ;;
esac
printf '{"mode":"enforce","pipeline_separation":{"enabled":true,"level":"hard"}}\n' > govern.json; sign
O="$(timeout 30 "$NAAB" c.naab 2>&1)"
case "$O" in
    *"unknown enforcement level"*) fail "LV-03c" "CONTROL: a valid level warned" "$O" ;;
    *RAN*) pass "LV-03c" "CONTROL: a valid lowercase level prints no warning" ;;
    *) fail "LV-03c" "CONTROL: program did not run" "$O" ;;
esac

# ------------------------------------------------------------------
# LC: level names are case-insensitive
# ------------------------------------------------------------------
# The project owner's decision: "HARD" is "hard". Case-sensitive matching was a
# fail-OPEN at the keys whose value also switches a check on.
echo -e "${CYAN}--- LC: level names are case-insensitive ---${NC}"
case "$O_CASE" in
    *"unknown enforcement level"*) fail "LC-01" "\"HARD\" is still an unknown level" "$(printf '%s' "$O_CASE" | grep 'unknown enforcement' | head -1)" ;;
    *"requires a main"*) pass "LC-01" "enabling site: \"HARD\" switches the check on and it blocks (it used to disable it)" ;;
    *) fail "LC-01" "\"HARD\" did not enforce the main-block requirement" "$(printf '%s' "$O_CASE" | tail -2)" ;;
esac
O="$(run_pipe_case '{"enabled": true, "level": "Soft"}' same)"
case "$O" in
    STUBFAIL*) skip "LC-02" "stub did not start - UNMEASURABLE" ;;
    *"unknown enforcement level"*) fail "LC-02" "\"Soft\" is still an unknown level" ;;
    *"SOFT-MANDATORY"*"RC=3"*) pass "LC-02" "level-only site: \"Soft\" is soft, not the strictest level by accident" ;;
    *) fail "LC-02" "\"Soft\" was not enforced as soft" "$(printf '%s' "$O" | grep -E 'MANDATORY|PIPE_|RC=' | head -3)" ;;
esac

# ------------------------------------------------------------------
# MB: requirements.main_block, the other hand-rolled level check
# ------------------------------------------------------------------
# Found building LV-02c: the VM tested "main chunk <= 1 instruction", which an
# empty main never is, so the requirement did nothing on the default engine;
# the tree-walker blocked with a plain runtime_error (exit 1, not 3).
echo -e "${CYAN}--- MB: requirements.main_block blocks on both engines, at exit 3 ---${NC}"
mkdir -p mb && cd mb
printf 'fn helper() { return 1 }\n' > nomain.naab
printf 'main { print("RAN") }\n' > withmain.naab
mb_run() { # $1 requirements json or "", $2 engine flag or "", $3 program
    if [ -n "$1" ]; then printf '{"mode":"enforce","requirements":%s}\n' "$1" > govern.json
    else printf '{"mode":"enforce"}\n' > govern.json; fi
    sign; timeout 30 "$NAAB" $2 "$3" 2>&1; echo "RC=$?"; }
O_VM="$(mb_run '{"main_block":"hard"}' "" nomain.naab)"
O_TW="$(mb_run '{"main_block":"hard"}' --tree-walk nomain.naab)"
O_OK="$(mb_run '{"main_block":"hard"}' "" withmain.naab)"
O_OFF="$(mb_run '' "" nomain.naab)"
cd ..
case "$O_VM" in
    *"requires a main"*"RC=3"*) pass "MB-01" "VM: a program with no main block is blocked at exit 3" ;;
    *) fail "MB-01" "VM: requirement did not block" "$(printf '%s' "$O_VM" | tail -2)" ;;
esac
case "$O_TW" in
    *"{{"*) fail "MB-02" "tree-walker: message prints a literal {{ }}" ;;
    *"requires a main"*"RC=3"*) pass "MB-02" "tree-walker: blocked at exit 3 (was 1)" ;;
    *) fail "MB-02" "tree-walker: wrong outcome" "$(printf '%s' "$O_TW" | tail -2)" ;;
esac
case "$O_OK" in
    *RAN*"RC=0"*) pass "MB-01c" "CONTROL: a program WITH a main block runs under the requirement" ;;
    *) fail "MB-01c" "CONTROL: a compliant program was blocked" "$(printf '%s' "$O_OK" | tail -2)" ;;
esac
# "Governance: PASS" is the positive half: an interpreter that did nothing
# would also print no block message and exit 0.
case "$O_OFF" in
    *"requires a main"*) fail "MB-02c" "CONTROL: blocked with no requirement configured" ;;
    *"Governance: PASS"*"RC=0"*) pass "MB-02c" "CONTROL: without the requirement the same program is not blocked" ;;
    *) fail "MB-02c" "CONTROL: unexpected outcome" "$(printf '%s' "$O_OFF" | tail -2)" ;;
esac

# ------------------------------------------------------------------
# OA: every evaluation says what happened to the response
# ------------------------------------------------------------------
echo -e "${CYAN}--- OA: disposition matches what the script received ---${NC}"
VARIED='{"responses":[
 {"content":"The calculator module exposes an add method that returns the sum of two operands.","output_tokens":40},
 {"content":"Subtraction is implemented by negating the second operand before delegating to add.","output_tokens":42},
 {"content":"Multiplication uses repeated addition only for integers; floats take the native operator.","output_tokens":44},
 {"content":"Division guards against a zero denominator and raises a descriptive error instead.","output_tokens":41},
 {"content":"Each arithmetic operation appends a formatted entry to the calculator history log.","output_tokens":43},
 {"content":"The history log is capped at one hundred entries and discards the oldest first.","output_tokens":42}]}'
IDENTICAL='{"responses":[{"content":"done","output_tokens":20}]}'
# $1 fixture, $2 adaptive, $3 action, $4 extra OA keys, $5 threshold (default
# 0.70). Prints the script's
# per-turn admissible flags; telemetry is left in tel.jsonl.
run_oa_case() {
    rm -f tel.jsonl
    printf '%s\n' "$1" > fixture.json
    start_stub fixture.json . >/dev/null 2>&1 || { echo "STUBFAIL"; return; }
    cat > govern.json <<GEOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tel.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": $2, "adaptive_baseline_window": 5 },
  "circuit_breaker": { "enabled": true, "output_admissibility": {
    "enabled": true, "threshold": ${5:-0.70}, "action": "$3", "max_quarantine_streak": 0$4 } },
  "agents": { "worker": { "provider": "gemini", "model": "stub",
    "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_ROUT",
    "max_tokens": 200, "max_turns": 30 } } }
GEOF
    sign
    cat > o.naab <<'EOF'
use agent
main {
    let h = agent.create("worker")
    let i = 0
    let seq = ""
    while i < 6 {
        let r = agent.send(h, "describe the next part of the calculator module")
        let a = r.get("admissibility")
        if a == null { seq = seq + "?" }
        else if a.get("admissible") == false { seq = seq + "H" }
        else { seq = seq + "A" }
        i = i + 1
    }
    print("SEQ=" + seq)
}
EOF
    timeout 120 "$NAAB" o.naab 2>&1
    stop_stub 2>/dev/null
}
# Telemetry's dispositions as one letter per evaluation: A admitted, H held
# (quarantined/attested), plus the set of `action` values and of dispositions.
oa_summary() {
    [ -f tel.jsonl ] || { echo "NOTEL"; return; }
    python3 -c '
import json
seq, actions, disp = "", set(), set()
for line in open("tel.jsonl", encoding="utf-8", errors="replace"):
    try: d = json.loads(line)
    except Exception: continue
    if d.get("event_type") != "OUTPUT_ADMISSIBILITY_EVAL": continue
    x = d.get("disposition", "MISSING")
    disp.add(x); actions.add(d.get("action", "MISSING"))
    seq += "A" if x == "admitted" else ("H" if x in ("quarantined", "attested") else "?")
print("TSEQ=%s ACTIONS=%s DISP=%s" % (seq, ",".join(sorted(actions)), ",".join(sorted(disp))))
'
}
oa_check() { # $1 id $2 label $3 script-output $4 summary $5 expected disposition set
    local sseq tseq
    sseq="$(printf '%s' "$3" | grep -oE 'SEQ=[AH?]+' | head -1 | cut -d= -f2)"
    tseq="$(printf '%s' "$4" | grep -oE 'TSEQ=[AH?]*' | cut -d= -f2)"
    case "$3" in STUBFAIL*) skip "$1" "stub did not start - UNMEASURABLE"; return ;; esac
    if [ -z "$sseq" ]; then fail "$1" "$2: script did not finish" "$(printf '%s' "$3" | tail -2)"; return; fi
    case "$4" in
        *"DISP=$5 "*|*"DISP=$5") ;;
        *) fail "$1" "$2: dispositions wrong" "$4"; return ;;
    esac
    if [ "$sseq" = "$tseq" ]; then pass "$1" "$2: disposition per turn = what the script received ($sseq)"
    else fail "$1" "$2: telemetry disagrees with the script" "script=$sseq telemetry=$tseq"; fi
}
# Threshold 0: with baselining off every signal charges from turn 1, so varied
# phrasing alone can fail 0.70; a floor of 0 makes every turn a determined pass.
O="$(run_oa_case "$VARIED" false quarantine "" 0.0)"; S="$(oa_summary)"
oa_check "OA-01" "determined passes" "$O" "$S" "admitted"
case "$S" in *"ACTIONS=quarantine"*) pass "OA-01b" "the configured action is still reported (action=quarantine on a pass)" ;;
    *) fail "OA-01b" "action field changed" "$S" ;; esac
# Vacuity guards read what the SCRIPT received, never telemetry: on a build
# without the field, "no held turn in telemetry" would blame the fixture for
# the defect under test.
O="$(run_oa_case "$IDENTICAL" false quarantine "")"; S="$(oa_summary)"
case "$O" in *SEQ=*H*) oa_check "OA-02" "quarantine fails" "$O" "$S" "admitted,quarantined" ;;
    *) fail "OA-02" "the script was never handed a held response - fixture broken" "$(printf '%s' "$O" | grep SEQ=)" ;; esac
O="$(run_oa_case "$IDENTICAL" false attest "")"; S="$(oa_summary)"
oa_check "OA-03" "attest fails" "$O" "$S" "admitted,attested"
O="$(run_oa_case "$VARIED" true quarantine ', "on_undetermined": "quarantine"')"; S="$(oa_summary)"
case "$O" in *SEQ=*H*) oa_check "OA-04" "undetermined held" "$O" "$S" "admitted,quarantined" ;;
    *) fail "OA-04" "the script was never handed a held response - fixture broken" "$(printf '%s' "$O" | grep SEQ=)" ;; esac
O="$(run_oa_case "$VARIED" true quarantine ', "on_undetermined": "pass"')"; S="$(oa_summary)"
oa_check "OA-05" "undetermined with on_undetermined=pass" "$O" "$S" "admitted"
# The control the OA arms rest on: the script-side sequences differ between the
# fixtures (all admitted vs some held), so agreement with telemetry is a
# comparison that can fail. Checked on whichever build runs this.
case "$O" in
    *SEQ=AAAAAA*) pass "OA-05c" "CONTROL: an undetermined pass is delivered (the script saw AAAAAA)" ;;
    *) fail "OA-05c" "CONTROL: on_undetermined=pass held something" "$(printf '%s' "$O" | grep SEQ=)" ;;
esac

# ------------------------------------------------------------------
# BS: behavioral_sequences patterns that can never fire are reported
# ------------------------------------------------------------------
echo -e "${CYAN}--- BS: unmatchable sequence patterns are reported at load ---${NC}"
mkdir -p bs && cd bs
cat > probe.naab <<'EOF'
use file
use process
main { let p = file.read("/etc/passwd") let r = process.run("echo hi") print("DONE") }
EOF
bs_run() { printf '{"mode":"enforce","security":{"sandbox_level":"elevated"},"behavioral_sequences":%s}\n' "$1" > govern.json; sign
           timeout 60 "$NAAB" probe.naab 2>&1; }
O_NONE="$(bs_run '{"enabled":true}')"
O_STEPS="$(bs_run '{"enabled":true,"patterns":[{"name":"relay","steps":["FILE_WRITE","FILE_READ"],"gap":5}]}')"
O_UPPER="$(bs_run '{"enabled":true,"patterns":[{"name":"chain","sequence":["TOOL_CALL","FILE_READ"],"max_gap":5,"level":"advisory"}]}')"
O_GOOD="$(bs_run '{"enabled":true,"patterns":[{"name":"ok","sequence":["file.read","FILE_WRITE"],"max_gap":5,"level":"advisory"}]}')"
cd ..
case "$O_NONE" in
    *sandbox_probe_escape*) pass "BS-00" "POSITIVE CONTROL: with no patterns listed, a built-in fires on this program" ; BS_OK=true ;;
    *) fail "BS-00" "POSITIVE CONTROL: built-in did not fire - BS-01b unreadable" "$(printf '%s' "$O_NONE" | tail -2)"; BS_OK=false ;;
esac
case "$O_STEPS" in
    *'pattern "relay" has no steps'*'built-ins are not active'*) pass "BS-01" "a step-less pattern and the built-ins it displaced are both reported" ;;
    *) fail "BS-01" "step-less pattern loaded silently" "$(printf '%s' "$O_STEPS" | grep -i warning | head -3)" ;;
esac
if $BS_OK; then
    case "$O_STEPS" in
        *sandbox_probe_escape*) fail "BS-01b" "the warning says built-ins are inactive but one fired" ;;
        *) pass "BS-01b" "and it is true: the same program trips no built-in" ;;
    esac
fi
case "$O_UPPER" in
    *'step "TOOL_CALL" matches no event type'*'Write "tool_call"'*) pass "BS-02" "a dead UPPERCASE step name is reported with its working spelling" ;;
    *) fail "BS-02" "dead UPPERCASE step loaded silently" "$(printf '%s' "$O_UPPER" | grep -i warning | head -3)" ;;
esac
case "$O_UPPER" in
    *'step "FILE_READ" matches no event type'*) fail "BS-02c" "CONTROL: FILE_READ (one of the 7 that work) was reported dead" ;;
    *DONE*) pass "BS-02c" "CONTROL: an UPPERCASE name that does match is not reported" ;;
    *) fail "BS-02c" "CONTROL: the program did not run, so the absence means nothing" "$(printf '%s' "$O_UPPER" | tail -2)" ;;
esac
case "$O_GOOD" in
    *"matches no event type"*|*"has no steps"*|*"built-ins are not active"*) fail "BS-03c" "CONTROL: a valid pattern list warned" "$(printf '%s' "$O_GOOD" | grep -i warning | head -2)" ;;
    *DONE*) pass "BS-03c" "CONTROL: a valid pattern list loads without these warnings" ;;
    *) fail "BS-03c" "CONTROL: program did not run" "$(printf '%s' "$O_GOOD" | tail -2)" ;;
esac

echo ""
echo "reported outcomes: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
