#!/usr/bin/env bash
# ============================================================
# test_config_contradictions.sh — CONTRA-011 and CONTRA-012
#
# Two config shapes that cost keyed runs. Both are the shape
# detectContradictions() exists for: configured, believed working, structurally
# unable to do what the operator intended.
#
# CONTRA-011 — rotation keys without within-call failover.
#   The obvious phrasing ("rotation is inert") is WRONG and this suite pins the
#   correct one. key_offset advances on every send and persists on the tracker,
#   so keys DO rotate between calls even at max_attempts=1, and a key returning a
#   skip_key_on code is still retired on the first attempt. What is lost is
#   failover WITHIN a call: the first key error aborts the call that hit it,
#   despite N healthy keys being configured.
#
# CONTRA-012 — context_growth that can never recover.
#   S12's input-token baseline is set once and only EMA-updated when
#   adaptive_baseline_enabled is true. With no context_window the history is
#   resent every turn, so input tokens climb monotonically; once past the factor
#   the signal fires every remaining turn against a frozen baseline.
#
# WHY THE NEGATIVE FIXTURES ARE THE POINT
#
# CC-02, CC-04, CC-05 and CC-06 are configs that must produce SILENCE. Without
# them, a pattern hardcoded to fire on every config passes CC-01 and CC-03. The
# measured fire rate across the in-tree corpus is 17/190 agents for CONTRA-011
# and 36/190 for CONTRA-012 — targeted, not universal — and these fixtures are
# what keep it that way.
#
# Level: both use contradiction_detection.max_level, which defaults to ADVISORY.
# detectContradictions() runs once per process from loadFromFile, and
# reloadIfChanged() calls loadFromJson instead — so these cannot repeat, and
# advisory escalation (off by default, 3 occurrences of one rule) is unreachable.
# CC-07 pins that a contradicting config still RUNS.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/contra-$$"

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
    skip "CC-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Config contradictions: CONTRA-011 / CONTRA-012               |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# $1 = case name, $2 = agents-block JSON, $3 = extra top-level JSON (may be empty)
emit() {
    local w="$TEST_TMP/$1"; mkdir -p "$w"
    local extra="$3"
    cat > "$w/govern.json" <<GOVEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" }${extra:+,}
  $extra
  , "agents": { $2 } }
GOVEOF
    echo 'main { print("RAN") }' > "$w/t.naab"
    (cd "$w" && "$NAAB" t.naab > out.txt 2> err.txt)
    cat "$w/err.txt"
}

# --- CONTRA-011 ------------------------------------------------------------
# Positive: 3 rotation keys, no retry.
OUT=$(emit c11_pos '"w": { "provider": "gemini", "model": "m",
      "api_key_env": ["K1","K2","K3"], "retry": { "max_attempts": 1 },
      "max_tokens": 50, "max_turns": 5, "system_prompt": "x" }' "")
if echo "$OUT" | grep -q "CONTRA-011"; then
    pass "CC-01" "rotation keys without within-call failover is reported"
else
    fail "CC-01" "CONTRA-011 did not fire on 3 keys + max_attempts 1" "$(echo "$OUT" | head -2)"
fi

# Negative A: same 3 keys WITH retry — the operator's intent is satisfied.
OUT=$(emit c11_neg '"w": { "provider": "gemini", "model": "m",
      "api_key_env": ["K1","K2","K3"], "retry": { "max_attempts": 3 },
      "max_tokens": 50, "max_turns": 5, "system_prompt": "x" }' "")
if echo "$OUT" | grep -q "CONTRA-011"; then
    fail "CC-02" "CONTRA-011 fires even with retry configured" \
         "the pattern ignores max_attempts; CC-01 proves nothing"
else
    pass "CC-02" "retry configured → silent (negative fixture)"
fi

# Negative B: a single key and no retry is not a contradiction — nothing to fail over to.
OUT=$(emit c11_single '"w": { "provider": "gemini", "model": "m",
      "api_key_env": "K1", "retry": { "max_attempts": 1 },
      "max_tokens": 50, "max_turns": 5, "system_prompt": "x" }' "")
if echo "$OUT" | grep -q "CONTRA-011"; then
    fail "CC-04" "CONTRA-011 fires on a single key" \
         "no rotation was requested, so no intent is being defeated"
else
    pass "CC-04" "single key → silent (negative fixture)"
fi

# --- CONTRA-012 ------------------------------------------------------------
# Positive: long run, no window, adaptive baseline off.
#
# adaptive_baseline_enabled is PINNED here rather than inherited. CONTRA-012 is
# ABOUT that key being false — the check reads `if (adaptive_baseline_enabled)
# continue;` because the baseline tracks growth when it is on, so the
# unrecoverable combination cannot arise. A fixture that leaves it to the
# default is not testing the condition it names, it is testing what the default
# happens to be, and it went silent the moment that default moved.
OUT=$(emit c12_pos '"w": { "provider": "gemini", "model": "m", "api_key_env": "K",
      "max_tokens": 50, "max_turns": 40, "system_prompt": "x" }' \
      '"context_drift": { "enabled": true, "adaptive_baseline_enabled": false }')
if echo "$OUT" | grep -q "CONTRA-012"; then
    pass "CC-03" "unrecoverable context_growth combination is reported"
else
    fail "CC-03" "CONTRA-012 did not fire" "$(echo "$OUT" | head -2)"
fi

# Negative A: a context_window bounds the history, so growth cannot run away.
OUT=$(emit c12_win '"w": { "provider": "gemini", "model": "m", "api_key_env": "K",
      "max_tokens": 50, "max_turns": 40, "context_window": 20,
      "context_strategy": "recent", "system_prompt": "x" }' \
      '"context_drift": { "enabled": true }')
if echo "$OUT" | grep -q "CONTRA-012"; then
    fail "CC-05" "CONTRA-012 fires despite a context_window" \
         "the pattern ignores windowing; CC-03 proves nothing"
else
    pass "CC-05" "context_window set → silent (negative fixture)"
fi

# Negative B: a short run cannot reach the crossing.
OUT=$(emit c12_short '"w": { "provider": "gemini", "model": "m", "api_key_env": "K",
      "max_tokens": 50, "max_turns": 6, "system_prompt": "x" }' \
      '"context_drift": { "enabled": true }')
if echo "$OUT" | grep -q "CONTRA-012"; then
    fail "CC-06" "CONTRA-012 fires on a short run" \
         "it cannot reach the growth crossing; this is the noise guard"
else
    pass "CC-06" "short max_turns → silent (negative fixture)"
fi

# Negative C: per-agent override disabling the signal must be honoured.
OUT=$(emit c12_off '"w": { "provider": "gemini", "model": "m", "api_key_env": "K",
      "max_tokens": 50, "max_turns": 40, "system_prompt": "x",
      "context_drift_signals": { "context_growth": false } }' \
      '"context_drift": { "enabled": true }')
if echo "$OUT" | grep -q "CONTRA-012"; then
    fail "CC-08" "CONTRA-012 ignores a per-agent signal override" \
         "the signal is off for this agent, so it cannot fire at all"
else
    pass "CC-08" "per-agent signal override honoured (negative fixture)"
fi

# --- CC-07: advisory, not a block -----------------------------------------
if [ -f "$TEST_TMP/c11_pos/out.txt" ] && grep -q "RAN" "$TEST_TMP/c11_pos/out.txt"; then
    pass "CC-07" "a contradicting config still runs (ADVISORY, not a block)"
else
    fail "CC-07" "contradicting config did not run" \
         "these must not reach a blocking level"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
