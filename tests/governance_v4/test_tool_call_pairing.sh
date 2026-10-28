#!/usr/bin/env bash
# test_tool_call_pairing.sh -- a refused tool call is answered beside its call (A44(b))
#
# The tool loop answers a call it refuses (not on the allowlist, an invalid
# name, unparseable arguments, the admissibility gate) with an error result.
# Until A44(b) it pushed only the RESULT: the call itself never reached the
# assistant turn, so the next request carried a result naming no call -- a
# request both providers reject (Anthropic: a tool_result with no matching
# tool_use; Gemini: more function responses than function calls). The stub
# accepted anything, so no suite saw it. agent_stub.py now validates the pairing
# as Gemini does (400 + pairing.log), and every arm below asserts both that the
# follow-up request was accepted (the second response's text comes back) and
# that the stub logged no pairing violation.
#
#   TP-00   CONTROL: the stub refuses an orphan function response sent to it
#           directly (400, pairing.log line) -- the check can fail
#   TP-00c  CONTROL: an allowed call runs (its side effect lands) and the
#           follow-up request is accepted -- the scenario works when nothing
#           is refused
#   TP-01   a call to a tool not on the allowlist is refused, and the follow-up
#           request is accepted
#   TP-02   a call with an invalid tool name is refused, and the follow-up is
#           accepted
#   TP-03   one response with an allowed call AND a refused one: the allowed
#           call runs, and the follow-up is accepted with both answered
#   (Unparseable arguments are not reachable through either provider: both hand
#   the engine arguments re-serialized from parsed JSON. That branch shares the
#   helper and is traced only. The admissibility-gate refusal is
#   test_tool_admissibility_gate.sh B-04.)
#
# Kill table (each mutant rebuilt, this suite and test_tool_admissibility_gate.sh
# re-run; observed 2026-10-28):
#   refuseCall pushes no tool_use (the pre-fix shape)   -> TP-01, TP-02, TP-03, B-04 red
#   refuseCall's result carries no "name"               -> TP-01, TP-02, TP-03, B-04 red
#   agent_stub.py validate_tool_pairing default False   -> TP-00 red
# TS-01 has no mutant: A44(a) only deleted unreachable branches, so it pins the
# behaviour they never changed. Red first: on master 6055ac3c (before A44(b))
# TP-01/02/03 and B-04 fail and every control passes; the stub logs, e.g.,
# "responses ['gemini_0'] answer calls []" -- the orphan, and its name fallen
# back to the call id.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_tcpair_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PORT=""
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup() { stop_stub 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT

"$NAAB" --keygen "$TEST_TMP/key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/key.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$TEST_TMP/key.pem"
export FAKE_KEY_TCPAIR="fake-key-tcpair"

# run_case ID FIXTURE_RESPONSES [EXTRA_TOP_LEVEL_KEYS] [TOOL_RETURN_EXPR]
#   -> runs the program against a fresh stub; sets OUT, RC, WDIR
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    printf '{"responses": %s}\n' "$2" > "$WDIR/fixture.json"
    if ! start_stub "$WDIR/fixture.json" "$WDIR"; then OUT="STUB-FAILED"; return; fi
    cat > "$WDIR/govern.json" <<GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },${3:-}
    "agents": {
        "tooler": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_TCPAIR",
            "max_tokens": 100,
            "max_turns": 20,
            "retry": { "max_attempts": 1 },
            "tools_enabled": true,
            "tools": ["get_data"]
        }
    }
}
GOVEOF
    (cd "$WDIR" && "$NAAB" --sign-governance >/dev/null 2>&1) || true
    local ret="${4:-\"data for: \" + string(query)}"
    cat > "$WDIR/test.naab" <<NAABEOF
use agent
use file

fn get_data(query) {
    file.write("side_effect.txt", "ran")
    return $ret
}

main {
    agent.register_tool("get_data", get_data, {
        "description": "Fetch data by query",
        "parameters": { "query": {"type": "string", "description": "what to fetch"} }
    })
    let h = agent.create("tooler")
    let r = agent.send(h, "fetch the data")
    print("CONTENT=" + string(r.get("content")))
}
NAABEOF
    RC=0; OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || RC=$?
    stop_stub
}
# accepted: the follow-up came back and the stub saw no pairing violation
accepted() { grep -q "CONTENT=FINAL-$1" <<<"$OUT" && [ ! -s "$WDIR/pairing.log" ]; }
why() { echo "$(grep -E 'CONTENT=|rror' <<<"$OUT" | head -3 | tr '\n' ' ') pairing.log: $(head -2 "$WDIR/pairing.log" 2>/dev/null | tr '\n' ' ')"; }

echo "=== Tool-call pairing (A44(b)) ==="

# TP-00: the validator itself
WDIR="$TEST_TMP/tp00"; mkdir -p "$WDIR"
echo '{"responses": [{"content": "unused"}]}' > "$WDIR/fixture.json"
if start_stub "$WDIR/fixture.json" "$WDIR"; then
    CODE=$(python3 -c 'import sys, json, urllib.request, urllib.error
body = {"contents": [{"role": "user", "parts": [{"text": "q"}]},
                     {"role": "model", "parts": [{"functionCall": {"name": "get_data", "args": {}}}]},
                     {"role": "user", "parts": [{"functionResponse": {"name": "get_data", "response": {}}},
                                                {"functionResponse": {"name": "other", "response": {}}}]}]}
r = urllib.request.Request("http://127.0.0.1:%s/v1beta/models/m:generateContent" % sys.argv[1],
                           data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
try:
    urllib.request.build_opener(urllib.request.ProxyHandler({})).open(r, timeout=5); print(200)
except urllib.error.HTTPError as e:
    print(e.code)' "$STUB_PORT" 2>/dev/null)
    stop_stub
    if [ "$CODE" = "400" ] && [ -s "$WDIR/pairing.log" ]; then
        pass TP-00 "CONTROL: the stub refuses an orphan function response (400, logged)"
    else
        fail TP-00 "the stub should refuse an orphan function response" "code=$CODE log=$(cat "$WDIR/pairing.log" 2>/dev/null)"
    fi
else
    unm TP-00 "the stub did not start"
fi

run_case tp00c '[{"tool_calls": [{"name": "get_data", "args": {"query": "q"}}]},
                 {"content": "FINAL-00c"}]'
if [ "$OUT" = "STUB-FAILED" ]; then unm TP-00c "the stub did not start"
elif accepted 00c && [ -f "$WDIR/side_effect.txt" ]; then
    pass TP-00c "CONTROL: an allowed call runs and the follow-up request is accepted"
else
    fail TP-00c "an allowed call should run and its follow-up be accepted" "$(why)"
fi

run_case tp01 '[{"tool_calls": [{"name": "not_listed", "args": {"x": 1}}]},
                {"content": "FINAL-01"}]'
if [ "$OUT" = "STUB-FAILED" ]; then unm TP-01 "the stub did not start"
elif accepted 01; then
    pass TP-01 "a call to a tool not on the allowlist is refused and the follow-up request is accepted"
else
    fail TP-01 "the follow-up after an allowlist refusal should be accepted" "$(why)"
fi

run_case tp02 '[{"tool_calls": [{"name": "get-data", "args": {"query": "q"}}]},
                {"content": "FINAL-02"}]'
if [ "$OUT" = "STUB-FAILED" ]; then unm TP-02 "the stub did not start"
elif accepted 02; then
    pass TP-02 "a call with an invalid tool name is refused and the follow-up request is accepted"
else
    fail TP-02 "the follow-up after an invalid-name refusal should be accepted" "$(why)"
fi

run_case tp03 '[{"tool_calls": [{"name": "get_data", "args": {"query": "q"}},
                                {"name": "not_listed", "args": {}}]},
                {"content": "FINAL-03"}]'
if [ "$OUT" = "STUB-FAILED" ]; then unm TP-03 "the stub did not start"
elif accepted 03 && [ -f "$WDIR/side_effect.txt" ]; then
    pass TP-03 "an allowed and a refused call in one response: the allowed one runs, both are answered"
else
    fail TP-03 "a mixed response should run the allowed call and answer both" "$(why)"
fi

# --- A44(a): a secret in a tool result ends the run --------------------------
# The redact-and-continue branches for tool arguments and results were dead
# since fd97d0ae made HARD uncatchable, and are gone; these arms pin what the
# scan really does. The value is built from fragments at run time (no literal
# secret in this file or the program), and matches the AWS access key pattern.
#   TS-01   with code_quality.no_secrets, a tool result carrying it ends the run
#           (exit 3) and the next request never carries it
#   TS-01c  CONTROL: without no_secrets the same result reaches the next request
#           -- the scenario does deliver it, so TS-01's absence is the scan's
KEYEXPR='"AK" + "IA" + "TEST" + "TEST" + "TEST" + "TEST"'
KEYVAL="AK""IA""TESTTESTTESTTEST"
sent_key() { cat "$WDIR"/req_*.json 2>/dev/null | grep -q "$KEYVAL"; }
run_case ts01 '[{"tool_calls": [{"name": "get_data", "args": {"query": "q"}}]},
                {"content": "FINAL-ts01"}]' '
    "code_quality": { "no_secrets": { "enabled": true, "level": "hard" } },' "$KEYEXPR"
if [ "$OUT" = "STUB-FAILED" ]; then unm TS-01 "the stub did not start"
elif [ "$RC" -eq 3 ] && [ -f "$WDIR/side_effect.txt" ] && ! sent_key && grep -q 'no_secrets' <<<"$OUT"; then
    pass TS-01 "a secret in a tool result ends the run (exit 3); the model never receives it"
else
    fail TS-01 "a secret in a tool result should end the run before the next request" "rc=$RC $(why)"
fi
run_case ts01c '[{"tool_calls": [{"name": "get_data", "args": {"query": "q"}}]},
                 {"content": "FINAL-ts01c"}]' "" "$KEYEXPR"
if [ "$OUT" = "STUB-FAILED" ]; then unm TS-01c "the stub did not start"
elif accepted ts01c && sent_key; then
    pass TS-01c "CONTROL: without no_secrets the tool result reaches the next request"
else
    fail TS-01c "without no_secrets the result should reach the next request" "rc=$RC $(why)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
