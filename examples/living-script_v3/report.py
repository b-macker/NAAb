#!/usr/bin/env python3
"""living-script_v3 run report. python3 stdlib only. Reads telemetry only.

  python3 report.py <telemetry.jsonl> [stdout.txt]

Prints FACTS, not diagnoses: every number is re-derivable from the input file,
and nothing here decides whether the run was good. That division exists because
the two keyed runs this tool was written for were each mis-attributed on first
reading -- to the wrong timeout knob, then to the wrong token limit -- by
reasoning about the config instead of reading the trace.

NEVER share transcript_*.jsonl. It carries raw prompts and responses. Telemetry
is audited and safe: response text appears only as content_hash.
"""
import json, os, sys, collections

tel = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else None

rows = []
for line in open(tel, errors="replace"):
    line = line.strip()
    if not line:
        continue
    try:
        rows.append(json.loads(line))
    except Exception:
        pass


def ev(t):
    return [r for r in rows if r.get("event_type") == t]


def num(v, d=0.0):
    try:
        return float(v)
    except Exception:
        return d


WORKER = "drift_worker"
LEVEL_NAME = {0: "normal", 1: "elevated", 2: "high", 3: "critical"}
NAME_LEVEL = {v: k for k, v in LEVEL_NAME.items()}

# HOLD[L] is the composite a run must keep to STAY at level L; falling below it
# is what makes the computed target drop. Keyed by the level being LEFT, not the
# level being entered -- misaligning those two by one silently turns "explains
# the run" into "contradicts it": a keyless run whose calm turns exactly produce
# its single step-down reported 23 calm turns and no account of why 21 of them
# did nothing. Read from govern.json rather than hardcoded, so retuning the
# scenario cannot leave this table describing a config that no longer exists.
_CFG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "govern.json")
HOLD = {1: 0.35, 2: 0.55, 3: 0.80}
DEESC = 3
try:
    _cb = json.load(open(_CFG)).get("circuit_breaker", {})
    HOLD = {1: float(_cb.get("elevated_threshold", 0.35)),
            2: float(_cb.get("high_threshold", 0.55)),
            3: float(_cb.get("critical_threshold", 0.80))}
    DEESC = int(_cb.get("deescalate_sustained", 3))
    _CFG_OK = "read from %s" % _CFG
except Exception as e:
    _CFG_OK = "govern.json unreadable (%s) -- using built-in defaults" % e

print("=" * 72)
print("SECTION 1 — run shape")
print("=" * 72)
print("telemetry rows: %d" % len(rows))
kinds = collections.Counter(r.get("event_type") for r in rows)
for k, v in sorted(kinds.items(), key=lambda kv: -kv[1]):
    print("  %-32s %d" % (k, v))
if out:
    try:
        txt = open(out, errors="replace").read()
        print("RUN|complete|true present: %s" % ("RUN|complete|true" in txt))
        for ln in txt.splitlines():
            if ln.startswith("PHASE|") or ln.startswith("WORKER|") or ln.startswith("AGENTS|"):
                print("  %s" % ln)
    except OSError as e:
        print("stdout unreadable: %s" % e)

print()
print("=" * 72)
print("SECTION 2 — every governance level change")
print("=" * 72)
lc = ev("GOVERNANCE_LEVEL_CHANGE")
if not lc:
    print("NONE — the ladder never moved")
for r in lc:
    print("  turn %-4s %-9s -> %-9s  handle=%s config=%s" % (
        r.get("turn"), r.get("from_level"), r.get("to_level"),
        r.get("handle_id"), r.get("config_name")))
downs = [r for r in lc if NAME_LEVEL.get(str(r.get("to_level")), 9) <
         NAME_LEVEL.get(str(r.get("from_level")), -1)]
print("STEP-DOWNS: %d" % len(downs))

print()
print("=" * 72)
print("SECTION 3 — %s, every ANALYZED turn (stale rows excluded)" % WORKER)
print("=" * 72)
print("hold thresholds: elevated %.2f  high %.2f  critical %.2f  "
      "deescalate_sustained %d" % (HOLD[1], HOLD[2], HOLD[3], DEESC))
print("config: %s" % _CFG_OK)
print()
# input_tokens is printed because context_growth (S12) is a RATIO against an
# early baseline, and without the numerator no one can say whether the signal
# fired because context genuinely grew, because the baseline was drawn from an
# unusually small opening, or because windowing is not bounding what it should.
# Keyed run 4 could not be diagnosed for exactly this reason: S12 fired on turns
# 11-23 and then stopped, and the tool built to explain S12 did not print the
# one number that would have explained it.
print("%-5s %-9s %-9s %-8s %-7s %-4s %s" %
      ("turn", "pressure", "coherence", "in_tok", "level", "sig",
       "penalties_detail"))
cdd = [r for r in ev("CDD_TURN")
       if r.get("config_name") == WORKER and str(r.get("analyzed")) == "true"]
cdd.sort(key=lambda r: int(num(r.get("turn"))))
# input tokens live on AGENT_RESPONSE, which carries NO turn field -- so this
# joins POSITIONALLY: the Nth worker AGENT_RESPONSE is the Nth worker CDD_TURN,
# both being appended in event order. Joining on r.get("turn") looks right and
# silently yields None for every row, which is how the first version of this
# printed "?" for the whole column.
in_tok = {}
_resp = [r for r in ev("AGENT_RESPONSE") if r.get("config_name") == WORKER]
_all_cdd = [r for r in ev("CDD_TURN") if r.get("config_name") == WORKER]
for _c, _r in zip(_all_cdd, _resp):
    in_tok[str(_c.get("turn"))] = _r.get("input_tokens")
for r in cdd:
    print("%-5s %-9s %-9s %-8s %-7s %-4s %s" % (
        r.get("turn"), r.get("pressure"), r.get("coherence"),
        in_tok.get(str(r.get("turn")), "?"),
        r.get("governance_level"), r.get("signals_fired"),
        (r.get("penalties_detail") or "")[:70]))
base = [num(in_tok.get(str(r.get("turn"))), 0.0) for r in cdd[:5]]
base = [b for b in base if b > 0]
if base:
    mean = sum(base) / len(base)
    peak = max([num(v, 0.0) for v in in_tok.values()] or [0.0])
    print("input-token baseline (first 5 analyzed turns): mean %.0f" % mean)
    print("peak input tokens: %.0f  -> peak/baseline = %.2fx  (S12 fires above "
          "context_growth_factor)" % (peak, peak / mean if mean else 0.0))
skipped = len([r for r in ev("CDD_TURN")
               if r.get("config_name") == WORKER and str(r.get("analyzed")) != "true"])
print("interval-skipped (stale) rows not shown: %d  <-- must be 0" % skipped)

print()
print("=" * 72)
print("SECTION 4 — de-escalation precondition, both readings")
print("=" * 72)
print("The engine steps DOWN when the COMPUTED TARGET is below the current")
print("level for deescalate_sustained consecutive turns FROM THE HANDLE THAT")
print("RAISED IT. Two readable proxies, printed separately because they are")
print("NOT the same predicate:")
print("  A: pressure < HOLD[current level]       -- sufficient for target<level")
print("  B: penalties_detail empty               -- stricter; a turn can pay a")
print("     penalty and still compute a lower target")
if lc:
    first = min(int(num(r.get("turn"))) for r in lc)
    post = [r for r in cdd if int(num(r.get("turn"))) > first]
    prior = 0
    a = b = 0
    a_run = b_run = 0
    a_max = b_max = 0
    # The level a turn is judged against is the one IN FORCE when it was
    # evaluated -- i.e. the previous row's -- not the one its own row reports.
    # A row's governance_level is written AFTER the update, so on the very turn
    # a step-down lands, the row already shows the lower level and the turn
    # scores itself against a threshold it was never held to. That single
    # misclassification is the difference between "longest calm run 2" and the
    # 3 that deescalate_sustained actually required, on a run that DID step down.
    prev_lvls = []
    for r in cdd:
        if int(num(r.get("turn"))) > first:
            prev_lvls.append(prior)
        prior = NAME_LEVEL.get(str(r.get("governance_level")), 0)
    for r, lvl in zip(post, prev_lvls):
        # At NORMAL there is no level to step down from, so no turn counts.
        is_a = lvl > 0 and num(r.get("pressure")) < HOLD.get(lvl, 0.35)
        is_b = not str(r.get("penalties_detail") or "").strip()
        a += is_a
        b += is_b
        a_run = a_run + 1 if is_a else 0
        b_run = b_run + 1 if is_b else 0
        a_max = max(a_max, a_run)
        b_max = max(b_max, b_run)
    print()
    print("first escalation at turn %d; %d analyzed worker turns after it" % (first, len(post)))
    print("  A (pressure below the level it must hold): %d turns, longest run %d" % (a, a_max))
    print("  B (no penalty recorded):                   %d turns, longest run %d" % (b, b_max))
    print()
    print("Reading it: A's LONGEST RUN is what deescalate_sustained (%d) is" % DEESC)
    print("compared against.")
    if a_max >= DEESC and not downs:
        print("  >>> longest calm run %d >= %d and ZERO step-downs: the"
              % (a_max, DEESC))
        print("  >>> hysteresis had what it needs and did not fire. REGRESSION.")
    elif downs:
        print("  >>> %d step-down(s) occurred. The mechanism fired." % len(downs))
    else:
        print("  >>> longest calm run %d < %d: the run never gave the mechanism"
              % (a_max, DEESC))
        print("  >>> the calm it needs. Scenario shortfall, NOT a governance")
        print("  >>> failure -- do not report this as a defect.")
    print("  min pressure after first escalation: %s" %
          (min([num(r.get("pressure")) for r in post]) if post else "n/a"))
else:
    print("no escalation, so nothing to de-escalate from")

print()
print("=" * 72)
print("SECTION 5 — coherence conservation invariant")
print("=" * 72)
snaps = []
for r in ev("SEMANTIC_TURN"):
    s = r.get("cdd_snapshot")
    if isinstance(s, str):
        try:
            s = json.loads(s)
        except Exception:
            continue
    if isinstance(s, dict):
        snaps.append((r.get("config_name"), s))
worst = 0.0
n = 0
for cfg, s in snaps:
    if "coherence_damage_total" not in s:
        continue
    n += 1
    resid = num(s.get("coherence_score")) - (1.0 - num(s.get("coherence_damage_total"))
                                             + num(s.get("coherence_healed_total")))
    worst = max(worst, abs(resid))
print("snapshots with ledger fields: %d" % n)
print("worst |coherence - (1 - damage + healed)|: %.12f   <-- must be 0.000000000000" % worst)
peak = max([int(num(s.get("cb_sustained_elevated"))) for _, s in snaps] or [0])
print("peak cb_sustained_elevated: %d" % peak)
print("peak consecutive_high_pressure_turns: %d" %
      max([int(num(s.get("consecutive_high_pressure_turns"))) for _, s in snaps] or [0]))

print()
print("=" * 72)
print("SECTION 6 — signal counts and anything that went wrong")
print("=" * 72)
sig = collections.Counter()
for r in cdd:
    for part in str(r.get("signals_detail") or "").replace(";", ",").split(","):
        part = part.strip()
        if part:
            sig[part.split("=")[0].split("(")[0].strip()] += 1
for k, v in sorted(sig.items(), key=lambda kv: -kv[1]):
    print("  %-32s %d" % (k, v))
print()
for t in ("RESPONSE_SUPPRESSED", "RESPONSE_TRUNCATED", "AGENT_RETRY", "AGENT_FALLBACK",
          "AGENT_KEY_DISABLED", "AGENT_HARD_STOP", "OUTPUT_INADMISSIBLE",
          "QUARANTINE_STREAK_EXCEEDED", "AGENT_CHALLENGE_FAIL", "AGENT_CHALLENGE_SKIPPED",
          "THINKING_UNREPORTED", "PULSE_TRANSITION"):
    c = len(ev(t))
    if c:
        print("  %-32s %d" % (t, c))
print()
print("prompt_compliance (S20) firings: %d   <-- expect 0" %
      sum(1 for r in cdd if "prompt_compliance" in str(r.get("signals_detail") or "")))
tok = ev("AGENT_RESPONSE")
print("AGENT_RESPONSE events: %d" % len(tok))
per = collections.Counter(r.get("config_name") for r in tok)
for k, v in per.most_common():
    print("  %-20s %d responses" % (k, v))
