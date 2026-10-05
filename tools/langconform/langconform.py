#!/usr/bin/env python3
"""Language conformance harness: what does governance see, per language?

WHY. Per-language knowledge (aliases, comment syntax, string syntax, import
forms) is written out separately at each place that needs it -- 540 language
name comparisons across 21 files on 2026-10-05 -- and the copies disagree.
Measured: a custom pattern inside a `-- comment` is ignored in a <<sql>> block
and reported in a <<sqlite>> block, though both tags run the same executor.
Consolidating that knowledge changes what the checks see, in both directions:
some findings disappear (a comment is finally recognised as a comment) and
some appear (a marker is finally found in a comment). This harness makes every
such change visible so that nothing loosens unnoticed.

HOW. For every registered language name, it places each PAYLOAD in each
syntactic POSITION (active code, every common line- and block-comment form,
every common string form) and asks `naab-gov check` -- the same
checkPolyglotBlock() path `naab-lang` uses, without executing anything --
which rules fired. The config (config.json here) runs in `mode: audit`, so
no finding stops the dispatcher before the later checks run. The result is a
matrix: (language, position, payload) -> sorted rule names.

The positions are deliberately NOT taken from any language's real syntax:
every language gets every position. Whether `-- x` is a comment in Python is
exactly the question, so the harness must not assume the answer.

Commands:
  languages --naab BIN                   registered language names, one per line
  snapshot  --gov BIN --naab BIN --out F full matrix as JSON
  diff A B                               cells whose findings changed; exit 1 if any
  groups F                               alias groups whose members disagree; exit 1 if any

Output is ASCII and LF-only, written as bytes (see CLAUDE.md, "A test's OUTPUT
CHANNEL is part of the instrument").
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(HERE, "config.json")
SCHEMA = 1

# One line each. Every payload triggers at least one check when it is active
# code in some language (checked by the harness's own positive control, see
# test_langconform.sh LC-01).
PAYLOADS = {
    "eval":        "eval(user_input)",
    "system":      "os.system(user_input)",
    "placeholder": "TODO implement this properly",
    "temporary":   "for now just return the default",
    "keyword":     "FORBIDDEN_KEYWORD",
    "import":      "import subprocess",
}

# {P} is the payload. Line comments, block comments and strings from the
# languages NAAb runs or might run; a position that is meaningless for a
# language is still probed, because the governance answer for it is data too.
POSITIONS = {
    "code":           "{P}",
    "line_hash":      "# {P}",
    "line_slashes":   "// {P}",
    "line_dashes":    "-- {P}",
    "line_semicolon": "; {P}",
    "line_percent":   "% {P}",
    "block_c":        "/* {P} */",
    "block_lua":      "--[[ {P} ]]",
    "block_julia":    "#= {P} =#",
    "block_haskell":  "{- {P} -}",
    "block_ruby":     "=begin\n{P}\n=end",
    "block_html":     "<!-- {P} -->",
    "string_double":  "s = \"{P}\"",
    "string_single":  "s = '{P}'",
    "string_back":    "s = `{P}`",
    "string_triple":  "s = \"\"\"{P}\"\"\"",
}

# Names that run the SAME executor class, read from the registrations in
# src/cli/main.cpp (registerExecutor). The registry records no alias relation
# -- each spelling is an independent registration -- so this list is AUTHORED
# from those lines, not discovered. A descriptor table is meant to make it
# data; until then a drifted entry here is a stale claim, not a measurement.
ALIAS_GROUPS = [
    ["sql", "sqlite"],
    ["shell", "sh", "bash"],
    ["javascript", "node"],
    ["go", "golang"],
    ["csharp", "cs"],
    ["typescript", "ts"],
]


def write_out(text):
    sys.stdout.buffer.write(text.encode("ascii", "backslashreplace"))


def registered_languages(naab):
    """Ask the binary. A program in a fresh directory with no govern.json, so
    discovery finds nothing and --no-governance waives the requirement."""
    with tempfile.TemporaryDirectory() as d:
        prog = os.path.join(d, "langs.naab")
        with open(prog, "w", encoding="ascii", errors="strict", newline="\n") as f:
            f.write("use codegen\nmain {\n    for x in codegen.supported_languages() { print(x) }\n}\n")
        p = subprocess.run([naab, prog, "--no-governance"], cwd=d, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = p.stdout.decode("utf-8", "replace").replace("\r", "").split("\n")
    names = sorted({l.strip() for l in out if l.strip() and all(c.isalnum() or c in "+#-_" for c in l.strip())})
    if p.returncode != 0 or not names:
        raise SystemExit("langconform: could not list languages from %s (rc=%d): %s"
                         % (naab, p.returncode, p.stderr.decode("utf-8", "replace")[-300:]))
    return names


def load_config():
    with open(CONFIG, "r", encoding="utf-8", errors="strict") as f:
        text = f.read()
    cfg = json.loads(text)
    if cfg.get("mode") != "audit":
        raise SystemExit("langconform: config.json must run in mode audit, or the "
                         "first finding stops the dispatcher and hides the rest")
    return json.dumps(cfg, sort_keys=True), hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def probe(gov, cfg, lang, code):
    p = subprocess.run([gov, "check", "--language", lang, "--config-string", cfg],
                       input=code.encode("utf-8"), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        doc = json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        return None, "rc=%d, not JSON: %s" % (p.returncode, p.stdout[:200] + p.stderr[-200:])
    return sorted({v["rule"] for v in doc.get("violations", [])}), None


def cmd_languages(a):
    write_out("".join(n + "\n" for n in registered_languages(a.naab)))


def cmd_snapshot(a):
    langs = registered_languages(a.naab)
    cfg, digest = load_config()
    jobs = []
    for lang in langs:
        for pos, tmpl in POSITIONS.items():
            for pay, text in PAYLOADS.items():
                jobs.append((lang, pos, pay, tmpl.replace("{P}", text) + "\n"))
    cells, errors = {}, []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(probe, a.gov, cfg, j[0], j[3]): j for j in jobs}
        for fut in concurrent.futures.as_completed(futs):
            lang, pos, pay, _ = futs[fut]
            rules, err = fut.result()
            if err:
                errors.append("%s|%s|%s: %s" % (lang, pos, pay, err))
            else:
                cells["%s|%s|%s" % (lang, pos, pay)] = rules
    if errors:
        # A probe that did not answer is UNMEASURABLE, never "no findings".
        write_out("langconform: %d probe(s) gave no answer; no snapshot written:\n" % len(errors))
        write_out("".join("  " + e + "\n" for e in sorted(errors)[:20]))
        return 2
    snap = {"schema": SCHEMA, "config_sha256_16": digest, "languages": langs,
            "positions": sorted(POSITIONS), "payloads": sorted(PAYLOADS), "cells": cells}
    data = (json.dumps(snap, sort_keys=True, indent=1) + "\n").encode("ascii")
    with open(a.out, "wb") as f:
        f.write(data)
    write_out("langconform: %d cells, %d languages, %d with a finding -> %s\n"
              % (len(cells), len(langs), sum(1 for v in cells.values() if v), a.out))
    return 0


def load_snap(path):
    with open(path, "rb") as f:
        snap = json.loads(f.read().decode("ascii"))
    if snap.get("schema") != SCHEMA:
        raise SystemExit("langconform: %s has schema %r, expected %d" % (path, snap.get("schema"), SCHEMA))
    return snap


def cmd_diff(a):
    A, B = load_snap(a.a), load_snap(a.b)
    out = []
    if A["config_sha256_16"] != B["config_sha256_16"]:
        out.append("config differs (%s -> %s): cells are not comparable rule-for-rule\n"
                   % (A["config_sha256_16"], B["config_sha256_16"]))
    for name in ("languages", "positions", "payloads"):
        if A[name] != B[name]:
            out.append("%s: removed %s, added %s\n" % (name, sorted(set(A[name]) - set(B[name])),
                                                       sorted(set(B[name]) - set(A[name]))))
    lost = gained = 0
    for key in sorted(set(A["cells"]) | set(B["cells"])):
        a_, b_ = set(A["cells"].get(key, [])), set(B["cells"].get(key, []))
        if a_ != b_:
            for r in sorted(a_ - b_):
                out.append("  - %-45s %s\n" % (key, r)); lost += 1
            for r in sorted(b_ - a_):
                out.append("  + %-45s %s\n" % (key, r)); gained += 1
    out.append("langconform diff: %d finding(s) disappeared (-), %d appeared (+)\n" % (lost, gained))
    write_out("".join(out))
    return 1 if (lost or gained or len(out) > 1) else 0


def cmd_groups(a):
    S = load_snap(a.f)
    out, bad = [], 0
    for group in ALIAS_GROUPS:
        present = [g for g in group if g in S["languages"]]
        if len(present) < 2:
            continue
        for pos in S["positions"]:
            for pay in S["payloads"]:
                rows = {g: tuple(S["cells"].get("%s|%s|%s" % (g, pos, pay), [])) for g in present}
                if len(set(rows.values())) > 1:
                    bad += 1
                    out.append("  %s %s|%s: %s\n" % ("/".join(present), pos, pay,
                               "; ".join("%s=%s" % (g, ",".join(r) or "-") for g, r in rows.items())))
    out.append("langconform groups: %d cell(s) where names for the same executor disagree\n" % bad)
    write_out("".join(out))
    return 1 if bad else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("languages"); s.add_argument("--naab", required=True)
    s = sub.add_parser("snapshot")
    s.add_argument("--gov", required=True); s.add_argument("--naab", required=True)
    s.add_argument("--out", required=True); s.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    s = sub.add_parser("diff"); s.add_argument("a"); s.add_argument("b")
    s = sub.add_parser("groups"); s.add_argument("f")
    a = ap.parse_args(argv)
    # The binaries are run from a scratch directory (so no govern.json is
    # discovered), so a relative path must be resolved against OUR cwd first.
    for attr in ("naab", "gov"):
        if getattr(a, attr, None):
            setattr(a, attr, os.path.abspath(getattr(a, attr)))
    return {"languages": cmd_languages, "snapshot": cmd_snapshot,
            "diff": cmd_diff, "groups": cmd_groups}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
