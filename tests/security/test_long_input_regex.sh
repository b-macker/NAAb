#!/usr/bin/env bash
# test_long_input_regex.sh -- a long input is matched, never a crash (A89)
#
# libstdc++'s std::regex recurses once per character on an unbounded
# repetition, so a long enough input overflowed the stack and the governor died
# with SIGSEGV: no verdict, no report, exit 139. The input was the governed
# side's to choose -- a model reply, a program's own text, a pattern and text
# the program passes in. Every regex in the engine now goes through naab::rx
# (QuickJS's libregexp, backtracking state on the heap).
#
# Each crash arm has a "c" arm with the offending content at the FAR END of the
# same long input, which must still be refused: a fix that stopped scanning
# early -- truncating the input -- would pass the crash arm and fail its "c".
#
#   LR-00   CONTROL: the stub answers and a short field is delivered (harness)
#   LR-01   an operator's output_contract regex ^[a-z ]+$ over a 60,000-byte
#           reply field: delivered (master: SIGSEGV)
#   LR-01c  the same field ending in "!": a contract violation, not delivered
#   LR-02   the engine's own no_pii scan over a 60,000-byte reply: delivered
#           (master: SIGSEGV)
#   LR-02c  the same reply with an SSN at its end: refused (exit 3)
#   LR-03   the whole-file PII check over a program holding a 60,000-byte
#           literal: the program runs (master: SIGSEGV before it ran)
#   LR-03c  the same literal ending in an SSN: refused (exit 3)
#   LR-04   orchestra.enforce_convergence with a program-supplied pattern over
#           a 60,000-byte response: valid (master: SIGSEGV)
#   LR-04c  the same response ending in "!": not valid
#   LR-05   regex.matches(text, "^[a-z ]+$") on 60,000 bytes: true (master:
#           SIGSEGV)
#   LR-05c  the same text ending in "!": false
#
# The SSN is built here from fragments; the repo holds none.
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-30):
#   naab::rx built on std::regex (the pre-fix engine, NAAB_RX_LIBREGEXP off)
#                          -> LR-01 LR-01c LR-02 LR-03 LR-04 LR-04c LR-05 LR-05c red
#   naab::rx searches only the first 50,000 bytes
#                          -> LR-01c LR-02c LR-03c LR-04c LR-05c red (the c arms
#                             are the guard against fixing the crash by truncating)
# Red first: on master the same eight arms as the first mutant fail (SIGSEGV).
# LR-02c and LR-03c pass on master too: the SSN pattern is the first PII
# pattern tried, finds the SSN and returns before the pattern that crashed runs.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
export FAKE_KEY_LR=stub-key
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_longrx_XXXXXX")
trap 'stop_stub 2>/dev/null; teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

LONG=$(head -c 60000 /dev/zero | tr '\0' 'a')
SSN="123""-45-""6789"
BASE='"mode": "enforce", "security": { "sandbox_level": "elevated" }'

# agent_case ID REPLY_CONTENT_JSON_ESCAPED EXTRA_GOVERN_JSON AGENT_EXTRA_JSON -> OUT, RC
agent_case() {
    local d="$W/$1"; mkdir -p "$d"; cd "$d" || return
    printf '{"responses":[{"content":"%s","output_tokens":9}]}\n' "$2" > fixture.json
    if ! start_stub fixture.json . >/dev/null 2>&1; then OUT="STUBFAIL"; RC=-1; cd "$W"; return; fi
    cat > govern.json <<GEOF
{ $BASE $3,
  "agents": { "w": { "provider": "gemini", "model": "stub",
    "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_LR",
    "max_tokens": 50, "max_turns": 20 $4 } } }
GEOF
    cat > p.naab <<'NEOF'
use agent
main {
    let h = agent.create("w")
    try {
        let r = agent.send(h, "answer")
        print("DELIVERED len=" + string(len(string(r.get("content")))))
    } catch (e) {
        print("CAUGHT=" + string(e))
    }
}
NEOF
    RC=0; OUT=$(timeout 180 "$NAAB" p.naab 2>&1) || RC=$?
    stop_stub 2>/dev/null
    cd "$W"
}

# prog_case ID GOVERN_EXTRA PROGRAM_TEXT -> OUT, RC
prog_case() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{ %s %s }\n' "$BASE" "$2" > "$d/govern.json"
    printf '%s' "$3" > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 180 "$NAAB" run p.naab 2>&1) || RC=$?
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
brief() { grep -m2 -E 'DELIVERED|CAUGHT|Error|error' <<<"$OUT" | head -c 240 | tr '\n' ' '; }

echo "=== Long input through every regex (A89) ==="

CONTRACT=', "output_contract": {"format": "json", "regex_checks": {"a": "^[a-z ]+$"}}'
agent_case lr00 '{\"a\": \"abc\"}' '' "$CONTRACT"
if [ "$OUT" = "STUBFAIL" ]; then
    unm LR-00 "the stub did not start -- the agent arms cannot be measured"
    STUB_OK=0
elif [ "$RC" -eq 0 ] && has "DELIVERED len=12"; then
    pass LR-00 "CONTROL: the stub answers and a short field is delivered"
    STUB_OK=1
else
    fail LR-00 "a short field should be delivered" "rc=$RC $(brief)"
    STUB_OK=0
fi

if [ "$STUB_OK" = 1 ]; then
    agent_case lr01 "{\\\"a\\\": \\\"$LONG\\\"}" '' "$CONTRACT"
    if [ "$RC" -eq 0 ] && has "DELIVERED len=60009"; then
        pass LR-01 "a 60,000-byte field under ^[a-z ]+\$ is delivered"
    else
        fail LR-01 "the long matching field should be delivered" "rc=$RC $(brief)"
    fi
    agent_case lr01c "{\\\"a\\\": \\\"$LONG!\\\"}" '' "$CONTRACT"
    if has "CAUGHT=" && has "does not match pattern" && ! has "DELIVERED"; then
        pass LR-01c "the same field ending in '!' is a contract violation"
    else
        fail LR-01c "a long field that breaks the pattern at its end must not be delivered" "rc=$RC $(brief)"
    fi

    PII=', "code_quality": {"no_pii": {"enabled": true, "level": "hard"}}'
    agent_case lr02 "$LONG" "$PII" ''
    if [ "$RC" -eq 0 ] && has "DELIVERED len=60000"; then
        pass LR-02 "no_pii scans a 60,000-byte reply and delivers it"
    else
        fail LR-02 "the long clean reply should be delivered" "rc=$RC $(brief)"
    fi
    agent_case lr02c "$LONG $SSN" "$PII" ''
    if [ "$RC" -eq 3 ] && ! has "DELIVERED"; then
        pass LR-02c "an SSN at the end of the long reply is refused (exit 3)"
    else
        fail LR-02c "PII at the end of a long reply must be refused" "rc=$RC $(brief)"
    fi
else
    for a in LR-01 LR-01c LR-02 LR-02c; do unm "$a" "no stub"; done
fi

PII_ONLY=', "code_quality": {"no_pii": {"enabled": true, "level": "hard"}}'
prog_case lr03 "$PII_ONLY" "$(printf 'main {\n    let s = "%s"\n    print("RAN len=" + string(len(s)))\n}\n' "$LONG")"
if [ "$RC" -eq 0 ] && has "RAN len=60000"; then
    pass LR-03 "a program holding a 60,000-byte literal runs under no_pii"
else
    fail LR-03 "the program should run" "rc=$RC $(brief)"
fi
prog_case lr03c "$PII_ONLY" "$(printf 'main {\n    let s = "%s %s"\n    print("RAN len=" + string(len(s)))\n}\n' "$LONG" "$SSN")"
if [ "$RC" -eq 3 ] && ! has "RAN len="; then
    pass LR-03c "an SSN at the end of the long literal is refused before the program runs"
else
    fail LR-03c "PII at the end of a long literal must be refused" "rc=$RC $(brief)"
fi

prog_case lr04 '' "$(printf 'use orchestra\nmain {\n    let r = orchestra.enforce_convergence("%s", {pattern: "^[a-z ]+$"})\n    print("VALID=" + string(r.get("valid")))\n}\n' "$LONG")"
if [ "$RC" -eq 0 ] && has "VALID=true"; then
    pass LR-04 "enforce_convergence with a program-supplied pattern accepts 60,000 bytes"
else
    fail LR-04 "the long response should be valid" "rc=$RC $(brief)"
fi
prog_case lr04c '' "$(printf 'use orchestra\nmain {\n    let r = orchestra.enforce_convergence("%s!", {pattern: "^[a-z ]+$"})\n    print("VALID=" + string(r.get("valid")))\n}\n' "$LONG")"
if [ "$RC" -eq 0 ] && has "VALID=false"; then
    pass LR-04c "the same response ending in '!' is not valid"
else
    fail LR-04c "a long response that breaks the pattern at its end must be invalid" "rc=$RC $(brief)"
fi

prog_case lr05 '' "$(printf 'use regex\nmain {\n    print("M=" + string(regex.matches("%s", "^[a-z ]+$")))\n}\n' "$LONG")"
if [ "$RC" -eq 0 ] && has "M=true"; then
    pass LR-05 "regex.matches is true on 60,000 bytes"
else
    fail LR-05 "regex.matches should be true" "rc=$RC $(brief)"
fi
prog_case lr05c '' "$(printf 'use regex\nmain {\n    print("M=" + string(regex.matches("%s!", "^[a-z ]+$")))\n}\n' "$LONG")"
if [ "$RC" -eq 0 ] && has "M=false"; then
    pass LR-05c "regex.matches is false when the last byte breaks the pattern"
else
    fail LR-05c "regex.matches should be false" "rc=$RC $(brief)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
