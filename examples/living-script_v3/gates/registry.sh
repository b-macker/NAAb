#!/usr/bin/env bash
# ============================================================
# living-script_v3 gate registry.
#
# Gates read exactly two globals, $G_OUT and $G_TELE, so a gate cannot tell
# whether the bytes came from a live run or a fixture. That is what makes each
# one independently invocable by gate_selftest.sh against its negative fixture.
#
# TWO RULES THIS FILE IS BUILT AROUND
#
# 1. A gate must never assert "agent X is at level Y". governance_level_ is a
#    single engine-global atomic driven by whichever handle last took a turn;
#    per-agent level is not a thing that exists, so such a gate is
#    unfalsifiable. The falsifiable form is "the system reached Y while X was
#    the pressure handle", which is what V3-06 asserts using
#    GOVERNANCE_LEVEL_CHANGE's own config_name field.
#
# 2. A zero is only evidence if you can say what would have made it non-zero.
#    V3-03 (DESIGN must not escalate) and V3-10 (siblings must never raise the
#    level) both assert absences, so each is paired with a positive control in
#    the same run: V3-04/V3-05 prove escalation is reachable at all, which is
#    what stops V3-03 and V3-10 passing on a run where nothing happened.
# ============================================================

gate_def V3-01 SETUP    "three agents created"
gate_def V3-02 DESIGN   "DESIGN phase ran start to end"
gate_def V3-03 CONTROL  "DESIGN did not escalate — the baseline is a baseline"
gate_def V3-04 LADDER   "system reached ELEVATED"
gate_def V3-05 LADDER   "system reached HIGH"
gate_def V3-06 LADDER   "drift_worker was the pressure handle at every escalation"
gate_def V3-07 LADDER   "cb_sustained_elevated accumulated past elevated_sustained"
gate_def V3-08 RECOVERY "DRIFT_RECOVERY phase ran start to end"
gate_def V3-09 RUN      "run completed rather than being cut short"
gate_def V3-10 CONTROL  "no sibling handle ever raised the level"
gate_def V3-11 DEESC    "de-escalation, when its precondition holds"

# --- helpers ---------------------------------------------------------------
# jq is not a dependency of this repo, so telemetry is read with python3, which
# already is (agent_stub.py, gen_fixture.py, tools/naabfuzz).
_tel() {  # $1 = python expression over `rows`, printed
    python3 - "$G_TELE" "$1" <<'PY' 2>/dev/null
import json, sys
rows = []
try:
    for line in open(sys.argv[1]):
        try: rows.append(json.loads(line))
        except Exception: pass
except OSError:
    pass
def ev(t): return [r for r in rows if r.get("event_type") == t]
def snaps():
    out = []
    for r in ev("SEMANTIC_TURN"):
        s = r.get("cdd_snapshot")
        if isinstance(s, str):
            try: s = json.loads(s)
            except Exception: continue
        if isinstance(s, dict): out.append(s)
    return out
print(eval(sys.argv[2]))
PY
}

# Longest run of CONSECUTIVE calm turns from drift_worker after the first
# escalation, where "calm" is the engine's own predicate: composite pressure
# below the threshold the level in force at that moment must hold. Kept as its
# own helper because expressing it inline made it unreadable, and an unreadable
# predicate is how the two earlier wrong versions survived review.
_calm_run() {
    python3 - "$G_TELE" <<'PY' 2>/dev/null
import json, sys, itertools
rows = []
try:
    for line in open(sys.argv[1]):
        try: rows.append(json.loads(line))
        except Exception: pass
except OSError:
    pass
def ev(t): return [r for r in rows if r.get("event_type") == t]
LV = {"normal": 0, "elevated": 1, "high": 2, "critical": 3}
HOLD = {1: 0.35, 2: 0.55, 3: 0.80}   # v3 govern.json circuit_breaker
lc = ev("GOVERNANCE_LEVEL_CHANGE")
first = min([int(r.get("turn", 0)) for r in lc] or [10**9])
cdd = sorted([r for r in ev("CDD_TURN")
              if r.get("config_name") == "drift_worker"
              and str(r.get("analyzed")) == "true"],
             key=lambda r: int(r.get("turn", 0)))
flags = []
for i in range(1, len(cdd)):
    if int(cdd[i].get("turn", 0)) <= first:
        continue
    # level IN FORCE for this turn = the previous row's, since a row's own
    # governance_level is written after the update
    lvl = LV.get(str(cdd[i - 1].get("governance_level")), 0)
    try: p = float(cdd[i].get("pressure") or 0)
    except Exception: p = 0.0
    flags.append(lvl > 0 and p < HOLD[lvl])
print(max([0] + [sum(1 for _ in g) for k, g in itertools.groupby(flags) if k]))
PY
}

# --- gates -----------------------------------------------------------------
gate_V3_01() {
    if grep -q 'AGENTS|created|3' "$G_OUT" 2>/dev/null; then
        pass V3-01 "three agents created"
    else
        gk_fail V3-01 "agents were not created" "no AGENTS|created|3 marker"
    fi
}

gate_V3_02() {
    if phase_complete DESIGN; then
        pass V3-02 "DESIGN phase ran start to end"
    else
        gk_fail V3-02 "DESIGN phase incomplete" "start and end markers not both present"
    fi
}

# The baseline must stay at NORMAL. If it does not, the tuning is wrong and
# every ladder gate below is measuring an artefact rather than drift.
gate_V3_03() {
    local design_end first_esc
    design_end=$(grep -n 'PHASE|DESIGN|end' "$G_OUT" 2>/dev/null | head -1 | cut -d: -f1)
    first_esc=$(_tel 'min([int(r.get("turn",0)) for r in ev("GOVERNANCE_LEVEL_CHANGE")] or [-1])')
    if [ -z "$design_end" ]; then
        gk_fail V3-03 "cannot evaluate the baseline" "DESIGN never ended"
    elif [ "${first_esc:--1}" = "-1" ]; then
        # No escalation anywhere. Not a pass: V3-04/V3-05 will fail and say so,
        # and reporting a clean baseline for a run that never escalated is
        # exactly the vacuity this scenario exists to avoid.
        gk_fail V3-03 "no escalation occurred anywhere in the run" \
                "a quiet DESIGN phase proves nothing when nothing ever escalated"
    elif [ "$first_esc" -ge 3 ]; then
        pass V3-03 "DESIGN did not escalate — the baseline is a baseline"
    else
        fail V3-03 "escalation began during the DESIGN baseline" \
             "first level change at turn $first_esc, before the baseline finished"
    fi
}

gate_V3_04() {
    local n; n=$(_tel 'len([r for r in ev("GOVERNANCE_LEVEL_CHANGE") if r.get("to_level")=="elevated"])')
    if [ "${n:-0}" -gt 0 ]; then
        pass V3-04 "system reached ELEVATED"
    else
        gk_fail V3-04 "never reached ELEVATED" "no GOVERNANCE_LEVEL_CHANGE to elevated"
    fi
}

gate_V3_05() {
    local n; n=$(_tel 'len([r for r in ev("GOVERNANCE_LEVEL_CHANGE") if r.get("to_level")=="high"])')
    if [ "${n:-0}" -gt 0 ]; then
        pass V3-05 "system reached HIGH"
    else
        gk_fail V3-05 "never reached HIGH" "no GOVERNANCE_LEVEL_CHANGE to high"
    fi
}

# "the system reached Y while X was the pressure handle" — the only falsifiable
# form, since the level itself is engine-global.
gate_V3_06() {
    local others total
    total=$(_tel 'len(ev("GOVERNANCE_LEVEL_CHANGE"))')
    others=$(_tel 'len([r for r in ev("GOVERNANCE_LEVEL_CHANGE") if r.get("config_name") not in ("drift_worker",)])')
    if [ "${total:-0}" -eq 0 ]; then
        gk_fail V3-06 "no escalation to attribute" "no level changes recorded"
    elif [ "${others:-1}" -eq 0 ]; then
        pass V3-06 "drift_worker was the pressure handle at every escalation"
    else
        fail V3-06 "a non-drift agent drove an escalation" \
             "$others of $total level changes attributed elsewhere"
    fi
}

gate_V3_07() {
    local peak; peak=$(_tel 'max([int(s.get("cb_sustained_elevated",0)) for s in snaps()] or [0])')
    if [ "${peak:-0}" -ge 2 ]; then
        pass V3-07 "cb_sustained_elevated accumulated past elevated_sustained (peak $peak)"
    else
        gk_fail V3-07 "cb_sustained_elevated never reached elevated_sustained" \
                "peak ${peak:-0}, needs 2 — decision_snapshots must be enabled for this to be readable"
    fi
}

gate_V3_08() {
    if phase_complete DRIFT_RECOVERY; then
        pass V3-08 "DRIFT_RECOVERY phase ran start to end"
    else
        gk_fail V3-08 "DRIFT_RECOVERY incomplete" "start and end markers not both present"
    fi
}

gate_V3_09() {
    if grep -q 'RUN|complete|true' "$G_OUT" 2>/dev/null; then
        pass V3-09 "run completed rather than being cut short"
    else
        gk_fail V3-09 "run did not reach its end" "no RUN|complete|true marker"
    fi
}

gate_V3_10() {
    local sib total
    total=$(_tel 'len(ev("GOVERNANCE_LEVEL_CHANGE"))')
    sib=$(_tel 'len([r for r in ev("GOVERNANCE_LEVEL_CHANGE") if r.get("config_name") in ("pm","judge")])')
    if [ "${total:-0}" -eq 0 ]; then
        gk_fail V3-10 "no level changes to attribute" \
                "silence from siblings means nothing when nothing escalated at all"
    elif [ "${sib:-1}" -eq 0 ]; then
        pass V3-10 "no sibling handle ever raised the level"
    else
        fail V3-10 "a sibling raised the level" \
             "$sib level changes attributed to pm or judge — their signals should make that impossible"
    fi
}

# De-escalation is SKIPPED with its reason rather than failed when the
# precondition does not hold, because "the raising handle never acted again"
# is a scenario shortfall, not a governance failure, and the two should not
# report the same way.
#
# This gate PASSES: the run steps high -> elevated at turn 18, the first time
# the hysteresis has been observed working at all. An earlier probe concluded
# it was structurally unreachable; that was wrong, and wrong for an instructive
# reason -- that probe's "recovery" phase still fired two signals a turn, so
# composite floored at 0.567, just above high_threshold 0.55. Nothing stepped
# down because nothing dropped below a threshold, which looks identical to a
# broken mechanism.
#
# What IS blocked is the last step. From turn 18 to 36 the handle is quiet and
# composite sits at exactly 0.5125, above elevated_threshold 0.35, so
# elevated -> normal never happens: with coherence floored at 0.0 the
# coherence_prox term alone holds the composite up, and coherence_natural_healing
# defaults to 0.0 so it never climbs back. See docs/open-investigations.md C1.
gate_V3_11() {
    local down post
    down=$(_tel 'len([r for r in ev("GOVERNANCE_LEVEL_CHANGE") if (r.get("from_level"),r.get("to_level")) in (("high","elevated"),("elevated","normal"),("high","normal"))])')
    # The engine's actual precondition, twice corrected. Version 1 counted every
    # analyzed CDD_TURN after the first escalation, so keyed run 2 reported "8
    # calm turns" when all 8 fired 2-3 signals and a correct non-firing was
    # called a governance failure. Version 2 counted turns with no penalty --
    # closer, but still not what the engine tests, and far too generous: on a
    # keyless run it reads 21 where the mechanism saw 3.
    #
    # The engine steps down when the COMPUTED TARGET is below the current level
    # for deescalate_sustained CONSECUTIVE turns from the raising handle. A turn
    # is calm when composite pressure falls below the threshold its CURRENT level
    # must hold -- that alone is sufficient for target < level. Two details this
    # depends on, both of which invert the answer if missed:
    #
    #   1. HOLD is keyed by the level being LEFT (elevated 0.35, high 0.55), not
    #      the level being entered.
    #   2. A row's governance_level is written AFTER the update, so on the very
    #      turn a step-down lands the row already shows the LOWER level and would
    #      score itself against a threshold it was never held to. Judge each turn
    #      by the previous row's level -- the one in force when it was evaluated.
    #
    # With both right this reproduces the run exactly: 3 calm turns,
    # deescalate_sustained 3, one step-down. With (2) wrong it reads 2, and the
    # gate contradicts a step-down sitting in its own telemetry.
    #
    # CONSECUTIVE, not total: the counter resets on any non-calm turn.
    post=$(_calm_run)
    if [ "${down:-0}" -gt 0 ]; then
        pass V3-11 "de-escalation fired ($down step-downs)"
    elif [ "${post:-0}" -lt 3 ]; then
        # Scenario shortfall, not a governance failure: the raising handle
        # simply did not act enough times afterwards for the calm counter to
        # reach deescalate_sustained. SKIP naming why, rather than failing.
        skip V3-11 "de-escalation not evaluable" \
             "longest run of consecutive calm turns from the raising handle was ${post:-0}; needs >= deescalate_sustained (3)"
    else
        # The precondition held and nothing stepped down. This branch was a
        # SKIP while I believed the hysteresis could not fire at all; the run
        # proved otherwise, so an absence here is now a real failure and must
        # report as one. A gate that skips on both of its negative branches
        # cannot fail, which is the defect this whole harness exists to prevent.
        gk_fail V3-11 "de-escalation did not fire despite ${post} calm turns from the raising handle" \
             "hysteresis is known to work (high -> elevated observed); with genuinely calm turns available, absence here is a regression"
    fi
}
