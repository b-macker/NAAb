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
  verify    --gov BIN                    prove the table's comment syntax with each
                                         language's own installed toolchain; exit 1
                                         on a declared form the toolchain rejects
  conform   --gov BIN --naab BIN         probes generated from the binary's language
                                         table; exit 1 on a registered name with no
                                         entry or a declared comment form not honoured

Output is ASCII and LF-only, written as bytes (see CLAUDE.md, "A test's OUTPUT
CHANNEL is part of the instrument").
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(HERE, "config.json")
SCHEMA = 2

# One line each. Every payload triggers at least one check when it is active
# code in some language (checked by the harness's own positive control, see
# test_langconform.sh LC-01).
PAYLOADS = {
    "eval":        "eval(user_input)",
    "system":      "os.system(user_input)",
    "placeholder": "TODO implement this properly",
    "temporary":   "for now just return the default",
    "oversimplified": "simplified version of the algorithm",
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



def language_table(gov):
    """The binary's own language table (naab-gov languages): canonical names,
    aliases and comment syntax. Alias groups and the conform probes come from
    here, so this file holds no list of languages of its own."""
    p = subprocess.run([gov, "languages"], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        table = json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        table = None
    if p.returncode != 0 or not isinstance(table, list) or not table:
        raise SystemExit("langconform: %s languages gave no table (rc=%d): %s"
                         % (gov, p.returncode, p.stderr.decode("utf-8", "replace")[-300:]))
    return table


def resolve(table, name):
    n = name.lower()
    for d in table:
        if d["canonical"] == n or n in d["aliases"]:
            return d
    return None


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
    table = language_table(a.gov)
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
    snap = {"schema": SCHEMA, "config_sha256_16": digest, "languages": langs, "table": table,
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
    if A["table"] != B["table"]:
        out.append("language table changed (naab/language_descriptors.h): %s\n"
                   % ", ".join(sorted({d["canonical"] for d in A["table"] if d not in B["table"]}
                                      | {d["canonical"] for d in B["table"] if d not in A["table"]})))
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
    for d in S["table"]:
        present = [n for n in [d["canonical"]] + d["aliases"] if n in S["languages"]]
        if len(present) < 2:
            continue
        for pos in S["positions"]:
            for pay in S["payloads"]:
                rows = {g: tuple(S["cells"].get("%s|%s|%s" % (g, pos, pay), [])) for g in present}
                if len(set(rows.values())) > 1:
                    bad += 1
                    out.append("  %s %s|%s: %s\n" % ("/".join(present), pos, pay,
                               "; ".join("%s=%s" % (g, ",".join(r) or "-") for g, r in rows.items())))
    out.append("langconform groups: %d cell(s) where names for the same language disagree\n" % bad)
    write_out("".join(out))
    return 1 if bad else 0


KEYWORD_RULE = "code_quality.no_hallucinated_apis"
MARKER_RULE = "code_quality.no_temporary_code"
MARKER = "for now just return the default"


def cmd_conform(a):
    """Probes generated from the language table itself.

    1. Every name the binary registers resolves to a table entry. A language
       registered without one is a FAILURE: governance would know nothing
       about its comments, and nobody would have decided that.
    2. For every name, the keyword as active code is reported (the control),
       and the keyword inside EACH comment form the entry declares is not:
       comments are hidden from the checks that read code.
    3. A temporary-code marker inside each declared comment form -- on the
       opener's line and on an interior line of a block -- IS reported:
       comments are visible to the checks that read comments.
    Names probed: every registered name, plus every name in a governance-only
    entry (no executor, but naab-gov check and the C API accept it). Adding a
    language or a comment form to the table adds its probes here with no edit
    to this file or to any test."""
    langs = registered_languages(a.naab)
    if a.table:
        # Test controls only: a planted table in place of the binary's own.
        with open(a.table, "r", encoding="utf-8", errors="strict") as f:
            table = json.load(f)
    else:
        table = language_table(a.gov)
    cfg, _ = load_config()
    out, bad = [], 0
    probes = []
    gov_only = [n for d in table if d.get("governance_only")
                for n in [d["canonical"]] + d["aliases"] if n not in langs]
    for name in langs + gov_only:
        d = resolve(table, name)
        if d is None:
            bad += 1
            out.append("  FAIL %s: registered, but the language table has no entry for it\n" % name)
            continue
        if d.get("governance_only") and name in langs:
            bad += 1
            out.append("  FAIL %s: registered, but its entry says no executor runs it\n" % name)
        probes.append((name, "code", "FORBIDDEN_KEYWORD\n", True))
        for m in d["line_comments"]:
            probes.append((name, "line %s" % m, "%s FORBIDDEN_KEYWORD\n" % m, False))
            probes.append((name, "marker in line %s" % m, "%s %s\n" % (m, MARKER), True, MARKER_RULE))
        for b in d["block_comments"]:
            probes.append((name, "block %s %s" % (b["open"], b["close"]),
                           "%s\nFORBIDDEN_KEYWORD\n%s\n" % (b["open"], b["close"]), False))
            probes.append((name, "marker in block %s %s" % (b["open"], b["close"]),
                           "%s %s\n%s\n" % (b["open"], MARKER, b["close"]), True, MARKER_RULE))
            probes.append((name, "marker inside block %s %s" % (b["open"], b["close"]),
                           "%s\n%s\n%s\n" % (b["open"], MARKER, b["close"]), True, MARKER_RULE))
            if b["line_start_only"]:
                # The control for line_start_only: mid-line, it is NOT a comment.
                probes.append((name, "mid-line %s" % b["open"],
                               "x = 1 %s FORBIDDEN_KEYWORD %s\n" % (b["open"], b["close"]), True))
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        res = list(ex.map(lambda p: probe(a.gov, cfg, p[0], p[2]), probes))
    for p, (rules, err) in zip(probes, res):
        name, what, _, expect = p[:4]
        rule = p[4] if len(p) > 4 else KEYWORD_RULE
        if err:
            bad += 1
            out.append("  UNMEASURABLE %s %s: %s\n" % (name, what, err))
            continue
        got = rule in rules
        if got != expect:
            bad += 1
            out.append("  FAIL %s %s: %s %s, expected %s\n"
                       % (name, what, rule, "reported" if got else "not reported",
                          "reported" if expect else "not reported (it is a comment)"))
    registered = {resolve(table, n)["canonical"] for n in langs if resolve(table, n)}
    unused = sorted(d["canonical"] for d in table
                    if not d.get("governance_only") and d["canonical"] not in registered)
    if unused:
        out.append("  info: table entries no executor registers here: %s\n" % ", ".join(unused))
    out.append("langconform conform: %d probe(s) over %d registered + %d governance-only name(s), %d problem(s)\n"
               % (len(probes), len(langs), len(gov_only), bad))
    write_out("".join(out))
    return 1 if bad else 0


# --- verify: prove the table's facts with each language's OWN toolchain -----

# Text that is a syntax error in every language the table describes. The
# quotes and closing braces make any reading as a string literal fail too (an
# unterminated quote, or a brace-delimited literal such as Ruby's %{...}
# closing early), so a form that merely STARTS a string cannot pass for a
# comment. It must contain no comment CLOSER the table declares (*/ ]] =# -}
# =end --> ]#), or a real block would end early and read as WRONG.
JUNK = ")))((( }}} \" ' ` @@ ;;"

# Candidate comment forms, tried against every language whose toolchain is
# installed. Deliberately NO quote-based forms: where a bare string literal is
# a valid statement (Python's triple quotes), "the parser accepts it" cannot
# tell a string from a comment, and string forms are their own probe class.
LINE_CANDIDATES = ["#", "//", "--", ";", "%", "'", "!", "REM"]
BLOCK_CANDIDATES = [("/*", "*/"), ("--[[", "]]"), ("#=", "=#"), ("{-", "-}"),
                    ("=begin", "=end"), ("<!--", "-->"), ("#[", "]#"), ("(*", "*)"),
                    ("%{", "%}"), ("#|", "|#")]


def pick_check(entry):
    for argv in entry.get("syntax_checks", []):
        if argv and shutil.which(argv[0]):
            return argv
    return None


def parses(entry, argv, code):
    """True when the language's own tool accepts `code` (prelude prepended)."""
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "probe" + entry["extension"])
        with open(f, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(entry.get("prelude", "") + code)
        cmd = [a.replace("{file}", f).replace("{dir}", d) for a in argv]
        try:
            p = subprocess.run(cmd, cwd=d, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        except subprocess.TimeoutExpired:
            return None
        return p.returncode == 0


def line_is_comment(entry, argv, m):
    # Hides junk to the end of the line, and ENDS at the newline.
    return parses(entry, argv, "%s %s\n" % (m, JUNK)) and \
        parses(entry, argv, "%s %s\n%s\n" % (m, JUNK, JUNK)) is False


def block_is_comment(entry, argv, o, c):
    # Hides junk across lines, and ENDS at its closer.
    return parses(entry, argv, "%s %s\n%s\n%s\n" % (o, JUNK, JUNK, c)) and \
        parses(entry, argv, "%s %s %s\n%s\n" % (o, JUNK, c, JUNK)) is False


def verify_entry(entry):
    """One language: confirm every declared form, find undeclared ones."""
    r = {"canonical": entry["canonical"], "tool": None, "status": "ok",
         "confirmed": [], "wrong": [], "proposed": []}
    argv = pick_check(entry)
    if argv is None:
        r["status"] = "UNMEASURABLE: no syntax-check tool installed (%s)" % (
            ", ".join(a[0] for a in entry.get("syntax_checks", [])) or "none listed")
        return r
    r["tool"] = argv[0]
    # Controls: an empty file must parse and the junk must not, or the tool
    # is not answering the question asked.
    empty, junk = parses(entry, argv, "\n"), parses(entry, argv, JUNK + "\n")
    if empty is not True or junk is not False:
        r["status"] = "UNMEASURABLE: %s control failed (empty file %s, junk %s)" % (
            argv[0], "accepted" if empty else "REJECTED", "ACCEPTED" if junk else "rejected")
        return r
    declared_line = set(entry["line_comments"])
    declared_block = {(b["open"], b["close"]) for b in entry["block_comments"]}
    for m in sorted(declared_line | set(LINE_CANDIDATES)):
        ok = line_is_comment(entry, argv, m)
        if m in declared_line:
            (r["confirmed"] if ok else r["wrong"]).append("line %s" % m)
        elif ok:
            r["proposed"].append("line %s" % m)
    for o, c in sorted(declared_block | set(BLOCK_CANDIDATES)):
        ok = block_is_comment(entry, argv, o, c)
        if (o, c) in declared_block:
            (r["confirmed"] if ok else r["wrong"]).append("block %s %s" % (o, c))
        elif ok:
            r["proposed"].append("block %s %s" % (o, c))
    return r


def cmd_verify(a):
    """Prove the table's comment syntax with each language's own toolchain.

    WRONG (exit 1): the table declares a form the language's own parser
    rejects -- the dangerous direction: governance would treat live code as a
    comment and hide it from every check.
    PROPOSED (report only): the parser accepts a form the table does not
    declare. That errs strict for the checks that read code, but hides markers
    from the checks that read comments; it becomes a table edit only after
    review, and the conformance baseline shows its effect.
    UNMEASURABLE: the toolchain is not installed, or its controls failed."""
    if a.table:
        with open(a.table, "r", encoding="utf-8", errors="strict") as f:
            table = json.load(f)
    else:
        table = language_table(a.gov)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        results = list(ex.map(verify_entry, table))
    out, wrong = [], 0
    for r in results:
        if r["status"] != "ok":
            out.append("  %-11s %s\n" % (r["canonical"], r["status"]))
            continue
        out.append("  %-11s [%s] confirmed: %s\n" % (r["canonical"], r["tool"],
                   ", ".join(r["confirmed"]) or "-"))
        for w in r["wrong"]:
            wrong += 1
            out.append("  %-11s WRONG: %s is declared, but %s rejects it\n" % ("", w, r["tool"]))
        for pr in r["proposed"]:
            out.append("  %-11s proposed: %s (accepted by %s, not declared)\n" % ("", pr, r["tool"]))
    measured = sum(1 for r in results if r["status"] == "ok")
    out.append("langconform verify: %d of %d languages measured, %d wrong fact(s), %d proposal(s)\n"
               % (measured, len(results), wrong, sum(len(r["proposed"]) for r in results)))
    write_out("".join(out))
    return 1 if wrong else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("languages"); s.add_argument("--naab", required=True)
    s = sub.add_parser("snapshot")
    s.add_argument("--gov", required=True); s.add_argument("--naab", required=True)
    s.add_argument("--out", required=True); s.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    s = sub.add_parser("diff"); s.add_argument("a"); s.add_argument("b")
    s = sub.add_parser("groups"); s.add_argument("f")
    s = sub.add_parser("conform")
    s.add_argument("--gov", required=True); s.add_argument("--naab", required=True)
    s.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    s.add_argument("--table", help="a planted language table instead of the binary's (test controls)")
    s = sub.add_parser("verify")
    s.add_argument("--gov", required=True)
    s.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    s.add_argument("--table", help="a planted language table instead of the binary's (test controls)")
    a = ap.parse_args(argv)
    # The binaries are run from a scratch directory (so no govern.json is
    # discovered), so a relative path must be resolved against OUR cwd first.
    for attr in ("naab", "gov"):
        if getattr(a, attr, None):
            setattr(a, attr, os.path.abspath(getattr(a, attr)))
    return {"languages": cmd_languages, "snapshot": cmd_snapshot,
            "diff": cmd_diff, "groups": cmd_groups, "conform": cmd_conform, "verify": cmd_verify}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
