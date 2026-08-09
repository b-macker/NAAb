#!/usr/bin/env bash
# ============================================================
# test_bounded_healing.sh — coherence recovery bounded by damage taken
#
# WHY
#
# coherence_score has 23 penalty sites and two increase paths, and every read of
# it is a live gate: lease renewal, output admissibility, tool gating,
# coherence_prox in the pressure composite, the drift threshold. Once it floors,
# all of them stop measuring the response in front of them. Measured on
# living-script_v3: 22 of 22 turns in the recovery phase were quarantined while
# the agent produced clean on-mandate work indistinguishable from the DESIGN
# phase that passed.
#
# Healing exists (F15) and is set by 25 shipped configs, but at the rates they
# ship (0.01-0.05) it is INERT: heal_factor is 1/(1+signals_fired), so one
# firing signal halves an already-small number while that signal's penalty is
# 0.08-0.15. Varying only the rate on v3: 0.0 and 0.03 give identical results,
# to the quarantine. 0.25 restores the full ladder including elevated->normal.
#
# But a rate large enough to matter is pumpable -- a clean turn heals the full
# amount while a bad turn costs less, so alternating nets positive. So the fix
# is not a bigger number: it is a ledger. Healing is capped at damage not yet
# healed, which is S22's own rule ("credits <= failures -- pass-spam cannot pump
# coherence") applied to the general case.
#
# WHAT IS AND IS NOT SHIPPED HERE
#
# Shipped: the ledger (R1), resetCoherence debiting the same ledger (R2), and
# the ratchet on the three coherence keys (R4). All tightenings.
#
# NOT shipped: raising the default rate (R3). That is the loosening and the only
# change that alters outcomes; it is held for an explicit decision. Which means
# these gates must NOT assert that recovery visibly happens on a stock config --
# it does not, because the shipped rates are still inert. They assert the BOUND,
# which is what was actually changed.
#
# EVERY GATE HAS A DEMONSTRATED FAILURE CASE
#
#   G1  bound removed (heal the full want)      -> BH-01 BH-03
#   G3  ratchet block removed                   -> BH-04 BH-05
#   G4  ratchet fires on ANY change             -> BH-06
#
# BH-07 fails whenever the snapshot fields are dropped, which G1..G4 leave
# intact by construction.
#
# A KNOWN GAP, recorded rather than papered over. G2 inflated the damage ledger
# by +10 per turn and NOTHING failed: BH-03 asserts healed <= damage, which an
# over-counted ledger satisfies trivially while permitting more healing than was
# ever lost -- a loosening these gates cannot see. Catching it needs a
# consistency check (damage booked should reconcile with coherence actually
# lost, i.e. damage ~= (1 - min_coherence_lifetime) + healed). Not written here;
# tracked in docs/open-investigations.md. The bound is verified in the direction
# that matters for safety (healing cannot exceed damage) and unverified in the
# direction that would require the engine to lie to itself about damage.
#
# BH-01 is worth reading before trusting it. Its first version used a control
# agent with every CDD signal disabled -- which emits NO snapshots at all, so
# the gate read that absence as "healed nothing" and passed even under G1. It
# now measures the DRIFTING agent's own per-turn series and requires a positive
# row count, so no-data fails loudly instead of passing quietly.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../helpers/gatelib.sh"
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_bounded_healing.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ ! -x "$NAAB" ]; then
    echo "  test_bounded_healing.sh: SKIPPED — build/naab-lang not found"
    exit 0
fi

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
W="${_SYSTMP}/bounded-heal-$$"
trap 'stop_stub; teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$W"' EXIT
mkdir -p "$W"

"$NAAB" --keygen "$W/key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$W/key.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$W/key.pem"
export FAKE_KEY_BH="fake-key-bounded-healing"

echo "=== bounded coherence healing ==="

gate_init "bounded-healing"
gate_def BH-01 BOUND   "an agent that takes no damage heals nothing"
gate_def BH-02 BOUND   "a drifted agent accrues damage in the ledger"
gate_def BH-03 BOUND   "healing never exceeds damage taken"
gate_def BH-04 RATCHET "raising coherence_natural_healing mid-run is a violation"
gate_def BH-05 RATCHET "raising coherence_recovery_amount mid-run is a violation"
gate_def BH-06 CONTROL "lowering a coherence key mid-run is accepted"
gate_def BH-07 EVIDENCE "the ledger is visible in decision snapshots"

# ------------------------------------------------------------------
# Fixtures: a clean agent and a drifting one, on separate routes.
# ------------------------------------------------------------------
python3 - "$W/fixture.json" << 'PY'
import json, sys
clean = [{"content": "Implemented the Calculator %s method and logged the operation in history."
          % m, "output_tokens": 60, "input_tokens": 200}
         for m in ("add","subtract","multiply","divide","add","subtract","multiply","divide")]
drift = [{"content": c, "output_tokens": 60, "input_tokens": 200} for c in (
    "Ocean tides recede beneath basalt cliffs while gulls circle the headland.",
    "Quarterly amortisation schedules require reconciliation against ledger entries.",
    "The kiln reached cone six before the glaze began to craze along the rim.",
    "Migratory terns span pole to pole across a single year of travel.",
    "Implemented the Calculator add method and logged the operation in history.",
    "Implemented the Calculator subtract method, logging each operation.",
    "Implemented the Calculator multiply method with history logging retained.",
    "Implemented the Calculator divide method, guarding against zero.")]
json.dump({"routes": {"BH-CLEAN": {"responses": clean},
                      "BH-DRIFT": {"responses": drift}}}, open(sys.argv[1], "w"))
PY

start_stub "$W/fixture.json" "$W" || { skip BH-00 "stub failed to start"; gate_exit; exit $?; }

MANDATE="Build a Calculator class with add, subtract, multiply and divide methods, each logged in a history."
cat > "$W/govern.json" << GOVEOF
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "behavioral_sequences": { "enabled": true },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl", "decision_snapshots": true },
  "context_drift": {
    "enabled": true,
    "check_interval_turns": 1,
    "coherence_natural_healing": 0.25
  },
  "agents": {
    "clean": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_BH",
      "max_tokens": 1024, "max_turns": 30,
      "system_prompt": "BH-CLEAN $MANDATE",
      "context_drift_signals": {
        "semantic_stability": false, "mandate_alignment": false, "instruction_recall": false,
        "response_repetition": false, "entity_consistency": false, "plan_drift": false,
        "response_quality": false, "context_growth": false, "prompt_compliance": false,
        "instruction_conflict": false, "persona_fingerprint": false, "thinking_collapse": false,
        "circular_actions": false, "repeated_failures": false, "scope_creep": false,
        "intent_contradictions": false, "vocabulary_contraction": false,
        "capability_underutilization": false, "tool_chain_integrity": false,
        "claim_result_reconciliation": false, "validation_outcome": false
      }
    },
    "drifter": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_BH",
      "max_tokens": 1024, "max_turns": 30,
      "system_prompt": "BH-DRIFT $MANDATE"
    }
  }
}
GOVEOF
(cd "$W" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1)

cat > "$W/t.naab" << 'NAABEOF'
use agent
main {
    let c = agent.create("clean")
    let d = agent.create("drifter")
    let i = 0
    while i < 8 {
        let rc = agent.send(c, "Continue the calculator work")
        let rd = agent.send(d, "Continue the calculator work")
        i = i + 1
    }
    print("RUN_DONE")
}
NAABEOF

(cd "$W" && timeout 180s "$NAAB" t.naab) > "$W/out.txt" 2>&1
stop_stub

# Ledger readout from the decision snapshots.
ledger() {  # $1 = agent config name ; echoes "damage healed rows"
    python3 - "$W/telemetry.jsonl" "$1" << 'PY' 2>/dev/null
import json, sys
dmg = heal = 0.0
rows = 0
try:
    for line in open(sys.argv[1]):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("event_type") != "SEMANTIC_TURN": continue
        if d.get("config_name") != sys.argv[2]: continue
        s = d.get("cdd_snapshot")
        if isinstance(s, str):
            try: s = json.loads(s)
            except Exception: continue
        if isinstance(s, dict) and "coherence_damage_total" in s:
            rows += 1
            dmg = max(dmg, float(s.get("coherence_damage_total", 0.0)))
            heal = max(heal, float(s.get("coherence_healed_total", 0.0)))
except OSError:
    pass
print("%.6f %.6f %d" % (dmg, heal, rows))
PY
}

CLEAN=$(ledger clean); DRIFT=$(ledger drifter)

# Per-turn series for the drifting agent: does healing ever precede damage?
# The control agent (all signals disabled) emits NO snapshots at all, so it
# could not evidence anything -- the first version of BH-01 read that absence
# as "healed nothing" and passed vacuously. Same invariant, real data, and it
# cannot be satisfied by an empty file.
PRE=$(python3 - "$W/telemetry.jsonl" << 'PY'
import json, sys
rows = 0; bad = 0; zero_damage_rows = 0
for line in open(sys.argv[1]):
    try: d = json.loads(line)
    except Exception: continue
    if d.get("event_type") != "SEMANTIC_TURN" or d.get("config_name") != "drifter": continue
    sn = d.get("cdd_snapshot")
    if isinstance(sn, str):
        try: sn = json.loads(sn)
        except Exception: continue
    if not isinstance(sn, dict) or "coherence_damage_total" not in sn: continue
    rows += 1
    dmg = float(sn.get("coherence_damage_total", 0.0))
    heal = float(sn.get("coherence_healed_total", 0.0))
    if dmg <= 1e-9:
        zero_damage_rows += 1
        if heal > 1e-9: bad += 1
print("%d %d %d" % (rows, zero_damage_rows, bad))
PY
)
read -r P_ROWS P_ZERO P_BAD <<< "$PRE"
read -r C_DMG C_HEAL C_ROWS <<< "$CLEAN"
read -r D_DMG D_HEAL D_ROWS <<< "$DRIFT"

gt() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a>b)}'; }
le() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a<=b+1e-9)}'; }

# BH-01 — a clean agent must never heal, because it never lost anything.
# The control agent has every CDD signal disabled, so its damage is zero by
# construction rather than by fixture luck. That matters: several signals key
# off the PROMPT (instruction_recall, instruction_conflict, prompt_compliance),
# which is identical for both agents, so no choice of response content can make
# an agent signal-free. The first version of this gate tried and failed -- the
# "clean" agent booked exactly the same damage as the drifter.
if [ "${P_ROWS:-0}" -eq 0 ]; then
    gk_fail BH-01 "no ledger snapshots at all" \
            "healed=0 and no-data read identically without a row count — this gate would pass vacuously"
elif [ "${P_ZERO:-0}" -eq 0 ]; then
    gk_fail BH-01 "no turn observed before damage was booked" \
            "cannot test the no-damage case; the fixture damages the agent on turn 1"
elif [ "${P_BAD:-1}" -eq 0 ]; then
    pass BH-01 "healing never precedes damage ($P_ZERO zero-damage turns, none healed)"
else
    fail BH-01 "healing accrued on $P_BAD turns with no damage booked" \
         "the ledger bound is not applied — healing can inflate an undamaged agent"
fi

# BH-02 — the drifting agent must actually book damage, or BH-03 is vacuous.
if gt "$D_DMG" 0.0; then
    pass BH-02 "a drifted agent accrues damage (damage=$D_DMG)"
else
    gk_fail BH-02 "drifting agent booked no damage" \
            "without damage, BH-03's inequality holds trivially and proves nothing"
fi

# BH-03 — the load-bearing invariant.
if [ -z "$D_HEAL" ] || [ -z "$D_DMG" ]; then
    gk_fail BH-03 "ledger unreadable" "cannot evaluate the bound"
elif le "$D_HEAL" "$D_DMG"; then
    pass BH-03 "healing never exceeds damage (healed=$D_HEAL <= damage=$D_DMG)"
else
    fail BH-03 "healing exceeded damage" "healed=$D_HEAL > damage=$D_DMG"
fi

# ------------------------------------------------------------------
# BH-04..BH-06 — the ratchet on the three coherence keys.
# ------------------------------------------------------------------
# ------------------------------------------------------------------
# BH-04..BH-06 — the ratchet on the coherence keys, exercised through a real
# mid-run reload rather than a source grep. There is no --check-ratchet entry
# point, and a gate that can only SKIP is an unfailable gate, which is the
# defect this harness exists to prevent.
# ------------------------------------------------------------------
presign() {  # $1 = json text ; echoes the signature
    local pdir="$W/.presign"; rm -rf "$pdir"; mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"
    (cd "$pdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""
}

# $1 = key, $2 = value at start, $3 = value after reload. Echoes engine stderr.
reload_probe() {
    local key="$1" from="$2" to="$3"
    local dir="$W/rt"; rm -rf "$dir"; mkdir -p "$dir"
    local base='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},'
    base+='"telemetry":{"enabled":true,"output_file":"telemetry.jsonl"},'
    base+='"behavioral_sequences":{"enabled":true},"capabilities":{"shell":{"enabled":true}},'
    local after='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},'
    after+='"telemetry":{"enabled":true,"output_file":"telemetry.jsonl"},'
    after+='"behavioral_sequences":{"enabled":true},"capabilities":{"shell":{"enabled":true}},'
    local agents=',"agents":{"w":{"provider":"gemini","model":"stub-model",'
    agents+="\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_BH\","
    agents+='"max_tokens":1024,"max_turns":20,"system_prompt":"BH-CLEAN work"}}}'
    local J1="${base}\"context_drift\":{\"enabled\":true,\"check_interval_turns\":1,\"${key}\":${from}}${agents}"
    local J2="${after}\"context_drift\":{\"enabled\":true,\"check_interval_turns\":1,\"${key}\":${to}}${agents}"
    printf '%s' "$J1" > "$dir/govern.json"
    (cd "$dir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1)
    export NAAB_PRESIGNED_JSON="$J2"
    export NAAB_PRESIGNED_SIG="$(presign "$J2")"
    cat > "$dir/t.naab" << 'NEOF'
use agent
main {
    let h = agent.create("w")
    let r1 = agent.send(h, "hello")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_PRESIGNED_JSON" > govern.json
printf '%s' "$NAAB_PRESIGNED_SIG" > govern.json.sig
>>
    try { let r2 = agent.send(h, "bye") } catch (e) { print("BLOCKED") }
}
NEOF
    (cd "$dir" && timeout 90s "$NAAB" t.naab >/dev/null 2>"$dir/stderr.txt") || true
    cat "$dir/stderr.txt" 2>/dev/null
}

start_stub "$W/fixture.json" "$W" >/dev/null 2>&1 || true

R4=$(reload_probe coherence_natural_healing 0.02 0.20)
if echo "$R4" | grep -qi 'coherence_natural_healing.*loosen'; then
    pass BH-04 "raising coherence_natural_healing mid-run is a violation"
else
    fail BH-04 "raising coherence_natural_healing was not refused" \
         "$(echo "$R4" | grep -i 'ratchet\|loosen' | head -2)"
fi

R5=$(reload_probe coherence_recovery_amount 0.10 0.50)
if echo "$R5" | grep -qi 'coherence_recovery_amount.*loosen'; then
    pass BH-05 "raising coherence_recovery_amount mid-run is a violation"
else
    fail BH-05 "raising coherence_recovery_amount was not refused" \
         "$(echo "$R5" | grep -i 'ratchet\|loosen' | head -2)"
fi

# Control: the ratchet must be directional, not a blanket refusal of any change.
R6=$(reload_probe coherence_natural_healing 0.20 0.02)
if echo "$R6" | grep -qi 'coherence_natural_healing.*loosen'; then
    fail BH-06 "lowering a coherence key was reported as loosening" \
         "the ratchet is refusing every change, not the loosening direction"
else
    pass BH-06 "lowering a coherence key mid-run is accepted"
fi
stop_stub

# BH-07 — the bound must be auditable, not merely correct.
if grep -q 'coherence_damage_total' "$W/telemetry.jsonl" 2>/dev/null; then
    pass BH-07 "the ledger is visible in decision snapshots"
else
    gk_fail BH-07 "ledger absent from snapshots" \
            "a turn that healed nothing and one whose allowance was spent look identical without it"
fi

gate_print_summary
gate_exit

# ============================================================
# DEGRADATION MATRIX — filled in by running each against this suite.
# ============================================================
