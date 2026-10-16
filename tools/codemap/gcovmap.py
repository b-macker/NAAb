"""codemap coverage (gcovmap.py): which functions the test suite executes, mapped onto the index.

    cmake -S . -B build-cov -DNAAB_COVERAGE=ON && make -C build-cov naab-lang naab-gov -j4
    codemap coverage run --name suite            # every run-all-tests.sh unit, one gcov tree each
    codemap tested GovernanceEngine::checkPathAccessImpl   # which units executed it
    codemap untested --under src/runtime         # compiled in, executed by no unit
    codemap impact [REV]                         # changed functions -> callers -> the units to run

How it measures. The instrumented binaries write gcov counters (.gcda) when a
process exits; GCOV_PREFIX puts each test unit's counters in a tree of their own
(tools/testrunner/parallel.py --unit-env), so "which unit executed this" is a
fact per unit, not a guess. A NAAB_COVERAGE build wraps _exit so a HARD block's
_exit(3) still writes them (src/testing/coverage_exit.c). `gcov --json-format`
then gives each function's source file, start line and execution count, which is
matched to the index by FILE and LINE -- not by name, so overloads and lambdas
are told apart by where they are.

What it cannot see, and says: a process killed by a signal (a timeout's SIGKILL)
or replaced by exec writes nothing; a forked child's lines between fork and exec
are not recorded; a unit that ran naab-lang with GCOV_PREFIX scrubbed from its
environment writes into the build directory itself, collected as
"(unattributed)"; code in no instrumented binary (naab-lsp's own sources, a file
the build skips) reads "not in the build", never "untested", while code that IS
in naab-lang but that a test reaches only through an uninstrumented binary
(libnaab, naab-lsp, the GoogleTest unit tests) reads executed by no unit -- the
false-untested direction. Execution is not verification: a unit that executes a
function may assert nothing about it.
"""

import json
import os
import re
import shutil
import subprocess
from collections import defaultdict

CALLABLE_KINDS = ("constructor", "conversion_function", "cxx_method", "destructor", "function_decl",
                  "function_template", "lambda")
UNATTRIBUTED = "(unattributed)"


def install_hint(build_dir):
    return ("codemap coverage needs an instrumented build in %s:\n"
            "  cmake -S . -B %s -DNAAB_COVERAGE=ON && make -C %s naab-lang naab-gov -j4\n"
            "(GCC only; it is slower than the normal build and is never installed)"
            % (build_dir, build_dir, build_dir))


def find_files(root, suffix):
    out = []
    for d, _, files in os.walk(root):
        out.extend(os.path.join(d, f) for f in files if f.endswith(suffix))
    return sorted(out)


def unit_gcda(unit_dir, build_dir):
    """The .gcda files a unit wrote under its GCOV_PREFIX, each paired with the .gcno
    the build left at the original path. -> [(gcda, gcno)]"""
    build_dir = os.path.realpath(build_dir)
    pairs = []
    for gcda in find_files(unit_dir, ".gcda"):
        orig = gcda[len(unit_dir.rstrip(os.sep)):]          # GCOV_PREFIX + the compiled-in path
        if not orig.startswith(build_dir + os.sep) and gcda.startswith(build_dir + os.sep):
            orig = gcda                                       # unattributed: written in place
        gcno = orig[: -len(".gcda")] + ".gcno"
        if os.path.exists(gcno):
            pairs.append((gcda, gcno))
    return pairs


def run_gcov(pairs, gcov="gcov", batch=64):
    """gcov --json-format over the given (gcda, gcno) pairs. A .gcno is linked next to
    each .gcda that lacks one, which is where gcov looks. -> list of JSON documents
    (each names its data file, which notes_map() ties back to the build's .gcno)."""
    docs = []
    for gcda, gcno in pairs:
        beside = gcda[: -len(".gcda")] + ".gcno"
        if not os.path.exists(beside):
            try:
                os.symlink(gcno, beside)
            except OSError:
                shutil.copyfile(gcno, beside)
    for i in range(0, len(pairs), batch):
        chunk = [p[0] for p in pairs[i:i + batch]]
        r = subprocess.run([gcov, "--json-format", "--stdout"] + chunk, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, cwd=os.path.dirname(chunk[0]))
        if r.returncode != 0 and not r.stdout:
            raise RuntimeError("gcov failed: %s" % r.stderr.decode("utf-8", "replace")[:300])
        for ln in r.stdout.decode("utf-8", "replace").splitlines():
            ln = ln.strip()
            if ln.startswith("{"):
                docs.append(json.loads(ln))
    return docs


def notes_map(pairs):
    """{normalised .gcda path: the build's .gcno} for functions_of's staleness test."""
    return {os.path.normpath(g): n for g, n in pairs}


def functions_of(docs, notes_of=None):
    """{(abs source file, start_line, is_lambda): {"count": n, "end": end_line, "names": set}}
    summed over every translation unit and template instantiation that reports it,
    and the set of source files changed since the notes file that describes them was
    written. gcov's lines are the lines the BUILD compiled; in a file edited since,
    they name whatever sits there now, so its functions are left out (and read
    UNMEASURED) rather than credited by a position that moved. notes_of maps a data
    file to its .gcno; without it nothing is judged stale."""
    out, stale = {}, set()
    mtime = {}

    def mt(path):
        if path not in mtime:
            try:
                mtime[path] = os.path.getmtime(path)
            except OSError:
                mtime[path] = None
        return mtime[path]

    for d in docs:
        cwd = d.get("current_working_directory", "")
        data = d.get("data_file", "")
        data = os.path.normpath(data if os.path.isabs(data) else os.path.join(cwd, data))
        notes = (notes_of or {}).get(data)
        notes_time = mt(notes) if notes else None
        for f in d.get("files", []):
            path = f["file"] if os.path.isabs(f["file"]) else os.path.join(cwd, f["file"])
            path = os.path.normpath(path)
            src_time = mt(path)
            if notes_time is not None and src_time is not None and src_time > notes_time:
                stale.add(path)
                continue
            for fn in f.get("functions", []):
                name = fn.get("demangled_name") or fn.get("name", "")
                key = (path, fn["start_line"], "{lambda(" in name)
                e = out.setdefault(key, {"count": 0, "end": fn.get("end_line", fn["start_line"]), "names": set()})
                e["count"] += fn.get("execution_count", 0)
                if len(e["names"]) < 3:
                    e["names"].add(name[:200])
    for key in [k for k in out if k[0] in stale]:     # stale in another TU's notes too
        del out[key]
    return out, stale


class LineMapper:
    """(file, start line, lambda?) -> indexed definition, by position, not name."""

    def __init__(self, db, repo):
        self.repo = os.path.realpath(repo)
        self.by_file = defaultdict(list)
        for usr, kind, file, line, end, is_def in db.execute(
                "SELECT usr, kind, file, line, end_line, is_def FROM symbols"):
            if kind in CALLABLE_KINDS and is_def and file and not file.startswith("/"):
                self.by_file[file].append((line, end or line, kind, usr))

    def rel(self, path):
        rp = os.path.realpath(path)
        if not rp.startswith(self.repo + os.sep):
            return None
        return os.path.relpath(rp, self.repo)

    def map(self, path, start, is_lambda):
        """-> (usr or None, how): "line" when a definition starts on that line,
        "inside" when only one encloses it (screened), None when nothing does."""
        rel = self.rel(path)
        if rel is None or rel not in self.by_file:
            return None, None
        syms = self.by_file[rel]
        want = [s for s in syms if (s[2] == "lambda") == is_lambda]
        exact = [s for s in want if s[0] == start]
        if len(exact) == 1:
            return exact[0][3], "line"
        if len(exact) > 1:
            return None, None                      # two definitions on one line: no guess
        inside = [s for s in want if s[0] <= start <= s[1]]
        if inside:
            inner = min(inside, key=lambda s: s[1] - s[0])
            return inner[3], "inside"
        return None, None


def ingest(units_dir, build_dir, db, repo, gcov="gcov", progress=None):
    """Every unit directory under units_dir (one GCOV_PREFIX tree each), plus counters
    written into the build directory itself (UNATTRIBUTED). -> result dict."""
    mapper = LineMapper(db, repo)
    units = sorted(d for d in os.listdir(units_dir) if os.path.isdir(os.path.join(units_dir, d))) \
        if os.path.isdir(units_dir) else []
    compiled, executed = {}, defaultdict(dict)
    unmatched = defaultdict(lambda: {"count": 0, "names": set()})
    inside, stale_files = {}, set()
    sources = [(u, os.path.join(units_dir, u)) for u in units]
    stray = [(g, g[: -len(".gcda")] + ".gcno") for g in find_files(build_dir, ".gcda")]
    for i, (unit, path) in enumerate(sources + ([(UNATTRIBUTED, None)] if stray else [])):
        pairs = stray if path is None else unit_gcda(path, build_dir)
        if not pairs:
            continue
        fns, stale = functions_of(run_gcov(pairs, gcov), notes_map(pairs))
        stale_files |= {mapper.rel(p) for p in stale if mapper.rel(p) in mapper.by_file}
        for (src, start, lam), e in fns.items():
            usr, how = mapper.map(src, start, lam)
            if usr is None:
                # a miss only in a file the index parsed: third-party headers under
                # the repo (external/) are not indexed, so they are not misses. Kept
                # executed or not: a definition at this position is UNMEASURED,
                # neither "executed by no unit" nor "not in the build"
                if mapper.rel(src) in mapper.by_file:
                    u = unmatched["%s:%d" % (mapper.rel(src), start)]
                    u["count"] += e["count"]
                    u["names"] |= e["names"]
                continue
            if how == "inside":
                # screened: no definition starts on this line, so the count is credited
                # to the innermost one around it; kept so each can be checked
                inside["%s:%d" % (mapper.rel(src), start)] = usr
            compiled[usr] = True
            if e["count"]:
                executed[usr][unit] = executed[usr].get(unit, 0) + e["count"]
        if progress:
            progress(i + 1, len(sources) + (1 if stray else 0), unit)
    return {"units": units + ([UNATTRIBUTED] if stray else []),
            "compiled": sorted(compiled),
            "executed": {u: dict(v) for u, v in executed.items()},
            "unmatched": {k: {"count": v["count"], "names": sorted(v["names"])} for k, v in unmatched.items()},
            "matched_inside": inside,
            "stale_files": sorted(stale_files)}


def save(result, outdir, meta):
    os.makedirs(outdir, exist_ok=True)
    result = dict(result, meta=meta)
    with open(os.path.join(outdir, "coverage.json"), "w") as fh:
        json.dump(result, fh)
    return result


def load(cache, name):
    path = os.path.join(cache, "coverage", name, "coverage.json")
    try:
        with open(path) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        raise SystemExit("codemap: no coverage %r (run `codemap coverage run --name %s` or "
                         "`codemap coverage ingest`; `codemap coverage list` shows what exists)" % (name, name))


def status(cov, sym):
    """executed / unmeasured / not_executed / not_in_build for one indexed definition.
    UNMEASURED: gcov reported a function at a position inside this definition that
    could not be matched to ONE definition (two on a line), so either may be it; or
    the definition's file changed after the instrumented build compiled it."""
    if sym["file"] in cov.get("stale_files", ()):
        return "unmeasured"
    if sym["usr"] in cov["executed"]:
        return "executed"
    for key in cov.get("unmatched", {}):
        f, _, ln = key.rpartition(":")
        if f == sym["file"] and sym["line"] <= int(ln) <= (sym.get("end_line") or sym["line"]):
            return "unmeasured"
    return "not_executed" if sym["usr"] in set(cov["compiled"]) else "not_in_build"


def changed_ranges(repo, rev):
    """{file: [(start, end)]} of working-tree lines changed since `rev` (new side of
    `git diff -U0 rev`); a pure deletion is recorded as the line it sits after."""
    out = subprocess.run(["git", "-C", repo, "diff", "-U0", "--no-color", "--no-ext-diff", rev],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        raise SystemExit("codemap impact: git diff %s failed: %s" % (rev, out.stderr.decode("utf-8", "replace")[:200]))
    ranges, cur = defaultdict(list), None
    for ln in out.stdout.decode("utf-8", "replace").splitlines():
        if ln.startswith("+++ "):
            cur = ln[6:] if ln.startswith("+++ b/") else None
        elif ln.startswith("@@") and cur:
            m = re.match(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", ln)
            if m:
                c, d = int(m.group(1)), int(m.group(2) or 1)
                ranges[cur].append((c, c + d - 1) if d else (max(c, 1), max(c, 1)))
    return dict(ranges)
