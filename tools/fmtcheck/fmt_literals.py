#!/usr/bin/env python3
"""Check every fmt call whose format string is a literal (A42).

fmt checks a format string at COMPILE time only under C++20, where the
format_string constructor is consteval. NAAb builds as C++17, so a bad literal
compiles and throws at run time instead -- in a help or error message, the one
place a reader most needs it to print. A42 found two: `returns {valid, error}`
in agent.<unknown>()'s help (a named argument that does not exist: "argument
not found", so a typo got a crash instead of the list of functions) and
`main { ... }` in `naab-lang blocks test` ("invalid format string", abort).

This reads each fmt::format / print / println / format_to / format_to_n call,
takes its format argument when that argument is nothing but string literals,
and checks it the way fmt's own parser does: `{{` and `}}` are escapes, a lone
`}` is an error, a replacement field is `{` [arg-id] [`:` spec] `}`, a spec may
hold nested `{}` fields (dynamic width/precision), auto and manual indexing may
not mix, and there must be at least as many arguments as the fields refer to.
A named arg-id is reported too: NAAb passes no named arguments (fmt::arg / _a),
so every one is "argument not found".

Screened, not verified: it is a small C++ tokenizer, not a compiler. It cannot
see a format string built at run time (fmt::runtime, a variable), and counts
those separately so a run that saw none can be told apart. The authoritative
check is the compiler's: `--cxx20` runs every repo translation unit through
`-std=gnu++20 -fsyntax-only` from build/compile_commands.json, where fmt's
consteval checker rejects exactly these defects (slow: minutes, not seconds).

Output is ASCII. Exit 0 = no problem found, 1 = problems, 2 = unusable.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys

# Every call that hands a format string to fmt: fmt itself, spdlog (fmt's
# format_string underneath), and NAAb's LOG_* macros, which forward theirs
# through a std::string -- so not even a C++20 build can check those.
CALL_RE = re.compile(
    r"\b(?:fmt\s*::\s*(?P<fmt>format_to_n|format_to|format|println|print)"
    r"|spdlog\s*::\s*(?P<spd>trace|debug|info|warn|error|critical)"
    r"|(?P<log>LOG_(?:TRACE|DEBUG|INFO|WARN|ERROR)))\s*\(")
SOURCE_DIRS = ("src", "include", "tests/unit", "tools")
SOURCE_EXT = (".cpp", ".cc", ".h", ".hpp")

STRING_PREFIX_RE = re.compile(r'(?:u8|u|U|L)?R?"')
FORMAT_POSITION = {"format": 0, "print": 0, "println": 0, "format_to": 1, "format_to_n": 2}
FILE_ARG_RE = re.compile(r"^\s*(?:std::)?(?:stdout|stderr)\s*$")
# fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "...", ...)
STYLE_ARG_RE = re.compile(r"^\s*(?:fmt::)?(?:fg|bg)\s*\(|^\s*fmt::emphasis::")


def skip_ws_comments(s, i):
    n = len(s)
    while i < n:
        c = s[i]
        if c in " \t\r\n\\":
            i += 1
        elif s.startswith("//", i):
            j = s.find("\n", i)
            i = n if j < 0 else j + 1
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            break
    return i


def read_string(s, i):
    """At a string literal (any prefix, raw or not): (decoded, end) or None."""
    m = STRING_PREFIX_RE.match(s, i)
    if not m:
        return None
    raw = "R" in m.group(0)
    j = m.end()
    if raw:
        k = s.find("(", j)
        if k < 0:
            return None
        delim = s[j:k]
        close = ")" + delim + '"'
        e = s.find(close, k + 1)
        if e < 0:
            return None
        return s[k + 1:e], e + len(close)
    out = []
    n = len(s)
    while j < n:
        c = s[j]
        if c == '"':
            return "".join(out), j + 1
        if c == "\n":
            return None
        if c == "\\" and j + 1 < n:
            d = s[j + 1]
            simple = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "a": "\a",
                      "b": "\b", "f": "\f", "v": "\v", "\\": "\\", '"': '"',
                      "'": "'", "?": "?", "\n": ""}
            if d == "x":
                k = j + 2
                while k < n and s[k] in "0123456789abcdefABCDEF":
                    k += 1
                out.append(chr(int(s[j + 2:k] or "0", 16) & 0xFF))
                j = k
                continue
            if d in "01234567":
                k = j + 1
                while k < n and k < j + 4 and s[k] in "01234567":
                    k += 1
                out.append(chr(int(s[j + 1:k], 8) & 0xFF))
                j = k
                continue
            if d in "uU":
                w = 4 if d == "u" else 8
                out.append(chr(int(s[j + 2:j + 2 + w], 16)))
                j += 2 + w
                continue
            out.append(simple.get(d, d))
            j += 2
            continue
        out.append(c)
        j += 1
    return None


def split_args(s, i):
    """At the char after '(': list of (text, start) top-level args, end index."""
    args = []
    depth = 0
    start = i
    n = len(s)
    while i < n:
        c = s[i]
        if c in "\"" or (c in "uULR" and STRING_PREFIX_RE.match(s, i)
                         and (i == 0 or not (s[i - 1].isalnum() or s[i - 1] == "_"))):
            r = read_string(s, i)
            if r is None:
                return None, i
            i = r[1]
            continue
        if c == "'":
            # char literal (or a digit separator: 1'000 -- then i-1 is a digit)
            if i > 0 and s[i - 1].isdigit():
                i += 1
                continue
            j = i + 1
            while j < n and s[j] != "'":
                j += 2 if s[j] == "\\" else 1
            i = j + 1
            continue
        if s.startswith("//", i) or s.startswith("/*", i):
            i = skip_ws_comments(s, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                if c != ")":
                    return None, i
                args.append((s[start:i], start))
                return args, i + 1
            depth -= 1
        elif c == "," and depth == 0:
            args.append((s[start:i], start))
            start = i + 1
        i += 1
    return None, i


def literal_only(text):
    """The decoded value when `text` is only adjacent string literals."""
    i = skip_ws_comments(text, 0)
    parts = []
    while i < len(text):
        r = read_string(text, i)
        if r is None:
            return None
        parts.append(r[0])
        i = skip_ws_comments(text, r[1])
    return "".join(parts) if parts else None


def check_format(fmt_str, nargs):
    """fmt's parse rules. Returns a list of problems (empty = valid)."""
    problems = []
    i = 0
    n = len(fmt_str)
    auto = 0
    manual_max = -1
    mode = None  # "auto" | "manual"

    def excerpt(pos):
        lo = max(0, pos - 24)
        hi = min(n, pos + 24)
        text = fmt_str[lo:hi].encode("unicode_escape").decode("ascii")
        return "%s%s%s" % ("..." if lo else "", text, "..." if hi < n else "")

    def field(i, in_spec):
        nonlocal auto, manual_max, mode
        # i is just past '{'
        j = i
        while j < n and (fmt_str[j].isalnum() or fmt_str[j] == "_"):
            j += 1
        arg_id = fmt_str[i:j]
        if arg_id == "":
            if mode == "manual":
                problems.append("cannot switch from manual to automatic argument indexing")
            mode = mode or "auto"
            auto += 1
        elif arg_id.isdigit():
            if mode == "auto":
                problems.append("cannot switch from automatic to manual argument indexing")
            mode = mode or "manual"
            manual_max = max(manual_max, int(arg_id))
        elif arg_id[0].isdigit():
            problems.append("invalid format string: arg-id %r" % arg_id)
            return n
        named = not (arg_id == "" or arg_id.isdigit())
        if j >= n:
            problems.append("invalid format string: unterminated field")
            return n
        if named and fmt_str[j] in "}:":
            problems.append("named argument {%s} -- no named arguments are passed "
                            "(argument not found): %s" % (arg_id, excerpt(i - 1)))
        if fmt_str[j] == "}":
            return j + 1
        if fmt_str[j] != ":":
            problems.append("invalid format string: %r after {%s: %s" % (fmt_str[j], arg_id, excerpt(i - 1)))
            return n
        if in_spec:
            problems.append("invalid format string: nested field with a spec")
            return n
        j += 1
        while j < n:
            c = fmt_str[j]
            if c == "{":
                j = field(j + 1, True)
                continue
            if c == "}":
                return j + 1
            j += 1
        problems.append("invalid format string: unterminated spec")
        return n

    while i < n:
        c = fmt_str[i]
        if c == "{":
            if i + 1 < n and fmt_str[i + 1] == "{":
                i += 2
                continue
            i = field(i + 1, False)
            continue
        if c == "}":
            if i + 1 < n and fmt_str[i + 1] == "}":
                i += 2
                continue
            problems.append("invalid format string: unmatched '}': %s" % excerpt(i))
            i += 1
            continue
        i += 1
    needed = auto if mode == "auto" else manual_max + 1
    if not problems and needed > nargs:
        problems.append("%d field(s) but %d argument(s) (argument not found)" % (needed, nargs))
    return problems


def line_of(s, idx):
    return s.count("\n", 0, idx) + 1


def scan_text(text, path, stats):
    found = []
    for m in CALL_RE.finditer(text):
        # skip a match inside a comment or string on the same line (cheap guard)
        ls = text.rfind("\n", 0, m.start()) + 1
        prefix = text[ls:m.start()]
        if "//" in prefix or prefix.count('"') % 2 == 1:
            continue
        if m.group("fmt"):
            func, label = m.group("fmt"), "fmt::" + m.group("fmt")
        elif m.group("spd"):
            func, label = "format", "spdlog::" + m.group("spd")
        else:
            func, label = "format", m.group("log")
        args, _ = split_args(text, m.end())
        stats["calls"] += 1
        if not args:
            stats["unparsed"] += 1
            continue
        # Where the format argument sits: format_to after the output iterator,
        # format_to_n after the iterator and size, print/println first or
        # after a FILE* (only stdout/stderr are taken as one, so a runtime
        # format followed by a string ARGUMENT is never read as the format).
        fmt_index = FORMAT_POSITION[func]
        if func in ("print", "println") and len(args) > 1 and (
                FILE_ARG_RE.match(args[0][0]) or STYLE_ARG_RE.match(args[0][0])):
            fmt_index = 1
        if fmt_index >= len(args) or literal_only(args[fmt_index][0]) is None:
            stats["runtime"] += 1
            continue
        stats["literal"] += 1
        fmt_str = literal_only(args[fmt_index][0])
        nargs = len(args) - fmt_index - 1
        for p in check_format(fmt_str, nargs):
            found.append((path, line_of(text, args[fmt_index][1] + 1), label, p))
    return found


def repo_files(root):
    for d in SOURCE_DIRS:
        base = os.path.join(root, d)
        for dp, dn, fn in os.walk(base):
            dn[:] = [x for x in dn if x not in ("external", "build", ".cache", "node_modules")]
            for f in sorted(fn):
                if f.endswith(SOURCE_EXT):
                    yield os.path.join(dp, f)


def scan(root, paths, stdin_label=None):
    stats = {"files": 0, "calls": 0, "literal": 0, "runtime": 0, "unparsed": 0}
    found = []
    for p in paths:
        if p == "-":
            # Bytes, not a path: a test hands a planted copy of a real file
            # here, because under MSYS2 a native python3 cannot open /tmp.
            text = sys.stdin.buffer.read().decode("utf-8", errors="replace")
            stats["files"] += 1
            found.extend(scan_text(text, stdin_label or "<stdin>", stats))
            continue
        try:
            with open(p, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        stats["files"] += 1
        found.extend(scan_text(text, os.path.relpath(p, root), stats))
    return found, stats


SELFTEST = [
    # (call text, expected problem count > 0?)
    ('fmt::format("returns {valid, error} for {}", x)', True),     # A42 site 1 shape
    ('fmt::print("  main { let r = a.f(args) }\\n");', True),        # A42 site 2 shape
    ('fmt::format("a }")', True),
    ('fmt::format("{} {}", a)', True),
    ('fmt::format("{0} {}", a, b)', True),
    ('fmt::format("{name}", a)', True),
    ('fmt::print(stderr, "{x}\\n", a);', True),
    ('fmt::format(\n  "line one {}\\n"\n  "line two {", a)', True),
    ('fmt::format("{{literal}} {}", a)', False),
    ('fmt::format("{:>5} {:.{}f}", a, b, c)', False),
    ('fmt::format("{0} {0} {1}", a, b)', False),
    ('fmt::print(stderr, "{} of {}\\n", a, b);', False),
    ('fmt::format(R"(raw {{ }} {})", a)', False),
    ('fmt::format(R"(raw { bad)", a)', True),
    ('fmt::format(fmt::runtime(s), a)', False),
    ('fmt::format(s, a)', False),
    ('fmt::print("x={}\\n", f(a, "{"));', False),                    # brace in an ARG, not the format
    ('fmt::format("\\x7b bad")', True),                              # hex escape for '{'
    ('// fmt::format("{bad}")', False),                              # commented out
    ('fmt::format_to(out, "{} {}", a)', True),
    ('fmt::format_to(out, "{}", a)', False),
    ('fmt::print(fg(fmt::color::red) | fmt::emphasis::bold, "bad {x}\\n", a);', True),
    ('fmt::print(fmt::emphasis::bold, "{} ok\\n", a);', False),
    ('fmt::print(msg, "{not the format}");', False),             # a runtime format then a string ARG
    ('spdlog::info("server on {}:{}", host);', True),
    ('spdlog::warn("{} ok", a);', False),
    ('LOG_DEBUG("[M] Loaded: {} v{}\\n", name);', True),
    ('LOG_DEBUG("[M] Loaded: {} v{}\\n", name, ver);', False),
]


def selftest():
    bad = 0
    for text, want in SELFTEST:
        st = {"files": 0, "calls": 0, "literal": 0, "runtime": 0, "unparsed": 0}
        got = bool(scan_text(text, "<selftest>", st))
        ok = got == want
        bad += not ok
        print("  %s  %s  %s" % ("ok  " if ok else "FAIL", "flag" if want else "pass", text.replace("\n", "\\n")))
    print("selftest: %d case(s), %d wrong" % (len(SELFTEST), bad))
    return 1 if bad else 0


def cxx20(root, jobs):
    """The compiler's answer: every repo unit as C++20, syntax only."""
    import concurrent.futures as cf
    ccp = os.path.join(root, "build", "compile_commands.json")
    if not os.path.exists(ccp):
        print("UNMEASURABLE: %s missing (run cmake)" % ccp)
        return 2
    units, seen = [], set()
    for e in json.load(open(ccp, encoding="utf-8")):
        f = os.path.realpath(e["file"])
        rel = os.path.relpath(f, root)
        if rel.startswith("..") or "/external/" in "/" + rel or not f.endswith((".cpp", ".cc")):
            continue
        if f in seen:
            continue
        seen.add(f)
        args = e.get("arguments") or shlex.split(e["command"])
        out, skip = [], False
        for a in args:
            if skip:
                skip = False
                continue
            if a == "-o":
                skip = True
                continue
            if a == "-c":
                continue
            out.append("-std=gnu++20" if a.startswith("-std=") else a)
        if not any(a.startswith("-std=") for a in out):
            out.insert(1, "-std=gnu++20")
        out.append("-fsyntax-only")
        units.append((rel, e["directory"], out))

    def run(u):
        p = subprocess.run(u[2], cwd=u[1], capture_output=True, text=True, errors="replace")
        return u[0], p.returncode, p.stderr

    fmt_err = re.compile(r"report_error|format_string_checker|on_error\(")
    loc = re.compile(r"^(/[^:]+):(\d+):\d+:\s+in 'constexpr' expansion", re.M)
    problems = 0
    other = 0
    with cf.ThreadPoolExecutor(jobs) as ex:
        for rel, rc, err in ex.map(run, units):
            if rc == 0:
                continue
            if fmt_err.search(err):
                for f, ln in sorted(set(loc.findall(err))):
                    print("%s:%s: fmt format string rejected by the compiler" % (os.path.relpath(f, root), ln))
                    problems += 1
            else:
                other += 1
                first = next((l for l in err.splitlines() if "error" in l), err[:200])
                print("%s: does not compile as C++20 (not a format-string error): %s" % (rel, first[:200]))
    print("cxx20: %d unit(s), %d format-string rejection(s), %d unit(s) failing for other reasons"
          % (len(units), problems, other))
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--cxx20", action="store_true", help="the compiler's check (slow)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--label", help="the name reported for '-' (stdin)")
    ap.add_argument("paths", nargs="*", help="files to scan, '-' = stdin (default: the repo's sources)")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.cxx20:
        return cxx20(a.root, a.jobs)
    paths = a.paths or list(repo_files(a.root))
    found, stats = scan(a.root, paths, a.label)
    if a.json:
        sys.stdout.write(json.dumps({"stats": stats, "problems": [
            {"file": f, "line": ln, "call": c, "problem": p} for f, ln, c, p in found]}) + "\n")
    else:
        for f, ln, c, p in found:
            print("%s:%d: %s: %s" % (f, ln, c, p))
        print("scanned %(files)d file(s): %(calls)d fmt call(s), %(literal)d literal format(s) checked, "
              "%(runtime)d runtime format(s) not checked, %(unparsed)d unparsed" % stats)
        print("%d problem(s)" % len(found))
    if stats["literal"] == 0:
        print("UNMEASURABLE: no literal format string was found -- the scanner saw nothing")
        return 2
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
