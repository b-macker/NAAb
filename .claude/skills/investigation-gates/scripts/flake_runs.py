#!/usr/bin/env python3
"""flake_runs.py -- how much does a clean loop prove about a flake fix?

docs/investigation-method.md, "A flake fix needs runs scaled to the failure
rate": "0/15 (was 1/12)" was reported as a fix, but the UNFIXED flake passes
15 straight runs about 27% of the time.

Usage:
  flake_runs.py --observed K/N [--observed K/N ...] [--runs M] [--alpha A]

  --observed K/N  K failures seen in N runs BEFORE the fix (repeatable; loops
                  are pooled, and a warning is printed when their rates differ)
  --runs M        clean runs seen AFTER the fix (omit to just size the loop)
  --alpha A       how unlikely "the unfixed flake would also pass" should be
                  before the loop counts as evidence (default 0.05)

Two rates are reported. The point estimate K/N, and the Wilson 95% LOWER
bound on the rate. With few failures observed you do not know the rate well,
and a lower true rate means the unfixed flake passes MORE often -- so the
lower bound is the conservative one for sizing a loop.

Assumes independent runs at one fixed rate. A flake whose rate depends on
load, ordering or user (the method records 1 in 3 and 1 in 12 non-root, 0 in
5 root for one operator race) breaks that assumption: no loop length rescues
it, and the claim should rest on a stronger instrument -- the captured failed
state -- named as such.

Output is ASCII only (CLAUDE.md: output channels on Windows runners).
"""
import argparse
import math
import sys

Z95 = 1.959963984540054


def out(text=""):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def parse_obs(text):
    try:
        k_s, n_s = text.split("/")
        k, n = int(k_s), int(n_s)
    except ValueError:
        raise argparse.ArgumentTypeError("expected K/N, e.g. 1/12, got %r" % text)
    if n <= 0 or k < 0 or k > n:
        raise argparse.ArgumentTypeError("need 0 <= K <= N and N > 0, got %r" % text)
    return k, n


def wilson_lower(k, n, z=Z95):
    if k == 0:
        return 0.0
    p = k / n
    denom = 1 + z * z / n
    centre = p + z * z / (2 * n)
    spread = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return max(0.0, (centre - spread) / denom)


def pass_streak_prob(rate, runs):
    return (1.0 - rate) ** runs


def runs_needed(rate, alpha):
    if rate <= 0.0:
        return None
    if rate >= 1.0:
        return 1
    return math.ceil(math.log(alpha) / math.log(1.0 - rate))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--observed", type=parse_obs, action="append", required=True)
    ap.add_argument("--runs", type=int, default=None)
    ap.add_argument("--alpha", type=float, default=0.05)
    a = ap.parse_args(argv)
    if not (0.0 < a.alpha < 1.0):
        ap.error("--alpha must be between 0 and 1")
    if a.runs is not None and a.runs < 0:
        ap.error("--runs must be >= 0")

    k = sum(o[0] for o in a.observed)
    n = sum(o[1] for o in a.observed)
    out("observed before the fix (authored by your loop, not by CI unless it was CI):")
    for ok, on in a.observed:
        out("  %d/%d  rate %.4f" % (ok, on, ok / on))
    if len(a.observed) > 1:
        rates = [ok / on for ok, on in a.observed]
        out("  pooled %d/%d  rate %.4f" % (k, n, k / n))
        if max(rates) > 0 and min(rates) < max(rates) / 2:
            out("  WARNING: loop rates differ by more than 2x -- the rate may depend on")
            out("  conditions (load, order, user). Pooling assumes it does not.")

    if k == 0:
        out("")
        out("UNMEASURABLE: no failure observed, so there is no rate to size a loop against.")
        out("A flake you never saw fail cannot be shown fixed by a clean loop. Capture a")
        out("failure first (and its state), or rest the claim on a different instrument.")
        return 2

    p_hat = k / n
    p_low = wilson_lower(k, n)
    out("")
    out("rate used:  point estimate %.4f   Wilson 95%% lower bound %.4f" % (p_hat, p_low))

    if a.runs is not None:
        ph = pass_streak_prob(p_hat, a.runs)
        pl = pass_streak_prob(p_low, a.runs)
        out("")
        out("after the fix: %d clean runs. Chance the UNFIXED flake does the same:" % a.runs)
        out("  at the point estimate    %.3f  (%.1f%%)" % (ph, 100 * ph))
        out("  at the lower bound       %.3f  (%.1f%%)  <- conservative" % (pl, 100 * pl))
        if pl > a.alpha:
            out("  VERDICT: NOT EVIDENCE at alpha=%g -- a clean loop this long is expected" % a.alpha)
            out("  even without a fix. Say so, or rest the claim on the captured failed state.")
        else:
            out("  VERDICT: the loop is long enough at alpha=%g (independence assumed)." % a.alpha)

    nh = runs_needed(p_hat, a.alpha)
    nl = runs_needed(p_low, a.alpha)
    out("")
    out("clean runs needed so an unfixed flake passes them all with chance <= %g:" % a.alpha)
    out("  at the point estimate    %s" % nh)
    out("  at the lower bound       %s  <- conservative" % (nl if nl is not None else "unbounded"))
    out("")
    out("assumes independent runs at one fixed rate; if the rate depends on conditions,")
    out("no loop length rescues the claim -- capture and inspect the failed state instead.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
