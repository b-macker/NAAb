#!/usr/bin/env python3
"""Measure what the polyglot block checks catch, against labels other people
made (Bundle 13 PR 2). Report only: nothing here changes a verdict, and the
exit status never depends on a rate.

    govmeasure.py score CASES.jsonl... --gov build/naab-gov [--json OUT] [--md OUT]
    govmeasure.py blocks --out CASES.jsonl      the repository's own blocks, unlabelled
    govmeasure.py selftest

A case is one JSON line: {"id", "source", "language", "category", "bad", "code"}.
`bad` is the label, and it comes from outside this repository: a benchmark's
own bad/good variant (juliet.py), or hidden tests that were run (evalplus.py).
Each case is sent to `GOV check --language LANG --config FILE` on stdin, as
tools/langconform/corpus_diff.py sends the repository's own blocks, under the
same config (tools/langconform/config.json: audit mode, every check on), so
the two reports are comparable. A rule FIRES on a case when it is in the
violations and is not a background rule (one that fires on a block that only
assigns a number: those come from the config, not the code).

Per source, category and rule the report gives: how many bad cases and how
many good cases the rule fired on, each rate with a 95% Wilson interval, and
their difference. A rate is about THIS corpus; precision in particular moves
with the share of bad cases, so the report prints the counts beside it.

Three outcomes, never two. The run is UNMEASURABLE (exit 2) and prints no
rates when the checker cannot be read (the background probe or more than 1%
of cases give no JSON) or when a control fails: a planted block the map says
a rule must flag is not flagged (that rule's "0 caught" would be the config's
doing), or the control that a rule fires on anything at all. Exit 0 means a
report was written; 4 is a usage error.

`--plant shuffle` permutes the labels (seeded) and `--plant dead` replaces
the checker with one that prints nothing: the self-test and the suite use
them to show a broken instrument is reported, not scored.

ASCII output only; bytes written with explicit encodings (CLAUDE.md, "A
test's OUTPUT CHANNEL is part of the instrument").
"""

import argparse
import collections
import concurrent.futures
import json
import math
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONFIG = os.path.join(HERE, "..", "langconform", "config.json")
DEFAULT_MAP = os.path.join(HERE, "check_map.json")
TRIVIAL = {"default": "x = 1\n", "cpp": "int x = 1;\n", "c": "int x = 1;\n",
           "go": "var x = 1\n", "rust": "let x = 1;\n", "shell": "x=1\n",
           "javascript": "var x = 1;\n", "php": "$x = 1;\n"}
UNREADABLE_LIMIT = 0.01


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def err(text):
    sys.stderr.buffer.write((text + "\n").encode("ascii", "replace"))


def wilson(k, n, z=1.96):
    """95% Wilson score interval for k successes of n. (None, None) when n is 0."""
    if n == 0:
        return (None, None)
    p = k / n
    d = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / d
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, centre - half), min(1.0, centre + half))


def config_file(path):
    """The measurement config as a file naab-gov reads with --config, with
    `extends` dropped (as tools/langconform does: a temp file has no parent
    beside it). Returns the temp path; the caller removes it."""
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    if isinstance(doc, dict):
        doc.pop("extends", None)
    fd, tmp = tempfile.mkstemp(prefix="govmeasure-", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return tmp


def check(gov, cfg, language, code, timeout=120):
    """The rules the checker reports for one block, or None when its output
    cannot be read (not JSON, no violations list, a timeout, not startable)."""
    try:
        p = subprocess.run(gov + ["check", "--language", language, "--config", cfg],
                           input=(code + "\n").encode("utf-8"), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    try:
        doc = json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        return None
    if not isinstance(doc, dict) or not isinstance(doc.get("violations"), list):
        return None
    return sorted({str(v.get("rule", "")) for v in doc["violations"] if isinstance(v, dict)})


def load_cases(paths):
    cases = []
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    c = json.loads(line)
                except ValueError:
                    raise SystemExit("govmeasure: %s:%d is not JSON" % (path, n))
                for key in ("id", "source", "language", "category", "bad", "code"):
                    if key not in c:
                        raise SystemExit("govmeasure: %s:%d has no \"%s\"" % (path, n, key))
                if not isinstance(c["bad"], bool):
                    raise SystemExit("govmeasure: %s:%d: \"bad\" must be true or false" % (path, n))
                cases.append(c)
    return cases


def rule_matches(rule, prefixes):
    return any(rule == p or rule.startswith(p) for p in prefixes)


def score(cases, gov, cfg, cmap, jobs=4, plant=None, seed=1, group="both"):
    """Returns (status, report). status is "ok" or "unmeasurable"."""
    report = {"status": "ok", "reasons": [], "cases": len(cases), "plant": plant or "", "group": group}

    def group_key(c):
        # "source": one group per source, every category together (no mapped
        # row: a mapping is per category). "category": one per category,
        # every source together.
        if group == "source":
            return (c["source"], "(every category)")
        if group == "category":
            return ("(every source)", c["category"])
        return (c["source"], c["category"])

    def unmeasurable(reason):
        report["status"] = "unmeasurable"
        report["reasons"].append(reason)

    if plant == "dead":
        gov = [sys.executable, "-c", "pass"]
    if plant == "shuffle":
        rnd = random.Random(seed)
        by_group = collections.defaultdict(list)
        for c in cases:
            by_group[(c["source"], c["category"])].append(c)
        for group in by_group.values():
            labels = [c["bad"] for c in group]
            rnd.shuffle(labels)
            for c, lab in zip(group, labels):
                c["bad"] = lab

    # Background: what fires on a block that only assigns a number.
    languages = sorted({c["language"] for c in cases})
    background = {}
    for lang in languages:
        got = check(gov, cfg, lang, TRIVIAL.get(lang, TRIVIAL["default"]))
        if got is None:
            unmeasurable("the checker gave no readable answer for a trivial %s block" % lang)
            background[lang] = []
        else:
            background[lang] = got
    report["background"] = background
    if report["status"] != "ok":
        return report["status"], report

    # Controls: each planted block must be flagged by the rule the map names.
    controls = []
    for ctl in cmap.get("controls", []):
        if ctl["language"] not in languages:
            continue                      # no case in that language this run
        code = ctl["code"] if isinstance(ctl["code"], str) else "".join(ctl["code"])
        got = check(gov, cfg, ctl["language"], code)
        fired = got is not None and any(rule_matches(r, [ctl["rule"]]) for r in got)
        controls.append({"name": ctl["name"], "rule": ctl["rule"], "fired": fired})
        if not fired:
            unmeasurable("control \"%s\": the checker does not flag the planted block with %s"
                         % (ctl["name"], ctl["rule"]))
    report["controls"] = controls

    def one(c):
        got = check(gov, cfg, c["language"], c["code"])
        if got is None:
            return None
        bg = set(background.get(c["language"], []))
        return [r for r in got if r not in bg]

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        fired = list(ex.map(one, cases))

    unreadable = [c["id"] for c, f in zip(cases, fired) if f is None]
    report["unreadable"] = len(unreadable)
    report["unreadable_examples"] = unreadable[:5]
    if cases and len(unreadable) / len(cases) > UNREADABLE_LIMIT:
        unmeasurable("%d of %d cases gave no readable answer" % (len(unreadable), len(cases)))

    # counts[(source, category)][rule] = [bad_fired, good_fired]; totals beside.
    totals = collections.defaultdict(lambda: [0, 0])
    counts = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    anyrule = collections.defaultdict(lambda: [0, 0])
    for c, f in zip(cases, fired):
        if f is None:
            continue
        key = group_key(c)
        side = 0 if c["bad"] else 1
        totals[key][side] += 1
        if f:
            anyrule[key][side] += 1
        for r in f:
            counts[key][r][side] += 1

    if not any(counts[k] for k in counts) and cases and report["status"] == "ok":
        unmeasurable("no rule fired on any case: the checker or the config is not judging code")

    groups = []
    mapping = cmap.get("categories", {})
    for key in sorted(totals):
        source, category = key
        nbad, ngood = totals[key]
        mapped = mapping.get(category, {}).get("rules", [])
        rows = []
        for rule in sorted(counts[key]):
            b, g = counts[key][rule]
            rows.append(rate_row(rule, b, nbad, g, ngood, rule_matches(rule, mapped)))
        mb = mg = 0
        if mapped:
            for c, f in zip(cases, fired):
                if f is None or group_key(c) != key:
                    continue
                if any(rule_matches(r, mapped) for r in f):
                    if c["bad"]:
                        mb += 1
                    else:
                        mg += 1
        groups.append({
            "source": source, "category": category,
            "note": mapping.get(category, {}).get("note", ""),
            "bad": nbad, "good": ngood,
            "any_rule": rate_row("(any rule)", anyrule[key][0], nbad, anyrule[key][1], ngood, False),
            "mapped_rules": mapped,
            "uncontrolled": [m for m in mapped if not any(
                c["fired"] and rule_matches(c["rule"], [m]) for c in controls)],
            "mapped": rate_row("(any mapped rule)", mb, nbad, mg, ngood, True) if mapped else None,
            "rules": rows,
        })
    report["groups"] = groups
    return report["status"], report


def rate_row(rule, b, nbad, g, ngood, mapped):
    lo_b, hi_b = wilson(b, nbad)
    lo_g, hi_g = wilson(g, ngood)
    return {
        "rule": rule, "mapped": mapped,
        "bad_fired": b, "bad": nbad, "good_fired": g, "good": ngood,
        "caught": (b / nbad) if nbad else None, "caught_ci": [lo_b, hi_b],
        "false_alarm": (g / ngood) if ngood else None, "false_alarm_ci": [lo_g, hi_g],
        "precision": (b / (b + g)) if (b + g) else None,
        "difference": ((b / nbad) - (g / ngood)) if nbad and ngood else None,
    }


def pct(x):
    return "n/a" if x is None else "%.1f%%" % (100 * x)


def ci(pair):
    return "n/a" if pair[0] is None else "%.1f-%.1f%%" % (100 * pair[0], 100 * pair[1])


def render_md(report, title):
    lines = ["# %s" % title, ""]
    if report["status"] != "ok":
        lines += ["**UNMEASURABLE.** No rate below is a finding.", ""]
        lines += ["- %s" % r for r in report["reasons"]]
        lines.append("")
        return "\n".join(lines)
    lines.append("%d cases; %d gave no readable answer and are left out." %
                 (report["cases"], report.get("unreadable", 0)))
    lines.append("")
    lines.append("Controls: " + (", ".join("%s (%s)" % (c["name"], "fired" if c["fired"] else "NOT fired")
                                           for c in report.get("controls", [])) or "none"))
    lines.append("")
    for lang, rules in sorted(report.get("background", {}).items()):
        lines.append("Background rules for %s (fire on a block that only assigns a number; "
                     "not counted): %s" % (lang, ", ".join(rules) or "none"))
    lines.append("")
    for g in report["groups"]:
        lines.append("## %s / %s  (%d bad, %d good)" % (g["source"], g["category"], g["bad"], g["good"]))
        if g["note"]:
            lines.append("")
            lines.append(g["note"])
        lines.append("")
        lines.append("| rule | bad flagged | 95% | good flagged | 95% | difference | precision |")
        lines.append("|---|---|---|---|---|---|---|")
        rows = [g["any_rule"]] + ([g["mapped"]] if g["mapped"] else []) + \
            sorted(g["rules"], key=lambda r: (-(r["bad_fired"] + r["good_fired"]), r["rule"]))
        for r in rows:
            name = r["rule"] + (" *" if r["mapped"] and not r["rule"].startswith("(") else "")
            lines.append("| %s | %d/%d (%s) | %s | %d/%d (%s) | %s | %s | %s |" % (
                name, r["bad_fired"], r["bad"], pct(r["caught"]), ci(r["caught_ci"]),
                r["good_fired"], r["good"], pct(r["false_alarm"]), ci(r["false_alarm_ci"]),
                "n/a" if r["difference"] is None else "%+.1f pts" % (100 * r["difference"]),
                pct(r["precision"])))
        if g["mapped_rules"]:
            lines.append("")
            lines.append("\\* mapped to this category in check_map.json: %s" % ", ".join(g["mapped_rules"]))
            if g.get("uncontrolled"):
                lines.append("")
                lines.append("No planted block shows these fire at all under this config, so a zero "
                             "for them may be the config's doing: %s" % ", ".join(g["uncontrolled"]))
        lines.append("")
    return "\n".join(lines)


def cmd_score(args):
    try:
        with open(args.map, "r", encoding="utf-8") as f:
            cmap = json.load(f)
    except (OSError, ValueError) as e:
        err("govmeasure: cannot read the map %s: %s" % (args.map, e))
        return 4
    try:
        cases = load_cases(args.cases)
    except OSError as e:
        err("govmeasure: cannot read the cases: %s" % e)
        return 4
    except SystemExit as e:
        err(str(e))
        return 4
    if not cases:
        err("govmeasure: no cases")
        return 4
    gov = [os.path.abspath(args.gov)] if not args.gov_argv else json.loads(args.gov_argv)
    cfg = config_file(args.config)
    try:
        status, report = score(cases, gov, cfg, cmap, jobs=args.jobs, plant=args.plant, seed=args.seed,
                               group=args.group)
    finally:
        try:
            os.remove(cfg)
        except OSError:
            pass
    report["title"] = args.title
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1, sort_keys=True)
    md = render_md(report, args.title)
    if args.md:
        with open(args.md, "w", encoding="utf-8", newline="\n") as f:
            f.write(md + "\n")
    else:
        out(md)
    if status != "ok":
        for r in report["reasons"]:
            err("govmeasure: UNMEASURABLE: %s" % r)
        return 2
    return 0


def cmd_blocks(args):
    """The repository's own polyglot blocks as cases (the false-alarm baseline
    the owner kept, 2026-11-10). They carry NO label: many are fixtures written
    to trip a check, so `bad: false` here means "not labelled bad", and the
    category says so. Read the group's rates as how often each rule fires on
    the code that exists, not as false alarms."""
    sys.path.insert(0, os.path.join(HERE, "..", "langconform"))
    import corpus_diff
    n = 0
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for path, index, lang, code in corpus_diff.blocks(args.root):
            f.write(json.dumps({"id": "%s#%d" % (path.replace(os.sep, "/"), index), "source": "repo-blocks",
                                "language": lang, "category": "unlabelled", "bad": False, "code": code}) + "\n")
            n += 1
    out("wrote %d blocks to %s" % (n, args.out))
    return 0 if n else 2


# --- self-test ---------------------------------------------------------------
# A stand-in checker whose behaviour is known exactly, so the arithmetic and
# the three outcomes are checked without naab-gov. It flags "rule.marker" on
# code containing MARK, "rule.always" on everything except the trivial block,
# and "config.bg" on everything (a background rule).
FAKE = r'''
import json, sys
code = sys.stdin.read()
v = [{"rule": "config.bg"}]
if "MARK" in code: v.append({"rule": "rule.marker"})
if "x = 1" != code.strip(): v.append({"rule": "rule.always"})
print(json.dumps({"violations": v}))
'''


def selftest():
    fails = []

    def expect(name, cond, detail=""):
        out("  %s [%s] %s" % ("PASS" if cond else "FAIL", name, detail))
        if not cond:
            fails.append(name)

    lo, hi = wilson(5, 10)
    expect("ST-01", abs(lo - 0.2366) < 0.001 and abs(hi - 0.7634) < 0.001,
           "Wilson 5/10 = %.4f-%.4f (expected 0.2366-0.7634)" % (lo, hi))
    expect("ST-01b", wilson(0, 0) == (None, None), "no cases gives no interval")

    cases = []
    for i in range(40):
        bad = i < 20
        # 15 of 20 bad cases carry the marker, 2 of 20 good ones do.
        marked = (bad and i < 15) or (not bad and i < 22)
        cases.append({"id": "c%d" % i, "source": "fake", "language": "python", "category": "CAT",
                      "bad": bad, "code": ("MARK " if marked else "") + "y = %d" % i})
    gov = [sys.executable, "-c", FAKE]
    cmap = {"categories": {"CAT": {"rules": ["rule.marker"]}},
            "controls": [{"name": "marker", "language": "python", "code": "MARK", "rule": "rule.marker"}]}
    fd, cfg = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    try:
        status, rep = score([dict(c) for c in cases], gov, cfg, cmap)
        g = rep["groups"][0] if rep.get("groups") else None
        m = g["mapped"] if g else None
        expect("ST-02", status == "ok" and m is not None and m["bad_fired"] == 15 and m["good_fired"] == 2
               and abs(m["caught"] - 0.75) < 1e-9 and abs(m["false_alarm"] - 0.10) < 1e-9
               and abs(m["precision"] - 15 / 17) < 1e-9,
               "known detector: 15/20 bad, 2/20 good, precision 15/17")
        always = [r for r in g["rules"] if r["rule"] == "rule.always"][0] if g else None
        expect("ST-03", always is not None and always["bad_fired"] == 20 and always["good_fired"] == 20
               and abs(always["difference"]) < 1e-9 and abs(always["precision"] - 0.5) < 1e-9,
               "a rule that fires on everything: difference 0, precision = the share of bad cases")
        expect("ST-04", g is not None and not any(r["rule"] == "config.bg" for r in g["rules"])
               and rep["background"]["python"] == ["config.bg"],
               "a background rule is listed and not counted")

        status, rep = score([dict(c) for c in cases], gov, cfg, cmap, plant="dead")
        expect("ST-05", status == "unmeasurable" and "groups" not in rep,
               "a checker that prints nothing is UNMEASURABLE, with no rates")

        bad_map = {"categories": {}, "controls": [{"name": "absent", "language": "python",
                                                   "code": "nothing here", "rule": "rule.marker"}]}
        status, rep = score([dict(c) for c in cases], gov, cfg, bad_map)
        expect("ST-06", status == "unmeasurable" and any("absent" in r for r in rep["reasons"]),
               "a planted block the checker does not flag is UNMEASURABLE")

        status, rep = score([dict(c) for c in cases], gov, cfg, cmap, plant="shuffle", seed=3)
        m = rep["groups"][0]["mapped"]
        expect("ST-07", status == "ok" and abs(m["difference"]) < 0.45 and m["bad_fired"] + m["good_fired"] == 17,
               "shuffled labels: the same 17 flags, the difference falls from 65 to %+.0f points"
               % (100 * m["difference"]))

        flaky = [sys.executable, "-c",
                 "import sys\ncode=sys.stdin.read()\n"
                 "print('' if 'y = 3' in code else '{\"violations\": [{\"rule\": \"rule.always\"}]}')"]
        status, rep = score([dict(c) for c in cases], flaky, cfg, {"categories": {}, "controls": []})
        expect("ST-08", status == "unmeasurable" and rep["unreadable"] == 11,
               "11 of 40 unreadable answers (over 1%) is UNMEASURABLE, and counted")
        two = [dict(c, source="fake" if i % 2 else "other") for i, c in enumerate(cases)]
        status, rep = score([dict(c) for c in two], gov, cfg, cmap, group="category")
        g = rep["groups"]
        status2, rep2 = score([dict(c) for c in two], gov, cfg, cmap, group="source")
        expect("ST-10", len(g) == 1 and g[0]["source"] == "(every source)" and g[0]["mapped"]["bad_fired"] == 15
               and len(rep2["groups"]) == 2 and all(x["mapped"] is None for x in rep2["groups"])
               and sum(x["any_rule"]["bad"] for x in rep2["groups"]) == 20,
               "grouped by category the sources merge (15 of 20 again); by source there are two tables "
               "and no mapped row")
        silent = [sys.executable, "-c", "print('{\"violations\": []}')"]
        status, rep = score([dict(c) for c in cases], silent, cfg, {"categories": {}, "controls": []})
        expect("ST-09", status == "unmeasurable",
               "a checker that never flags anything is UNMEASURABLE, not a clean corpus")
    finally:
        os.remove(cfg)
    out("")
    out("selftest: %d failed" % len(fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser(description="Measure what the block checks catch (report only).")
    sub = ap.add_subparsers(dest="cmd")
    sc = sub.add_parser("score")
    sc.add_argument("cases", nargs="+")
    sc.add_argument("--gov", default=os.path.join(HERE, "..", "..", "build", "naab-gov"))
    sc.add_argument("--gov-argv", default="", help=argparse.SUPPRESS)
    sc.add_argument("--config", default=DEFAULT_CONFIG)
    sc.add_argument("--map", default=DEFAULT_MAP)
    sc.add_argument("--jobs", type=int, default=4)
    sc.add_argument("--json", default="")
    sc.add_argument("--md", default="")
    sc.add_argument("--title", default="What the block checks catch")
    sc.add_argument("--plant", choices=["dead", "shuffle"], default=None)
    sc.add_argument("--seed", type=int, default=1)
    sc.add_argument("--group", choices=["both", "source", "category"], default="both",
                    help="one table per source and category (default), per source, or per category")
    bl = sub.add_parser("blocks")
    bl.add_argument("--root", default=os.path.join(HERE, "..", ".."))
    bl.add_argument("--out", required=True)
    sub.add_parser("selftest")
    try:
        args = ap.parse_args(argv)
    except SystemExit as e:
        return 4 if e.code else 0
    if args.cmd == "score":
        return cmd_score(args)
    if args.cmd == "blocks":
        return cmd_blocks(args)
    if args.cmd == "selftest":
        return selftest()
    ap.print_usage(sys.stderr)
    return 4


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
