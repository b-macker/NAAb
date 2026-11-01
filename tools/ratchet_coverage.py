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
               order K [default=J] -- why
                                   a DECLARED ORDER the reload enforces
                                   (checkDeclaredOrderRatchet): K is true,
                                   false, lower, lower0 (0 = unlimited),
                                   higher, level, enum:a<b<c (loosest first),
                                   superset or subset; J is the traced value
                                   an absent key has
               fixed    -- why     no single tighter direction: the reload
                                   refuses any change
             An entry may name an ANCESTOR of loader leaves (a list of rules,
             say); it then classifies every leaf below it.
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
      python3 tools/ratchet_coverage.py --emit-orders   (the engine's table)
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

CLASSES = ("ratchet", "inert", "startup", "neutral", "order", "fixed")
ORDER_KINDS = {"true": "True", "false": "False", "lower": "Lower", "lower0": "LowerZero",
               "higher": "Higher", "level": "Level", "enum": "Enum",
               "superset": "Superset", "subset": "Subset"}
LEVEL_NAMES = ("hard", "soft", "advisory", "detect", "approval_required", "none")
INC = os.path.join(REPO, "src", "runtime", "ratchet_declared_orders.inc")

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


def parse_order(rest):
    """`K[:values] [default=JSON] -- reason` -> (kind, values, default, reason, error)."""
    import json
    m = re.match(r'^(\w+)(?::(\S+))?\s*(.*)$', rest)
    if not m:
        return None, None, None, None, "order entry needs a kind"
    kind, vals, tail = m.group(1), m.group(2), m.group(3)
    if kind not in ORDER_KINDS:
        return None, None, None, None, "unknown order kind %r" % kind
    if (kind == "enum") != (vals is not None):
        return None, None, None, None, "enum needs ':a<b<c' and only enum takes values"
    values = vals.split("<") if vals else None
    default = None
    if tail.startswith("default="):
        try:
            default, end = json.JSONDecoder().raw_decode(tail[len("default="):])
        except ValueError:
            return None, None, None, None, "default= is not JSON"
        tail = tail[len("default=") + end:].strip()
        has_default = True
    else:
        has_default = False
    if not tail.startswith("--") or len(tail[2:].strip()) < 10:
        return None, None, None, None, "order entry needs '-- reason'"
    if has_default:
        ok = {"true": isinstance(default, bool), "false": isinstance(default, bool),
              "lower": _num(default), "lower0": _num(default), "higher": _num(default),
              "level": isinstance(default, str) and default.lower() in LEVEL_NAMES,
              "enum": isinstance(default, str) and values is not None and default in values,
              "superset": isinstance(default, list), "subset": isinstance(default, list)}[kind]
        if not ok:
            return None, None, None, None, "default %r does not fit order %s" % (default, kind)
    return kind, values, (default if has_default else _NO_DEFAULT), tail[2:].strip(), None


_NO_DEFAULT = object()


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


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
        elif cls == "order":
            kind, values, default, reason, err = parse_order(rest)
            if err:
                errors.append("table line %d: %s" % (n, err))
                continue
            entries[key] = (cls, (kind, values, default, reason))
        else:
            if not rest.startswith("--") or len(rest[2:].strip()) < 10:
                errors.append("table line %d: %s entry needs '-- reason'" % (n, cls))
                continue
            entries[key] = (cls, rest[2:].strip())
    return entries, errors


def emit_orders(path=TABLE):
    """The engine's table: one C++ initialiser per order/fixed entry, in file order."""
    import json
    entries, errors = load_table(path)
    if errors:
        return None, errors

    def cstr(s):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
    out = ["// GENERATED by tools/ratchet_coverage.py --emit-orders from",
           "// tests/governance_v4/ratchet_classification.txt -- do not edit;",
           "// test_ratchet_coverage.sh RC-07 fails when the two differ.",
           "// {path, order, enum values (loosest first), default JSON of an absent key}"]
    for key, (cls, arg) in entries.items():
        if cls == "fixed":
            out.append("{%s, DeclaredOrder::Fixed, nullptr, nullptr}," % cstr(key))
        elif cls == "order":
            kind, values, default, _ = arg
            v = cstr("|".join(values)) if values else "nullptr"
            d = "nullptr" if default is _NO_DEFAULT else cstr(json.dumps(default, separators=(",", ":")))
            out.append("{%s, DeclaredOrder::%s, %s, %s}," % (cstr(key), ORDER_KINDS[kind], v, d))
    return "\n".join(out) + "\n", []


def covered_by(leaf, table):
    """The table entry classifying `leaf`: the leaf itself, else its nearest ancestor."""
    if leaf in table:
        return leaf
    segs = leaf.split(".")
    for k in range(len(segs) - 1, 0, -1):
        anc = ".".join(segs[:k])
        if anc in table:
            return anc
    return None


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
        hit = covered_by(p, table)
        if hit is not None:
            cls, arg = table[hit]
            if cls == "ratchet" and not mentions(lits, arg):
                result[p] = "hollow"   # claims a comparison the code does not name
            else:
                result[p] = "table"
        elif mentions(lits, ratchet_token(p)):
            result[p] = "named"
        else:
            result[p] = "unclassified"
    for key in table:
        if key not in result and not any(p.startswith(key + ".") for p in result):
            failures.append("!! stale table entry (the loader no longer reads it): " + key)
    for key, want in SELF_TEST.items():
        got = result.get(key)
        if got != want and not strip:
            failures.append("!! SELF-TEST FAIL: %s classified %r, expected %r" % (key, got, want))
    return result, failures


def main(argv):
    if "--emit-orders" in argv:
        text, errors = emit_orders()
        if errors:
            sys.stdout.buffer.write(("\n".join("!! " + e for e in errors) + "\n").encode("ascii", "replace"))
            return 1
        sys.stdout.buffer.write(text.encode("ascii", "replace"))
        return 0
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
