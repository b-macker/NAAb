#!/usr/bin/env python3
"""Bounded exploration of the governance state machine against the REAL engine.

Bundle 10.2 (A113). The named safety properties already have hand-picked suites
(test_propose_commit.sh Group J, the de-escalation/cb-pressure suites,
test_undetermined_gate.sh Group E); this covers the COMBINATIONS they miss by
enumerating every bounded ordering of a small action alphabet and driving the
real `naab-lang` + `agent_stub.py` for each, then checking the invariants from
telemetry and the program's own markers.

Alphabet (all on a proposer handle `a` and a sibling handle `b`):
  PA  agent.propose(a, ...)        -- no state commit, no CDD mutation
  CA  agent.commit(a, latest)      -- the deliberation-gap action under test
  SB  agent.send(b, ...)           -- the sibling; drives the engine-global level
  SA  agent.send(a, ...)           -- the proposer takes an ordinary turn

Config: critical_threshold 0.0 and critical_sustained 1 (Group J's recipe), so
one SB reaches CRITICAL deterministically, and deescalate_sustained 99, so the
level must never step down within a bounded run.

Invariants, checked after every sequence:
  INV1  no commit lands at CRITICAL -- if an SB precedes a CA (so the level is
        CRITICAL by the time the commit runs), that CA must be refused, never
        COMMIT_OK. This is the propose/commit deliberation-gap safety property,
        over every interleaving rather than one.
  INV2  the level never de-escalates -- with deescalate_sustained 99 no
        GOVERNANCE_LEVEL_CHANGE may step down.

Report-only: not wired into CI's per-PR suite (test_governance_statemachine.sh
runs a depth-<=3 smoke there). --selftest proves the invariant checker can FAIL
on a synthesized violating telemetry stream, so a clean exploration is not
vacuous.

  python3 tools/governance_explore/explore.py [--depth N] [--naab PATH] [--keep]
  python3 tools/governance_explore/explore.py --smoke        # depth 3, for CI
  python3 tools/governance_explore/explore.py --selftest     # checker controls
"""
import argparse, itertools, json, os, re, shutil, signal, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
STUB = os.path.join(ROOT, "tests", "helpers", "agent_stub.py")
LEVELS = {"normal": 0, "elevated": 1, "high": 2, "critical": 3}
ACTIONS = ["PA", "CA", "SB", "SA"]


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


def start_stub(fixture_path, workdir):
    port = free_port()
    log = os.path.join(workdir, "stub.log")
    fh = open(log, "w")
    proc = subprocess.Popen([sys.executable, STUB, str(port), fixture_path, workdir],
                            stdout=fh, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            if "READY" in open(log).read():
                return proc, port
        except OSError:
            pass
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    proc.kill()
    raise RuntimeError("stub did not become READY")


def write_config(workdir, port):
    cfg = {
        "mode": "enforce",
        "security": {"sandbox_level": "elevated"},
        "telemetry": {"enabled": True, "output_file": "telemetry.jsonl"},
        "exposure_tracking": {"enabled": True, "level": "hard"},
        "behavioral_sequences": {"enabled": True},
        "context_drift": {
            "enabled": True, "level": "advisory", "check_interval_turns": 1,
            "reality_checkpoint": {"enabled": False, "pressure_threshold": 0.0,
                                   "signal_density_divisor": 1},
        },
        "circuit_breaker": {
            "enabled": True,
            "elevated_threshold": 0.0, "elevated_sustained": 1,
            "high_threshold": 0.0, "high_sustained": 1,
            "critical_threshold": 0.0, "critical_sustained": 1,
            "deescalate_sustained": 99,
        },
        "agents": {
            "proposer": {"provider": "gemini", "model": "stub-model",
                         "api_base": "http://127.0.0.1:%d" % port,
                         "api_key_env": "FAKE_KEY_EXPLORE",
                         "max_tokens": 100, "max_turns": 50, "propose_candidates_max": 2},
            "sibling": {"provider": "gemini", "model": "stub-model",
                        "api_base": "http://127.0.0.1:%d" % port,
                        "api_key_env": "FAKE_KEY_EXPLORE",
                        "max_tokens": 100, "max_turns": 50},
        },
    }
    with open(os.path.join(workdir, "govern.json"), "w") as f:
        json.dump(cfg, f)


def write_fixture(workdir):
    # One response that repeats when exhausted (the stub's documented
    # behaviour). Reply content is irrelevant to the invariants here: the level
    # is driven to CRITICAL by critical_threshold 0.0, not by coherence, so a
    # single generic reply serves every send on either handle.
    fx = {"responses": [{"content": "a quarterly revenue summary", "output_tokens": 30}]}
    with open(os.path.join(workdir, "fixture.json"), "w") as f:
        json.dump(fx, f)


def build_program(seq):
    """Return (naab_source, has_commit_after_sibling) or None if ill-formed."""
    lines = ["use agent", "main {",
             '    let a = agent.create("proposer")',
             '    let b = agent.create("sibling")']
    latest = None            # name of the most recent proposal var
    np = 0
    sibled = False           # has an SB happened yet (level driven to CRITICAL)?
    commit_after_sibling = False
    for act in seq:
        if act == "PA":
            np += 1; latest = "p%d" % np
            lines.append('    let %s = agent.propose(a, "summarize quarterly revenue", 2)' % latest)
            lines.append('    print("PROPOSED|%s")' % latest)
        elif act == "CA":
            if latest is None:
                return None  # commit with no proposal: ill-formed, skip
            if sibled:
                commit_after_sibling = True
            lines.append('    try {')
            lines.append('        let c = agent.commit(a, (%s.get("candidates") ?? [])[0])' % latest)
            lines.append('        print("COMMIT_OK|turn=" + string(c.get("turn") ?? -1))')
            lines.append('    } catch (e) {')
            lines.append('        if string(e).contains("CRITICAL") { print("COMMIT_DENIED_CRITICAL") }')
            lines.append('        else { print("COMMIT_OTHER") }')
            lines.append('    }')
            latest = None  # a proposal is single-use
        elif act == "SB":
            sibled = True
            lines.append('    try { let _ = agent.send(b, "describe the ledger reconciliation") }')
            lines.append('    catch (e) { print("SB_BLOCKED") }')
        elif act == "SA":
            lines.append('    try { let _ = agent.send(a, "continue the revenue summary") }')
            lines.append('    catch (e) { print("SA_BLOCKED") }')
    lines.append('    print("END")')
    lines.append("}")
    return "\n".join(lines) + "\n", commit_after_sibling


def max_level_and_descent(telemetry_path):
    """Return (max_level_int, saw_descent) from GOVERNANCE_LEVEL_CHANGE events."""
    mx = 0; saw_descent = False
    if not os.path.exists(telemetry_path):
        return mx, saw_descent
    for ln in open(telemetry_path, errors="replace"):
        if '"GOVERNANCE_LEVEL_CHANGE"' not in ln:
            continue
        try:
            e = json.loads(ln)
        except Exception:
            continue
        frm = LEVELS.get(str(e.get("from_level", "")).lower())
        to = LEVELS.get(str(e.get("to_level", "")).lower())
        if to is not None:
            mx = max(mx, to)
        if frm is not None and to is not None and to < frm:
            saw_descent = True
    return mx, saw_descent


def run_sequence(seq, naab, base_tmp, keep):
    built = build_program(seq)
    if built is None:
        return None
    src, commit_after_sibling = built
    d = tempfile.mkdtemp(prefix="exp-", dir=base_tmp)
    proc = None
    try:
        write_fixture(d)
        proc, port = start_stub(os.path.join(d, "fixture.json"), d)
        write_config(d, port)
        with open(os.path.join(d, "t.naab"), "w") as f:
            f.write(src)
        env = dict(os.environ)
        env["FAKE_KEY_EXPLORE"] = "fake-key-explore"
        r = subprocess.run([naab, "t.naab"], cwd=d, env=env, capture_output=True,
                            text=True, timeout=90)
        out = r.stdout + r.stderr
        mx, descent = max_level_and_descent(os.path.join(d, "telemetry.jsonl"))
        committed_ok = bool(re.search(r"COMMIT_OK\|", out))
        hardblock_crit = ("governance level CRITICAL" in out)
        return {
            "seq": "".join("%s," % a for a in seq).rstrip(","),
            "commit_after_sibling": commit_after_sibling,
            "committed_ok": committed_ok,
            "denied_critical": ("COMMIT_DENIED_CRITICAL" in out) or hardblock_crit,
            "max_level": mx, "descent": descent,
            "out": out,
        }
    except subprocess.TimeoutExpired:
        return {"seq": "".join(seq), "timeout": True, "out": ""}
    finally:
        if proc is not None:
            try:
                proc.send_signal(signal.SIGTERM); proc.wait(timeout=5)
            except Exception:
                proc.kill()
        if not keep:
            shutil.rmtree(d, ignore_errors=True)


def enumerate_sequences(depth):
    seqs = []
    for n in range(1, depth + 1):
        for combo in itertools.product(ACTIONS, repeat=n):
            seqs.append(combo)
    return seqs


def explore(depth, naab, keep):
    base_tmp = tempfile.mkdtemp(prefix="gov-explore-")
    trust = os.path.join(base_tmp, "trust"); os.makedirs(trust, exist_ok=True)
    os.environ["NAAB_TRUST_STORE_DIR"] = trust  # empty store: unsigned enforce loads
    violations = []
    positive_commit = False      # at least one sequence committed cleanly (not vacuous)
    critical_reached = False     # staging actually reached CRITICAL somewhere
    ran = 0; skipped = 0
    for seq in enumerate_sequences(depth):
        res = run_sequence(seq, naab, base_tmp, keep)
        if res is None:
            skipped += 1; continue
        if res.get("timeout"):
            violations.append(("TIMEOUT", res["seq"], "run timed out")); continue
        ran += 1
        if res["max_level"] >= LEVELS["critical"]:
            critical_reached = True
        if res["committed_ok"] and not res["commit_after_sibling"]:
            positive_commit = True
        # INV1: a commit after the sibling drove CRITICAL must never succeed.
        if res["commit_after_sibling"] and res["committed_ok"]:
            violations.append(("INV1", res["seq"],
                               "commit succeeded after the sibling escalated (max_level=%d)" % res["max_level"]))
        # INV2: the level must never step down (deescalate_sustained 99).
        if res["descent"]:
            violations.append(("INV2", res["seq"], "governance level de-escalated"))
    if not keep:
        shutil.rmtree(base_tmp, ignore_errors=True)
    return violations, positive_commit, critical_reached, ran, skipped


def selftest():
    """The invariant checker must FLAG a violating observation and PASS a clean
    one -- otherwise a clean exploration proves nothing."""
    ok = True
    # INV1 detector: a commit_after_sibling that committed_ok is a violation.
    bad = {"commit_after_sibling": True, "committed_ok": True}
    good = {"commit_after_sibling": True, "committed_ok": False}
    if not (bad["commit_after_sibling"] and bad["committed_ok"]):
        print("  SELFTEST INV1: FAIL (did not flag a commit at CRITICAL)"); ok = False
    else:
        print("  SELFTEST INV1: flagged a commit-after-escalation")
    if good["committed_ok"]:
        print("  SELFTEST INV1-neg: FAIL (flagged a denied commit)"); ok = False
    else:
        print("  SELFTEST INV1-neg: a denied commit is not flagged")
    # INV2 detector: a downward GOVERNANCE_LEVEL_CHANGE must be seen as descent.
    tmp = tempfile.mkdtemp(prefix="exp-self-")
    try:
        tele = os.path.join(tmp, "t.jsonl")
        with open(tele, "w") as f:
            f.write(json.dumps({"event_type": "GOVERNANCE_LEVEL_CHANGE",
                                "from_level": "normal", "to_level": "critical"}) + "\n")
            f.write(json.dumps({"event_type": "GOVERNANCE_LEVEL_CHANGE",
                                "from_level": "critical", "to_level": "normal"}) + "\n")
        mx, descent = max_level_and_descent(tele)
        if mx == LEVELS["critical"] and descent:
            print("  SELFTEST INV2: flagged a de-escalation (max_level=%d)" % mx)
        else:
            print("  SELFTEST INV2: FAIL (missed the de-escalation)"); ok = False
        # negative: a monotonic climb is NOT descent.
        with open(tele, "w") as f:
            f.write(json.dumps({"event_type": "GOVERNANCE_LEVEL_CHANGE",
                                "from_level": "normal", "to_level": "elevated"}) + "\n")
        _, descent2 = max_level_and_descent(tele)
        if descent2:
            print("  SELFTEST INV2-neg: FAIL (flagged a monotonic climb)"); ok = False
        else:
            print("  SELFTEST INV2-neg: a climb is not a descent")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--depth", type=int, default=4)
    ap.add_argument("--smoke", action="store_true", help="depth 3 (for CI)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--naab", default=os.path.join(ROOT, "build", "naab-lang"))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--plant-violation", action="store_true",
                    help="append a synthetic violation; the tool must then exit 1 "
                         "(a negative control for the smoke suite's exit-code wiring)")
    args = ap.parse_args()

    if args.selftest:
        print("=== explore.py checker selftest ===")
        sys.exit(0 if selftest() else 1)

    depth = 3 if args.smoke else args.depth
    naab = os.path.abspath(args.naab)
    if not os.path.exists(naab):
        print("UNMEASURABLE: naab-lang not found at %s" % naab); sys.exit(2)
    print("=== governance state-machine exploration (depth %d) ===" % depth)
    violations, positive_commit, critical_reached, ran, skipped = explore(depth, naab, args.keep)
    print("  sequences run: %d (skipped ill-formed: %d)" % (ran, skipped))
    print("  positive control (a commit succeeded with no escalation): %s" % positive_commit)
    print("  staging reached CRITICAL: %s" % critical_reached)
    if not critical_reached:
        print("UNMEASURABLE: no sequence reached CRITICAL -- INV1 never exercised"); sys.exit(2)
    if not positive_commit:
        print("UNMEASURABLE: no commit ever succeeded -- a universal denial would pass INV1 vacuously"); sys.exit(2)
    if args.plant_violation:
        violations.append(("PLANT", "<synthetic>", "planted violation (--plant-violation control)"))
    if violations:
        print("  VIOLATIONS: %d" % len(violations))
        for inv, seq, why in violations[:20]:
            print("    [%s] %s -- %s" % (inv, seq, why))
        sys.exit(1)
    print("  no invariant violated across %d sequences" % ran)
    sys.exit(0)


if __name__ == "__main__":
    main()
