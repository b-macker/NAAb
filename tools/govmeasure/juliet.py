#!/usr/bin/env python3
"""Cases from NIST's Juliet Test Suite for C/C++ (Bundle 13 PR 2).

    juliet.py cases --zip juliet-cpp-1.3.zip --out cases.jsonl [--per-cwe N] [--seed S]
    juliet.py selftest

Each single-file test case (name ending _NN.c or _NN.cpp) holds a flawed
function between `#ifndef OMITBAD` and its `#endif`, and fixed ones between
`#ifndef OMITGOOD` and its `#endif`. It becomes TWO cases: the file with the
OMITGOOD part removed (bad: true) and the file with the OMITBAD part removed
(bad: false). The labels are NIST's, by construction of the suite.

Removed from both, because they would hand the label to a text check or are
not the test: every comment (they say FLAW and FIX), the `#ifdef INCLUDEMAIN`
tail, and the preprocessor lines that delimit the variants. What stays shared
between the two variants stays: a `#define PASSWORD "..."` above both sits in
the good variant too, so a text check cannot tell them apart there, and the
report shows that as an equal rate on both sides.

Test cases that span several files (names ending in a letter, or classes
split over files) are left out and counted. `--per-cwe N` takes a seeded
sample of N files per CWE (all of them when it has fewer); the CWEs are the
ones check_map.json lists under source "juliet". C files are sent to the
checker as cpp: NAAb has one C-family executor.
"""

import argparse
import json
import os
import random
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
NAME_RE = re.compile(r"testcases/(CWE\d+)_[^/]+/(?:[^/]+/)?[^/]+_\d\d\.(c|cpp)$")
ANY_RE = re.compile(r"testcases/(CWE\d+)_[^/]+/.*\.(c|cpp)$")


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def strip_comments(text):
    """Remove /* */ and // comments, leaving string and char literals alone."""
    res = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch:
                j += 2 if text[j] == "\\" else 1
            res.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            res.append("\n" * text.count("\n", i, j))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        else:
            res.append(ch)
            i += 1
    return "".join(res)


def drop_region(lines, opener):
    """Drop every region from a line matching `opener` to its matching #endif
    (nesting counted). Returns (kept lines, regions dropped)."""
    kept, depth, dropped = [], 0, 0
    for line in lines:
        s = line.strip()
        if depth == 0:
            if re.match(opener, s):
                depth = 1
                dropped += 1
                continue
            kept.append(line)
        else:
            if re.match(r"#\s*if", s):
                depth += 1
            elif re.match(r"#\s*endif", s):
                depth -= 1
    return kept, dropped


def variants(text):
    """(bad variant, good variant) of one test case file, or None when the
    file does not have exactly the two guarded parts."""
    lines = strip_comments(text).split("\n")
    lines, _ = drop_region(lines, r"#\s*ifdef\s+INCLUDEMAIN\b")
    bad, n_good = drop_region(lines, r"#\s*ifndef\s+OMITGOOD\b")
    good, n_bad = drop_region(lines, r"#\s*ifndef\s+OMITBAD\b")
    if n_good < 1 or n_bad < 1:
        return None

    def finish(ls, guard):
        # Unwrap the part that stays: drop its #ifndef line and that #endif.
        res, depth, open_at = [], 0, []
        for line in ls:
            s = line.strip()
            if re.match(r"#\s*ifndef\s+" + guard + r"\b", s):
                open_at.append(depth)
                depth += 1
                continue
            if re.match(r"#\s*if", s):
                depth += 1
            elif re.match(r"#\s*endif", s):
                depth -= 1
                if open_at and open_at[-1] == depth:
                    open_at.pop()
                    continue
            res.append(line)
        text2 = "\n".join(res)
        return re.sub(r"\n{3,}", "\n\n", text2).strip() + "\n"

    return finish(bad, "OMITBAD"), finish(good, "OMITGOOD")


def cmd_cases(args):
    with open(args.map, "r", encoding="utf-8") as f:
        cmap = json.load(f)
    wanted = sorted(c for c, v in cmap.get("categories", {}).items() if v.get("source") == "juliet")
    if not wanted:
        sys.stderr.write("juliet.py: the map lists no juliet category\n")
        return 4
    try:
        z = zipfile.ZipFile(args.zip)
    except (OSError, zipfile.BadZipFile) as e:
        out("UNMEASURABLE: cannot open %s: %s" % (args.zip, e))
        return 2
    by_cwe, multi = {}, {}
    for name in z.namelist():
        m = NAME_RE.search(name)
        a = ANY_RE.search(name)
        if a:
            cwe = "CWE-" + a.group(1)[3:]
            if cwe in wanted and not m:
                multi[cwe] = multi.get(cwe, 0) + 1
        if m:
            cwe = "CWE-" + m.group(1)[3:]
            if cwe in wanted:
                by_cwe.setdefault(cwe, []).append(name)
    rnd = random.Random(args.seed)
    written = 0
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for cwe in wanted:
            names = sorted(by_cwe.get(cwe, []))
            total = len(names)
            if args.per_cwe and total > args.per_cwe:
                names = sorted(rnd.sample(names, args.per_cwe))
            unsplit = 0
            for name in names:
                pair = variants(z.read(name).decode("utf-8", "replace"))
                if pair is None:
                    unsplit += 1
                    continue
                base = os.path.basename(name)
                for bad, code in ((True, pair[0]), (False, pair[1])):
                    f.write(json.dumps({"id": "%s:%s" % (base, "bad" if bad else "good"),
                                        "source": "juliet", "language": "cpp", "category": cwe,
                                        "bad": bad, "code": code}) + "\n")
                    written += 1
            out("%s: %d single-file test cases, %d taken, %d without both guarded parts (left out), "
                "%d files of multi-file test cases (left out)"
                % (cwe, total, len(names) - unsplit, unsplit, multi.get(cwe, 0)))
    out("wrote %d cases to %s" % (written, args.out))
    if written == 0:
        out("UNMEASURABLE: no case was produced")
        return 2
    return 0


SAMPLE = """/* FLAW comment */
#include "std_testcase.h"
#define PASSWORD "x" // FIX: not here
#ifndef OMITBAD
void t_bad()
{
    char * s = "/* not a comment */"; /* FLAW */
    system(s);
}
#endif /* OMITBAD */
#ifndef OMITGOOD
static void good1()
{
#ifdef _WIN32
    safe();
#endif
}
void t_good() { good1(); }
#endif /* OMITGOOD */
#ifdef INCLUDEMAIN
int main() {
#ifndef OMITGOOD
    t_good();
#endif
    return 0; }
#endif
"""


def selftest():
    fails = []

    def expect(name, cond, detail):
        out("  %s [%s] %s" % ("PASS" if cond else "FAIL", name, detail))
        if not cond:
            fails.append(name)

    pair = variants(SAMPLE)
    bad, good = pair if pair else ("", "")
    expect("JT-01", "t_bad" in bad and "system(s)" in bad and "t_good" not in bad and "good1" not in bad,
           "the bad variant keeps the flawed function and drops the fixed ones")
    expect("JT-02", "t_good" in good and "safe()" in good and "t_bad" not in good and "system" not in good,
           "the good variant keeps the fixed functions and drops the flawed one")
    expect("JT-03", all("FLAW" not in v and "FIX" not in v and "OMIT" not in v and "main" not in v
                        for v in (bad, good)),
           "comments, the variant guards and the INCLUDEMAIN tail are gone from both")
    expect("JT-04", '"/* not a comment */"' in bad, "a comment marker inside a string literal is kept")
    expect("JT-05", "#ifdef _WIN32" in good and good.count("#endif") == 1,
           "an unrelated #ifdef inside a variant keeps its own #endif")
    expect("JT-06", '#define PASSWORD "x"' in bad and '#define PASSWORD "x"' in good,
           "text above both parts stays in both variants")
    expect("JT-07", variants("int f() { return 0; }\n") is None,
           "a file without the two guarded parts yields no case")
    out("")
    out("selftest: %d failed" % len(fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("cases")
    c.add_argument("--zip", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--map", default=os.path.join(HERE, "check_map.json"))
    c.add_argument("--per-cwe", type=int, default=0)
    c.add_argument("--seed", type=int, default=1)
    sub.add_parser("selftest")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 4 if e.code else 0
    if args.cmd == "cases":
        return cmd_cases(args)
    if args.cmd == "selftest":
        return selftest()
    ap.print_usage(sys.stderr)
    return 4


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
