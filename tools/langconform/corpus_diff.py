#!/usr/bin/env python3
"""Blast radius on REAL code: every polyglot block in the repository, judged
by two naab-gov binaries (e.g. master and a branch), findings diffed.

tools/langconform measures synthetic probes someone chose; this measures the
code that exists. Each block found in a .naab file (`<<lang[...]` up to a
line whose first non-blank text is `>>`) is sent to `naab-gov check
--language lang` under tools/langconform/config.json (audit mode, so every
check runs) by both binaries. Output: per-block rule sets that differ, and a
tally by rule and language. Report only.

Usage: corpus_diff.py --old OLD/naab-gov --new NEW/naab-gov [--root .] [--json OUT]
"""

import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP_DIRS = {"build", "external", "test-parallel", "test-timing", ".git", "node_modules"}
BLOCK_RE = re.compile(r"<<([A-Za-z0-9_+#-]+)(?:\[[^\]\n]*\])?[ \t]*\n(.*?)\n[ \t]*>>", re.S)


def blocks(root):
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in SKIP_DIRS and not d.startswith("build")]
        for fn in fns:
            if not fn.endswith(".naab"):
                continue
            p = os.path.join(dp, fn)
            try:
                with open(p, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for i, m in enumerate(BLOCK_RE.finditer(text)):
                yield os.path.relpath(p, root), i, m.group(1), m.group(2)


def rules(gov, cfg, lang, code):
    p = subprocess.run([gov, "check", "--language", lang, "--config-string", cfg],
                       input=(code + "\n").encode("utf-8"), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=120)
    try:
        doc = json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        return None
    return sorted(v["rule"] for v in doc.get("violations", []))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--root", default=".")
    ap.add_argument("--json")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    a = ap.parse_args(argv)
    old, new = os.path.abspath(a.old), os.path.abspath(a.new)
    with open(os.path.join(HERE, "config.json"), "r", encoding="utf-8", errors="strict") as f:
        cfg = json.dumps(json.load(f))
    items = list(blocks(a.root))

    def judge(it):
        _, _, lang, code = it
        return rules(old, cfg, lang, code), rules(new, cfg, lang, code)

    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        res = list(ex.map(judge, items))

    changed, unmeasured = [], 0
    gained, lost = collections.Counter(), collections.Counter()
    langs = collections.Counter(it[2] for it in items)
    for it, (o, n) in zip(items, res):
        if o is None or n is None:
            unmeasured += 1
            continue
        oc, nc = collections.Counter(o), collections.Counter(n)
        if oc != nc:
            plus, minus = nc - oc, oc - nc
            changed.append((it, sorted(plus.elements()), sorted(minus.elements())))
            for r in plus.elements():
                gained[(r, it[2])] += 1
            for r in minus.elements():
                lost[(r, it[2])] += 1

    out = ["corpus: %d polyglot blocks in %d files; languages: %s\n" % (
        len(items), len({i[0] for i in items}),
        ", ".join("%s %d" % kv for kv in langs.most_common()))]
    out.append("blocks whose findings changed: %d   unmeasured: %d\n" % (len(changed), unmeasured))
    out.append("\nfindings LOST (old fired, new does not), by rule and language:\n")
    for (r, l), n in sorted(lost.items(), key=lambda kv: -kv[1]):
        out.append("  -%-4d %-45s %s\n" % (n, r, l))
    out.append("\nfindings GAINED, by rule and language:\n")
    for (r, l), n in sorted(gained.items(), key=lambda kv: -kv[1]):
        out.append("  +%-4d %-45s %s\n" % (n, r, l))
    out.append("\nchanged blocks:\n")
    for (path, idx, lang, _), plus, minus in changed:
        out.append("  %s#%d <<%s>>  +%s  -%s\n" % (path, idx, lang, ",".join(plus) or "", ",".join(minus) or ""))
    sys.stdout.buffer.write("".join(out).encode("ascii", "backslashreplace"))
    if a.json:
        with open(a.json, "wb") as f:
            f.write(json.dumps([{"file": p, "block": i, "lang": l, "gained": pl, "lost": mi}
                                for (p, i, l, _), pl, mi in changed], indent=1).encode("ascii"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
