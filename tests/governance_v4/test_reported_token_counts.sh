#!/usr/bin/env bash
# test_reported_token_counts.sh -- a provider's token counts cannot un-spend a budget (A110)
#
# The run's token budget (agent_dispatch.hard_stop.max_tokens_per_run) and the
# per-agent totals add the counts the provider reports in its response body.
# They were taken as given: a negative count was charged as negative, so one
# reply reporting about -1,000,000 tokens let a 1,000-token run keep sending,
# and a count past int's range wrapped (2^62 read as 0, 2^32-1 as -1). Found by
# the model-channel fuzzer (tests/fuzzing/model_channel). Counts are now read
# as 64-bit and clamped to [0, 10,000,000] where they are parsed.
#
# Each run: a 1,000-token run budget, four sends; the first reply reports 900
# tokens, so the run is past its budget after the second call.
#   TC-00   CONTROL: ordinary counts -- the third and fourth sends are refused
#           (hard stop) and the stub sees two requests
#   TC-01   the second reply reports -500,000 input and -500,000 output tokens:
#           the run still stops at its budget (no more than three requests,
#           and the total never goes below the first reply's 900)
#   TC-02   the second reply reports 2^62 and 2^32-1: they are charged (the
#           total reaches at least 10,000,000) and the run stops at once
#   TC-03   (A110 residual, Bundle 19) every reply reports the 10M+10M each
#           count is clamped to, under a run budget of INT_MAX - 1 with the
#           per-agent budget off: the run stops after the 108th call, where
#           the total (2,160,000,000) first passes the budget. As int, the
#           total wrapped negative there and the run never stopped. The shown
#           total saturates at INT_MAX and is never negative.
#   TC-04   the same with the per-agent max_total_tokens at INT_MAX - 1 and no
#           run budget: the 109th send is refused
#
# Kill table (observed 2026-11-01): reportedTokenCount() not clamping at 0 ->
# TC-01; reading the count with get<int>() again -> TC-01, TC-02. Bundle 19:
# the run total back to int -> TC-03; the tracker counts back to int -> TC-04.
# Red first (observed): on master fc9b8099 TC-01 fails (four requests, lowest
# total -999100) and TC-02 fails (three requests, highest total 1799); TC-00 passes.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_tokcount_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

STUB_PORT=""
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
# An empty trust store: with a key in the ambient one, every unsigned config
# here would be an INTEGRITY BLOCK (exit 3) whatever it says.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { stop_stub 2>/dev/null; rm -rf "$TEST_TMP"; teardown_isolated_trust; }
trap cleanup EXIT
export FAKE_KEY_TOKCOUNT="fake-key-tokcount"

# run_case ID SECOND_REPLY_JSON -> OUT, REQS, WDIR
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    cat > "$WDIR/fixture.json" <<J
{"responses": [
 {"content": "first", "input_tokens": 100, "output_tokens": 800},
 $2,
 {"content": "third", "input_tokens": 100, "output_tokens": 800},
 {"content": "fourth", "input_tokens": 100, "output_tokens": 800}
]}
J
    if ! start_stub "$WDIR/fixture.json" "$WDIR"; then OUT="STUB-FAILED"; REQS=0; return; fi
    cat > "$WDIR/govern.json" <<G
{"version": "3.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "agent_dispatch": {"hard_stop": {"max_tokens_per_run": 1000}},
 "agents": {"a": {"provider": "gemini", "model": "stub-model",
   "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_TOKCOUNT",
   "max_tokens": 100, "max_turns": 20, "max_total_tokens": 100000000,
   "retry": {"max_attempts": 1}}}}
G
    cat > "$WDIR/t.naab" <<'P'
use agent
main {
    let h = agent.create("a")
    let i = 0
    while i < 4 {
        try {
            let r = agent.send(h, "turn " + string(i))
            print("SENT " + string(i) + " tokens_used=" + string(agent.dispatch_status().get("tokens_used")))
        } catch (e) {
            print("CAUGHT " + string(i) + " " + string(e).split("\n")[0])
        }
        i = i + 1
    }
}
P
    OUT=$(cd "$WDIR" && timeout 60s "$NAAB" t.naab 2>&1)
    stop_stub
    REQS=$(ls "$WDIR"/req_*.json 2>/dev/null | wc -l | tr -d ' ')
}
min_total() { grep -oE 'tokens_used=-?[0-9]+' <<<"$OUT" | cut -d= -f2 | sort -n | head -1; }
max_total() { grep -oE 'tokens_used=-?[0-9]+' <<<"$OUT" | cut -d= -f2 | sort -n | tail -1; }

echo "=== Provider token counts cannot un-spend a budget (A110) ==="

run_case tc00 '{"content": "second", "input_tokens": 100, "output_tokens": 800}'
if [ "$OUT" = "STUB-FAILED" ]; then unm TC-00 "the stub did not start"
elif [ "$REQS" -eq 2 ] && grep -q 'CAUGHT 2 .*Hard stop' <<<"$OUT"; then
    pass TC-00 "CONTROL: with ordinary counts the run stops at its budget (2 requests)"
else fail TC-00 "CONTROL: the budget should stop the run after two calls" "requests=$REQS $(tr '\n' ' ' <<<"$OUT" | cut -c1-300)"; fi

run_case tc01 '{"raw_payload": {"candidates": [{"content": {"parts": [{"text": "negative"}], "role": "model"}, "finishReason": "STOP"}], "usageMetadata": {"promptTokenCount": -500000, "candidatesTokenCount": -500000}}}'
if [ "$OUT" = "STUB-FAILED" ]; then unm TC-01 "the stub did not start"
elif [ "$REQS" -le 3 ] && [ "$(min_total)" -ge 900 ] && grep -q 'CAUGHT 3' <<<"$OUT"; then
    pass TC-01 "a reply reporting negative counts does not un-spend the run budget (requests=$REQS, lowest total $(min_total))"
else fail TC-01 "negative reported counts un-spent the run budget" "requests=$REQS lowest total=$(min_total)"; fi

run_case tc02 '{"raw_payload": {"candidates": [{"content": {"parts": [{"text": "huge"}], "role": "model"}, "finishReason": "STOP"}], "usageMetadata": {"promptTokenCount": 4611686018427387904, "candidatesTokenCount": 4294967295}}}'
if [ "$OUT" = "STUB-FAILED" ]; then unm TC-02 "the stub did not start"
elif [ "$REQS" -eq 2 ] && [ "$(max_total)" -ge 10000000 ]; then
    pass TC-02 "counts past int's range are charged, not wrapped (total $(max_total)), and the run stops"
else fail TC-02 "huge reported counts were not charged as large" "requests=$REQS highest total=$(max_total)"; fi

# run_big ID RUN_BUDGET AGENT_BUDGET -> OUT, REQS
run_big() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    echo '{"responses": [{"content": "big", "input_tokens": 10000000, "output_tokens": 10000000}]}' > "$WDIR/fixture.json"
    if ! start_stub "$WDIR/fixture.json" "$WDIR"; then OUT="STUB-FAILED"; REQS=0; return; fi
    cat > "$WDIR/govern.json" <<G
{"version": "3.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "agent_dispatch": {"hard_stop": {"max_tokens_per_run": $2}},
 "agents": {"a": {"provider": "gemini", "model": "stub-model",
   "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_TOKCOUNT",
   "max_tokens": 100, "max_turns": 1000, "max_total_tokens": $3,
   "retry": {"max_attempts": 1}}}}
G
    cat > "$WDIR/t.naab" <<'P'
use agent
main {
    let h = agent.create("a")
    let i = 0
    let stopped = -1
    while i < 130 {
        try {
            let r = agent.send(h, "turn " + string(i))
            print("SENT " + string(i) + " tokens_used=" + string(agent.dispatch_status().get("tokens_used")))
        } catch (e) {
            if stopped < 0 {
                stopped = i
                print("FIRST_REFUSED " + string(i) + " " + string(e).split("\n")[0])
            }
        }
        i = i + 1
    }
    print("USAGE total_tokens=" + string(agent.usage(h).get("total_tokens")))
}
P
    OUT=$(cd "$WDIR" && timeout 120s "$NAAB" t.naab 2>&1)
    stop_stub
    REQS=$(ls "$WDIR"/req_*.json 2>/dev/null | wc -l | tr -d ' ')
}

run_big tc03 2147483646 0
if [ "$OUT" = "STUB-FAILED" ]; then unm TC-03 "the stub did not start"
elif ! grep -q "SENT 0 " <<<"$OUT"; then unm TC-03 "the first send did not go out: $(tr '\n' ' ' <<<"$OUT" | cut -c1-200)"
elif [ "$REQS" -eq 108 ] && grep -q "FIRST_REFUSED 108 .*Hard stop" <<<"$OUT" && [ "$(min_total)" -ge 0 ] \
     && grep -q "tokens_used=2147483647" <<<"$OUT"; then
    pass TC-03 "a run budget of INT_MAX - 1 stops the run after the 108th call (total shown saturated at INT_MAX, never negative)"
else fail TC-03 "the run budget near INT_MAX was not reached" "requests=$REQS lowest shown=$(min_total) $(grep -m1 FIRST_REFUSED <<<"$OUT" | cut -c1-160)"; fi

run_big tc04 0 2147483646
if [ "$OUT" = "STUB-FAILED" ]; then unm TC-04 "the stub did not start"
elif ! grep -q "SENT 0 " <<<"$OUT"; then unm TC-04 "the first send did not go out: $(tr '\n' ' ' <<<"$OUT" | cut -c1-200)"
elif [ "$REQS" -eq 108 ] && grep -q "FIRST_REFUSED 108 " <<<"$OUT" && grep -q "USAGE total_tokens=2147483647" <<<"$OUT"; then
    pass TC-04 "a per-agent budget of INT_MAX - 1 refuses the 109th send (usage shown saturated at INT_MAX)"
else fail TC-04 "the per-agent budget near INT_MAX was not reached" "requests=$REQS $(grep -m1 -E 'FIRST_REFUSED|USAGE' <<<"$OUT" | cut -c1-160) $(grep USAGE <<<"$OUT")"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
exit 0
