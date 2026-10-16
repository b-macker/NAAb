"""codemap store: merge per-file shards into one SQLite database, and read it.

Headers are parsed once per file that includes them, so the same symbol and
edge arrive many times; the unique indexes make the merge idempotent. A
symbol seen with a body anywhere wins over a declaration-only record.
"""

import json
import os
import re
import sqlite3

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
CREATE INDEX symbols_name ON symbols(name);
"""


# The gates the VM and the tree-walker run before dispatching a NAAb stdlib
# call that the allowlist classifies (vm.cpp OP_CALL_METHOD, call_dispatch.cpp).
PREDISPATCH_GATES = ("checkFilesystemAllowed", "checkPathAccess")


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
