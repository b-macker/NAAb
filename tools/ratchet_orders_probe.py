#!/usr/bin/env python3
"""Probe every declared order through the reload decision.

For each `order` / `fixed` line in tests/governance_v4/ratchet_classification.txt
(the lines tools/ratchet_coverage.py --emit-orders compiles into the engine),
build govern.json pairs that differ ONLY at that key and ask
`naab-gov ratchet OLD NEW --ratchet-only` -- the engine's own reload decision,
evaluateReload() -- what a mid-run reload would do:

  same     OLD -> OLD                     must be ACCEPTED (exit 0): the control
                                          that the pair loads and nothing else
                                          refuses it
  loosen   the looser value               must be REFUSED (exit 1), with a
                                          violation naming the key
  tighten  the tighter value (orders)     must be ACCEPTED (exit 0)
  change   another value (fixed)          must be REFUSED, naming the key
  absent   the key removed (no default)   must be REFUSED, naming the key
  present  a presence-enabled check block: removing it, or a shorthand false,
           must be REFUSED; adding it to a config without it ACCEPTED

A pair the engine cannot load (exit 4), or whose `same` control is refused, is
UNMEASURABLE for that key -- loudly, never folded into PASS. Every verdict comes
from the engine binary; this script only writes the pairs.

Run:  python3 tools/ratchet_orders_probe.py --naab-gov build/naab-gov
          [--only REGEX] [--table PATH] [--verbose]
Exit: 0 when every key PASSes, 1 on any FAIL or UNMEASURABLE.
Output is ASCII.
"""
import copy
import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import ratchet_coverage as rc  # noqa: E402

TEMPLATE = os.path.join(REPO, "govern-template.json")
BASE = {"version": "3.0", "mode": "enforce"}


def placeholder(seg, prev):
    if seg == "<lang_name>":
        return "python"
    if prev == "agents":
        return "probe_agent"
    if prev == "rule_weights":
        return "code_quality.no_secrets"
    return "probe"


def concrete_path(path):
    segs = path.split(".")
    out = []
    for i, s in enumerate(segs):
        out.append(placeholder(s, segs[i - 1] if i else "") if s.startswith("<") else s)
    return out


def template_value(path):
    """The template's value at `path`, taking the first child for a placeholder."""
    try:
        with open(TEMPLATE, "rb") as f:
            node = json.loads(f.read().decode("utf-8"))
    except (OSError, ValueError):
        return None
    for s in path.split("."):
        if not isinstance(node, dict):
            return None
        if s.startswith("<"):
            keys = [k for k in node if not k.startswith("_")]
            if not keys:
                return None
            node = node[keys[0]]
        elif s in node:
            node = node[s]
        else:
            return None
    return node


def mutate(v):
    if isinstance(v, bool):
        return not v
    if isinstance(v, int):
        return v + 1
    if isinstance(v, float):
        return v + 0.5
    if isinstance(v, str):
        return v + "_probe"
    if isinstance(v, list):
        return (copy.deepcopy(v[:1]) and [mutate(v[0])] + copy.deepcopy(v[1:])) or ["probe"]
    if isinstance(v, dict):
        keys = [k for k in v if not k.startswith("_")]
        if not keys:
            return {"probe": 1}
        # Prefer a value whose change cannot name something the loader
        # validates (a language, a level): a number or a switch, then a list.
        def rank(k):
            x = v[k]
            if isinstance(x, (bool, int, float)):
                return 0
            if isinstance(x, (list, dict)):
                return 1
            return 2 if k in ("language", "level", "name") else 1
        k = sorted(keys, key=rank)[0]
        w = copy.deepcopy(v)
        w[k] = mutate(w[k])
        return w
    return 1


def values_for(kind, vals, path):
    """(old, tighter, looser) for an order kind; looser may be a list of values."""
    if kind == "true":
        return True, None, [False]
    if kind == "false":
        return False, None, [True]
    if kind == "lower":
        return 10, 5, [20]
    if kind == "lower0":
        return 10, 5, [20, 0]
    if kind == "higher":
        return 10, 20, [5]
    if kind == "level":
        return "soft", "hard", ["advisory"]
    if kind == "enum":
        if len(vals) < 2:
            return vals[0], None, []
        return vals[1], (vals[2] if len(vals) > 2 else None), [vals[0]]
    if kind == "superset":
        return ["probe_a"], ["probe_a", "probe_b"], [[]]
    if kind == "subset":
        return ["probe_a", "probe_b"], ["probe_a"], [["probe_a", "probe_b", "probe_c"]]
    raise ValueError(kind)


def build(segs, value, present=True):
    cfg = copy.deepcopy(BASE)
    node = cfg
    for s in segs[:-1]:
        node = node.setdefault(s, {})
    if present:
        node[segs[-1]] = value
    return cfg


def ask(gov, tmp, old, new):
    po, pn = os.path.join(tmp, "old.json"), os.path.join(tmp, "new.json")
    with open(po, "w", encoding="utf-8", errors="strict") as f:
        json.dump(old, f)
    with open(pn, "w", encoding="utf-8", errors="strict") as f:
        json.dump(new, f)
    # A later mtime for NEW, as a reload sees it.
    st = os.stat(po)
    os.utime(pn, (st.st_atime + 2, st.st_mtime + 2))
    p = subprocess.run([gov, "ratchet", po, pn, "--ratchet-only"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=60)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def names_key(out, segs):
    shown = ".".join(segs)
    return shown in out


def probe(gov, path, cls, arg, tmp):
    segs = concrete_path(path)
    arms = []  # (label, old, new, want_rc, must_name)
    if cls == "fixed":
        old = template_value(path)
        if old is None:
            old = 1
        arms.append(("same", old, old, 0, False))
        arms.append(("change", old, mutate(old), 1, True))
        arms.append(("absent", old, None, 1, True))
    elif arg[0] == "present":
        # A check block whose presence is its on switch: removing it, or a
        # shorthand false (which the loaders read differently), is refused;
        # adding it to a config without it is accepted.
        block = {"level": "hard"}
        arms.append(("same", block, block, 0, False))
        arms.append(("loosen:removed", block, None, 1, True))
        arms.append(("loosen:false", block, False, 1, True))
        arms.append(("tighten:added", None, block, 0, False))
    else:
        kind, vals, default, _ = arg
        old, tight, looser = values_for(kind, vals, path)
        arms.append(("same", old, old, 0, False))
        for lv in looser:
            arms.append(("loosen:%s" % json.dumps(lv), old, lv, 1, True))
        if tight is not None:
            arms.append(("tighten:%s" % json.dumps(tight), old, tight, 0, False))
        if default is rc._NO_DEFAULT:
            arms.append(("absent", old, None, 1, True))
    results = []
    same_ok = True
    for label, o, n, want, must_name in arms:
        oc = build(segs, o, present=o is not None)
        nc = build(segs, n, present=n is not None)
        rcode, out = ask(gov, tmp, oc, nc)
        if label == "same":
            same_ok = rcode == 0
            results.append((label, "PASS" if same_ok else "UNMEASURABLE", rcode, out))
            continue
        if not same_ok:
            results.append((label, "UNMEASURABLE", rcode, out))
            continue
        if rcode == 4:
            results.append((label, "UNMEASURABLE", rcode, out))
        elif rcode == want and (not must_name or names_key(out, segs)):
            results.append((label, "PASS", rcode, out))
        else:
            results.append((label, "FAIL", rcode, out))
    return results


def main(argv):
    gov, only, table, verbose = None, None, rc.TABLE, False
    i = 0
    while i < len(argv):
        if argv[i] == "--naab-gov" and i + 1 < len(argv):
            gov = argv[i + 1]; i += 2
        elif argv[i] == "--only" and i + 1 < len(argv):
            only = re.compile(argv[i + 1]); i += 2
        elif argv[i] == "--table" and i + 1 < len(argv):
            table = argv[i + 1]; i += 2
        elif argv[i] == "--verbose":
            verbose = True; i += 1
        else:
            sys.stdout.buffer.write(("unknown argument: %s\n" % argv[i]).encode("ascii", "replace"))
            return 2
    if not gov or not os.path.exists(gov):
        sys.stdout.buffer.write(b"probe: --naab-gov PATH is required and must exist -- UNMEASURABLE\n")
        return 1
    entries, errors = rc.load_table(table)
    if errors:
        sys.stdout.buffer.write(("\n".join("!! " + e for e in errors) + "\n").encode("ascii", "replace"))
        return 1
    lines, counts = [], {"PASS": 0, "FAIL": 0, "UNMEASURABLE": 0}
    keys = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path, (cls, arg) in entries.items():
            if cls not in ("order", "fixed") or (only and not only.search(path)):
                continue
            keys += 1
            res = probe(gov, path, cls, arg, tmp)
            worst = "PASS"
            for label, verdict, rcode, out in res:
                counts[verdict] += 1
                if verdict == "FAIL" or (verdict == "UNMEASURABLE" and worst == "PASS"):
                    worst = verdict if worst != "FAIL" else worst
                if verdict != "PASS" or verbose:
                    detail = " | ".join(l.strip() for l in out.splitlines()
                                        if "violation" in l or "error" in l.lower() or "REFUSED" in l
                                        or "ACCEPTED" in l)[:300]
                    lines.append("%s %s [%s] exit %d: %s" % (verdict, path, label, rcode, detail))
            lines.append("key %s %s" % (worst, path))
    lines.append("keys probed: %d" % keys)
    lines.append("arms: %d PASS, %d FAIL, %d UNMEASURABLE" % (counts["PASS"], counts["FAIL"], counts["UNMEASURABLE"]))
    sys.stdout.buffer.write(("\n".join(lines) + "\n").encode("ascii", "replace"))
    return 0 if counts["FAIL"] == 0 and counts["UNMEASURABLE"] == 0 and keys > 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
