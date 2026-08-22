#!/usr/bin/env python3
"""Failure-mode map for test_failure_mode_map.sh.

WHY A MAP AND NOT A VERDICT

Reporting one verdict per configuration answers one question, so each new
question needs a new premise, and every premise added to this campaign found a
failure mode nobody had predicted:

    calibration off               fires on everything, so catching the
                                  adversary carries no information
    calibration on, 8-turn run    off-mandate agent completes every send —
                                  the baseline window eats 5 of 8 turns and
                                  learns the drift IS this agent's normal
    parroting                     undetected in every configuration measured
    window                        inert at 20 turns, decisive at 8

That is whack-a-mole: the parameter space was never mapped, and each finding
was one point sampled from it by hand.

So this classifies every cell by WHAT WENT WRONG rather than by whether
something did:

    for each cell:  which CORRECT arms were falsely killed
                    which DRIFT arms escaped

and the assertion is set equality against a recorded manifest. A failure mode
appearing that is not in the manifest fails — including one nobody predicted.
A recorded mode disappearing fails too: either it was fixed (update the
manifest and everything resting on it) or the sweep stopped exercising it.

This does NOT enumerate every possible engine issue, and the report says so
out loud: it enumerates every failure mode reachable in the swept axes, and
prints the axes it does not sweep. Adding an axis is one entry in the grid.
"""
import json
import sys

CORRECT = ["ctl_narrow", "ctl_varied", "ctl_verbose"]
DRIFT = ["drift_repeat", "drift_abandon", "drift_parrot"]
SHORT = {"ctl_narrow": "narrow", "ctl_varied": "varied", "ctl_verbose": "verbose",
         "drift_repeat": "repeat", "drift_abandon": "abandon", "drift_parrot": "parrot"}


def classify(floors, oa):
    """floors: {arm: coherence floor}. Returns (code, killed, escaped).

    A CORRECT arm is falsely killed when its floor drops below the
    admissibility threshold. A DRIFT arm escapes when its floor stays at or
    above it. Both are stated against the same threshold the engine itself
    uses to decide admissibility, so the classification is the engine's own
    decision rather than a second opinion layered on top.
    """
    killed = [SHORT[a] for a in CORRECT if floors.get(a, 1.0) < oa]
    escaped = [SHORT[a] for a in DRIFT if floors.get(a, 0.0) >= oa]
    if not killed and not escaped:
        return "CLEAN", killed, escaped
    parts = []
    if escaped:
        parts.append("BYPASS[" + ",".join(escaped) + "]")
    if killed:
        parts.append("FALSE_KILL[" + ",".join(killed) + "]")
    return " ".join(parts), killed, escaped


def analyze(cells, oa, declared_manifest, unswept):
    """cells: list of {axes: {...}, floors: {arm: float}, err: bool}"""
    rows, errors = [], 0
    for c in cells:
        if c.get("err"):
            errors += 1
            rows.append({"axes": c["axes"], "err": True})
            continue
        code, killed, escaped = classify(c["floors"], oa)
        rows.append({"axes": c["axes"], "err": False, "code": code,
                     "killed": killed, "escaped": escaped,
                     "floors": c["floors"]})

    live = [r for r in rows if not r["err"]]
    observed = sorted({r["code"] for r in live})

    # Minimal witness per mode: the first cell exhibiting it, so every mode in
    # the report comes with a configuration that reproduces it.
    witness = {}
    for r in live:
        witness.setdefault(r["code"], r["axes"])

    return {
        "rows": rows,
        "errors": errors,
        "observed": observed,
        "declared": sorted(declared_manifest),
        "new_modes": sorted(set(observed) - set(declared_manifest)),
        "gone_modes": sorted(set(declared_manifest) - set(observed)),
        "witness": witness,
        "clean_cells": sum(1 for r in live if r["code"] == "CLEAN"),
        "total_cells": len(live),
        "unswept": unswept,
    }


# --------------------------------------------------------------------------
def _cell(rl, cal, win, floors):
    return {"axes": {"run": rl, "cal": cal, "win": win}, "floors": floors}


def selftest():
    f, OA = [], 0.70
    ok = dict(zip(CORRECT + DRIFT, [1.0, 1.0, 1.0, 0.0, 0.0, 0.0]))

    def chk(name, got, want):
        if got != want:
            f.append("%s: expected %s, got %s" % (name, want, got))

    # classify: the four shapes
    chk("clean", classify(ok, OA)[0], "CLEAN")
    chk("bypass", classify(dict(ok, drift_parrot=1.0), OA)[0], "BYPASS[parrot]")
    chk("false kill", classify(dict(ok, ctl_varied=0.0), OA)[0], "FALSE_KILL[varied]")
    chk("both", classify(dict(ok, ctl_varied=0.0, drift_parrot=1.0), OA)[0],
        "BYPASS[parrot] FALSE_KILL[varied]")
    # everything killed and nothing caught — the calibration-off shape
    chk("indiscriminate",
        classify(dict(zip(CORRECT + DRIFT, [0.0] * 6)), OA)[0],
        "FALSE_KILL[narrow,varied,verbose]")

    # A manifest that matches exactly must report nothing. Without this
    # positive control, an analyzer that flags every mode as new passes all
    # the negative cases below.
    cells = [_cell("long", "on", "5", ok)]
    a = analyze(cells, OA, ["CLEAN"], [])
    chk("matching manifest is silent", (a["new_modes"], a["gone_modes"]), ([], []))

    # An UNPREDICTED mode must surface. This is the case the whole file exists
    # for — a failure nobody thought to look for.
    cells = [_cell("long", "on", "5", ok),
             _cell("short", "on", "5", dict(ok, drift_abandon=1.0))]
    a = analyze(cells, OA, ["CLEAN"], [])
    chk("new mode surfaces", a["new_modes"], ["BYPASS[abandon]"])
    chk("new mode carries a witness",
        a["witness"].get("BYPASS[abandon]"), {"run": "short", "cal": "on", "win": "5"})

    # A manifest entry that no longer occurs must fail — fixed, or no longer
    # exercised. Silence there would let the manifest rot into fiction.
    a = analyze([_cell("long", "on", "5", ok)], OA, ["CLEAN", "BYPASS[parrot]"], [])
    chk("stale manifest entry surfaces", a["gone_modes"], ["BYPASS[parrot]"])

    # An errored cell is not a data point and must not be silently counted
    # clean — otherwise an environment where every run dies reports a perfect map.
    a = analyze([{"axes": {}, "err": True}], OA, [], [])
    chk("errored cell counted", (a["errors"], a["total_cells"]), (1, 0))

    for x in f:
        print("SELFTEST FAIL: " + x)
    print("selftest: 10 checks, %d failures" % len(f))
    return 1 if f else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    payload = json.load(sys.stdin)
    print(json.dumps(analyze(payload["cells"], payload["oa"],
                             payload["manifest"], payload["unswept"])))
