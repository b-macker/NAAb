#!/usr/bin/env python3
"""dead_gate.py -- every suite must be able to fail when the interpreter does nothing.

    python3 tools/testrunner/dead_gate.py run      [--jobs N] [--out DIR]
    python3 tools/testrunner/dead_gate.py classify RESULTS.json [--allowlist FILE]

`run` copies the working tree (a git worktree of HEAD, plus the working tree's
modified and untracked files, so uncommitted work is what gets tested),
replaces build/naab-lang in the copy with a DEAD interpreter -- a script that
prints nothing and exits 0 -- and runs every shell-phase unit against it with
tools/testrunner/parallel.py. The real build/ is never touched.

A unit passes the gate when, against the dead interpreter, it
  - fails or times out: it noticed the interpreter does nothing; or
  - passes but reports SKIP / UNMEASURABLE / XFAIL: it said it could not measure.
A unit that passes CLEAN -- no failure, no skip marker -- cannot tell a working
interpreter from one that does nothing. That FAILS the gate unless the unit is
in tests/self-audit/dead_interpreter_allowlist.txt with a reason:
  tool        it tests something other than naab-lang (naab-gov, a Python tool)
  structural  it checks source text, headers or files, not behaviour
  weak        it runs naab-lang but cannot fail on a dead one; it needs a
              positive control, and leaves the list when it gets one

An allowlist entry whose unit no longer passes clean is reported as STALE but
does not fail the gate: which units skip depends on the tools a machine has
(python3, compilers, curl), so failing on it would make the gate red on platform
differences instead of on weak tests. Remove a stale entry when you see one.

Where it found its keep: the first run (2026-10-03) caught two suites that printed
FAIL and exited 0 (test_bounded_healing.sh, test_cb_pressure_counter.sh) and
seven that cannot fail on a dead interpreter -- none visible in any green run.

Standard library only; POSIX only (git worktree, the dead interpreter is sh).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_ALLOWLIST = os.path.join(REPO, "tests", "self-audit", "dead_interpreter_allowlist.txt")
KINDS = ("tool", "structural", "weak")
DEAD = "#!/bin/sh\n# DEAD INTERPRETER (tools/testrunner/dead_gate.py): prints nothing, exits 0\nexit 0\n"
# Real binaries some units legitimately use. naab-lang is deliberately absent.
KEEP_BINARIES = ("naab-gov", "naab-verify-audit", "naab-lsp", "libnaab.a", "libnaab.so")


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))
    sys.stdout.flush()


def load_allowlist(path):
    entries = {}
    with open(path, "rb") as f:
        for n, raw in enumerate(f.read().decode("utf-8").splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 3 or parts[1] not in KINDS or not parts[2].strip():
                raise SystemExit("%s:%d: want <unit>TAB<%s>TAB<reason>, got %r"
                                 % (path, n, "|".join(KINDS), raw))
            if parts[0] in entries:
                raise SystemExit("%s:%d: %s listed twice" % (path, n, parts[0]))
            entries[parts[0]] = (parts[1], parts[2].strip())
    return entries


def classify(results, allow):
    """-> dict of lists: flagged (gate failures), stale, and the passing groups."""
    seen = set()
    r = {"failed": [], "skipped": [], "allowed": [], "flagged": [], "stale": []}
    for u in results["results"]:
        key = u["key"]
        seen.add(key)
        clean = u["verdict"] == "PASS" and u.get("skip_markers", 0) == 0
        if not clean:
            (r["skipped"] if u["verdict"] in ("PASS", "SKIP-TIMEOUT") else r["failed"]).append(key)
            if key in allow:
                r["stale"].append((key, "no longer passes clean (%s, %d skip markers)"
                                   % (u["verdict"], u.get("skip_markers", 0))))
        elif key in allow:
            r["allowed"].append(key)
        else:
            r["flagged"].append(key)
    for key in allow:
        if key not in seen:
            r["stale"].append((key, "not a listed unit any more"))
    return r


def report(r, allow, results):
    n = sum(len(r[k]) for k in ("failed", "skipped", "allowed", "flagged"))
    weak = [k for k in r["allowed"] if allow[k][0] == "weak"]
    out("## Dead-interpreter gate")
    out("")
    out("Against an interpreter that prints nothing and exits 0, %d units:" % n)
    out("  failed (they can fail)            %d" % len(r["failed"]))
    out("  passed, reporting skips (honest)  %d" % len(r["skipped"]))
    out("  passed clean, allowlisted         %d (of which %d WEAK, awaiting a positive control)"
        % (len(r["allowed"]), len(weak)))
    out("  passed clean, NOT allowlisted     %d" % len(r["flagged"]))
    if results.get("units_run") != results.get("units_listed"):
        out("")
        out("INCOMPLETE: %s of %s units ran -- the gate cannot pass on a partial run."
            % (results.get("units_run"), results.get("units_listed")))
    if r["flagged"]:
        out("")
        out("FLAGGED -- these pass with no working interpreter, so they would also pass")
        out("with a broken one. Give each a positive control (a check that the program")
        out("really ran and produced its expected output), or, if it does not test")
        out("naab-lang at all, list it in %s with a reason." % os.path.relpath(DEFAULT_ALLOWLIST, REPO))
        for k in r["flagged"]:
            out("  - %s" % k)
    if weak:
        out("")
        out("Known weak (allowlisted, still need a positive control):")
        for k in weak:
            out("  - %s: %s" % (k, allow[k][1]))
    if r["stale"]:
        out("")
        out("STALE allowlist entries (not a failure -- remove them):")
        for k, why in r["stale"]:
            out("  - %s: %s" % (k, why))


def overlay_working_tree(dst):
    """Copy the working tree's modified and untracked files over a worktree of HEAD."""
    ls = subprocess.run(["git", "ls-files", "-z", "-m", "-o", "--exclude-standard"], cwd=REPO,
                        stdout=subprocess.PIPE, check=True).stdout.split(b"\0")
    for rel in filter(None, (p.decode("utf-8", "surrogateescape") for p in ls)):
        src = os.path.join(REPO, rel)
        if os.path.isfile(src):
            os.makedirs(os.path.dirname(os.path.join(dst, rel)) or dst, exist_ok=True)
            shutil.copy2(src, os.path.join(dst, rel))
    gone = subprocess.run(["git", "ls-files", "-z", "-d"], cwd=REPO,
                          stdout=subprocess.PIPE, check=True).stdout.split(b"\0")
    for rel in filter(None, (p.decode("utf-8", "surrogateescape") for p in gone)):
        try:
            os.remove(os.path.join(dst, rel))
        except OSError:
            pass


def cmd_run(args):
    if os.name == "nt":
        raise SystemExit("dead_gate.py is POSIX-only")
    allow = load_allowlist(args.allowlist)
    work = tempfile.mkdtemp(prefix="naab-dead-")
    copy = os.path.join(work, "repo")
    outdir = os.path.abspath(args.out or os.path.join(REPO, "test-parallel", "dead-gate"))
    try:
        subprocess.run(["git", "worktree", "add", "-q", "--detach", copy, "HEAD"], cwd=REPO, check=True)
        overlay_working_tree(copy)
        b = os.path.join(copy, "build")
        os.makedirs(b, exist_ok=True)
        for name in KEEP_BINARIES:
            src = os.path.join(REPO, "build", name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(b, name))
        # No CMakeCache.txt in the copy: a suite that would rebuild from it must
        # not reach back into the real build directory.
        dead = os.path.join(b, "naab-lang")
        with open(dead, "w") as f:
            f.write(DEAD)
        os.chmod(dead, 0o755)
        probe = subprocess.run([dead, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if probe.returncode != 0 or probe.stdout:
            raise SystemExit("the dead interpreter is not dead (rc=%d, output %r)" % (probe.returncode, probe.stdout))
        env = dict(os.environ)
        # A suite waiting for a server the dead interpreter never starts would
        # otherwise hold its slot for the full 600 s; a timeout still counts as
        # "can fail".
        env.setdefault("SHELL_TEST_TIMEOUT", "120s")
        # The runner keeps a unit's leftovers for inspection under TMPDIR; put
        # them inside this work dir so they go with it.
        env["TMPDIR"] = work
        cmd = [sys.executable, os.path.join(copy, "tools", "testrunner", "parallel.py"), "run",
               "--no-naab-phase", "--out", outdir]
        if args.jobs:
            cmd += ["--jobs", str(args.jobs)]
        subprocess.run(cmd, cwd=copy, env=env, stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL if not args.verbose else None)
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", copy], cwd=REPO,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(work, ignore_errors=True)
    with open(os.path.join(outdir, "results.json"), "rb") as f:
        results = json.loads(f.read().decode("utf-8"))
    return finish(results, allow)


def finish(results, allow):
    r = classify(results, allow)
    report(r, allow, results)
    complete = results.get("units_run") == results.get("units_listed")
    return 0 if (complete and not r["flagged"]) else 1


def cmd_classify(args):
    allow = load_allowlist(args.allowlist)
    with open(args.results, "rb") as f:
        results = json.loads(f.read().decode("utf-8"))
    return finish(results, allow)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--jobs", type=int)
    r.add_argument("--out")
    r.add_argument("--verbose", action="store_true")
    r.add_argument("--allowlist", default=DEFAULT_ALLOWLIST)
    c = sub.add_parser("classify")
    c.add_argument("results")
    c.add_argument("--allowlist", default=DEFAULT_ALLOWLIST)
    a = ap.parse_args(argv)
    return cmd_run(a) if a.cmd == "run" else cmd_classify(a)


if __name__ == "__main__":
    sys.exit(main())
