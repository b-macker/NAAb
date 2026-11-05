#!/usr/bin/env python3
"""Measure the static information-flow report over a tree of NAAb programs.

Bundle 12 PR 2, report only: `naab-gov taint` refuses nothing, and this tool is
how its findings are counted before anyone decides whether to enforce them.

    python3 tools/taintflow/measure.py [--gov build/naab-gov] [--out DIR] ROOT...

For every .naab file under each ROOT it runs `naab-gov taint FILE --json`
twice:
  - under PROFILE (below), a fixed policy written for this measurement, so
    every file is judged by the same sources and sinks -- CONFIGURED numbers:
    they say what the analysis finds under this policy, not what any project
    enforces;
  - under the govern.json a run of the file would find, only to count how many
    files have taint tracking configured at all -- OBSERVED.

It writes DIR/results.json (per file) and DIR/summary.md, and prints the
summary. Nothing is classified here: whether a control flow is a real implicit
flow or a false alarm is read by hand from the listed findings
(--list-control prints them).

Exit 0 when every file was measured (a parse failure counts as measured: it is
reported), 2 when naab-gov could not be run.
"""
import argparse
import collections
import json
import os
import subprocess
import sys
import tempfile

# The measurement policy (configured). Sources: data from outside the program.
# Sinks: the ways out of it -- network, files, processes, shells, generated
# code. print is NOT a sink here: console output is where most programs report
# their own results, and counting it would measure printing, not egress.
PROFILE = {
    "version": "5.0",
    "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    "taint_tracking": {
        "enabled": True,
        "level": "advisory",
        "sources": ["env.get", "file.read", "io.read_line", "http.get", "http.request"],
        "sinks": ["http.post", "http.put", "http.patch", "http.request", "file.write",
                  "file.append", "process.run", "shell_exec", "codegen.run"],
        "sanitizers": ["sanitize_", "validate_", "escape_"],
    },
}


def run(gov, path, config=None, timeout=60):
    cmd = [gov, "taint", path, "--json"]
    if config:
        cmd += ["--config", config]
    try:
        p = subprocess.run(cmd, capture_output=True, stdin=subprocess.DEVNULL, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"result": "timeout"}
    out = p.stdout.decode("utf-8", "replace")
    try:
        d = json.loads(out)
    except ValueError:
        d = {"result": "error", "stderr": p.stderr.decode("utf-8", "replace")[-400:]}
    d["exit"] = p.returncode
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--gov", default=os.path.join("build", "naab-gov"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--list-control", action="store_true")
    args = ap.parse_args()
    if not os.access(args.gov, os.X_OK):
        print(f"measure: {args.gov} is not an executable naab-gov -- UNMEASURABLE")
        return 2

    files = []
    for root in args.roots:
        for dirpath, _, names in os.walk(root):
            if "/build" in dirpath or "/.cache" in dirpath:
                continue
            for n in sorted(names):
                if n.endswith(".naab"):
                    files.append(os.path.join(dirpath, n))
    files.sort()

    with tempfile.TemporaryDirectory() as td:
        profile = os.path.join(td, "govern.json")
        with open(profile, "w", encoding="utf-8") as f:
            json.dump(PROFILE, f)
        results = []
        for path in files:
            r = run(args.gov, path, config=profile)
            own = run(args.gov, path)
            results.append({"file": path, "profile": r,
                            "own_config": {"result": own.get("result"), "exit": own.get("exit"),
                                           "reason": own.get("reason", "")}})

    by_result = collections.Counter(r["profile"].get("result", "?") for r in results)
    kinds = collections.Counter()
    sinks = collections.Counter()
    blind = collections.Counter()
    files_with = collections.Counter()
    for r in results:
        flows = r["profile"].get("flows", [])
        seen = set()
        for fl in flows:
            kinds[fl["kind"]] += 1
            sinks[(fl["kind"], fl["sink"])] += 1
            seen.add(fl["kind"])
        for k in seen:
            files_with[k] += 1
        for what, n in r["profile"].get("not_analysed", {}).items():
            blind[what] += n
    # A file's own config configures taint unless the report stopped before
    # analysing (no config, taint off, no source or sink: it says why) or failed.
    own_taint = sum(1 for r in results
                    if r["own_config"]["result"] in ("flows", "none", "unmeasurable")
                    and not r["own_config"]["reason"])

    lines = []
    lines.append(f"files: {len(files)} (under {', '.join(args.roots)})")
    lines.append("results under the measurement profile (configured): " +
                 ", ".join(f"{k} {v}" for k, v in sorted(by_result.items())))
    lines.append(f"flows: value {kinds['value']} in {files_with['value']} files, "
                 f"control {kinds['control']} in {files_with['control']} files")
    for (k, s), n in sorted(sinks.items(), key=lambda x: (-x[1], x[0])):
        lines.append(f"  {k:7s} {s:14s} {n}")
    lines.append("not analysed (counted constructs):")
    for what, n in blind.most_common():
        lines.append(f"  {n:5d}  {what}")
    lines.append(f"files whose own govern.json configures taint sources and sinks (observed): {own_taint}")
    summary = "\n".join(lines)
    print(summary)
    if args.list_control:
        print("\ncontrol flows:")
        for r in results:
            for fl in r["profile"].get("flows", []):
                if fl["kind"] == "control":
                    print(f"  {r['file']}:{fl['line']}  {fl['sink']}  in {fl['function']}  -- {fl['origin']}")
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "results.json"), "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)
        with open(os.path.join(args.out, "summary.md"), "w", encoding="utf-8") as f:
            f.write(summary + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
