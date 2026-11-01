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
#
# Kill table (observed 2026-11-01): reportedTokenCount() not clamping at 0 ->
# TC-01; reading the count with get<int>() again -> TC-01, TC-02.
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

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
exit 0
