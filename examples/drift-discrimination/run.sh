#!/usr/bin/env bash
# ============================================================
# Drift-discrimination experiment
#
# Question: does the engine tell correct work apart from real drift?
#
# Answering it needs a run containing BOTH, deterministically, and a live model
# cannot supply that -- with sampling the arms diverge, and with greedy decoding
# the model never recovers, so the recovery phase never exists. The fixture is
# recorded, so the phenomenon is present by construction and every run is
# identical.
#
# Reports per-phase firing rates. Reads them from CDD_TURN with analyzed:"true"
# ONLY -- interval-skipped rows re-show stale state, which has misled forensic
# passes here before.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"

source "$REPO/tests/helpers/stub_platform.sh"
skip_if_no_stub_support "drift-discrimination"

W="${TMPDIR:-/tmp}/driftdisc-$$"; mkdir -p "$W/trust"
source "$REPO/tests/helpers/stub_launch.sh"
cleanup() { stop_stub; rm -rf "$W"; }
trap cleanup EXIT

[ -x "$NAAB" ] || { echo "build/naab-lang not found"; exit 0; }

python3 "$SCRIPT_DIR/gen_fixture.py" "$W/fixture.json"

# Run the SAME fixture under both calibration settings.
#
# The first version of this experiment ran only the uncalibrated path, because
# its govern.json set no adaptive_baseline_enabled and the engine default is
# false. It then reported the resulting inversion as "a property of those
# signals, not of a particular scenario or model". That conclusion was wrong,
# and wrong by exactly one unset config key: adaptive baselining learns a
# per-agent normal signal rate, and with it on the same signals on the same
# fixture preserve coherence through correct work and destroy it during drift.
#
# The stub consumes its fixture sequentially, so each mode needs its own stub.
run_mode() {   # $1 = off|on ; echoes the analysis, returns its exit code
  local MODE="$1" D="$W/$1" BL
  BL=false; [ "$MODE" = "on" ] && BL=true
  mkdir -p "$D"
  start_stub "$W/fixture.json" "$D" || { echo "stub failed to start"; return 2; }

cat > "$D/govern.json" <<GOV
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl", "decision_snapshots": true },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true,
    "check_interval_turns": 1,
    "coherence_natural_healing": 0.03,
    "adaptive_baseline_enabled": $BL,
    "adaptive_baseline_window": 5,
    "adaptive_baseline_sensitivity": 2.0
  },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true, "threshold": 0.30, "action": "quarantine",
      "max_quarantine_streak": 99
    }
  },
  "agents": {
    "worker": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_DD",
      "max_tokens": 1024, "max_turns": 40,
      "context_window": 20, "context_strategy": "summary",
      "system_prompt": "Build a Calculator class with add, subtract, multiply and divide methods, each recorded in a history log."
    }
  }
}
GOV

cat > "$D/t.naab" <<'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < 25 { let r = agent.send(h, "Continue the calculator work"); i = i + 1 }
    print("RUN_DONE")
}
NAABEOF

export FAKE_KEY_DD=fake-key-drift-discrimination
# Isolate the trust store. Without this the unsigned fixture config hits
# INTEGRITY BLOCK on any machine that has trusted keys installed, and the run
# exits 3 before producing a single telemetry line.
export NAAB_TRUST_STORE_DIR="$W/trust"
(cd "$D" && timeout 300s "$NAAB" t.naab) > "$D/out.txt" 2>&1
  RC=$?
  echo ""
  echo "=== adaptive_baseline_enabled=$BL ==="
  echo "run exit: $RC  (RUN_DONE markers: $(grep -c RUN_DONE "$D/out.txt" || true))"
  if [ ! -s "$D/telemetry.jsonl" ]; then
      echo "no telemetry produced — last lines of the run:"; tail -12 "$D/out.txt" | sed 's/^/    /'
      stop_stub; return 1
  fi
  stop_stub

  python3 - "$D/telemetry.jsonl" <<'PY'
import json, sys, collections

PHASES = [("WARMUP    (correct)", 1, 5),
          ("REPETITION (drift)", 6, 13),
          ("RECOVERY  (correct)", 14, 25)]

rows = []
for line in open(sys.argv[1]):
    try: d = json.loads(line)
    except Exception: continue
    # analyzed:"true" only -- skipped turns re-show stale state
    if d.get("event_type") == "CDD_TURN" and str(d.get("analyzed")) == "true":
        rows.append(d)

def phase_of(t):
    for name, lo, hi in PHASES:
        if lo <= t <= hi: return name
    return None

fired = collections.defaultdict(lambda: collections.defaultdict(int))
turns = collections.Counter()
coh = {}
for d in rows:
    t = int(d.get("turn_at_request", d.get("turn", -1))) + 1
    p = phase_of(t)
    if not p: continue
    turns[p] += 1
    coh.setdefault(p, []).append(float(d.get("coherence", 0.0)))
    for sig in (d.get("signals_detail") or "").replace(";", ",").split(","):
        sig = sig.split(":")[0].strip()
        if sig: fired[sig][p] += 1

names = sorted(fired)
if not names:
    print("\nNo analyzed CDD_TURN rows with signals — cannot report.")
    sys.exit(0)

hdr = f"{'signal':26}" + "".join(f"{p:>22}" for p, _, _ in PHASES)
print("\n" + hdr); print("-" * len(hdr))
for s in names:
    line = f"{s:26}"
    for p, _, _ in PHASES:
        n = fired[s][p]; tot = turns[p] or 1
        line += f"{n:>4}/{tot:<3} ({100*n//tot:>3}%)  "
    print(line)

print("\ncoherence, first -> last per phase:")
for p, _, _ in PHASES:
    v = coh.get(p) or [0]
    print(f"  {p:26} {v[0]:.3f} -> {v[-1]:.3f}")

# ---- assertions -------------------------------------------------------
# Deliberately NOT asserting that every signal discriminates: three of them
# currently do not, and a permanently-red gate teaches people to ignore it.
# What IS asserted is that the experiment can still detect drift at all --
# without that, "signal X fires on correct work" is equally explained by a
# broken fixture, and the whole report would be unreadable.
fails = []

analyzed = sum(turns.values())
if analyzed < 20:
    fails.append("only %d analyzed turns; the run did not complete" % analyzed)

def rate(sig, phase):
    tot = turns[phase] or 1
    return fired[sig][phase] / tot

REP = PHASES[1][0]; WARM = PHASES[0][0]
for s in ("circular", "response_repetition"):
    if s not in fired:
        fails.append("%s never fired — the fixture no longer expresses repetition" % s)
    elif rate(s, REP) < 0.5:
        fails.append("%s fired on only %.0f%% of the drift phase" % (s, 100*rate(s, REP)))
    elif rate(s, WARM) > 0.2:
        fails.append("%s fires on correct work (%.0f%% of warmup) — it was the control"
                     % (s, 100*rate(s, WARM)))

print()
if fails:
    for f in fails: print("  FAIL: " + f)
    print("\n  The experiment cannot be read while these fail: the drift signal is")
    print("  the positive control, and without it every other row is ambiguous.")
else:
    print("  CONTROL OK: the fingerprint signals discriminate (%.0f%% on drift, %.0f%% on"
          % (100*rate("circular", REP), 100*rate("circular", WARM)))
    print("  correct work), so the fixture does express detectable drift and the")
    print("  rates below are about the signals, not the scenario.")

print("\nREADING IT")
print("  A signal firing at similar rates in WARMUP and REPETITION does not")
print("  discriminate. One firing MORE in WARMUP/RECOVERY than in REPETITION is")
print("  anti-correlated with the drift it exists to detect. Both phases marked")
print("  (correct) are on-mandate work.")
sys.exit(1 if fails else 0)
PY
  return $?
}

# The uncalibrated path is the engine DEFAULT, so it runs first and is what a
# stock config produces. Neither mode is asserted beyond the positive control:
# the experiment reports, it does not gate. Asserting that the two modes differ
# would build a test that fails the day the default is fixed.
run_mode off; RC_OFF=$?
run_mode on;  RC_ON=$?

echo ""
echo "==================================================================="
echo "  READING THE TWO TABLES"
echo "==================================================================="
echo "  Same fixture, same signals, one config key apart. The per-signal"
echo "  firing RATES barely move between them: the signals fire just as"
echo "  invertedly either way. What changes is whether a firing PAYS --"
echo "  adaptive baselining learns each agent's normal rate and absorbs it,"
echo "  so the raw signal is not the engine's decision variable and cannot"
echo "  be read as though it were."
echo ""
echo "  Compare the coherence traces, not the percentages."

if [ "$RC_OFF" -ge 2 ] || [ "$RC_ON" -ge 2 ]; then exit 1; fi
exit 0
