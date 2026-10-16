#!/usr/bin/env python3
"""codemap -- a map of the NAAb C++ source an LLM (or a person) can query.

    python3 tools/codemap/codemap.py index            # build/refresh (needs libclang)
    python3 tools/codemap/codemap.py explain enforce  # one card: callers, callees, overrides, tags
    python3 tools/codemap/codemap.py callers GovernanceEngine::checkPathAccess --depth 3
    python3 tools/codemap/codemap.py callees agentSend
    python3 tools/codemap/codemap.py paths main GovernanceEngine::enforce
    python3 tools/codemap/codemap.py reach fopen --gate GovernanceEngine::checkPathAccess
    python3 tools/codemap/codemap.py types Executor            # class tree, both directions
    python3 tools/codemap/codemap.py threads                   # where threads start, what they run
    python3 tools/codemap/codemap.py tags --kind ref:A45       # task tags and record references
    python3 tools/codemap/codemap.py jump enforce              # named bookmark -> file:line
    python3 tools/codemap/codemap.py uml Executor              # PlantUML class diagram text
    python3 tools/codemap/codemap.py dot agentSend             # Graphviz call graph text

Every answer says how far to trust it:
  [ast]       a direct call, read from the compiler's syntax tree
  [value]     the function is taken as a value (callback, std::function) at that
              site; it may be called from somewhere no syntax tree shows
  [lambda]    a lambda defined in that function; who invokes it is decided at run time
  [virtual]   a call through a base-class method, which may land in this override
  [dispatch]  a NAAb name (stdlib `module.fn`, `<<lang>>` executor) mapped to C++
              by recognising the dispatch pattern -- screened, not compiled
  [thread]    started on another thread (std::thread/async, a pool's enqueue) -- screened
  [member]    a virtual method the registered class defines (after a [dispatch] to a class)

It does not replace running the code: whether a check FIRES on a path needs a
probe. It replaces the sweep that finds the path. Output is ASCII only (Windows
consoles mangle anything else). Add --json for jq.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import store as storemod  # noqa: E402

DEFAULT_REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
# The VM and the tree-walker gate a NAAb stdlib call BEFORE dispatching it
# (vm.cpp OP_CALL_METHOD, call_dispatch.cpp: filesystemAccessMode() ->
# checkPathAccess(), checkFunctionCapability()), keyed on runtime strings. Those
# gates are not inside the stdlib function, so no chain from a NAAb name shows
# them: an "ungated" NAAb entry point is a lead to check, not a finding.
PREDISPATCH = ("gates the VM and tree-walker apply before dispatching a NAAb stdlib call (filesystemAccessMode "
               "-> checkPathAccess, function capabilities) are not on chains that start at a NAAb name")
BLIND_SPOTS = [
    "calls through std::function or function pointers appear only where the function is taken as a value [value]",
    "code the preprocessor removes for this build (e.g. #ifdef _WIN32 on Linux) is not indexed",
    "calls inside templates on dependent names are not resolved",
    "NAAb-name dispatch and thread starts are recognised by pattern, so a new dispatch shape is invisible",
    "what actually runs for a given program needs a run (profiling is a later slice)",
    PREDISPATCH,
]
# Library calls a reader rarely wants in a callee list: constructors, member
# functions of library classes, operators, std::move. Free library functions
# (fopen, std::filesystem::exists, getenv) stay -- those are the sinks.
LIB_NOISE_KINDS = {"constructor", "destructor", "conversion_function", "cxx_method"}
LIB_NOISE_NAMES = {"std::move", "std::forward", "std::make_shared", "std::make_unique", "std::get",
                   "std::swap", "std::addressof", "std::declval"}
TIER = {"call": "ast", "ref": "value", "defines": "lambda", "dispatch": "dispatch", "thread": "thread",
        "virtual": "virtual"}


# --- output -----------------------------------------------------------------

def out(text=""):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def emit_json(obj):
    sys.stdout.buffer.write((json.dumps(obj, indent=1, ensure_ascii=True) + "\n").encode("ascii"))


def loc(file, line):
    return "%s:%s" % (file, line)


# --- index ------------------------------------------------------------------

def _extract_worker(job):
    repo, cdb_dir, source, shard_path, cmd_hash = job
    try:
        import extract  # imported in the worker: libclang is loaded per process
        data = extract.extract_tu(repo, cdb_dir, source)
        data["cmd_hash"] = cmd_hash
        tmp = shard_path + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(data, fh)
        os.replace(tmp, shard_path)
        return source, None, data.get("errors", [])
    except Exception as e:  # report and carry on; one file must not sink the index
        return source, "%s: %s" % (type(e).__name__, e), []


def extractor_version():
    """A shard is stale when the code that wrote it changes, not only its inputs."""
    h = hashlib.sha256()
    for name in ("extract.py",):
        with open(os.path.join(HERE, name), "rb") as fh:
            h.update(fh.read())
    return h.hexdigest()[:16]


def shard_name(rel):
    return hashlib.sha1(rel.encode()).hexdigest() + ".json"


def shard_fresh(repo, path, cmd_hash):
    """The shard, if no file it read has changed and the compile command is the same."""
    try:
        with open(path) as fh:
            sh = json.load(fh)
    except (OSError, ValueError):
        return None
    if sh.get("cmd_hash") != cmd_hash:
        return None
    for f, st in sh.get("deps", {}).items():
        try:
            s = os.stat(os.path.join(repo, f))
        except OSError:
            return None
        if st is None or [s.st_mtime_ns, s.st_size] != st:
            return None
    return sh


def list_sources(repo, cdb_dir, only, exclude):
    with open(os.path.join(cdb_dir, "compile_commands.json")) as fh:
        entries = json.load(fh)
    seen, out_list = set(), []
    for e in entries:
        f = os.path.abspath(os.path.join(e["directory"], e["file"]))
        rel = os.path.relpath(f, repo)
        if rel.startswith("..") or rel.startswith("external" + os.sep) or rel.startswith("build" + os.sep):
            continue
        if only and not any(rel.startswith(p) for p in only):
            continue
        if exclude and any(rel.startswith(p) for p in exclude):
            continue
        if f not in seen:
            seen.add(f)
            cmd = json.dumps([e["directory"], e.get("arguments") or e.get("command"), extractor_version()])
            out_list.append((f, hashlib.sha256(cmd.encode()).hexdigest()[:16]))
    return out_list


def unindexed_sources(repo, files, indexed):
    """Tracked C/C++ sources the build does not compile: the map cannot see them."""
    have = {os.path.relpath(f, repo) for f in indexed}
    return sorted(f for f in files if f.endswith((".cpp", ".cc", ".c")) and f not in have)


def tracked_files(repo):
    try:
        res = subprocess.run(["git", "-C", repo, "ls-files"], capture_output=True, text=True, check=True)
        files = res.stdout.splitlines()
    except (OSError, subprocess.CalledProcessError):
        files = []
        for root, dirs, names in os.walk(repo):
            dirs[:] = [d for d in dirs if d not in (".git", "build", "external", ".cache")]
            files.extend(os.path.relpath(os.path.join(root, n), repo) for n in names)
    exts = (".cpp", ".cc", ".h", ".hpp", ".c", ".py", ".sh", ".naab", ".yml")
    return [f for f in files if f.endswith(exts) and not f.startswith(("external/", "build/"))]


def cmd_index(a):
    try:
        import clang.cindex  # noqa: F401
    except ImportError:
        raise SystemExit("codemap index needs the libclang Python bindings.\n"
                         "  Debian/Ubuntu: apt-get install python3-clang   (or python3-clang-18)\n"
                         "  pip:           pip install libclang\n"
                         "Queries on an existing index do not need it.")
    os.makedirs(os.path.join(a.cache, "shards"), exist_ok=True)
    cdb_dir = a.cdb or os.path.join(a.repo, "build")
    if not os.path.exists(os.path.join(cdb_dir, "compile_commands.json")):
        raise SystemExit("codemap: no compile_commands.json in %s -- configure the build first "
                         "(cmake exports it; CMakeLists.txt sets CMAKE_EXPORT_COMPILE_COMMANDS)" % cdb_dir)
    listed = list_sources(a.repo, cdb_dir, a.only, a.exclude)
    sources = [f for f, _ in listed]
    t0 = time.time()
    jobs, shards = [], []
    for src, cmd_hash in listed:
        rel = os.path.relpath(src, a.repo)
        path = os.path.join(a.cache, "shards", shard_name(rel))
        sh = None if a.rebuild else shard_fresh(a.repo, path, cmd_hash)
        if sh is None:
            jobs.append((a.repo, cdb_dir, src, path, cmd_hash))
    sys.stderr.write("codemap: %d sources, %d to parse, %d up to date\n"
                     % (len(sources), len(jobs), len(sources) - len(jobs)))
    failed, with_errors = [], []
    if jobs:
        import multiprocessing as mp
        ctx = mp.get_context("fork") if hasattr(os, "fork") else mp.get_context()
        with ctx.Pool(max(1, a.jobs)) as pool:
            for i, (src, err, diag) in enumerate(pool.imap_unordered(_extract_worker, jobs), 1):
                if err:
                    failed.append([os.path.relpath(src, a.repo), err])
                elif diag:
                    with_errors.append([os.path.relpath(src, a.repo), diag[0]])
                if i % 20 == 0 or i == len(jobs):
                    sys.stderr.write("codemap: parsed %d/%d (%ds)\n" % (i, len(jobs), time.time() - t0))
    for src in sources:
        path = os.path.join(a.cache, "shards", shard_name(os.path.relpath(src, a.repo)))
        try:
            with open(path) as fh:
                shards.append(json.load(fh))
        except (OSError, ValueError):
            pass
    # Count parse errors over every shard, not only the ones parsed this run.
    with_errors = [[sh["source"], sh["errors"][0]] for sh in shards if sh.get("errors")]
    tracked = tracked_files(a.repo)
    tags = storemod.scan_tags(a.repo, tracked)
    head = ""
    try:
        head = subprocess.run(["git", "-C", a.repo, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
    except OSError:
        pass
    unindexed = [] if (a.only or a.exclude) else unindexed_sources(a.repo, tracked, sources)
    meta = {"built_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "git_head": head or "(not a git checkout)",
            "sources": len(sources), "parsed": len(jobs), "failed": failed,
            "parse_errors": with_errors, "unindexed": unindexed, "seconds": round(time.time() - t0, 1)}
    storemod.build(os.path.join(a.cache, "codemap.db"), shards, tags, meta)
    sys.stderr.write("codemap: index built in %ds -- %d shards, %d failed, %d with parse errors\n"
                     % (time.time() - t0, len(shards), len(failed), len(with_errors)))
    for f, e in failed[:10]:
        sys.stderr.write("  FAILED %s: %s\n" % (f, e))
    return 1 if failed else 0


# --- shared resolution ---------------------------------------------------------

FILE_QUALIFIED = re.compile(r"^([\w./-]+\.(?:cpp|cc|c|h|hpp)):(?!:)(.+)$")


def pick(st, query, kinds=None):
    """Symbols for a query. `FILE:NAME` restricts to one file (src/cli/main.cpp:main)."""
    m = FILE_QUALIFIED.match(query)
    if m:
        syms = [x for x in st.resolve(m.group(2), kinds) if x["file"] == m.group(1)]
        if not syms:
            raise SystemExit("codemap: no symbol %r defined in %s" % (m.group(2), m.group(1)))
        return pick_among(syms, m.group(2))
    syms = st.resolve(query, kinds)
    if not syms:
        raise SystemExit("codemap: no symbol matches %r (try `codemap find %s`)" % (query, query))
    return pick_among(syms, query)


def pick_among(syms, query):
    # A name written in full wins outright: `getenv` is libc's, even though
    # naab::platform::getenv is the only DEFINITION -- a library sink has none.
    full = [s for s in syms if s["name"] == query or s["usr"] == query]
    defs = [s for s in syms if s["is_def"]]
    suffix = [s for s in (defs or syms) if s["name"].endswith("::" + query)]
    choice = full or suffix or defs or syms
    if len(choice) > 1 and len({s["name"] for s in choice}) > 1:
        out("codemap: %r is ambiguous; candidates:" % query)
        for s in choice[:20]:
            out("  %-70s %s" % (s["name"], loc(s["file"], s["line"])))
        out("Repeat with a longer qualified name, or FILE:NAME.")
        raise SystemExit(2)
    return choice


def name_of(st, usr):
    s = st.symbol(usr)
    if s:
        return s["name"]
    c = st.class_row(usr)
    return c["name"] if c else usr


def ancestors_overridden(st, usr):
    """Base methods this method overrides, transitively."""
    seen, todo = [], [usr]
    while todo:
        u = todo.pop()
        for _, _, base, _, _, _ in st.edges_from(u, ["override"]):
            if base not in seen:
                seen.append(base)
                todo.append(base)
    return seen


def overriders(st, usr):
    seen, todo = [], [usr]
    while todo:
        u = todo.pop()
        for _, sub, _, _, _, _ in st.edges_to(u, ["override"]):
            if sub not in seen:
                seen.append(sub)
                todo.append(sub)
    return seen


def incoming(st, usr):
    """(caller_usr, tier, site_file, site_line) for everything that reaches usr."""
    rows = []
    for kind, src, _, f, l, note in st.edges_to(usr, ["call", "ref", "defines", "dispatch", "thread"]):
        rows.append((src, TIER[kind], f, l))
    for base in ancestors_overridden(st, usr):
        for kind, src, _, f, l, note in st.edges_to(base, ["call"]):
            rows.append((src, "virtual", f, l))
    return rows


def outgoing(st, usr):
    if usr.startswith("c:") and st.symbol(usr) is None and st.class_row(usr):
        # A class reached through a registration (<<lang>> -> executor class):
        # its next step is the virtual methods it defines.
        return [(m["usr"], "member", m["file"], m["line"]) for m in st.methods_of(usr)
                if m and m["virtual"] and m["is_def"]]
    rows = []
    for kind, _, dst, f, l, note in st.edges_from(usr, ["call", "ref", "defines", "dispatch", "thread"]):
        rows.append((dst, TIER[kind], f, l))
        if kind == "call" and note == "virtual":
            for sub in overriders(st, dst):
                rows.append((sub, "virtual", f, l))
    return rows


def is_library_noise(st, usr):
    s = st.symbol(usr)
    if not s or not s["file"] or not (s["file"].startswith("/") or s["file"].startswith("..")):
        return False
    return (s["kind"] in LIB_NOISE_KINDS or s["name"].split("::")[-1].startswith("operator")
            or s["name"] in LIB_NOISE_NAMES)


def filter_noise(st, rows, show_all):
    """(kept rows, number hidden)."""
    if show_all:
        return rows, 0
    kept = [r for r in rows if not is_library_noise(st, r[0])]
    return kept, len(rows) - len(kept)


def dedupe(rows):
    seen, outl = set(), []
    for r in rows:
        if r not in seen:
            seen.add(r)
            outl.append(r)
    return outl


# --- queries -------------------------------------------------------------------

def cmd_find(a, st):
    syms = st.resolve(a.pattern)
    res = [{"name": s["name"], "kind": s["kind"], "at": loc(s["file"], s["line"]), "def": bool(s["is_def"])}
           for s in syms[:a.max]]
    if a.json:
        return emit_json(res)
    for r in res:
        out("%-14s %-70s %s%s" % (r["kind"], r["name"], r["at"], "" if r["def"] else "  (decl)"))
    if len(syms) > a.max:
        out("... %d more (raise --max)" % (len(syms) - a.max))


def tree(st, usr, direction, depth, max_children, show_all=False, path=None, lines=None, indent=0):
    path = (path or []) + [usr]
    lines = [] if lines is None else lines
    rows = dedupe(incoming(st, usr) if direction == "up" else outgoing(st, usr))
    rows, hidden = filter_noise(st, rows, show_all)
    if hidden:
        lines.append({"depth": indent + 1, "hidden": hidden})
    shown = rows[:max_children]
    for other, tier, f, l in shown:
        cyc = other in path
        lines.append({"depth": indent + 1, "symbol": other, "name": name_of(st, other), "tier": tier,
                      "site": loc(f, l), "cycle": cyc})
        if not cyc and indent + 1 < depth:
            tree(st, other, direction, depth, max_children, show_all, path, lines, indent + 1)
    if len(rows) > max_children:
        lines.append({"depth": indent + 1, "more": len(rows) - max_children})
    return lines


def print_tree(title, lines, direction):
    out(title)
    arrow = "<-" if direction == "up" else "->"
    for ln in lines:
        pad = "  " * ln["depth"]
        if "more" in ln:
            out("%s... +%d more (raise --max)" % (pad, ln["more"]))
            continue
        if "hidden" in ln:
            out("%s(%d library calls hidden: constructors, operators, library methods; --all shows them)"
                % (pad, ln["hidden"]))
            continue
        out("%s%s %-64s [%s] %s%s" % (pad, arrow, ln["name"], ln["tier"], ln["site"],
                                      "  (cycle)" if ln["cycle"] else ""))


def cmd_callers(a, st):
    for s in pick(st, a.symbol):
        lines = tree(st, s["usr"], "up", a.depth, a.max, True)
        if a.json:
            emit_json({"symbol": s["name"], "callers": lines})
        else:
            print_tree("callers of %s  (%s)  depth %d" % (s["name"], loc(s["file"], s["line"]), a.depth),
                       lines, "up")


def cmd_callees(a, st):
    for s in pick(st, a.symbol):
        lines = tree(st, s["usr"], "down", a.depth, a.max, a.all)
        if a.json:
            emit_json({"symbol": s["name"], "callees": lines})
        else:
            print_tree("callees of %s  (%s)  depth %d" % (s["name"], loc(s["file"], s["line"]), a.depth),
                       lines, "down")


def cmd_paths(a, st):
    srcs = [s["usr"] for s in pick(st, a.source)]
    dsts = {s["usr"] for s in pick(st, a.target)}
    found = []
    q = deque([[(u, None, None)] for u in srcs])
    seen = {u: 0 for u in srcs}
    while q and len(found) < a.max:
        p = q.popleft()
        u = p[-1][0]
        if u in dsts and len(p) > 1:
            found.append(p)
            continue
        if len(p) > a.depth:
            continue
        for dst, tier, f, l in dedupe(outgoing(st, u)):
            if dst in seen and seen[dst] < len(p):
                if dst not in dsts:
                    continue
            seen[dst] = len(p)
            q.append(p + [(dst, tier, loc(f, l))])
    res = [[{"name": name_of(st, u), "tier": t, "site": site} for u, t, site in p] for p in found]
    if a.json:
        return emit_json(res)
    if not res:
        out("no call path from %s to %s within depth %d (blind spots: %s)"
            % (a.source, a.target, a.depth, "; ".join(BLIND_SPOTS[:3])))
        return 1
    for i, p in enumerate(res, 1):
        out("path %d (%d hops):" % (i, len(p) - 1))
        for step in p:
            out("   %s%s" % (step["name"], "" if step["tier"] is None else "   [%s] %s" % (step["tier"], step["site"])))


def is_root(st, usr):
    if usr.startswith(("naab:", "lang:", "codemap:static-init")):
        return True
    s = st.symbol(usr)
    if s and (s["name"] == "main" or s["file"].startswith("tests/")):
        return True
    return not incoming(st, usr)


def gate_wrappers(st, gates, hops):
    """Functions that reach a gate through at most `hops` direct calls, with the
    gate each reaches. A gate is usually called through a helper; a search that
    knows only the gate's own name reports every caller of the helper as ungated."""
    found = {g: (0, g) for g in gates}
    frontier = list(gates)
    for n in range(1, hops):
        nxt = []
        for u in frontier:
            for kind, src, _, _, _, _ in st.edges_to(u, ["call", "dispatch"]):
                if src not in found:
                    found[src] = (n, found[u][1])
                    nxt.append(src)
        frontier = nxt
    return found


def entry_group(st, usr):
    if usr.startswith(("naab:", "lang:")):
        return "naab"
    s = st.symbol(usr)
    if s and s["file"].startswith("tests/"):
        return "test"
    if usr.startswith("codemap:static-init") or (s and s["name"] == "main"):
        return "main"
    return "uncalled"


ENTRY_GROUPS = [("naab", "NAAb names (a program can call these)"), ("main", "main / static initialisers"),
                ("uncalled", "nothing in the index calls these (dead, or called through a pointer)"),
                ("test", "test code")]


def cmd_reach(a, st):
    """Search backward from a sink: which entry points reach it with no gate on the way."""
    picked = pick(st, a.sink)
    sinks = [x["usr"] for x in picked]
    gates = set()
    for g in a.gate or []:
        hit = st.resolve(g)
        if not hit:
            # A gate that names nothing would silently gate nothing.
            raise SystemExit("codemap: gate %r matches no symbol (try `codemap find %s`)" % (g, g))
        gates.update(x["usr"] for x in hit)
    wrappers = gate_wrappers(st, gates, a.gate_hops) if gates else {}
    direct = sum(len(dedupe(incoming(st, u))) for u in sinks)
    toward = {u: None for u in sinks}       # function -> (next hop toward the sink, tier, site)
    depth = {u: 0 for u in sinks}
    gated, entries, cut = {}, [], []
    q = deque(sinks)
    while q:
        u = q.popleft()
        if depth[u] > 0 and is_root(st, u):
            entries.append(u)
        if depth[u] >= a.depth:
            cut.append(u)
            continue
        for src, tier, f, l in dedupe(incoming(st, u)):
            if src in toward or src in gated:
                continue
            via = [dst for dst, t, _, _ in outgoing(st, src) if t in ("ast", "dispatch") and dst in wrappers]
            if via:
                gated[src] = (loc(f, l), via[0])
                continue
            toward[src] = (u, tier, loc(f, l))
            depth[src] = depth[u] + 1
            q.append(src)

    def chain(u):
        hops = [name_of(st, u)]
        while toward.get(u):
            u, tier, site = toward[u]
            hops.append("[%s %s] %s" % (tier, site, name_of(st, u)))
        return hops

    groups = {g: [u for u in entries if entry_group(st, u) == g] for g, _ in ENTRY_GROUPS}
    res = {"sink": a.sink, "resolved": [x["name"] + " " + loc(x["file"], x["line"]) for x in picked],
           "direct_callers": direct, "gates": a.gate or [], "depth": a.depth,
           "gate_wrappers": [{"function": name_of(st, u), "hops_to_gate": n, "gate": name_of(st, g)}
                             for u, (n, g) in sorted(wrappers.items(), key=lambda kv: kv[1][0]) if n > 0],
           "functions_reaching": len(toward) - len(sinks),
           "gated": [{"function": name_of(st, u), "site": site, "via": name_of(st, w)}
                     for u, (site, w) in sorted(gated.items(), key=lambda kv: name_of(st, kv[0]))],
           "entry_points": {g: [{"entry": name_of(st, u), "chain": chain(u)} for u in groups[g]]
                            for g, _ in ENTRY_GROUPS},
           "stopped_at_depth": len(cut),
           "tier": "call graph, backward [ast/value/lambda/virtual/dispatch/thread]; 'gated' means the "
                   "function calls a gate or a gate wrapper -- screened: order and conditions are not "
                   "checked, so a 'gated' function may still reach the sink another way"}
    if a.json:
        return emit_json(res)
    out("reach %s%s, depth %d" % (a.sink, "  (gate: %s)" % ", ".join(a.gate) if a.gate else "", a.depth))
    out("  " + res["tier"])
    out("sink: %s; %d direct call sites%s" % ("; ".join(res["resolved"]), direct,
                                          "  -- NOTHING in the index calls it" if not direct else ""))
    if a.gate:
        out("gate wrappers (%d: each calls a gate within %d hop(s), so it counts as the gate):"
            % (len(res["gate_wrappers"]), max(0, a.gate_hops - 1)))
        for w in res["gate_wrappers"][:a.max]:
            out("   %s  -> %s" % (w["function"], w["gate"]))
        out("gated (%d -- the search stopped at each; verify these, they are the reassuring answer):"
            % len(res["gated"]))
        for g in res["gated"][:a.max]:
            out("   %s  %s  via %s" % (g["function"], g["site"], g["via"]))
        if len(res["gated"]) > a.max:
            out("   ... +%d more (raise --max)" % (len(res["gated"]) - a.max))
    out("%d functions reach it with no gate on the way; entry points among them, one chain each:"
        % res["functions_reaching"])
    for g, title in ENTRY_GROUPS:
        rows = res["entry_points"][g]
        if g == "test" and not a.tests:
            out("  %s: %d (--tests lists them)" % (title, len(rows)))
            continue
        out("  %s: %d" % (title, len(rows)))
        if g == "naab" and rows:
            out("     note: " + PREDISPATCH + " (codemap jump fs-allowlist)")
        for e in rows[:a.max]:
            out("     %s" % e["entry"])
            out("        " + " -> ".join(e["chain"][1:]))
        if len(rows) > a.max:
            out("     ... +%d more (raise --max, or --json)" % (len(rows) - a.max))
    if cut:
        out("%d functions were at the depth limit and not searched further (raise --depth)" % len(cut))


def class_tree(st, usr, direction, depth, indent=1, path=None):
    path = (path or []) + [usr]
    rows = []
    if direction == "down":
        nxt = st.subclasses_of(usr)
    else:
        nxt = [b for b, _ in st.bases_of(usr) if b]
    for n in nxt:
        c = st.class_row(n)
        rows.append({"depth": indent, "name": c["name"] if c else n, "at": loc(c["file"], c["line"]) if c else ""})
        if n not in path and indent < depth:
            rows.extend(class_tree(st, n, direction, depth, indent + 1, path))
    return rows


def cmd_types(a, st):
    classes = st.class_by_name(a.cls)
    if not classes:
        raise SystemExit("codemap: no class matches %r" % a.cls)
    for c in classes:
        up = class_tree(st, c["usr"], "up", a.depth)
        down = class_tree(st, c["usr"], "down", a.depth)
        if a.json:
            emit_json({"class": c["name"], "at": loc(c["file"], c["line"]), "bases": up, "subclasses": down})
            continue
        out("class %s  %s%s" % (c["name"], loc(c["file"], c["line"]), "  (NAAb module '%s')" % c["module"]
                                if c["module"] else ""))
        out("bases:")
        for r in up:
            out("%s^ %s  %s" % ("  " * r["depth"], r["name"], r["at"]))
        out("subclasses (%d):" % len(down))
        for r in down:
            out("%sv %s  %s" % ("  " * r["depth"], r["name"], r["at"]))


def cmd_includes(a, st):
    path = os.path.relpath(os.path.abspath(a.file), a.repo) if os.path.exists(a.file) else a.file
    rows = st.includes(path, reverse=a.by)
    if a.json:
        return emit_json({"file": path, "included_by" if a.by else "includes": rows})
    out("%s %s (%d):" % (path, "is included by" if a.by else "includes", len(rows)))
    for r in rows:
        out("   " + r)


def cmd_threads(a, st):
    rows = st.db.execute("SELECT src,dst,file,line,note FROM edges WHERE kind='thread' ORDER BY file,line").fetchall()
    res = []
    for src, dst, f, l, note in rows:
        rows, _ = filter_noise(st, [r for r in dedupe(outgoing(st, dst)) if r[1] in ("ast", "virtual")], False)
        callees = [name_of(st, d) for d, _, _, _ in rows][:8]
        res.append({"started_by": name_of(st, src), "site": loc(f, l), "how": note,
                    "entry": name_of(st, dst), "entry_calls": callees})
    if a.json:
        return emit_json(res)
    out("thread starts (%d) [thread: screened by pattern]" % len(res))
    for r in res:
        out("%s  %s  via %s" % (r["site"], r["started_by"], r["how"]))
        out("   runs %s" % r["entry"])
        if r["entry_calls"]:
            out("   which calls: " + ", ".join(r["entry_calls"]))


def cmd_tags(a, st):
    rows = st.tags(a.kind, a.file)
    if a.json:
        return emit_json([{"at": loc(f, l), "tag": t, "text": x} for f, l, t, x in rows[:a.max]])
    for f, l, t, x in rows[:a.max]:
        out("%-48s %-14s %s" % (loc(f, l), t, x))
    if len(rows) > a.max:
        out("... %d more (raise --max or filter with --kind/--file)" % (len(rows) - a.max))


def load_anchors(repo, path=None):
    path = path or os.path.join(HERE, "anchors.txt")
    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2:
                rows.append((parts[0].strip(), parts[1].strip(), parts[2].strip() if len(parts) > 2 else ""))
    return rows


def grep_definition(repo, name):
    """Without an index: lines that look like NAME's definition (screened).
    `A::B::name` is matched as `B::name(`; a bare name as `name(` on a line with no `;`."""
    parts = name.split("::")
    if len(parts) > 1:
        rx = re.compile(r"\b%s::%s\s*\(" % (re.escape(parts[-2]), re.escape(parts[-1])))
    else:
        rx = re.compile(r"^[^/;]*\b%s\s*\([^;]*$" % re.escape(parts[-1]))
    hits = []
    for rel in tracked_files(repo):
        if not rel.endswith((".cpp", ".cc", ".c", ".h", ".hpp")):
            continue
        try:
            with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as fh:
                for n, line in enumerate(fh, 1):
                    if rx.search(line) and not line.lstrip().startswith(("//", "*")):
                        hits.append(loc(rel, n))
        except OSError:
            continue
    return hits[:5] or None


def resolve_anchor(repo, st, target):
    """symbol:NAME through the index (or a screened grep without one);
    grep:FILE:REGEX by reading the file now."""
    if target.startswith("symbol:"):
        if st is None:
            return grep_definition(repo, target[7:])
        syms = [s for s in st.resolve(target[7:]) if s["is_def"]]
        return [loc(s["file"], s["line"]) for s in syms[:5]] or None
    if target.startswith("grep:"):
        _, file, rx = target.split(":", 2)
        hits = []
        try:
            with open(os.path.join(repo, file), encoding="utf-8", errors="replace") as fh:
                for n, line in enumerate(fh, 1):
                    if re.search(rx, line):
                        hits.append(loc(file, n))
        except OSError:
            return None
        return hits or None
    return None


def cmd_anchors(a, st):
    res = []
    if st is None and not a.json:
        out("(no index: symbol anchors located by grep -- screened)")
    for name, target, note in load_anchors(a.repo, a.anchors):
        if a.name and name != a.name:
            continue
        res.append({"name": name, "at": resolve_anchor(a.repo, st, target), "note": note, "target": target})
    if a.name and not res:
        raise SystemExit("codemap: no anchor %r (see %s)" % (a.name, a.anchors or "tools/codemap/anchors.txt"))
    if a.json:
        return emit_json(res)
    bad = 0
    for r in res:
        where = ", ".join(r["at"]) if r["at"] else "UNRESOLVED (%s)" % r["target"]
        bad += r["at"] is None
        out("%-22s %s" % (r["name"], where))
        if r["note"] and (a.name or a.verbose):
            out("%-22s %s" % ("", r["note"]))
    return 1 if bad and a.check else 0


def cmd_explain(a, st):
    for s in pick(st, a.symbol):
        usr = s["usr"]
        callers = dedupe(incoming(st, usr))
        callees, hidden = filter_noise(st, dedupe(outgoing(st, usr)), a.all)
        over = ancestors_overridden(st, usr)
        subs = overriders(st, usr)
        tags = st.tags_in(s["file"], s["line"], s["end_line"]) if s["file"] and not s["file"].startswith("/") else []
        anchors = [n for n, t, _ in load_anchors(a.repo, a.anchors) if t == "symbol:" + s["name"]
                   or (t.startswith("symbol:") and s["name"].endswith("::" + t[7:]))]
        card = {"name": s["name"], "kind": s["kind"], "at": loc(s["file"], s["line"]),
                "end_line": s["end_line"], "signature": s["sig"],
                "class": name_of(st, s["class_usr"]) if s["class_usr"] else "",
                "overrides": [name_of(st, u) for u in over], "overridden_by": [name_of(st, u) for u in subs],
                "callers": [{"name": name_of(st, u), "tier": t, "site": loc(f, l)} for u, t, f, l in callers],
                "callees": [{"name": name_of(st, u), "tier": t, "site": loc(f, l)} for u, t, f, l in callees],
                "tags": [{"at": loc(f, l), "tag": t, "text": x} for f, l, t, x in tags],
                "library_calls_hidden": hidden, "anchors": anchors, "blind_spots": BLIND_SPOTS}
        if a.json:
            emit_json(card)
            continue
        out("%s  [%s]" % (card["name"], card["kind"]))
        out("  at        %s-%s%s" % (card["at"], card["end_line"], "" if s["is_def"] else "  (declaration only)"))
        if card["signature"]:
            out("  sig       %s" % card["signature"])
        if card["class"]:
            out("  class     %s" % card["class"])
        if card["overrides"]:
            out("  overrides %s" % ", ".join(card["overrides"]))
        if card["overridden_by"]:
            out("  overridden by %s" % ", ".join(card["overridden_by"]))
        if card["anchors"]:
            out("  anchors   %s" % ", ".join(card["anchors"]))
        files = sorted({c["site"].rsplit(":", 1)[0] for c in card["callers"]})
        out("  callers   %d sites in %d files" % (len(card["callers"]), len(files)))
        for c in card["callers"][:a.max]:
            out("     <- %-60s [%s] %s" % (c["name"], c["tier"], c["site"]))
        if len(card["callers"]) > a.max:
            out("     ... +%d more (codemap callers)" % (len(card["callers"]) - a.max))
        out("  callees   %d%s" % (len(card["callees"]), "  (+%d library calls hidden; --all)" % hidden
                                       if hidden else ""))
        for c in card["callees"][:a.max]:
            out("     -> %-60s [%s] %s" % (c["name"], c["tier"], c["site"]))
        if len(card["callees"]) > a.max:
            out("     ... +%d more (codemap callees)" % (len(card["callees"]) - a.max))
        for t in card["tags"]:
            out("  tag       %s %s %s" % (t["at"], t["tag"], t["text"]))
        out("  not seen: " + "; ".join(BLIND_SPOTS[:3]))


def cmd_uml(a, st):
    classes = st.class_by_name(a.cls)
    if not classes:
        raise SystemExit("codemap: no class matches %r" % a.cls)
    root = classes[0]
    members = {root["usr"]}
    frontier = [root["usr"]]
    for _ in range(a.depth):
        nxt = []
        for u in frontier:
            for n in st.subclasses_of(u) + [b for b, _ in st.bases_of(u) if b]:
                if n not in members:
                    members.add(n)
                    nxt.append(n)
        frontier = nxt
    alias = {u: re.sub(r"[^A-Za-z0-9_]", "_", name_of(st, u)) for u in members}
    lines = ["@startuml", "' generated by tools/codemap/codemap.py uml %s -- from the compiler's view of the classes"
             % a.cls, "' members shown: virtual methods only (the interface); {abstract} = pure virtual",
             "hide empty members"]
    for u in sorted(members, key=lambda x: name_of(st, x)):
        c = st.class_row(u)
        label = name_of(st, u)
        methods = [m for m in st.methods_of(u) if m and m["kind"] in ("cxx_method", "function_template")]
        virt = [m for m in methods if m["virtual"]]
        body = ["  %s%s()" % ("{abstract} " if m["virtual"] == 2 else "", m["name"].split("::")[-1])
                for m in virt[:12]]
        lines.append('class "%s" as %s%s' % (label, alias[u], " <<%s:%s>>" % (c["file"], c["line"]) if c else ""))
        if body:
            lines[-1] += " {"
            lines.extend(body)
            lines.append("}")
    for u in members:
        for b, bname in st.bases_of(u):
            if b in members:
                lines.append("%s <|-- %s" % (alias[b], alias[u]))
    lines.append("@enduml")
    text = "\n".join(lines) + "\n"
    if a.out:
        with open(a.out, "w") as fh:
            fh.write(text)
    else:
        out(text.rstrip("\n"))
    if a.render:
        return render_plantuml(a, text)


def render_plantuml(a, text):
    """Render only with a jar whose sha256 is pinned in plantuml.sha256."""
    jar = os.environ.get("CODEMAP_PLANTUML_JAR") or os.path.join(a.cache, "plantuml.jar")
    pins = set()
    with open(os.path.join(HERE, "plantuml.sha256"), encoding="utf-8") as fh:
        for line in fh:
            if line.strip() and not line.startswith("#"):
                pins.add(line.split()[0].lower())
    if not os.path.exists(jar):
        sys.stderr.write("codemap: no PlantUML jar at %s; the .puml text above renders in any PlantUML viewer\n" % jar)
        return 3
    digest = hashlib.sha256(open(jar, "rb").read()).hexdigest()
    if digest not in pins:
        sys.stderr.write("codemap: refusing to run %s: sha256 %s is not pinned in tools/codemap/plantuml.sha256.\n"
                         "  Verify the jar against its publisher's checksum, then add the sha256 there.\n"
                         % (jar, digest))
        return 3
    target = a.out or os.path.join(a.cache, "uml.puml")
    if not a.out:
        with open(target, "w") as fh:
            fh.write(text)
    res = subprocess.run(["java", "-jar", jar, "-tsvg", target], capture_output=True, text=True)
    sys.stderr.write(res.stderr)
    svg = os.path.splitext(target)[0] + ".svg"
    sys.stderr.write("codemap: wrote %s\n" % svg if os.path.exists(svg) else "codemap: render failed\n")
    return 0 if os.path.exists(svg) else 3


def cmd_dot(a, st):
    s = pick(st, a.symbol)[0]
    nodes, edges = {s["usr"]}, set()
    for direction in ("up", "down"):
        frontier = [s["usr"]]
        for _ in range(a.depth):
            nxt = []
            for u in frontier:
                rows = filter_noise(st, dedupe(incoming(st, u) if direction == "up" else outgoing(st, u)),
                                    a.all)[0][:a.max]
                for other, tier, _, _ in rows:
                    edges.add((other, u, tier) if direction == "up" else (u, other, tier))
                    if other not in nodes:
                        nodes.add(other)
                        nxt.append(other)
            frontier = nxt
    ids = {u: "n%d" % i for i, u in enumerate(sorted(nodes))}
    lines = ["digraph codemap {", '  rankdir=LR; node [shape=box, fontsize=10];']
    for u in sorted(nodes):
        style = ', style=bold' if u == s["usr"] else ""
        lines.append('  %s [label="%s"%s];' % (ids[u], name_of(st, u).replace('"', "'"), style))
    for x, y, tier in sorted(edges):
        st_ = "" if tier == "ast" else ' [style=dashed, label="%s"]' % tier
        lines.append("  %s -> %s%s;" % (ids[x], ids[y], st_))
    lines.append("}")
    out("\n".join(lines))


def cmd_stats(a, st):
    m = st.meta()
    kinds = dict(st.db.execute("SELECT kind, COUNT(*) FROM edges GROUP BY kind").fetchall())
    res = {"built_at": m.get("built_at"), "git_head": m.get("git_head"), "sources": m.get("sources"),
           "failed": m.get("failed"), "parse_errors": len(m.get("parse_errors") or []),
           "symbols": st.count("symbols"), "classes": st.count("classes"), "edges": kinds,
           "tags": st.count("tags"), "unindexed": m.get("unindexed") or [], "blind_spots": BLIND_SPOTS}
    if a.json:
        return emit_json(res)
    out("index built %s at %s from %s sources (%s failed, %s with parse errors)"
        % (res["built_at"], res["git_head"], res["sources"], len(res["failed"] or []), res["parse_errors"]))
    out("symbols %d, classes %d, tags %d" % (res["symbols"], res["classes"], res["tags"]))
    out("edges: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items())))
    if res["unindexed"]:
        out("not in the build, so not indexed (%d tracked C/C++ files): %s%s"
            % (len(res["unindexed"]), ", ".join(res["unindexed"][:8]),
               " ..." if len(res["unindexed"]) > 8 else ""))
    out("not seen:")
    for b in BLIND_SPOTS:
        out("  - " + b)


def main(argv=None):
    p = argparse.ArgumentParser(prog="codemap", description=__doc__.split("\n\n")[0])
    p.add_argument("--repo", default=DEFAULT_REPO)
    p.add_argument("--cache", default=None, help="index directory (default REPO/.cache/codemap)")
    p.add_argument("--json", action="store_true")
    p.add_argument("--anchors", help="bookmarks file (default tools/codemap/anchors.txt)")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("index"); s.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    s.add_argument("--cdb"); s.add_argument("--only", nargs="*"); s.add_argument("--exclude", nargs="*")
    s.add_argument("--rebuild", action="store_true")
    s = sub.add_parser("stats")
    s = sub.add_parser("find"); s.add_argument("pattern"); s.add_argument("--max", type=int, default=40)
    for name in ("callers", "callees"):
        s = sub.add_parser(name); s.add_argument("symbol")
        s.add_argument("--depth", type=int, default=2); s.add_argument("--max", type=int, default=25)
        s.add_argument("--all", action="store_true", help="include library constructors/operators/methods")
    s = sub.add_parser("explain"); s.add_argument("symbol"); s.add_argument("--max", type=int, default=15)
    s.add_argument("--all", action="store_true")
    s = sub.add_parser("paths"); s.add_argument("source"); s.add_argument("target")
    s.add_argument("--depth", type=int, default=8); s.add_argument("--max", type=int, default=5)
    s = sub.add_parser("reach"); s.add_argument("sink"); s.add_argument("--gate", nargs="*")
    s.add_argument("--depth", type=int, default=10); s.add_argument("--max", type=int, default=20)
    s.add_argument("--gate-hops", type=int, default=2,
                   help="a function this many calls from a gate counts as the gate (1 = the gate only)")
    s.add_argument("--tests", action="store_true", help="list entry points in test code too")
    s = sub.add_parser("types"); s.add_argument("cls"); s.add_argument("--depth", type=int, default=6)
    s = sub.add_parser("includes"); s.add_argument("file"); s.add_argument("--by", action="store_true")
    s = sub.add_parser("threads")
    s = sub.add_parser("tags"); s.add_argument("--kind"); s.add_argument("--file")
    s.add_argument("--max", type=int, default=60)
    for name in ("anchors", "jump"):
        s = sub.add_parser(name); s.add_argument("name", nargs="?")
        s.add_argument("--verbose", action="store_true"); s.add_argument("--check", action="store_true")
    s = sub.add_parser("uml"); s.add_argument("cls"); s.add_argument("--depth", type=int, default=2)
    s.add_argument("--out"); s.add_argument("--render", action="store_true")
    s = sub.add_parser("dot"); s.add_argument("symbol"); s.add_argument("--depth", type=int, default=1)
    s.add_argument("--max", type=int, default=15); s.add_argument("--all", action="store_true")
    a = p.parse_args(argv)
    a.repo = os.path.abspath(a.repo)
    a.cache = os.path.abspath(a.cache or os.path.join(a.repo, ".cache", "codemap"))
    if a.cmd == "index":
        return cmd_index(a)
    db = os.path.join(a.cache, "codemap.db")
    if a.cmd in ("anchors", "jump"):
        st = storemod.Store(db) if os.path.exists(db) else None
        return cmd_anchors(a, st)
    st = storemod.Store(db)
    return globals()["cmd_" + a.cmd](a, st) or 0


if __name__ == "__main__":
    sys.exit(main())
