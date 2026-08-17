#!/usr/bin/env bash
# ============================================================
# test_signal_evaluability.sh — "did not fire" vs "could not fire"
#
# A silent signal had one representation in telemetry regardless of why. The
# campaign paid for that repeatedly: S12 inert because its baseline was never
# set, S17 inert unless adaptive baselining is on, S23 inert because it ships
# off, S20 inert without mandate keywords. In every case the silence was read as
# "nothing detected".
#
# S9 had already solved this for itself — thinking_unreported_streak plus a
# one-shot event — and the concept existed in exactly that one place. CDD_TURN
# now carries signals_off and signals_starved, and a one-shot SIGNAL_INERT event
# fires when a signal has been inert for a sustained streak.
#
# THREE STATES, AND SE-05 IS WHY
#
# Only preconditions that are a pure function of accumulated state are
# instrumented. Per-event conditions live inside per-event loops, and deriving
# them a second time would duplicate parsing that could diverge from the real
# gate. Those signals are absent from BOTH lists: their evaluability is unknown,
# not clean. SE-05 pins that scope_creep and vocabulary_contraction — whose gate
# input (turn_types) is built further down the function — are reported as neither
# off nor starved. Without SE-05 the honest three-state model could silently
# collapse into a two-state one that claims every uninstrumented signal is fine.
#
# SE-03 is the other load-bearing control: a signal that DOES have its inputs
# must appear in neither list. Without it, an implementation that lists every
# signal as starved passes SE-01.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_signal_evaluability.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/sigeval-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup() { stop_stub; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

if [ ! -x "$NAAB" ]; then
    skip "SE-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Signal evaluability: silent, or unable to speak?             |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

W="$TEST_TMP/run"; mkdir -p "$W"
python3 -c "
import json
json.dump({'responses':[{'content':'inventory report section %d with records and totals'%i,
                         'output_tokens':40} for i in range(14)]},
          open('$W/fixture.json','w'))"
start_stub "$W/fixture.json" "$W" || { skip "SE-00" "stub failed to start"; exit 0; }

# response_degenerate is switched OFF explicitly, giving SE-02 a known "disabled".
# No tools, no validation, no plan and a single instruction: several signals are
# enabled but structurally starved of inputs.
cat > "$W/govern.json" <<GOVEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "t.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "check_interval_turns": 1,
                     "signals": { "response_degenerate": false } },
  "agents": { "w": { "provider": "gemini", "model": "m",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "K",
      "max_tokens": 100, "max_turns": 30,
      "system_prompt": "Report on the inventory service." } } }
GOVEOF
cat > "$W/t.naab" <<'NAABEOF'
use agent
main {
    let h = agent.create("w")
    let i = 0
    while i < 13 { let r = agent.send(h, "continue"); i = i + 1 }
    print("DONE")
}
NAABEOF
(cd "$W" && K=x "$NAAB" t.naab > out.txt 2> err.txt)
stop_stub

Q() { python3 - "$W/t.jsonl" "$@" <<'PY'
import json,sys,collections
rows=[json.loads(l) for l in open(sys.argv[1]) if l.strip()]
mode=sys.argv[2]
cdd=[r for r in rows if r.get("event_type")=="CDD_TURN" and str(r.get("analyzed"))=="true"]
if mode=="off":      print((cdd[len(cdd)//2].get("signals_off") or "") if cdd else "NOCDD")
elif mode=="starved":print((cdd[len(cdd)//2].get("signals_starved") or "") if cdd else "NOCDD")
elif mode=="inertmax":
    c=collections.Counter(r.get("signal") for r in rows if r.get("event_type")=="SIGNAL_INERT")
    print(max(c.values()) if c else 0)
elif mode=="inertcount":
    print(sum(1 for r in rows if r.get("event_type")=="SIGNAL_INERT"))
elif mode=="reason":
    for r in rows:
        if r.get("event_type")=="SIGNAL_INERT" and r.get("signal")==sys.argv[3]:
            print(r.get("reason")); break
    else: print("ABSENT")
PY
}

OFF=$(Q off); STARVED=$(Q starved)

# --- SE-01: a starved signal is named ------------------------------------
if echo "$STARVED" | grep -q "tool_chain_integrity"; then
    pass "SE-01" "signal with no inputs is reported starved"
else
    fail "SE-01" "starved signal not reported" "signals_starved='$STARVED'"
fi

# --- SE-02: a disabled signal is named, with reason 'disabled' ------------
if echo "$OFF" | grep -q "response_degenerate" && [ "$(Q reason response_degenerate)" = "disabled" ]; then
    pass "SE-02" "config-disabled signal reported as off/disabled"
else
    fail "SE-02" "disabled signal not reported correctly" \
         "signals_off='$OFF' reason='$(Q reason response_degenerate)'"
fi

# --- SE-03: a signal WITH inputs is in neither list (control) -------------
# mandate_alignment has mandate_keywords, because system_prompt is set.
if ! echo "$OFF" | grep -q "mandate_alignment" && ! echo "$STARVED" | grep -q "mandate_alignment"; then
    pass "SE-03" "signal with inputs appears in neither list (control)"
else
    fail "SE-03" "a signal that had its inputs was reported inert" \
         "everything is being listed; SE-01 proves nothing"
fi

# --- SE-04: SIGNAL_INERT is one-shot per signal ---------------------------
MAXN=$(Q inertmax)
if [ "$MAXN" = "1" ]; then
    pass "SE-04" "SIGNAL_INERT fires once per signal ($(Q inertcount) events, max 1 each)"
elif [ "$MAXN" = "0" ]; then
    fail "SE-04" "no SIGNAL_INERT events emitted at all"
else
    fail "SE-04" "SIGNAL_INERT repeated (max $MAXN for one signal)" \
         "the announce mask is latching instead of firing once"
fi

# --- SE-05: uninstrumented signals are absent from BOTH lists -------------
if echo "$OFF$STARVED" | grep -qE "scope_creep|vocabulary_contraction"; then
    fail "SE-05" "an uninstrumented signal was classified" \
         "scope_creep/vocabulary_contraction gate on turn_types, which is not checked here"
else
    pass "SE-05" "uninstrumented signals reported as neither (unknown, not clean)"
fi

# --- SE-07: a disabled signal with NO instrumented precondition ----------
# context_growth is disabled per-agent below and has no traced input
# precondition, so it exercises the exact gap a live run exposed: the OFF half
# was filtered through the instrumented mask, hiding genuinely disabled signals
# in NEITHER list. Being switched off is knowable for every signal.
run_case_off_uninstrumented() {
    local w="$TEST_TMP/offuninst"; mkdir -p "$w"
    python3 -c "
import json
json.dump({'responses':[{'content':'inventory report section %d with records'%i,
                         'output_tokens':40} for i in range(14)]},
          open('$w/fixture.json','w'))"
    start_stub "$w/fixture.json" "$w" || return 1
    cat > "$w/govern.json" <<GOVEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "t.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "check_interval_turns": 1 },
  "agents": { "w": { "provider": "gemini", "model": "m",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "K",
      "max_tokens": 100, "max_turns": 30,
      "context_drift_signals": { "context_growth": false },
      "system_prompt": "Report on the inventory service." } } }
GOVEOF
    cat > "$w/t.naab" <<'NAABEOF'
use agent
main { let h = agent.create("w"); let i = 0
  while i < 12 { let r = agent.send(h, "continue"); i = i + 1 }
  print("DONE") }
NAABEOF
    (cd "$w" && K=x "$NAAB" t.naab > out.txt 2>&1)
    stop_stub
    grep '"event_type":"CDD_TURN"' "$w/t.jsonl" 2>/dev/null | tail -3 | head -1 \
        | grep -o '"signals_off":"[^"]*"' | sed 's/.*":"//; s/"$//'
}
OFF_U=$(run_case_off_uninstrumented)
if echo "$OFF_U" | grep -q "context_growth"; then
    pass "SE-07" "per-agent disabled signal reported off even without an instrumented precondition"
else
    fail "SE-07" "disabled context_growth absent from signals_off" \
         "got '$OFF_U' — the off half is being filtered through the instrumented mask"
fi

# --- SE-06: config keys, not telemetry labels ----------------------------
# The four divergent signals must appear under their govern.json spelling.
if echo "$OFF$STARVED" | grep -qE "vocab_contraction|capability_underutil\b|contradictions\b"; then
    fail "SE-06" "telemetry labels used instead of config keys" \
         "a reader copying these into govern.json would disable nothing"
else
    pass "SE-06" "names are govern.json config keys"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
