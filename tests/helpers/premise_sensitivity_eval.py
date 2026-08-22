#!/usr/bin/env python3
"""Verdict analysis for test_premise_sensitivity.sh.

In its own file, and driven by --selftest, for the same reason as
signal_contract_eval.py: analysis embedded in the shell script can only ever
be exercised against the live sweep, where a pass is ambiguous between "the
engine behaves as recorded" and "the check cannot fail". Here every assertion
is run against synthetic grids that violate it.

Input (stdin): TSV, one row per cell —
    calibration<TAB>healing<TAB>window<TAB>err<TAB>correct floors<TAB>drift floors
Args: margin, declared-load-bearing (space separated), default verdict
Output (stdout): JSON analysis.
"""
import json
import sys

KEYS = {"calibration": "cal", "healing": "heal", "window": "win"}


def verdict_of(cf, df, margin):
    sep = (sum(cf) / len(cf)) - (sum(df) / len(df))
    if sep > margin:
        return sep, "DISCRIMINATES"
    if sep < -margin:
        return sep, "INVERTED"
    return sep, "BLIND"


def parse(lines):
    rows = []
    for ln in lines:
        f = ln.rstrip("\n").split("\t")
        if len(f) != 6:
            continue
        row = {"cal": f[0], "heal": f[1], "win": f[2], "err": f[3] == "1"}
        if not row["err"]:
            row["cf"] = [float(x) for x in f[4].split()]
            row["df"] = [float(x) for x in f[5].split()]
        rows.append(row)
    return rows


def analyze(rows, margin, declared, default_verdict):
    for r in rows:
        if not r["err"]:
            r["sep"], r["verdict"] = verdict_of(r["cf"], r["df"], margin)

    live = [r for r in rows if not r["err"]]
    out = {"rows": rows, "errors": sum(1 for r in rows if r["err"]),
           "verdicts": sorted({r["verdict"] for r in live})}

    # A premise is LOAD-BEARING when, holding the OTHERS FIXED, flipping only
    # it changes the verdict. Comparing marginals instead would let two
    # premises mask each other: if A only matters when B is set, a marginal
    # comparison averages the effect away and reports both as inert.
    flips = {}
    for name, k in KEYS.items():
        others = [x for x in ("cal", "heal", "win") if x != k]
        found = []
        for r in live:
            for s in live:
                if r is s:
                    continue
                if all(r[o] == s[o] for o in others) and r[k] != s[k]:
                    if r["verdict"] != s["verdict"]:
                        found.append("%s=%s->%s flips %s->%s (others %s)" % (
                            name, r[k], s[k], r["verdict"], s["verdict"],
                            ",".join("%s=%s" % (o, r[o]) for o in others)))
        flips[name] = sorted(set(found))

    observed = sorted(n for n, f in flips.items() if f)
    out["flips"] = flips
    out["load_bearing_observed"] = observed
    out["load_bearing_declared"] = sorted(declared)
    out["undeclared"] = sorted(set(observed) - declared)
    out["stale"] = sorted(declared - set(observed))

    # The engine defaults: adaptive_baseline_enabled false, window 5, and
    # coherence_natural_healing unset (0.0). This cell is the stock config.
    d = [r for r in live
         if r["cal"] == "false" and r["heal"] == "0.0" and r["win"] == "5"]
    out["default_cell"] = d[0] if d else None
    out["default_expected"] = default_verdict
    return out


# --------------------------------------------------------------------------
# Vacuity: every assertion must FAIL on a grid known to violate it, and the
# clean grid must report nothing. Both directions — a checker that always
# reports a violation passes the negative cases alone.
# --------------------------------------------------------------------------
def _grid(fn):
    """Build an 8-cell grid; fn(cal,heal,win) -> (correct floors, drift floors)."""
    lines = []
    for cal in ("false", "true"):
        for heal in ("0.0", "0.03"):
            for win in ("5", "12"):
                cf, df = fn(cal, heal, win)
                lines.append("%s\t%s\t%s\t0\t%s\t%s" % (
                    cal, heal, win,
                    " ".join("%.3f" % x for x in cf),
                    " ".join("%.3f" % x for x in df)))
    return lines


def _real(cal, heal, win):
    """Approximates the measured engine: calibration decides, nothing else."""
    if cal == "true":
        return [1.0, 1.0, 0.975], [0.0, 0.452, 1.0]
    return [0.0, 0.0, 0.0], [0.0, 0.0, 0.0]


def selftest():
    failures = []
    M = 0.15

    def check(name, rows, declared, default, want):
        a = analyze(rows, M, set(declared), default)
        got = {
            "undeclared": bool(a["undeclared"]),
            "stale": bool(a["stale"]),
            "default_mismatch": (a["default_cell"] or {}).get("verdict") != a["default_expected"],
            "single_verdict": len(a["verdicts"]) < 2,
        }
        if got != want:
            failures.append("%s: expected %s, got %s" % (name, want, got))

    clean = {"undeclared": False, "stale": False,
             "default_mismatch": False, "single_verdict": False}

    # Positive control. Without it, an analyzer that flags everything passes
    # every negative case below.
    check("real grid, correctly declared", parse(_grid(_real)),
          ["calibration"], "BLIND", clean)

    # PS-02, undeclared direction: a premise decides the verdict and is not
    # declared. This is the shape of every error in this campaign.
    w = dict(clean); w["undeclared"] = True; w["stale"] = True
    check("undeclared premise is caught", parse(_grid(_real)), ["healing"], "BLIND", w)

    # PS-02, stale direction: declared load-bearing, no longer is.
    w = dict(clean); w["stale"] = True
    check("stale declaration is caught", parse(_grid(_real)),
          ["calibration", "window"], "BLIND", w)

    # PS-03: the default cell's verdict changed.
    w = dict(clean); w["default_mismatch"] = True
    check("default verdict change is caught", parse(_grid(_real)),
          ["calibration"], "DISCRIMINATES", w)

    # PS-01: an inert sweep. Every cell identical — nothing is being varied,
    # so "no undeclared premise" is true only because nothing was measured.
    flat = _grid(lambda c, h, w_: ([1.0, 1.0, 1.0], [0.0, 0.0, 0.0]))
    w = dict(clean); w["single_verdict"] = True; w["stale"] = True
    check("inert sweep is caught", parse(flat), ["calibration"], "DISCRIMINATES", w)

    # Interaction: a premise that matters ONLY when another is set must still
    # be found. Marginal comparison averages this away and reports it inert.
    def interact(cal, heal, win):
        if cal == "true" and win == "5":
            return [1.0, 1.0, 1.0], [0.0, 0.0, 0.0]
        return [0.5, 0.5, 0.5], [0.5, 0.5, 0.5]
    a = analyze(parse(_grid(interact)), M, {"calibration", "window"}, "BLIND")
    if "window" not in a["load_bearing_observed"]:
        failures.append("interaction: window matters only when cal=true and was "
                        "reported inert — held-fixed comparison is not working")

    # A grid where a premise genuinely does nothing must NOT be reported.
    a = analyze(parse(_grid(_real)), M, {"calibration"}, "BLIND")
    for inert in ("healing", "window"):
        if inert in a["load_bearing_observed"]:
            failures.append("%s reported load-bearing on a grid where it does "
                            "nothing" % inert)

    for f in failures:
        print("SELFTEST FAIL: " + f)
    print("selftest: 8 checks, %d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    margin = float(sys.argv[1])
    declared = set(w for w in sys.argv[2].split() if w)
    print(json.dumps(analyze(parse(sys.stdin), margin, declared, sys.argv[3])))
