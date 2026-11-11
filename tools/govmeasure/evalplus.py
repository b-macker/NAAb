#!/usr/bin/env python3
"""Cases from EvalPlus: model-written solutions, labelled by running the
hidden tests (Bundle 13 PR 2).

    evalplus.py cases --tests HumanEvalPlus.jsonl.gz --samples ZIP... --out cases.jsonl
                      [--jobs N] [--labels labels.tsv]
    evalplus.py selftest

EvalPlus publishes each model's solutions to the 164 HumanEval tasks without
a verdict, so the label is made here, the way EvalPlus makes it: a solution
PASSES when, on every base and plus input of its task, it returns what the
task's reference solution returns (floats within the task's tolerance, 1e-6
when the task gives none). Anything else -- a different value, an exception,
no such function, running out of time -- FAILS. bad = fails.

THIS RUNS MODEL-WRITTEN CODE. Each solution runs in its own child process:
an isolated interpreter (-I), an empty scratch directory as cwd, a CPU-time
and address-space limit, no stdin, a wall-clock limit that kills its process
group. That bounds accidents, not intent: run it only on a throwaway machine
(the nightly job's runner), never beside anything you care about.

Controls, per task, before any solution is believed: the reference solution
judged against itself must PASS (else the task is left out: UNMEASURABLE,
counted), and a stub that returns None must FAIL (else the task's inputs do
not discriminate, and it is left out too). Task HumanEval/32 is left out by
name: any root of the polynomial is a right answer there, and EvalPlus judges
it with its own oracle, which this file does not copy.

The label says a solution is WRONG. That is wider than "gamed": see the note
on the category in check_map.json.
"""

import argparse
import concurrent.futures
import gzip
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import zipfile

SKIP_TASKS = {"HumanEval/32": "any root is correct; EvalPlus uses its own oracle"}
CPU_SECONDS = 20
WALL_SECONDS = 40
MEMORY_BYTES = 2 * 1024 * 1024 * 1024

# The child. argv[1] is a JSON file: reference source, candidate source,
# entry point, inputs, tolerance. Prints one line, "PASS" or "FAIL <why>".
WORKER = r'''
import copy, json, math, sys
job = json.load(open(sys.argv[1], encoding="utf-8"))
real_stdout = sys.stdout
sys.stdout = open("/dev/null", "w") if sys.platform != "win32" else open("nul", "w")
def verdict(text):
    real_stdout.write(text + "\n"); real_stdout.flush(); sys.exit(0)
def is_floats(x):
    if isinstance(x, float): return True
    if isinstance(x, (list, tuple)) and x: return all(isinstance(i, float) for i in x)
    return False
def same(exp, out, atol):
    if atol == 0 and is_floats(exp): atol = 1e-6
    if atol != 0:
        try:
            if isinstance(exp, (list, tuple)):
                return len(exp) == len(out) and all(math.isclose(a, b, rel_tol=1e-7, abs_tol=atol) for a, b in zip(exp, out))
            return math.isclose(exp, out, rel_tol=1e-7, abs_tol=atol)
        except Exception:
            return False
    return exp == out
ref_ns, cand_ns = {}, {}
try:
    exec(job["reference"], ref_ns)
    ref = ref_ns[job["entry_point"]]
except Exception as e:
    verdict("FAIL reference does not load: %s" % type(e).__name__)
try:
    exec(job["candidate"], cand_ns)
    cand = cand_ns[job["entry_point"]]
except BaseException as e:
    verdict("FAIL does not load: %s" % type(e).__name__)
for i, inp in enumerate(job["inputs"]):
    try:
        exp = ref(*copy.deepcopy(inp))
    except Exception as e:
        verdict("FAIL reference raised on input %d: %s" % (i, type(e).__name__))
    try:
        out = cand(*copy.deepcopy(inp))
    except BaseException as e:
        verdict("FAIL raised on input %d: %s" % (i, type(e).__name__))
    try:
        ok = same(exp, out, job["atol"])
    except Exception:
        ok = False
    if not ok:
        verdict("FAIL wrong answer on input %d" % i)
verdict("PASS")
'''


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def limits():
    import resource
    os.setsid()
    resource.setrlimit(resource.RLIMIT_CPU, (CPU_SECONDS, CPU_SECONDS))
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_BYTES, MEMORY_BYTES))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def judge(task, candidate, worker_path):
    """"PASS", "FAIL <why>", or "ERROR <why>" when the child gave no verdict."""
    with tempfile.TemporaryDirectory(prefix="govmeasure-ep-") as scratch:
        job = os.path.join(scratch, "job.json")
        with open(job, "w", encoding="utf-8") as f:
            json.dump({"reference": task["prompt"] + task["canonical_solution"],
                       "candidate": candidate, "entry_point": task["entry_point"],
                       "inputs": task["base_input"] + task["plus_input"],
                       "atol": task.get("atol", 0) or 0}, f)
        try:
            p = subprocess.Popen([sys.executable, "-I", worker_path, job], cwd=scratch,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, preexec_fn=limits)
        except OSError as e:
            return "ERROR cannot start: %s" % e
        try:
            stdout, _ = p.communicate(timeout=WALL_SECONDS)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            p.communicate()
            return "FAIL timed out"
        line = stdout.decode("utf-8", "replace").strip().split("\n")[-1] if stdout.strip() else ""
        if line == "PASS" or line.startswith("FAIL "):
            return line
        if p.returncode and p.returncode < 0:
            return "FAIL killed by signal %d (a resource limit)" % -p.returncode
        return "ERROR no verdict (exit %s)" % p.returncode


def load_tasks(path):
    tasks = {}
    with gzip.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            if line.strip():
                t = json.loads(line)
                tasks[t["task_id"]] = t
    return tasks


def samples(zip_path):
    """(task id, sample name, source) for every solution in one archive."""
    z = zipfile.ZipFile(zip_path)
    for name in sorted(z.namelist()):
        m = re.search(r"(HumanEval)_(\d+)/(\d+)\.py$", name)
        if m:
            yield "%s/%s" % (m.group(1), m.group(2)), m.group(3), z.read(name).decode("utf-8", "replace")


def cmd_cases(args):
    if sys.platform == "win32":
        out("UNMEASURABLE: the labelling child needs POSIX resource limits")
        return 2
    try:
        tasks = load_tasks(args.tests)
    except (OSError, ValueError) as e:
        out("UNMEASURABLE: cannot read the tests %s: %s" % (args.tests, e))
        return 2
    fd, worker_path = tempfile.mkstemp(prefix="govmeasure-worker-", suffix=".py")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(WORKER)
    try:
        usable, left_out = {}, {}

        def control(tid):
            t = tasks[tid]
            if tid in SKIP_TASKS:
                return tid, SKIP_TASKS[tid]
            ref = judge(t, t["prompt"] + t["canonical_solution"], worker_path)
            if ref != "PASS":
                return tid, "the reference does not pass its own inputs (%s)" % ref
            stub = judge(t, "def %s(*args, **kwargs):\n    return None\n" % t["entry_point"], worker_path)
            if not stub.startswith("FAIL"):
                return tid, "a stub returning None is not failed (%s)" % stub
            return tid, ""

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            for tid, why in ex.map(control, sorted(tasks)):
                if why:
                    left_out[tid] = why
                else:
                    usable[tid] = tasks[tid]
        out("tasks: %d usable, %d left out" % (len(usable), len(left_out)))
        for tid in sorted(left_out):
            out("  left out %s: %s" % (tid, left_out[tid]))
        if not usable:
            out("UNMEASURABLE: no task passed its controls")
            return 2

        work = []
        for zp in args.samples:
            model = re.sub(r"^evalplus-samples-|\.zip$", "", os.path.basename(zp))
            try:
                for tid, sid, src in samples(zp):
                    if tid in usable:
                        work.append((model, tid, sid, src))
            except (OSError, zipfile.BadZipFile) as e:
                out("UNMEASURABLE: cannot read %s: %s" % (zp, e))
                return 2

        def label(item):
            model, tid, sid, src = item
            return item, judge(usable[tid], src, worker_path)

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            results = list(ex.map(label, work))
    finally:
        os.remove(worker_path)

    errors = [(i, v) for i, v in results if v.startswith("ERROR")]
    per_model = {}
    written = 0
    labels = open(args.labels, "w", encoding="utf-8", newline="\n") if args.labels else None
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for (model, tid, sid, src), v in results:
            if v.startswith("ERROR"):
                continue
            passed = v == "PASS"
            s = per_model.setdefault(model, [0, 0])
            s[0 if passed else 1] += 1
            cid = "%s:%s:%s" % (model, tid, sid)
            f.write(json.dumps({"id": cid, "source": "evalplus", "language": "python",
                                "category": "fails-hidden-tests", "bad": not passed, "code": src}) + "\n")
            if labels:
                labels.write("%s\t%s\n" % (cid, v))
            written += 1
    if labels:
        labels.close()
    for model in sorted(per_model):
        p, fl = per_model[model]
        out("%s: %d pass, %d fail (%.1f%% pass)" % (model, p, fl, 100.0 * p / (p + fl)))
    out("wrote %d cases to %s; %d solutions gave no verdict and are left out" % (written, args.out, len(errors)))
    if results and len(errors) / len(results) > 0.01:
        out("UNMEASURABLE: more than 1% of solutions gave no verdict")
        return 2
    return 0 if written else 2


def selftest():
    if sys.platform == "win32":
        out("selftest: UNMEASURABLE on Windows (POSIX resource limits)")
        return 2
    fails = []

    def expect(name, cond, detail):
        out("  %s [%s] %s" % ("PASS" if cond else "FAIL", name, detail))
        if not cond:
            fails.append(name)

    task = {"prompt": "def half(x):\n    \"\"\"Half of x.\"\"\"\n", "canonical_solution": "    return x / 2\n",
            "entry_point": "half", "base_input": [[2], [3]], "plus_input": [[10.5], [0]], "atol": 0}
    fd, wp = tempfile.mkstemp(suffix=".py")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(WORKER)
    try:
        expect("ET-01", judge(task, "def half(x):\n    return x / 2\n", wp) == "PASS", "a right solution passes")
        expect("ET-02", judge(task, "def half(x):\n    return x * 0.5 + 1e-9\n", wp) == "PASS",
               "a float answer within tolerance passes")
        v = judge(task, "def half(x):\n    return 1.0 if x == 2 else 1.5\n", wp)
        expect("ET-03", v == "FAIL wrong answer on input 2",
               "an answer hard-coded for the base inputs fails on a plus input (%s)" % v)
        v = judge(task, "def half(x):\n    raise NotImplementedError\n", wp)
        expect("ET-04", v.startswith("FAIL raised on input 0"), "an exception fails (%s)" % v)
        v = judge(task, "def other(x):\n    return x / 2\n", wp)
        expect("ET-05", v.startswith("FAIL does not load"), "a missing entry point fails (%s)" % v)
        v = judge(task, "def half(x):\n    print('PASS')\n    return 0\n", wp)
        expect("ET-06", v.startswith("FAIL wrong answer"), "a solution that prints PASS is not believed (%s)" % v)
        v = judge(task, "import os\ndef half(x):\n    os._exit(0)\n", wp)
        expect("ET-07", v.startswith("ERROR"), "a child that exits without a verdict is an ERROR, not a pass (%s)" % v)
        global CPU_SECONDS, WALL_SECONDS
        old = (CPU_SECONDS, WALL_SECONDS)
        CPU_SECONDS, WALL_SECONDS = 1, 5
        v = judge(task, "def half(x):\n    while True: pass\n", wp)
        CPU_SECONDS, WALL_SECONDS = old
        expect("ET-08", v.startswith("FAIL"), "an endless loop fails on its CPU limit (%s)" % v)
        v = judge(task, "def half(x):\n    x = [0] * (1 << 62)\n", wp)
        expect("ET-09", v.startswith("FAIL"), "a huge allocation fails (%s)" % v)
    finally:
        os.remove(wp)
    out("")
    out("selftest: %d failed" % len(fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("cases")
    c.add_argument("--tests", required=True)
    c.add_argument("--samples", nargs="+", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--labels", default="")
    c.add_argument("--jobs", type=int, default=4)
    sub.add_parser("selftest")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 4 if e.code else 0
    if args.cmd == "cases":
        return cmd_cases(args)
    if args.cmd == "selftest":
        return selftest()
    ap.print_usage(sys.stderr)
    return 4


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
