#!/usr/bin/env python3
"""Contract evaluator for test_signal_contract.sh.

Kept in its own file, and driven by --selftest, for one reason: an evaluator
embedded in the shell script can only be exercised against the live engine,
where a passing result is ambiguous between "the engine complies" and "the
check cannot fail". Here every contract is run against synthetic tables that
violate it, so the checker's ability to fail is established before it is
pointed at anything real.

Input (stdin, JSON):
  {
    "oa_threshold": 0.70,
    "expected_turns": 8,
    "correct_arms": ["ctl_narrow", "ctl_varied", "ctl_verbose"],
    "drift_arms":   ["drift_repeat", "drift_abandon", "drift_parrot"],
    "per_signal": { "<signal>": { "<arm>": {"fires": int, "turns": int} } },
    "ensemble":   { "<arm>": {"floor": float, "turns": int, "done": bool} }
  }

Output (stdout, JSON): {"violations": [...], "silent": [...], "live": [...],
                        "notes": [...]}
Each violation is {"id": "C1"|..., "subject": str, "detail": str}.
"""
import json
import sys

EPS = 1e-9


def _rate(cell):
    """Firing rate, or None when there is no denominator.

    V5: a zero-turn arm must never become a 0.0 rate. A crashed arm that
    silently reads as "fired 0% of the time" is indistinguishable from a
    signal that behaved, and it passes C1.
    """
    turns = cell.get("turns", 0)
    if not turns:
        return None
    return cell.get("fires", 0) / float(turns)


def evaluate(t):
    oa = t["oa_threshold"]
    correct = t["correct_arms"]
    drift = t["drift_arms"]
    per_signal = t.get("per_signal", {})
    ensemble = t.get("ensemble", {})

    violations = []
    silent = []
    live = []
    notes = []

    def v(cid, subject, detail):
        violations.append({"id": cid, "subject": subject, "detail": detail})

    # ---- V1: every arm ran, on both the per-signal and ensemble paths ----
    expected = t["expected_turns"]
    for arm, cell in sorted(ensemble.items()):
        if not cell.get("done"):
            v("V1", arm, "ensemble arm did not reach RUN_DONE")
        if cell.get("turns", 0) != expected:
            v("V1", arm, "ensemble arm analyzed %d turns, expected %d"
              % (cell.get("turns", 0), expected))
    for sig, arms in sorted(per_signal.items()):
        for arm, cell in sorted(arms.items()):
            if cell.get("turns", 0) != expected:
                v("V1", "%s/%s" % (sig, arm),
                  "analyzed %d turns, expected %d" % (cell.get("turns", 0), expected))

    # ---- C1: no signal fires more on correct work than on every drift arm ----
    for sig, arms in sorted(per_signal.items()):
        crates = [_rate(arms.get(a, {})) for a in correct]
        drates = [_rate(arms.get(a, {})) for a in drift]
        if any(r is None for r in crates + drates):
            # V5. Already reported as V1; do not also emit a bogus C1.
            notes.append("%s: rate undefined on some arm, C1 not evaluated" % sig)
            continue
        total_fires = sum(arms.get(a, {}).get("fires", 0) for a in correct + drift)
        if total_fires == 0:
            silent.append(sig)
            continue
        live.append(sig)
        max_c, max_d = max(crates), max(drates)
        # STRICT separation, not merely "not inverted".
        #
        # An earlier draft required only max_c <= max_d, which passed a signal
        # firing 88% on correct work and 88% on topic abandonment — and three
        # signals did exactly that on the real matrix. Equal rates mean the
        # signal carries no information about drift (P(fire|drift) ==
        # P(fire|correct) is zero mutual information) while still charging the
        # shared coherence budget on every normal turn. That is the failure
        # this gate exists to catch, so equality must fail.
        #
        # The bar is strict inequality and nothing more: no margin constant,
        # because any particular margin would be arbitrary, and the aggregate
        # cost of a barely-separating signal is already bounded by C2.
        if max_c >= max_d - EPS:
            worst = correct[crates.index(max_c)]
            rel = "as often as" if abs(max_c - max_d) <= EPS else "more often than"
            v("C1", sig,
              "fires %.0f%% on %s — %s on any drift arm (best drift %.0f%%), "
              "so it carries no information about drift"
              % (max_c * 100, worst, rel, max_d * 100))

    # ---- V3: an inert CDD must not pass C1/C3 by having nothing to say ----
    min_live = t.get("min_live_signals", 3)
    if len(live) < min_live:
        v("V3", "matrix",
          "only %d signal(s) fired anywhere; need >= %d for the per-signal "
          "contracts to mean anything" % (len(live), min_live))

    # ---- C2: correct work must not exhaust the coherence budget ----
    for arm in correct:
        cell = ensemble.get(arm)
        if not cell:
            v("C2", arm, "no ensemble measurement")
            continue
        if cell["floor"] < oa - EPS:
            v("C2", arm, "coherence fell to %.3f on correct work (< %.2f)"
              % (cell["floor"], oa))

    # ---- C4: every drift arm must still be caught ----
    for arm in drift:
        cell = ensemble.get(arm)
        if not cell:
            v("C4", arm, "no ensemble measurement")
            continue
        if cell["floor"] >= oa - EPS:
            v("C4", arm, "coherence held at %.3f on drift (>= %.2f) — undetected"
              % (cell["floor"], oa))

    # ---- C3: quoting the mandate must not beat doing the work ----
    # Stated against the WORST correct arm: parroting must not outscore even
    # the correct arm the engine treats least kindly. Using the best correct
    # arm instead would let a signal that mauls progressive work call parroting
    # "not better" purely because real work scored so badly.
    if "drift_parrot" in ensemble:
        p = ensemble["drift_parrot"]["floor"]
        cfloors = {a: ensemble[a]["floor"] for a in correct if a in ensemble}
        if cfloors:
            worst_arm = min(cfloors, key=lambda a: cfloors[a])
            if p > cfloors[worst_arm] + EPS:
                v("C3", "ensemble",
                  "parroting the mandate ends at coherence %.3f, above correct "
                  "work on %s at %.3f — restating the task outscores doing it"
                  % (p, worst_arm, cfloors[worst_arm]))
    return {"violations": violations, "silent": sorted(silent),
            "live": sorted(live), "notes": notes}


# --------------------------------------------------------------------------
# V4: the evaluator must FAIL on data known to violate each contract, and must
# PASS on data known to satisfy all of them. Both directions are required —
# a checker that always reports a violation passes the first half alone.
# --------------------------------------------------------------------------
def _base():
    """A table that satisfies every contract. Each negative case below mutates
    exactly one thing away from this."""
    good_sig = {
        "ctl_narrow": {"fires": 0, "turns": 8},
        "ctl_varied": {"fires": 0, "turns": 8},
        "ctl_verbose": {"fires": 0, "turns": 8},
        "drift_repeat":         {"fires": 7, "turns": 8},
        "drift_abandon":        {"fires": 6, "turns": 8},
        "drift_parrot":         {"fires": 5, "turns": 8},
    }
    return {
        "oa_threshold": 0.70,
        "expected_turns": 8,
        "correct_arms": ["ctl_narrow", "ctl_varied", "ctl_verbose"],
        "drift_arms": ["drift_repeat", "drift_abandon", "drift_parrot"],
        "min_live_signals": 3,
        "per_signal": {"sig_a": json.loads(json.dumps(good_sig)),
                       "sig_b": json.loads(json.dumps(good_sig)),
                       "sig_c": json.loads(json.dumps(good_sig))},
        "ensemble": {
            "ctl_narrow": {"floor": 0.95, "turns": 8, "done": True},
            "ctl_varied": {"floor": 0.88, "turns": 8, "done": True},
            "ctl_verbose": {"floor": 0.91, "turns": 8, "done": True},
            "drift_repeat":         {"floor": 0.20, "turns": 8, "done": True},
            "drift_abandon":        {"floor": 0.15, "turns": 8, "done": True},
            "drift_parrot":         {"floor": 0.30, "turns": 8, "done": True},
        },
    }


def _ids(res):
    return sorted({x["id"] for x in res["violations"]})


def selftest():
    failures = []

    def check(name, table, expect_id):
        res = evaluate(table)
        got = _ids(res)
        if expect_id is None:
            if got:
                failures.append("%s: expected clean, got %s" % (name, got))
        elif expect_id not in got:
            failures.append("%s: expected %s among violations, got %s"
                            % (name, expect_id, got))

    # Positive control. Without it, an evaluator that reports every contract
    # violated on every input would pass all the negative cases below.
    check("clean baseline reports nothing", _base(), None)

    # C1 — a signal that fires on correct work and never on drift.
    t = _base()
    t["per_signal"]["sig_a"]["ctl_varied"] = {"fires": 8, "turns": 8}
    t["per_signal"]["sig_a"]["drift_repeat"] = {"fires": 0, "turns": 8}
    t["per_signal"]["sig_a"]["drift_abandon"] = {"fires": 0, "turns": 8}
    t["per_signal"]["sig_a"]["drift_parrot"] = {"fires": 0, "turns": 8}
    check("C1 catches inversion", t, "C1")

    # C1 must NOT fire on a signal that is noisy but genuinely separating.
    # Without this case, tightening C1 to "flag everything" would pass.
    t = _base()
    t["per_signal"]["sig_a"]["ctl_varied"] = {"fires": 5, "turns": 8}
    t["per_signal"]["sig_a"]["drift_repeat"] = {"fires": 6, "turns": 8}
    check("C1 tolerates noisy-but-separating", t, None)

    # C1 catches EQUAL rates. This is the case the first draft of this gate
    # let through: firing identically on correct work and on drift is zero
    # mutual information, not "not inverted".
    t = _base()
    for arm in ("ctl_narrow", "ctl_varied", "ctl_verbose",
                "drift_repeat", "drift_abandon", "drift_parrot"):
        t["per_signal"]["sig_a"][arm] = {"fires": 7, "turns": 8}
    check("C1 catches equal rates on correct and drift", t, "C1")

    # C2 — coherence floored by correct work.
    t = _base()
    t["ensemble"]["ctl_varied"]["floor"] = 0.0
    check("C2 catches floored correct work", t, "C2")

    # C3 — parroting ends above correct work.
    t = _base()
    t["ensemble"]["drift_parrot"]["floor"] = 0.99
    check("C3 catches rewarded parroting", t, "C3")

    # C4 — a drift arm sails through.
    t = _base()
    t["ensemble"]["drift_repeat"]["floor"] = 0.99
    check("C4 catches undetected drift", t, "C4")

    # C4 is what stops "disable everything" from passing: an inert engine
    # leaves every arm at full coherence.
    t = _base()
    for arm in t["ensemble"]:
        t["ensemble"][arm]["floor"] = 1.0
    for sig in t["per_signal"]:
        for arm in t["per_signal"][sig]:
            t["per_signal"][sig][arm] = {"fires": 0, "turns": 8}
    res = evaluate(t)
    got = _ids(res)
    for need in ("C4", "V3"):
        if need not in got:
            failures.append("inert engine: expected %s among violations, got %s"
                            % (need, got))

    # V1 — an arm that never completed.
    t = _base()
    t["ensemble"]["drift_repeat"]["done"] = False
    check("V1 catches an arm that never finished", t, "V1")

    # V1 — an arm short of its expected turns.
    t = _base()
    t["per_signal"]["sig_a"]["drift_repeat"] = {"fires": 0, "turns": 3}
    check("V1 catches a truncated arm", t, "V1")

    # V5 — zero denominator must not silently become a 0.0 rate that passes C1.
    t = _base()
    t["per_signal"]["sig_a"]["ctl_varied"] = {"fires": 0, "turns": 0}
    res = evaluate(t)
    if "C1" in _ids(res):
        failures.append("V5: zero-turn arm produced a C1 verdict instead of "
                        "being excluded")
    if not any("C1 not evaluated" in n for n in res["notes"]):
        failures.append("V5: zero-turn arm was not reported as unevaluable")

    for f in failures:
        print("SELFTEST FAIL: " + f)
    print("selftest: %d checks, %d failures" % (12, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    print(json.dumps(evaluate(json.load(sys.stdin)), indent=1))
