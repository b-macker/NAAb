#!/usr/bin/env bash
# ============================================================
# test_coherence_recovery.sh — what an agent gets for recovering
#
# THE MECHANISM, NOT THE DEFAULT -- but only PARTLY insulated from it. An
# earlier version of this header claimed "every arm PINS
# coherence_natural_healing explicitly, so this file keeps testing the same
# thing if the default ever moves." That is FALSE and was never checked. The
# `absent` arm sets nothing -- that is the entire point of the arm -- so two of
# the five gates are load-bearing on the default being 0:
#
#   CR-02  absent -> trapped (coherence <= 0.0001, level != normal)
#   CR-03  absent is byte-identical to explicit 0.0 ("the default IS 0")
#
# Both break the moment the default becomes non-zero. Whoever moves it must
# also move CR-02 onto the explicit-0.0 arm and re-anchor CR-03 to compare
# `absent` against the NEW value -- keeping the property that the default is
# swept as its own case rather than inferred from explicit ones.
#
# The companion acceptance gate that depends on the default wholesale is
# test_recovery_default.sh.
#
# WHAT IS MEASURED
#
# One agent, one fixture: five clean turns, ten turns of unrelated drift that
# floors coherence, then TWENTY-FIVE turns of genuinely correct, on-instruction,
# mutually distinct work. Only the healing rate differs between arms.
#
#     healing   final level   final coherence
#     absent    elevated      0.0000
#     0.0       elevated      0.0000
#     0.30      normal        1.0000
#
# At zero, twenty-five turns of correct work move coherence by nothing at all.
#
# WHY THAT IS A SEMANTICS PROBLEM AND NOT A TUNING PREFERENCE
#
# Coherence is CONSUMED as a current-state signal — the output-admissibility
# gate asks "is this response coherent?", and coherence_proximity asks "how far
# below threshold are we now?", both present tense. With no healing it is
# COMPUTED as worst-ever-sustained: monotonically non-increasing through the
# agent's own behaviour.
#
# It is not strictly one-way — there are three recovery channels — but the other
# two need something external:
#
#     natural healing        coherence_natural_healing > 0   (default 0)
#     S22 validation credit  operator calls agent.record_validation()
#     challenge-pass credit  step_up_enabled                 (default false)
#
# So at defaults, with no operator instrumentation, an agent cannot recover
# through its own behaviour alone.
#
# CR-05 IS THE LOAD-BEARING CONTROL
#
# "The agent recovered its behaviour and coherence did not move" is only
# meaningful if the recovery turns are genuinely good. Run alone, they must hold
# coherence at 1.0000 — otherwise the trapped arms are trapped because the
# fixture's "recovery" is itself drifting, and every other assertion here is
# measuring the fixture rather than the engine.
#
#   CR-01  setup: the drift phase floors coherence in every arm
#   CR-02  healing absent  -> trapped: coherence flat at the floor, level held
#   CR-03  healing 0.0     -> identical to absent (the default IS 0)
#   CR-04  healing 0.30    -> recovers: coherence climbs, level returns to NORMAL
#   CR-05  CONTROL: the recovery turns alone keep coherence at 1.0000
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/cohrec-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_coherence_recovery.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_CR="fake-key-coherence-recovery"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# $1=outfile  $2=full|recovery_only
gen_fixture() {
    python3 - "$1" "$2" <<'PYEOF'
import json, sys
mode = sys.argv[2]
clean = "Ledger reconcile part %d: quarterly totals computed and the balance recorded."
verbs = ["recomputed","verified","reconciled","audited","closed"]
def recovery(n):
    return [{"content": "Ledger reconcile resumed: quarterly totals %s and the balance "
                        "recorded against source ledger section %d." % (verbs[i % 5], i),
             "output_tokens": 45, "thinking_tokens": 20} for i in range(n)]
if mode == "recovery_only":
    # The recovery turns with nothing before them. If these are genuinely good
    # work, coherence must never leave 1.0000.
    r = recovery(30)
else:
    r = [{"content": clean % i, "output_tokens": 45, "thinking_tokens": 20} for i in range(5)]
    drift = ["photography lenses","mountain weather","pasta recipes","jazz history","bicycle gears",
             "tide tables","volcano types","origami folds","desert beetles","harbour cranes"]
    for i, t in enumerate(drift):
        r.append({"content": "Consider %s." % t, "output_tokens": max(6, 18-i), "thinking_tokens": 0})
    r += recovery(25)
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# $1=name $2=full|recovery_only $3=healing json fragment (may be empty) $4=sends
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json" "$2"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_window": 5, $3 "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true,
    "elevated_threshold": 0.40, "elevated_sustained": 1,
    "high_threshold": 0.60, "high_sustained": 2, "critical_threshold": 0.99,
    "deescalate_sustained": 2 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_CR", "max_tokens": 200, "max_turns": 80,
      "system_prompt": "You reconcile ledger data and report quarterly totals." } }
}
EOF
    (cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat > "$WDIR/t.naab" <<EOF
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < $4 { let r = agent.send(h, "continue the ledger reconciliation") i = i + 1 }
    print("DONE")
}
EOF
    (cd "$WDIR" && timeout 300s "$NAAB" t.naab > out.txt 2> err.txt) || true
    stop_stub
    return 0
}

# "turn level coherence" for analysed rows
rows() {
    python3 - "$1" <<'PYEOF'
import json, sys
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") != "CDD_TURN" or d.get("analyzed") != "true": continue
    print("%s %s %s" % (d.get("turn"), d.get("governance_level"), d.get("coherence")))
PYEOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  What an agent gets for recovering                            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

declare -A FINAL_COH FINAL_LVL MIN_COH
for ARM in absent zero heal; do
    case "$ARM" in
        absent) HJ="" ;;
        zero)   HJ='"coherence_natural_healing": 0.0,' ;;
        heal)   HJ='"coherence_natural_healing": 0.30,' ;;
    esac
    if run_case "$ARM" full "$HJ" 40; then
        R=$(rows "$WDIR/tele.jsonl")
        FINAL_COH[$ARM]=$(echo "$R" | tail -1 | awk '{print $3}')
        FINAL_LVL[$ARM]=$(echo "$R" | tail -1 | awk '{print $2}')
        MIN_COH[$ARM]=$(echo "$R" | awk '{print $3}' | sort -g | head -1)
    fi
done

echo -e "${CYAN}--- CR-01: the drift phase floors coherence in every arm ---${NC}"
ALL_FLOORED=true
for ARM in absent zero heal; do
    M=${MIN_COH[$ARM]:-}
    [ -n "$M" ] && awk "BEGIN{exit !($M <= 0.0001)}" || ALL_FLOORED=false
done
if $ALL_FLOORED; then
    pass "CR-01" "all three arms floored (absent ${MIN_COH[absent]}, zero ${MIN_COH[zero]}, heal ${MIN_COH[heal]})"
else
    fail "CR-01" "an arm never floored — recovery is not being exercised" \
         "absent=${MIN_COH[absent]:-?} zero=${MIN_COH[zero]:-?} heal=${MIN_COH[heal]:-?}"
fi

echo -e "${CYAN}--- CR-02/03/04: what 25 turns of correct work earn ---${NC}"
FC_A=${FINAL_COH[absent]:-}; FL_A=${FINAL_LVL[absent]:-}
if [ -n "$FC_A" ] && awk "BEGIN{exit !($FC_A <= 0.0001)}" && [ "$FL_A" != "normal" ]; then
    pass "CR-02" "healing absent: trapped — coherence $FC_A, level $FL_A after 25 correct turns"
else
    fail "CR-02" "healing absent did not trap" "final coherence $FC_A, level $FL_A"
fi

FC_Z=${FINAL_COH[zero]:-}; FL_Z=${FINAL_LVL[zero]:-}
if [ "$FC_Z" = "$FC_A" ] && [ "$FL_Z" = "$FL_A" ]; then
    pass "CR-03" "explicit 0.0 is identical to absent ($FC_Z / $FL_Z) — the default IS 0"
else
    fail "CR-03" "absent and explicit 0.0 differ — the default is not what it appears" \
         "absent=$FC_A/$FL_A explicit=$FC_Z/$FL_Z"
fi

FC_H=${FINAL_COH[heal]:-}; FL_H=${FINAL_LVL[heal]:-}
if [ -n "$FC_H" ] && awk "BEGIN{exit !($FC_H >= 0.9)}" && [ "$FL_H" = "normal" ]; then
    pass "CR-04" "healing 0.30: recovered — coherence $FC_H, level $FL_H on identical input"
else
    fail "CR-04" "healing did not restore the agent" "final coherence $FC_H, level $FL_H"
fi

echo -e "${CYAN}--- CR-05: CONTROL — the recovery turns are genuinely good work ---${NC}"
# Without this the trapped arms could be trapped because the fixture's
# "recovery" is itself drifting, and CR-02 would be measuring the fixture.
if run_case control recovery_only "" 30; then
    RC=$(rows "$WDIR/tele.jsonl")
    RC_MIN=$(echo "$RC" | awk '{print $3}' | sort -g | head -1)
    RC_N=$(echo "$RC" | grep -c .)
    if [ "${RC_N:-0}" -ge 20 ] && [ -n "$RC_MIN" ] && awk "BEGIN{exit !($RC_MIN >= 0.9999)}"; then
        pass "CR-05" "recovery turns alone hold coherence at $RC_MIN over $RC_N turns"
    else
        fail "CR-05" "the 'recovery' turns are not clean — CR-02 measures the fixture, not the engine" \
             "min coherence $RC_MIN over $RC_N turns"
    fi
else skip "CR-05" "stub failed"; fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
