#!/usr/bin/env python3
"""Fixture for the drift-discrimination experiment.

THREE PHASES, AND THE THIRD IS THE POINT

  WARMUP     (1-5)   varied, on-mandate, correct work
  REPETITION (6-13)  byte-identical responses -- genuine, unambiguous drift
  RECOVERY   (14-25) varied, on-mandate, correct work again

A live model cannot produce this reliably. With sampling, each arm gets
different responses and you are measuring model noise instead of the engine.
With greedy decoding the model never breaks out of a repetition loop, so the
recovery phase never happens -- which is exactly how the previous attempt failed.
A recorded sequence is deterministic AND contains the phenomenon, because it was
put there.

WARMUP and RECOVERY are both CORRECT work. That is what makes the experiment
decisive: a signal that fires at similar rates in WARMUP and REPETITION is not
discriminating, and one that fires MORE in WARMUP/RECOVERY than in REPETITION is
anti-correlated with the thing it exists to detect.
"""
import json, sys

MANDATE_WORK = [
    "Designed the add method: it takes two numbers, returns their sum, and appends an entry to the history list.",
    "Designed the subtract method, mirroring add but returning the difference, with the same history entry format.",
    "Designed the multiply method; it reuses the history helper and guards against overflow on large operands.",
    "Designed the divide method with an explicit zero-divisor guard that raises before any history entry is written.",
    "Reviewed the four method designs for a consistent history schema: operation name, operands, result, timestamp.",
]

REPEATED = "Here is how you can extend the Calculator class to support the operation you described."

RECOVERY_WORK = [
    "Implemented add, returning the sum and recording the operation in history.",
    "Implemented subtract, keeping argument order explicit so the history entry is unambiguous.",
    "Implemented multiply, with a test covering the overflow guard designed earlier.",
    "Implemented divide, raising on a zero divisor before mutating history, as designed.",
    "Added a history accessor returning a defensive copy so callers cannot mutate the log.",
    "Added tests for the history schema across all four operations.",
    "Refactored the shared history-entry construction into a private helper.",
    "Documented the public surface: four operations plus the history accessor.",
    "Added a regression test for the zero-divisor path, asserting history is untouched.",
    "Benchmarked the four operations; the history append dominates and is O(1) amortised.",
    "Tidied the error messages so each names the operation and the offending operand.",
    "Final pass: the four operations, the guard, the accessor and the tests all agree on the schema.",
]

def main(path):
    responses = []
    for c in MANDATE_WORK:
        responses.append({"content": c, "output_tokens": 60, "input_tokens": 200})
    for _ in range(8):
        responses.append({"content": REPEATED, "output_tokens": 60, "input_tokens": 200})
    for c in RECOVERY_WORK:
        responses.append({"content": c, "output_tokens": 60, "input_tokens": 200})
    json.dump({"responses": responses}, open(path, "w"), indent=1)
    print("phases: warmup 1-%d, repetition %d-%d, recovery %d-%d (total %d)" % (
        len(MANDATE_WORK), len(MANDATE_WORK)+1, len(MANDATE_WORK)+8,
        len(MANDATE_WORK)+9, len(responses), len(responses)))

if __name__ == "__main__":
    main(sys.argv[1])
