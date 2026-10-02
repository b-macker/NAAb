#!/usr/bin/env python3
"""parallel.py -- run run-all-tests.sh's shell suites concurrently, report-only.

    python3 tools/testrunner/parallel.py run [--jobs N] [--order plan|reverse|shuffle]
                                             [--seed S] [--shared] [--durations FILE]
                                             [--out DIR] [--no-naab-phase]
    python3 tools/testrunner/parallel.py compare REFERENCE.json CANDIDATE.json

REPORT-ONLY. run-all-tests.sh remains the verdict. This tool earns trust only
by agreeing with it, which `compare` checks unit by unit.

What it runs. It does not keep a list of suites. It asks run-all-tests.sh
(NAAB_TEST_LIST list mode), so registration, skip lists and per-suite
conditions stay in that one file, and a suite added there is picked up here
with no edit. Each unit runs under the same policy run-all-tests.sh applies:
the same timeout, and the same exit codes counted as failure or skip. The
naab phase (single .naab files, about 45 s) runs as ONE unit,
`NAAB_TEST_PHASE=naab bash run-all-tests.sh`. Parallelising it would save
little, and several of its tests share fixed files in HOME.

Isolation (the default). Each suite gets its own HOME and TMPDIR under --out,
so nothing one suite leaves behind can reach another. That covers:
  - the real trust store: run-all-tests.sh clears it once per run, but a key
    installed mid-run would reach every later suite;
  - the shared C++ compile cache under ~/.naab/cache, written by copy-over
    with only an in-process lock;
  - leftovers written directly into HOME.
Toolchains that live under HOME (cargo, rustup, Python user site, Go,
npm) are pointed back at the real directories, and only when those exist.
--shared turns isolation off and moves the real trust store aside instead,
as run-all-tests.sh does.

Exclusive units run alone, after the pool drains (see EXCLUSIVE for each
reason). Each one was traced, not guessed.

What is reported per unit:
  - verdict;
  - duration;
  - SKIP / UNMEASURABLE / XFAIL markers in the output. A suite starved of CPU
    often reports "could not measure" and still exits 0, so comparing exit
    codes alone would miss exactly the effect a parallel run can cause;
  - files the suite left in its HOME and TMPDIR.
For the run as a whole it also reports changes to the git worktree, to the
real HOME's NAAb paths, and to the top level of /tmp.

Standard library only; Python 3.8+. POSIX only for now: on Windows the
native Python and MSYS bash disagree on paths, and the suites run serially
there anyway.
"""
import argparse
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Units that must not overlap anything else. Keyed by the suite path as
# run-all-tests.sh lists it. A key that no longer matches a listed unit is
# reported as stale, so this list cannot silently rot.
EXCLUSIVE = {
    "tests/self-audit/test_prescan_canaries.sh":
        "writes canary code into src/interpreter/interpreter.cpp for its whole run; "
        "suites that read src/ or build from it would see the injection",
    "tests/embedding/test_libnaab_build.sh":
        "runs cmake --build in the shared build/ dir (target libnaab links the same "
        "static libs as naab-lang); a concurrent canary injection would be compiled in",
    # Wall-clock upper bounds on timeout delivery ("fired within N s"). They exist
    # to catch timeout regressions, so the bounds must not be loosened; run them
    # where a CPU-heavy neighbour cannot stretch them.
    "tests/cli/test_timeout_ab.sh": "wall-clock bound on timeout delivery",
    "tests/cli/test_js_timeout_rt002.sh": "wall-clock bound on timeout delivery",
    "tests/cli/test_async_timeout_async001.sh": "wall-clock bounds (upper and lower)",
    "tests/security/test_alarm_delivery_rt007.sh": "wall-clock bound on alarm delivery",
    "tests/security/test_regex_timeout_bound.sh": "wall-clock bound on regex timeout",
    "tests/security/test_timeout_reach.sh": "wall-clock bound on timeout reach",
    "tests/security/test_r24_fixes.sh": "wall-clock bound on an HTTP round trip",
    "tests/security/test_orphan_kill_rt003.sh":
        "its control degrades to SKIP when the child is slow to start under load",
}

# State the ENGINE or a TOOLCHAIN writes under HOME, not something a suite
# left behind: NAAb's caches, its append-only security log and REPL history,
# and Go's telemetry counters. Reported apart so they cannot bury a real
# leftover (a first isolated run listed security.log for 27 suites).
HOME_STATE_PREFIXES = (".naab_cpp_cache", ".naab/cache", ".naab/logs", ".naab_history",
                       ".config/go")

SKIP_RE = re.compile(rb"\b(SKIP|SKIPPED|UNMEASURABLE|XFAIL)\b")
TOOLCHAIN_DIRS = {  # env var -> path under the real HOME
    "CARGO_HOME": ".cargo",
    "RUSTUP_HOME": ".rustup",
    "PYTHONUSERBASE": ".local",
    "GOPATH": "go",
    "npm_config_cache": ".npm",
}


def eprint(*a):
    print(*a, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- planning --

def list_units():
    """Ask run-all-tests.sh which shell-phase units exist."""
    fd, path = tempfile.mkstemp(prefix="naab-plan-", suffix=".tsv")
    os.close(fd)
    try:
        env = dict(os.environ, NAAB_TEST_PHASE="shell", NAAB_TEST_LIST=path)
        r = subprocess.run(["bash", "run-all-tests.sh"], cwd=REPO, env=env,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        if r.returncode != 0:
            raise SystemExit("listing failed (rc=%d):\n%s"
                             % (r.returncode, r.stdout.decode("utf-8", "replace")[-2000:]))
        with open(path, "rb") as f:
            data = f.read().decode("utf-8", "strict")
    finally:
        os.unlink(path)
    units = []
    for line in data.splitlines():
        kind, tmo, *argv = line.split("\t")
        if kind not in ("shell", "shell-skip124", "python") or not argv:
            raise SystemExit("unrecognised list line: %r" % line)
        if kind == "python":
            tmo = ""          # listed as "-": run-all-tests.sh gives it no timeout
        units.append({"kind": kind, "timeout": tmo, "argv": argv, "key": " ".join(argv)})
    keys = [u["key"] for u in units]
    dup = sorted({k for k in keys if keys.count(k) > 1})
    if dup:
        raise SystemExit("unit listed twice: %s" % ", ".join(dup))
    return units


def command_for(u):
    if u["kind"] == "python":
        return ["python3"] + u["argv"]
    if u["kind"] == "shell":
        kill_after = os.environ.get("SHELL_TEST_KILL_AFTER", "30s")
        return ["timeout", "-k", kill_after, u["timeout"], "bash"] + u["argv"]
    return ["timeout", u["timeout"], "bash"] + u["argv"]          # shell-skip124


def verdict_for(u, rc):
    if rc == 0:
        return "PASS"
    if u["kind"] == "shell-skip124" and rc == 124:
        return "SKIP-TIMEOUT"   # that suite's documented policy; reported, never hidden
    if u["kind"] != "python" and rc in (124, 137):
        return "TIMEOUT"
    return "FAIL"


# --------------------------------------------------------------- isolation --

def unit_env(base_home, home, tmp):
    env = dict(os.environ)
    env.pop("NAAB_TEST_LIST", None)
    env.pop("NAAB_TEST_PHASE", None)
    if home is None:
        return env
    for var, rel in TOOLCHAIN_DIRS.items():
        real = os.path.join(base_home, rel)
        if var not in env and os.path.isdir(real):
            env[var] = real
    if "GOCACHE" not in env and os.path.isdir(os.path.join(base_home, ".cache", "go-build")):
        env["GOCACHE"] = os.path.join(base_home, ".cache", "go-build")
    env["HOME"] = home
    env["TMPDIR"] = tmp
    return env


def tree(root):
    out = []
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files) + [x + "/" for x in dirs if not os.listdir(os.path.join(d, x))]:
            out.append(os.path.relpath(os.path.join(d, name.rstrip("/")), root) + ("/" if name.endswith("/") else ""))
    return out


def git_status(exclude):
    r = subprocess.run(["git", "status", "--porcelain", "--untracked-files=all"], cwd=REPO,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        return None
    rel = os.path.relpath(exclude, REPO).replace(os.sep, "/") + "/"
    return sorted(l for l in r.stdout.decode("utf-8", "replace").splitlines() if rel not in l)


def home_naab_snapshot(home):
    snap = {}
    for name in sorted(os.listdir(home)) if os.path.isdir(home) else []:
        if not name.startswith(".naab"):
            continue
        p = os.path.join(home, name)
        if os.path.isdir(p):
            for rel in tree(p):
                full = os.path.join(p, rel)
                snap[name + "/" + rel] = os.path.getmtime(full) if os.path.exists(full) else 0
        else:
            snap[name] = os.path.getmtime(p)
    return snap


def tmp_snapshot():
    root = "/tmp"
    try:
        return sorted(os.listdir(root))
    except OSError:
        return []


# --------------------------------------------------------------- execution --

class Runner:
    def __init__(self, args, units, out):
        self.args, self.units, self.out = args, units, out
        self.lock = threading.Lock()
        self.done = 0
        self.total = len(units)
        self.procs = set()
        self.stop = False

    def run_unit(self, u):
        idx = u["index"]
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", u["key"])[:120]
        log = os.path.join(self.out, "logs", "%03d_%s.log" % (idx, safe))
        home = tmp = None
        if not self.args.shared:
            # Outside the repo, deliberately: NAAb discovers govern.json by walking
            # up from a script's directory, so a suite writing scripts under an
            # in-repo HOME/TMPDIR could find a config it never sees under /tmp.
            home = os.path.join(self.iso_root, "%03d" % idx, "home")
            tmp = os.path.join(self.iso_root, "%03d" % idx, "tmp")
            os.makedirs(home)
            os.makedirs(tmp)
        env = unit_env(self.base_home, home, tmp)
        if u["kind"] == "naab-phase":
            env["NAAB_TEST_PHASE"] = "naab"
            cmd = ["bash", "run-all-tests.sh"]
        else:
            cmd = command_for(u)
        t0 = time.time()
        with open(log, "wb") as lf:
            p = subprocess.Popen(cmd, cwd=REPO, env=env, stdin=subprocess.DEVNULL,
                                 stdout=lf, stderr=subprocess.STDOUT, start_new_session=True)
            with self.lock:
                self.procs.add(p)
            rc = p.wait()
            with self.lock:
                self.procs.discard(p)
        t1 = time.time()
        with open(log, "rb") as lf:
            body = lf.read()
        res = {
            "key": u["key"], "kind": u["kind"], "index": idx, "exclusive": u["exclusive"],
            "rc": rc, "verdict": "PASS" if (u["kind"] == "naab-phase" and rc == 0)
            else ("FAIL" if u["kind"] == "naab-phase" else verdict_for(u, rc)),
            "seconds": round(t1 - t0, 3), "start": round(t0 - self.t_start, 3),
            "skip_markers": len(SKIP_RE.findall(body)),
            "log": os.path.relpath(log, self.out),
        }
        if home is not None:
            left = tree(home)
            res["home_state"] = [x for x in left if x.startswith(HOME_STATE_PREFIXES)]
            res["left_in_home"] = [x for x in left if not x.startswith(HOME_STATE_PREFIXES)]
            res["left_in_tmpdir"] = tree(tmp)
            if not res["left_in_home"] and not res["left_in_tmpdir"]:
                shutil.rmtree(os.path.dirname(home), ignore_errors=True)
        with self.lock:
            self.done += 1
            print("[%3d/%d] %-12s %7.1fs  %s%s" % (
                self.done, self.total, res["verdict"], res["seconds"], u["key"],
                "  (%d skip markers)" % res["skip_markers"] if res["skip_markers"] else ""),
                flush=True)
        return res

    def kill_all(self):
        with self.lock:
            self.stop = True
            for p in list(self.procs):
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except OSError:
                    pass

    def execute(self):
        self.base_home = os.path.expanduser("~")
        self.iso_root = tempfile.mkdtemp(prefix="naab-par-")
        self.t_start = time.time()
        pool_units = [u for u in self.units if not u["exclusive"]]
        excl_units = [u for u in self.units if u["exclusive"]]
        results = []
        jobs = max(1, self.args.jobs)
        if jobs == 1:
            # Serial: honour the requested order exactly, exclusives in place.
            for u in self.units:
                if self.stop:
                    break
                results.append(self.run_unit(u))
            return results
        queue = list(pool_units)
        qlock = threading.Lock()

        def worker():
            while True:
                with qlock:
                    if not queue or self.stop:
                        return
                    u = queue.pop(0)
                r = self.run_unit(u)
                with qlock:
                    results.append(r)

        threads = [threading.Thread(target=worker, daemon=True) for _ in range(jobs)]
        for t in threads:
            t.start()
        for t in threads:
            while t.is_alive():
                t.join(0.5)
        for u in excl_units:
            if self.stop:
                break
            results.append(self.run_unit(u))
        return results

    def cleanup(self):
        for d in sorted(os.listdir(self.iso_root)):
            try:
                os.rmdir(os.path.join(self.iso_root, d))
            except OSError:
                pass
        try:
            os.rmdir(self.iso_root)
        except OSError:
            eprint("kept units' leftovers for inspection: %s" % self.iso_root)


def order_units(units, args):
    if args.order == "reverse":
        units = list(reversed(units))
    elif args.order == "shuffle":
        units = list(units)
        random.Random(args.seed).shuffle(units)
    if args.jobs > 1 and args.durations:
        # Longest first: the pool's finish time is bounded below by its longest
        # unit, so starting long units early is what keeps the tail short.
        with open(args.durations, "rb") as f:
            d = json.loads(f.read().decode("utf-8"))
        known = {}
        for s in d.get("suites", []):
            known[(s["name"] + " " + s.get("args", "")).strip()] = s["seconds"]
        for r in d.get("results", []):
            known[r["key"]] = r["seconds"]
        units = sorted(units, key=lambda u: -known.get(u["key"], 1e9))  # unknown = assume slow
    return units


def cmd_run(args):
    if os.name == "nt":
        raise SystemExit("parallel.py is POSIX-only for now (see the module docstring)")
    out = os.path.abspath(args.out or os.path.join(REPO, "test-parallel"))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "logs"))

    units = list_units()
    listed = {u["argv"][0] for u in units}
    stale = sorted(k for k in EXCLUSIVE if k not in listed)
    if args.only:
        units = [u for u in units if re.search(args.only, u["key"])]
    for u in units:
        u["exclusive"] = (u["argv"][0] in EXCLUSIVE) if args.jobs > 1 else False
    if not args.no_naab_phase:
        units.append({"kind": "naab-phase", "timeout": "", "argv": ["run-all-tests.sh"],
                      "key": "NAAB_TEST_PHASE=naab run-all-tests.sh", "exclusive": False})
    units = order_units(units, args)
    for i, u in enumerate(units):
        u["index"] = i

    trust_bak = None
    real_trust = os.path.join(os.path.expanduser("~"), ".naab", "trusted-keys")
    if args.shared and os.path.isdir(real_trust):
        trust_bak = tempfile.mkdtemp(prefix="trust_bak_par_")
        shutil.move(real_trust, os.path.join(trust_bak, "trusted-keys"))

    before = {"git": git_status(out), "home": home_naab_snapshot(os.path.expanduser("~")),
              "tmp": tmp_snapshot()}
    runner = Runner(args, units, out)
    prev = {s: signal.signal(s, lambda *_: runner.kill_all()) for s in (signal.SIGINT, signal.SIGTERM)}
    t0 = time.time()
    try:
        results = runner.execute()
        runner.cleanup()
    finally:
        for s, h in prev.items():
            signal.signal(s, h)
        if args.shared:
            shutil.rmtree(real_trust, ignore_errors=True)
            if trust_bak:
                shutil.move(os.path.join(trust_bak, "trusted-keys"), real_trust)
                shutil.rmtree(trust_bak, ignore_errors=True)
    wall = time.time() - t0
    after = {"git": git_status(out), "home": home_naab_snapshot(os.path.expanduser("~")),
             "tmp": tmp_snapshot()}

    hb, ha = before["home"], after["home"]
    report = {
        "schema": 1,
        "mode": {"jobs": args.jobs, "order": args.order, "seed": args.seed,
                 "isolated": not args.shared},
        "wall_seconds": round(wall, 3),
        "units_listed": len(units),
        "units_run": len(results),
        "interrupted": runner.stop,
        "stale_exclusive_keys": stale,
        "hygiene": {
            "git_changed": sorted(set(after["git"] or []) ^ set(before["git"] or []))
            if before["git"] is not None else "UNMEASURABLE (git status failed)",
            "real_home_naab_added_or_changed": sorted(k for k in ha if hb.get(k) != ha[k]),
            "real_home_naab_removed": sorted(k for k in hb if k not in ha),
            "tmp_added": sorted(set(after["tmp"]) - set(before["tmp"])),
        },
        "results": sorted(results, key=lambda r: r["index"]),
    }
    with open(os.path.join(out, "results.json"), "wb") as f:
        f.write((json.dumps(report, indent=1, ensure_ascii=True) + "\n").encode("ascii"))
    summary = summarise(report)
    with open(os.path.join(out, "summary.md"), "wb") as f:
        f.write(summary.encode("ascii", "replace"))
    sys.stdout.buffer.write(("\n" + summary).encode("ascii", "replace"))
    sys.stdout.flush()
    bad = [r for r in results if r["verdict"] in ("FAIL", "TIMEOUT")]
    complete = len(results) == len(units)
    return 0 if (complete and not bad) else 1


def summarise(rep):
    res = rep["results"]
    counts = {}
    for r in res:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    busy = sum(r["seconds"] for r in res)
    m = rep["mode"]
    out = ["## Parallel test run (report only)", "",
           "Mode: jobs=%d, order=%s%s, %s." % (
               m["jobs"], m["order"], " (seed %s)" % m["seed"] if m["order"] == "shuffle" else "",
               "isolated HOME/TMPDIR per unit" if m["isolated"] else "shared HOME"),
           "Units: %d listed, %d run%s." % (rep["units_listed"], rep["units_run"],
                                             " -- INTERRUPTED" if rep["interrupted"] else ""),
           "Verdicts: " + ", ".join("%s %d" % kv for kv in sorted(counts.items())) + ".",
           "Wall %.0f s for %.0f s of unit time (%.1fx)." % (
               rep["wall_seconds"], busy, busy / rep["wall_seconds"] if rep["wall_seconds"] else 0),
           ""]
    if rep["units_run"] != rep["units_listed"]:
        out.append("**INCOMPLETE: %d unit(s) never ran.**" % (rep["units_listed"] - rep["units_run"]))
        out.append("")
    bad = [r for r in res if r["verdict"] != "PASS"]
    if bad:
        out += ["### Not passed", "", "| verdict | rc | seconds | unit | log |", "|---|---:|---:|---|---|"]
        out += ["| %s | %d | %.1f | `%s` | `%s` |" % (r["verdict"], r["rc"], r["seconds"], r["key"], r["log"])
                for r in bad]
        out.append("")
    left = [r for r in res if r.get("left_in_home") or r.get("left_in_tmpdir")]
    if left:
        out += ["### Units that left files behind (in their own HOME/TMPDIR)", ""]
        for r in left:
            items = ["HOME/" + x for x in r.get("left_in_home", [])] + \
                    ["TMPDIR/" + x for x in r.get("left_in_tmpdir", [])]
            out.append("- `%s`: %s%s" % (r["key"], ", ".join("`%s`" % x for x in items[:6]),
                                        " (+%d more)" % (len(items) - 6) if len(items) > 6 else ""))
        out.append("")
    h = rep["hygiene"]
    out += ["### Run-wide side effects", ""]
    for label, key in (("git worktree changed", "git_changed"),
                       ("real ~/.naab* added or changed", "real_home_naab_added_or_changed"),
                       ("real ~/.naab* removed", "real_home_naab_removed"),
                       ("new top-level /tmp entries", "tmp_added")):
        v = h[key]
        if isinstance(v, str):
            out.append("- %s: %s" % (label, v))
        else:
            out.append("- %s: %s" % (label, "none" if not v else "%d -- %s" % (
                len(v), ", ".join("`%s`" % x for x in v[:8]) + (" ..." if len(v) > 8 else ""))))
    if rep["stale_exclusive_keys"]:
        out += ["", "Stale EXCLUSIVE entries (no longer listed): " +
                ", ".join("`%s`" % k for k in rep["stale_exclusive_keys"])]
    slow = sorted(res, key=lambda r: -r["seconds"])[:10]
    out += ["", "### Slowest units", "", "| seconds | unit |", "|---:|---|"]
    out += ["| %.1f | `%s` |" % (r["seconds"], r["key"]) for r in slow]
    return "\n".join(out) + "\n"


# ----------------------------------------------------------------- compare --

def load_verdicts(path):
    """Accept a parallel.py results.json or a tools/testtiming report (timing.json)."""
    with open(path, "rb") as f:
        d = json.loads(f.read().decode("utf-8"))
    v = {}
    if "results" in d:
        for r in d["results"]:
            v[r["key"]] = {"rc": r["rc"], "skips": r.get("skip_markers")}
    elif "suites" in d:
        for s in d["suites"]:
            v[(s["name"] + " " + s.get("args", "")).strip()] = {"rc": s["exit"], "skips": None}
    else:
        raise SystemExit("%s: neither a results.json nor a timing.json" % path)
    return v


def cmd_compare(args):
    a, b = load_verdicts(args.reference), load_verdicts(args.candidate)
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    rc_diff = sorted(k for k in set(a) & set(b) if a[k]["rc"] != b[k]["rc"])
    skip_diff = sorted(k for k in set(a) & set(b)
                       if a[k]["skips"] is not None and b[k]["skips"] is not None
                       and a[k]["skips"] != b[k]["skips"])
    out = ["## Verdict equivalence", "",
           "reference: %d units, candidate: %d units, common: %d." % (len(a), len(b), len(set(a) & set(b))),
           ""]
    for label, keys, fmt in (
            ("Exit status differs", rc_diff, lambda k: "%d -> %d" % (a[k]["rc"], b[k]["rc"])),
            ("Skip-marker count differs", skip_diff, lambda k: "%d -> %d" % (a[k]["skips"], b[k]["skips"])),
            ("Only in reference", only_a, lambda k: ""),
            ("Only in candidate", only_b, lambda k: "")):
        out.append("### %s: %d" % (label, len(keys)))
        out += ["- `%s` %s" % (k, fmt(k)) for k in keys]
        out.append("")
    equivalent = not (rc_diff or skip_diff or only_a or only_b)
    out.append("EQUIVALENT" if equivalent else "NOT EQUIVALENT")
    sys.stdout.buffer.write(("\n".join(out) + "\n").encode("ascii", "replace"))
    return 0 if equivalent else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    r.add_argument("--order", choices=("plan", "reverse", "shuffle"), default="plan")
    r.add_argument("--seed", type=int, default=0)
    r.add_argument("--shared", action="store_true", help="no per-unit HOME/TMPDIR isolation")
    r.add_argument("--durations", help="timing.json or results.json, for longest-first scheduling")
    r.add_argument("--out", help="output dir (default: test-parallel/)")
    r.add_argument("--no-naab-phase", action="store_true")
    r.add_argument("--only", help="regex: run only matching units (debugging; never a verdict)")
    c = sub.add_parser("compare")
    c.add_argument("reference")
    c.add_argument("candidate")
    a = ap.parse_args(argv)
    return cmd_run(a) if a.cmd == "run" else cmd_compare(a)


if __name__ == "__main__":
    sys.exit(main())
