#!/usr/bin/env bash
# ============================================================
# test_relative_healing.sh — R5: healing as a fraction of the agent's own
#                            damage rate, instead of a global constant
#
# WHY R5 EXISTS
#
# `coherence_natural_healing` is a per-turn CONSTANT. Whether it suppresses
# escalation depends on the workload's own damage rate, so no constant is right
# everywhere -- R3 (raise the constant) was withdrawn on that basis twice, most
# recently in #172, which measured the usable window as bounded below by
# inertness (nothing earned under 0.12) and above by suppression (0.30 hid
# escalation entirely on a light profile).
#
# R5 removes the constant instead of tuning it:
#
#     base = fraction * mean(damage per DAMAGING turn)
#
# Recovery then takes ~ n_damaging/fraction clean turns whatever the damage
# MAGNITUDE, because the rate cancels. That is what "no cliff" means here, and
# RH-03 is the gate that actually tests it.
#
# WHY THIS FILE PINS persona_baseline_adaptive: false
#
# RH-10 needs a signal that fires on EVERY turn to test that healing does not
# out-heal a live one. The only thing supplying that was S17's frozen warm-up
# baseline -- i.e. a defect. C1e fixed it and defaulted the fix ON (2026-08-29),
# at which point S17 correctly goes quiet on correct work, no live signal exists,
# healing recovers, and RH-10 fails while nothing is actually wrong. Verified: on
# the default-flip probe RH-10 reported relative reaching 1.0000.
#
# So every arm here pins the frozen baseline. The alternative -- letting RH-10
# lose its premise silently -- is worse than a failing gate: a guard that has
# stopped testing anything still looks green.
#
# THE POSITIVE CONTROL IS NOT THE ONE THE DESIGN PROPOSED
#
# docs/proposal-relative-healing.md specified RH-05 as "suppression reappears at
# fraction = 1.5". The implementation clamps the fraction below 1.0 -- that is
# the safety invariant, since at >= 1.0 alternating clean/dirty turns net
# positive and the ledger is pumpable on every profile at once. The clamp makes
# the proposed control UNREACHABLE by construction.
#
# Asserting "no suppression at any fraction" with no reachable positive case
# would repeat the exact failure of #172, where the light-profile control could
# not reach HIGH with healing off and the deciding cell was vacuous.
#
# So the control moved to the mechanism R5 REPLACES: RH-05 shows this same
# fixture and harness DO detect suppression when driven by absolute healing at
# 0.30 (the value #172 observed suppressing escalation on a light profile).
# Without RH-05 passing, RH-04 proves nothing. RH-09 separately pins the clamp.
#
# WHAT IMPLEMENTATION CHANGED ABOUT THE DESIGN
#
# The first version of this file failed RH-02 and RH-08, and the reason was not
# in the code. On this fixture S17 persona_fingerprint fires 0.0500 on EVERY
# recovery turn, forever -- its baseline is set once from the five warm-up turns
# and the recovery responses differ from them in keyword count. Relative healing
# cannot climb through that: once the damage window fills with the residual r,
# the rate IS r, and the grant is f*r/(1+k) < r for any f < 1.
#
# The instinct was to redesign R5 (a decaying high-water reference) so it could
# climb through. Measuring first showed that would have been building the defect
# on purpose: absolute healing at 0.30, which DOES climb through, pins coherence
# at 1.0000 for fourteen straight turns while the signal fires every one of them.
# Out-healing a live signal IS suppression; there is no version of it that is
# recovery. Refusing to heal while a signal fires is R5 behaving correctly.
#
# So the gate moved, not the mechanism. RH-02 now asks the real question on a
# fixture that is clean in the ENGINE's view, with a no-healing control; RH-10
# asserts the inverse, with absolute 0.30 as its demonstrated failure case.
#
#   RH-01   default off: absent key is byte-identical to explicit 0.0
#   RH-02a  CONTROL: same fixture without healing stays floored
#   RH-02b  recovery: an agent that floored recovers at fraction 0.5
#   RH-03   SCALE INVARIANCE: heal-per-clean-turn / damage-rate == fraction,
#           holding when the damage magnitude is tripled
#   RH-04   no suppression at fraction < 1 on a light profile
#   RH-05   POSITIVE CONTROL: absolute healing 0.30 DOES suppress on that same
#           profile -- the harness can see suppression
#   RH-06   no pumping: alternating clean/dirty nets negative
#   RH-07   ratchet: raising the fraction mid-run is a violation
#   RH-08   a floored agent still heals (window spans damaging turns only)
#   RH-09   the fraction is clamped below 1.0
#   RH-10   INVERSE OF RH-02: a persistently firing signal must NOT be
#           out-healed -- absolute 0.30 is the demonstrated failure case
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/relheal-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_relative_healing.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_RH="fake-key-relative-healing"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# $1=outfile  $2=full|drift_only|alternating
gen_fixture() {
    python3 - "$1" "$2" <<'PYEOF'
import json, sys
mode = sys.argv[2]
clean = "Ledger reconcile part %d: quarterly totals computed and the balance recorded."
verbs = ["recomputed","verified","reconciled","audited","closed"]
topics = ["photography lenses","mountain weather","pasta recipes","jazz history","bicycle gears",
          "tide tables","volcano types","origami folds","desert beetles","harbour cranes",
          "kite design","reef fish","train signals","cheese caves","wind turbines",
          "moss species","clock escapements","salt flats","owl calls","canal locks"]
def recovery(n):
    return [{"content": "Ledger reconcile resumed: quarterly totals %s and the balance "
                        "recorded against source ledger section %d." % (verbs[i % 5], i),
             "output_tokens": 45, "thinking_tokens": 20} for i in range(n)]
def drift(n, off=0):
    return [{"content": "Consider %s." % topics[(i+off) % len(topics)],
             "output_tokens": max(6, 18 - (i % 12)), "thinking_tokens": 0} for i in range(n)]
if mode == "drift_only":
    r = [{"content": clean % i, "output_tokens": 45, "thinking_tokens": 20} for i in range(5)]
    r += drift(30)
elif mode == "alternating":
    # One damaging turn, one clean turn, repeated. With fraction < 1 the pair
    # must net NEGATIVE; at fraction >= 1 it would net >= 0 and be pumpable.
    r = [{"content": clean % i, "output_tokens": 45, "thinking_tokens": 20} for i in range(5)]
    for i in range(15):
        r += drift(1, off=i)
        r += recovery(1)
else:
    r = [{"content": clean % i, "output_tokens": 45, "thinking_tokens": 20} for i in range(5)]
    r += drift(10)
    r += recovery(25)
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# $1=name $2=fixture mode $3=context_drift json fragment $4=sends
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json" "$2"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl", "decision_snapshots": true },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_window": 5, $3
    "thresholds": { "persona_baseline_adaptive": false },
    "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true,
    "elevated_threshold": 0.40, "elevated_sustained": 1,
    "high_threshold": 0.60, "high_sustained": 2, "critical_threshold": 0.99,
    "deescalate_sustained": 2 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_RH", "max_tokens": 200, "max_turns": 90,
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

# damage_rate_mean from the decision snapshot on the LAST turn that carries one
damage_mean() {
    python3 - "$1" <<'PYEOF'
import json, sys
last = ""
for line in open(sys.argv[1]):
    if "damage_rate_mean" not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    snap = d.get("cdd_snapshot")
    if isinstance(snap, str):
        try: snap = json.loads(snap)
        except Exception: continue
    if isinstance(snap, dict) and "damage_rate_mean" in snap:
        last = "%.6f" % float(snap["damage_rate_mean"])
print(last)
PYEOF
}

maxlvl() {
    local best="none" bestr=-1 r
    for L in $(echo "$1" | awk '{print $2}' | sort -u); do
        case "$L" in normal) r=0;; elevated) r=1;; high) r=2;; critical) r=3;; *) r=-1;; esac
        if [ "$r" -gt "$bestr" ]; then bestr=$r; best=$L; fi
    done
    echo "$best"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  R5 — healing relative to the agent's own damage rate         |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

REL05='"coherence_healing_damage_fraction": 0.5,'
# The signal that fires on EVERY recovery turn of this fixture (S17, whose
# baseline is set once from the warm-up window). Disabled where the gate is
# about healing; left ON in RH-10, where it is the point.
NOS17='"signals": {"persona_fingerprint": false},'

# ---------- RH-01: the default is OFF ----------
echo -e "${CYAN}--- RH-01: default off ---${NC}"
run_case abs_absent full "" 40 && R_ABSENT=$(rows "$TEST_TMP/abs_absent/tele.jsonl")
run_case abs_zero   full '"coherence_healing_damage_fraction": 0.0,' 40 && R_ZERO=$(rows "$TEST_TMP/abs_zero/tele.jsonl")
FC_ABSENT=$(echo "${R_ABSENT:-}" | tail -1 | awk '{print $3}')
FC_ZERO=$(echo "${R_ZERO:-}"   | tail -1 | awk '{print $3}')
FL_ABSENT=$(echo "${R_ABSENT:-}" | tail -1 | awk '{print $2}')
FL_ZERO=$(echo "${R_ZERO:-}"   | tail -1 | awk '{print $2}')
if [ -n "$FC_ABSENT" ] && [ "$FC_ABSENT" = "$FC_ZERO" ] && [ "$FL_ABSENT" = "$FL_ZERO" ]; then
    pass "RH-01" "absent is byte-identical to explicit 0.0 ($FC_ABSENT / $FL_ABSENT) — default IS off"
else
    fail "RH-01" "absent and explicit 0.0 differ — the default is not what it appears" \
         "absent=$FC_ABSENT/$FL_ABSENT explicit=$FC_ZERO/$FL_ZERO"
fi

# ---------- RH-02: recovery at fraction 0.5 ----------
echo -e "${CYAN}--- RH-02: an agent that floored recovers ---${NC}"
# The recovery turns must be clean IN THE ENGINE'S VIEW, which is not the same
# as being good work. On the raw fixture S17 persona_fingerprint fires 0.05
# EVERY recovery turn forever: its baseline is set once from the five warm-up
# turns and the recovery responses differ from them in keyword count. Measured:
# turns 18-40, persona_fingerprint=0.0500, every turn, alone.
#
# A gate that demands recovery through that is not a recovery gate. It can only
# be passed by a mechanism that OUT-HEALS a live signal, which is suppression --
# see RH-10, where absolute healing does exactly that and is the demonstrated
# failure case. So RH-02 disables the persistently-firing signal and asks the
# real question: with nothing firing, does relative healing return the agent?
run_case rel_05 full "$REL05 $NOS17" 40 && R_REL=$(rows "$TEST_TMP/rel_05/tele.jsonl")
run_case rel_none full "$NOS17" 40 && R_NONE=$(rows "$TEST_TMP/rel_none/tele.jsonl")
MIN_REL=$(echo "${R_REL:-}" | awk '{print $3}' | sort -g | head -1)
FC_REL=$(echo "${R_REL:-}" | tail -1 | awk '{print $3}')
FL_REL=$(echo "${R_REL:-}" | tail -1 | awk '{print $2}')
FC_NONE=$(echo "${R_NONE:-}" | tail -1 | awk '{print $3}')
FL_NONE=$(echo "${R_NONE:-}" | tail -1 | awk '{print $2}')

# CONTROL FIRST. Without it, RH-02 is satisfied by a fixture that recovers on
# its own once the signal is off, and would be measuring the signal removal
# rather than the healing.
if [ -n "$FC_NONE" ] && awk "BEGIN{exit !($FC_NONE <= 0.0001)}" && [ "$FL_NONE" != "normal" ]; then
    pass "RH-02a" "CONTROL: same fixture, no healing — still floored at $FC_NONE / $FL_NONE"
    REL_CONTROL_OK=1
else
    fail "RH-02a" "CONTROL FAILED: the fixture recovers without healing — RH-02b measures the fixture" \
         "no-healing final $FC_NONE / $FL_NONE"
    REL_CONTROL_OK=0
fi
if [ "$REL_CONTROL_OK" = "1" ]; then
    if [ -n "$MIN_REL" ] && awk "BEGIN{exit !($MIN_REL <= 0.0001)}"; then
        if awk "BEGIN{exit !($FC_REL >= 0.30)}" && [ "$FL_REL" = "normal" ]; then
            pass "RH-02b" "floored ($MIN_REL) then recovered to $FC_REL, level $FL_REL"
        else
            fail "RH-02b" "relative healing did not restore the agent" "final $FC_REL / $FL_REL"
        fi
    else
        skip "RH-02b" "the drift phase never floored — gate not exercised (min ${MIN_REL:-?})"
    fi
else
    skip "RH-02b" "control failed — a recovery result here would mean nothing"
fi

# ---------- RH-03: SCALE INVARIANCE ----------
echo -e "${CYAN}--- RH-03: scale invariance (the no-cliff property) ---${NC}"
# Same fixture, damage magnitude tripled via the signal weights. If `base`
# carried any absolute term, heal/damage_rate would NOT be equal across arms.
W3='"weights": {"semantic_stability": 0.30, "instruction_recall": 0.24, "entity_consistency": 0.24, "mandate_alignment": 0.36},'
# Same clean-recovery basis as RH-02: with a signal firing every recovery turn
# the damage window fills with the residual and the ratio measures that, not
# the formula.
run_case rel_x3 full "$REL05 $W3 $NOS17" 40 && R_X3=$(rows "$TEST_TMP/rel_x3/tele.jsonl")
D1=$(damage_mean "$TEST_TMP/rel_05/tele.jsonl")
D3=$(damage_mean "$TEST_TMP/rel_x3/tele.jsonl")
# heal per clean turn = the largest single positive coherence step in recovery
heal_step() {
    echo "$1" | awk '{c=$3+0; if (NR>1 && c>p) {d=c-p; if (d>m) m=d} p=c} END{printf "%.6f", m+0}'
}
H1=$(heal_step "${R_REL:-}")
H3=$(heal_step "${R_X3:-}")
if [ -n "$D1" ] && [ -n "$D3" ] && awk "BEGIN{exit !($D1>0 && $D3>0 && $H1>0 && $H3>0)}"; then
    RATIO1=$(awk "BEGIN{printf \"%.3f\", $H1/$D1}")
    RATIO3=$(awk "BEGIN{printf \"%.3f\", $H3/$D3}")
    SCALE=$(awk "BEGIN{printf \"%.2f\", $D3/$D1}")
    if awk "BEGIN{exit !(($RATIO1-$RATIO3)<0.12 && ($RATIO3-$RATIO1)<0.12)}"; then
        pass "RH-03" "heal/damage-rate held at ${RATIO1} vs ${RATIO3} while the damage rate scaled ${SCALE}x"
    else
        fail "RH-03" "heal/damage-rate moved with the damage magnitude — there is an absolute term in base" \
             "x1: heal=$H1 rate=$D1 ratio=$RATIO1 | x3: heal=$H3 rate=$D3 ratio=$RATIO3"
    fi
else
    skip "RH-03" "no damage-rate snapshot to compare (x1='${D1:-}' x3='${D3:-}' h1='${H1:-}' h3='${H3:-}')"
fi

# ---------- RH-04 / RH-05: suppression, and whether we could see it ----------
echo -e "${CYAN}--- RH-04/05: escalation suppression, with a reachable control ---${NC}"
LIGHT='"signals": {"instruction_recall": false, "semantic_stability": false, "persona_fingerprint": false, "entity_consistency": false},'
run_case sup_off  drift_only "$LIGHT" 35 && R_SOFF=$(rows "$TEST_TMP/sup_off/tele.jsonl")
run_case sup_rel  drift_only "$REL05 $LIGHT" 35 && R_SREL=$(rows "$TEST_TMP/sup_rel/tele.jsonl")
run_case sup_rel9 drift_only '"coherence_healing_damage_fraction": 0.9,'" $LIGHT" 35 && R_SREL9=$(rows "$TEST_TMP/sup_rel9/tele.jsonl")
run_case sup_abs  drift_only '"coherence_natural_healing": 0.30,'" $LIGHT" 35 && R_SABS=$(rows "$TEST_TMP/sup_abs/tele.jsonl")
L_OFF=$(maxlvl "${R_SOFF:-}"); L_REL=$(maxlvl "${R_SREL:-}")
L_REL9=$(maxlvl "${R_SREL9:-}"); L_ABS=$(maxlvl "${R_SABS:-}")

# RH-05 first: it licenses RH-04.
if [ "$L_OFF" != "normal" ] && [ "$L_ABS" = "normal" ]; then
    pass "RH-05" "CONTROL: harness detects suppression — no healing reaches '$L_OFF', absolute 0.30 stays 'normal'"
    CONTROL_OK=1
else
    fail "RH-05" "the harness cannot demonstrate suppression — RH-04 is unfalsifiable" \
         "no-healing max='$L_OFF' absolute-0.30 max='$L_ABS' (control needs off!=normal and abs==normal)"
    CONTROL_OK=0
fi
if [ "$CONTROL_OK" = "1" ]; then
    if [ "$L_REL" = "$L_OFF" ] && [ "$L_REL9" = "$L_OFF" ]; then
        pass "RH-04" "no suppression at fraction 0.5 or 0.9 — still reaches '$L_REL' / '$L_REL9' like unhealed '$L_OFF'"
    else
        fail "RH-04" "relative healing suppressed escalation" \
             "off='$L_OFF' f=0.5:'$L_REL' f=0.9:'$L_REL9'"
    fi
else
    skip "RH-04" "control failed — a no-suppression result here would mean nothing"
fi

# ---------- RH-06: no pumping ----------
echo -e "${CYAN}--- RH-06: alternating clean/dirty must net negative ---${NC}"
run_case pump full "$REL05" 8 >/dev/null 2>&1
run_case pump_alt alternating "$REL05" 36 && R_ALT=$(rows "$TEST_TMP/pump_alt/tele.jsonl")
FIRST_ALT=$(echo "${R_ALT:-}" | head -1 | awk '{print $3}')
LAST_ALT=$(echo "${R_ALT:-}" | tail -1 | awk '{print $3}')
if [ -n "$FIRST_ALT" ] && [ -n "$LAST_ALT" ]; then
    if awk "BEGIN{exit !($LAST_ALT < $FIRST_ALT)}"; then
        pass "RH-06" "15 clean/dirty pairs net negative ($FIRST_ALT -> $LAST_ALT)"
    else
        fail "RH-06" "alternating turns did not net negative — the ledger is pumpable" \
             "$FIRST_ALT -> $LAST_ALT"
    fi
else
    skip "RH-06" "no rows from the alternating fixture"
fi

# ---------- RH-07: ratchet ----------
echo -e "${CYAN}--- RH-07: raising the fraction mid-run is a violation ---${NC}"
RDIR="$TEST_TMP/ratchet"; mkdir -p "$RDIR"
cat > "$RDIR/govern.json" <<'EOF'
{ "version": "5.0", "mode": "enforce",
  "context_drift": { "enabled": true, "coherence_healing_damage_fraction": 0.3 } }
EOF
cat > "$RDIR/govern_new.json" <<'EOF'
{ "version": "5.0", "mode": "enforce",
  "context_drift": { "enabled": true, "coherence_healing_damage_fraction": 0.7 } }
EOF
if grep -q "coherence_healing_damage_fraction" "$SCRIPT_DIR/../../src/runtime/governance_config.cpp" && \
   grep -q "healing covers more of the damage rate" "$SCRIPT_DIR/../../src/runtime/governance_config.cpp"; then
    pass "RH-07" "the fraction is registered in the coherence ratchet block (raise = loosening)"
else
    fail "RH-07" "the fraction is not ratcheted — a withdrawn bound could be re-granted mid-run"
fi

# ---------- RH-08: a floored agent still heals ----------
echo -e "${CYAN}--- RH-08: the window spans damaging turns only ---${NC}"
# The RH-02 arm floors at 0.0000 and then recovers. If the damage window
# counted non-damaging turns, the mean would decay toward 0 while the agent sat
# floored and healing would never restart.
if [ -n "${MIN_REL:-}" ] && awk "BEGIN{exit !($MIN_REL <= 0.0001)}" && \
   [ -n "${FC_REL:-}" ] && awk "BEGIN{exit !($FC_REL > 0.0001)}"; then
    pass "RH-08" "an agent at the floor ($MIN_REL) still healed back to $FC_REL"
else
    fail "RH-08" "a floored agent did not heal — the window is being diluted by non-damaging turns" \
         "min=${MIN_REL:-?} final=${FC_REL:-?}"
fi

# ---------- RH-09: the clamp ----------
echo -e "${CYAN}--- RH-09: the fraction is clamped below 1.0 ---${NC}"
run_case clamp_hi full '"coherence_healing_damage_fraction": 1.5,' 40 && R_CHI=$(rows "$TEST_TMP/clamp_hi/tele.jsonl")
run_case clamp_99 full '"coherence_healing_damage_fraction": 0.99,' 40 && R_C99=$(rows "$TEST_TMP/clamp_99/tele.jsonl")
FC_CHI=$(echo "${R_CHI:-}" | tail -1 | awk '{print $3}')
FC_C99=$(echo "${R_C99:-}" | tail -1 | awk '{print $3}')
if [ -n "$FC_CHI" ] && [ "$FC_CHI" = "$FC_C99" ]; then
    pass "RH-09" "fraction 1.5 behaves as 0.99 ($FC_CHI) — the safety invariant cannot be configured away"
else
    fail "RH-09" "1.5 and 0.99 differ — the clamp is not binding" "1.5=$FC_CHI 0.99=$FC_C99"
fi

# ---------- RH-10: a persistent signal must NOT be out-healed ----------
echo -e "${CYAN}--- RH-10: healing must not neutralise a live signal ---${NC}"
# THE INVERSE OF RH-02, and the reason this file exists in the shape it does.
#
# On the raw fixture S17 fires 0.0500 on every recovery turn. Absolute healing
# at 0.30 pays 0.30 * 1/(1+1) = 0.15 against it, nets +0.10/turn, and pins
# coherence at 1.0000 from turn 27 to turn 40 -- fourteen consecutive turns at
# PERFECT coherence while a penalty signal fires every one of them. Observed,
# not derived. That is the campaign's own definition of the failure it exists to
# catch: an agent that never escalates looks exactly like an agent that behaved.
#
# Relative healing cannot do this by construction. Once the window fills with
# the residual r, the rate IS r, so the grant is f*r/(1+k) < r for any f < 1 --
# the signal always outruns its own contribution to the healing rate.
#
# Absolute 0.30 is therefore the DEMONSTRATED FAILURE CASE for this gate: it is
# a real, shipped, configurable setting that fails it today.
run_case live_rel full "$REL05" 40 && R_LREL=$(rows "$TEST_TMP/live_rel/tele.jsonl")
run_case live_abs full '"coherence_natural_healing": 0.30,' 40 && R_LABS=$(rows "$TEST_TMP/live_abs/tele.jsonl")
FC_LREL=$(echo "${R_LREL:-}" | tail -1 | awk '{print $3}')
FC_LABS=$(echo "${R_LABS:-}" | tail -1 | awk '{print $3}')
if [ -n "$FC_LABS" ] && awk "BEGIN{exit !($FC_LABS >= 0.99)}"; then
    if [ -n "$FC_LREL" ] && awk "BEGIN{exit !($FC_LREL < 0.30)}"; then
        pass "RH-10" "live signal: relative held at $FC_LREL while absolute 0.30 pinned $FC_LABS"
    else
        fail "RH-10" "relative healing neutralised a persistently firing signal — this is suppression" \
             "relative final $FC_LREL (absolute, the known-bad case, reached $FC_LABS)"
    fi
else
    skip "RH-10" "the failure case did not reproduce — absolute 0.30 reached '${FC_LABS:-?}', expected ~1.0"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Total: $((PASS_COUNT+FAIL_COUNT+SKIP_COUNT)) | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}FAILURES:${NC}$FAILURES"; exit 1; fi
echo -e "  test_relative_healing.sh: ALL PASSED"
exit 0
