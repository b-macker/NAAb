#!/usr/bin/env bash
# test_model_channel_fuzz.sh -- what a model sends back never breaks the governor (Bundle 9)
#
# tests/fuzzing/model_channel/fuzz_model_channel.py serves seeded hostile model
# replies through tests/helpers/agent_stub.py (empty, huge, odd encodings,
# malformed and truncated bodies, odd JSON shapes, wrong tool calls, HTTP
# errors, a secret-shaped reply) to a program that sends three messages to an
# agent with a tool. Per case: no hang, exit 0/3/4, an exit 0 run finished, the
# telemetry chain verifies, the report parses, no pairing violation reached the
# stub.
#
#   MC-00/<plant>  CONTROL: an interpreter planted to break one invariant
#                  (crash, hang, exit1, noevidence, pairing) fails every case --
#                  each check can fail
#   MC-01          seeds 1-40 pass on the real interpreter
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
FUZZ="$SCRIPT_DIR/model_channel/fuzz_model_channel.py"
PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -25 | sed 's/^/       | /'; }

echo "=== Model-channel fuzzer (Bundle 9) ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    echo "  UNMEASURABLE [MC-01] naab-lang not built"; echo "Results: 0 passed, 1 failed"; exit 1
fi
for plant in crash hang exit1 noevidence pairing; do
    out="$(python3 "$FUZZ" --naab "$NAAB" --seeds 1-2 --plant "$plant" 2>&1)"; rc=$?
    if [ "$rc" -eq 0 ]; then pass "MC-00/$plant" "CONTROL: a planted $plant fails every case"
    else fail "MC-00/$plant" "CONTROL: the planted $plant was not caught on every case" "$out"; fi
done
out="$(python3 "$FUZZ" --naab "$NAAB" --seeds 1-40 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then pass MC-01 "40 seeded hostile reply sequences: no crash, hang or lost evidence ($(tail -1 <<<"$out"))"
else fail MC-01 "a seeded reply sequence broke an invariant" "$out"; fi
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
