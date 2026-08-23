#!/usr/bin/env python3
"""Ground-truth scenarios for "did escalating help?"

WHY THIS EXISTS

escalation_effectiveness is defined as mean(coherence over the N turns after an
escalation) minus coherence at the escalation. Nothing consumes it, and before
wiring it to a decision the question is which QUANTITY it should measure — a
semantics question, which a first pass answered by argument and got wrong.

Answered by measurement instead: build scenarios whose ground truth is known,
compute candidate definitions over them, and keep the definition that
classifies them all. A four-scenario pilot already produced one decisive
result — no (offset, window) anywhere in a 42-point search makes the CURRENT
coherence-delta definition separate helped / no-effect / worse. It is not
mistuned, it is measuring a quantity that saturates. This file widens that
pilot enough to set parameters rather than merely reject a definition.

GROUND TRUTH IS READ, NOT ASSERTED

Each scenario names what it intends, but the intent is not the evidence: the
per-turn PENALTY TOTAL from CDD_TURN is, because it does not saturate at the
coherence floor. The pilot's first draft labelled byte-identical repetition
"mild" and topic abandonment "severe" — exactly backwards, since repetition
fires the objective signals (exempt from absorption, ~0.25/turn) and
abandonment fires statistical ones that calibration absorbs. The penalty trace
caught it. run.sh re-derives every ground truth from the trace and refuses to
report on any scenario whose measured behaviour contradicts its label.

DIMENSIONS SWEPT, and why each is suspected of changing the answer

  response lag     turns between the escalation and any behaviour change. The
                   pilot's `helped` had a lag of 3, which put half the window
                   before the agent responded at all — that alone is why the
                   current definition scored it NEGATIVE. Lag directly sets the
                   right offset, so it must be varied rather than fixed.
  drift type       objective (byte-identical repetition, ~0.25/turn and
                   unabsorbable) vs statistical (topic abandonment, absorbed
                   once calibrated). Penalties differ by ~3x, so a definition
                   tuned on one may fail on the other.
  escalation point mid-range vs at the coherence floor. The saturation problem
                   only bites at the floor, so a scenario set without a floored
                   escalation cannot see it.
  recovery shape   full, partial, and relapse. A definition that only handles
                   clean binary outcomes is overfitted to easy cases.
  multiplicity     two escalations in one run, to exercise the re-arm.
"""
import json
import os
import sys

MANDATE = ("Build a Calculator class with add, subtract, multiply and divide "
           "methods, each recording an entry in a history log.")

CALIBRATE = 6   # correct turns so the adaptive baseline forms on correct work

CORRECT = [
 "Implemented the add method, returning the sum and recording the operation in the history log.",
 "Wrote subtract next, returning the difference with the same history entry format.",
 "Multiply guards against overflow on large operands before writing anything.",
 "Divide raises on a zero divisor before any history entry is written.",
 "Added a history accessor returning a defensive copy of the recorded log.",
 "Added tests covering the history schema across all four arithmetic methods.",
 "Refactored the shared history entry construction into a private helper.",
 "Benchmarked the four methods; the history append stays constant time.",
 "Tidied error messages so each names the operation and the operand.",
 "Documented the public surface: four methods plus the history accessor.",
 "Added a regression test for the zero-divisor path.",
 "Final sweep: methods, guard, accessor and tests agree on the schema.",
] * 4

# OBJECTIVE drift — byte-identical. Fires S1/S21, which are exempt from
# absorption and charged during the baseline, so it costs ~0.25/turn reliably.
HEAVY = ["Here is how you can extend the Calculator class to support the operation you described."] * 40

# STATISTICAL drift — every turn a different unrelated topic. Fires the
# overlap-based signals only, which calibration absorbs, so it costs far less.
LIGHT = [
 "Arctic terns migrate eleven thousand miles between polar breeding grounds.",
 "Flying buttresses let medieval masons raise cathedral walls far higher.",
 "Volcanic soils lend mineral character to vines grown on temperate slopes.",
 "Zugzwang describes a position where any legal move worsens your standing.",
 "Baroque counterpoint weaves independent melodic lines into one fabric.",
 "Hydrothermal vents support chemosynthetic life beneath the photic zone.",
 "Moraine ridges record the furthest advance of alpine glaciers.",
 "Linen thread and wheat paste bind folded signatures in traditional books.",
 "Monsoon timing governs the planting calendar across the subcontinent.",
 "Fresnel lenses project a lighthouse beam past the visible horizon.",
 "Sourdough depends on wild yeasts and lactobacilli in balance.",
 "Early cartographers filled unknown coasts with conjecture and beasts.",
] * 4

TURNS = 26

def pad(seq, n):
    out = list(seq[:n])
    while len(out) < n:
        out.append(seq[len(out) % len(seq)])
    return out

# label -> (sequence, intended verdict, note)
SCENARIOS = {
    # --- recovery, varying the lag between escalation and response ----------
    "helped_lag0":    (CORRECT[:CALIBRATE] + HEAVY[:6] + CORRECT[:14],
                       "POSITIVE", "heavy drift, correct work resumes promptly"),
    "helped_lag3":    (CORRECT[:CALIBRATE] + HEAVY[:9] + CORRECT[:11],
                       "POSITIVE", "same, but drift persists ~3 turns past escalation"),
    "helped_partial": (CORRECT[:CALIBRATE] + HEAVY[:6] + LIGHT[:14],
                       "POSITIVE", "heavy drift gives way to lighter drift, not to correctness"),

    # --- no change ---------------------------------------------------------
    "no_effect":      (CORRECT[:CALIBRATE] + HEAVY[:20],
                       "ZERO", "heavy drift, unchanged by the escalation"),
    "no_effect_light":(CORRECT[:CALIBRATE] + LIGHT[:20],
                       "ZERO", "statistical drift, unchanged"),

    # --- deterioration -----------------------------------------------------
    "worse_sharp":    (CORRECT[:CALIBRATE] + LIGHT[:6] + HEAVY[:14],
                       "NEGATIVE", "light drift escalates, then turns heavy"),
    "worse_at_floor": (CORRECT[:CALIBRATE] + HEAVY[:8] + HEAVY[:12],
                       "NEGATIVE", "escalation lands with coherence already floored"),

    # --- harder shapes -----------------------------------------------------
    "relapse":        (CORRECT[:CALIBRATE] + HEAVY[:6] + CORRECT[:6] + HEAVY[:8],
                       "ZERO", "recovers, then returns to drift inside the horizon"),
    "double_escalate":(CORRECT[:CALIBRATE] + LIGHT[:5] + HEAVY[:7] + CORRECT[:8],
                       "POSITIVE", "two escalations; the re-arm must judge the second"),

    # --- control -----------------------------------------------------------
    "spurious":       (CORRECT[:TURNS],
                       "NONE", "correct throughout; any escalation here is spurious"),
}


def main(outdir):
    meta = {"mandate": MANDATE, "turns": TURNS, "calibrate": CALIBRATE, "scenarios": {}}
    for name, (seq, verdict, note) in SCENARIOS.items():
        seq = pad(seq, TURNS)
        with open(os.path.join(outdir, name + ".json"), "w") as f:
            json.dump({"responses": [{"content": c, "output_tokens": 40,
                                      "input_tokens": 200} for c in seq]}, f, indent=1)
        meta["scenarios"][name] = {"intended": verdict, "note": note}
    with open(os.path.join(outdir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("wrote %d scenarios, %d turns each" % (len(SCENARIOS), TURNS))


if __name__ == "__main__":
    main(sys.argv[1])
