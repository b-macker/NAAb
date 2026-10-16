"""codemap profile: what actually ran, from a callgrind run of any command.
(Named cgprofile, not profile: `profile` is a standard-library module.)

    codemap profile --name hello -- build/naab-lang examples/hello.naab

runs the command under valgrind's callgrind and writes, under
.cache/codemap/profiles/NAME/:

  folded.txt     one line per call stack, root first: `a;b;c COST`
  flame.svg      flame graph (root at the bottom), icicle.svg (root at the top)
  summary.txt    total cost, the hottest functions (self and inclusive), the
                 hottest stacks, and how much of the cost maps onto the index
  run.json       functions executed and caller -> callee edges with call
                 counts, read by `codemap ran` and `callers/callees --run`

Cost is callgrind's instruction count (Ir), not time: it is deterministic
from run to run, and it counts instructions, so I/O and waiting are invisible.
Stacks come from --separate-callers=N, so a stack deeper than N is cut at N
callers and drawn under a "[truncated]" root. Frames in system libraries are
never matched to an indexed symbol, whatever their name (glibc's own
`start_thread` is not the repo's).

A frame is matched to ONE indexed function by its name AND its parameter types
(overloads, a const and a non-const method, a user constructor and the implicit
copy constructor all share a name), or not at all: a frame whose source file the
index parsed but that matches no single definition is listed as UNMATCHED, and a
function it could be reads "unmeasured" in --run output, never "not run".
Functions the compiler writes for a declaration (`TLS wrapper function for x`,
thunks, guard variables) are never matched to the declaration.
"""

import json
import os
import re
import shutil
import sqlite3
import subprocess
import time
from collections import defaultdict

SYSTEM_OBJECT = re.compile(r"^(/usr/lib|/lib|/usr/libexec/valgrind|/usr/local/lib)/|/ld-linux|/vgpreload_")

# GCC tags names whose type changed ABI (every function returning std::string):
# `readString[abi:cxx11](bool)`. The tag is not part of the source name.
ABI_TAG = re.compile(r"\[abi:[^\]]*\]")

# Code the compiler writes FOR a declaration. `TLS wrapper function for x` is a
# function, x is a variable: matching the frame to x by its name would report a
# variable as a callee.
GENERATED = re.compile(r"^(?:TLS (?:wrapper|init) function for|guard variable for|vtable for|VTT for|"
                       r"construction vtable for|typeinfo(?: name)? for|(?:non-)?virtual thunk to|"
                       r"covariant return thunk to|transaction clone for) ")

CALLABLE_KINDS = ("constructor", "conversion_function", "cxx_method", "destructor", "function_decl",
                  "function_template")


def valgrind_path():
    return shutil.which(os.environ.get("CODEMAP_VALGRIND", "valgrind"))


INSTALL_HINT = ("codemap profile needs valgrind (callgrind).\n"
                "  Debian/Ubuntu: apt-get install valgrind\n"
                "  Fedora:        dnf install valgrind\n"
                "  macOS:         not supported by valgrind on Apple silicon; profile on Linux\n"
                "Set CODEMAP_VALGRIND to use a valgrind that is not on PATH.")


# --- demangled-name normalisation ------------------------------------------------

def _strip_balanced(s, open_c, close_c):
    out, depth = [], 0
    for ch in s:
        if ch == open_c:
            depth += 1
            continue
        if ch == close_c and depth:
            depth -= 1
            continue
        if depth == 0:
            out.append(ch)
    return "".join(out)


def _strip_trailing_args(s):
    """'a::b(int, char)' -> 'a::b'; 'X::operator()(int)' -> 'X::operator()'."""
    s = s.strip()
    while True:
        changed = False
        for suffix in (" const", " volatile", " &&", " &", " noexcept"):
            if s.endswith(suffix):
                s, changed = s[: -len(suffix)].rstrip(), True
        m = re.search(r"\s*\[clone [^\]]*\]$", s)
        if m:
            s, changed = s[: m.start()], True
        if not changed:
            break
    if not s.endswith(")"):
        return s
    depth = 0
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                head = s[:i]
                if head.endswith("operator") and s[i:i + 2] == "()":
                    # `operator()` itself; its argument list follows it
                    return _strip_trailing_args(head + "()" + s[i + 2:]) if s[i + 2:] else head + "()"
                return head
    return s


def frame_args(s):
    """A frame's OWN parameter list and constness: 'a::b(int, char) const' ->
    ('int, char', True). (None, False) when it shows none (a C symbol)."""
    s = ABI_TAG.sub("", s).strip()
    const = False
    while True:
        changed = False
        for suffix in (" const", " volatile", " &&", " &", " noexcept"):
            if s.endswith(suffix):
                const = const or suffix == " const"
                s, changed = s[: -len(suffix)].rstrip(), True
        m = re.search(r"\s*\[clone [^\]]*\]$", s)
        if m:
            s, changed = s[: m.start()], True
        if not changed:
            break
    if not s.endswith(")"):
        return None, const
    depth = 0
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                return s[i + 1:-1], const
    return None, const


def _drop_return_type(s):
    """'void std::__invoke_impl' -> 'std::__invoke_impl'; keeps 'X::operator new'."""
    depth, last_space = 0, -1
    for i, ch in enumerate(s):
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        elif ch == " " and depth == 0:
            last_space = i
    if last_space < 0:
        return s
    m = re.search(r"(?:^|::)operator\b", s)
    if m and m.start() < last_space:
        start = s.rfind(" ", 0, m.start() + 1)
        return s[start + 1:] if start >= 0 else s
    return s[last_space + 1:]


# A lambda's own frame ENDS in `{lambda(...)#N}::operator()`; a library frame that
# only mentions the closure in its template arguments (std::__invoke_impl<void,
# f()::{lambda()#1}>) is not the lambda and must not map to it.
LAMBDA = re.compile(r"^(?P<owner>.*)::\{lambda\((?P<sig>[^{}]*)\)#(?P<n>\d+)\}::operator\(\)(?:<.*>)?$")


def _split_params(sig):
    parts, depth, cur = [], 0, []
    for ch in sig:
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur))
    return parts


def param_key(sig):
    """A signature reduced to comparable base type names, from either side:
    demangled `std::__cxx11::basic_string<char, ...> const&, bool` or libclang's
    canonical `const std::basic_string<char> &, bool` -> ('basic_string', 'bool')."""
    parts = _split_params(sig)
    out = []
    for p in parts:
        p = _strip_balanced(ABI_TAG.sub("", p), "<", ">")
        # `char *const p`: a top-level const on a pointer is not part of the type
        # a demangler prints; `const char *` (const pointee) is.
        p = re.sub(r"([*&])\s*const\s*$", r"\1", p.strip())
        # GCC numbers `(std::string)` and `(std::string const&)` separately, so the
        # reference/pointer shape is part of the key. A top-level const on a
        # by-value parameter is not (mangling drops it).
        refs = "".join(ch for ch in p if ch in "&*")
        const = "const" if refs and re.search(r"\bconst\b", p) else ""
        base = re.sub(r"\b(const|volatile|struct|class|enum)\b|[&*]", " ", p).strip()
        # a generic lambda's parameter: GCC prints `auto:1`, libclang's canonical
        # spelling is `type-parameter-0-0`
        base = re.sub(r"\bauto:\d+|\btype-parameter-\d+-\d+", "auto", base.split("::")[-1].strip())
        if base and base != "void":
            out.append(" ".join(x for x in (const, base, refs) if x))
    return tuple(out)


def _first_template_args(t):
    """'std::map<K, V, less<K> > const&' -> 'map<K>' with qualifiers dropped: the
    FIRST template argument at each level, which both a demangler and libclang's
    canonical spelling print (they differ on defaulted ones: allocator, traits)."""
    i = t.find("<")
    if i < 0:
        return t
    depth = 0
    for j in range(i, len(t)):
        if t[j] == "<":
            depth += 1
        elif t[j] == ">":
            depth -= 1
            if depth == 0:
                break
    else:
        return t
    inner, parts, d, cur = t[i + 1:j], [], 0, []
    for ch in inner:
        if ch in "<(":
            d += 1
        elif ch in ">)":
            d -= 1
        if ch == "," and d == 0:
            break
        cur.append(ch)
    return t[:i] + "<" + _first_template_args("".join(cur)) + ">" + _first_template_args(t[j + 1:])


def fine_key(sig):
    """param_key plus each parameter's first template arguments: tells
    `Value(std::shared_ptr<BlockValue>)` from `Value(std::shared_ptr<StructValue>)`."""
    out = []
    for p in _split_params(sig):
        p = _first_template_args(ABI_TAG.sub("", p))
        p = re.sub(r"\b[A-Za-z_]\w*::", "", p)
        out.append(re.sub(r"\b(const|volatile|struct|class|enum)\b|[&*\s]", "", p))
    return tuple(x for x in out if x and x != "void")


def normalise(raw):
    """Demangled callgrind name -> (qualified name without args or template args, lambda) where
    lambda is (owner_name, ordinal, signature key, owner frame) for a
    `...::{lambda(...)#N}::operator()` frame, else None. The owner frame is itself a
    lambda frame when the lambda is defined inside another lambda."""
    s = raw.strip()
    if s.startswith("0x") or GENERATED.match(s):
        return s, None
    s = ABI_TAG.sub("", s).replace("(anonymous namespace)::", "")
    lam = None
    m = LAMBDA.match(_strip_trailing_args(s)) if "{lambda" in s else None
    if m:
        owner_frame = m.group("owner")
        if "{lambda" in owner_frame:
            owner = normalise(owner_frame)[0]
        else:
            owner = _drop_return_type(_strip_balanced(_strip_trailing_args(owner_frame), "<", ">"))
        lam = (owner, int(m.group("n")), param_key(m.group("sig")), owner_frame)
        return "%s::{lambda#%d}" % lam[:2], lam
    name = _strip_trailing_args(s)
    name = _strip_balanced(name, "<", ">")
    name = _drop_return_type(name).strip()
    return (name or s), lam          # "(below main)" is all parentheses


# --- callgrind output --------------------------------------------------------------

def parse_callgrind(path):
    """-> dict with events, total, self[ctx], calls[(ctx_a, ctx_b)] = [count, inclusive],
    obj[ctx] (the object file each context's function belongs to) and file[ctx] (its
    source file, from the debug information)."""
    names, objs, files = {}, {}, {}
    cur_ob = cur_cob = None
    cur_fl = cur_cfi = None
    fil = {}
    cur = None
    cfn = None
    pending_call = None
    selfc = defaultdict(int)
    calls = {}
    obj = {}
    events, total = [], 0

    def resolve(spec, table):
        m = re.match(r"\((\d+)\)(?:\s+(.*))?$", spec)
        if not m:
            return spec
        i, name = m.group(1), m.group(2)
        if name is not None:
            table[i] = name
        return table.get(i, "?" + i)

    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line:
                continue
            c0 = line[0]
            if c0.isdigit() or c0 in "+-*":
                parts = line.split()
                if len(parts) < 2:
                    continue
                try:
                    cost = int(parts[1])
                except ValueError:
                    continue
                if pending_call is not None:
                    key, n = pending_call
                    rec = calls.setdefault(key, [0, 0])
                    rec[0] += n
                    rec[1] += cost
                    pending_call = None
                elif cur is not None:
                    selfc[cur] += cost
                continue
            if line.startswith("fn="):
                cur = resolve(line[3:], names)
                obj[cur] = cur_ob          # a function's own block names its object
                fil[cur] = cur_fl          # ... and its source file
                cur_cob = cur_ob
                cur_cfi = cur_fl
                cfn = None
                continue
            if line.startswith("cfn="):
                cfn = resolve(line[4:], names)
                continue
            if line.startswith("ob="):
                cur_ob = resolve(line[3:], objs)
                cur_cob = cur_ob
                continue
            if line.startswith("cob="):
                cur_cob = resolve(line[4:], objs)
                continue
            if line.startswith("fl="):
                cur_fl = cur_cfi = resolve(line[3:], files)
                continue
            if line.startswith(("fi=", "fe=")):
                resolve(line[3:], files)   # inlined code: names a file, not the function's
                continue
            if line.startswith(("cfi=", "cfl=")):
                cur_cfi = resolve(line[4:], files)
                continue
            if line.startswith("calls="):
                try:
                    n = int(line[6:].split()[0])
                except (ValueError, IndexError):
                    n = 0
                if cur is not None and cfn is not None:
                    obj.setdefault(cfn, cur_cob)
                    fil.setdefault(cfn, cur_cfi)
                    pending_call = ((cur, cfn), n)
                cur_cob = cur_ob               # cob= applies to the next call only
                cur_cfi = cur_fl
                continue
            if line.startswith("events:"):
                events = line.split()[1:]
            elif line.startswith("summary:") or line.startswith("totals:"):
                try:
                    total = max(total, int(line.split()[1]))
                except (ValueError, IndexError):
                    pass
    return {"events": events, "total": total, "self": selfc, "calls": calls, "obj": obj, "file": fil}


# --- mapping frames onto the index ------------------------------------------------------

class Mapper:
    """Callgrind frame -> indexed symbol usr (or None). System-library frames never map,
    nor do frames the compiler generates for a declaration. A frame maps to ONE
    definition matching its name and parameter types; anything else is None, never a
    guess -- the edge then hangs off the nearest caller that does map."""

    def __init__(self, db, repo=None):
        self.by_name = defaultdict(list)
        self.lambdas = defaultdict(list)
        self.files = set()
        self.repo = os.path.realpath(repo) if repo else None
        self.cache = {}
        if db is None:
            return
        try:
            rows = db.execute("SELECT usr, name, file, line, is_def, kind, sig, psig FROM symbols").fetchall()
        except sqlite3.OperationalError:
            raise SystemExit("codemap: the index predates profile matching (no parameter signatures); "
                             "run `codemap index` again")
        for usr, name, file, line, is_def, kind, sig, psig in rows:
            if file and not file.startswith("/") and not file.startswith("<"):
                self.files.add(file)
            if kind == "lambda":
                # GCC numbers the lambdas of one function (or of one enclosing
                # lambda) #1, #2, ... per parameter signature, in order of appearance
                owner, _, where = usr.rpartition("@lambda:")
                try:
                    col = int(where.rsplit(":", 1)[1])
                except (IndexError, ValueError):
                    col = 0
                m = re.match(r"^lambda\((.*)\)$", sig or "")
                key = param_key(m.group(1)) if m else None
                self.lambdas[(owner, key)].append((line, col, usr))
                continue
            if kind not in CALLABLE_KINDS or not file or file.startswith("/") or not is_def:
                continue
            self.by_name[name].append((usr, kind, psig or "", file))
        for v in self.lambdas.values():
            v.sort()

    def rel(self, path):
        """A frame's source file as the index names it, or None (unknown, outside the repo)."""
        if not path or path == "???" or not self.repo:
            return None
        rp = os.path.realpath(path)
        if not rp.startswith(self.repo + os.sep):
            return None
        return os.path.relpath(rp, self.repo)

    def in_repo(self, path):
        """Is this source file one the index parsed?"""
        return self.rel(path) in self.files

    def resolve(self, name, frame, file=None):
        """The one definition named `name` whose parameter types match the frame's;
        among several, the one whose template arguments match, then the one in the
        frame's source file (static helpers of one name in many files, `main`)."""
        cands = self.by_name.get(name)
        if not cands:
            return None
        args, const = frame_args(frame)
        if args is None:                        # the frame shows no parameters
            hit = [(c[0], "", c[3]) for c in cands]
        else:
            want = param_key(args)
            hit = []
            for usr, kind, psig, cfile in cands:
                if not psig:
                    continue
                p_args, p_const = frame_args(psig)
                if p_args is None:
                    continue
                have = param_key(p_args)
                if kind == "function_template":
                    # parameter types are template parameters: compare the count
                    # only; a parameter pack matches any number, none included
                    pack = "..." in p_args
                    ok = len(want) >= len(have) - 1 if pack else len(have) == len(want)
                else:
                    ok = have == want and p_const == const
                if ok:
                    hit.append((usr, p_args, cfile))
            if len(hit) > 1:
                fine = fine_key(args)
                hit = [h for h in hit if fine_key(h[1]) == fine]
        if len(hit) > 1:
            rel = self.rel(file)
            if rel is not None:
                hit = [h for h in hit if h[2] == rel]
        return hit[0][0] if len(hit) == 1 else None

    def map(self, frame, obj, file=None):
        if frame.startswith("0x") or (obj and SYSTEM_OBJECT.search(obj)) or GENERATED.match(frame):
            return None
        key = (frame, file)
        if key in self.cache:
            return self.cache[key]
        name, lam = normalise(frame)
        usr = None
        if lam is not None:
            owner_frame = lam[3]
            owner = (self.map(owner_frame, obj, file) if "{lambda" in owner_frame
                     else self.resolve(lam[0], ABI_TAG.sub("", owner_frame), file))
            if owner is not None:
                ls = self.lambdas.get((owner, lam[2]), [])
                usr = ls[lam[1] - 1][2] if 0 < lam[1] <= len(ls) else None
        else:
            usr = self.resolve(name, frame, file)
        self.cache[key] = usr
        return usr


# --- the profile --------------------------------------------------------------------------

def analyse(cg, mapper, depth):
    """Folded stacks, functions executed, and observed caller -> callee edges.
    A function is its name AND its source file: two static helpers of one name in
    two files demangle to the same string, and only the file tells them apart."""
    folded = defaultdict(int)
    obj, fil = cg["obj"], cg.get("file", {})
    frame_obj, frame_files = {}, defaultdict(set)
    for ctx, o in obj.items():
        frame_obj[ctx.split("'")[0]] = o
    for ctx, f in fil.items():
        frame_files[ctx.split("'")[0]].add(f)

    def where(ctx):
        """(frame, object, source file) of a context; a context the run did not
        record falls back to what every context of that frame agrees on."""
        frame = ctx.split("'")[0]
        o = obj.get(ctx, frame_obj.get(frame))
        if ctx in fil:
            f = fil[ctx]
        else:
            fs = frame_files.get(frame, ())
            f = next(iter(fs)) if len(fs) == 1 else None
        return frame, o, f

    for ctx, cost in cg["self"].items():
        if cost <= 0:
            continue
        comps = ctx.split("'")
        stack = [normalise(c)[0] for c in reversed(comps)]
        if len(comps) - 1 >= depth:
            stack = ["[truncated]"] + stack
        folded[tuple(stack)] += cost

    funcs = {}

    def fn_entry(ctx):
        frame, o, f = where(ctx)
        key = (frame, f or "")
        e = funcs.get(key)
        if e is None:
            usr = mapper.map(frame, o, f)
            e = funcs[key] = {"name": normalise(frame)[0], "usr": usr, "calls": 0, "self": 0, "inclusive": 0,
                              "object": o or "", "file": f or ""}
        return e

    for ctx, cost in cg["self"].items():
        fn_entry(ctx)["self"] += cost
    edges = defaultdict(lambda: [0, False])
    for (a, b), (n, incl) in cg["calls"].items():
        ce = fn_entry(b)
        ce["calls"] += n
        ce["inclusive"] += incl
        chain = a.split("'")          # the caller, then ITS callers
        fn_entry(a)
        if ce["usr"] is None:
            continue
        # The nearest caller that maps onto the index: library frames between
        # (std::function, std::thread's trampoline) are walked through.
        src, via = None, False
        for i in range(len(chain)):
            fr, o, f = where("'".join(chain[i:]))
            u = mapper.map(fr, o, f)
            if u is not None:
                src, via = u, i > 0
                break
        if src is None:
            continue
        rec = edges[(src, ce["usr"])]
        rec[0] += n
        rec[1] = rec[1] or via
    return folded, funcs, edges


def files_known(funcs):
    """Did the profiled program carry debug information (source files per function)?"""
    return any(e.get("file") not in (None, "", "???") for e in funcs.values()
               if e.get("object") and e["object"] != "???" and not SYSTEM_OBJECT.search(e["object"]))


def unmatched_frames(funcs, mapper):
    """Repo frames that matched no single indexed definition.
    -> (unmatched, unindexed): unmatched are those whose NAME the index knows (an
    overload or lambda the matcher could not tell apart -- a function it could be
    reads 'unmeasured', never 'not run'); unindexed have no indexed name at all
    (implicit constructors and operators, code the index does not record).
    A repo frame is one from a source file the index parsed; without debug
    information there are no source files, and a frame of the program's own
    object counts only when its name is indexed -- so unindexed is unmeasurable."""
    with_files = files_known(funcs)
    unmatched, unindexed = {}, defaultdict(lambda: {"frames": 0, "calls": 0})
    for (frame, _), e in funcs.items():
        if e["usr"] or frame.startswith("0x") or GENERATED.match(frame):
            continue
        if with_files:
            if not mapper.in_repo(e.get("file")):
                continue
        elif not e.get("object") or e["object"] == "???" or SYSTEM_OBJECT.search(e["object"]):
            continue
        name, lam = normalise(frame)
        top = lam[0].split("::{lambda#")[0] if lam is not None else None
        if lam is not None and (with_files or top in mapper.by_name):
            rec = unmatched.setdefault(("lambda", top), {"name": top, "lambda": True, "arity": None,
                                                         "frames": 0, "calls": 0, "examples": []})
        elif lam is not None:
            continue
        elif name in mapper.by_name:
            args = frame_args(frame)[0]
            arity = None if args is None else len(param_key(args))
            rec = unmatched.setdefault((name, arity), {"name": name, "lambda": False, "arity": arity,
                                                       "frames": 0, "calls": 0, "examples": []})
        elif with_files:
            rec = unindexed[name]
        else:
            continue
        rec["frames"] += 1
        rec["calls"] += e["calls"]
        if "examples" in rec and len(rec["examples"]) < 3:
            rec["examples"].append(frame[:300])
    return list(unmatched.values()), dict(unindexed)


def write_outputs(outdir, meta, folded, funcs, edges, symbol_names, unmatched=None, unindexed=None):
    from flame import render  # noqa: E402  (same directory)
    total = sum(folded.values())
    with open(os.path.join(outdir, "folded.txt"), "w", encoding="ascii", errors="replace") as fh:
        for stack, cost in sorted(folded.items(), key=lambda kv: -kv[1]):
            fh.write("%s %d\n" % (";".join(stack), cost))
    mapped_frames = {f["name"] for f in funcs.values() if f["usr"]}
    for kind in ("flame", "icicle"):
        svg = render(folded, kind=kind, title="%s graph: %s" % (kind, meta["label"]),
                     subtitle="%s Ir (instructions, not time) | callgrind --separate-callers=%d | "
                              "stacks deeper than %d are cut and drawn under [truncated]"
                              % ("{:,}".format(total), meta["depth"], meta["depth"]),
                     highlight=mapped_frames)
        with open(os.path.join(outdir, kind + ".svg"), "w", encoding="utf-8") as fh:
            fh.write(svg)
    mapped_self = sum(f["self"] for f in funcs.values() if f["usr"])
    unmatched, unindexed = unmatched or [], unindexed or {}
    run = {"meta": meta, "total_ir": total, "mapped_self_ir": mapped_self,
           "functions": [{"frame": k[0], **v} for k, v in funcs.items()],
           "edges": [{"src": s, "dst": d, "calls": c, "via_library": via} for (s, d), (c, via) in edges.items()],
           "unmatched": unmatched,
           "files_known": files_known(funcs),
           "unindexed": {"frames": sum(v["frames"] for v in unindexed.values()),
                         "calls": sum(v["calls"] for v in unindexed.values()),
                         "names": sorted(unindexed)[:200]}}
    with open(os.path.join(outdir, "run.json"), "w") as fh:
        json.dump(run, fh)
    lines = summary_lines(meta, total, mapped_self, folded, funcs, symbol_names, run)
    with open(os.path.join(outdir, "summary.txt"), "w", encoding="ascii", errors="replace") as fh:
        fh.write("\n".join(lines) + "\n")
    return lines


def summary_lines(meta, total, mapped_self, folded, funcs, symbol_names, run=None, top=20):
    pct = lambda x: (100.0 * x / total) if total else 0.0  # noqa: E731
    out = ["profile %s: %s" % (meta["label"], " ".join(meta["cmd"])),
           "  exit status %s, %s Ir in %d stacks, %.1f s under callgrind"
           % (meta["exit_status"], "{:,}".format(total), len(folded), meta["seconds"]),
           "  cost is instructions executed (Ir), not time; I/O and waiting do not show",
           "  %.1f%% of the cost is in functions the index knows; the rest is libraries/system"
           % pct(mapped_self),
           "  executed: %d indexed functions (of %d frames seen)"
           % (len({f["usr"] for f in funcs.values() if f["usr"]}), len(funcs))]
    if run is not None:
        um = run.get("unmatched", [])
        ui = run.get("unindexed", {})
        if not run.get("files_known"):
            out.append("  NO DEBUG INFO in the program (no -g): frames are matched by name and parameter"
                       " types only, so same-named functions in different files (static helpers, main)"
                       " read 'unmeasured'; build with -g (cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo) to"
                       " tell them apart")
        out.append("  repo frames matched to no single indexed function: %d (%d calls) -- a function"
                   " they could be reads 'unmeasured', not 'not run'"
                   % (sum(u["frames"] for u in um), sum(u["calls"] for u in um)))
        for u in sorted(um, key=lambda u: -u["calls"])[:5]:
            out.append("      %s%s  %d frames, %d calls" % (u["name"][:70], " (lambda)" if u["lambda"] else
                                                          "" if u["arity"] is None else "/%d" % u["arity"],
                                                          u["frames"], u["calls"]))
        if run.get("files_known"):
            out.append("  repo frames the index has no name for (implicit constructors/operators): %d (%d calls)"
                       % (ui.get("frames", 0), ui.get("calls", 0)))
        else:
            out.append("  repo frames the index has no name for (implicit constructors/operators):"
                       " UNMEASURABLE without debug info")
    out += ["", "hottest by SELF cost (instructions spent in the function itself):"]
    rows = sorted(funcs.values(), key=lambda f: -f["self"])[:top]
    for f in rows:
        out.append("  %6.2f%%  %-70s %s" % (pct(f["self"]), f["name"][:70], symbol_names.get(f["usr"], "(library)")))
    out += ["", "hottest by INCLUSIVE cost (the function and everything it calls; recursion counts twice):"]
    for f in sorted(funcs.values(), key=lambda f: -f["inclusive"])[:top]:
        out.append("  %6.2f%%  %-70s calls %d" % (pct(f["inclusive"]), f["name"][:70], f["calls"]))
    out += ["", "hottest stacks (self cost at the leaf):"]
    for stack, cost in sorted(folded.items(), key=lambda kv: -kv[1])[:10]:
        shown = list(stack[-6:])
        out.append("  %6.2f%%  %s%s" % (pct(cost), "... ; " if len(stack) > 6 else "", " ; ".join(shown)))
    return out


def run_profile(cmd, outdir, depth, label, db, symbol_names, keep_raw=False, repo=None):
    vg = valgrind_path()
    if not vg:
        raise SystemExit(INSTALL_HINT)
    os.makedirs(outdir, exist_ok=True)
    raw = os.path.join(outdir, "callgrind.out")
    vg_cmd = [vg, "--tool=callgrind", "--separate-callers=%d" % depth, "--separate-recs=1",
              "--callgrind-out-file=" + raw, "--compress-strings=yes"] + cmd
    t0 = time.time()
    res = subprocess.run(vg_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    seconds = time.time() - t0
    with open(os.path.join(outdir, "program.stdout"), "wb") as fh:
        fh.write(res.stdout)
    with open(os.path.join(outdir, "valgrind.stderr"), "wb") as fh:
        fh.write(res.stderr)
    if not os.path.exists(raw):
        raise SystemExit("codemap profile: callgrind wrote no output (exit %d); see %s"
                         % (res.returncode, os.path.join(outdir, "valgrind.stderr")))
    cg = parse_callgrind(raw)
    mapper = Mapper(db, repo)
    folded, funcs, edges = analyse(cg, mapper, depth)
    unmatched, unindexed = unmatched_frames(funcs, mapper)
    meta = {"label": label, "cmd": cmd, "exit_status": res.returncode, "depth": depth,
            "seconds": round(seconds, 1), "events": cg["events"], "at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    lines = write_outputs(outdir, meta, folded, funcs, edges, symbol_names, unmatched, unindexed)
    if not keep_raw:
        os.remove(raw)
    return lines, meta
