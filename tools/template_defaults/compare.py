#!/usr/bin/env python3
"""Compare every setting in a govern template against the engine default.

Usage: compare.py TEMPLATE.json DUMPER OUTDIR

For each template node N (leaf or object), the config "template minus N" is
loaded through the real loader (DUMPER) and its parsed rules are diffed against
the full template's. A field that changes is a field whose template value
differs from what a user gets by OMITTING the key while keeping its section.
Three further passes separate the cases a single deletion cannot:

  perturb   a leaf with no deletion diff is set to other values. If the rules
            change, the key is live and the template happens to match; if not,
            nothing in the rules struct reads it (here).
  isolate   a no-effect leaf is perturbed alone in an otherwise empty config.
            An effect there means a sibling key in the template SHADOWS it.
  masked    for every live, no-diff leaf, the field it controls is compared
            with the empty-config value. Two aliases carrying the same value
            (governance.sandbox_level and security.sandbox_level) hide each
            other from the deletion test; this pass is what finds them.

Blind spots, by construction (report them, do not read silence as a match):
  - keys resolved through getenv() at load (*_env): the variable is unset
    here, so every value looks inert. Set the variable to measure them.
  - process globals the loader sets outside GovernanceRules
    (naab::limits::setMaxJsonDepth for limits.data.max_json_depth).
  - post-load logic in main.cpp (the enforce-mode sandbox upgrade, the
    runtime.timeout != 30 sentinel) and raw-JSON readers (the scanner section;
    see scanner_compare.py).
  - leaves under arrays of objects are deleted, not isolated.

Outputs (OUTDIR): deletion.json, perturb.json, isolate.json, masked.tsv,
leaf_diffs.tsv (template / key-omitted / empty-config value per field).
"""
import copy
import json
import os
import subprocess
import sys

TEMPLATE, DUMPER, OUT = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]
os.makedirs(OUT, exist_ok=True)
CUR = os.path.join(OUT, "cur.json")
DOC_KEYS = ("rationale", "description", "message")


def run(cfg):
    with open(CUR, "w") as f:
        f.write(json.dumps(cfg))
    r = subprocess.run([DUMPER, CUR], capture_output=True, text=True)
    return {l.split("\t", 1)[0]: l.split("\t", 1)[1] for l in r.stdout.splitlines() if "\t" in l}


def ignored(k):
    return k.startswith("R.explicitly_set")


def diff(a, b):
    return [[k, a.get(k), b.get(k)] for k in sorted(set(a) | set(b))
            if not ignored(k) and a.get(k) != b.get(k)]


base = json.load(open(TEMPLATE))
# extends names a parent that does not ship; loadFromString never resolves it,
# so it would only add an extends_path row. Measure the template's own values.
base.pop("extends", None)
T = run(base)
E = run({})
assert T.get("LOAD") == "ok", "template failed to load"
assert E.get("LOAD") == "ok", "empty config failed to load"

nodes = []


def walk(o, p):
    if isinstance(o, dict):
        for k, v in o.items():
            if k.startswith("_comment"):
                continue
            nodes.append(p + [k])
            walk(v, p + [k])
    elif isinstance(o, list) and o and all(isinstance(x, dict) for x in o):
        for i, x in enumerate(o):
            nodes.append(p + [i])
            walk(x, p + [i])


walk(base, [])


def get_parent(cfg, path):
    par = cfg
    for k in path[:-1]:
        par = par[k]
    return par


# --- deletion ---------------------------------------------------------------
deletion = []
for path in nodes:
    cfg = copy.deepcopy(base)
    par = get_parent(cfg, path)
    tv = par[path[-1]]
    if isinstance(par, list):
        par.pop(path[-1])
    else:
        del par[path[-1]]
    D = run(cfg)
    leaf = not isinstance(tv, dict) and not (isinstance(tv, list) and tv and all(isinstance(x, dict) for x in tv))
    deletion.append({"path": path, "leaf": leaf, "tvalue": tv if leaf else None,
                     "load": D.get("LOAD"), "diffs": diff(T, D)})
json.dump(deletion, open(os.path.join(OUT, "deletion.json"), "w"))


# --- perturbation -----------------------------------------------------------
def alts(v):
    if isinstance(v, bool):
        return [not v]
    if isinstance(v, int):
        return [v + 7, -3] if v != 0 else [7, 1]
    if isinstance(v, float):
        return [v * 0.5 + 0.123, 0.987]
    if isinstance(v, str):
        words = ["zz_perturbed", "hard", "soft", "advisory", "detect", "block", "quarantine",
                 "attest", "full", "recent", "summary", "none", "read", "write", "enforce",
                 "audit", "off", "allowlist", "blocklist", "append", "replace", "pass"]
        return [w for w in words if w != v]
    if isinstance(v, list):
        return [v + ["zz_perturbed"], [], v[:1] if len(v) > 1 else ["zz_perturbed", "yy"]]
    if v is None:
        return [1, "x", True]
    return []


perturb = []
for r in deletion:
    if not r["leaf"] or r["diffs"]:
        continue
    path, hit = r["path"], None
    for a in alts(r["tvalue"]):
        cfg = copy.deepcopy(base)
        get_parent(cfg, path)[path[-1]] = a
        D = run(cfg)
        if D.get("LOAD") != "ok":
            continue
        d = diff(T, D)
        if d:
            hit = {"alt": a, "diffs": d}
            break
    perturb.append({"path": path, "tvalue": r["tvalue"], "live": hit is not None, "hit": hit})
json.dump(perturb, open(os.path.join(OUT, "perturb.json"), "w"))


# --- isolation --------------------------------------------------------------
def build(path, val):
    cfg, cur = {}, None
    cur = cfg
    for k in path[:-1]:
        if isinstance(k, int):
            return None
        cur[k] = {}
        cur = cur[k]
    cur[path[-1]] = val
    return cfg


iso = {"shadowed": [], "unread": [], "skipped": [], "scanner": []}
for o in perturb:
    if o["live"]:
        continue
    name = ".".join(map(str, o["path"]))
    if o["path"][0] == "scanner":
        iso["scanner"].append(name)   # raw-JSON reader; see scanner_compare.py
        continue
    if build(o["path"], 0) is None:
        iso["skipped"].append(name)
        continue
    outs = set()
    for a in [o["tvalue"]] + alts(o["tvalue"]):
        D = run(build(o["path"], a))
        outs.add(json.dumps({k: v for k, v in D.items() if not ignored(k)}, sort_keys=True))
    iso["shadowed" if len(outs) > 1 else "unread"].append(name)
json.dump(iso, open(os.path.join(OUT, "isolate.json"), "w"), indent=1)

# --- alias masking ------------------------------------------------------------
with open(os.path.join(OUT, "masked.tsv"), "w") as f:
    for o in perturb:
        if not o["live"]:
            continue
        for k, _, _ in o["hit"]["diffs"]:
            if "#size" in k:
                continue
            if T.get(k) != E.get(k) and E.get(k) is not None:
                f.write(f"{'.'.join(map(str, o['path']))}\t{k[2:]}\t{T.get(k)}\t{E.get(k)}\n")

# --- leaf table: template / key omitted in section / empty config -----------
with open(os.path.join(OUT, "leaf_diffs.tsv"), "w") as f:
    f.write("template_key\ttemplate_value\tfield\ttemplate\tkey_omitted\tempty_config\n")
    for x in deletion:
        if not x["leaf"] or not x["diffs"] or x["path"][-1] in DOC_KEYS:
            continue
        for k, t, a in x["diffs"]:
            if a is None or "#size" in k:
                continue
            f.write("\t".join([".".join(map(str, x["path"])), json.dumps(x["tvalue"]),
                               k[2:], str(t), str(a), str(E.get(k))]) + "\n")

leaves = [x for x in deletion if x["leaf"]]
ldiff = [x for x in leaves if x["diffs"]]
print(f"nodes={len(deletion)} leaves={len(leaves)} leaf_diffs={len(ldiff)} "
      f"(doc_strings={sum(1 for x in ldiff if x['path'][-1] in DOC_KEYS)}) "
      f"no_diff={len(perturb)} live_equal={sum(o['live'] for o in perturb)} "
      f"no_rules_effect={sum(not o['live'] for o in perturb)} "
      f"isolate={ {k: len(v) for k, v in iso.items()} } "
      f"load_failures={sum(1 for x in deletion if x['load'] != 'ok')}")
