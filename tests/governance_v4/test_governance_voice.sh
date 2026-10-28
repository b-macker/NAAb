#!/usr/bin/env bash
# test_governance_voice.sh -- the governance voice prints once, and its reply is scanned (A40)
#
# output.voice names an agent that the engine asks, at the end of a run with
# violations, for a remediation guide (631db80a). c0d3c5da made
# runGovernanceVoice() print the guide itself, because on an error path the
# summary formatter never runs -- and on a clean run the formatter then printed
# the same guide again: one call, two prints (observed, master 6055ac3c). Nothing
# scanned the reply, a model's text printed to the terminal; it now goes through
# secretFinding()/piiFinding() -- decide-only, the twins guidance uses -- and a
# reply that matches is withheld: neither printed nor cached. A cache hit takes
# the same path, so a guide cached by an older build is scanned too.
#
#   VO-00   CONTROL: the stub received exactly one voice request, and it carries
#           the violation -- the voice ran, and only once
#   VO-01   a clean run with an advisory: the guide appears exactly once
#   VO-02   a compile-time HARD block (a TODO under a hard rule): the voice is
#           called once and the guide printed once -- on this path the summary
#           formatter never runs, so the guide must be printed where it is made
#           (the reason c0d3c5da added that print; dropping it instead of the
#           formatter's copy turns this arm red)
#   VO-05   a whole-source HARD block (a secret literal in main{}) sends nothing
#           to the voice: its prompt carries the whole script, so voicing that
#           block would send the blocked value to a model provider. The source is
#           handed to the voice only after the whole-source checks (main.cpp),
#           and this pins it there
#   VO-03   a guide carrying a secret, under no_secrets: withheld -- the value is
#           in no output and no cache file, and the run says it was withheld
#   VO-03c  CONTROL: without no_secrets the same guide is printed -- the stub
#           delivers it, so VO-03's absence is the scan's
#   VO-04   a guide carrying PII, under no_pii: withheld likewise
#   VO-04c  CONTROL: without no_pii it is printed
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-28):
#   the summary formatter prints the guide again (the pre-fix shape) -> VO-01 red
#   the guide printed only by the formatter (the immediate print
#     dropped instead -- the other way to "print once")               -> VO-02 red
#   no secretFinding() on the guide                                   -> VO-03 red
#   no piiFinding() on the guide                                      -> VO-04 red
# VO-05 has no mutant: it pins where main.cpp hands the voice its source.
# Red first: on master VO-01, VO-03 and VO-04 fail (printed twice; the secret
# and PII guides printed twice and cached), every other arm passes.
# Secret and PII values are assembled from fragments at run time.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_voice_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

STUB_PORT=""
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup() { stop_stub 2>/dev/null; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
export FAKE_KEY_VOICE="fake-key-voice"

KEY="AK""IA""TESTTESTTESTTEST"
SSN="12""3-4""5-6789"
GUIDE="VOICEFIX-ONE: In f(), replace the placeholder with real logic."

# run_case ID REPLY CODE_QUALITY_JSON PROGRAM -> sets OUT, RC, WDIR, REQS
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    python3 -c 'import json, sys; json.dump({"responses": [{"content": sys.argv[1]}]}, open(sys.argv[2], "w"))' \
        "$2" "$WDIR/fixture.json"
    if ! start_stub "$WDIR/fixture.json" "$WDIR"; then OUT="STUB-FAILED"; REQS=0; return; fi
    cat > "$WDIR/govern.json" <<GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "code_quality": $3,
    "output": { "voice": "voice", "voice_cache": true },
    "agents": {
        "voice": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_VOICE",
            "max_tokens": 200
        }
    }
}
GOVEOF
    printf '%s' "$4" > "$WDIR/p.naab"
    RC=0; OUT=$(cd "$WDIR" && timeout 60s "$NAAB" run p.naab 2>&1) || RC=$?
    stop_stub
    REQS=$(ls "$WDIR"/req_*.json 2>/dev/null | wc -l)
}
times() { grep -c -F -- "$1" <<<"$OUT"; }
cached() { grep -rqs -F -- "$1" "$WDIR/.naab_cache"; }

ADVISORY_PROG=$'fn f() {\n    // TODO: finish\n    return 1\n}\nmain {\n    print(f())\n}\n'
ADVISORY_CQ='{"no_placeholders": {"enabled": true, "level": "advisory"}}'

echo "=== Governance voice (A40) ==="

run_case vo01 "$GUIDE" "$ADVISORY_CQ" "$ADVISORY_PROG"
if [ "$OUT" = "STUB-FAILED" ]; then
    unm VO-00 "the stub did not start"; unm VO-01 "the stub did not start"
else
    if [ "$REQS" -eq 1 ] && grep -qs 'governance advisor' "$WDIR"/req_*.json && grep -qs 'no_placeholders' "$WDIR"/req_*.json; then
        pass VO-00 "CONTROL: one voice request, carrying the violation"
    else
        fail VO-00 "the voice should be asked once, about the violation" "requests=$REQS rc=$RC"
    fi
    n=$(times "VOICEFIX-ONE")
    if [ "$RC" -eq 0 ] && [ "$n" -eq 1 ]; then
        pass VO-01 "a clean run with an advisory prints the guide exactly once"
    else
        fail VO-01 "the guide should print exactly once" "rc=$RC printed=$n times"
    fi
fi

run_case vo02 "$GUIDE" '{"no_placeholders": {"enabled": true, "level": "hard"}}' "$ADVISORY_PROG"
if [ "$OUT" = "STUB-FAILED" ]; then unm VO-02 "the stub did not start"
elif [ "$RC" -eq 3 ] && [ "$REQS" -eq 1 ] && [ "$(times "VOICEFIX-ONE")" -eq 1 ]; then
    pass VO-02 "a compile-time HARD block calls the voice once and prints the guide once"
else
    fail VO-02 "a compile-time block should be voiced and printed once" "rc=$RC requests=$REQS printed=$(times "VOICEFIX-ONE")"
fi

SECRET_SRC_PROG="$(printf 'main {\n    let k = "%s"\n    print(k)\n}\n' "$KEY")"
run_case vo05 "$GUIDE" '{"no_secrets": {"enabled": true, "level": "hard"}}' "$SECRET_SRC_PROG"
if [ "$OUT" = "STUB-FAILED" ]; then unm VO-05 "the stub did not start"
elif [ "$RC" -eq 3 ] && [ "$REQS" -eq 0 ] && grep -q 'no_secrets' <<<"$OUT"; then
    pass VO-05 "a whole-source secret block sends nothing to the voice (the prompt would carry the script)"
else
    fail VO-05 "a whole-source secret block must not reach the voice" "rc=$RC requests=$REQS"
fi

for spec in "VO-03|secret|$KEY|{\"no_placeholders\": {\"enabled\": true, \"level\": \"advisory\"}, \"no_secrets\": {\"enabled\": true, \"level\": \"hard\"}}|1" \
            "VO-03c|secret|$KEY|$ADVISORY_CQ|0" \
            "VO-04|PII|$SSN|{\"no_placeholders\": {\"enabled\": true, \"level\": \"advisory\"}, \"no_pii\": {\"enabled\": true, \"level\": \"hard\"}}|1" \
            "VO-04c|PII|$SSN|$ADVISORY_CQ|0"; do
    IFS='|' read -r arm kind value cq scanned <<<"$spec"
    run_case "$arm" "1. In f(), the value $value goes here." "$cq" "$ADVISORY_PROG"
    if [ "$OUT" = "STUB-FAILED" ]; then unm "$arm" "the stub did not start"; continue; fi
    n=$(times "$value")
    if [ "$scanned" = 1 ]; then
        if [ "$RC" -eq 0 ] && [ "$REQS" -eq 1 ] && [ "$n" -eq 0 ] && ! cached "$value" \
                && grep -q 'Voice Summary withheld' <<<"$OUT"; then
            pass "$arm" "a guide carrying $kind is withheld: printed nowhere, cached nowhere, and said so"
        else
            fail "$arm" "a guide carrying $kind should be withheld" "rc=$RC requests=$REQS printed=$n cached=$(cached "$value" && echo yes || echo no)"
        fi
    else
        if [ "$RC" -eq 0 ] && [ "$n" -ge 1 ]; then
            pass "$arm" "CONTROL: without the rule the guide carrying $kind is printed"
        else
            fail "$arm" "without the rule the guide should be printed" "rc=$RC requests=$REQS printed=$n"
        fi
    fi
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
