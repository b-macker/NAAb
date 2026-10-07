#!/usr/bin/env python3
"""writeup_lint.py -- flag places in a write-up to re-read against
docs/investigation-method.md before it is published.

  "The rules get broken in the write-up before they get broken in the
   reasoning" -- "a green suite is not evidence of correctness" was the
   correctness argument in all seven PR bodies of the campaign after it.

Usage:
  writeup_lint.py [--strict] FILE...      (or - for stdin)

TIER: SCREENED. These are regexes. Expect false positives (a flag is a place
to re-read, not a finding) and expect misses (anything phrased differently
passes silently -- an empty report does NOT mean the write-up obeys the
rules). Rules are tuned to MISS rather than invent: a line that already
carries the evidence a rule asks for is not flagged.

Skipped: fenced code blocks and table rows (narrative is where fabrication
concentrates -- "Fabrication risk is highest in narrative").

Exit: 0, or 1 under --strict when anything is flagged. 2 when a file cannot
be read. Output is ASCII only.
"""
import re
import sys

# (rule id, method heading to re-read, pattern, suppress-if-line-matches)
LINE_RULES = [
    ("green-suite",
     "A green suite is not evidence of correctness",
     re.compile(r"\b(green suite|suite (?:is |stays |stayed |was )?green|"
                r"(?:all |the )?(?:tests?|suites?|checks?|ci)\s+(?:all\s+)?(?:pass(?:es|ed|ing)?|green))\b", re.I),
     re.compile(r"\b(not evidence|floor|reverted|made red|fails? (?:without|when))\b", re.I)),
    ("action-claimed",
     "Report what an action DID, not what you instructed",
     re.compile(r"\b(pushed|merged|committed|deployed|published)\b", re.I),
     re.compile(r"\b(rev-parse|ls-remote|read back|verified|confirmed on|git log origin|shows? (?:the )?sha)\b", re.I)),
    ("single-cause",
     "n=1 is not causation, least of all when the fix worked",
     re.compile(r"\b(root cause|confirmed cause|the cause (?:is|was)|caused by|was the cause)\b", re.I),
     re.compile(r"\b(one sample|single sample|n\s*=\s*1|not re-?tested|hypothes\w+|reproduc\w+|captured)\b", re.I)),
    ("absence-claim",
     "Before claiming something is inert (Checklist)",
     re.compile(r"\b(?:is|are|was|were)\s+(?:inert|dead|unused|unreachable|never\s+(?:read|called|used|consumed|reached|loaded))\b|"
                r"\bdoes nothing\b|\bnothing\s+(?:reads|calls|loads|uses|consumes)\b", re.I),
     re.compile(r"\b(traced|verified|positive control|screened)\b", re.I)),
    ("fix-claim",
     "Verify a fix against the symptom, not the patch site",
     re.compile(r"\b(?:is|now|was)\s+fixed\b|\bfix(?:es|ed)\s+the\b", re.I),
     re.compile(r"\b(reproduc\w+|revert\w*|fails? (?:without|before)|red\b|symptom)\b", re.I)),
    ("confidence-word",
     "Confidence is not a signal, and may run backwards",
     re.compile(r"\b(clearly|obviously|definitely|certainly|undoubtedly|conclusively)\b", re.I),
     None),
    ("done-claim",
     "Check \"done\" against the plan, not your memory of it",
     re.compile(r"\b(?:is|are|now|all|feature)\s+(?:done|complete|finished|shipped)\b", re.I),
     re.compile(r"\b(plan|item|artifact|grep)\w*\b", re.I)),
]

# A figure in narrative: a percentage, K/N, or "K of N".
FIGURE = re.compile(r"\b\d+(?:\.\d+)?\s?%|\b\d+\s*/\s*\d+\b|\b\d+ of (?:about |roughly |~)?\d+\b")
PROVENANCE = re.compile(r"\b(observed|configured|authored|measured|computed|counted|screened|traced|"
                        r"verified|estimated?|from |per |source)\b", re.I)

# Document-level: things the write-up checklist asks for that should appear SOMEWHERE.
DOC_RULES = [
    ("no-blind-spots", "Unmeasurable is not absent",
     re.compile(r"\b(unmeasurable|undetermined|blind spot|not measured|unmeasured|could not measure|can'?t measure)\b", re.I)),
    ("no-error-direction", "Say which direction an error would fall",
     re.compile(r"\b(direction|false alarm|false reassurance|erring|err(?:s|ed)? (?:on|toward))\b", re.I)),
    ("no-retrace-count", "Stopping rule (record the number of independent re-tracings)",
     re.compile(r"\bre-?trac\w*", re.I)),
    ("no-adversarial-pass", "Schedule the doubt -- it will not arrive on its own",
     re.compile(r"\b(adversarial|most likely (?:to be )?wrong|what would (?:show|falsify|disprove)|falsif\w+)\b", re.I)),
]


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def read(path):
    if path == "-":
        return sys.stdin.buffer.read().decode("utf-8", errors="replace")
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def lint(text):
    flags = []
    in_fence = False
    for no, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if s.startswith("```") or s.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence or s.startswith("|"):
            continue
        for rid, heading, pat, suppress in LINE_RULES:
            if pat.search(line) and not (suppress and suppress.search(line)):
                flags.append((no, rid, heading, s))
        if FIGURE.search(line) and not PROVENANCE.search(line):
            flags.append((no, "unlabelled-figure",
                          "Name the provenance of every number / Fabrication risk is highest in narrative", s))
    doc = [(rid, heading) for rid, heading, pat in DOC_RULES if not pat.search(text)]
    return flags, doc


def main(argv):
    strict = "--strict" in argv
    paths = [a for a in argv if a != "--strict"]
    if not paths:
        out("usage: writeup_lint.py [--strict] FILE...   (- for stdin)")
        return 2
    total = 0
    out("TIER: SCREENED (regex heuristics). Flags are places to re-read, not findings.")
    out("An empty report does NOT mean the write-up obeys the method; it means no pattern matched.")
    for path in paths:
        try:
            text = read(path)
        except OSError as exc:
            out("UNMEASURABLE: cannot read %s (%s)" % (path, exc))
            return 2
        if not text.strip():
            out("UNMEASURABLE: %s is empty -- nothing was linted" % path)
            return 2
        flags, doc = lint(text)
        out("")
        out("== %s: %d line flag(s), %d missing document-level item(s)" % (path, len(flags), len(doc)))
        for no, rid, heading, s in flags:
            out("  %d: [%s] %s" % (no, rid, s[:160]))
            out("      re-read: %s" % heading)
        for rid, heading in doc:
            out("  (whole document) [%s] nothing found for: %s" % (rid, heading))
        total += len(flags) + len(doc)
    return 1 if (strict and total) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
