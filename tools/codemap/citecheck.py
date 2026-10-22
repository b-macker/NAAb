"""cite-check: do a doc's `path:line` citations still point where they say?

NAAb's docs cite `file.cpp:123` (and "`file.h`, ~line 123") beside the names of
what they mean, and the numbers drift with every edit. The method is borrowed
from alibaba/open-code-review's comment resolver: locate by the quoted text,
never trust the number, and decline when zero or several places match.

For each citation, the backticked identifiers in the same sentence (prose) or
table cell are looked for in the cited file at HEAD (the working tree):

  HOLDS        one of them is within WINDOW lines of the cited line, and it occurs
               at most COMMON_IN_FILE times in the file (a commoner name places
               nothing: such a citation reads AMBIGUOUS and says why)
  MOVED        none is there, but one occurs exactly once in the file (the new line
               is printed); two that each occur once on different lines -> AMBIGUOUS
  GONE         none of them occurs in the file
  AMBIGUOUS    they occur, but only in several places each
  FILE GONE    no tracked file has that path (a bare name matches by its last
               components; several matches are narrowed to the files that contain
               an identifier, and stay AMBIGUOUS if more than one does)
  UNCHECKABLE  no identifier beside the citation, so nothing can be checked

Screened tier. HOLDS is the reassuring answer and the weakest: a name near a
line says nothing about whether the sentence around it is still true. GONE and
MOVED can be wrong the other way when a sentence names something that lives in
ANOTHER file than the one it cites. It never edits a doc.
"""

import os
import re
import subprocess

OUTCOMES = ("HOLDS", "MOVED", "GONE", "AMBIGUOUS", "FILE GONE", "UNCHECKABLE")
COMMON_IN_FILE = 10      # a name found more often than this in the file places no line
EXTS = ("cpp", "cc", "cxx", "c", "h", "hpp", "py", "sh", "md", "json", "naab", "yml", "yaml",
        "txt", "toml", "js", "ts", "go", "rs", "java", "cs", "rb", "php", "cmake")
_EXT = "|".join(sorted(EXTS, key=len, reverse=True))
# path:LINE or path:LINE-LINE; never inside a URL or a longer path (lookbehind)
CITE = re.compile(r"(?<![\w/.:-])((?:[\w.-]+/)*[\w.-]+\.(?:%s)):(\d+)(?:-(\d+))?(?![\w])" % _EXT)
# `path` ... ~line N  (the "in `governance.h`, ~line 3024" form)
TILDE = re.compile(r"`((?:[\w.-]+/)*[\w.-]+\.(?:%s))`[^`\n]{0,40}?~\s?line\s+(\d+)" % _EXT)
# `:194` -- a further line of the path cited just before it in the same unit
CONT = re.compile(r"`:(\d+)`")
SPAN = re.compile(r"`([^`\n]+)`")
TOKEN = re.compile(r"[A-Za-z_]\w*(?:::[A-Za-z_~]\w*)*")
LIST_ITEM = re.compile(r"^\s*(?:[-*+]|\d+[.)])\s+")
SENTENCE_END = re.compile(r"(?<=[.!?])\s+(?=[A-Z`*(\[])")
ABBREV = ("e.g.", "i.e.", "vs.", "cf.")


def tracked_files(repo):
    try:
        r = subprocess.run(["git", "-C", repo, "ls-files"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if r.returncode == 0:
            return [f for f in r.stdout.decode("utf-8", "replace").splitlines() if f]
    except OSError:
        pass
    files = []
    for root, dirs, names in os.walk(repo):
        dirs[:] = [d for d in dirs if d not in (".git", ".cache", "build", "build-cov")]
        files += [os.path.relpath(os.path.join(root, n), repo).replace(os.sep, "/") for n in names]
    return files


def sentences(body):
    """(offset, sentence) pieces of one paragraph or table cell."""
    pieces, start = [], 0
    for m in SENTENCE_END.finditer(body):
        if body[:m.start()].endswith(ABBREV):
            continue
        pieces.append((start, body[start:m.start()]))
        start = m.end()
    pieces.append((start, body[start:]))
    return pieces


def cells(row):
    """A table row's cells, split on pipes OUTSIDE code spans: a cell quoting
    `secrets|PII` is one cell, and splitting it would unpair its backticks."""
    found, cur, code, i = [], [], False, 0
    while i < len(row):
        ch = row[i]
        if ch == "\\" and i + 1 < len(row):
            cur.append(row[i:i + 2]); i += 2; continue
        if ch == "`":
            code = not code
        if ch == "|" and not code:
            found.append("".join(cur)); cur = []
        else:
            cur.append(ch)
        i += 1
    found.append("".join(cur))
    return found


def units(text):
    """(first line, text, group) for every sentence of prose and of every table
    cell; group numbers the paragraph or cell, across which a `:N` continuation
    still refers to the path named before it.
    Code blocks are skipped: a citation in an example is not a claim about the tree."""
    out, para, para_line, fence, group = [], [], 0, False, [0]

    def flush():
        if not para:
            return
        body = "\n".join(para)
        group[0] += 1
        for off, s in sentences(body):
            out.append((para_line + body[:off].count("\n"), s, group[0]))
        del para[:]

    for i, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("```"):
            flush()
            fence = not fence
            continue
        if fence:
            continue
        if line.lstrip().startswith("|"):
            flush()
            for cell in cells(line):
                if cell.strip():
                    group[0] += 1
                    out += [(i, sent, group[0]) for _, sent in sentences(cell)]
            continue
        if not line.strip() or line.lstrip().startswith("#") or LIST_ITEM.match(line):
            flush()
            if not line.strip() or line.lstrip().startswith("#"):
                continue
        if not para:
            para_line = i
        para.append(line)
    flush()
    return out


def identifiers(unit_text, lo=0, hi=None):
    """Backticked names that look like code: with an underscore, `::`, an inner
    capital, or a call's parentheses -- and a multi-word span such as
    `class GovernanceHardError`, kept whole as a phrase, which pins one place where
    its single words occur in dozens. Paths and citations are not names."""
    ids = []
    hi = len(unit_text) if hi is None else hi
    for sm in SPAN.finditer(unit_text):
        # spans are paired over the WHOLE unit and then picked by position: a slice
        # can cut a pair and read the prose between two spans as code
        if not lo <= sm.start() < hi:
            continue
        span = sm.group(1)
        if CITE.search(span) or CONT.fullmatch("`%s`" % span):
            continue
        if re.fullmatch(r"[\w./-]+\.(?:%s)" % _EXT, span):
            # a file named in the sentence anchors a citation of ANOTHER file (a suite's
            # registration line in run-all-tests.sh); judge() drops the cited file's own
            base = span.rsplit("/", 1)[-1]
            if base not in ids:
                ids.append(base)
            continue
        if "/" in span and re.fullmatch(r"[\w./-]+", span):
            continue                    # a directory: its parts are not names
        phrase = span.strip()
        if " " in phrase and len(phrase) >= 8 and re.match(r"\w", phrase) and re.search(r"\w$", phrase):
            if phrase not in ids:
                ids.append(phrase)
        for m in TOKEN.finditer(span):
            tok = m.group(0)
            last = tok.split("::")[-1]
            call = span[m.end():m.end() + 1] == "("
            if len(last) < 4 and "::" not in tok:
                continue
            if "_" in tok or "::" in tok or re.search(r"[a-z][A-Z]", tok) or call:
                if tok not in ids:
                    ids.append(tok)
    return ids


BARE_PATH = re.compile(r"`((?:[\w.-]+/)*[\w.-]+\.(?:%s))`" % _EXT)


def citations(unit_text, last_path=None):
    """(offset, end, path, first line, last line) cited in one unit, in order, and
    the path a following `:N` would refer to. A backticked file name with no line
    (`main.cpp` (`:1475`)) names that path too."""
    found = []
    events = []
    for m in BARE_PATH.finditer(unit_text):
        events.append((m.start(), m.end(), m.group(1), None, None))
    for m in CITE.finditer(unit_text):
        events.append((m.start(), m.end(), m.group(1), int(m.group(2)), int(m.group(3) or m.group(2))))
    for m in TILDE.finditer(unit_text):
        events.append((m.start(), m.end(), m.group(1), int(m.group(2)), int(m.group(2))))
    for m in CONT.finditer(unit_text):
        events.append((m.start(), m.end(), None, int(m.group(1)), int(m.group(1))))
    for pos, end, path, lo, hi in sorted(events, key=lambda e: e[0]):
        if lo is None:
            last_path = path                 # a file named without a line
            continue
        if path is None:
            if last_path is None:
                found.append((pos, end, None, lo, hi))    # reported, never dropped
                continue
            path = last_path
        last_path = path
        found.append((pos, end, path, lo, hi))
    return found, last_path


def _inherited(unit_text, cite):
    """A `:N` whose file was named in an earlier sentence, not in its own."""
    return unit_text[cite[0]:cite[1]].startswith("`:")


def segment_identifiers(unit_text, cites, k):
    """The names a citation is judged by: those between the citation before it and
    the one after it, so one rare name in a long table cell does not decide every
    citation in the cell; the whole unit's names when its segment has none."""
    lo = cites[k - 1][1] if k else 0
    hi = cites[k + 1][0] if k + 1 < len(cites) else len(unit_text)
    seg = identifiers(unit_text, lo, hi)
    return (seg, "segment") if seg else (identifiers(unit_text), "unit")


def _lines_of(repo, path, cache):
    if path not in cache:
        try:
            with open(os.path.join(repo, path), encoding="utf-8", errors="replace") as fh:
                cache[path] = fh.read().splitlines()
        except OSError:
            cache[path] = None
    return cache[path]


def _occurrences(lines, ident):
    """Lines holding the identifier as a whole word: the qualified form first, then
    its last component when the qualified one is nowhere (a .h declares `name(`,
    the .cpp defines `Class::name(`)."""
    forms = [ident] + ([ident.split("::")[-1]] if "::" in ident and " " not in ident else [])
    for form in forms:
        w = re.compile(r"(?<![\w])%s(?![\w])" % re.escape(form))
        hits = [i for i, l in enumerate(lines, 1) if w.search(l)]
        if hits:
            return hits
    return []


def judge(repo, files, cache, path, lo, hi, ids, window):
    ids = [i for i in ids if i not in (path, path.rsplit("/", 1)[-1])]
    cands = [f for f in files if f == path or f.endswith("/" + path)]
    if not cands:
        return {"outcome": "FILE GONE", "file": None}
    if not ids:
        return {"outcome": "UNCHECKABLE", "file": cands[0] if len(cands) == 1 else None}
    if len(cands) > 1:
        having = [f for f in cands if any(_occurrences(_lines_of(repo, f, cache) or [], i) for i in ids)]
        if len(having) != 1:
            return {"outcome": "AMBIGUOUS" if having else "GONE", "file": None,
                    "note": "%d tracked files end in %s, %d of them name one of these" % (len(cands), path, len(having))}
        cands = having
    f = cands[0]
    lines = _lines_of(repo, f, cache) or []
    occ = {i: _occurrences(lines, i) for i in ids}
    if not any(occ.values()):
        return {"outcome": "GONE", "file": f}
    # Only a name that is rare in the file can place a line: `code_quality` (137 times
    # in governance_checks.cpp) sat beside a stale citation and read HOLDS.
    specific = {i: h for i, h in occ.items() if 0 < len(h) <= COMMON_IN_FILE}
    if not specific:
        return {"outcome": "AMBIGUOUS", "file": f,
                "note": "only names too common in the file to place a line: " +
                        ", ".join("%s (%d)" % (i, len(h)) for i, h in occ.items() if h)}
    near = [i for i, hits in specific.items() if any(lo - window <= h <= hi + window for h in hits)]
    if near:
        return {"outcome": "HOLDS", "file": f, "by": near[0]}
    occ = specific
    once = {hits[0] for hits in occ.values() if len(hits) == 1}
    if len(once) == 1:
        new = once.pop()
        return {"outcome": "MOVED", "file": f, "new_line": new,
                "by": next(i for i, h in occ.items() if h == [new])}
    return {"outcome": "AMBIGUOUS", "file": f,
            "note": "; ".join("%s at %s" % (i, ",".join(map(str, h[:4])) + ("..." if len(h) > 4 else ""))
                              for i, h in occ.items() if h)}


def check(repo, doc, window=5, files=None):
    files = files if files is not None else tracked_files(repo)
    cache = {}
    with open(os.path.join(repo, doc) if not os.path.isabs(doc) else doc, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    rows = []
    last_path, last_group = None, None
    for line, unit, group in units(text):
        if group != last_group:
            last_path, last_group = None, group
        cites, last_path = citations(unit, last_path)
        for k, (pos, _, path, lo, hi) in enumerate(cites):
            ids, scope = segment_identifiers(unit, cites, k)
            if path is None:
                r = {"outcome": "UNCHECKABLE", "file": None, "note": "a `:N` with no file named before it"}
            else:
                r = judge(repo, files, cache, path, lo, hi, ids, window)
            r.update(doc=doc, doc_line=line + unit[:pos].count("\n"), cited=path or "?", line=lo, end_line=hi,
                     identifiers=ids, scope=scope,
                     file_from="continuation" if _inherited(unit, cites[k]) else "citation")
            rows.append(r)
    return rows
