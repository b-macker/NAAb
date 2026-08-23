#!/usr/bin/env python3
"""Score candidate definitions of escalation_effectiveness against the scenarios.

Ground truth is RE-DERIVED from the measured penalty total either side of the
escalation, never taken from the scenario's label. A scenario whose measurement
contradicts its label is reported INVALID and excluded: a mislabelled scenario
silently rewards whichever definition shares its error, which is how the pilot
nearly concluded the opposite of the truth.
"""
import json
import os
import re
import sys

LEVEL = {"normal": 0, "elevated": 1, "high": 2, "critical": 3}


def load(d):
    """-> (rows[(turn, coherence, penalty)], first_escalation_turn)"""
    p = os.path.join(d, "t.jsonl")
    rows, esc = [], None
    if not os.path.exists(p):
        return rows, esc
    for ln in open(p):
        try:
            e = json.loads(ln)
        except Exception:
            continue
        t = e.get("event_type")
        if t == "GOVERNANCE_LEVEL_CHANGE" and esc is None:
            if LEVEL.get(str(e.get("to_level")), 0) > LEVEL.get(str(e.get("from_level")), 0):
                esc = int(e.get("turn", 0))
        if t == "CDD_TURN" and e.get("analyzed") == "true":
            pen = sum(float(m.group(1))
                      for m in re.finditer(r"=([0-9.]+)", e.get("penalties_detail", "") or ""))
            rows.append((int(e.get("turn", 0)), float(e.get("coherence", 1.0)), pen))
    return sorted(rows), esc


def derive_truth(rows, esc, horizon=8):
    """Ground truth from measured penalty rate, over a horizon long enough to
    contain any response lag. Deliberately wider than the candidate windows:
    the truth should not depend on the parameter being fitted."""
    m = {t: (c, p) for t, c, p in rows}
    pre = [m[t][1] for t in range(esc - horizon, esc) if t in m]
    post = [m[t][1] for t in range(esc + 1, esc + 1 + horizon) if t in m]
    if not pre or not post:
        return None, None, None
    a, b = sum(pre) / len(pre), sum(post) / len(post)
    if b < a - 0.03:
        v = "POSITIVE"
    elif b > a + 0.03:
        v = "NEGATIVE"
    else:
        v = "ZERO"
    return v, a, b


def candidates(rows, esc, off, win):
    m = {t: (c, p) for t, c, p in rows}
    pre = [t for t in range(esc - win, esc) if t in m]
    post = [t for t in range(esc + off, esc + off + win) if t in m]
    if not pre or not post or esc not in m:
        return None
    out = {}
    out["A_coherence_delta"] = sum(m[t][0] for t in post) / len(post) - m[esc][0]
    out["B_coherence_slope"] = m[post[-1]][0] - m[post[0]][0]
    pp = sum(m[t][1] for t in pre) / len(pre)
    qp = sum(m[t][1] for t in post) / len(post)
    out["C_penalty_rate"] = pp - qp
    out["D_penalty_ratio"] = (pp - qp) / max(pp, 0.01)
    return out


def cls(v, eps):
    return "POSITIVE" if v > eps else ("NEGATIVE" if v < -eps else "ZERO")


EPS = {"A_coherence_delta": 0.05, "B_coherence_slope": 0.03,
       "C_penalty_rate": 0.03, "D_penalty_ratio": 0.15}


def main(work, metap):
    meta = json.load(open(metap))
    truths, invalid, noesc = {}, [], []
    print("")
    print("  %-18s %-9s %-9s %-7s %-7s %s" %
          ("scenario", "intended", "measured", "pre", "post", "status"))
    print("  " + "-" * 78)
    for name in sorted(meta["scenarios"]):
        d = os.path.join(work, name)
        rows, esc = load(d)
        intended = meta["scenarios"][name]["intended"]
        if not rows:
            print("  %-18s %-9s %s" % (name, intended, "NO TELEMETRY"))
            invalid.append(name)
            continue
        if esc is None:
            status = "OK (control)" if intended == "NONE" else "NO ESCALATION"
            print("  %-18s %-9s %-9s %-7s %-7s %s" % (name, intended, "-", "-", "-", status))
            (noesc if intended != "NONE" else []).append(name) if intended != "NONE" else None
            if intended != "NONE":
                noesc.append(name)
            continue
        # HORIZON STABILITY. A verdict that changes with the derivation
        # horizon is an artifact of that choice, not a property of the
        # scenario, and scoring definitions against it rewards whichever one
        # shares the artifact. Two scenarios in the first full run flipped
        # ZERO -> NEGATIVE between horizon 8 and 10.
        verdicts = {derive_truth(rows, esc, horizon=h)[0] for h in (4, 6, 8, 10, 12)}
        verdicts.discard(None)
        if len(verdicts) > 1:
            print("  %-18s %-9s %-9s %-7s %-7s %s"
                  % (name, intended, "/".join(sorted(verdicts)), "-", "-",
                     "HORIZON-UNSTABLE -> excluded"))
            invalid.append(name)
            continue
        v, a, b = derive_truth(rows, esc)
        if v is None:
            print("  %-18s %-9s %s" % (name, intended, "NO WINDOW"))
            invalid.append(name)
            continue
        ok = (v == intended)
        print("  %-18s %-9s %-9s %-7.3f %-7.3f %s" %
              (name, intended, v, a, b, "OK" if ok else "MISLABELLED -> excluded"))
        if ok:
            truths[name] = (rows, esc, v)
        else:
            invalid.append(name)

    print("")
    print("  %d scenario(s) usable, %d excluded, %d never escalated"
          % (len(truths), len(invalid), len(noesc)))
    # DEGENERACY GATE. If every surviving scenario shares one verdict, a
    # definition that always returns that verdict scores perfectly, and the
    # comparison is meaningless. This is not a technicality: the first full run
    # left exactly two usable scenarios, both NEGATIVE.
    distinct = {t for _, _, t in truths.values()}
    if len(truths) < 3 or len(distinct) < 2:
        print("")
        print("  CANNOT COMPARE DEFINITIONS.")
        print("    usable scenarios: %d, distinct verdicts among them: %s"
              % (len(truths), sorted(distinct) or "none"))
        print("    A comparison needs at least three usable scenarios covering")
        print("    at least two verdicts; otherwise a definition that answers")
        print("    the same way every time scores perfectly.")
        if "POSITIVE" not in distinct:
            print("")
            print("    NOTE: no POSITIVE scenario survived validation. Every")
            print("    'helped' variant measured ZERO or NEGATIVE, which is")
            print("    itself the finding: with the pre-window anchored at the")
            print("    escalation turn, it spans back into the pre-drift period")
            print("    where the penalty rate is 0, while the post-window still")
            print("    contains the agent's response lag. Both push a genuine")
            print("    recovery toward 'no change' or 'worse'. The ANCHOR is")
            print("    wrong, not only the quantity — which is a stronger claim")
            print("    than 'use penalty rate instead of coherence' and applies")
            print("    to any measure taken relative to the escalation turn.")
        return 1

    print("")
    print("  Searching offset 1..6 x window 2..8 for definitions that classify")
    print("  every usable scenario correctly:")
    print("")
    found = {}
    for kind in ("A_coherence_delta", "B_coherence_slope", "C_penalty_rate", "D_penalty_ratio"):
        sols = []
        for off in range(1, 7):
            for win in range(2, 9):
                ok = 0
                for name, (rows, esc, truth) in truths.items():
                    c = candidates(rows, esc, off, win)
                    if not c:
                        ok = -99
                        break
                    if cls(c[kind], EPS[kind]) == truth:
                        ok += 1
                if ok == len(truths):
                    sols.append((off, win))
        found[kind] = sols
        if sols:
            offs = sorted({o for o, _ in sols})
            wins = sorted({w for _, w in sols})
            print("    %-20s %2d solution(s)   offsets %s  windows %s"
                  % (kind, len(sols), offs, wins))
        else:
            print("    %-20s NONE — no offset/window in the space classifies all %d"
                  % (kind, len(truths)))

    print("")
    print("  READING THIS")
    print("    A negative result here is the strong one: a definition with NO")
    print("    solution anywhere in the space is not mistuned, it is measuring")
    print("    the wrong quantity, and no amount of parameter choice fixes it.")
    print("")
    print("    A definition with solutions is NOT thereby validated. With %d"
          % len(truths))
    print("    scenarios and a 42-point search, a narrow solution — one that")
    print("    works at a single window size — is as consistent with overfitting")
    print("    as with correctness. Treat the range of solutions as the evidence:")
    print("    a definition that works across many offsets and windows is robust;")
    print("    one that works at exactly one point is a coincidence until more")
    print("    scenarios say otherwise.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
