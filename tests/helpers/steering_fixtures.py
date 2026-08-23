#!/usr/bin/env python3
"""Fixtures for test_steering_efficacy.sh.

WHAT THIS MAKES MEASURABLE

Every fixture in this campaign until now replayed fixed responses regardless of
what the engine injected, so no measurement taken here could detect steering
working OR failing. The claim "the correction helps the agent recover" was
untested by construction, in both directions.

agent_stub.py routes on request BODY CONTENT (`for k in routes: if k in body`),
so a fixture can answer differently once the engine's injected text appears in
the request. That is enough to separate two questions that look identical from
outside:

  is the steering loop wired end to end   — does the marker reach the body,
                                            and does the engine notice when the
                                            agent comes back?
  does a real model comply                — NOT measurable here, and this file
                                            does not claim to.

WHAT "RESPONSIVE" MODELS, AND WHAT IT DOES NOT

The stub selects a response list per request by matching the body, and holds no
memory between requests. So a routed fixture can express "complies on the turn
it is told" and CANNOT express "stays corrected afterwards" — on any turn
without a marker, the responsive arm returns to the drift list.

That makes this the PESSIMISTIC model of compliance: the responsive agent obeys
roughly half the turns and drifts the rest. Measured, that still separates
clearly from the inert arm (final coherence 0.085 vs 0.0, and it holds above
the admissibility threshold for 5 turns against 2), and coherence visibly RISES
on steered turns — 0.76, 0.79, 0.82, 0.85.

Read the gap as a lower bound on what steering is worth, never as a estimate of
it. An agent that stayed corrected would do better than this fixture can show,
and no fixture here can say whether a real model complies at all.

RESPONSIVE vs INERT

  responsive  routes on the injection marker and returns to the task when it
              sees one — an agent that takes the hint
  inert       has no routes and keeps drifting whatever it is told — an agent
              that does not

Both arms drift identically until the first injection. Any divergence after it
is caused by the injection and by nothing else, because the response stream is
otherwise byte-identical between the arms.

MARKERS come from agent_impl.cpp and must stay in sync with it:

  "[Task Reminder: "          mandate reinforcement (interval-based) AND the
                              gentle tier of coherence correction
  "[IMPORTANT - Stay on task" correction, coherence 0.5-0.7
  "[CRITICAL - You are off-task"  correction, coherence < 0.5

A marker that stops matching makes the responsive arm behave like the inert one
— the test would then report "steering does not work" for a reason that has
nothing to do with steering. SF-01 exists to catch exactly that: it asserts the
marker reached a logged request body before any recovery claim is read.
"""
import json
import os
import sys

MANDATE = ("Build a Calculator class with add, subtract, multiply and divide "
           "methods, each recording an entry in a history log.")

# Drift: byte-identical repetition. Chosen because it is the ONE failure the
# engine detects in every configuration measured (objective signals are exempt
# from absorption and charged during the baseline), so coherence provably falls
# and the steering trigger is provably reachable. A drift type the engine misses
# would make a silent test.
DRIFT = "Here is how you can extend the Calculator class to support the operation you described."

ON_TASK = [
    "Implemented the add method, returning the sum of its operands and recording the operation in the history log.",
    "Wrote subtract next, returning the difference and using the same history entry format.",
    "Multiply now guards against overflow on large operands before writing anything.",
    "Divide raises on a zero divisor before any history entry is written.",
    "Added a history accessor returning a defensive copy so callers cannot mutate the log.",
    "Added tests covering the history schema across all four arithmetic methods.",
    "Refactored the shared history entry construction into a private helper.",
    "Benchmarked the four methods; the history append dominates and stays constant time.",
    "Tidied the error messages so each names the operation and the offending operand.",
    "Documented the public surface: four methods plus the history accessor.",
    "Added a regression test for the zero-divisor path, asserting history is untouched.",
    "Final sweep: the four methods, the guard, the accessor and the tests agree.",
]

MARKERS = ["[Task Reminder: ", "[IMPORTANT - Stay on task", "[CRITICAL - You are off-task"]


def build(turns, responsive):
    fx = {"responses": [{"content": DRIFT, "output_tokens": 40, "input_tokens": 200}
                        for _ in range(turns)]}
    if responsive:
        # Every marker routes to the SAME on-task list so the arm recovers
        # whichever tier fires. Separate lists would make the result depend on
        # which tier the engine happened to pick.
        fx["routes"] = {}
        for m in MARKERS:
            fx["routes"][m] = {"responses": [
                {"content": c, "output_tokens": 40, "input_tokens": 200} for c in ON_TASK]}
    return fx


def main(outdir, turns):
    for name, responsive in (("responsive", True), ("inert", False)):
        with open(os.path.join(outdir, name + ".json"), "w") as f:
            json.dump(build(turns, responsive), f, indent=1)
    with open(os.path.join(outdir, "meta.json"), "w") as f:
        json.dump({"mandate": MANDATE, "turns": turns, "markers": MARKERS,
                   "drift": DRIFT}, f, indent=1)
    print("wrote responsive/inert, %d turns each" % turns)


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 14)
