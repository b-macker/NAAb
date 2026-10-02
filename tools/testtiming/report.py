#!/usr/bin/env python3
"""report.py -- summarise a timeout-shim log (calls.tsv) into Markdown or JSON.

Reads the log on STDIN and writes the report to STDOUT. It never opens a path:
under MSYS2, python3 is a native Windows build and a path that reaches it
unconverted cannot be opened (CLAUDE.md, "a test's OUTPUT CHANNEL...").
Output is ASCII only, written as bytes, so the platform's stdout encoding and
newline translation cannot change it.

Standard library only; written for Python 3.8 and later.

Each log line: start <TAB> end <TAB> exit code <TAB> cwd <TAB> timeout's argv.

Which calls count. run-all-tests.sh starts every shell suite as
`timeout [-k K] D bash <suite> [args]` and every single .naab test as
`timeout D ./build/naab-lang ... <file>`. Suites call `timeout` themselves
too, so the log also holds calls made INSIDE a suite. Those are excluded by
structure, not by path: the top-level runner starts suites one at a time, so
a call that lies inside another suite call's time window is that suite's own
work and is already inside its duration. Paths are not compared, because on
Windows the same directory can be spelled two ways (/d/a/... and D:/a/...).
"""
import argparse
import json
import re
import sys

SUITE_RE = re.compile(r"^(?:-k \S+ )?\S+ bash (\S+)(.*)$")
NAAB_MARK = "./build/naab-lang"


def parse(data):
    rows = []
    for raw in data.splitlines():
        parts = raw.split("\t", 4)
        if len(parts) != 5:
            continue
        try:
            t0, t1, rc = float(parts[0]), float(parts[1]), int(parts[2])
        except ValueError:
            continue
        rows.append({"t0": t0, "t1": t1, "rc": rc, "argv": parts[4]})
    return rows


def classify(rows):
    suites, naab = [], []
    for r in rows:
        m = SUITE_RE.match(r["argv"])
        if m:
            r["kind"], r["name"], r["args"] = "suite", m.group(1), m.group(2).strip()
            suites.append(r)
        elif NAAB_MARK in r["argv"]:
            files = [t for t in r["argv"].split() if t.endswith(".naab")]
            r["kind"], r["name"], r["args"] = "naab", (files[-1] if files else r["argv"]), r["argv"]
            naab.append(r)

    # Containment against suite windows only. O(n*m) is fine at this size
    # (a full run is ~2,000 calls and ~260 suites).
    windows = sorted((s["t0"], s["t1"], id(s)) for s in suites)

    def nested(r):
        for a, b, ident in windows:
            if ident != id(r) and a <= r["t0"] and r["t1"] <= b and (a, b) != (r["t0"], r["t1"]):
                return True
        return False

    top_suites = [s for s in suites if not nested(s)]
    top_naab = [n for n in naab if not nested(n)]
    return top_suites, top_naab


def dur(r):
    return r["t1"] - r["t0"]


def build(rows, start, end, exit_code):
    suites, naab = classify(rows)
    wall = max(end - start, 0.0)
    st = sum(dur(s) for s in suites)
    nt = sum(dur(n) for n in naab)
    return {
        "schema": 1,
        "exit_code": exit_code,
        "wall_seconds": round(wall, 3),
        "suite_seconds": round(st, 3),
        "naab_test_seconds": round(nt, 3),
        "unattributed_seconds": round(wall - st - nt, 3),
        "calls_logged": len(rows),
        "suites": sorted(
            ({"name": s["name"], "args": s["args"], "seconds": round(dur(s), 3), "exit": s["rc"]}
             for s in suites), key=lambda x: -x["seconds"]),
        "naab_tests": sorted(
            ({"name": n["name"], "seconds": round(dur(n), 3), "exit": n["rc"]}
             for n in naab), key=lambda x: -x["seconds"]),
    }


def pct(part, whole):
    return "%d%%" % round(100.0 * part / whole) if whole > 0 else "-"


def markdown(rep):
    w = rep["wall_seconds"]
    st = rep["suite_seconds"]
    suites = rep["suites"]
    out = []
    out.append("## Test timing (report only)")
    out.append("")
    out.append("The suite verdict is run-all-tests.sh's own exit status (%d); this report cannot change it."
               % rep["exit_code"])
    out.append("")
    out.append("| | seconds | share of wall |")
    out.append("|---|---:|---:|")
    out.append("| wall clock | %.0f | 100%% |" % w)
    out.append("| shell suites (%d) | %.0f | %s |" % (len(suites), st, pct(st, w)))
    out.append("| single .naab tests (%d) | %.0f | %s |"
               % (len(rep["naab_tests"]), rep["naab_test_seconds"], pct(rep["naab_test_seconds"], w)))
    out.append("| unattributed (runner overhead, work not run under timeout) | %.0f | %s |"
               % (rep["unattributed_seconds"], pct(rep["unattributed_seconds"], w)))
    out.append("")
    if suites:
        out.append("Concentration: top 10 suites = %s, top 20 = %s, top 40 = %s of suite time."
                   % tuple(pct(sum(s["seconds"] for s in suites[:k]), st) for k in (10, 20, 40)))
        buckets = [("< 5 s", 0, 5), ("5-30 s", 5, 30), ("30-120 s", 30, 120), (">= 120 s", 120, float("inf"))]
        out.append("Suites by duration: " + ", ".join(
            "%s: %d" % (label, sum(1 for s in suites if lo <= s["seconds"] < hi)) for label, lo, hi in buckets) + ".")
        out.append("")
        out.append("### Slowest suites")
        out.append("")
        out.append("| # | seconds | cumulative | exit | suite |")
        out.append("|---:|---:|---:|---:|---|")
        cum = 0.0
        for i, s in enumerate(suites[:25], 1):
            cum += s["seconds"]
            name = s["name"] + ((" " + s["args"]) if s["args"] else "")
            out.append("| %d | %.1f | %s | %d | `%s` |" % (i, s["seconds"], pct(cum, st), s["exit"], name))
        out.append("")
    if rep["naab_tests"]:
        out.append("### Slowest single .naab tests")
        out.append("")
        out.append("| seconds | exit | test |")
        out.append("|---:|---:|---|")
        for n in rep["naab_tests"][:10]:
            out.append("| %.1f | %d | `%s` |" % (n["seconds"], n["exit"], n["name"]))
        out.append("")
    timed_out = [s["name"] for s in suites if s["exit"] in (124, 137)] + \
                [n["name"] for n in rep["naab_tests"] if n["exit"] in (124, 137)]
    out.append("Timed out (exit 124/137): %s" % (", ".join("`%s`" % t for t in timed_out) if timed_out else "none"))
    out.append("")
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--format", choices=("md", "json"), default="md")
    ap.add_argument("--start", type=float, required=True)
    ap.add_argument("--end", type=float, required=True)
    ap.add_argument("--exit-code", type=int, required=True)
    a = ap.parse_args(argv)
    data = sys.stdin.buffer.read().decode("utf-8", errors="replace")
    rep = build(parse(data), a.start, a.end, a.exit_code)
    if a.format == "json":
        text = json.dumps(rep, indent=1, ensure_ascii=True) + "\n"
    else:
        text = markdown(rep)
    sys.stdout.buffer.write(text.encode("ascii", errors="replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
