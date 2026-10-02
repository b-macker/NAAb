#!/usr/bin/env python3
"""check_govern_keys.py -- report govern.json keys the engine never reads.

    python3 tools/check_govern_keys.py src/govern.json [--repo <NAAb checkout>]

Exit 0: every key is mentioned by the parser for its section.
Exit 1: one or more keys are UNREAD -- a typo, or a key from a template or a
        doc that no code consumes. The engine ignores it silently.
Exit 2: usage error, or the parser source could not be read (UNMEASURABLE --
        never reported as a pass).

Why this exists: the engine warns about unknown TOP-LEVEL keys only
(validateSchema); a misspelled key inside a section is silently ignored, and
meta.schema_validation.warn_unknown_keys, which reads as though it would
change that, is parsed and never consulted. A misspelled nested key is
therefore indistinguishable from a working one until something fails to
happen.

Scope: only src/runtime/governance_config.cpp is scanned. A section parsed in
another file reports its keys as UNREAD, so on a large config treat a hit as
"check this", not as proof -- grep the key across src/ before deleting it.

What it proves and what it does not. A key the parser NEVER mentions is
certainly ignored. A key it DOES mention is only "read somewhere in that
section's parser" -- not proof the setting has an effect. Effects are proven by
behaviour (tests/governance_v4/test_agent_harness_example.sh), not by this.
"""
import json
import os
import re
import sys

# Maps whose children are user-chosen names, not keys. The value under each
# name is checked against the vocabulary given here.
NAMED_MAPS = {
    ("agents",): "agents",
    ("capabilities", "functions"): "capabilities",
    ("contracts", "functions"): "contracts",
    ("languages", "per_language"): "languages",
    ("environments",): None,
    ("scopes",): None,
}

# Keys whose value is a map of user-chosen names (field -> regex, signal ->
# bool, ...). The key itself is checked; its children are names, not keys.
NAME_VALUED = {"regex_checks", "field_types", "rule_weights", "context_drift_signals",
               "must_derive_from", "function_intents", "expect", "blocked", "allowed"}


def section_vocab(src_text):
    """Split the config parser into top-level sections and collect the keys
    each one reads. Approximate by design: a section's vocabulary is the union
    of every key string read between its `j.contains("<section>")` and the
    next section's."""
    lines = src_text.split("\n")
    starts = []
    for i, l in enumerate(lines):
        m = re.search(r'\bj\.contains\("([A-Za-z0-9_]+)"\)', l)
        if m:
            starts.append((i, m.group(1)))
    vocab, top = {}, set()
    for k, (ln, sec) in enumerate(starts):
        top.add(sec)
        end = starts[k + 1][0] if k + 1 < len(starts) else min(len(lines), ln + 400)
        txt = "\n".join(lines[ln:end])
        keys = set(re.findall(r'(?:contains|value|count|loadSimpleCheck)\(\s*"([A-Za-z0-9_]+)"', txt))
        keys |= set(re.findall(r'\[\s*"([A-Za-z0-9_]+)"\s*\]', txt))
        vocab.setdefault(sec, set()).update(keys)
    # agents_key resolves to "agents" at runtime; its block is keyed off that variable.
    if "agent_roles" in vocab:
        vocab.setdefault("agents", set()).update(vocab["agent_roles"])
    m = re.search(r'for \(auto& \[name, cfg_json\] : j\[agents_key\]\.items\(\)\)', src_text)
    if m:
        start = src_text[:m.start()].count("\n")
        block = "\n".join(lines[start:start + 400])
        vocab.setdefault("agents", set()).update(
            re.findall(r'(?:contains|value|count|loadSimpleCheck)\(\s*"([A-Za-z0-9_]+)"', block))
        vocab["agents"] |= set(re.findall(r'\[\s*"([A-Za-z0-9_]+)"\s*\]', block))
        top.add("agents")
    return top, vocab


def walk(obj, path, top, vocab, unread):
    if not isinstance(obj, dict):
        return
    for key, val in obj.items():
        if key.startswith("_"):
            continue  # _comment keys: documentation, deliberately unread
        here = path + (key,)
        if not path:
            if key not in top:
                unread.append(".".join(here))
                continue
            walk(val, here, top, vocab, unread)
            continue
        parent_named = NAMED_MAPS.get(path, "absent")
        if parent_named != "absent":
            # `key` is a user-chosen name; check the fields under it.
            if parent_named and isinstance(val, dict):
                for field in val:
                    if field.startswith("_"):
                        continue
                    if field not in vocab.get(parent_named, set()):
                        unread.append(".".join(here + (field,)))
                    elif isinstance(val[field], dict) and field not in NAME_VALUED:
                        walk(val[field], here + (field,), top, vocab, unread)
            continue
        if key not in vocab.get(path[0], set()):
            unread.append(".".join(here))
            continue
        if key in NAME_VALUED:
            continue
        walk(val, here, top, vocab, unread)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    repo = None
    if "--repo" in argv:
        repo = argv[argv.index("--repo") + 1]
        args = [a for a in args if a != repo]
    if len(args) != 1:
        sys.stderr.write(__doc__)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    repo = repo or os.path.normpath(os.path.join(here, "..", "..", ".."))
    parser_src = os.path.join(repo, "src", "runtime", "governance_config.cpp")
    try:
        src = open(parser_src, encoding="utf-8", errors="replace").read()
        cfg = json.load(open(args[0], encoding="utf-8"))
    except (OSError, ValueError) as e:
        print("UNMEASURABLE: " + str(e))
        return 2
    top, vocab = section_vocab(src)
    # "rationale" is read by the shared parseRationale() helper, which the
    # per-section segmentation cannot attribute; it is valid in every section.
    if "parseRationale(" in src:
        for sec in vocab:
            vocab[sec].add("rationale")
    if len(top) < 20:
        print("UNMEASURABLE: parser segmentation found only %d sections" % len(top))
        return 2
    unread = []
    walk(cfg, (), top, vocab, unread)
    if unread:
        print("UNREAD keys (the engine never reads these):")
        for u in unread:
            print("  " + u)
        return 1
    print("OK: every key is read by its section's parser (%d sections scanned)" % len(top))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
