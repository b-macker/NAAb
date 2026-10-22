"""codemap store: merge per-file shards into one SQLite database, and read it.

Headers are parsed once per file that includes them, so the same symbol and
edge arrive many times; the unique indexes make the merge idempotent. A
symbol seen with a body anywhere wins over a declaration-only record.
"""

import json
import os
import re
import sqlite3

# Kinds whose definitions can collide across files on one USR (lambdas cannot:
# their USR carries file, line and column).
DEFINITION_KINDS = ("constructor", "conversion_function", "cxx_method", "destructor", "function_decl",
                    "function_template")

SCHEMA = """
CREATE TABLE symbols(usr TEXT PRIMARY KEY, name TEXT, kind TEXT, file TEXT, line INT,
                     end_line INT, is_def INT, sig TEXT, class_usr TEXT, virtual INT, psig TEXT);
CREATE TABLE edges(kind TEXT, src TEXT, dst TEXT, file TEXT, line INT, note TEXT);
CREATE UNIQUE INDEX edges_u ON edges(kind, src, dst, file, line);
CREATE INDEX edges_dst ON edges(dst, kind);
CREATE INDEX edges_src ON edges(src, kind);
CREATE TABLE classes(usr TEXT PRIMARY KEY, name TEXT, file TEXT, line INT, end_line INT, module TEXT);
CREATE TABLE bases(cls TEXT, base_usr TEXT, base_name TEXT, UNIQUE(cls, base_usr, base_name));
CREATE TABLE includes(src TEXT, dst TEXT, UNIQUE(src, dst));
CREATE TABLE tags(file TEXT, line INT, tag TEXT, text TEXT);
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE guarded(src TEXT, dst TEXT, file TEXT, line INT, literals TEXT, UNIQUE(src, dst, file, line));
CREATE INDEX symbols_name ON symbols(name);
"""


# The gates the VM and the tree-walker run before dispatching a NAAb stdlib
# call that the allowlist classifies (vm.cpp OP_CALL_METHOD, call_dispatch.cpp).
PREDISPATCH_GATES = ("checkFilesystemAllowed", "checkPathAccess")


# Roadmap item 6, from reading both dispatchers end to end (CLAUDE.md, "Pre-dispatch
# checks"). A MODULE gate is called by a dispatcher under `if (module == "m")`: it
# refuses every call of that module by policy, so it may stop `reach` -- added only
# for a module EVERY dispatcher gates that way.
MODULE_GATES = (("GovernanceEngine", "checkNetworkAllowed"),)
# A DETECTOR refuses only when something beyond the call's name holds: a configured
# sequence completes, an argument is tainted, a rate is exceeded, a pattern matches.
# Its [precheck] edge is context -- `reach` prints it and never stops at it, since
# treating it as a gate would report a sink as governed that no policy guards.
# (class, method, what it needs before it refuses)
DETECTORS = (("GovernanceEngine", "checkPreExecution", "a configured behavioural sequence completes"),
             ("GovernanceEngine", "checkTaintedValueAtSink", "an argument is tainted"),
             ("Interpreter", "checkExpressionTaintedSink", "an argument is tainted"),
             ("GovernanceEngine", "checkStdlibRate", "the stdlib call rate is exceeded"),
             ("GovernanceEngine", "checkFileOpsRate", "the file operation rate is exceeded"),
             ("GovernanceEngine", "checkDangerousCall", "a dangerous-call pattern matches the NAAb name"))


def _class_method(db, cls_name, method):
    """The one definition of Class::method (any namespace), or None."""
    hits = db.execute("SELECT s.usr FROM symbols s JOIN classes c ON c.usr=s.class_usr "
                      "WHERE (c.name=? OR c.name LIKE ?) AND s.name LIKE ? AND s.is_def=1",
                      (cls_name, "%::" + cls_name, "%::" + method)).fetchall()
    return hits[0][0] if len(hits) == 1 else None


def _reached_from(db, dispatcher):
    """The dispatcher and the helpers it calls that are defined in its own file
    (VM::callStdlibMethod, vm.cpp's bsdPreCheck): checks there run on its routes."""
    row = db.execute("SELECT file FROM symbols WHERE usr=?", (dispatcher,)).fetchone()
    if not row:
        return [dispatcher]
    helpers = [r[0] for r in db.execute(
        "SELECT DISTINCT e.dst FROM edges e JOIN symbols s ON s.usr=e.dst "
        "WHERE e.kind='call' AND e.src=? AND s.file=? AND s.is_def=1", (dispatcher, row[0]))]
    return [dispatcher] + [h for h in helpers if h != dispatcher]


_MARKER_LIKE = ("%__stdlib_module__:%", "%__stdlib_call__:%", "%__builtin__:%")


def _route_owners(db, callers):
    """Each allowlist caller, replaced by the same-file function that calls it on a
    stdlib route (a guard naming a stdlib marker), when there is one: VM::run
    calls VM::stdlibCallGates, which consults the allowlist, and the checks a
    route makes are VM::run's -- crediting them to the helper would lose the
    ones VM::run makes itself (the BSD pre-check, the rate limits)."""
    out = []
    for c in callers:
        row = db.execute("SELECT file FROM symbols WHERE usr=?", (c,)).fetchone()
        owners = [r[0] for r in db.execute(
            "SELECT DISTINCT g.src FROM guarded g JOIN symbols s ON s.usr=g.src "
            "WHERE g.dst=? AND s.file=? AND s.is_def=1 AND g.src<>? AND ("
            + " OR ".join("g.literals LIKE ?" for _ in _MARKER_LIKE) + ")",
            (c, row[0] if row else "", c) + _MARKER_LIKE)] if row else []
        for o in owners or [c]:
            if o not in out:
                out.append(o)
    return out


def apply_module_checks(db, callers, module_names):
    """[predispatch] module-gate edges and [precheck] detector edges (roadmap item 6)."""
    callers = _route_owners(db, callers)
    names_of = {}
    for (usr,) in db.execute("SELECT usr FROM symbols WHERE usr LIKE 'naab:%'").fetchall():
        names_of.setdefault(usr[5:].partition(".")[0], []).append(usr)
    report = {"module_gates": {}, "prechecks": {}}
    if not callers:
        report["status"] = "no dispatchers (nothing outside tests/ calls the allowlist)"
        return report
    for cls, g in MODULE_GATES:
        gu = _class_method(db, cls, g)
        if gu is None:
            report["module_gates"][g] = {"status": "unverified: %s::%s does not resolve to one definition" % (cls, g)}
            continue
        per = []
        for c in callers:
            mods = set()
            reach = _reached_from(db, c)
            for (lits,) in db.execute("SELECT literals FROM guarded WHERE dst=? AND src IN (%s)"
                                      % ",".join("?" * len(reach)), [gu] + reach):
                mods.update(x for x in lits.split("|") if x in module_names)
            per.append(mods)
        verified = sorted(set.intersection(*per)) if per else []
        missing = sorted(set.union(*per) - set(verified)) if per else []
        site = db.execute("SELECT file, line FROM guarded WHERE dst=? ORDER BY file, line", (gu,)).fetchone()
        n = 0
        for m in verified:
            for usr in names_of.get(m, []):
                db.execute("INSERT OR IGNORE INTO edges VALUES(?,?,?,?,?,?)",
                           ("predispatch", usr, gu, site[0], site[1], "module:" + m))
                n += 1
        report["module_gates"][g] = {"status": "applied" if verified else "unverified: no module every dispatcher gates",
                                     "modules": verified, "names_gated": n,
                                     "not_every_dispatcher": missing}
    for cls, meth, needs in DETECTORS:
        du = _class_method(db, cls, meth)
        if du is None:
            continue                      # not in this code base (a fixture has few)
        by_dispatcher = {}
        for c in callers:
            reach = _reached_from(db, c)
            hits = [r for r in db.execute(
                "SELECT src, file, line FROM edges WHERE kind='call' AND dst=? AND src IN (%s)"
                % ",".join("?" * len(reach)), [du] + reach)]
            if not hits:
                continue
            mods = set()
            unguarded = False
            for src, f, l in hits:
                g = db.execute("SELECT literals FROM guarded WHERE src=? AND dst=? AND file=? AND line=?",
                               (src, du, f, l)).fetchone()
                named = {x for x in (g[0].split("|") if g else []) if x in module_names}
                if named:
                    mods |= named
                else:
                    unguarded = True
            by_dispatcher[c] = (None if unguarded else mods, hits[0])
        if not by_dispatcher:
            continue
        targets = set()
        for mods, _ in by_dispatcher.values():
            targets |= set(names_of) if mods is None else mods
        dnames = sorted(db.execute("SELECT name FROM symbols WHERE usr=?", (c,)).fetchone()[0]
                        for c in by_dispatcher)
        f, l = next(iter(by_dispatcher.values()))[1][1:]
        n = 0
        for m in sorted(targets):
            for usr in names_of.get(m, []):
                db.execute("INSERT OR IGNORE INTO edges VALUES(?,?,?,?,?,?)",
                           ("precheck", usr, du, f, l, "%s; dispatchers: %s" % (needs, ", ".join(dnames))))
                n += 1
        report["prechecks"][meth] = {"needs": needs, "dispatchers": dnames, "names": n,
                                     "modules": "all" if any(m is None for m, _ in by_dispatcher.values())
                                     else sorted(targets)}
    return report


def apply_predispatch(db, pd):
    """Add [predispatch] edges NAAb name -> gate for every stdlib name the
    allowlist classifies -- but only after checking, from the call graph, that
    every non-test caller of the allowlist also calls each gate. A gate edge is
    the REASSURING direction (it makes reach stop), so an unverified one is
    worse than none: on any doubt nothing is added and the reason is recorded."""
    if not pd:
        return {"status": "absent: no definition of the allowlist was indexed"}
    if not pd.get("modules"):
        return {"status": "unrecognised: the allowlist's if/return shape was not read", "at": "%s:%s"
                % (pd.get("file"), pd.get("line"))}
    allow = pd["usr"]
    row = db.execute("SELECT class_usr FROM symbols WHERE usr=?", (allow,)).fetchone()
    cls = row[0] if row else ""
    gates = {}
    for g in PREDISPATCH_GATES:
        hit = db.execute("SELECT usr FROM symbols WHERE class_usr=? AND name LIKE ?", (cls, "%::" + g)).fetchall()
        if len(hit) != 1:
            return {"status": "unverified: gate %s resolves to %d symbols" % (g, len(hit))}
        gates[g] = hit[0][0]
    callers = [r[0] for r in db.execute(
        "SELECT DISTINCT e.src FROM edges e JOIN symbols s ON s.usr=e.src "
        "WHERE e.kind='call' AND e.dst=? AND s.file NOT LIKE 'tests/%'", (allow,)).fetchall()]
    if not callers:
        return {"status": "unverified: nothing outside tests/ calls the allowlist"}
    missing = []
    for c in callers:
        called = {r[0] for r in db.execute("SELECT dst FROM edges WHERE kind='call' AND src=?", (c,))}
        for g, gu in gates.items():
            if gu not in called:
                missing.append("%s does not call %s" % (c, g))
    if missing:
        return {"status": "unverified: " + "; ".join(missing)}
    gated = 0
    for (usr,) in db.execute("SELECT usr FROM symbols WHERE usr LIKE 'naab:%'").fetchall():
        mod, _, fn = usr[5:].partition(".")
        entry = pd["modules"].get(mod)
        if not entry:
            continue
        mode = entry["methods"].get(fn, entry.get("default", ""))
        if not mode:
            continue
        gated += 1
        for gu in gates.values():
            db.execute("INSERT OR IGNORE INTO edges VALUES(?,?,?,?,?,?)",
                       ("predispatch", usr, gu, pd["file"], pd["line"], mode))
    names = {r[0]: r[1] for r in db.execute("SELECT usr, name FROM symbols WHERE usr IN (%s)"
                                               % ",".join("?" * len(callers)), callers)}
    return {"status": "applied", "allowlist_at": "%s:%s" % (pd["file"], pd["line"]),
            "modules": sorted(pd["modules"]), "names_gated": gated,
            "callers_checked": sorted(names.get(c, c) for c in callers), "gates": list(PREDISPATCH_GATES)}


def build(db_path, shards, tags, meta):
    if os.path.exists(db_path):
        os.remove(db_path)
    db = sqlite3.connect(db_path)
    db.executescript(SCHEMA)
    modules = {}
    dispatch_ranges = {}
    symbols = {}
    def_files = {}                     # usr -> files defining it (callables)
    predispatch = None
    for sh in shards:
        pd = sh.get("predispatch")
        if pd and (predispatch is None or (pd.get("modules") and not predispatch.get("modules"))):
            predispatch = pd
        modules.update(sh.get("modules", {}))
        dispatch_ranges.update(sh.get("dispatch_ranges", {}))
        for s in sh["symbols"]:
            row = (s["usr"], s["name"], s["kind"], s["file"], s["line"], s.get("end_line", s["line"]),
                   s.get("is_def", 0), s.get("sig", ""), s.get("class_usr", ""), s.get("virtual", 0),
                   s.get("psig", ""))
            if row[6] and row[2] in DEFINITION_KINDS:
                def_files.setdefault(row[0], [])
                if row[3] not in def_files[row[0]]:
                    def_files[row[0]].append(row[3])
            cur = symbols.get(s["usr"])
            # A definition beats a declaration. The class and the virtual flag
            # (1 virtual, 2 pure) may be known only from the declaration.
            keep = row if cur is None or (row[6] and not cur[6]) else cur
            other = cur if keep is row else row
            if other is not None:
                keep = keep[:8] + (keep[8] or other[8], max(keep[9] or 0, other[9] or 0), keep[10] or other[10])
            symbols[s["usr"]] = keep
        db.executemany("INSERT OR IGNORE INTO edges VALUES(?,?,?,?,?,?)", [tuple(e) for e in sh["edges"]])
        for c in sh["classes"]:
            db.execute("INSERT OR IGNORE INTO classes VALUES(?,?,?,?,?,?)",
                       (c["usr"], c["name"], c["file"], c["line"], c["end_line"], ""))
            db.executemany("INSERT OR IGNORE INTO bases VALUES(?,?,?)",
                           [(c["usr"], b["usr"], b["name"]) for b in c["bases"]])
        db.executemany("INSERT OR IGNORE INTO includes VALUES(?,?)", [tuple(p) for p in sh["includes"]])
        db.executemany("INSERT OR IGNORE INTO guarded VALUES(?,?,?,?,?)",
                       [tuple(g) for g in sh.get("guarded", [])])
    db.executemany("INSERT INTO symbols VALUES(?,?,?,?,?,?,?,?,?,?,?)", list(symbols.values()))

    # Name the stdlib dispatch entries by their module ("naab:file.read"), now
    # that every module class's getName() has been seen.
    for cls_usr, module in modules.items():
        db.execute("UPDATE classes SET module=? WHERE usr=?", (module, cls_usr))
    for key, r in dispatch_ranges.items():
        module = modules.get(r["class_usr"])
        if not module:
            continue
        new = "naab:%s.%s" % (module, r["fn"])
        db.execute("UPDATE edges SET src=? WHERE src=? AND kind='dispatch'", (new, key))
        db.execute("INSERT OR REPLACE INTO symbols VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                   (new, "naab %s.%s()" % (module, r["fn"]), "naab_function", r["file"], r["line"],
                    r["end_line"], 1, "", r["class_usr"], 0, ""))
    # Executors registered by language name point at a class by its short name.
    for (dst,) in db.execute("SELECT DISTINCT dst FROM edges WHERE dst LIKE 'codemap:class:%'").fetchall():
        name = dst[len("codemap:class:"):]
        hit = (db.execute("SELECT usr FROM classes WHERE name=?", (name,)).fetchone()
               or db.execute("SELECT usr FROM classes WHERE name LIKE ?", ("%::" + name,)).fetchone())
        if hit:
            db.execute("UPDATE OR IGNORE edges SET dst=? WHERE dst=?", (hit[0], dst))
    meta["predispatch"] = apply_predispatch(db, predispatch)
    # Module gates and detectors, checked against the same dispatchers (callers of the
    # allowlist outside tests/): one definition of "a dispatcher" for every kind.
    dispatchers = []
    if predispatch:
        dispatchers = [r[0] for r in db.execute(
            "SELECT DISTINCT e.src FROM edges e JOIN symbols s ON s.usr=e.src "
            "WHERE e.kind='call' AND e.dst=? AND s.file NOT LIKE 'tests/%'", (predispatch["usr"],)).fetchall()]
    module_names = {r[0] for r in db.execute("SELECT module FROM classes WHERE module != ''")}
    meta["predispatch_modules"] = apply_module_checks(db, dispatchers, module_names)
    # A USR is the symbols key, so a function DEFINED in more than one file -- each
    # binary's main(int, char**), one helper per tool -- is stored once: the first
    # definition read is kept, the others are lost and their call edges merge into
    # it. Which definition a caller means depends on the link, which the index does
    # not know; recorded so `stats` names every such function instead of hiding it.
    meta["definition_collisions"] = sorted(
        ({"usr": u, "name": symbols[u][1], "kept": symbols[u][3], "files": sorted(fs)}
         for u, fs in def_files.items() if len(fs) > 1 and u in symbols),
        key=lambda c: (c["name"], c["usr"]))
    db.executemany("INSERT INTO tags VALUES(?,?,?,?)", tags)
    db.executemany("INSERT OR REPLACE INTO meta VALUES(?,?)", [(k, json.dumps(v)) for k, v in meta.items()])
    db.commit()
    db.close()


class Store:
    def __init__(self, db_path):
        if not os.path.exists(db_path):
            raise SystemExit("codemap: no index at %s -- run `codemap index` first" % db_path)
        self.db = sqlite3.connect(db_path)

    def meta(self):
        return {k: json.loads(v) for k, v in self.db.execute("SELECT key, value FROM meta")}

    def symbol(self, usr):
        r = self.db.execute("SELECT usr,name,kind,file,line,end_line,is_def,sig,class_usr,virtual,psig "
                            "FROM symbols WHERE usr=?", (usr,)).fetchone()
        if r is None:
            return None
        return dict(zip(("usr", "name", "kind", "file", "line", "end_line", "is_def", "sig",
                         "class_usr", "virtual", "psig"), r))

    def resolve(self, query, kinds=None):
        """Symbols matching a name: exact usr, exact qualified name, or `::`-suffix."""
        if self.symbol(query):
            return [self.symbol(query)]
        rows = self.db.execute("SELECT usr FROM symbols WHERE name=? OR name LIKE ?",
                               (query, "%::" + query)).fetchall()
        if not rows:
            rows = self.db.execute("SELECT usr FROM symbols WHERE name LIKE ? LIMIT 200",
                                   ("%" + query + "%",)).fetchall()
        syms = [self.symbol(r[0]) for r in rows]
        if kinds:
            syms = [s for s in syms if s["kind"] in kinds]
        # prefer definitions, then repo files over system headers
        syms.sort(key=lambda s: (not s["is_def"], s["file"].startswith("/"), s["name"]))
        return syms

    def edges_to(self, usr, kinds):
        q = "SELECT kind,src,dst,file,line,note FROM edges WHERE dst=? AND kind IN (%s) ORDER BY file,line" \
            % ",".join("?" * len(kinds))
        return self.db.execute(q, (usr, *kinds)).fetchall()

    def edges_from(self, usr, kinds):
        q = "SELECT kind,src,dst,file,line,note FROM edges WHERE src=? AND kind IN (%s) ORDER BY file,line" \
            % ",".join("?" * len(kinds))
        return self.db.execute(q, (usr, *kinds)).fetchall()

    def class_by_name(self, query):
        rows = self.db.execute("SELECT usr,name,file,line,end_line,module FROM classes WHERE name=? OR name LIKE ?",
                               (query, "%::" + query)).fetchall()
        return [dict(zip(("usr", "name", "file", "line", "end_line", "module"), r)) for r in rows]

    def class_row(self, usr):
        r = self.db.execute("SELECT usr,name,file,line,end_line,module FROM classes WHERE usr=?", (usr,)).fetchone()
        return dict(zip(("usr", "name", "file", "line", "end_line", "module"), r)) if r else None

    def bases_of(self, usr):
        return self.db.execute("SELECT base_usr, base_name FROM bases WHERE cls=?", (usr,)).fetchall()

    def subclasses_of(self, usr):
        return [r[0] for r in self.db.execute("SELECT cls FROM bases WHERE base_usr=?", (usr,)).fetchall()]

    def methods_of(self, cls_usr):
        return [self.symbol(r[0]) for r in self.db.execute(
            "SELECT usr FROM symbols WHERE class_usr=? ORDER BY line", (cls_usr,)).fetchall()]

    def includes(self, path, reverse=False):
        col, other = ("dst", "src") if reverse else ("src", "dst")
        return [r[0] for r in self.db.execute(
            "SELECT %s FROM includes WHERE %s=? ORDER BY 1" % (other, col), (path,)).fetchall()]

    def tags(self, kind=None, file_like=None):
        q, args = "SELECT file,line,tag,text FROM tags WHERE 1=1", []
        if kind:
            q += " AND tag LIKE ?"
            args.append(kind)
        if file_like:
            q += " AND file LIKE ?"
            args.append("%" + file_like + "%")
        return self.db.execute(q + " ORDER BY file,line", args).fetchall()

    def tags_in(self, file, start, end):
        return self.db.execute("SELECT file,line,tag,text FROM tags WHERE file=? AND line BETWEEN ? AND ?",
                               (file, start, end)).fetchall()

    def count(self, table, where="1=1", args=()):
        return self.db.execute("SELECT COUNT(*) FROM %s WHERE %s" % (table, where), args).fetchone()[0]


# --- task tags and bookmarks (no libclang needed) -----------------------------

TAG_RE = re.compile(r"\b(TODO|FIXME|HACK|XXX|BUG)\b(\([^)]*\))?:?\s*(.*)")
# References from code comments to rows of docs/open-investigations.md and
# other recorded findings, so a reader can jump from a comment to its record.
REF_RE = re.compile(r"\b(A\d{1,2}[a-z]?(?:\([a-z]\))?|CONTRA-\d{3}|V-[A-Z]+-\d{3}|EVA-\d{1,2}|F\d{1,3})\b")
# A comment that STARTS with a short id ("A4: Empty nonce", "[F3]") is labelling
# a test arm or a list item, not citing docs/open-investigations.md row A4.
LABEL_RE = re.compile(r"^\s*(?:\[\s*)?([A-Z][A-Z0-9-]*\d)\s*[]:.)]")
COMMENT_RE = re.compile(r"(//|/\*|^\s*\*|^\s*#)(.*)$")


def scan_tags(repo, files):
    out = []
    for rel in files:
        try:
            with open(os.path.join(repo, rel), encoding="utf-8", errors="replace") as fh:
                for n, line in enumerate(fh, 1):
                    m = COMMENT_RE.search(line)
                    if not m:
                        continue
                    text = m.group(2)
                    t = TAG_RE.search(text)
                    if t:
                        text_ = ((t.group(2) or "") + " " + t.group(3)).strip()
                        out.append((rel, n, t.group(1), text_[:160]))
                    label = LABEL_RE.match(text)
                    for r in sorted(set(REF_RE.findall(text))):
                        # Only the short ids collide with arm labels; CONTRA-013 is unambiguous.
                        if label and label.group(1) == r and re.match(r"^[AF]\d", r):
                            continue
                        out.append((rel, n, "ref:" + r, text.strip()[:160]))
        except OSError:
            continue
    return out
