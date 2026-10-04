#!/usr/bin/env python3
"""setting_drop.py -- does any test notice when a govern.json setting stops working?

    python3 tools/testrunner/setting_drop.py map   --mut-binary PATH [--jobs N] [--out DIR]
    python3 tools/testrunner/setting_drop.py probe --mut-binary PATH [--jobs N] [--out DIR]
    python3 tools/testrunner/setting_drop.py report [--out DIR]

The dead-interpreter gate asks whether each TEST can fail. This asks the same of
each SETTING in govern-template.json: drop it, so the engine falls back to its
default, and see whether any test that uses it notices.

It needs a TEST build of naab-lang (cmake -DNAAB_CONFIG_MUTATION=ON; never a
build anyone runs for real). That build reads two variables in loadFromJson,
which every config load passes through -- file, extends chain, inline string,
mid-run reload:
  NAAB_SETTINGS_LOG  append every setting path the loaded config contains
  NAAB_DROP_SETTING  comma-separated setting paths to remove before parsing
                     ('*' matches any key at that level: agents.*.model)
  NAAB_DROP_LOG      append each drop actually performed

map    Run every shell suite once on the test build with nothing dropped,
       logging what each one loads. Output: a MEASURED map of suite -> settings
       loaded, and the baseline verdict (exit status + skip-marker count) of each.
probe  For each suite, drop groups of the settings it loads and bisect the groups
       whose drop changes the verdict, down to single settings.
report Summarise per setting:
         live          dropping it changed some test's verdict
         survives      tests load it, none noticed it dropped -- dead, set to its
                       default, or no test checks its effect
         untested      no test loads it at all
         unmeasurable  a drop that was asked for was never performed

Traps this is built around (docs/investigation-method.md):
  - A probe that did not run must not read as a finding: every drop is logged by
    the engine itself, and a run that logged none is UNMEASURABLE.
  - A sweep over explicit values cannot see a default: "survives" includes
    settings tests set to their default value. The report shows the value.
  - Flaky suites would read as "live": each suite's baseline is run twice, and a
    suite whose two baselines disagree is excluded and reported.
  - Group bisection can in principle miss a setting whose effect is masked by a
    sibling dropped in the same group. That would show as "survives", so the
    survivors are a list to triage, never a verdict that a setting is dead.

Standard library only; POSIX only.
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import parallel  # noqa: E402  (same directory: the unit list and verdict policy)

TEMPLATE = os.path.join(REPO, "govern-template.json")
USER_NAMED = ("agents", "functions", "environments")   # map keys a user names
ANSI_RE = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))
    sys.stdout.flush()


def norm(path):
    """agents.reviewer.model -> agents.*.model (and the other user-named maps)."""
    parts = path.split(".")
    for i in range(1, len(parts) - 1):
        if parts[i - 1] in USER_NAMED and not (i >= 2 and parts[i - 2] in USER_NAMED):
            parts[i] = "*"
    return ".".join(parts)


# Keys the loader never reads ("_comment...", "comment"): documentation in the
# template, not settings. "rationale" and "description" ARE read -- into reports
# and rule metadata -- so they stay, reported apart as documentation settings.
NOT_SETTINGS = re.compile(r"(^|\.)(_[^.]*|comment)(\.|$)")
DOC_LEAVES = ("rationale", "description")


def is_setting(path):
    return not NOT_SETTINGS.search(path)


def template_settings():
    def walk(o, p, acc):
        if isinstance(o, dict) and o:
            for k, v in o.items():
                if k.startswith("_") or k == "comment":
                    continue
                walk(v, p + [k], acc)
        else:
            acc.add(norm(".".join(p)))
    acc = set()
    with open(TEMPLATE, "rb") as f:
        walk(json.loads(f.read().decode("utf-8")), [], acc)
    return acc


# ------------------------------------------------------------------ copy --

class Copy:
    """A worktree of HEAD + the working tree's changes, with the test build as
    build/naab-lang. The real build/ is never touched."""

    def __init__(self, mut_binary):
        self.work = tempfile.mkdtemp(prefix="naab-drop-")
        self.root = os.path.join(self.work, "repo")
        subprocess.run(["git", "worktree", "add", "-q", "--detach", self.root, "HEAD"],
                       cwd=REPO, check=True)
        import dead_gate
        dead_gate.overlay_working_tree(self.root)
        b = os.path.join(self.root, "build")
        os.makedirs(b, exist_ok=True)
        for name in dead_gate.KEEP_BINARIES:
            src = os.path.join(REPO, "build", name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(b, name))
        shutil.copy2(mut_binary, os.path.join(b, "naab-lang"))
        # Prove it is a mutation build: it must log what it loads. In its OWN
        # temp dir, never self.work: NAAb finds govern.json by walking up from
        # the script, and self.work is an ancestor of every suite in the copy --
        # a probe config left there was loaded by 35 suites in the first run.
        pdir = tempfile.mkdtemp(prefix="naab-drop-probe-")
        try:
            probe_log = os.path.join(pdir, "probe.log")
            with open(os.path.join(pdir, "govern.json"), "w") as f:
                f.write('{"mode": "audit", "probe": {"setting": 1}}')
            prog = os.path.join(pdir, "probe.naab")
            with open(prog, "w") as f:
                f.write('main { print("ok") }\n')
            subprocess.run([os.path.join(b, "naab-lang"), prog], cwd=pdir,
                           env=dict(os.environ, NAAB_SETTINGS_LOG=probe_log),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
            ok = os.path.exists(probe_log) and "probe.setting" in open(probe_log).read()
        finally:
            shutil.rmtree(pdir, ignore_errors=True)
        if not ok:
            self.close()
            raise SystemExit("%s is not a NAAB_CONFIG_MUTATION build (it logged nothing)" % mut_binary)
        # Nothing may sit above the copy that a suite could discover.
        stray = [n for n in os.listdir(self.work) if n != "repo"]
        if stray:
            self.close()
            raise SystemExit("unexpected files above the copy: %s" % stray)

    def close(self):
        subprocess.run(["git", "worktree", "remove", "--force", self.root], cwd=REPO,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(self.work, ignore_errors=True)


def run_unit(copy, unit, drop=None, slot="x"):
    """Run one unit isolated; -> (verdict, rc, skips, loaded_settings, drops_logged)."""
    d = tempfile.mkdtemp(prefix="u-", dir=copy.work)
    home, tmp = os.path.join(d, "home"), os.path.join(d, "tmp")
    os.makedirs(home)
    os.makedirs(tmp)
    env = parallel.unit_env(os.path.expanduser("~"), home, tmp)
    env["NAAB_SETTINGS_LOG"] = os.path.join(d, "loaded.log")
    env["NAAB_DROP_LOG"] = os.path.join(d, "drops.log")
    if drop:
        env["NAAB_DROP_SETTING"] = ",".join(sorted(drop))
    p = subprocess.run(parallel.command_for(unit), cwd=copy.root, env=env, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    body = ANSI_RE.sub(b"", p.stdout)
    skips = len(parallel.SKIP_RE.findall(body))
    loaded = set()
    if os.path.exists(env["NAAB_SETTINGS_LOG"]):
        with open(env["NAAB_SETTINGS_LOG"], "rb") as f:
            loaded = {norm(l) for l in f.read().decode("utf-8", "replace").splitlines() if l}
    drops = []
    if os.path.exists(env["NAAB_DROP_LOG"]):
        with open(env["NAAB_DROP_LOG"], "rb") as f:
            drops = [l.split("\t", 1) for l in f.read().decode("utf-8", "replace").splitlines() if l]
    shutil.rmtree(d, ignore_errors=True)
    return parallel.verdict_for(unit, p.returncode), p.returncode, skips, loaded, drops


def outcome(r):
    return (r[1], r[2])   # exit status and skip-marker count


# ------------------------------------------------------------------ map ---

def cmd_map(args):
    units = parallel.list_units()
    units = [u for u in units if u["kind"] != "python"]
    copy = Copy(args.mut_binary)
    results = {}
    lock = threading.Lock()
    queue = list(units)
    excl = [u for u in queue if u["argv"][0] in parallel.EXCLUSIVE]
    queue = [u for u in queue if u["argv"][0] not in parallel.EXCLUSIVE]
    t0 = time.time()

    def do(u):
        a = run_unit(copy, u)
        b = run_unit(copy, u)        # second baseline: a flaky unit would read as "live"
        with lock:
            results[u["key"]] = {"unit": u, "verdict": a[0], "outcome": outcome(a),
                                 "stable": outcome(a) == outcome(b),
                                 "loaded": sorted(a[3] | b[3])}
            out("[%3d/%d] %-12s %s  (%d settings)" % (len(results), len(units), a[0],
                                                     u["key"], len(a[3] | b[3])))

    def worker():
        while True:
            with lock:
                if not queue:
                    return
                u = queue.pop(0)
            do(u)

    try:
        ts = [threading.Thread(target=worker, daemon=True) for _ in range(args.jobs)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        for u in excl:
            do(u)
    finally:
        copy.close()
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "map.json"), "w") as f:
        json.dump({"seconds": round(time.time() - t0), "units": {k: {kk: vv for kk, vv in v.items() if kk != "unit"}
                                                                for k, v in results.items()}},
                  f, indent=1)
    summarise_map(results)
    return 0


def summarise_map(results):
    tmpl = template_settings()
    loaded_by = collections.defaultdict(set)
    for k, r in results.items():
        for s in r["loaded"]:
            loaded_by[s].add(k)
    in_tmpl = {s for s in loaded_by if s in tmpl}
    outside = {s for s in loaded_by if s not in tmpl}
    unstable = [k for k, r in results.items() if not r["stable"]]
    runs = sum(len([s for s in r["loaded"] if s in tmpl]) for r in results.values() if r["stable"])
    out("")
    out("## Setting coverage (measured: what each suite's configs really contain)")
    out("")
    out("template settings                       %d" % len(tmpl))
    out("  loaded by at least one suite          %d" % len(in_tmpl))
    out("  loaded by NO suite (untested)         %d" % (len(tmpl) - len(in_tmpl)))
    out("settings suites load that the template does not have  %d" % len(outside))
    out("suites whose two baselines disagree (excluded)        %d %s" % (len(unstable), unstable[:5]))
    out("(suite, setting) pairs a probe would test             %d" % runs)


# ------------------------------------------------------------------ probe -

def cmd_probe(args):
    with open(os.path.join(args.out, "map.json")) as f:
        m = json.load(f)
    tmpl = template_settings()
    units = {u["key"]: u for u in parallel.list_units()}
    copy = Copy(args.mut_binary)
    live = {}                     # setting -> first unit that noticed
    performed = collections.defaultdict(list)   # setting -> values actually dropped
    asked_not_done = set()
    lock = threading.Lock()
    todo = sorted(((k, r) for k, r in m["units"].items()
                   if r["stable"] and k in units and any(s in tmpl for s in r["loaded"])),
                  key=lambda kr: len(kr[1]["loaded"]))
    progress = {"done": 0, "runs": 0}

    def changed(u, base, group):
        res = run_unit(copy, u, drop=group)
        with lock:
            progress["runs"] += 1
            for path, val in res[4]:
                performed[norm(path)].append(val)
        if not res[4]:
            with lock:
                asked_not_done.update(group)
            return False
        return outcome(res) != base

    def bisect(u, base, group):
        if not changed(u, base, group):
            return
        if len(group) == 1:
            with lock:
                live.setdefault(group[0], u["key"])
            return
        half = len(group) // 2
        bisect(u, base, group[:half])
        bisect(u, base, group[half:])

    def do(key, r):
        u = units[key]
        base = tuple(r["outcome"])
        with lock:
            cand = [s for s in r["loaded"] if s in tmpl and s not in live]
        for i in range(0, len(cand), args.group):
            bisect(u, base, cand[i:i + args.group])
        with lock:
            progress["done"] += 1
            out("[%3d/%d] %s  (%d candidates, %d runs so far, %d live)"
                % (progress["done"], len(todo), key, len(cand), progress["runs"], len(live)))

    queue = [kr for kr in todo if units[kr[0]]["argv"][0] not in parallel.EXCLUSIVE]
    excl = [kr for kr in todo if units[kr[0]]["argv"][0] in parallel.EXCLUSIVE]
    qlock = threading.Lock()

    def worker():
        while True:
            with qlock:
                if not queue:
                    return
                kr = queue.pop(0)
            do(*kr)

    t0 = time.time()
    try:
        ts = [threading.Thread(target=worker, daemon=True) for _ in range(args.jobs)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        for kr in excl:
            do(*kr)
    finally:
        copy.close()
    with open(os.path.join(args.out, "probe.json"), "w") as f:
        json.dump({"seconds": round(time.time() - t0), "runs": progress["runs"], "live": live,
                   "dropped_values": {k: sorted(set(v))[:5] for k, v in performed.items()},
                   "asked_not_performed": sorted(asked_not_done)}, f, indent=1)
    return cmd_report(args)


def cmd_report(args):
    with open(os.path.join(args.out, "map.json")) as f:
        m = json.load(f)
    p = {}
    if os.path.exists(os.path.join(args.out, "probe.json")):
        with open(os.path.join(args.out, "probe.json")) as f:
            p = json.load(f)
    tmpl = template_settings()
    loaded_by = collections.defaultdict(set)
    for k, r in m["units"].items():
        if r["stable"]:
            for s in r["loaded"]:
                loaded_by[s].add(k)
    live = p.get("live", {})
    rows = []
    for s in sorted(tmpl):
        if s.split(".")[-1] in DOC_LEAVES:
            st = "documentation"
        elif s in live:
            st = "live"
        elif s not in loaded_by:
            st = "untested"
        elif s in p.get("asked_not_performed", []) and s not in p.get("dropped_values", {}):
            st = "unmeasurable"
        elif p:
            st = "survives"
        else:
            st = "not probed"
        rows.append({"setting": s, "status": st, "suites_loading": len(loaded_by.get(s, ())),
                     "noticed_by": live.get(s), "values_dropped": p.get("dropped_values", {}).get(s)})
    with open(os.path.join(args.out, "settings.json"), "w") as f:
        json.dump(rows, f, indent=1)
    c = collections.Counter(r["status"] for r in rows)
    out("## Setting liveness")
    out("")
    for st in ("live", "survives", "untested", "unmeasurable", "not probed", "documentation"):
        if c[st]:
            out("  %-13s %4d" % (st, c[st]))
    by = collections.defaultdict(collections.Counter)
    for r in rows:
        by[r["setting"].split(".")[0]][r["status"]] += 1
    out("")
    out("  %-24s %7s %8s %6s %8s %8s" % ("section", "settings", "untested", "live", "survives", "loaded"))
    for sec in sorted(by, key=lambda s: -sum(by[s].values())):
        b = by[sec]
        n = sum(b.values()) - b["documentation"]
        if not n:
            continue
        out("  %-24s %7d %8d %6d %8d %8s" % (sec, n, b["untested"], b["live"], b["survives"],
                                             "%d%%" % round(100.0 * (n - b["untested"]) / n)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("map", "probe", "report"):
        s = sub.add_parser(name)
        s.add_argument("--out", default=os.path.join(REPO, "test-parallel", "settings"))
        if name != "report":
            s.add_argument("--mut-binary", required=True)
            s.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
        if name == "probe":
            s.add_argument("--group", type=int, default=32)
    a = ap.parse_args(argv)
    a.out = os.path.abspath(a.out)
    return {"map": cmd_map, "probe": cmd_probe, "report": cmd_report}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
