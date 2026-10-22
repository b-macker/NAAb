"""predispatch_enum: every check a dispatcher makes before a NAAb stdlib call runs.

Roadmap item 6. The pre-dispatch checks were found once by grepping for names
already known, which is exactly what misses a check under a new name. This
enumerates them from the code instead:

  1. parse the translation units that hold the dispatchers (libclang, through
     the code map's own extractor, so it sees what the index sees);
  2. a dispatcher is a function that calls GovernanceEngine::filesystemAccessMode,
     or that calls a check or a stdlib sink under a guard naming a stdlib marker
     (`== "__stdlib_module__:"`, `"__stdlib_call__:"`, `"__builtin__:"`) -- the
     second half found VM::callValue, which the first misses;
  3. its stdlib routes are the calls it makes under such a guard, plus
     everything a helper DEFINED IN THE SAME FILE does when it is called on such
     a route (VM::stdlibCallGates, VM::callStdlibMethod, vm.cpp's bsdPreCheck);
  4. every GovernanceEngine method, Interpreter check* method and stdlib
     set*Tainted plumbing call found there is listed with the module literals
     guarding it, and every SINK call (the call that runs the stdlib function)
     is listed as a route with a count -- searched backward from the sink, so a
     route that runs a stdlib function with no check at all is listed too.

tests/self-audit/test_predispatch_enumeration.sh compares the list with
tools/codemap/predispatch_checks.txt, where each entry is classified (gate,
detector, context, post-call, inert) -- a check added later fails the test until
someone classifies it, and the classification decides what the code map models
([predispatch] for gates, [precheck] for detectors; store.py).
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

DISPATCHER_TUS = ("src/vm/vm.cpp", "src/interpreter/call_dispatch.cpp")
MARKERS = ("__stdlib_module__:", "__stdlib_call__:", "__builtin__:")
ALLOWLIST = "GovernanceEngine::filesystemAccessMode"
CHECK = re.compile(r"(?:GovernanceEngine::\w+|Interpreter::check\w+|stdlib::set\w*Tainted)$")
# The calls that RUN a stdlib function: VM::callStdlibMethod and
# VM::callBuiltinFunction on the VM, the module's own call() on the tree-walker.
# A route is one such call under a marker guard -- searched backward from the
# sink, so a route that runs a stdlib function with no check at all is listed too.
SINK = re.compile(r"(?:VM::callStdlibMethod|VM::callBuiltinFunction|Module::call)$")


def short(name):
    """`naab::governance::GovernanceEngine::checkPathAccess` -> `GovernanceEngine::checkPathAccess`."""
    parts = name.split("::")
    return "::".join(parts[-2:]) if len(parts) >= 2 else name


def label(sym):
    """`VM::callValue(NaabVal, int)`: the short name plus the parameter types without
    namespaces, so overloads (`Interpreter::visit`) are told apart and an edit that
    moves a function does not change its label."""
    sig = re.sub(r"\b\w+::", "", sym.get("psig") or "()")
    return short(sym["name"]) + re.sub(r"\s+([&*])", r"\1", sig)


def enumerate_checks(repo, cdb_dir, tus=DISPATCHER_TUS):
    """(dispatchers, check rows, route rows, parse errors).

    A dispatcher is a function that consults the filesystem allowlist, or that
    makes a check or a sink call under a stdlib-marker guard -- unless another
    such function calls it on a stdlib route from the same file, which makes it a
    helper whose checks count for its caller. Check rows are
    (dispatcher, check, guard literals); route rows are (dispatcher, sink, guard
    literals, count) -- two routes guarded by the same literals (the VM's method
    call and zero-argument member routes) are one row with count 2."""
    import extract
    rows, routes, errors = set(), {}, []
    dispatchers_seen = set()
    for tu in tus:
        d = extract.extract_tu(repo, cdb_dir, os.path.join(repo, tu))
        shard = d.get("shard", d)
        errors += ["%s: %s" % (tu, e) for e in (d.get("errors") or shard.get("errors") or [])]
        syms = {s["usr"]: s for s in shard["symbols"]}
        names = {u: s["name"] for u, s in syms.items()}
        files = {u: s.get("file") for u, s in syms.items() if s.get("is_def")}
        calls = [e for e in shard["edges"] if e[0] == "call"]
        guards = {(g[0], g[1], g[2], g[3]): g[4].split("|") for g in shard.get("guarded", [])}

        def lits_of(e):
            return guards.get((e[1], e[2], e[3], e[4]), [])

        def on_route(e):
            return any(m in lits_of(e) for m in MARKERS)

        def callee(e):
            return short(names.get(e[2], e[2]))

        allow = {u for u, n in names.items() if n.endswith(ALLOWLIST)}
        candidates = {e[1] for e in calls if e[2] in allow}
        candidates |= {e[1] for e in calls
                       if on_route(e) and (CHECK.search(callee(e)) or SINK.search(callee(e)))}
        # A function a candidate calls on a stdlib route, defined in its file, is
        # that dispatcher's helper (VM::stdlibCallGates consults the allowlist for
        # VM::run), not a dispatcher of its own.
        route_helpers = {e[2] for e in calls if e[1] in candidates and on_route(e)
                         and e[2] != e[1] and files.get(e[2]) == files.get(e[1])}
        dispatchers = candidates - route_helpers
        for dsp in dispatchers:
            dname = label(syms[dsp]) if dsp in syms else dsp
            dispatchers_seen.add(dname)
            mine = [e for e in calls if e[1] == dsp and on_route(e)]
            helpers = {e[2] for e in mine if files.get(e[2]) == files.get(dsp) and e[2] != dsp}
            for e in mine:
                lits = lits_of(e)
                mods = sorted(x for x in lits if x not in MARKERS and re.fullmatch(r"[a-z_]+", x))
                if CHECK.search(callee(e)):
                    rows.add((dname, callee(e), ",".join(mods) or "*"))
                if SINK.search(callee(e)):
                    marker = sorted(m.rstrip(":") for m in MARKERS if m in lits)
                    key = (dname, callee(e), ",".join(marker + mods))
                    routes[key] = routes.get(key, 0) + 1
            for h in helpers:
                for e in calls:
                    if e[1] == h and CHECK.search(callee(e)):
                        mods = sorted(x for x in lits_of(e) if re.fullmatch(r"[a-z_]+", x))
                        rows.add((dname, callee(e), ",".join(mods) or "*"))
    route_rows = sorted(k + (str(n),) for k, n in routes.items())
    return sorted(dispatchers_seen), sorted(rows), route_rows, errors


CLASSES = ("gate", "detector", "post-call", "context")


def read_baseline(path):
    """predispatch_checks.txt -> {key: (class, why)}: a check row's key is
    (dispatcher, method, guard), a route row's ("route", dispatcher, sink, guard,
    count) and its class is the checks that route runs."""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n").rstrip("\r")
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "route":
                parts += [""] * (7 - len(parts))
                out[tuple(parts[:5])] = (parts[5], parts[6])
            else:
                parts += [""] * (5 - len(parts))
                out[tuple(parts[:3])] = (parts[3], parts[4])
    return out


def read_enumeration(path):
    """A saved enumeration (this tool's own output) -> (dispatchers, rows)."""
    dispatchers, rows = [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n").rstrip("\r")
            if line.startswith("# dispatchers:"):
                # "; " separates them: a label's parameter list holds commas
                dispatchers = [d.strip() for d in line.split(":", 1)[1].split(";") if d.strip()]
            elif line.startswith("route\t"):
                rows.append(tuple(line.split("\t")[:5]))
            elif line and not line.startswith("#"):
                rows.append(tuple(line.split("\t")[:3]))
    return dispatchers, sorted(rows)


def compare(dispatchers, rows, baseline):
    """Every problem, one line each; empty when the list and the code agree.

    Both directions (a call the code makes that is not listed; a listed call the
    code no longer makes), the dispatcher set, each row's class, and the class
    against what store.py models -- a gate there must be a gate here and a
    detector a detector, or the map's [predispatch]/[precheck] edges and this
    list tell different stories."""
    import store
    problems = []
    listed = set(baseline)
    found = set(rows)
    for r in sorted(found - listed):
        problems.append("UNCLASSIFIED %s" % "\t".join(r))
    for r in sorted(listed - found):
        problems.append("STALE %s" % "\t".join(r))
    named = {r[1] if r[0] == "route" else r[0] for r in listed}
    for d in sorted(set(dispatchers) - named):
        problems.append("UNLISTED DISPATCHER %s" % d)
    for d in sorted(named - set(dispatchers)):
        problems.append("GONE DISPATCHER %s" % d)
    for r, (cls, why) in sorted(baseline.items()):
        if r[0] == "route":
            if not cls.strip() or not why.strip():
                problems.append("ROUTE UNEXPLAINED %s" % "\t".join(r))
            continue
        if cls not in CLASSES:
            problems.append("BAD CLASS %r %s" % (cls, "\t".join(r)))
        if not why.strip():
            problems.append("NO REASON %s" % "\t".join(r))
    gates = set(store.PREDISPATCH_GATES) | {m for _, m in store.MODULE_GATES}
    detectors = {"%s::%s" % (c, m) for c, m, _ in store.DETECTORS}
    for r, (cls, _) in sorted(baseline.items()):
        if r[0] == "route":
            continue
        method = r[1].split("::")[-1]
        if cls == "gate" and method not in gates:
            problems.append("GATE NOT MODELLED %s (store.py models gates %s)" % (r[1], sorted(gates)))
        if cls != "gate" and method in gates and r[1].startswith("GovernanceEngine::"):
            problems.append("MODELLED GATE CLASSED %s %s" % (cls, "\t".join(r)))
        if cls == "detector" and r[1] not in detectors:
            problems.append("DETECTOR NOT MODELLED %s" % r[1])
        if cls != "detector" and r[1] in detectors:
            problems.append("MODELLED DETECTOR CLASSED %s %s" % (cls, "\t".join(r)))
    for d in sorted(detectors - {r[1] for r, (c, _) in baseline.items() if c == "detector" and r[0] != "route"}):
        problems.append("MODELLED DETECTOR UNLISTED %s" % d)
    for g in sorted(gates - {r[1].split("::")[-1] for r, (c, _) in baseline.items() if c == "gate" and r[0] != "route"}):
        problems.append("MODELLED GATE UNLISTED %s" % g)
    return problems


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("repo", nargs="?", default=os.path.join(HERE, "..", ".."))
    ap.add_argument("--check", metavar="BASELINE",
                    help="compare with a classified list; exit 1 and name each difference")
    ap.add_argument("--enum", metavar="SAVED",
                    help="with --check: read a saved enumeration instead of parsing the code")
    a = ap.parse_args(sys.argv[1:] if argv is None else argv)
    repo = os.path.abspath(a.repo)
    errors = []
    if a.enum:
        dispatchers, rows = read_enumeration(a.enum)
    else:
        dispatchers, rows, routes, errors = enumerate_checks(repo, os.path.join(repo, "build"))
        rows = sorted(rows + [("route",) + r for r in routes])
    for e in errors:
        sys.stdout.write("# parse error %s\n" % e)
    if not a.check:
        sys.stdout.write("# dispatchers: %s\n" % "; ".join(dispatchers))
        for r in rows:
            sys.stdout.write("\t".join(r) + "\n")
        return 0
    if errors or not dispatchers or not rows:
        # A partial parse drops calls, which would read as STALE rows: a broken
        # probe reporting a finding. Say so instead.
        sys.stdout.write("UNMEASURABLE: %d parse errors, %d dispatchers, %d rows\n"
                         % (len(errors), len(dispatchers), len(rows)))
        return 2
    problems = compare(dispatchers, rows, read_baseline(a.check))
    for p in problems:
        sys.stdout.write(p + "\n")
    sys.stdout.write("%d dispatchers, %d calls enumerated, %d differences\n"
                     % (len(dispatchers), len(rows), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
