#!/usr/bin/env python3
"""Cross-check NAAb's two text engines on the same inputs (report only).

NAAb has two engines that judge source text with regexes:
  - the polyglot checker (checkPolyglotBlock, governance_checks.cpp), which
    enforces on every polyglot block at runtime -- reached here through
    `naab-gov check`, nothing executed;
  - the C++ scanner (src/scanner, naab-q), which reports on files -- reached
    through `naab-gov scan`, and run as an advisory preflight on .naab files.
Both implement shell injection, deserialization, weak crypto, hard-coded
addresses and more, as separate pattern sets. This measures where they
disagree, before anyone merges them.

ATTRIBUTION WITHOUT A RULE-NAME MAP. The engines name rules differently, and
a hand-written map would be an authored claim. Instead every dangerous
snippet has a BENIGN TWIN of the same shape (eval(user_input) vs
len(user_input)); a rule counts for a concept only if it fires on the snippet
and NOT on its twin, at the same position. So "mutable global state" and
other shape-driven findings cancel out.

POSITIONS. code (should fire), inside the language's own line comment and
inside a plain double-quoted string. In a comment or string a firing is a
false positive -- except for concepts that LIVE in strings (addresses,
credentials, SQL text), whose string position is their real position.

CONFIGS (stated, because they differ): the scanner runs with its default
config (no govern.json in the scratch dir, which is what a user without one
gets); the checker runs with tools/langconform/config.json (the template in
audit mode). A disagreement can therefore be configuration, not engine --
the report says which engine fired, and that is all it claims.

Usage: crosscheck.py --gov build/naab-gov [--json OUT]
"""

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from langconform import config_path  # noqa: E402  (--config, never --config-string: see there)

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(HERE, "config.json")

# Languages both engines claim to know, with the scanner's file extension
# and the line-comment form the language actually uses.
LANGS = {
    "python":     (".py",  "#"),
    "javascript": (".js",  "//"),
    "go":         (".go",  "//"),
    "rust":       (".rs",  "//"),
    "cpp":        (".cpp", "//"),
}

# concept -> {language: (dangerous, benign twin)}. One line each.
CONCEPTS = {
    "code_eval": {
        "python": ("x = eval(user_input)", "x = len(user_input)"),
        "javascript": ("let x = eval(userInput);", "let x = String(userInput);"),
    },
    "shell_exec": {
        "python": ("os.system(cmd)", "os.getcwd(cmd)"),
        "javascript": ("require('child_process').exec(cmd);", "require('path').join(cmd);"),
        "go": ("exec.Command(\"sh\", \"-c\", cmd).Run()", "strings.Fields(\"sh\", \"-c\", cmd).Len()"),
        "rust": ("std::process::Command::new(cmd).spawn();", "std::string::String::new(cmd).len();"),
        "cpp": ("system(cmd);", "strlen(cmd);"),
    },
    "deserialization": {
        "python": ("obj = pickle.loads(data)", "obj = json.loads(data)"),
        "javascript": ("obj = unserialize(data);", "obj = JSON.parse(data);"),
    },
    "weak_crypto": {
        "python": ("h = hashlib.md5(data)", "h = hashlib.sha256(data)"),
        "javascript": ("h = crypto.createHash('md5');", "h = crypto.createHash('sha256');"),
        "go": ("h := md5.Sum(data)", "h := sha256.Sum256(data)"),
    },
    "insecure_random": {
        "python": ("token = random.random()", "token = secrets.token_hex()"),
        "javascript": ("token = Math.random();", "token = crypto.randomUUID();"),
    },
    "path_traversal": {
        "python": ("f = open(base + '/../../etc/passwd')", "f = open(base + '/data/report.txt')"),
        "javascript": ("fs.readFileSync(base + '/../../etc/passwd');", "fs.readFileSync(base + '/data/report.txt');"),
    },
    "hardcoded_ip": {
        "python": ("host = '10.20.30.40'", "host = 'example-host'"),
        "javascript": ("const host = '10.20.30.40';", "const host = 'example-host';"),
    },
    "hardcoded_credential": {
        "python": ("password = 'hunter2hunter2'", "username = 'hunter2hunter2'"),
        "javascript": ("const password = 'hunter2hunter2';", "const username = 'hunter2hunter2';"),
    },
    "sql_concat": {
        "python": ("q = \"SELECT * FROM users WHERE id=\" + uid", "q = \"users listing for \" + uid"),
        "javascript": ("q = 'SELECT * FROM users WHERE id=' + uid;", "q = 'users listing for ' + uid;"),
    },
}
STRING_BORNE = {"hardcoded_ip", "hardcoded_credential", "sql_concat", "path_traversal"}


def place(position, comment, text):
    if position == "code":
        return text
    if position == "comment":
        return "%s %s" % (comment, text)
    # A plain string: the snippet's own quotes are swapped so it stays one literal.
    return 's = "%s"' % text.replace('"', "'")


def scanner_rules(gov, lang, ext, code):
    with tempfile.TemporaryDirectory() as d:
        f = os.path.join(d, "probe" + ext)
        with open(f, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(code + "\n")
        p = subprocess.run([gov, "scan", f, "--language", lang], cwd=d,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        rep = os.path.join(d, "quality-report.json")
        if not os.path.exists(rep):
            return None
        with open(rep, "r", encoding="utf-8", errors="replace") as fh:
            doc = json.load(fh)
    return {"%s.%s" % (i["category"], i["rule"]) for i in doc.get("issues", [])}


def checker_rules(gov, cfg, lang, code):
    p = subprocess.run([gov, "check", "--language", lang, "--config", config_path(cfg)],
                       input=(code + "\n").encode("utf-8"), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        doc = json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        return None
    return {v["rule"] for v in doc.get("violations", [])} - {"languages.allowed"}


def attributed(engine, gov, cfg, lang, ext, comment, position, pair):
    bad, good = (place(position, comment, s) for s in pair)
    if engine == "scanner":
        a, b = scanner_rules(gov, lang, ext, bad), scanner_rules(gov, lang, ext, good)
    else:
        a, b = checker_rules(gov, cfg, lang, bad), checker_rules(gov, cfg, lang, good)
    if a is None or b is None:
        return None
    return sorted(a - b)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--gov", required=True)
    ap.add_argument("--json")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    a = ap.parse_args(argv)
    gov = os.path.abspath(a.gov)
    with open(CONFIG, "r", encoding="utf-8", errors="strict") as f:
        cfg = json.dumps(json.load(f))

    jobs = []
    for concept, per in CONCEPTS.items():
        for lang, pair in per.items():
            ext, comment = LANGS[lang]
            for position in ("code", "comment", "string"):
                for engine in ("scanner", "checker"):
                    jobs.append((concept, lang, position, engine, ext, comment, pair))
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        res = list(ex.map(lambda j: attributed(j[3], gov, cfg, j[1], j[4], j[5], j[2], j[6]), jobs))

    cells = {}
    unmeasured = 0
    for j, r in zip(jobs, res):
        if r is None:
            unmeasured += 1
        cells.setdefault((j[0], j[1], j[2]), {})[j[3]] = r

    out = []
    tally = {"both": 0, "scanner_only": 0, "checker_only": 0, "neither": 0}
    out.append("concept / language / position: scanner | checker\n")
    for (concept, lang, position), e in sorted(cells.items()):
        s, c = e.get("scanner"), e.get("checker")
        expect = position == "code" or (position == "string" and concept in STRING_BORNE)
        if expect and s is not None and c is not None:
            k = "both" if (s and c) else "scanner_only" if s else "checker_only" if c else "neither"
            tally[k] += 1
        flag = "" if expect else ("   <- FALSE POSITIVE" if (s or c) else "")
        out.append("  %-20s %-10s %-7s scanner=%s | checker=%s%s\n" % (
            concept, lang, position,
            "UNMEASURED" if s is None else (",".join(s) or "-"),
            "UNMEASURED" if c is None else (",".join(c) or "-"), flag))
    out.append("\nWhere the construct is real (code, or a string for string-borne concepts):\n")
    out.append("  both engines fire: %(both)d   scanner only: %(scanner_only)d   "
               "checker only: %(checker_only)d   neither: %(neither)d\n" % tally)
    if unmeasured:
        out.append("  %d probe(s) gave no answer (UNMEASURED, not counted)\n" % unmeasured)
    sys.stdout.buffer.write("".join(out).encode("ascii", "backslashreplace"))
    if a.json:
        with open(a.json, "wb") as f:
            f.write((json.dumps({"%s|%s|%s" % k: v for k, v in cells.items()},
                                sort_keys=True, indent=1) + "\n").encode("ascii"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
