#!/usr/bin/env bash
# ============================================================
# test_persona_adaptive.sh — C1e: the S17 persona baseline may follow an agent
#                            that is otherwise behaving
#
# THE MEASURED PROBLEM (#176)
#
# persona_baseline_mean is set ONCE at baseline completion, from as few as three
# early samples, and never updated. Observed on the test_coherence_recovery.sh
# fixture: baseline fixed at turn 6 as mean 7.167 / stddev 1.863 -- a firing band
# of "below 3.4 or above 10.9 keywords" -- taken from five TERSE warm-up turns.
# The agent's later, richer, entirely correct responses cleared 10.9, and it paid
# persona_fingerprint=0.0500 EVERY TURN for the rest of the run. The deviation
# test is abs() on a SINGLE response, so producing MORE scores identically to
# collapsing.
#
# WHY NOT THE TWO OBVIOUS FIXES
#
# Directional-only (fire on collapse, not growth) blinds S17 to an agent that
# starts rambling, which is a real drift mode. Naive re-baselining lets sustained
# drift quietly become the new normal -- the exact failure that keeps S9's
# baseline deliberately frozen.
#
# WHAT THIS DOES INSTEAD
#
# The baseline is re-derived from the rolling window at the end of any turn where
# NO OTHER signal fired. Adaptation is gated on independent evidence of health:
# a degrading agent trips other signals, which freezes the baseline and leaves
# S17 paying. Excluding S17 from its own gate is load-bearing -- while S17 is the
# only thing firing no turn is ever "clean", so a baseline gated on cleanliness
# could never follow, and the signal would hold its own baseline hostage.
#
# Ships OFF (context_drift.thresholds.persona_baseline_adaptive, default false).
# Enabling it strictly reduces S17 firings, so it is ratcheted.
#
# RESIDUAL RISK, STATED NOT HIDDEN
#
# An agent whose ONLY symptom is slow persona drift, with no other signal ever
# firing, would be followed by the baseline. That is the boiling-frog case, it is
# why this ships off, and it is why PF-04 exists.
#
#   PF-01  default off: absent key is byte-identical to explicit false
#   PF-02  CONTROL: with the flag OFF the permanent firing reproduces
#   PF-03  with the flag ON, S17 goes quiet once the agent is otherwise clean
#   PF-04  POSITIVE CONTROL: a genuine shift WITH other signals firing is STILL
#          caught -- without this, PF-03 is indistinguishable from "S17 disabled"
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/personaad-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_persona_adaptive.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_PA="fake-key-persona-adaptive"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# 5 TERSE warm-up turns (set a low baseline), 10 DRIFT turns (other signals fire
# AND keyword count shifts -> S17 must still be caught: PF-04), then 25 RICH but
# on-mandate turns (clean apart from S17 -> baseline should follow: PF-03).
gen_fixture() {
    python3 - "$1" <<'PYEOF'
import json, sys
terse = "Ledger reconcile part %d done."
topics = ["photography lenses","mountain weather","pasta recipes","jazz history","bicycle gears",
          "tide tables","volcano types","origami folds","desert beetles","harbour cranes"]
verbs = ["recomputed","verified","reconciled","audited","closed"]
r = [{"content": terse % i, "output_tokens": 12, "thinking_tokens": 8} for i in range(5)]
for i, t in enumerate(topics):
    r.append({"content": "Consider %s." % t, "output_tokens": max(6, 18 - i), "thinking_tokens": 0})
for i in range(25):
    r.append({"content": "Ledger reconcile resumed: quarterly totals %s and the balance "
                         "recorded against source ledger section %d with variance noted."
                         % (verbs[i % 5], i), "output_tokens": 48, "thinking_tokens": 20})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# $1=name $2=thresholds json fragment
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl", "decision_snapshots": true },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_window": 5,
    "thresholds": { $2 },
    "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true, "critical_threshold": 0.99 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_PA", "max_tokens": 200, "max_turns": 60,
      "system_prompt": "You reconcile ledger data and report quarterly totals." } }
}
EOF
    (cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat > "$WDIR/t.naab" <<'EOF'
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < 40 { let r = agent.send(h, "continue the ledger reconciliation") i = i + 1 }
    print("DONE")
}
EOF
    (cd "$WDIR" && timeout 300s "$NAAB" t.naab > out.txt 2> err.txt) || true
    stop_stub
    return 0
}

# "$1"=telemetry  "$2"=first turn  "$3"=last turn -> count of turns S17 fired
s17_fires() {
    python3 - "$1" "$2" "$3" <<'PYEOF'
import json, sys
lo, hi, n = int(sys.argv[2]), int(sys.argv[3]), 0
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") != "CDD_TURN" or d.get("analyzed") != "true": continue
    try: t = int(d.get("turn"))
    except Exception: continue
    if not (lo <= t <= hi): continue
    if "persona_fingerprint" in (d.get("penalties_detail") or ""): n += 1
print(n)
PYEOF
}
final_coh() { python3 - "$1" <<'PYEOF'
import json, sys
last = ""
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") == "CDD_TURN" and d.get("analyzed") == "true": last = d.get("coherence")
print(last)
PYEOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  C1e — adaptive persona baseline (S17)                        |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

ON='"persona_baseline_adaptive": true'
OFF='"persona_baseline_adaptive": false'

run_case off_absent ""    && T_ABSENT="$TEST_TMP/off_absent/tele.jsonl"
run_case off_expl   "$OFF" && T_OFF="$TEST_TMP/off_expl/tele.jsonl"
run_case on_adapt   "$ON"  && T_ON="$TEST_TMP/on_adapt/tele.jsonl"

# ---------- PF-01: the default is OFF ----------
echo -e "${CYAN}--- PF-01: default off ---${NC}"
A_LATE=$(s17_fires "${T_ABSENT:-/dev/null}" 16 40 2>/dev/null)
O_LATE=$(s17_fires "${T_OFF:-/dev/null}" 16 40 2>/dev/null)
A_COH=$(final_coh "${T_ABSENT:-/dev/null}" 2>/dev/null)
O_COH=$(final_coh "${T_OFF:-/dev/null}" 2>/dev/null)
if [ -n "$A_LATE" ] && [ "$A_LATE" = "$O_LATE" ] && [ "$A_COH" = "$O_COH" ]; then
    pass "PF-01" "absent is byte-identical to explicit false (S17 fired ${A_LATE}x late, coherence $A_COH)"
else
    fail "PF-01" "absent and explicit false differ — the default is not what it appears" \
         "absent=${A_LATE}/${A_COH} explicit=${O_LATE}/${O_COH}"
fi

# ---------- PF-02: CONTROL — the bug reproduces with the flag off ----------
echo -e "${CYAN}--- PF-02: CONTROL — permanent firing reproduces when OFF ---${NC}"
if [ -n "$O_LATE" ] && [ "$O_LATE" -ge 15 ]; then
    pass "PF-02" "flag OFF: S17 fired on ${O_LATE} of the 25 post-drift turns — the defect is present"
    CONTROL_OK=1
else
    fail "PF-02" "the defect did not reproduce with the flag OFF — PF-03 would prove nothing" \
         "S17 fired only ${O_LATE:-?}x on turns 16-40"
    CONTROL_OK=0
fi

# ---------- PF-03: the flag quiets S17 once the agent is otherwise clean ----------
echo -e "${CYAN}--- PF-03: baseline follows an otherwise-clean agent ---${NC}"
N_LATE=$(s17_fires "${T_ON:-/dev/null}" 16 40 2>/dev/null)
if [ "$CONTROL_OK" = "1" ]; then
    if [ -n "$N_LATE" ] && [ "$N_LATE" -lt "$O_LATE" ] && [ "$N_LATE" -le 5 ]; then
        pass "PF-03" "flag ON: S17 fired ${N_LATE}x on turns 16-40 (was ${O_LATE}x) — the baseline followed"
    else
        fail "PF-03" "the baseline did not follow the agent" "ON=${N_LATE:-?} OFF=${O_LATE}"
    fi
else
    skip "PF-03" "control failed — a quiet result here would mean nothing"
fi

# ---------- PF-04: POSITIVE CONTROL — detection preserved ----------
echo -e "${CYAN}--- PF-04: POSITIVE CONTROL — a genuine shift is still caught ---${NC}"
# Turns 6-15 are the drift phase: OTHER signals fire there, so the baseline must
# stay frozen and S17 must still catch the keyword-count shift. If this is 0, the
# fix did not make S17 adaptive — it made it blind, and PF-03 is worthless.
N_DRIFT=$(s17_fires "${T_ON:-/dev/null}" 6 15 2>/dev/null)
O_DRIFT=$(s17_fires "${T_OFF:-/dev/null}" 6 15 2>/dev/null)
if [ -n "$N_DRIFT" ] && [ "$N_DRIFT" -ge 1 ]; then
    pass "PF-04" "flag ON still caught the shift during drift: S17 fired ${N_DRIFT}x on turns 6-15 (OFF: ${O_DRIFT}x)"
else
    fail "PF-04" "S17 no longer fires during genuine drift — the fix blinded it rather than adapting it" \
         "ON drift-phase fires=${N_DRIFT:-?}, OFF=${O_DRIFT:-?}"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Total: $((PASS_COUNT+FAIL_COUNT+SKIP_COUNT)) | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}FAILURES:${NC}$FAILURES"; exit 1; fi
echo -e "  test_persona_adaptive.sh: ALL PASSED"
exit 0
