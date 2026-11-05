#!/usr/bin/env python3
"""Compare `naab-gov taint` with what a run of the same program flags.

Bundle 12 PR 2, report only. For every .naab file under ROOT... whose OWN
govern.json (the one a run finds) makes the report list flows, this runs the
program on the VM under that config and compares, line by line:

  - the lines the run's taint check flags (taint_tracking.* results that did
    not pass, located by the file:line in the result's MESSAGE -- the result's
    own file/line fields can be a stale check location, observed), with
  - the report's VALUE flows (the explicit flows a run's check exists to judge).

    python3 tools/taintflow/crosscheck.py [--naab build/naab-lang] [--gov build/naab-gov] [--out DIR] ROOT...

Each program runs in a scratch copy of its config's directory with an empty
HOME (so no installed trust key signs or raises anything) and stdin from the
null device (a `command` prerequisite reads the caller's stdin, A122), for at
most 30 s.
Programs are the repository's own tests, run as the suite runs them.

A program whose run flagged nothing is UNMEASURABLE here, not agreement: an
outer gate can stop it before any taint check runs (several edge tests are
refused by code_quality.complexity_floor first), and a refused run flags
nothing. The numbers are observed for these programs under their own configs.

Exit 0 when it ran, 2 when naab-lang or naab-gov could not be run.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile


def report(gov, path):
    p = subprocess.run([gov, "taint", path, "--json"], capture_output=True, stdin=subprocess.DEVNULL, timeout=60)
    try:
        return json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        return {"result": "error"}


def run_flagged(naab, prog_dir, prog_name, work):
    home = os.path.join(work, "home")
    os.makedirs(home)
    rep = os.path.join(work, "rep.json")
    env = dict(os.environ, HOME=home)
    try:
        p = subprocess.run([naab, prog_name, "--governance-report", rep], cwd=prog_dir, env=env,
                           capture_output=True, stdin=subprocess.DEVNULL, timeout=30)
        rc = p.returncode
    except subprocess.TimeoutExpired:
        rc = "timeout"
    flagged = set()
    if os.path.exists(rep) and os.path.getsize(rep) > 0:
        try:
            d = json.load(open(rep, encoding="utf-8"))
        except ValueError:
            d = {}
        pat = re.compile(re.escape(prog_name) + r":(\d+)")
        for r in d.get("results", []):
            if str(r.get("rule", "")).startswith("taint_tracking") and not r.get("passed", True):
                for m in pat.findall(str(r.get("message", ""))):
                    flagged.add(int(m))
    return rc, flagged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--naab", default=os.path.join("build", "naab-lang"))
    ap.add_argument("--gov", default=os.path.join("build", "naab-gov"))
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    for b in (args.naab, args.gov):
        if not os.access(b, os.X_OK):
            print(f"crosscheck: {b} is not executable -- UNMEASURABLE")
            return 2
    naab = os.path.abspath(args.naab)
    gov = os.path.abspath(args.gov)

    files = []
    for root in args.roots:
        for dirpath, _, names in os.walk(root):
            if "/build" in dirpath or "/.cache" in dirpath:
                continue
            files += [os.path.join(dirpath, n) for n in sorted(names) if n.endswith(".naab")]
    files.sort()

    rows = []
    for f in files:
        rep = report(gov, f)
        if rep.get("result") != "flows":
            continue
        cfg = rep.get("config", "")
        if not cfg:
            continue
        root = os.path.dirname(cfg)
        rel = os.path.relpath(os.path.abspath(f), root)
        work = tempfile.mkdtemp(prefix="taintxc_")
        try:
            dst = os.path.join(work, "w")
            # follow links: a config dir may hold a relative link to a parent
            # config, which would dangle in the copy (observed)
            shutil.copytree(root, dst, symlinks=False)
            prog = os.path.join(dst, rel)
            rc, flagged = run_flagged(naab, os.path.dirname(prog), os.path.basename(prog), work)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        value = {fl["line"] for fl in rep.get("flows", []) if fl["kind"] == "value"}
        rows.append({"file": f, "rc": rc, "run_flagged": sorted(flagged), "value": sorted(value),
                     "run_not_value": sorted(flagged - value), "value_not_run": sorted(value - flagged)})

    measured = [r for r in rows if r["run_flagged"]]
    lines = [f"programs whose own config makes the report list flows: {len(rows)}",
             f"  the run flagged at least one line (measured): {len(measured)}",
             f"  the run flagged nothing (UNMEASURABLE here): {len(rows) - len(measured)}"]
    flagged_n = sum(len(r["run_flagged"]) for r in measured)
    both = sum(len(set(r["run_flagged"]) & set(r["value"])) for r in measured)
    value_n = sum(len(r["value"]) for r in measured)
    lines.append(f"lines the run flags: {flagged_n}; also a value flow in the report: {both}")
    lines.append(f"value flows in the report: {value_n}; also flagged by the run: {both}")
    for r in measured:
        if r["run_not_value"]:
            lines.append(f"  run flags, report has no value flow: {r['file']} {r['run_not_value']}")
    for r in measured:
        if r["value_not_run"]:
            lines.append(f"  report value flow, run flags nothing: {r['file']} {r['value_not_run']}")
    summary = "\n".join(lines)
    print(summary)
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "crosscheck.json"), "w", encoding="utf-8") as fh:
            json.dump(rows, fh, indent=1)
        with open(os.path.join(args.out, "crosscheck.md"), "w", encoding="utf-8") as fh:
            fh.write(summary + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
