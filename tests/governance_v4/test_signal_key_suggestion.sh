#!/usr/bin/env bash
# ============================================================
# test_signal_key_suggestion.sh — telemetry label typed as a config key
#
# Four CDD signals are spelled differently in telemetry than in govern.json:
#
#   config key (kCddSignalKeys)   telemetry label (signalName)
#   circular_actions              circular
#   intent_contradictions         contradictions
#   vocabulary_contraction        vocab_contraction
#   capability_underutilization   capability_underutil
#
# penalties_detail and signals_detail print the LABEL. context_drift_signals
# parses the KEY. So the name an operator is most likely to type -- the one they
# just watched firing -- disables NOTHING: the signal keeps firing and paying,
# and the only symptom is a single stderr line calling a visibly-working signal
# "unknown".
#
# The alias is deliberately NOT accepted (SK-04 pins that). A second spelling
# that silently worked would be config surface the ratchet comparison and the
# override bitmask do not know about. The warning names the canonical key
# instead, turning a dead end into the fix.
#
# SK-03 is the load-bearing control: a genuine typo must still get the PLAIN
# warning. Without it, a suggestion appended unconditionally to every unknown key
# would pass SK-01 and SK-02 while telling an operator with a real typo that
# their nonsense key is "the telemetry label".
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/sigkey-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

if [ ! -x "$NAAB" ]; then
    skip "SK-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  CDD signal key: telemetry label typed into govern.json        |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# No API key and no send -- config parsing is all this exercises.
emit() {
    local key="$1"
    cat > "$TEST_TMP/govern.json" <<GOVEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "agents": { "a": {
      "provider": "gemini", "model": "m", "api_key_env": "K",
      "max_tokens": 50, "system_prompt": "x",
      "context_drift_signals": { "$key": false } } } }
GOVEOF
    echo 'main { print("ok") }' > "$TEST_TMP/t.naab"
    (cd "$TEST_TMP" && "$NAAB" t.naab 2>&1 | grep -i "context_drift_signals" | head -2)
}

OUT=$(emit "vocab_contraction")
if echo "$OUT" | grep -q "vocabulary_contraction"; then
    pass "SK-01" "telemetry label 'vocab_contraction' suggests 'vocabulary_contraction'"
else
    fail "SK-01" "no canonical key suggested for a telemetry label" "$OUT"
fi

OUT=$(emit "capability_underutil")
if echo "$OUT" | grep -q "capability_underutilization"; then
    pass "SK-02" "telemetry label 'capability_underutil' suggests its config key"
else
    fail "SK-02" "no canonical key suggested" "$OUT"
fi

# Control: a real typo must NOT be told it is a telemetry label.
OUT=$(emit "vocab_contractionX")
if echo "$OUT" | grep -q "unknown context_drift_signals key" && \
   ! echo "$OUT" | grep -q "telemetry label"; then
    pass "SK-03" "genuine typo still gets the plain warning (control)"
elif ! echo "$OUT" | grep -q "unknown context_drift_signals key"; then
    fail "SK-03" "genuine typo produced no warning at all" "$OUT"
else
    fail "SK-03" "genuine typo wrongly described as a telemetry label" \
         "the suggestion is being appended unconditionally"
fi

# The alias must stay rejected, not silently start working.
OUT=$(emit "vocab_contraction")
if echo "$OUT" | grep -q "NOT overridden"; then
    pass "SK-04" "alias is refused, not silently accepted"
else
    fail "SK-04" "alias no longer refused — it may be silently accepted now" \
         "a second working spelling is config surface the ratchet cannot see"
fi

# The canonical key itself must stay silent.
OUT=$(emit "vocabulary_contraction")
if [ -z "$OUT" ]; then
    pass "SK-05" "canonical config key warns about nothing"
else
    fail "SK-05" "canonical key produced a warning" "$OUT"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
