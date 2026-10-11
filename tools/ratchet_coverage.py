#!/usr/bin/env python3
"""Which govern.json settings can a signed mid-run reload change without the
ratchet comparing them?

reloadIfChanged() installs a new rule set when the reloaded govern.json passes
checkRatchetViolation(), and every check that reads rules() afterwards sees the
new values. The ratchet is a hand-written list of comparisons. It was written
in 596dd798 against ~50 scalar fields and grew one feature at a time, so a key
nobody added to it is loosenable mid-run without a violation or a notice:
until the capability-list ratchet (#303) no allow/block list was compared, and
until this guard taint sinks, sanitizers and taint_tracking.level were not.

This tool lists every LEAF loadFromJson() reads (the extractor in
tools/template_coverage.py, with its self-test) and classifies each one:

  named      the key's dotted path (agents.<name>.X as "agent.{}.X") appears in
             a string literal inside the ratchet code -- the comparison names
             it in its violation or notice text;
  table      tests/governance_v4/ratchet_classification.txt says why:
               ratchet "<token>"   compared under a different message; the
                                   token must appear in a ratchet literal
               inert    -- why     parsed, consulted by nothing
               startup  -- why     read once at startup; a reload cannot
                                   change its effect
               neutral  -- why     changes no enforcement in either direction
  filtered   per-block "rationale" (text only) and the .level.enabled /
             .level.level paths parseEnforcementLevel() statically reads.

Everything else is UNCLASSIFIED. tests/governance_v4/test_ratchet_coverage.sh
pins that set to ratchet_unclassified_baseline.txt, which may only shrink: a
NEW loader key fails the gate until it is classified.

SCREENED, NOT VERIFIED. "named" proves the ratchet MENTIONS a key, not that it
compares it in the right direction -- that needs a behavioural test, and the
reload suites (test_capability_list_ratchet.sh and the per-feature ones) are
where it lives. An "inert"/"startup"/"neutral" line is a claim its reason must
carry. The guard's job is the class: no new key reaches the reload path
without someone deciding what a loosening of it means.

Run:  python3 tools/ratchet_coverage.py [--strip-token TOKEN]... [--config-stdin]
      --strip-token removes a literal from the ratchet text before matching,
      and --config-stdin reads governance_config.cpp from stdin instead of the
      tree (the test's negative controls: a planted loader key, a dropped
      ratchet token). Stdin, not a path, so a native Windows python never has
      to open an MSYS path.

Output is ASCII, written as bytes. Exit 0 = the self-test passed; non-zero
means the INSTRUMENT is broken and the list is not a result.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import template_coverage as tc  # noqa: E402

CONFIG = os.path.join(REPO, "src", "runtime", "governance_config.cpp")
TABLE = os.path.join(REPO, "tests", "governance_v4", "ratchet_classification.txt")

REGION_START = "// Capability lists in the reload ratchet."
REGION_END = "bool GovernanceEngine::reloadIfChanged()"

CLASSES = ("ratchet", "inert", "startup", "neutral")

# POSITIVE controls: must classify as shown, or the instrument is broken.
SELF_TEST = {
    "limits.timeout.global": "named",              # plain literal in chkLimit
    "agents.<name>.max_turns": "named",            # "agent.{}.max_turns"
    "capabilities.env_vars.blocked_read": "table",  # list ratchet, token form
    "integrity.blocked_flags": "table",            # startup-only claim
}


def ratchet_literals(text, strip):
    a = text.find(REGION_START)
    b = text.find(REGION_END)
    if a < 0 or b < 0 or b <= a:
        return None
    region = text[a:b]
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', region)
    out = []
    for lit in lits:
        for s in strip:
            lit = lit.replace(s, "")
        out.append(lit)
    return out


def mentions(lits, token):
    pat = re.compile(r"(?<![\w.])" + re.escape(token) + r"(?![\w])")
    return any(pat.search(l) for l in lits)


def leaf_paths(reads):
    trie = {}
    for path in reads:
        node = trie
        for seg in path:
            node = node.setdefault(seg, {})
    out = []

    def is_level_artifact(p):
        ctxs = reads.get(p, set())
        return (len(p) >= 2 and p[-2] in tc.LEVEL_VALUE_KEYS and p[-1] in ("enabled", "level")
                and any("parseEnforcementLevel" in c for c in ctxs))

    def walk(node, path):
        p = tuple(path)
        # A "level" key read through parseEnforcementLevel() has only artifact
        # children (.enabled/.level, read in case the value were an object).
        # The KEY is the setting: dropping the artifacts must not drop it too,
        # or every enforcement level vanishes from the guard's view.
        if node and path and all(is_level_artifact(p + (k,)) and not v for k, v in node.items()):
            out.append(".".join(p))
            return
        if not node:
            if is_level_artifact(p) or p[-1] == "rationale":
                return
            out.append(".".join(p))
            return
        for k, v in node.items():
            walk(v, path + [k])

    walk(trie, [])
    return sorted(set(out))


def ratchet_token(path):
    if path.startswith("agents.<name>."):
        return "agent.{}." + path[len("agents.<name>."):]
    return path


def load_table(path):
    entries, errors = {}, []
    with open(path, "rb") as f:
        lines = f.read().decode("utf-8", "replace").splitlines()
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r'^(\S+)\s+(\S+)\s*(.*)$', line)
        if not m:
            errors.append("table line %d: cannot parse: %s" % (n, line))
            continue
        key, cls, rest = m.group(1), m.group(2), m.group(3).strip()
        if key in entries:
            errors.append("table line %d: duplicate key %s" % (n, key))
            continue
        if cls not in CLASSES:
            errors.append("table line %d: unknown class %r" % (n, cls))
            continue
        if cls == "ratchet":
            t = re.match(r'^"([^"]+)"', rest)
            if not t:
                errors.append("table line %d: ratchet entry needs a \"token\"" % n)
                continue
            entries[key] = (cls, t.group(1))
        else:
            if not rest.startswith("--") or len(rest[2:].strip()) < 10:
                errors.append("table line %d: %s entry needs '-- reason'" % (n, cls))
                continue
            entries[key] = (cls, rest[2:].strip())
    return entries, errors


def classify(argv):
    strip = []
    config = CONFIG
    i = 0
    while i < len(argv):
        if argv[i] == "--strip-token" and i + 1 < len(argv):
            strip.append(argv[i + 1]); i += 2
        elif argv[i] == "--config" and i + 1 < len(argv):
            config = argv[i + 1]; i += 2
        elif argv[i] == "--config-stdin":
            config = None; i += 1
        else:
            i += 1
    if config is None:
        text = sys.stdin.buffer.read().decode("utf-8", "replace")
        return classify_text(text, strip)
    with open(config, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    return classify_text(text, strip)


def classify_text(text, strip):
    failures = []
    lits = ratchet_literals(text, strip)
    if lits is None:
        return None, ["!! SELF-TEST FAIL: ratchet region markers not found in governance_config.cpp"]
    if len(lits) < 100:
        failures.append("!! SELF-TEST FAIL: only %d string literals in the ratchet region" % len(lits))
    ex = tc.Extractor(text)
    ex.run()
    if len(ex.reads) < 500:
        failures.append("!! SELF-TEST FAIL: only %d loader paths extracted" % len(ex.reads))
    leaves = leaf_paths(ex.reads)
    table, terr = load_table(TABLE)
    failures += ["!! " + e for e in terr]

    result = {}
    for p in leaves:
        if p in table:
            cls, arg = table[p]
            if cls == "ratchet" and not mentions(lits, arg):
                result[p] = "hollow"   # claims a comparison the code does not name
            else:
                result[p] = "table"
        elif mentions(lits, ratchet_token(p)):
            result[p] = "named"
        else:
            result[p] = "unclassified"
    for key in table:
        if key not in result:
            failures.append("!! stale table entry (the loader no longer reads it): " + key)
    for key, want in SELF_TEST.items():
        got = result.get(key)
        if got != want and not strip:
            failures.append("!! SELF-TEST FAIL: %s classified %r, expected %r" % (key, got, want))
    return result, failures


def main(argv):
    result, failures = classify(argv)
    lines = []
    if result is not None:
        counts = {}
        for v in result.values():
            counts[v] = counts.get(v, 0) + 1
        lines.append("count leaves: %d" % len(result))
        for k in ("named", "table", "hollow", "unclassified"):
            lines.append("count %s: %d" % (k, counts.get(k, 0)))
    lines += failures
    lines.append("self-test failures: %d" % len(failures))
    if result is not None:
        lines += ["hollow: " + p for p, v in sorted(result.items()) if v == "hollow"]
        lines += ["unclassified: " + p for p, v in sorted(result.items()) if v == "unclassified"]
    sys.stdout.buffer.write(("\n".join(lines) + "\n").encode("ascii", "replace"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
