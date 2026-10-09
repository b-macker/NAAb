#!/usr/bin/env python3
"""Find `PRODUCER | grep -q` pipelines in shell scripts that run under pipefail.

Under `set -o pipefail` a pipeline's status is the last non-zero status in it,
so a pipeline whose last stage is `grep -q` (or --quiet/--silent) does not
report "the pattern was found":

  * when the producer exits non-zero, the pipeline fails even though grep
    matched -- deterministic. `naab-lang prog | grep -q blocked` is false for
    every program that exits 3 while printing "blocked", and
    `! naab-lang prog | grep -q SECRET` then PASSES whether or not the secret
    was printed;
  * when the producer writes again after grep has matched and exited, it takes
    SIGPIPE and the pipeline reports 141 -- a timing-dependent false result.

Measured (bash 5, Linux): `(printf 'match\\n'; exit 3) | grep -q match` -> 3;
`(printf 'match\\n'; sleep .2; printf 'more\\n') | grep -q match` -> 141.
`echo "$VAR" | grep -q` gave 0 false results in 2,000 trials at 5 lines and
300 at 40 KB: echo writes its argument in one go, so it can only take SIGPIPE
once the text outgrows the pipe (64 KB). It is reported too -- the fix is
free and the size limit is not something a test author checks.

The fix is to stop piping: `grep -q PAT <<<"$OUTPUT"`, where OUTPUT was
captured with `OUTPUT=$(cmd 2>&1)`. A here-string is written by the shell
before grep starts, so there is no producer to fail or take SIGPIPE.

  pipefail_grep.py scan PATH... [--exclude FILE]...
                                    report sites, exit 1 if any
  pipefail_grep.py fix PATH...      rewrite the echo/printf sites in place
                                    (`echo "X" | grep` -> `grep <<<"X"`) and
                                    report the rest, which need review
  pipefail_grep.py scan-stdin [--assume-pipefail]
  pipefail_grep.py fix-stdin  [--assume-pipefail]
                                    the same for one script on stdin (the
                                    fixed text goes to stdout)
  pipefail_grep.py --selftest       planted cases, each must be decided right

Output is ASCII and written as bytes (CLAUDE.md: a test's output channel).
"""
import os
import re
import sys

PIPEFAIL = re.compile(r"(^|[;&|\s])set\s+[^#\n]*-[A-Za-z]*o\s+pipefail\b"
                      r"|(^|[;&|\s])set\s+[^#\n]*-o\s+pipefail\b")
HEREDOC = re.compile(r"(?<!<)<<(-?)\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\2")
GREP_WORD = re.compile(r"(e|f)?grep\b")
SOURCE = re.compile(r"(^|[;&|{(]\s*|\s)(?:source|\.)\s+(\"[^\"]+\"|'[^']+'|\S+)", re.M)
# echo "WORD" | grep   /   printf '%s\n' "WORD" | grep
ECHO_SITE = re.compile(
    r"\b(?:echo|printf\s+(?:'%s\\n'|\"%s\\n\"|'%s'|\"%s\"))\s+"
    r"(\"(?:[^\"\\]|\\.)*\")\s*\|\s*(?=(?:e|f)?grep\b)")


def _unquoted_positions(line, state=None):
    """Yield (index, char) for shell code outside quotes, stopping at a comment.

    `state` is the quoting stack carried from the previous line and updated in
    place, so a quoted string that spans lines (`python3 -c "..."`) is not
    read as shell. Command substitution inside double quotes starts a fresh
    quoting context -- `"$(grep '"a"' f)"` is balanced -- which a flat
    in-single/in-double flag gets wrong, and once carried across lines that
    error hides every site after it. Characters inside $( ) are code and are
    yielded.

    Stack entries: ["c", depth] code (top level or inside $( ), with paren
    depth), ["d"] double quotes, ["s"] single, ["a"] $'...', ["p"] ${...},
    ["b"] backticks.
    """
    if state is None:
        state = []
    if not state:
        state.append(["c", 0])
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        nxt = line[i + 1] if i + 1 < n else ""
        top = state[-1]
        kind = top[0]
        if kind == "s":
            if c == "'":
                state.pop()
        elif kind == "a":
            if c == "\\":
                i += 2
                continue
            if c == "'":
                state.pop()
        elif kind in ("d", "p", "b"):
            if c == "\\":
                i += 2
                continue
            if (kind == "d" and c == '"') or (kind == "p" and c == "}") \
                    or (kind == "b" and c == "`"):
                state.pop()
            elif c == "$" and nxt == "(":
                state.append(["c", 0])
                i += 2
                continue
            elif c == "$" and nxt == "{":
                state.append(["p"])
                i += 2
                continue
            elif kind != "d" and c == '"':
                state.append(["d"])
            elif kind != "d" and c == "'":
                state.append(["s"])
            elif kind == "d" and c == "`":
                state.append(["b"])
        else:  # code
            if c == "\\":
                i += 2
                continue
            if c == "'":
                state.append(["a"] if i > 0 and line[i - 1] == "$" else ["s"])
            elif c == '"':
                state.append(["d"])
            elif c == "`":
                state.append(["b"])
            elif c == "#" and (i == 0 or line[i - 1] in " \t;"):
                return
            elif c == "$" and nxt == "(":
                state.append(["c", 0])
                i += 2
                continue
            elif c == "$" and nxt == "{":
                state.append(["p"])
                i += 2
                continue
            elif c == ")" and top[1] == 0 and len(state) > 1:
                state.pop()
            else:
                if c == "(":
                    top[1] += 1
                elif c == ")" and top[1] > 0:
                    top[1] -= 1
                yield i, c
        i += 1


def _early_exit_grep(rest):
    """rest starts just after the pipe. True if it is grep with -q/--quiet/--silent."""
    rest = rest.lstrip()
    m = GREP_WORD.match(rest)
    if not m:
        return False
    toks = rest[m.end():].split()
    for t in toks:
        if not t.startswith("-") or t == "-":
            return False
        if t in ("--quiet", "--silent"):
            return True
        if t == "--":
            return False
        if t.startswith("--"):
            continue
        if "q" in t[1:]:
            return True
        # options that take a separate argument: the next token is not an option
        if t[-1] in "efmABCd" and len(t) == 2:
            return False
    return False


def scan_text(text):
    """Return (pipefail_enabled, [(lineno, kind, line)])."""
    lines = text.replace("\r\n", "\n").split("\n")
    pending = []          # heredoc terminators to skip past
    code = []             # (lineno, line, quote state at start, unquoted chars)
    quote = []
    for no, line in enumerate(lines, 1):
        if pending:
            term, strip = pending[0]
            if (line.strip() if strip else line) == term:
                pending.pop(0)
            continue
        uq = list(_unquoted_positions(line, quote))
        code.append((no, line, None, uq))
        uqi = {i for i, _ in uq}
        for m in HEREDOC.finditer(line):
            # only heredocs outside quotes start a body
            if m.start() in uqi:
                pending.append((m.group(3), m.group(1) == "-"))
    enabled = False
    for _, line, _, uq in code:
        uqi = {i for i, _ in uq}
        for m in PIPEFAIL.finditer(line):
            if line.find("set", m.start()) in uqi:
                enabled = True
    sites = []
    open_pipe = False     # previous code line ended in a pipe
    for no, line, _, uq in code:
        if open_pipe and line.strip() and _early_exit_grep(line):
            sites.append((no, "continued", line))
        if line.strip() and not line.lstrip().startswith("#"):
            last = [i for i, c in uq if not c.isspace()]
            open_pipe = bool(last) and line[last[-1]] == "|" and \
                not (last[-1] > 0 and line[last[-1] - 1] == "|") and \
                not re.search(r":\s*$", line[:last[-1]])   # YAML `run: |`
        for i, c in uq:
            if c != "|":
                continue
            if i + 1 < len(line) and line[i + 1] == "|":
                continue
            if i > 0 and line[i - 1] == "|":
                continue
            rest = line[i + 1:]
            if rest.startswith("&"):          # |& pipes stderr too
                rest = rest[1:]
            if not _early_exit_grep(rest):
                continue
            before = line[:i]
            if before.strip() == "":
                kind = "continued"      # producer on the line(s) above
            elif ECHO_SITE.search(line[:i + 1] + line[i + 1:]) and \
                    re.search(r"(?:echo|printf[^|]*)\s+\"(?:[^\"\\]|\\.)*\"\s*$",
                              before):
                kind = "echo"
            else:
                kind = "command"
            sites.append((no, kind, line))
    return enabled, sites


def fix_text(text, force=False):
    """Rewrite echo/printf sites; return (new_text, count)."""
    enabled, sites = scan_text(text)
    if not (enabled or force):
        return text, 0
    lines = text.split("\n")
    count = 0
    for no, kind, _ in sites:
        if kind != "echo":
            continue
        line = lines[no - 1]
        # the echo whose pipe feeds an early-exit grep -- not an earlier
        # `echo "$x" | grep -c` on the same line
        m = next((m for m in ECHO_SITE.finditer(line)
                  if _early_exit_grep(line[m.end():])), None)
        if not m:
            continue
        word = m.group(1)
        g = GREP_WORD.match(line, m.end())
        new = line[:m.start()] + line[m.end():g.end()] + " <<<" + word + line[g.end():]
        lines[no - 1] = new
        count += 1
    return "\n".join(lines), count


def _is_workflow(path):
    parts = os.path.normpath(path).split(os.sep)
    return (path.endswith((".yml", ".yaml")) and len(parts) >= 3
            and parts[-3:-1] == [".github", "workflows"])


def iter_files(paths):
    for p in paths:
        if os.path.isfile(p):
            yield p
            continue
        for dp, dn, fn in os.walk(p):
            dn[:] = sorted(d for d in dn if d not in (".git", "build", "node_modules")
                           and not d.startswith("build"))
            for f in sorted(fn):
                full = os.path.join(dp, f)
                if f.endswith(".sh") or _is_workflow(full):
                    yield full


def sourced_names(text):
    """Basenames of the files a script sources (`source X` / `. X`)."""
    names = set()
    for m in SOURCE.finditer(text):
        arg = m.group(2).strip("\"'")
        base = re.split(r"[/\"']", arg)[-1]
        if base:
            names.add(base)
    return names


def pipefail_files(paths):
    """Map path -> why pipefail applies to it ('own', 'sourced by X', 'workflow').

    A function defined in a helper runs under the CALLER's shell options, so a
    helper sourced by a pipefail script is a pipefail script for every
    pipeline it runs. Workflow `run:` steps are counted too: GitHub runs a
    `shell: bash` step as `bash -eo pipefail`.
    """
    files = list(iter_files(paths))
    texts = {f: read(f) for f in files}
    why = {}
    inherited = {}
    for f, t in texts.items():
        if _is_workflow(f):
            why[f] = "workflow"
        elif scan_text(t)[0]:
            why[f] = "own"
            for name in sourced_names(t):
                inherited.setdefault(name, f)
    for f in files:
        if f not in why and os.path.basename(f) in inherited:
            why[f] = "sourced by " + inherited[os.path.basename(f)]
    return files, texts, why


def out(s):
    sys.stdout.buffer.write((s + "\n").encode("ascii", "backslashreplace"))


def read(path):
    with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(text)


def cmd_scan(paths, fix=False):
    total = fixed = 0
    exclude = set()
    while "--exclude" in paths:
        k = paths.index("--exclude")
        exclude.add(os.path.normpath(paths[k + 1]))
        del paths[k:k + 2]
    files, texts, why = pipefail_files(paths)
    for path in files:
        if path not in why or os.path.normpath(path) in exclude:
            continue
        text = texts[path]
        if fix:
            text2, n = fix_text(text, force=True)
            if n:
                write(path, text2)
                fixed += n
                text = text2
        _, sites = scan_text(text)
        for no, kind, line in sites:
            total += 1
            tag = "" if why[path] == "own" else " (%s)" % why[path]
            out("%s:%d: [%s]%s %s" % (path, no, kind, tag, line.strip()[:160]))
    if fix:
        out("rewrote %d echo/printf site(s)" % fixed)
    out("%d site(s) left" % total)
    return 1 if total else 0


SELFTEST = [
    # (name, script, expected kinds in order, expected fixed text or None)
    ("command producer", "set -o pipefail\nif cmd | grep -q x; then :; fi\n",
     ["command"], None),
    ("negated command", "set -euo pipefail\nif ! \"$NAAB\" p 2>&1 | grep -qi s; then :; fi\n",
     ["command"], None),
    ("quiet after other flags", "set -uo pipefail\na | grep -E -q x && b\n",
     ["command"], None),
    ("long option", "set -o pipefail\na | grep --quiet x\n", ["command"], None),
    ("egrep", "set -o pipefail\na | egrep -q x\n", ["command"], None),
    ("two echo pipes, the second is the site",
     "set -o pipefail\nx=\"$(echo \"$o\" | grep -c a)|$(echo \"$o\" | grep -q b && echo Y)\"\n",
     ["echo"],
     "set -o pipefail\nx=\"$(echo \"$o\" | grep -c a)|$(grep <<<\"$o\" -q b && echo Y)\"\n"),
    ("echo of a variable", "set -o pipefail\nif echo \"$OUT\" | grep -qi \"a|b\"; then :; fi\n",
     ["echo"], "set -o pipefail\nif grep <<<\"$OUT\" -qi \"a|b\"; then :; fi\n"),
    ("printf of a variable", "set -o pipefail\nprintf '%s\\n' \"$O\" | grep -q x || f\n",
     ["echo"], "set -o pipefail\ngrep <<<\"$O\" -q x || f\n"),
    ("continued producer", "set -o pipefail\nif cat f \\\n  | grep -q x; then :; fi\n",
     ["continued"], None),
    ("pipe ending the line", "set -o pipefail\nif cat f |\n  grep -q x; then :; fi\n",
     ["continued"], None),
    ("pipe ending the line, comment between",
     "set -o pipefail\ncat f |\n  # why\n  grep -q x\n", ["continued"], None),
    ("|& pipes stderr too", "set -o pipefail\nif cmd |& grep -q x; then :; fi\n",
     ["command"], None),
    # negatives
    ("here-string form", "set -o pipefail\nif grep -q x <<<\"$o\"; then :; fi\n", [], None),
    ("no pipefail", "set -u\nif cmd | grep -q x; then :; fi\n", [], None),
    ("or-list, not a pipe", "set -o pipefail\na || grep -q x f\n", [], None),
    ("or-list ending the line", "set -o pipefail\na ||\n  grep -q x f\n", [], None),
    ("YAML block scalar", "set -o pipefail\n  run: |\n    grep -q x f\n", [], None),
    ("grep without -q", "set -o pipefail\nn=$(a | grep -c x)\n", [], None),
    ("-e takes the next word", "set -o pipefail\na | grep -e -q f\n", [], None),
    ("inside a comment", "set -o pipefail\n# a | grep -q x\n", [], None),
    ("inside single quotes", "set -o pipefail\nx='a | grep -q b'\n", [], None),
    ("inside a heredoc", "set -o pipefail\ncat > f <<'EOF'\na | grep -q x\nEOF\n", [], None),
    ("after a heredoc", "set -o pipefail\ncat > f <<EOF\nz\nEOF\na | grep -q x\n",
     ["command"], None),
    ("quoted string spanning lines",
     "set -o pipefail\npython3 -c \"\nprint(1)\nx = a | grep -q b\n\"\nc | grep -q d\n",
     ["command"], None),
    ("ANSI-C string with an escaped quote",
     "set -o pipefail\nx=$'it\\'s'\nc | grep -q d\n", ["command"], None),
    ("pipefail only inside a string", "x='set -o pipefail'\nc | grep -q d\n", [], None),
    ("command substitution inside double quotes",
     "set -o pipefail\nfail \"x\" \"$(grep '\"a\":\"[^\"]*\"' f | tail -2)\"\nc | grep -q d\n",
     ["command"], None),
    ("CRLF line endings", "set -o pipefail\r\ncat > f <<EOF\r\nz\r\nEOF\r\na | grep -q x\r\n",
     ["command"], None),
    ("here-string is not a heredoc", "set -o pipefail\ngrep <<<\"$x\" -q y\na | grep -q x\n",
     ["command"], None),
]


def selftest():
    bad = 0
    for name, script, kinds, fixed in SELFTEST:
        enabled, sites = scan_text(script)
        got = [k for _, k, _ in sites] if enabled else []
        if got != kinds:
            out("FAIL selftest [%s]: kinds %r, expected %r" % (name, got, kinds))
            bad += 1
            continue
        if fixed is not None:
            new, n = fix_text(script)
            if new != fixed:
                out("FAIL selftest [%s]: fix gave %r" % (name, new))
                bad += 1
                continue
            if scan_text(new)[1]:
                out("FAIL selftest [%s]: the fixed text is still flagged" % name)
                bad += 1
                continue
        out("ok   selftest [%s]" % name)
    bad += selftest_files()
    out("selftest: %d case(s), %d failed" % (len(SELFTEST) + len(FILE_CASES), bad))
    return 1 if bad else 0


# File-level cases: which files count as running under pipefail.
FILE_CASES = [
    # (relative path, contents, expected reason or None)
    ("suite.sh", 'set -uo pipefail\nsource "$D/../helpers/lib.sh"\n', "own"),
    ("helpers/lib.sh", "f() { a | grep -q x; }\n", "sourced by"),
    ("helpers/unrelated.sh", "g() { a | grep -q x; }\n", None),
    (".github/workflows/ci.yml", "    run: a | grep -q x\n", "workflow"),
    ("notes.yml", "run: a | grep -q x\n", None),
]


def selftest_files():
    import tempfile
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        for rel, body, _ in FILE_CASES:
            full = os.path.join(d, rel)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write(body)
        _, _, why = pipefail_files([d])
        for rel, _, want in FILE_CASES:
            got = why.get(os.path.join(d, rel))
            ok = (got is None) if want is None else (got or "").startswith(want)
            out("%s selftest [file %s]: %r" % ("ok  " if ok else "FAIL", rel, got))
            bad += 0 if ok else 1
    return bad


def cmd_stdin(fix, assume_pipefail):
    """Scan (or fix) one script read from stdin. No path crosses into python,
    so this works with a native Windows python3 under MSYS2 too."""
    text = sys.stdin.buffer.read().decode("utf-8", "surrogateescape")
    if fix:
        text, n = fix_text(text, force=assume_pipefail)
        sys.stdout.buffer.write(text.encode("utf-8", "surrogateescape"))
        return 0
    enabled, sites = scan_text(text)
    if not (enabled or assume_pipefail):
        sites = []
    for no, kind, line in sites:
        out("<stdin>:%d: [%s] %s" % (no, kind, line.strip()[:160]))
    out("%d site(s) left" % len(sites))
    return 1 if sites else 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) >= 2 and argv[1] in ("scan-stdin", "fix-stdin"):
        return cmd_stdin(argv[1] == "fix-stdin", "--assume-pipefail" in argv[2:])
    if len(argv) >= 3 and argv[1] in ("scan", "fix"):
        return cmd_scan(argv[2:], fix=(argv[1] == "fix"))
    out(__doc__.strip().split("\n\n")[-2])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
