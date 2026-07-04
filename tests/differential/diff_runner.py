#!/usr/bin/env python3
"""Differential harness v2: run every corpus program on both engines and
compare normalized stdout + exit codes. Python3 stdlib only.

Per corpus entry:
  1. determinism pre-screen: run the VM twice; differing output -> NONDET skip
  2. run VM and tree-walker (--no-governance, cwd = file's directory)
  3. normalize (ANSI strip, rstrip, error-block split) and compare;
     for error cases compare the error CATEGORY, not the wording (absorbs
     the terse-VM vs help-text-TW message fork)
  4. mismatch -> divergences.json detector match => KNOWN (non-failing),
     otherwise FAIL

Usage: diff_runner.py --naab <bin> --corpus corpus.list --root <repo-root>
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))

from naabfuzz.runner import split_error_block  # noqa: E402
from naabfuzz.triage import classify_error     # noqa: E402

TIMEOUT = 20


# ------------------------------------------------------------- detectors
# Named detector functions referenced by divergences.json "detect" fields.
# Each takes (vm, tw) result dicts and returns True if the divergence is
# the registered one.

def det_mixed_compare_fork(vm, tw):
    """VM rejects string<->number ordering comparisons; TW coerces."""
    return ("Cannot compare" in vm["error"] and tw["rc"] == 0) or \
           ("Cannot compare" in tw["error"] and vm["rc"] == 0)


def det_bool_arith_fork(vm, tw):
    """VM rejects bool operands in + - * /; TW treats bool as numeric."""
    vm_msg = vm["error"]
    return (("Cannot add bool" in vm_msg or "Cannot subtract" in vm_msg
             or "Cannot multiply" in vm_msg or "bool" in vm_msg.split("\n")[0])
            and vm["rc"] == 1 and tw["rc"] == 0)


def det_tw_feature_gap(vm, tw):
    """VM supports string iteration/subscript and array dot-notation that
    the tree-walker rejects with a type error."""
    if not (vm["rc"] == 0 and tw["rc"] != 0):
        return False
    markers = ("Cannot iterate over string",
               "Subscript operation not supported",
               "don't support dot notation")
    return any(m in tw["error"] for m in markers)


DETECTORS = {
    "mixed_compare_fork": det_mixed_compare_fork,
    "bool_arith_fork": det_bool_arith_fork,
    "tw_feature_gap": det_tw_feature_gap,
}


def run_engine(naab, path, tree_walk):
    cmd = [naab, "--no-governance", "--timeout", "15"]
    if tree_walk:
        cmd.append("--tree-walk")
    cmd.append(os.path.basename(path))
    try:
        p = subprocess.run(cmd, cwd=os.path.dirname(os.path.abspath(path)),
                           capture_output=True, text=True, timeout=TIMEOUT,
                           errors="replace",
                           env={**os.environ, "NO_COLOR": "1", "TERM": "dumb"})
    except subprocess.TimeoutExpired:
        return {"rc": 124, "lines": [], "error": "", "timeout": True}
    lines, err = split_error_block(p.stdout)
    return {"rc": p.returncode, "lines": lines, "error": err,
            "timeout": False}


def compare(vm, tw):
    """Returns None if equal, else a short mismatch description."""
    if vm["timeout"] or tw["timeout"]:
        if vm["timeout"] and tw["timeout"]:
            return None  # both hung identically — corpus problem, not a fork
        return "timeout: vm=%s tw=%s" % (vm["timeout"], tw["timeout"])
    if vm["rc"] != tw["rc"]:
        return "rc: vm=%d tw=%d" % (vm["rc"], tw["rc"])
    if vm["lines"] != tw["lines"]:
        for i, (a, b) in enumerate(zip(vm["lines"], tw["lines"])):
            if a != b:
                return "line %d: vm=%r tw=%r" % (i, a[:200], b[:200])
        return "line count: vm=%d tw=%d" % (len(vm["lines"]), len(tw["lines"]))
    if (vm["error"] == "") != (tw["error"] == ""):
        return "error presence: vm=%r tw=%r" % (bool(vm["error"]),
                                                bool(tw["error"]))
    if vm["error"] and classify_error(vm["error"]) != classify_error(tw["error"]):
        return "error category: vm=%s tw=%s" % (classify_error(vm["error"]),
                                                classify_error(tw["error"]))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--naab", required=True)
    ap.add_argument("--corpus", default=os.path.join(HERE, "corpus.list"))
    ap.add_argument("--root", default=os.path.join(HERE, "..", ".."))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    naab = os.path.abspath(args.naab)
    root = os.path.abspath(args.root)

    with open(os.path.join(HERE, "divergences.json")) as f:
        registry = json.load(f)["divergences"]
    active = [(d, DETECTORS[d["detect"]]) for d in registry
              if d["detect"] in DETECTORS]

    passed = failed = known = nondet = 0
    with open(args.corpus) as f:
        entries = [ln.split()[0] for ln in f
                   if ln.strip() and not ln.startswith("#")]

    for rel in entries:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            print("MISSING %s" % rel)
            failed += 1
            continue
        # determinism pre-screen
        r1 = run_engine(naab, path, False)
        r2 = run_engine(naab, path, False)
        if r1 != r2:
            nondet += 1
            if args.verbose:
                print("NONDET  %s" % rel)
            continue
        tw = run_engine(naab, path, True)
        mismatch = compare(r1, tw)
        if mismatch is None:
            passed += 1
            if args.verbose:
                print("PASS    %s" % rel)
            continue
        hit = next((d for d, det in active if det(r1, tw)), None)
        if hit is not None:
            known += 1
            print("KNOWN   %s [%s] %s" % (rel, hit["id"], mismatch))
            continue
        failed += 1
        print("FAIL    %s: %s" % (rel, mismatch))
        print("  vm rc=%d lines=%r err=%r" % (r1["rc"], r1["lines"][:3],
                                              r1["error"][:200]))
        print("  tw rc=%d lines=%r err=%r" % (tw["rc"], tw["lines"][:3],
                                              tw["error"][:200]))

    print("=== Differential v2: %d passed, %d failed, %d known, "
          "%d nondet-skipped ===" % (passed, failed, known, nondet))
    return 1 if failed > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
