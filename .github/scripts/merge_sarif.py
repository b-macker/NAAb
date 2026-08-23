#!/usr/bin/env python3
"""Merge per-file NAAb governance SARIF reports into one for code scanning.

WHY THIS IS NOT A CONCATENATION
-------------------------------
SARIF `result.ruleIndex` is an index into `runs[].tool.driver.rules`, and that
array is PER FILE. The governance job emits one SARIF per scanned `.naab`
source, so every file numbers its own rules from zero. Appending results from
file N into file 1's run without remapping leaves each of those results
pointing at whatever rule happens to sit at that position in the merged array.

That is not a cosmetic mismatch. GitHub validates `ruleId`/`ruleIndex`
consistency on ingest and rejects the whole document — the upload reports
"Error when processing the SARIF file" as a *neutral* check, the job it came
from stays green, and every governance finding is silently dropped. It stayed
that way unnoticed because nothing in the pipeline fails when it happens.

Reproduced before the fix, with the previous inline merger:

    merged rules: ['A', 'B', 'C']
      result ruleId=B ruleIndex=1 -> resolves to B   OK
      result ruleId=C ruleIndex=0 -> resolves to A   MISMATCH

So: merge the rules FIRST, build id -> index over the merged array, then rewrite
each incoming result's `ruleIndex` through it. Order matters — the previous
version extended results before appending rules, so even a correct remap would
have had nothing to remap against.

Run `merge_sarif.py --selftest` to exercise the invariants without a build.
"""

import glob
import json
import os
import sys

REPORT_DIR = ".governance-reports"
OUTPUT = os.path.join(REPORT_DIR, "governance.sarif")


def _driver(run):
    return run.setdefault("tool", {}).setdefault("driver", {})


def _rules(run):
    return _driver(run).setdefault("rules", [])


def _rule_id_for(result, src_rules):
    """The rule this result refers to, by id.

    Prefer the explicit `ruleId`. Fall back to resolving `ruleIndex` against the
    SOURCE run's rules — that is the only array the index was ever valid in, and
    resolving it after the merge is exactly the bug this script exists to fix.
    """
    rid = result.get("ruleId")
    if rid is not None:
        return rid
    idx = result.get("ruleIndex")
    if isinstance(idx, int) and 0 <= idx < len(src_rules):
        return src_rules[idx].get("id")
    return None


def merge(documents):
    """Merge a list of parsed SARIF documents into one. Pure; no I/O."""
    if not documents:
        return None

    merged = documents[0]
    if not merged.get("runs"):
        merged["runs"] = [{}]
    base = merged["runs"][0]
    base_rules = _rules(base)
    base.setdefault("results", [])

    # The first document is the base, so its own indices are already correct.
    # Everything after it gets remapped.
    index_by_id = {r["id"]: i for i, r in enumerate(base_rules) if "id" in r}

    for doc in documents[1:]:
        for run in doc.get("runs", []):
            src_rules = _rules(run)

            # Rules first: results are remapped against the MERGED array, so it
            # has to be complete before any remapping happens.
            for rule in src_rules:
                rid = rule.get("id")
                if rid is None or rid in index_by_id:
                    continue
                index_by_id[rid] = len(base_rules)
                base_rules.append(rule)

            for result in run.get("results", []):
                rid = _rule_id_for(result, src_rules)
                if rid is None:
                    # Nothing to anchor to. A stale index is worse than no
                    # index — GitHub falls back to ruleId, and an out-of-range
                    # or wrong-rule index is what gets the document rejected.
                    result.pop("ruleIndex", None)
                else:
                    result["ruleId"] = rid
                    if rid in index_by_id:
                        result["ruleIndex"] = index_by_id[rid]
                    else:
                        result.pop("ruleIndex", None)
                base["results"].append(result)

    return merged


def validate(doc):
    """Return a list of consistency problems. Empty list = clean.

    This is the check whose absence let the corruption ship: the merge step
    reported success while emitting a document GitHub would refuse.
    """
    problems = []
    for ri, run in enumerate(doc.get("runs", [])):
        rules = run.get("tool", {}).get("driver", {}).get("rules", [])
        ids = [r.get("id") for r in rules]
        seen = set()
        for rid in ids:
            if rid in seen:
                problems.append("runs[%d]: duplicate rule id %r" % (ri, rid))
            seen.add(rid)
        for xi, result in enumerate(run.get("results", [])):
            idx = result.get("ruleIndex")
            rid = result.get("ruleId")
            if idx is None:
                continue
            if not isinstance(idx, int) or not (0 <= idx < len(rules)):
                problems.append(
                    "runs[%d].results[%d]: ruleIndex %r out of range (%d rules)"
                    % (ri, xi, idx, len(rules)))
            elif rid is not None and ids[idx] != rid:
                problems.append(
                    "runs[%d].results[%d]: ruleIndex %d resolves to %r, ruleId is %r"
                    % (ri, xi, idx, ids[idx], rid))
    return problems


def _selftest():
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print("  %-4s %s%s" % ("PASS" if cond else "FAIL", name,
                               "" if cond else "  -> " + detail))
        if not cond:
            ok = False

    def doc(rules, results):
        return {"version": "2.1.0",
                "runs": [{"tool": {"driver": {"name": "t", "rules": rules}},
                          "results": results}]}

    # 1. The exact corruption this script was written for: a second file whose
    #    rules start at zero. Before the fix, ruleId=C/ruleIndex=0 resolved to A.
    m = merge([
        doc([{"id": "A"}, {"id": "B"}], [{"ruleId": "B", "ruleIndex": 1}]),
        doc([{"id": "C"}], [{"ruleId": "C", "ruleIndex": 0}]),
    ])
    rules = [r["id"] for r in m["runs"][0]["tool"]["driver"]["rules"]]
    res = m["runs"][0]["results"]
    check("T1 rules merged in order", rules == ["A", "B", "C"], str(rules))
    check("T1 second-file index remapped",
          rules[res[1]["ruleIndex"]] == "C",
          "ruleIndex=%r -> %r" % (res[1]["ruleIndex"], rules[res[1]["ruleIndex"]]))
    check("T1 first-file index untouched",
          rules[res[0]["ruleIndex"]] == "B", str(res[0]))
    check("T1 validates clean", validate(m) == [], str(validate(m)))

    # 2. NEGATIVE CONTROL. validate() must actually catch the corruption, or
    #    T1's clean bill of health means nothing. This is the pre-fix output
    #    shape: results appended verbatim against a merged rules array.
    broken = doc([{"id": "A"}, {"id": "B"}, {"id": "C"}],
                 [{"ruleId": "C", "ruleIndex": 0}])
    check("T2 validate catches a wrong index", len(validate(broken)) == 1,
          str(validate(broken)))
    out_of_range = doc([{"id": "A"}], [{"ruleId": "A", "ruleIndex": 7}])
    check("T2 validate catches out-of-range", len(validate(out_of_range)) == 1,
          str(validate(out_of_range)))

    # 3. Shared rules dedupe rather than duplicating, and both files' results
    #    land on the one surviving entry.
    m = merge([
        doc([{"id": "A"}], [{"ruleId": "A", "ruleIndex": 0}]),
        doc([{"id": "A"}, {"id": "B"}], [{"ruleId": "B", "ruleIndex": 1}]),
    ])
    rules = [r["id"] for r in m["runs"][0]["tool"]["driver"]["rules"]]
    check("T3 shared rule deduped", rules == ["A", "B"], str(rules))
    check("T3 validates clean", validate(m) == [], str(validate(m)))

    # 4. A result carrying only ruleIndex resolves through the SOURCE rules —
    #    the index is meaningless anywhere else.
    m = merge([
        doc([{"id": "A"}], []),
        doc([{"id": "Z"}], [{"ruleIndex": 0}]),
    ])
    r0 = m["runs"][0]["results"][0]
    check("T4 index-only result recovers its ruleId", r0.get("ruleId") == "Z", str(r0))
    check("T4 validates clean", validate(m) == [], str(validate(m)))

    # 5. A result with neither anchor keeps no index at all — a stale index is
    #    what gets the document rejected.
    m = merge([doc([{"id": "A"}], []), doc([], [{"message": {"text": "x"}}])])
    check("T5 unanchored result drops ruleIndex",
          "ruleIndex" not in m["runs"][0]["results"][0], str(m["runs"][0]["results"][0]))

    # 6. Degenerate inputs must not raise: a single file, and a base run with
    #    no rules key at all.
    single = doc([{"id": "A"}], [{"ruleId": "A", "ruleIndex": 0}])
    check("T6 single document passes through", validate(merge([single])) == [])
    m = merge([{"version": "2.1.0", "runs": [{}]}, doc([{"id": "A"}],
                                                       [{"ruleId": "A", "ruleIndex": 0}])])
    check("T6 base run without rules", validate(m) == [], str(validate(m)))
    check("T6 empty input returns None", merge([]) is None)

    print("\n  selftest: %s" % ("ALL PASSED" if ok else "FAILURES"))
    return 0 if ok else 1


def main(argv):
    if "--selftest" in argv:
        return _selftest()

    paths = sorted(glob.glob(os.path.join(REPORT_DIR, "sarif-*.json")))
    if not paths:
        return 0

    documents = []
    for path in paths:
        try:
            documents.append(json.load(open(path)))
        except Exception as exc:  # noqa: BLE001 - one bad file must not lose the rest
            print("Warning: failed to read %s: %s" % (path, exc), file=sys.stderr)

    merged = merge(documents)
    if merged is None:
        return 0

    # Fail loudly rather than uploading a document GitHub will reject with a
    # neutral check nobody reads.
    problems = validate(merged)
    if problems:
        print("SARIF merge produced an inconsistent document:", file=sys.stderr)
        for p in problems[:20]:
            print("  " + p, file=sys.stderr)
        return 1

    with open(OUTPUT, "w") as out:
        json.dump(merged, out, indent=2)
    print("Merged %d SARIF files into %s" % (len(paths), OUTPUT))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
