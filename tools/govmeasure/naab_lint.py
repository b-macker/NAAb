#!/usr/bin/env python3
"""What would the lint-only whole-file checks flag in NAAb code? (A132,
Bundle 13 PR 3.) Report only.

    naab_lint.py measure [ROOT...] --gov build/naab-gov [--json OUT] [--md OUT] [--samples N]
    naab_lint.py selftest --gov build/naab-gov

`naab-gov lint` applies fifteen whole-file checks to a .naab file that
`naab-lang run` applies to polyglot blocks only (A132). Before anyone decides
whether a run should apply them to NAAb code too, this counts what they flag
in the NAAb programs that exist: every .naab file under the roots (default:
the repository) is linted under tools/langconform/config.json (audit mode, so
every check runs and none stops the others), twice --

  whole     the file as it is. Lint reads the whole text, block bodies
            included, so this is what lint reports today.
  naab-only the same file with each polyglot block's body lines emptied
            (header and closing line kept). A run already applies these
            checks to block bodies, so only what is flagged HERE would be
            new at run time.

Per check it prints the files flagged each way and, for the naab-only hits,
the matched text lint printed, most frequent first: the material for reading
a sample by hand. The corpus is this repository's own code, much of it test
fixtures written to trip a check; the counts say what would start being
flagged here, not how often a check is wrong.

Three outcomes: exit 0 a report; 2 UNMEASURABLE (the two controls -- a
planted file that must be flagged and a plain one that must not -- or more
than 5% of files giving no summary line); 4 usage. A file lint cannot parse
is counted and left out; so is one whose emptied copy does not parse.
"""

import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP_DIRS = {"build", "external", "test-parallel", "test-timing", ".git", "node_modules", ".cache"}
BLOCK_RE = re.compile(r"(<<[A-Za-z0-9_+#-]+(?:\[[^\]\n]*\])?[ \t]*\n)(.*?)(\n[ \t]*>>)", re.S)
AUDIT_RE = re.compile(r"^\[governance\] AUDIT ([A-Za-z0-9_.\-]+): (.*)$")
# The fifteen checks cmdLint applies to the whole file and a run does not
# apply to NAAb text (A132), by the rule name each reports under.
LINT_ONLY = [
    "languages.per_language.banned_functions",
    "code_quality.no_temporary_code",
    "code_quality.no_simulation_markers",
    "code_quality.no_mock_data",
    "code_quality.no_apologetic_language",
    "code_quality.no_dead_code",
    "code_quality.no_debug_artifacts",
    "code_quality.no_unsafe_deserialization",
    "code_quality.no_sql_injection",
    "code_quality.no_path_traversal",
    "restrictions.shell_injection",
    "restrictions.code_injection",
    "code_quality.no_hardcoded_urls",
    "code_quality.no_hardcoded_ips",
    "code_quality.no_hardcoded_results",
]
PLANTED = ('main {\n    let id = "7"\n    let q = "SELECT * FROM users WHERE id=" + id\n    print(q)\n}\n',
           "code_quality.no_sql_injection")
PLAIN = "main {\n    let total = 2 + 3\n    print(total)\n}\n"


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def err(text):
    sys.stderr.buffer.write((text + "\n").encode("ascii", "replace"))


def empty_block_bodies(text):
    """Each polyglot block's body replaced by as many empty lines (line
    numbers keep their meaning; header and closing line stay)."""
    return BLOCK_RE.sub(lambda m: m.group(1) + "\n" * m.group(2).count("\n") + m.group(3), text)


def config_file():
    with open(os.path.join(HERE, "..", "langconform", "config.json"), "r", encoding="utf-8") as f:
        doc = json.load(f)
    doc.pop("extends", None)
    fd, tmp = tempfile.mkstemp(prefix="naablint-", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return tmp


def lint(gov, cfg, path):
    """{rule: [first line of each finding]} for one file, or None when lint
    printed no summary (it could not parse the file, or did not run)."""
    try:
        p = subprocess.run(gov + ["lint", path, "--config", cfg], stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = p.stdout.decode("utf-8", "replace").replace("\r", "")
    # Lint runs its whole-file checks before it parses, so a file it cannot
    # parse still gets a summary line; such a file is not a program.
    if "[governance] Summary (mode:" not in text or "naab-gov lint: cannot parse" in text:
        return None
    found = collections.defaultdict(list)
    for line in text.split("\n"):
        m = AUDIT_RE.match(line)
        if m:
            found[m.group(1)].append(m.group(2))
    return found


def lint_text(gov, cfg, text, directory):
    fd, path = tempfile.mkstemp(prefix="case-", suffix=".naab", dir=directory)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    try:
        return lint(gov, cfg, path)
    finally:
        os.remove(path)


def naab_files(roots):
    for root in roots:
        for dp, dns, fns in os.walk(root):
            dns[:] = sorted(d for d in dns if d not in SKIP_DIRS and not d.startswith("build"))
            for fn in sorted(fns):
                if fn.endswith(".naab"):
                    yield os.path.join(dp, fn)


def matched_text(message):
    """The quoted match in a finding's first line, else the line itself."""
    m = re.search(r'"(.*)"', message)
    return (m.group(1) if m else message)[:80]


def controls(gov, cfg, scratch):
    reasons = []
    planted = lint_text(gov, cfg, PLANTED[0], scratch)
    if planted is None:
        reasons.append("lint gave no summary for the planted control file")
    elif PLANTED[1] not in planted:
        reasons.append("lint does not flag the planted file with %s" % PLANTED[1])
    plain = lint_text(gov, cfg, PLAIN, scratch)
    if plain is None:
        reasons.append("lint gave no summary for the plain control file")
    elif any(r in plain for r in LINT_ONLY):
        reasons.append("lint flags the plain control file: %s"
                       % ", ".join(r for r in LINT_ONLY if r in plain))
    return reasons


def measure(roots, gov, jobs=4, samples=8):
    cfg = config_file()
    report = {"status": "ok", "reasons": []}
    try:
        with tempfile.TemporaryDirectory(prefix="naablint-") as scratch:
            report["reasons"] = controls(gov, cfg, scratch)
            if report["reasons"]:
                report["status"] = "unmeasurable"
                return report
            files = list(naab_files(roots))

            def one(path):
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as f:
                        text = f.read()
                except OSError:
                    return path, None, None, False
                whole = lint(gov, cfg, path)
                emptied = empty_block_bodies(text)
                has_blocks = emptied != text
                naab = lint_text(gov, cfg, emptied, scratch) if has_blocks else whole
                return path, whole, naab, has_blocks

            with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
                results = list(ex.map(one, files))
    finally:
        os.remove(cfg)

    unparsed = [p for p, w, n, hb in results if w is None]
    emptied_unparsed = [p for p, w, n, hb in results if w is not None and n is None]
    measured = [(p, w, n) for p, w, n, hb in results if w is not None and n is not None]
    report.update({"files": len(results), "measured": len(measured), "unparsed": len(unparsed),
                   "emptied_unparsed": len(emptied_unparsed),
                   "with_blocks": sum(1 for p, w, n, hb in results if hb)})
    if results and (len(unparsed) + len(emptied_unparsed)) / len(results) > 0.05 and not measured:
        report["status"] = "unmeasurable"
        report["reasons"].append("no file gave a summary")
        return report
    checks = []
    any_whole, any_naab = set(), set()
    for rule in LINT_ONLY:
        whole_files = [p for p, w, n in measured if rule in w]
        naab_files_ = [p for p, w, n in measured if rule in n]
        any_whole.update(whole_files)
        any_naab.update(naab_files_)
        texts = collections.Counter()
        where = {}
        for p, w, n in measured:
            for msg in n.get(rule, []):
                t = matched_text(msg)
                texts[t] += 1
                where.setdefault(t, p)
        checks.append({"rule": rule, "whole": len(whole_files), "naab_only": len(naab_files_),
                       "matches": [{"text": t, "findings": c, "example_file": where[t]}
                                   for t, c in texts.most_common(samples)],
                       "distinct_matches": len(texts)})
    report["checks"] = checks
    report["any_whole"] = len(any_whole)
    report["any_naab_only"] = len(any_naab)
    return report


def render_md(report, title):
    lines = ["# %s" % title, ""]
    if report["status"] != "ok":
        lines += ["**UNMEASURABLE.** No count below is a finding.", ""] + ["- %s" % r for r in report["reasons"]]
        return "\n".join(lines)
    lines.append("%d .naab files; %d measured (%d contain polyglot blocks); %d lint could not parse, "
                 "%d whose emptied copy did not parse (left out)."
                 % (report["files"], report["measured"], report["with_blocks"], report["unparsed"],
                    report["emptied_unparsed"]))
    lines.append("")
    lines.append("Flagged by at least one of the fifteen: %d files as they are, %d with block bodies emptied."
                 % (report["any_whole"], report["any_naab_only"]))
    lines.append("")
    lines.append("| check | files flagged (whole file) | files flagged (NAAb text only) | distinct matches |")
    lines.append("|---|---|---|---|")
    for c in sorted(report["checks"], key=lambda c: -c["naab_only"]):
        lines.append("| %s | %d | %d | %d |" % (c["rule"], c["whole"], c["naab_only"], c["distinct_matches"]))
    lines.append("")
    for c in sorted(report["checks"], key=lambda c: -c["naab_only"]):
        if not c["matches"]:
            continue
        lines.append("**%s** -- most frequent matches in NAAb text:" % c["rule"])
        lines.append("")
        for m in c["matches"]:
            lines.append("- `%s` (%d findings; e.g. %s)" % (m["text"].replace("`", "'"), m["findings"],
                                                         m["example_file"].replace(os.sep, "/")))
        lines.append("")
    return "\n".join(lines)


def cmd_measure(args):
    gov = [os.path.abspath(args.gov)] if not args.gov_argv else json.loads(args.gov_argv)
    roots = [os.path.relpath(os.path.abspath(r)) for r in
             (args.roots or [os.path.join(HERE, "..", "..")])]
    report = measure(roots, gov, jobs=args.jobs, samples=args.samples)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1, sort_keys=True)
    md = render_md(report, args.title)
    if args.md:
        with open(args.md, "w", encoding="utf-8", newline="\n") as f:
            f.write(md + "\n")
    else:
        out(md)
    if report["status"] != "ok":
        for r in report["reasons"]:
            err("naab_lint: UNMEASURABLE: %s" % r)
        return 2
    return 0


def selftest(args):
    gov = [os.path.abspath(args.gov)]
    fails = []

    def expect(name, cond, detail):
        out("  %s [%s] %s" % ("PASS" if cond else "FAIL", name, detail))
        if not cond:
            fails.append(name)

    src = 'main {\n    let r = <<python\nimport os\nos.system("ls")\n>>\n    print(r)\n}\n'
    emptied = empty_block_bodies(src)
    expect("NL-01", "os.system" not in emptied and "<<python" in emptied and "\n>>" in emptied
           and emptied.count("\n") == src.count("\n"),
           "a block's body is emptied; its header, closing line and the line count stay")
    expect("NL-02", empty_block_bodies(PLAIN) == PLAIN, "a file without blocks is unchanged")
    with tempfile.TemporaryDirectory(prefix="naablint-st-") as root:
        files = {"sql.naab": PLANTED[0], "plain.naab": PLAIN,
                 "block.naab": src, "broken.naab": "main { let = \n"}
        for name, text in files.items():
            with open(os.path.join(root, name), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        rep = measure([root], gov, jobs=2)
        by = {c["rule"]: c for c in rep.get("checks", [])}
        sql = by.get("code_quality.no_sql_injection", {})
        inj = by.get("restrictions.code_injection", {})
        expect("NL-03", rep["status"] == "ok" and rep["files"] == 4 and rep["unparsed"] == 1
               and rep["measured"] == 3,
               "four files: three measured, the one lint cannot parse counted and left out")
        expect("NL-04", sql.get("whole") == 1 and sql.get("naab_only") == 1,
               "a flaw in NAAb text counts both ways")
        # The planted SQL line also trips code_injection (a string joined with
        # '+'), so that rule is 2 whole / 1 NAAb-only: the block file's
        # os.system() is the one that counts for the whole file only.
        expect("NL-05", inj.get("whole") == 2 and inj.get("naab_only") == 1,
               "a flaw inside a polyglot block counts for the whole file and not for NAAb text")
        expect("NL-06", rep.get("any_naab_only") == 1 and rep.get("any_whole") == 2,
               "one file flagged in NAAb text, two as they are")
        dead = measure([root], [sys.executable, "-c", "pass"], jobs=1)
        expect("NL-07", dead["status"] == "unmeasurable" and "checks" not in dead,
               "a lint that prints nothing is UNMEASURABLE, with no counts")
        quiet = [sys.executable, "-c", "print('[governance] Summary (mode: audit): 0 passed')"]
        q = measure([root], quiet, jobs=1)
        expect("NL-08", q["status"] == "unmeasurable" and any("planted" in r for r in q["reasons"]),
               "a lint that never flags the planted file is UNMEASURABLE")
    out("")
    out("selftest: %d failed" % len(fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd")
    m = sub.add_parser("measure")
    m.add_argument("roots", nargs="*")
    m.add_argument("--gov", default=os.path.join(HERE, "..", "..", "build", "naab-gov"))
    m.add_argument("--gov-argv", default="", help=argparse.SUPPRESS)
    m.add_argument("--jobs", type=int, default=4)
    m.add_argument("--samples", type=int, default=8)
    m.add_argument("--json", default="")
    m.add_argument("--md", default="")
    m.add_argument("--title", default="The lint-only checks on NAAb code (A132)")
    s = sub.add_parser("selftest")
    s.add_argument("--gov", default=os.path.join(HERE, "..", "..", "build", "naab-gov"))
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 4 if e.code else 0
    if args.cmd == "measure":
        return cmd_measure(args)
    if args.cmd == "selftest":
        return selftest(args)
    ap.print_usage(sys.stderr)
    return 4


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
