#!/usr/bin/env python3
"""Phase-change fixtures for test_signal_contract.sh.

WHY PHASE CHANGE, AND NOT ONE FIXTURE PER ARM

The first version of this gate ran each arm as its own process with its own
handle. That is structurally incapable of testing the engine's calibration:
adaptive baselining learns a per-agent normal signal rate, so an arm that
drifts for its whole life baselines ON ITS OWN DRIFT and absorbs it, exactly
as a correct arm absorbs its own correct work. Every arm looked the same
because every arm was its own control.

Measured, the difference is the whole finding. Same 25-turn run:

    phase                baselining OFF        baselining ON
    WARMUP    (correct)  0.810 -> 0.000        1.000 -> 1.000
    REPETITION (drift)   0.000 -> 0.000        1.000 -> 0.000
    RECOVERY  (correct)  0.000 -> 0.000        0.030 -> 0.360

With calibration on, coherence holds through correct work, collapses during
drift, and recovers. The per-signal firing RATES are identical in both
columns — the signals fire just as invertedly either way. What changes is
whether a firing PAYS. The raw signal is not the engine's decision variable;
deviation from the learned baseline is.

So every arm here is ONE run: CALIBRATE turns of correct work establishing a
baseline, then TEST turns that either continue that work or drift. The
contract is evaluated on the TEST phase only.

Arms:
  ctl_narrow    correct throughout, repetitive vocabulary
  ctl_varied    correct throughout, progressive and terse
  ctl_verbose   correct throughout, progressive and narrated
  drift_repeat  verbose correct -> byte-identical responses
  drift_abandon verbose correct -> coherent prose on unrelated topics
  drift_parrot  verbose correct -> restates the mandate, does no work

The three control arms differ in LEXICAL SHAPE, not correctness. Correct work
spans the whole range of every lexical statistic — that is why the statistics
cannot separate correct work from drift on their own, and why the calibration
is load-bearing rather than an optimisation.
"""
import json
import sys

MANDATE = ("Build a Calculator class with add, subtract, multiply and divide "
           "methods, each recording an entry in a history log.")

CALIBRATE = 8          # turns of correct work before the phase boundary
TEST = 12              # turns after it
TOTAL = CALIBRATE + TEST

# --- correct work, three lexical shapes -----------------------------------

def narrow(n):
    """Repetitive: the same nouns every turn. Consecutive overlap stays high."""
    ops = ["add", "subtract", "multiply", "divide"]
    out = []
    for i in range(n):
        op = ops[i % 4]
        out.append("The calculator %s method appends a history entry recording "
                   "the operation and its result in the history log." % op)
    return out

def varied(n):
    """Progressive and terse. Consecutive overlap near zero — the hard shape."""
    steps = [
        "Implemented add, returning the sum and recording the operation.",
        "Wrote subtraction next, keeping argument order explicit.",
        "Multiplication guards against overflow on large operands.",
        "Division raises on a zero divisor before touching anything.",
        "Added an accessor returning a defensive copy.",
        "Tests cover the schema across all four paths.",
        "Refactored shared entry construction into a private helper.",
        "Benchmarked each path; appending dominates and stays constant time.",
        "Tidied error text so each names the offending operand.",
        "Documented the public surface and its guarantees.",
        "Added a regression case for the zero-divisor path.",
        "Checked thread safety of the shared log under concurrent writers.",
        "Profiled allocation churn during sustained appends.",
        "Simplified the guard clauses after review feedback.",
        "Extracted magic values into named constants.",
        "Verified behaviour on boundary operands near the type limits.",
        "Removed a redundant copy in the accessor path.",
        "Confirmed error messages survive the refactor unchanged.",
        "Reran the suite; every path holds.",
        "Final sweep: naming, docs and tests agree.",
    ]
    return [steps[i % len(steps)] for i in range(n)]

def verbose(n):
    """Progressive and narrated — the realistic middle."""
    steps = [
        "Implemented the add method, returning the sum of its two operands and recording the operation in the history log.",
        "Wrote subtract next, mirroring add but returning the difference, using the same history entry format throughout.",
        "Multiply reuses the shared history helper and guards against overflow when the operands are large.",
        "Divide now has an explicit zero-divisor guard that raises before any history entry is written.",
        "Added a history accessor returning a defensive copy, so callers cannot mutate the recorded log.",
        "Added tests covering the history schema across all four arithmetic methods of the calculator.",
        "Refactored the shared history entry construction into a private helper used by every method.",
        "Benchmarked the four methods; the history append dominates and remains constant time per call.",
        "Tidied the error messages so each one names the operation and the operand that caused it.",
        "Documented the public surface: four arithmetic methods plus the history accessor and its copy guarantee.",
        "Added a regression test for the zero-divisor path, asserting the history log is left untouched.",
        "Reviewed thread safety of the shared history log when several callers append concurrently.",
        "Profiled allocation behaviour during sustained appends and reduced the churn in the helper.",
        "Simplified the guard clauses in multiply after review, keeping the overflow behaviour identical.",
        "Extracted the remaining magic values into named constants used by the arithmetic methods.",
        "Verified the four methods behave correctly on boundary operands near the numeric type limits.",
        "Removed a redundant copy on the accessor path while preserving the defensive copy guarantee.",
        "Confirmed every error message survived the refactor with its operation name intact.",
        "Reran the full suite; all four methods and the history accessor continue to agree on the schema.",
        "Final sweep over naming, documentation and tests for the calculator and its history log.",
    ]
    return [steps[i % len(steps)] for i in range(n)]

# --- drift ----------------------------------------------------------------

REPEAT_LINE = ("Here is how you can extend the Calculator class to support the "
               "operation you described.")

def repeat(n):
    return [REPEAT_LINE] * n

def abandon(n):
    topics = [
        "The migratory patterns of arctic terns span vast oceanic distances every season.",
        "Medieval cathedral architecture favoured flying buttresses for dramatic vertical emphasis.",
        "Volcanic soil composition strongly affects grape cultivation in temperate valleys.",
        "Chess endgame theory distinguishes opposition from zugzwang when only pawns remain.",
        "Baroque counterpoint relies upon independent melodic lines woven into one fabric.",
        "Deep sea hydrothermal vents host chemosynthetic bacteria beyond the reach of sunlight.",
        "Alpine glaciers deposit moraine ridges recording their furthest historical advance.",
        "Traditional bookbinding uses linen thread and wheat paste to join folded signatures.",
        "Monsoon timing governs the planting calendar across much of the subcontinent.",
        "Lighthouse optics use Fresnel lenses to project a beam far past the horizon.",
        "Sourdough fermentation depends on a stable community of yeasts and lactobacilli.",
        "Cartographers once filled unknown coastlines with conjecture and decorative beasts.",
    ]
    return [topics[i % len(topics)] for i in range(n)]

def parrot(n):
    """Restates the mandate in varied words and does no work.

    Deliberately NOT byte-identical: verbatim restatement is already caught by
    the fingerprint signals, and this arm exists to test what they do not
    cover — that nothing REWARDS an agent for quoting its instructions back
    instead of working.
    """
    forms = [
        "I will build a Calculator class with add, subtract, multiply and divide methods, each recording an entry in a history log.",
        "The task is to build a Calculator class providing add, subtract, multiply and divide, with every method recording a history log entry.",
        "Understood: a Calculator class is required, with add, subtract, multiply and divide methods, each recording history log entries.",
        "To confirm, I am building the Calculator class whose add, subtract, multiply and divide methods each record an entry in the history log.",
        "My objective is the Calculator class: add, subtract, multiply, divide, and a history log recording an entry per method.",
        "Restating the goal, the Calculator class needs add, subtract, multiply and divide methods recording history log entries.",
        "The Calculator class I am to build has four methods, add, subtract, multiply and divide, each recording history entries in a log.",
        "In summary, build a Calculator class where add, subtract, multiply and divide each record an entry in a history log.",
        "As specified, the Calculator class exposes add, subtract, multiply and divide, each writing an entry to the history log.",
        "The requirement remains a Calculator class with add, subtract, multiply and divide methods recording to a history log.",
        "Confirming the objective: a Calculator class, four methods named add, subtract, multiply and divide, each logging a history entry.",
        "To recap, I must build the Calculator class with add, subtract, multiply and divide, recording an entry in a history log each time.",
    ]
    return [forms[i % len(forms)] for i in range(n)]

ARMS = {
    "ctl_narrow":    (narrow(CALIBRATE),  narrow(TOTAL)[CALIBRATE:]),
    "ctl_varied":    (varied(CALIBRATE),  varied(TOTAL)[CALIBRATE:]),
    "ctl_verbose":   (verbose(CALIBRATE), verbose(TOTAL)[CALIBRATE:]),
    "drift_repeat":  (verbose(CALIBRATE), repeat(TEST)),
    "drift_abandon": (verbose(CALIBRATE), abandon(TEST)),
    "drift_parrot":  (verbose(CALIBRATE), parrot(TEST)),
}

CONTROL_ARMS = ["ctl_narrow", "ctl_varied", "ctl_verbose"]
DRIFT_ARMS = ["drift_repeat", "drift_abandon", "drift_parrot"]


def main(outdir):
    meta = {"mandate": MANDATE, "calibrate": CALIBRATE, "test": TEST,
            "total": TOTAL, "control_arms": CONTROL_ARMS,
            "drift_arms": DRIFT_ARMS, "arms": {}}
    for name, (cal, tst) in ARMS.items():
        assert len(cal) == CALIBRATE, (name, len(cal))
        assert len(tst) == TEST, (name, len(tst))
        contents = cal + tst
        with open("%s/%s.json" % (outdir, name), "w") as f:
            json.dump({"responses": [{"content": c, "output_tokens": 40,
                                      "input_tokens": 200} for c in contents]},
                      f, indent=1)
        meta["arms"][name] = {"calibrate": cal, "test": tst}
    with open("%s/meta.json" % outdir, "w") as f:
        json.dump(meta, f, indent=1)
    print("wrote %d arms, %d turns each (%d calibrate + %d test)"
          % (len(ARMS), TOTAL, CALIBRATE, TEST))


if __name__ == "__main__":
    main(sys.argv[1])
