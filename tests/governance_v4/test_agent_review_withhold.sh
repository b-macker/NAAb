#!/usr/bin/env bash
# test_agent_review_withhold.sh -- what agent review's models wrote is scanned
# like every other model output before it is printed, recorded or cached (A166)
#
# agent_review (922041ec) runs before the program: detection agents answer
# FINDING|category|description, an optional validation agent filters them, and
# an optional voice agent writes one remediation guide. Every one of those texts
# is a model's -- and each agent is shown the whole script, so a key the script
# holds can come back in a description, a category, a rejected finding's hint
# or the guide. None of it was scanned: the description is printed and passed
# to enforce() (so it reaches every report and telemetry), the guide printed,
# the rejected findings printed as hints, and all of it written to
# .naab_cache. Since Bundle 6 a model's reply to the PROGRAM is scanned for
# secrets unless the config switches that off (`"no_secrets": false`); the
# review's texts now are too, by the same switch, and for PII under no_pii
# (opt-in, as for every model output). A matching text is withheld -- replaced
# by a notice that names the pattern kind, never the value -- and the finding
# itself still counts: its zone, score and level are computed before anything
# is withheld, so withholding never lowers what the review enforces.
#
# The scan runs once, on the result the review returns -- fresh or read from the
# cache -- and before the cache is written, so a cache written by an older build
# is scanned when it is read (one site; there is no separate load path to miss).
#
#   AW-00   CONTROL: a clean description is printed and enforced, and the
#           program runs -- the stub plays the reviewer
#   AW-01   a description carrying a key, under the default: printed nowhere,
#           cached nowhere, the finding still enforced, and the run says so
#   AW-01c  CONTROL: `"no_secrets": false` -- the same description is printed,
#           so AW-01's absence is the scan's
#   AW-02   a CATEGORY carrying a key: withheld too, the finding still enforced
#   AW-03   a voice guide carrying a key: withheld, the findings listed instead
#   AW-03c  CONTROL: `"no_secrets": false` -- the guide is printed
#   AW-04   a rejected finding's hint carrying a key: withheld, the hint kept
#   AW-05   PII in a description under no_pii: withheld
#   AW-05c  CONTROL: without no_pii it is printed (PII is opt-in, as everywhere)
#   AW-06   a hard zone still blocks (exit 3), with the key withheld -- the
#           block prints the description itself, so the notice that replaced
#           it is what shows (and it must survive the error sanitizer whole)
#   AW-07   a cache hit: the reviewer is asked once over two runs, and neither
#           run nor the cache file carries the key
#
# Kill table (each mutant rebuilt, suite re-run):
#   no scan of descriptions                        -> AW-01 AW-06 AW-07 red
#   no scan of categories                          -> AW-02 red
#   no scan of the guide                           -> AW-03 red
#   no scan of rejected findings                   -> AW-04 red
#   no PII scan                                    -> AW-05 red
#   secrets scanned only under no_secrets          -> AW-01 AW-02 AW-03 AW-04 AW-06 AW-07 red
#   the cache written before the scan              -> AW-07 red
#   a withheld finding dropped instead of kept     -> AW-01 AW-02 AW-06 red
#   every text withheld                            -> AW-00 AW-01c AW-03c AW-05c red
# Red first: on master AW-01, AW-02, AW-03, AW-04, AW-05, AW-06 and AW-07
# fail, and every control passes.
# Secret and PII values are assembled from fragments at run time.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_agent_review_withhold.sh"
if [ ! -x "$NAAB" ]; then echo "  test_agent_review_withhold.sh: SKIPPED -- $NAAB not built"; exit 0; fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "  test_agent_review_withhold.sh: SKIPPED -- python3 (agent stub) not available"; exit 0
fi
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
STUB_PORT=""
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_arw_XXXXXX")
cleanup() { stop_stub 2>/dev/null; teardown_isolated_trust; [ -n "${AW_KEEP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
export FAKE_AW_KEY="fake-key-agent-review"

KEY="AK""IA""TESTTESTTESTTEST"
SSN="12""3-4""5-6789"
export KEY SSN

agent_json() {  # $1=name
    printf '"%s": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_AW_KEY", "max_tokens": 100, "system_prompt": "TOKEN_%s"}' \
        "$1" "$STUB_PORT" "$1"
}

# fixture DET [ANALYST [VOICE]] -> JSON on stdout; each argument is one agent's
# whole reply, with @KEY@ and @SSN@ replaced at run time.
fixture() {
    python3 -c '
import json, os, sys
sub = lambda t: t.replace("@KEY@", os.environ["KEY"]).replace("@SSN@", os.environ["SSN"])
routes = {"TOKEN_det_a": {"responses": [{"content": sub(sys.argv[1])}]}}
if len(sys.argv) > 2 and sys.argv[2]:
    routes["TOKEN_analyst"] = {"responses": [{"content": sub(sys.argv[2])}]}
if len(sys.argv) > 3 and sys.argv[3]:
    routes["TOKEN_voicer"] = {"responses": [{"content": sub(sys.argv[3])}]}
json.dump({"routes": routes, "responses": [{"content": "UNROUTED_REQUEST"}]}, sys.stdout)
' "$@"
}

# run_review ID AGENT_REVIEW_JSON CODE_QUALITY_JSON FIXTURE [RUNS]
#   -> D, RC (last run), OUT (stdout+stderr of every run), REQS (det_a requests)
run_review() {
    D="$TEST_TMP/$1"; mkdir -p "$D"; OUT=""; RC=""; REQS=0
    printf '%s' "$4" > "$D/fixture.json"
    if ! start_stub "$D/fixture.json" "$D"; then OUT="STUB-FAILED"; return; fi
    cat > "$D/govern.json" <<EOF
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "code_quality": $3,
  "agents": {
    $(agent_json det_a), $(agent_json analyst), $(agent_json voicer)
  },
  "agent_review": $2
}
EOF
    if ! python3 -c 'import json,sys; json.load(sys.stdin)' < "$D/govern.json" 2>/dev/null; then
        stop_stub; OUT="BAD-CONFIG"; return
    fi
    printf 'main {\n    print("PROGRAM_RAN")\n}\n' > "$D/p.naab"
    local r o
    for r in $(seq 1 "${5:-1}"); do
        RC=0; o=$(cd "$D" && timeout 60 "$NAAB" run p.naab 2>&1) || RC=$?
        OUT="$OUT$o"$'\n'
    done
    stop_stub
    REQS=$(grep -c ' TOKEN_det_a$' "$D/routes.log" 2>/dev/null || true); REQS=${REQS:-0}
}
has()    { grep -q -F -- "$1" <<<"$OUT"; }
cached() { grep -rqs -F -- "$1" "$D/.naab_cache"; }
ok_run() { [ "$OUT" != "STUB-FAILED" ] && [ "$OUT" != "BAD-CONFIG" ]; }

AR='{"enabled": true, "detection": ["det_a"], "cache": true}'
AR_VOICE='{"enabled": true, "detection": ["det_a"], "voice": "voicer", "cache": true}'
AR_HINTS='{"enabled": true, "detection": ["det_a"], "validation": "analyst", "hints": true, "cache": true}'
AR_HARD='{"enabled": true, "detection": ["det_a"], "cache": true, "enforcement": {"green": "hard", "yellow": "hard", "red": "hard"}}'
CQ_DEFAULT='{}'
CQ_OFF='{"no_secrets": false}'
CQ_PII='{"no_pii": {"enabled": true, "level": "hard"}}'

echo "=== agent_review: what its models wrote is scanned (A166) ==="

# withheld ID WHAT VALUE -- the common assertion: the value is in no output and
# no cache file, the run says something was withheld, and the program ran
withheld() {
    if [ "$RC" -eq 0 ] && [ "$REQS" -ge 1 ] && ! has "$3" && ! cached "$3" \
            && has "withheld" && has "PROGRAM_RAN" && has "${4:-agent_review.}"; then
        pass "$1" "$2: printed nowhere, cached nowhere, the finding kept, and said so"
    else
        fail "$1" "$2 should be withheld" "rc=$RC asked=$REQS printed=$(has "$3" && echo yes || echo no) cached=$(cached "$3" && echo yes || echo no) notice=$(has withheld && echo yes || echo no) ran=$(has PROGRAM_RAN && echo yes || echo no)"
    fi
}

run_review aw00 "$AR" "$CQ_DEFAULT" "$(fixture 'FINDING|cat_alpha|alpha issue CLEANMARK')"
if ! ok_run; then unm AW-00 "harness: $OUT"
elif [ "$RC" -eq 0 ] && [ "$REQS" -eq 1 ] && has "CLEANMARK" && has "agent_review.cat_alpha" && has "PROGRAM_RAN" && ! has "withheld"; then
    pass AW-00 "CONTROL: a clean description is printed and enforced, and the program runs"
else
    fail AW-00 "a clean finding should be printed" "rc=$RC asked=$REQS"
fi

run_review aw01 "$AR" "$CQ_DEFAULT" "$(fixture 'FINDING|cat_alpha|the key @KEY@ is hardcoded')"
if ! ok_run; then unm AW-01 "harness: $OUT"; else withheld AW-01 "a description carrying a key" "$KEY" "agent_review.cat_alpha"; fi

run_review aw01c "$AR" "$CQ_OFF" "$(fixture 'FINDING|cat_alpha|the key @KEY@ is hardcoded')"
if ! ok_run; then unm AW-01c "harness: $OUT"
elif [ "$RC" -eq 0 ] && has "$KEY" && has "agent_review.cat_alpha"; then
    pass AW-01c "CONTROL: with the scan switched off the description is printed"
else
    fail AW-01c "with the scan off the description should be printed" "rc=$RC asked=$REQS"
fi

run_review aw02 "$AR" "$CQ_DEFAULT" "$(fixture 'FINDING|@KEY@|a category the reviewer filled with the key')"
if ! ok_run; then unm AW-02 "harness: $OUT"; else withheld AW-02 "a category carrying a key" "$KEY"; fi

run_review aw03 "$AR_VOICE" "$CQ_DEFAULT" "$(fixture 'FINDING|cat_alpha|alpha issue' '' '1. Move @KEY@ out of the script.')"
if ! ok_run; then unm AW-03 "harness: $OUT"
else
    withheld AW-03 "a voice guide carrying a key" "$KEY" "cat_alpha"
    if [ "$(grep -c ' TOKEN_voicer$' "$D/routes.log" 2>/dev/null)" -ne 1 ]; then
        fail AW-03 "the voice agent should have been asked once" "$(grep -c ' TOKEN_voicer$' "$D/routes.log" 2>/dev/null)"
    fi
fi

run_review aw03c "$AR_VOICE" "$CQ_OFF" "$(fixture 'FINDING|cat_alpha|alpha issue' '' '1. Move @KEY@ out of the script.')"
if ! ok_run; then unm AW-03c "harness: $OUT"
elif [ "$RC" -eq 0 ] && has "Move $KEY out"; then
    pass AW-03c "CONTROL: with the scan switched off the guide is printed"
else
    fail AW-03c "with the scan off the guide should be printed" "rc=$RC"
fi

run_review aw04 "$AR_HINTS" "$CQ_DEFAULT" \
    "$(fixture $'FINDING|cat_alpha|alpha issue\nFINDING|cat_beta|beta: the key @KEY@' $'VERDICT|CONFIRMED|cat_alpha|real\nVERDICT|FALSE_POSITIVE|cat_beta|not real')"
if ! ok_run; then unm AW-04 "harness: $OUT"
else
    withheld AW-04 "a rejected finding's hint carrying a key" "$KEY" "agent_review.cat_alpha"
    if ! grep -q '^\[hint\].*cat_beta' <<<"$OUT"; then fail AW-04 "the rejected finding's hint line should be kept" "no [hint] cat_beta line"; fi
fi

run_review aw05 "$AR" "$CQ_PII" "$(fixture 'FINDING|cat_alpha|the SSN @SSN@ is printed')"
if ! ok_run; then unm AW-05 "harness: $OUT"; else withheld AW-05 "PII in a description, under no_pii" "$SSN" "agent_review.cat_alpha"; fi

run_review aw05c "$AR" "$CQ_DEFAULT" "$(fixture 'FINDING|cat_alpha|the SSN @SSN@ is printed')"
if ! ok_run; then unm AW-05c "harness: $OUT"
elif [ "$RC" -eq 0 ] && has "$SSN"; then
    pass AW-05c "CONTROL: without no_pii the PII description is printed (PII is opt-in)"
else
    fail AW-05c "without no_pii the description should be printed" "rc=$RC"
fi

run_review aw06 "$AR_HARD" "$CQ_DEFAULT" "$(fixture 'FINDING|cat_alpha|the key @KEY@ is hardcoded')"
if ! ok_run; then unm AW-06 "harness: $OUT"
elif [ "$RC" -eq 3 ] && ! has "$KEY" && ! cached "$KEY" && has "looked like it held a secret)" && ! has "PROGRAM_RAN"; then
    pass AW-06 "a hard zone still blocks (exit 3), with the key withheld"
else
    fail AW-06 "a hard zone should block with the key withheld" "rc=$RC printed=$(has "$KEY" && echo yes || echo no) ran=$(has PROGRAM_RAN && echo yes || echo no): $(tail -8 <<<"$OUT" | tr '\n' ' ' | cut -c1-400)"
fi

run_review aw07 "$AR" "$CQ_DEFAULT" "$(fixture 'FINDING|cat_alpha|the key @KEY@ is hardcoded')" 2
if ! ok_run; then unm AW-07 "harness: $OUT"
elif [ "$REQS" -eq 1 ] && ls "$D"/.naab_cache/*.review.json >/dev/null 2>&1 \
        && ! has "$KEY" && ! cached "$KEY" && [ "$(grep -c 'agent_review.cat_alpha' <<<"$OUT")" -ge 2 ]; then
    pass AW-07 "a cache hit: one review over two runs, and neither run nor the cache carries the key"
else
    fail AW-07 "the cached review should carry no key" "asked=$REQS printed=$(has "$KEY" && echo yes || echo no) cached=$(cached "$KEY" && echo yes || echo no) enforced=$(grep -c 'agent_review.cat_alpha' <<<"$OUT")"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
