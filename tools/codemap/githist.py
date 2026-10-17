"""codemap history: the history pass for one function, in one command.

    codemap history GovernanceEngine::checkPathAccessImpl
    codemap history --range src/runtime/governance_engine.cpp:2591-2825

docs/investigation-method.md asks, before any change, for `git log -S` on every
name touched, `git log -L`/blame on the edited lines, the full messages and PR
threads, and the decision docs -- then for a classification (never built /
working / regressed / removed on purpose / lost / decided). This gathers the
evidence for that classification; it does not make it.

  lines    git log -L over the function's line range: every commit that changed
           those lines, following the range through earlier edits of the same
           file (not across files). The range comes from the index, which reads
           the WORKING TREE; it is mapped onto HEAD through `git diff` first, so
           an uncommitted edit above the function does not shift it.
  name     git log -S on the name, limited to the code and docs paths (the whole
           history of every file is minutes; `--all-paths` asks for it): commits
           that changed how often the name occurs, with lines added/removed that
           contain it as a whole word -- a commit that only removes is where a
           caller, a declaration or the function itself went away. Screened: a
           name is text, so another function of the same name counts too.
  blame    which commits wrote the lines as they are now, looking past moves and
           copies (-M -C). A commit blame names that -L did not list means -L
           lost the trail at a move or copy: those commits are reported as such.
  PRs      the (#N) references in those commits' subjects.
  records  every tracked .md line that names the function, with the record id of
           its table row (| A32 | ...) or the heading it sits under.

A shallow clone cuts the history: its oldest commit then looks like the one that
added every line. That is reported as UNMEASURABLE, never as an origin.
"""

import os
import re
import subprocess
from collections import OrderedDict

DEFAULT_PATHS = ("src", "include", "tests", "tools", "docs", "examples", "*.md")
# Names too common to search alone: the qualified Class::name is used instead.
COMMON_NAMES = {"run", "get", "set", "call", "init", "main", "visit", "size", "name", "type",
                "value", "data", "load", "read", "write", "open", "close", "start", "stop", "reset",
                "clear", "check", "parse", "execute", "apply", "update", "create", "find"}
RECORD_ROW = re.compile(r"^\|\s*([A-Z][A-Za-z]{0,7}-?\d+[a-z]?(?:\([a-z]\))?)\s*\|")
HEADING = re.compile(r"^(#{1,6})\s+(.*\S)")
PR_REF = re.compile(r"\(#(\d+)\)|(?:^|\s)#(\d{2,6})\b")


class HistoryError(Exception):
    """The history cannot be measured here (not a git checkout, file not committed)."""


def git(repo, *args):
    r = subprocess.run(["git", "-C", repo] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise HistoryError("git %s: %s" % (args[0], r.stderr.decode("utf-8", "replace").strip()[:300]))
    return r.stdout.decode("utf-8", "replace")


def repo_state(repo):
    """{shallow: bool, boundary: set of full SHAs where a shallow clone's history is cut}"""
    try:
        if git(repo, "rev-parse", "--is-inside-work-tree").strip() != "true":
            raise HistoryError("not a git work tree")
        shallow = git(repo, "rev-parse", "--is-shallow-repository").strip() == "true"
    except HistoryError as e:
        raise HistoryError("%s is not a git checkout (%s)" % (repo, e))
    boundary = set()
    if shallow:
        path = git(repo, "rev-parse", "--git-path", "shallow").strip()
        path = path if os.path.isabs(path) else os.path.join(repo, path)
        try:
            with open(path) as fh:
                boundary = {ln.strip() for ln in fh if ln.strip()}
        except OSError:
            pass
    return {"shallow": shallow, "boundary": boundary}


# --- working tree -> HEAD -----------------------------------------------------------

def diff_hunks(repo, path):
    """(old_start, old_len, new_start, new_len) for every uncommitted hunk of `path`."""
    out = git(repo, "diff", "-U0", "--no-color", "--no-ext-diff", "HEAD", "--", path)
    hunks = []
    for m in re.finditer(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", out, re.M):
        a, b, c, d = int(m.group(1)), int(m.group(2) or 1), int(m.group(3)), int(m.group(4) or 1)
        hunks.append((a, b, c, d))
    return hunks


def map_line(line, hunks):
    """A working-tree line -> its line at HEAD, or None when the line itself is uncommitted."""
    delta = 0
    for a, b, c, d in hunks:
        first_new = c if d > 0 else c + 1      # d == 0: a pure deletion AFTER new line c
        if line < first_new:
            return line + delta
        if d > 0 and line <= c + d - 1:
            return None
        delta += b - d
    return line + delta


def map_range(repo, path, start, end):
    """-> (head_start, head_end, uncommitted_line_count); (None, None, n) when every line
    of the range is uncommitted. Raises HistoryError when the file is not in HEAD."""
    try:
        git(repo, "cat-file", "-e", "HEAD:" + path)
    except HistoryError:
        raise HistoryError("%s is not committed at HEAD" % path)
    hunks = diff_hunks(repo, path)
    mapped = [map_line(n, hunks) for n in range(start, end + 1)]
    kept = [m for m in mapped if m is not None]
    if not kept:
        return None, None, len(mapped)
    return min(kept), max(kept), len(mapped) - len(kept)


# --- the three histories --------------------------------------------------------------

FMT = "%H%x1f%h%x1f%ad%x1f%s"


def _commit(line):
    full, short, date, subject = (line.split("\x1f") + ["", "", "", ""])[:4]
    return {"sha": full, "short": short, "date": date, "subject": subject}


def line_history(repo, path, start, end, limit=0):
    args = ["log", "-L", "%d,%d:%s" % (start, end, path), "--no-patch", "--date=short", "--format=" + FMT]
    if limit:
        args.insert(1, "-n%d" % limit)
    return [_commit(ln) for ln in git(repo, *args).splitlines() if "\x1f" in ln]


def word_re(name):
    return re.compile(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(name))


def name_history(repo, name, paths=DEFAULT_PATHS, limit=40):
    """Commits that changed how often `name` occurs, newest first, each with the lines
    added and removed that contain it as a whole word. Commits where it changed only
    inside a longer identifier are counted apart (`partial`)."""
    existing = [p for p in paths if "*" in p or os.path.exists(os.path.join(repo, p))]
    args = ["log", "-S" + name, "-p", "-U0", "--no-color", "--no-ext-diff", "--date=short",
            "--format=@@@" + FMT]
    if limit:
        args.insert(1, "-n%d" % limit)
    if existing:
        args += ["--"] + list(existing)
    w = word_re(name)
    commits, cur = [], None
    for ln in git(repo, *args).splitlines():
        if ln.startswith("@@@"):
            cur = _commit(ln[3:])
            cur.update(added=0, removed=0)
            commits.append(cur)
        elif cur is not None and ln[:1] in "+-" and not ln.startswith(("+++", "---")) and w.search(ln):
            cur["added" if ln[0] == "+" else "removed"] += 1
    whole = [c for c in commits if c["added"] or c["removed"]]
    return whole, len(commits) - len(whole), existing


def files_at_head(repo, name, paths):
    """How many tracked files at HEAD (within `paths`) contain `name` as a whole word.
    The positive control for name_history: a name present at HEAD was added by SOME
    commit, so with full history an empty pickaxe means the probe, not the code, is
    wrong (the same control as .claude/skills/investigation-gates/history_pass.sh)."""
    args = ["grep", "-l", "-I", "-w", "-F", "-e", name, "HEAD", "--"] + (list(paths) or ["."])
    try:
        out = git(repo, *args)
    except HistoryError:
        return 0                            # git grep exits 1 when nothing matches
    return len([ln for ln in out.splitlines() if ln])


def blame_summary(repo, path, start, end):
    """Which commits wrote lines start..end of `path` as committed at HEAD. Move- and
    copy-aware (-M -C, git's default thresholds): `git log -L` stops at a commit that
    MOVED the lines, which then looks like where they began; blame looks past it."""
    out = git(repo, "blame", "--line-porcelain", "-M", "-C", "-L", "%d,%d" % (start, end), "HEAD", "--", path)
    by, info, cur = OrderedDict(), {}, None
    for ln in out.splitlines():
        m = re.match(r"^([0-9a-f]{40}) \d+ \d+", ln)
        if m:
            cur = m.group(1)
            by[cur] = by.get(cur, 0) + 1
            info.setdefault(cur, {"sha": cur, "short": cur[:8], "date": "", "subject": "", "boundary": False,
                                  "files": []})
        elif cur and ln.startswith("summary "):
            info[cur]["subject"] = ln[8:]
        elif cur and ln.startswith("author-time "):
            import time
            info[cur]["date"] = time.strftime("%Y-%m-%d", time.gmtime(int(ln.split()[1])))
        elif cur and ln.startswith("filename ") and ln[9:] not in info[cur]["files"]:
            info[cur]["files"].append(ln[9:])     # the path the line had in that commit
        elif cur and ln == "boundary":
            info[cur]["boundary"] = True      # the root commit, or a shallow cut
    rows = [dict(info[s], lines=n) for s, n in by.items()]
    rows.sort(key=lambda r: -r["lines"])
    return rows


def pr_numbers(commits):
    seen = OrderedDict()
    for c in commits:
        for m in PR_REF.finditer(c.get("subject", "")):
            seen[int(m.group(1) or m.group(2))] = True
    return list(seen)


# --- decision records ------------------------------------------------------------------

def records(repo, names, max_hits=60):
    """Tracked .md lines naming any of `names` (whole word), each with its record id or
    heading. A name with more than max_hits mentions is dropped as too common and
    reported, so a short name cannot bury the qualified ones."""
    hits, dropped = OrderedDict(), []
    for name in names:
        try:
            out = git(repo, "grep", "-n", "-I", "-w", "--no-color", "-F", "-e", name, "--", "*.md")
        except HistoryError:
            out = ""                       # git grep exits 1 when nothing matches
        rows = [ln for ln in out.splitlines() if ln]
        if len(rows) > max_hits:
            dropped.append((name, len(rows)))
            continue
        for ln in rows:
            path, num, text = ln.split(":", 2)
            hits.setdefault((path, int(num)), text)
    headings = {}
    result = []
    for (path, num), text in hits.items():
        m = RECORD_ROW.match(text)
        where = "[%s]" % m.group(1) if m else ""
        if not where:
            if path not in headings:
                try:
                    with open(os.path.join(repo, path), encoding="utf-8", errors="replace") as fh:
                        headings[path] = [(i + 1, h.group(2)) for i, l in enumerate(fh)
                                          for h in [HEADING.match(l)] if h]
                except OSError:
                    headings[path] = []
            above = [h for i, h in headings[path] if i <= num]
            where = "(%s)" % above[-1][:60] if above else ""
        result.append({"file": path, "line": num, "where": where, "text": text.strip()[:160]})
    return result, dropped


def search_names(qualified):
    """The names to search for a qualified C++ name: the bare name, unless it is too
    common to mean anything alone, and Class::name."""
    parts = [p for p in qualified.split("::") if p]
    short = parts[-1] if parts else qualified
    names = []
    if short not in COMMON_NAMES and len(short) > 3:
        names.append(short)
    if len(parts) >= 2:
        names.append("%s::%s" % (parts[-2], short))
    return names or [short]
