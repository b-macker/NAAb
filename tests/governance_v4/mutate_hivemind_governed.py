#!/usr/bin/env python3
"""Mutation harness for test_hivemind_governed.sh.

Breaks one guard at a time and records which assertions notice. An assertion
that never fails under any mutation is not testing anything — a green suite is
evidence only if it can go red for the right reason.

NOT part of run-all-tests.sh: it runs the whole suite once per mutation (16
mutations, several minutes). Run it after changing a gate or an assertion.

    python3 tests/governance_v4/mutate_hivemind_governed.py

Every open() here names UTF-8 explicitly. Python otherwise uses the LOCALE
encoding, which is cp1252 on the Windows runners — and cp1252 does not ERROR on
this file, it mojibakes it (54222 characters where UTF-8 reads 52968). This
harness reads the example and writes it back, so a locale round-trip would
silently corrupt the box-drawing comment banners in the mutated copy, and the
extraction the test then performs against that copy would fail for a reason
unrelated to the mutation. Same defect f5eea2b fixed in the test itself.

Exit 0 means every mutation was detected by exactly the assertions expected to
catch it, and no assertion survived all of them. Two findings came out of the
first run and are why it exists: E-03 grepped for the bare token CONTENT_GATE,
so renaming the event to a LONGER name left the assertion passing; and a
mutation setting filesystem.mode to "read_only" changed nothing, because
fsModeRank knows only "none" and "read" and anything else ranks as full write.
"""
import json, os, re, shutil, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = f"{REPO}/examples/hivemind_governed/src"
TEST = f"{REPO}/tests/governance_v4/test_hivemind_governed.sh"


def mut_script(text, old, new, count=1):
    assert text.count(old) == count, f"anchor found {text.count(old)}x, expected {count}"
    return text.replace(old, new)


MUTATIONS = {}


def mutation(name, expect):
    def deco(fn):
        MUTATIONS[name] = (fn, expect)
        return fn
    return deco


# ---- script-side gates ----------------------------------------------------
@mutation("quarantine_never_set", ["A-01", "E-05"])
def _(naab, cfg):
    return mut_script(naab, "    if len(inj.get(\"hits\")) > 0 { quarantine = true }",
                            "    if false { quarantine = true }"), cfg


@mutation("no_fence_discrimination", ["A-03", "E-07"])
def _(naab, cfg):
    return mut_script(naab, "                if in_fence {", "                if false {"), cfg


@mutation("no_quote_discrimination", ["A-02", "A-05", "E-06"])
def _(naab, cfg):
    # every quoted marker becomes an imperative hit — the naive scanner that
    # would have quarantined the audit's own section 2.4
    return mut_script(naab, "                    if quoted {", "                    if false {"), cfg


# A-04 asserts ordinary prose is NOT quarantined. Nothing above breaks it —
# the harness reported it as caught by no mutation, which means it was asserting
# nothing enforceable. This is the mutation it exists to catch: a marker test
# that matches every line, i.e. the scanner that flags all prose. Both the
# line-level and the imperative-position test have to go, because disabling only
# the first still routes plain prose to `observed` rather than to a hit.
@mutation("marker_matches_every_line", ["A-04"])
def _(naab, cfg):
    naab = mut_script(naab, "            if string.contains(lower, marker) {",
                            "            if true {")
    naab = mut_script(naab, "                        if string.contains(head, marker) {",
                            "                        if true {")
    return naab, cfg


@mutation("no_observed_recording", ["A-05", "E-06"])
def _(naab, cfg):
    return mut_script(naab,
        "                        observed.push({marker: marker, line: i, context: \"quoted\"})",
        "                        let _drop = 0"), cfg


@mutation("prefix_only_redaction", ["B-01"])
def _(naab, cfg):
    # the original bug: replace the marker, leave the key body in the log
    return mut_script(naab,
        "        if hit {\n            out.push(\"[REDACTED-CREDENTIAL]\")",
        "        if hit {\n            out.push(string.replace(w, found[0], \"[REDACTED-CREDENTIAL]\"))"), cfg


@mutation("index_always_valid", ["B-03", "B-04"])
def _(naab, cfg):
    return mut_script(naab,
        "fn validate_temp_index(idx) {\n    let s = string.trim(string(idx))",
        "fn validate_temp_index(idx) {\n    if true { return true }\n    let s = string.trim(string(idx))"), cfg


@mutation("index_always_invalid", ["B-02"])
def _(naab, cfg):
    return mut_script(naab,
        "fn validate_temp_index(idx) {\n    let s = string.trim(string(idx))",
        "fn validate_temp_index(idx) {\n    if true { return false }\n    let s = string.trim(string(idx))"), cfg


@mutation("no_collusion_detection", ["B-05"])
def _(naab, cfg):
    return mut_script(naab,
        "fn detect_response_collusion(hashes, names) {\n    let dupes = []",
        "fn detect_response_collusion(hashes, names) {\n    if true { return [] }\n    let dupes = []"), cfg


@mutation("no_content_gate_telemetry", ["E-03", "E-05", "E-06", "E-07"])
def _(naab, cfg):
    return mut_script(naab, 'event: "CONTENT_GATE",', 'event: "GATE_CONTENT",'), cfg


@mutation("no_full_prompt_logging", ["E-04"])
def _(naab, cfg):
    return mut_script(naab, "                full_prompt: prompts[wi],\n", ""), cfg


# ---- engine-side gates (config) -------------------------------------------
@mutation("taint_sink_removed", ["C-01"])
def _(naab, cfg):
    cfg["taint_tracking"]["sinks"] = ["shell_exec", "env.set_var"]
    return naab, cfg


@mutation("sanitizer_prefix_removed", ["C-02"])
def _(naab, cfg):
    cfg["taint_tracking"]["sanitizers"] = ["validate_", "escape_"]
    return naab, cfg


@mutation("bsd_disabled", ["D-01"])
def _(naab, cfg):
    cfg["behavioral_sequences"]["enabled"] = False
    return naab, cfg


@mutation("bsd_patterns_removed", ["D-01"])
def _(naab, cfg):
    cfg["behavioral_sequences"]["patterns"] = []
    return naab, cfg


# ---- controls: these must break the CONTROL assertions, proving they bind --
@mutation("all_process_blocked", ["D-02"])
def _(naab, cfg):
    cfg["capabilities"]["shell"]["enabled"] = False
    return naab, cfg


@mutation("all_writes_blocked", ["C-03"])
def _(naab, cfg):
    cfg["capabilities"]["filesystem"]["mode"] = "read"
    return naab, cfg


def run(src_dir):
    env = dict(os.environ, HIVEMIND_GOVERNED_SRC=src_dir)
    p = subprocess.run(["bash", TEST], capture_output=True, text=True, env=env, timeout=900)
    failed = set(re.findall(r"FAIL \[([A-Z]-\d+)", p.stdout))
    passed = set(re.findall(r"PASS \[([A-Z]-\d+)", p.stdout))
    return failed, passed


def main():
    base_fail, base_pass = run(SRC)
    print(f"baseline: {len(base_pass)} pass, {len(base_fail)} fail")
    if base_fail:
        print(f"  ABORT — baseline is not clean: {sorted(base_fail)}")
        return 1
    all_ids = sorted(base_pass)
    caught_by = {i: [] for i in all_ids}
    rows = []

    for name, (fn, expect) in MUTATIONS.items():
        with tempfile.TemporaryDirectory() as td:
            work = os.path.join(td, "src")
            shutil.copytree(SRC, work)
            npath = os.path.join(work, "hivemind-governed.naab")
            cpath = os.path.join(work, "govern.json")
            naab = open(npath, encoding="utf-8").read()
            cfg = json.load(open(cpath, encoding="utf-8"))
            try:
                naab, cfg = fn(naab, cfg)
            except AssertionError as e:
                rows.append((name, "ANCHOR-MISS", str(e), expect))
                continue
            open(npath, "w", encoding="utf-8").write(naab)
            json.dump(cfg, open(cpath, "w", encoding="utf-8"), indent=2)
            failed, _ = run(work)
        for i in failed:
            caught_by.setdefault(i, []).append(name)
        missed = [e for e in expect if e not in failed]
        extra = sorted(failed - set(expect))
        status = "OK" if not missed else "NOT-DETECTED"
        rows.append((name, status, f"missed={missed} also_failed={extra}", expect))

    print("\n=== mutation results ===")
    for name, status, detail, expect in rows:
        mark = "  " if status == "OK" else "!!"
        print(f"{mark} {status:14s} {name:28s} expected {expect}  {detail}")

    print("\n=== assertions never caught by any mutation ===")
    dead = [i for i in all_ids if not caught_by[i]]
    print("  " + (", ".join(dead) if dead else "none — every assertion failed under at least one mutation"))
    return 1 if (dead or any(r[1] != "OK" for r in rows)) else 0


sys.exit(main())
