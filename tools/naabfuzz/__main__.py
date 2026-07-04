"""CLI: python3 -m naabfuzz {gen,repro,fuzz,oracle,properties,minimize,selftest}"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

from . import gen as G
from .emitter import Emitter
from .oracle import Oracle, OracleGap, OracleResult
from .runner import run_both
from .triage import triage, Finding


def load_known_findings(path):
    known = set()
    if path and os.path.exists(path):
        with open(path) as f:
            for ln in f:
                ln = ln.split("#", 1)[0].strip()
                if ln:
                    known.add(ln.split()[0])
    return known


def oracle_or_none(prog):
    try:
        return Oracle().run(prog)
    except OracleGap as e:
        return None


def eval_seed(naab_bin: str, seed: int, known_detectors=None) -> Finding:
    prog = G.generate(seed)
    src = Emitter().program(prog)
    oracle = oracle_or_none(prog)
    vm, tw = run_both(naab_bin, src)
    if oracle is None:
        # Differential-only fallback: no oracle attribution possible.
        # Engines agreeing is a pass; disagreement is still severity 1
        # (reported as DOUBLE_MISMATCH — read as "unattributed divergence").
        from .triage import engines_agree
        f = triage(vm, tw, OracleResult(lines=[], error=None), known_detectors)
        if engines_agree(vm, tw) and f.classification not in ("CRASH", "HANG"):
            return Finding("PASS_DIFF_ONLY", "", "", seed)
        f.seed = seed
        return f
    f = triage(vm, tw, oracle, known_detectors)
    f.seed = seed
    return f


def cmd_gen(args):
    prog = G.generate(args.seed)
    sys.stdout.write(Emitter(paren_all=args.paren_all).program(prog))
    return 0


def cmd_repro(args):
    prog = G.generate(args.seed)
    src = Emitter().program(prog)
    print("=== program (seed %d) ===" % args.seed)
    print(src)
    oracle = oracle_or_none(prog)
    if oracle is None:
        print("=== oracle: GAP (differential-only) ===")
    else:
        print("=== oracle ===")
        for ln in oracle.lines:
            print("  %s%s" % (ln.text, "  [~4ulp]" if ln.approx else ""))
        if oracle.error:
            print("  ERROR: %s" % oracle.error)
    vm, tw = run_both(args.naab, src)
    for name, r in (("vm", vm), ("tw", tw)):
        print("=== %s rc=%d%s ===" % (name, r.rc,
                                      " TIMEOUT" if r.timed_out else ""))
        for ln in r.out_lines:
            print("  %s" % ln)
        if r.error_text:
            print("  ERROR: %s" % r.error_text.split(chr(10))[0][:160])
    f = eval_seed(args.naab, args.seed)
    print("=== triage: %s sig=%s ===" % (f.classification, f.signature))
    if f.detail:
        print("  " + f.detail)
    return 0 if f.severity > 2 else 1


def cmd_fuzz(args):
    known = load_known_findings(args.known_findings)
    findings = []
    stats = {}
    t0 = time.time()
    seeds = []
    if args.seeds:
        a, b = args.seeds.split("..")
        seeds = list(range(int(a), int(b) + 1))
    n_run = 0
    new_sev1 = []
    out_path = args.findings or "findings.jsonl"
    out_f = open(out_path, "w") if args.findings is not None else None
    seen_sigs = set()
    i = 0
    while True:
        if seeds:
            if i >= len(seeds):
                break
            seed = seeds[i]
        else:
            if args.time_budget and time.time() - t0 > args.time_budget:
                break
            if args.count and i >= args.count:
                break
            seed = (args.seed_base or 0) * 1_000_000 + i
        i += 1
        n_run += 1
        f = eval_seed(args.naab, seed)
        stats[f.classification] = stats.get(f.classification, 0) + 1
        if f.classification in ("PASS", "PASS_DIFF_ONLY", "FLOAT_TOLERANCE",
                                "KNOWN"):
            continue
        if f.signature in seen_sigs:
            continue
        seen_sigs.add(f.signature)
        findings.append(f)
        rec = {"seed": f.seed, "classification": f.classification,
               "signature": f.signature, "severity": f.severity,
               "detail": f.detail}
        if out_f:
            out_f.write(json.dumps(rec) + "\n")
            out_f.flush()
        print("FINDING %s sev=%d sig=%s seed=%s\n  %s"
              % (f.classification, f.severity, f.signature, f.seed, f.detail))
        if f.severity == 1 and f.signature not in known:
            new_sev1.append(f)
    if out_f:
        out_f.close()
    print("=== naabfuzz: %d seeds in %.1fs ===" % (n_run, time.time() - t0))
    for k in sorted(stats):
        print("  %-22s %d" % (k, stats[k]))
    if new_sev1:
        print("NEW severity-1 signatures (not in known_findings):")
        for f in new_sev1:
            print("  %s %s seed=%s" % (f.signature, f.classification, f.seed))
        return 1
    return 0


def cmd_minimize(args):
    from .minimize import Minimizer
    prog = G.generate(args.seed)
    target = eval_seed(args.naab, args.seed)
    if target.classification in ("PASS", "PASS_DIFF_ONLY", "FLOAT_TOLERANCE"):
        print("seed %d does not fail (%s); nothing to minimize"
              % (args.seed, target.classification))
        return 0
    m = Minimizer(args.naab, target)
    small = m.minimize(prog)
    print("=== minimized (%d executions) — %s sig=%s ==="
          % (m.executions, target.classification, target.signature))
    sys.stdout.write(Emitter().program(small))
    return 0


def cmd_oracle(args):
    """Fixed deterministic vector set + full property suite on both engines."""
    from .properties import run_properties
    from .vectors import run_vectors
    print("--- oracle: fixed arithmetic vectors ---")
    v_total, v_fail = run_vectors(args.naab)
    print("  vectors: %d run, %d failed" % (v_total, v_fail))
    print("--- oracle: generated programs (seeds 0..%d) ---" % (args.count - 1))
    g_fail = 0
    for i in range(args.count):
        f = eval_seed(args.naab, args.seed + i)
        if f.severity <= 2:
            g_fail += 1
            print("  FAIL seed=%d %s: %s" % (f.seed, f.classification, f.detail))
    print("  generated: %d run, %d failed" % (args.count, g_fail))
    print("--- oracle: metamorphic properties ---")
    p_total, p_fail = run_properties(args.naab, args.seed, args.prop_count)
    print("  properties: %d programs, %d failed" % (p_total, p_fail))
    total_fail = v_fail + g_fail + p_fail
    print("=== oracle suite: %s ===" % ("PASS" if total_fail == 0 else
                                        "FAIL (%d)" % total_fail))
    return 0 if total_fail == 0 else 1


def cmd_parencheck(args):
    """Emit each seed's AST twice — fully parenthesized and precedence-based
    — and require identical engine output. A mismatch is a parser
    precedence/associativity bug (or an emitter table drift)."""
    from .runner import run_engine
    from .triage import classify_error
    bad = 0
    for i in range(args.count):
        seed = args.seed + i
        prog = G.generate(seed)
        plain = Emitter(paren_all=False).program(prog)
        paren = Emitter(paren_all=True).program(prog)
        a = run_engine(args.naab, plain, tree_walk=False)
        b = run_engine(args.naab, paren, tree_walk=False)
        same = (a.rc == b.rc and a.out_lines == b.out_lines and
                classify_error(a.error_text) == classify_error(b.error_text))
        if not same:
            bad += 1
            print("PAREN MISMATCH seed=%d: plain rc=%d %r / paren rc=%d %r"
                  % (seed, a.rc, a.out_lines[:3], b.rc, b.out_lines[:3]))
    print("=== paren-check: %d seeds, %d mismatches ===" % (args.count, bad))
    return 1 if bad else 0


def cmd_properties(args):
    from .properties import run_properties
    total, failures = run_properties(args.naab, args.seed, args.count)
    print("=== properties: %d programs, %d failures ===" % (total, failures))
    return 0 if failures == 0 else 1


def cmd_selftest(args):
    import unittest
    from .tests import test_oracle_selfcheck
    suite = unittest.defaultTestLoader.loadTestsFromModule(test_oracle_selfcheck)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


def main(argv=None):
    ap = argparse.ArgumentParser(prog="naabfuzz")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add_naab(p):
        p.add_argument("--naab", default="build/naab-lang",
                       help="path to naab-lang binary")

    p = sub.add_parser("gen", help="print the program for a seed")
    p.add_argument("--seed", type=int, required=True)
    p.add_argument("--paren-all", action="store_true")
    p.set_defaults(fn=cmd_gen)

    p = sub.add_parser("repro", help="run one seed with full detail")
    add_naab(p)
    p.add_argument("--seed", type=int, required=True)
    p.set_defaults(fn=cmd_repro)

    p = sub.add_parser("fuzz", help="fuzz loop with triage")
    add_naab(p)
    p.add_argument("--seeds", help="inclusive range, e.g. 1..300")
    p.add_argument("--count", type=int)
    p.add_argument("--seed-base", type=int, default=0)
    p.add_argument("--time-budget", type=float,
                   help="seconds; alternative to --count/--seeds")
    p.add_argument("--findings", help="findings.jsonl output path")
    p.add_argument("--known-findings",
                   default="tests/fuzzing/known_findings.txt")
    p.set_defaults(fn=cmd_fuzz)

    p = sub.add_parser("minimize", help="minimize a failing seed")
    add_naab(p)
    p.add_argument("--seed", type=int, required=True)
    p.set_defaults(fn=cmd_minimize)

    p = sub.add_parser("oracle", help="fixed vectors + generated + properties")
    add_naab(p)
    p.add_argument("--vectors", default="fixed")
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--count", type=int, default=150)
    p.add_argument("--prop-count", type=int, default=5)
    p.set_defaults(fn=cmd_oracle)

    p = sub.add_parser("paren-check",
                       help="paren-all vs precedence emission parity")
    add_naab(p)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--count", type=int, default=50)
    p.set_defaults(fn=cmd_parencheck)

    p = sub.add_parser("properties", help="metamorphic property suite only")
    add_naab(p)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--count", type=int, default=5)
    p.set_defaults(fn=cmd_properties)

    p = sub.add_parser("selftest", help="oracle self-tests (no binary needed)")
    p.set_defaults(fn=cmd_selftest)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
