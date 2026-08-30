#!/usr/bin/env bash
# ============================================================
# test_vocab_baseline.sh — S5 (vocabulary_contraction): a frozen baseline taken
#                          from three turns, and a turn bucket that goes stale
#
# WHAT S5 ACTUALLY MEASURES
#
# `turn_types` is built from RuntimeEventType (ER/EW/FR/FW/NC/SX/AS/AR/EN/DE/
# PX/RA), NOT from response vocabulary. The signal's name is misleading: it is
# ACTION-TYPE diversity. Nothing an agent says can move it.
#
# DEFECT 1 — the baseline is frozen, and it is sampled from three turns
#
# `initial_entropy` is set ONCE, from the FIRST HALF of the sliding window the
# first time that window reaches `vocab_contraction_window` (6) turns — i.e.
# from turns 0-2 — and is never updated. It is the earliest-set of the four
# frozen baselines (S5/S9/S12/S17): it is fixed BEFORE `baseline_complete`, and
# it is the only one of the four not exposed in `snapshotState()`.
#
# A pure `agent.send()` loop is {AS, AR} forever: entropy 1.0, and 1.0 < 0.6 is
# false, so S5 can never fire. Add k distinct non-agent event types in turn 0
# alone and the frozen baseline rises:
#
#     k=1   initial_entropy 1.449   threshold 0.869   1.0 > 0.869  -> silent
#     k=2   initial_entropy 1.811   threshold 1.087   1.0 < 1.087  -> FIRES
#
# and once it fires it fires every remaining turn, because the baseline it is
# compared against never moves. Two lines of ordinary startup code — an env read
# and a base64 call before the first send — is the whole trip.
#
# Adaptive baselining does NOT absorb it: S5 never fires DURING the baseline
# window, so its expected rate is 0 and every later firing pays full weight.
#
# DEFECT 2 — action diversity after turn 0 is invisible to CDD
#
# `ev.turn` is stamped from `current_agent_turn_` (governance_engine.cpp), which
# is written only by `setAgentContext()` at the START of each send. An event
# raised BETWEEN send N and send N+1 therefore carries turn N, whose bucket
# `checkContextDrift()` already consumed during send N. In a no-tools script the
# only live bucket is turn 0's. Consequences, both measured below: an agent that
# performs varied actions before EVERY send is charged identically to one that
# did two ops once at startup (VC-05), and diversity introduced later is not
# merely uncredited but wholly invisible (VC-06).
#
# WHAT THESE GATES DO AND DO NOT ASSERT
#
# They pin CURRENT, DEFECTIVE behaviour so that a fix shows up as a deliberate
# gate change rather than as noise. They do NOT assert this behaviour is
# correct. VC-01/VC-02/VC-03/VC-05/VC-06 are all expected to change when either
# defect is fixed; VC-00 and VC-04's control half are the ones that should hold
# either way.
#
#   VC-00  CONTROL: a pure send loop never fires S5 and holds coherence 1.0.
#          Everything downstream skips if this fails — without it, "the setup
#          arm fires" cannot be distinguished from a fixture that fires always.
#   VC-01  k=1 (one extra event type at startup) stays silent — the lower half
#          of the trip point. Without it VC-02 does not locate a boundary.
#   VC-02  k=2 fires: two event types before the first send is the trip point.
#   VC-03  ...and keeps firing, because the baseline never re-derives.
#   VC-04  the cost: the k=2 arm floors coherence while the control stays at 1.
#   VC-05  varied actions before EVERY send are charged the same as two ops at
#          startup — sustained diversity earns nothing.
#   VC-06  diversity introduced from turn 10 changes NOTHING in CDD output; the
#          arm is signal-identical to the control that never did any of it.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab-vocab-baseline-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_vocab_baseline.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_VC="fake-key-vocab-baseline"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

TURNS=25

# One uniform response stream for every arm. The arms differ ONLY in what the
# orchestration script does around the sends -- never in what the agent says.
gen_fixture() {
    python3 - "$1" <<'PYEOF'
import json, sys
r = [{"content": "Reconcile step %d: quarterly totals verified against the source "
                 "ledger section and the variance recorded for later review." % i,
      "output_tokens": 40, "thinking_tokens": 15} for i in range(40)]
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# $1=arm name  $2=full NAAb program
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true, "critical_threshold": 0.99 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_VC", "max_tokens": 200, "max_turns": 40,
      "system_prompt": "You reconcile ledger data and report quarterly totals." } }
}
EOF
    (cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    printf '%s\n' "$2" > "$WDIR/t.naab"
    (cd "$WDIR" && timeout 300s "$NAAB" t.naab > out.txt 2> err.txt) || true
    stop_stub
    [ -f "$WDIR/tele.jsonl" ]
}

# k=1: one extra event type (ENV_READ).  k=2: adds ENCODE.
OPS_K1='    let _a = env.get("HOME") ?? "x"'
OPS_K2='    let _a = env.get("HOME") ?? "x"
    let _b = crypto.base64_encode("d")'

mk_prog() {  # $1 = ops emitted before the loop, $2 = ops emitted inside the loop
    printf 'use agent\nuse env\nuse crypto\nmain {\n%s\n    let h = agent.create("worker")\n    let i = 0\n    while i < %d {\n%s\n        let r = agent.send(h, "continue the reconciliation")\n        i = i + 1\n    }\n    print("DONE")\n}\n' "$1" "$TURNS" "$2"
}

mk_prog_late() {  # varied ops only from iteration 10 onward
    printf 'use agent\nuse env\nuse crypto\nmain {\n    let h = agent.create("worker")\n    let i = 0\n    while i < %d {\n        if i >= 10 {\n%s\n        }\n        let r = agent.send(h, "continue the reconciliation")\n        i = i + 1\n    }\n    print("DONE")\n}\n' "$TURNS" "$OPS_K2"
}

# Turns on which S5 fired, from CDD_TURN penalties/signals detail.
s5_fire_turns() {
python3 - "$1" <<'PYEOF'
import json, sys
out=[]
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e=json.loads(line)
    except Exception: continue
    d=e.get("fields",e)
    if d.get("event_type")!="CDD_TURN" or d.get("analyzed")!="true": continue
    blob=(d.get("penalties_detail") or "")+"|"+(d.get("signals_detail") or "")
    if "vocab_contraction" in blob: out.append(int(d.get("turn")))
print(" ".join(str(t) for t in sorted(out)))
PYEOF
}

final_coherence() {
python3 - "$1" <<'PYEOF'
import json, sys
last=None
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e=json.loads(line)
    except Exception: continue
    d=e.get("fields",e)
    if d.get("event_type")=="CDD_TURN" and d.get("analyzed")=="true": last=d.get("coherence")
print(last if last is not None else "NONE")
PYEOF
}

# Per-turn "turn:penalties_detail" fingerprint, for arm-to-arm identity checks.
signal_trace() {
python3 - "$1" <<'PYEOF'
import json, sys
rows={}
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e=json.loads(line)
    except Exception: continue
    d=e.get("fields",e)
    if d.get("event_type")!="CDD_TURN" or d.get("analyzed")!="true": continue
    rows[int(d.get("turn"))]=(d.get("penalties_detail") or "")
for t in sorted(rows): print("%d:%s" % (t, rows[t]))
PYEOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  S5 vocabulary_contraction — frozen baseline + stale bucket   |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

OK_STEADY=0; OK_K1=0; OK_K2=0; OK_EVERY=0; OK_LATE=0
run_case steady      "$(mk_prog ''          '')"        && OK_STEADY=1
run_case k1_startup  "$(mk_prog "$OPS_K1"   '')"        && OK_K1=1
run_case k2_startup  "$(mk_prog "$OPS_K2"   '')"        && OK_K2=1
run_case k2_everyturn "$(mk_prog ''         "$OPS_K2")" && OK_EVERY=1
run_case k2_late     "$(mk_prog_late)"                  && OK_LATE=1

T_STEADY="$TEST_TMP/steady/tele.jsonl"
T_K1="$TEST_TMP/k1_startup/tele.jsonl"
T_K2="$TEST_TMP/k2_startup/tele.jsonl"
T_EVERY="$TEST_TMP/k2_everyturn/tele.jsonl"
T_LATE="$TEST_TMP/k2_late/tele.jsonl"

# ---------- VC-00: the control, and the vacuity guard ----------
echo -e "${CYAN}--- VC-00: CONTROL — a pure send loop never fires S5 ---${NC}"
CONTROL_OK=0
if [ "$OK_STEADY" != "1" ]; then
    fail "VC-00" "control arm produced no telemetry"
else
    SF="$(s5_fire_turns "$T_STEADY")"; SC="$(final_coherence "$T_STEADY")"
    if [ -z "$SF" ] && [ "$SC" = "1.0000" ]; then
        pass "VC-00" "control: S5 fired 0 times, final coherence $SC"
        CONTROL_OK=1
    else
        fail "VC-00" "control is contaminated" "S5 turns=[$SF] final_coherence=$SC (want none / 1.0000)"
    fi
fi

# ---------- VC-01 / VC-02: the trip point ----------
echo -e "${CYAN}--- VC-01/VC-02: two event types at startup is the boundary ---${NC}"
if [ "$CONTROL_OK" != "1" ]; then
    skip "VC-01" "control failed — a firing count means nothing"
    skip "VC-02" "control failed — a firing count means nothing"
    K2_OK=0
else
    K1F="$(s5_fire_turns "$T_K1")"; K2F="$(s5_fire_turns "$T_K2")"
    K1N=$(echo $K1F | wc -w); K2N=$(echo $K2F | wc -w)
    if [ "$OK_K1" = "1" ] && [ "$K1N" -eq 0 ]; then
        pass "VC-01" "k=1 (one extra event type) stays silent"
    else
        fail "VC-01" "k=1 fired" "turns=[$K1F] — the boundary has moved; recompute initial_entropy"
    fi
    if [ "$OK_K2" = "1" ] && [ "$K2N" -gt 0 ]; then
        pass "VC-02" "k=2 fires — $K2N turns, first at $(echo $K2F | cut -d' ' -f1)"
        K2_OK=1
    else
        fail "VC-02" "k=2 did not fire" "the fixture no longer exercises S5; everything below is vacuous"
        K2_OK=0
    fi
fi

# ---------- VC-03: it never stops, because the baseline never moves ----------
echo -e "${CYAN}--- VC-03: the frozen baseline never re-derives ---${NC}"
if [ "${K2_OK:-0}" != "1" ]; then
    skip "VC-03" "VC-02 did not establish a firing arm"
else
    K2N=$(echo "$(s5_fire_turns "$T_K2")" | wc -w)
    if [ "$K2N" -ge 15 ]; then
        pass "VC-03" "k=2 fires on $K2N of $TURNS turns (>=15)"
    else
        fail "VC-03" "firing is not persistent" "$K2N of $TURNS turns — a re-deriving baseline would look like this"
    fi
fi

# ---------- VC-04: what it costs ----------
echo -e "${CYAN}--- VC-04: coherence cost against the control ---${NC}"
if [ "${K2_OK:-0}" != "1" ]; then
    skip "VC-04" "VC-02 did not establish a firing arm"
else
    C_CTL="$(final_coherence "$T_STEADY")"; C_K2="$(final_coherence "$T_K2")"
    if [ "$C_CTL" = "1.0000" ] && [ "$C_K2" = "0.0000" ]; then
        pass "VC-04" "control $C_CTL vs k=2 $C_K2 on an identical response stream"
    else
        fail "VC-04" "cost differs from the measurement" "control=$C_CTL k2=$C_K2 (want 1.0000 / 0.0000)"
    fi
fi

# ---------- VC-05: sustained diversity earns nothing ----------
echo -e "${CYAN}--- VC-05: varied actions every turn are charged the same ---${NC}"
if [ "${K2_OK:-0}" != "1" ]; then
    skip "VC-05" "VC-02 did not establish a firing arm"
elif [ "$OK_EVERY" != "1" ]; then
    fail "VC-05" "every-turn arm produced no telemetry"
else
    EF="$(s5_fire_turns "$T_EVERY")"; EN=$(echo $EF | wc -w)
    K2F="$(s5_fire_turns "$T_K2")"
    if [ "$EN" -ge 15 ] && [ "$EF" = "$K2F" ]; then
        pass "VC-05" "acting varied every turn fires identically to two ops at startup ($EN turns)"
    else
        fail "VC-05" "the two arms diverged" "every_turn=[$EF] vs startup_only=[$K2F]"
    fi
fi

# ---------- VC-06: later diversity is invisible, not merely uncredited ----------
echo -e "${CYAN}--- VC-06: diversity from turn 10 changes nothing at all ---${NC}"
if [ "$CONTROL_OK" != "1" ]; then
    skip "VC-06" "control failed"
elif [ "$OK_LATE" != "1" ]; then
    fail "VC-06" "late arm produced no telemetry"
else
    A="$(signal_trace "$T_STEADY")"; B="$(signal_trace "$T_LATE")"
    if [ "$A" = "$B" ]; then
        pass "VC-06" "introducing 3 new event types at turn 10 left every CDD turn unchanged"
    else
        fail "VC-06" "the late arm differs from the control" "$(diff <(echo "$A") <(echo "$B") | head -4 | tr '\n' ' ')"
    fi
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Passed: ${GREEN}$PASS_COUNT${NC}  Failed: ${RED}$FAIL_COUNT${NC}  Skipped: ${YELLOW}$SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}$FAILURES"
    echo -e "${CYAN}+==============================================================+${NC}"
    exit 1
fi
echo -e "${GREEN}ALL PASSED${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
exit 0
