#!/usr/bin/env python3
"""Cases from the Semgrep rules repository's own test files (Bundle 13 PR 3):
security labels for Python, JavaScript, shell and the other languages NAAb
runs, which NIST Juliet does not cover.

    semgrep_tests.py cases --repo DIR --out cases.jsonl [--languages python,javascript,...]
    semgrep_tests.py selftest

Beside each rule (NAME.yaml) the repository keeps a test file in the rule's
language whose comments mark, line by line, where the rule must fire
(`ruleid: NAME`) and where it must not (`ok: NAME`). Only rules under a
`security` directory are taken. A test file becomes one case per marked
place: the file's opening (everything before its first mark: imports, set-up)
plus the stretch from that mark to the next one. `ruleid` gives bad: true,
`ok` gives bad: false; the category is the rule's first CWE, read from its
yaml. The marker comments themselves are removed, since they carry the label.
`todoruleid` / `todook` / `deepruleid` marks (known gaps in Semgrep itself,
or its paid engine) are left out and counted.

What a label means here, and it is narrower than "dangerous": `ok` says THIS
rule must not fire, usually because the argument is a constant or was
sanitised. NAAb's text checks flag the call whatever its argument, so a
flagged `ok` case is often the same sink on a constant (the Juliet CWE-78
result again), not a mistake about what the code is. A stretch cut out of
the middle of a function is not a program; the checker reads text, so that
does not matter to it, but nothing here is run.

LICENCE. The repository is under the Semgrep Rules License v1.0
(https://semgrep.dev/legal/rules-license, read 2026-11-11): use "only for
your own internal business purposes", no distributing the rules or making
them available to others as a service. So: the checkout is fetched at run
time (datasets.py, pinned to a commit), the cases file is written to a
scratch directory, and NEITHER is ever committed, uploaded as a build
artifact or quoted in a report. Reports carry counts and NAAb's own rule
names only: no Semgrep rule id, file path or code. A case's id is therefore
opaque (a hash of its file's path, the mark's number and its kind), because
the scorer lists a few ids in its report when cases give no readable answer;
and this tool prints counts per language, never a file or rule name.
"""

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# Semgrep directory -> (NAAb language, test-file extensions, line-comment token)
LANGS = {
    "python": ("python", (".py",), "#"),
    "javascript": ("javascript", (".js", ".jsx"), "//"),
    "typescript": ("typescript", (".ts", ".tsx"), "//"),
    "bash": ("shell", (".sh", ".bash"), "#"),
    "go": ("go", (".go",), "//"),
    "ruby": ("ruby", (".rb",), "#"),
    "php": ("php", (".php",), "//"),
    "csharp": ("csharp", (".cs",), "//"),
    "rust": ("rust", (".rs",), "//"),
    "c": ("cpp", (".c", ".cpp"), "//"),
}
MARK_RE = re.compile(r"(?:#|//|/\*|<!--)\s*(todoruleid|todook|deepruleid|deepok|ruleid|ok)\s*:\s*([A-Za-z0-9_.\-]+)")
CWE_RE = re.compile(r"CWE-(\d+)")


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def rule_cwes(yaml_text):
    """{rule id: "CWE-N"} for each rule in one yaml file: the first CWE named
    after each `- id:` line (no yaml parser: the two shapes are stable)."""
    result, current = {}, None
    for line in yaml_text.split("\n"):
        m = re.match(r"\s*-\s+id:\s*([A-Za-z0-9_.\-]+)", line)
        if m:
            current = m.group(1)
            continue
        if current and current not in result:
            c = CWE_RE.search(line)
            if c and "cwe" in yaml_text.lower():
                result[current] = "CWE-" + c.group(1)
    return result


def split_cases(text):
    """[(kind, rule, snippet)] for one test file; kind is "ruleid" or "ok".
    Also returns how many marks were left out (todo*/deep*)."""
    lines = text.split("\n")
    marks = []
    for i, line in enumerate(lines):
        m = MARK_RE.search(line)
        if m:
            marks.append((i, m.group(1), m.group(2)))
    if not marks:
        return [], 0
    opening = [l for l in lines[:marks[0][0]] if not MARK_RE.search(l)]
    cases, skipped = [], 0
    for k, (i, kind, rule) in enumerate(marks):
        end = marks[k + 1][0] if k + 1 < len(marks) else len(lines)
        if kind not in ("ruleid", "ok"):
            skipped += 1
            continue
        # A mark at the end of a code line labels that line; one on its own
        # line labels what follows.
        own_line = lines[i].strip().startswith(("#", "//", "/*", "<!--"))
        body = ([] if own_line else [MARK_RE.split(lines[i])[0].rstrip(" \t#/*<!-")]) + lines[i + 1:end]
        if not any(l.strip() for l in body):
            skipped += 1
            continue
        cases.append((kind, rule, "\n".join(opening + body).strip("\n") + "\n"))
    return cases, skipped


def cmd_cases(args):
    wanted = [l.strip() for l in args.languages.split(",") if l.strip()] if args.languages else sorted(LANGS)
    unknown = [l for l in wanted if l not in LANGS]
    if unknown:
        sys.stderr.write("semgrep_tests.py: unknown language(s): %s\n" % ", ".join(unknown))
        return 4
    if not os.path.isdir(args.repo):
        out("UNMEASURABLE: %s is not a directory" % args.repo)
        return 2
    written = 0
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for lang in wanted:
            naab_lang, exts, _ = LANGS[lang]
            files = bad = good = skipped = no_cwe = 0
            for dp, dns, fns in os.walk(os.path.join(args.repo, lang)):
                dns.sort()
                parts = dp.replace(os.sep, "/").split("/")
                if "security" not in parts:
                    continue
                cwes = {}
                for fn in sorted(fns):
                    if fn.endswith((".yaml", ".yml")):
                        try:
                            with open(os.path.join(dp, fn), "r", encoding="utf-8", errors="replace") as y:
                                cwes.update(rule_cwes(y.read()))
                        except OSError:
                            pass
                for fn in sorted(fns):
                    if not fn.endswith(exts) or ".fixed." in fn:
                        continue
                    try:
                        with open(os.path.join(dp, fn), "r", encoding="utf-8", errors="replace") as t:
                            cases, skip = split_cases(t.read())
                    except OSError:
                        continue
                    files += 1
                    skipped += skip
                    rel = os.path.relpath(os.path.join(dp, fn), args.repo).replace(os.sep, "/")
                    for n, (kind, rule, snippet) in enumerate(cases):
                        cwe = cwes.get(rule)
                        if not cwe:
                            no_cwe += 1
                            continue
                        opaque = hashlib.sha256(rel.encode("utf-8")).hexdigest()[:12]
                        f.write(json.dumps({"id": "sg-%s-%d:%s" % (opaque, n, kind), "source": "semgrep/" + lang,
                                            "language": naab_lang, "category": cwe, "bad": kind == "ruleid",
                                            "code": snippet}) + "\n")
                        written += 1
                        if kind == "ruleid":
                            bad += 1
                        else:
                            good += 1
            out("%s: %d test files, %d bad cases, %d good cases; left out: %d todo/deep marks or empty "
                "stretches, %d whose rule names no CWE" % (lang, files, bad, good, skipped, no_cwe))
    out("wrote %d cases to %s (do not commit or upload this file: see the licence note in this tool)"
        % (written, args.out))
    return 0 if written else 2


SAMPLE = """import os
from lib import fetch

# ok: demo-rule
os.system("ls -al")

def f(x):
    # ruleid: demo-rule
    os.system(x)
    return 1

os.popen(fetch())  # ruleid: demo-rule

# todoruleid: demo-rule
os.system(fetch())
# ok: other-rule
print("done")
"""
SAMPLE_YAML = """rules:
- id: demo-rule
  metadata:
    cwe:
    - "CWE-78: OS Command Injection"
    owasp: "A03"
- id: other-rule
  metadata:
    category: style
"""


def selftest():
    fails = []

    def expect(name, cond, detail):
        out("  %s [%s] %s" % ("PASS" if cond else "FAIL", name, detail))
        if not cond:
            fails.append(name)

    cases, skipped = split_cases(SAMPLE)
    kinds = [(k, r) for k, r, s in cases]
    expect("SG-01", kinds == [("ok", "demo-rule"), ("ruleid", "demo-rule"), ("ruleid", "demo-rule"),
                              ("ok", "other-rule")] and skipped == 1,
           "four marked places become four cases; the todo mark is left out and counted")
    expect("SG-02", all("ruleid" not in s and "ok:" not in s for k, r, s in cases),
           "no case carries a marker comment")
    expect("SG-03", all(s.startswith("import os\nfrom lib import fetch") for k, r, s in cases),
           "every case starts with the file's opening")
    expect("SG-04", 'os.system("ls -al")' in cases[0][2] and "os.system(x)" not in cases[0][2]
           and "os.system(x)" in cases[1][2] and 'os.system("ls -al")' not in cases[1][2],
           "a case holds its own stretch and not its neighbour's")
    expect("SG-05", "os.popen(fetch())" in cases[2][2] and "os.system(fetch())" not in cases[2][2],
           "a mark at the end of a code line labels that line; the todo stretch after it is not included")
    cw = rule_cwes(SAMPLE_YAML)
    expect("SG-06", cw == {"demo-rule": "CWE-78"}, "a rule's first CWE is read; a rule with none has none")
    with tempfile.TemporaryDirectory(prefix="sgtest-") as root:
        d = os.path.join(root, "python", "lang", "security")
        os.makedirs(d)
        os.makedirs(os.path.join(root, "python", "lang", "style"))
        for base in (d, os.path.join(root, "python", "lang", "style")):
            with open(os.path.join(base, "demo.py"), "w", encoding="utf-8") as f:
                f.write(SAMPLE)
            with open(os.path.join(base, "demo.yaml"), "w", encoding="utf-8") as f:
                f.write(SAMPLE_YAML)
        outp = os.path.join(root, "cases.jsonl")

        class A:
            repo, out, languages = root, outp, "python"
        import io
        cap = io.BytesIO()
        real = sys.stdout

        class Out:
            buffer = cap
        sys.stdout = Out()
        try:
            rc = cmd_cases(A)
        finally:
            sys.stdout = real
        printed = cap.getvalue().decode("ascii", "replace").replace(outp, "")
        rows = [json.loads(l) for l in open(outp, encoding="utf-8")]
        expect("SG-08", rows and all("demo" not in r["id"] and "/" not in r["id"] and "security" not in r["id"]
                                     for r in rows) and len({r["id"] for r in rows}) == len(rows)
               and "demo" not in printed and "os.system" not in printed,
               "a case's id is opaque and unique, and what the tool prints names no file, rule or code")
        expect("SG-07", rc == 0 and len(rows) == 3 and [r["bad"] for r in rows] == [False, True, True]
               and all(r["category"] == "CWE-78" and r["source"] == "semgrep/python"
                       and r["language"] == "python" for r in rows),
               "only the security directory is read; the case whose rule names no CWE is left out")
    out("")
    out("selftest: %d failed" % len(fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("cases")
    c.add_argument("--repo", required=True)
    c.add_argument("--out", required=True)
    c.add_argument("--languages", default="")
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
