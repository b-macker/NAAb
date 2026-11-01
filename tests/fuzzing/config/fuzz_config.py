#!/usr/bin/env python3
"""Config fuzzer (Bundle 9): a govern.json loads as written or refuses to load.

Every case starts from one restrictive config -- a blocked environment
variable, a blocked path, shell disabled -- and applies one to three mutations
that do NOT change what it restricts:

  junk      an unknown key, with a random value, anywhere in the tree
  retype    a non-restriction value given another type (mode, version, levels)
  restrict  a restriction written in another form that still says the same
            thing ("NAAB_FUZZ_SECRET" for ["NAAB_FUZZ_SECRET"], "false" or 0
            for false, a nested list)
  malformed the text itself: truncated, trailing garbage, a BOM, comments,
            trailing commas, NaN, a 5 MB string, 5,000 levels of nesting,
            invalid UTF-8, a NUL, duplicate keys

Then three probe programs run under it, each attempting one restricted action
and printing a token only the action can produce:

  env    env.get() of the blocked variable
  file   file.read() of the blocked path
  shell  a <<sh>> block that writes a marker file

The invariant per (case, probe): the run ends inside its time limit, never by a
signal, and EITHER refuses the config (exit 4) OR the action does not happen.
An action that happens is "loaded weaker than written".

Controls, run first: the unmutated config refuses every action (the probes
judge the restrictions), and an unrestricted config lets every action happen
(the probes can see it; a probe whose action cannot happen here -- no shell
executor -- is UNMEASURABLE, not passed). --plant drop adds a mutation that
really removes a restriction, which every case must then FAIL on.

Usage:
  fuzz_config.py --naab build/naab-lang [--seeds 1-40] [--plant drop]
                 [--keep DIR] [--show SEED] [--timeout 30]
Exit 0 when every case PASSes (with --plant, when every case FAILs), 1 otherwise.
Output is ASCII.
"""
import argparse
import copy
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


def out(line):
    sys.stdout.buffer.write((line + "\n").encode("ascii", "replace"))
    sys.stdout.flush()


SECRET_VAR = "NAAB_FUZZ_SECRET"

BASE = {
    "version": "3.0",
    "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    "capabilities": {
        "env_vars": {"blocked_read": [SECRET_VAR]},
        "filesystem": {"mode": "write", "blocked_paths": ["vault/"]},
        "shell": {"enabled": False},
    },
}

OPEN = {"version": "3.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}}

# The paths whose VALUES are the restrictions: junk and retype never touch them.
RESTRICTED = [("capabilities", "env_vars", "blocked_read"),
              ("capabilities", "filesystem", "blocked_paths"),
              ("capabilities", "shell", "enabled"),
              ("capabilities", "filesystem", "mode")]

PROBES = {
    "env": "use env\nmain {\n    print(\"VAL=\" + string(env.get(\"%s\")))\n}\n" % SECRET_VAR,
    "file": "use file\nmain {\n    print(\"FILE=\" + file.read(\"vault/s.txt\"))\n}\n",
    "shell": "main {\n    let r = <<sh\necho made > marker.txt\n>>\n    print(\"SHELL-RAN\")\n}\n",
}


def rand_value(r, depth=0):
    kinds = ["int", "str", "bool", "null", "float", "list", "obj"] if depth < 3 else ["int", "str", "bool", "null"]
    k = r.choice(kinds)
    if k == "int":
        return r.choice([0, -1, 1, 2 ** 31, -2 ** 63, 10 ** 18])
    if k == "float":
        return r.choice([0.5, -1e300, 1e-300])
    if k == "str":
        return r.choice(["", "enforce", "off", "hard", "x" * r.choice([1, 100, 10_000]), "‮", "../../etc", "*"])
    if k == "bool":
        return r.choice([True, False])
    if k == "null":
        return None
    if k == "list":
        return [rand_value(r, depth + 1) for _ in range(r.randint(0, 3))]
    return {r.choice(["a", "level", "enabled", "mode", "x" * 50]): rand_value(r, depth + 1) for _ in range(r.randint(0, 3))}


def objects(node, path=()):
    if isinstance(node, dict):
        yield path, node
        for k, v in node.items():
            yield from objects(v, path + (k,))


def m_junk(r, cfg):
    path, node = r.choice(list(objects(cfg)))
    key = r.choice(["fuzz_unknown", "", " ", "__proto__", "été", "k" * 300, "_comment", "Mode"])
    node[key] = rand_value(r)
    return "junk key %r at %s" % (key[:20], ".".join(path) or "<root>")


def m_retype(r, cfg):
    target = r.choice([("version",), ("mode",), ("security", "sandbox_level")])
    node = cfg
    for k in target[:-1]:
        node = node[k]
    node[target[-1]] = r.choice([5, None, True, ["enforce"], {"mode": "off"}, "ENFORCE", "Enforce", "strict", ""])
    return "retype %s" % ".".join(target)


def m_restrict(r, cfg):
    caps = cfg["capabilities"]
    which = r.choice(["env", "path", "shell"])
    if which == "env":
        caps["env_vars"]["blocked_read"] = r.choice([SECRET_VAR, [SECRET_VAR, 5], [[SECRET_VAR]], [SECRET_VAR, None],
                                                    [SECRET_VAR.lower()], [SECRET_VAR, SECRET_VAR]])
        return "blocked_read rewritten as %s" % json.dumps(caps["env_vars"]["blocked_read"])[:60]
    if which == "path":
        caps["filesystem"]["blocked_paths"] = r.choice(["vault/", ["vault/", 5], [["vault/"]], ["./vault"], ["vault"],
                                                       ["vault/", None]])
        return "blocked_paths rewritten as %s" % json.dumps(caps["filesystem"]["blocked_paths"])[:60]
    form = r.choice(["false", 0, "no", "off", "bare"])
    if form == "bare":
        caps["shell"] = False
    else:
        caps["shell"]["enabled"] = form
    return "shell disabled written as %s" % json.dumps(caps["shell"])[:60]


def m_drop(r, cfg):
    del cfg["capabilities"]["env_vars"]["blocked_read"]
    del cfg["capabilities"]["filesystem"]["blocked_paths"]
    cfg["capabilities"]["shell"]["enabled"] = True
    return "PLANT: restrictions removed"


TEXT_MUTATIONS = ["truncate", "trailing", "bom", "comment", "trailing_comma", "nan", "huge_string",
                  "deep", "bad_utf8", "nul", "duplicate"]


def m_text(r, text):
    kind = r.choice(TEXT_MUTATIONS)
    if kind == "truncate":
        return text[: r.randint(1, max(1, len(text) - 1))], kind
    if kind == "trailing":
        return text + r.choice(["}", "x", ",", "\n{}\n", "\0"]), kind
    if kind == "bom":
        return "﻿" + text, kind
    if kind == "comment":
        return text.replace("{", "{ // c\n", 1), kind
    if kind == "trailing_comma":
        return text.replace("}", ",}", 1), kind
    if kind == "nan":
        return text.replace("\"version\": \"3.0\"", "\"version\": \"3.0\", \"fuzz_nan\": NaN", 1), kind
    if kind == "huge_string":
        return text.replace("{", "{\"fuzz_big\": \"%s\", " % ("y" * 5_000_000), 1), kind
    if kind == "deep":
        return text.replace("{", "{\"fuzz_deep\": %s1%s, " % ("[" * 5000, "]" * 5000), 1), kind
    if kind == "bad_utf8":
        return text.replace("{", "{\"fuzz_bytes\": \"\udcff\udcfe\", ", 1), kind
    if kind == "nul":
        return text.replace("{", "{\"fuzz_nul\": \"a\u0000b\", ", 1), kind
    # duplicate keys: the second copy of a restriction section carries the SAME
    # restriction, so the text says one thing however a parser resolves it.
    sec = json.dumps({"env_vars": {"blocked_read": [SECRET_VAR]},
                      "filesystem": {"mode": "write", "blocked_paths": ["vault/"]},
                      "shell": {"enabled": False}})
    body = text.rstrip()
    body = body[:-1] if body.endswith("}") else body  # reopen the root object only
    return body + ", \"capabilities\": %s}" % sec, kind


def gen_case(seed, plant=None):
    r = random.Random(seed)
    cfg = copy.deepcopy(BASE)
    notes = []
    for _ in range(r.randint(1, 3)):
        op = r.choice([m_junk, m_junk, m_retype, m_restrict])
        notes.append(op(r, cfg))
    if plant == "drop":
        notes.append(m_drop(r, cfg))
    text = json.dumps(cfg, indent=1)
    if r.random() < 0.35:
        text, kind = m_text(r, text)
        notes.append("text: " + kind)
    return text, notes


def run_probe(naab, wdir, cfg_text, probe, token, timeout):
    os.makedirs(wdir, exist_ok=True)
    os.makedirs(os.path.join(wdir, "vault"), exist_ok=True)
    with open(os.path.join(wdir, "vault", "s.txt"), "w", encoding="utf-8", errors="strict") as f:
        f.write(token)
    marker = os.path.join(wdir, "marker.txt")
    if os.path.exists(marker):
        os.remove(marker)
    with open(os.path.join(wdir, "govern.json"), "wb") as f:
        f.write(cfg_text.encode("utf-8", "surrogateescape"))
    with open(os.path.join(wdir, "p.naab"), "w", encoding="utf-8", errors="strict") as f:
        f.write(PROBES[probe])
    env = dict(os.environ, **{SECRET_VAR: token})
    try:
        p = subprocess.run([naab, "p.naab"], cwd=wdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hang", None, ""
    text = p.stdout.decode("utf-8", "replace")
    happened = (token in text) if probe in ("env", "file") else os.path.exists(marker)
    return p.returncode, happened, text


def judge(rc, happened):
    if rc == "hang":
        return "FAIL", "hang"
    if rc < 0 or rc >= 128:
        return "FAIL", "crash: exit %d" % rc
    if rc == 4:
        return "PASS", "refused to load (exit 4)"
    if happened:
        return "FAIL", "loaded weaker than written: the restricted action happened (exit %d)" % rc
    return "PASS", "loaded; the action was refused (exit %d)" % rc


def parse_seeds(spec):
    seeds = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-", 1)
            seeds.extend(range(int(a), int(b) + 1))
        elif part:
            seeds.append(int(part))
    return seeds


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--naab", required=True)
    ap.add_argument("--seeds", default="1-40")
    ap.add_argument("--plant", choices=["drop"])
    ap.add_argument("--keep")
    ap.add_argument("--show", type=int)
    ap.add_argument("--timeout", type=int, default=30)
    a = ap.parse_args(argv)
    if a.show is not None:
        text, notes = gen_case(a.show, a.plant)
        out("mutations: " + "; ".join(notes))
        out(text[:3000] + (" ... (%d chars)" % len(text) if len(text) > 3000 else ""))
        return 0
    naab = os.path.abspath(a.naab)
    if not os.path.exists(naab) and os.path.exists(naab + ".exe"):
        naab += ".exe"
    if not os.path.exists(naab):
        out("UNMEASURABLE: --naab %s does not exist" % a.naab)
        return 1
    tmp = a.keep or tempfile.mkdtemp(prefix="naab_fuzz_cfg_")
    # An empty trust store of our own: with any key in the ambient one, every
    # unsigned config here is an INTEGRITY BLOCK, so each run would end in the
    # same refusal whatever it was given (tests/helpers/trust_setup.sh).
    os.makedirs(os.path.join(tmp, "trust-store"), exist_ok=True)
    os.environ["NAAB_TRUST_STORE_DIR"] = os.path.join(tmp, "trust-store")
    rnd = random.Random(12345)
    token = lambda: "tok-%08x" % rnd.getrandbits(32)

    # Controls: which probes can observe their action at all, and that the
    # unmutated config refuses each.
    measurable = []
    for probe in PROBES:
        rc_o, happened_o, text_o = run_probe(naab, os.path.join(tmp, "open-" + probe), json.dumps(OPEN), probe, token(), a.timeout)
        rc_b, happened_b, text_b = run_probe(naab, os.path.join(tmp, "base-" + probe), json.dumps(BASE), probe, token(), a.timeout)
        if happened_o is not True:
            out("UNMEASURABLE control %s: the action does not happen even unrestricted (exit %s) -- its cases are skipped" % (probe, rc_o))
            continue
        if happened_b:
            out("FAIL control %s: the unmutated restrictive config let the action happen (exit %s)" % (probe, rc_b))
            return 1
        out("PASS control %s: happens unrestricted, refused by the unmutated config (exit %s)" % (probe, rc_b))
        measurable.append(probe)
    if not measurable:
        out("UNMEASURABLE: no probe can observe its action here")
        return 1

    counts = {"PASS": 0, "FAIL": 0}
    for seed in parse_seeds(a.seeds):
        text, notes = gen_case(seed, a.plant)
        worst = ("PASS", "")
        for probe in measurable:
            rc, happened, ptext = run_probe(naab, os.path.join(tmp, "seed%d-%s" % (seed, probe)), text, probe, token(), a.timeout)
            verdict, why = judge(rc, happened)
            if verdict == "FAIL":
                worst = (verdict, "%s: %s" % (probe, why))
                if not a.plant:
                    out("FAIL seed %d %s: %s [%s]" % (seed, probe, why, "; ".join(notes)))
                    for line in ptext.splitlines()[-5:]:
                        out("       | " + line[:200])
        counts[worst[0]] += 1
        if worst[0] == "PASS" or a.plant:
            out("%s seed %d [%s]" % (worst[0], seed, "; ".join(notes)[:160]))
    if not a.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    out("cases: %d PASS, %d FAIL; probes measured: %s" % (counts["PASS"], counts["FAIL"], ",".join(measurable)))
    if a.plant:
        return 0 if counts["FAIL"] > 0 and counts["PASS"] == 0 else 1
    return 0 if counts["FAIL"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
