#!/usr/bin/env bash
# ============================================================
# Which quantity should "did escalating help?" be measured in?
#
# Runs every scenario, then evaluates candidate definitions of
# escalation_effectiveness over the resulting traces and reports which ones
# classify the scenarios correctly.
#
# GROUND TRUTH IS RE-DERIVED, NOT TRUSTED. Each scenario declares an intent,
# but the verdict used for scoring comes from the measured per-turn PENALTY
# TOTAL either side of the escalation — the one quantity that does not saturate
# at the coherence floor. A scenario whose measurement contradicts its label is
# reported as INVALID and excluded rather than scored, because a mislabelled
# scenario silently rewards whichever definition shares its error. The pilot
# had exactly that: repetition labelled "mild" and abandonment "severe", which
# is backwards for this engine.
#
# This experiment REPORTS. It does not gate, and it sets no constants by
# itself — see the caveat printed at the end about how many scenarios a
# parameter choice actually rests on.
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
source "$REPO/tests/helpers/stub_platform.sh"
skip_if_no_stub_support "effectiveness-semantics"
[ -x "$NAAB" ] || { echo "build/naab-lang not found"; exit 0; }

W="${TMPDIR:-/tmp}/effsem-$$"; mkdir -p "$W/fx"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; [ -n "${KEEP_WORK:-}" ] || rm -rf "$W"; }
trap cleanup EXIT

python3 "$SCRIPT_DIR/gen_scenarios.py" "$W/fx" || exit 1
"$NAAB" --keygen "$W/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$W/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$W/k.pem" FAKE_KEY_ES=fake
TURNS=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["turns"])' "$W/fx/meta.json")
MANDATE=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["mandate"])' "$W/fx/meta.json")

echo "Running $(ls "$W/fx"/*.json | grep -vc meta) scenarios x ${TURNS} turns  (elevated_threshold=${ELEVATED:-0.30})"
for f in "$W/fx"/*.json; do
  arm=$(basename "$f" .json); [ "$arm" = "meta" ] && continue
  D="$W/$arm"; mkdir -p "$D"
  PORT=$(( (RANDOM % 20000) + 20000 ))
  python3 "$REPO/tests/helpers/agent_stub.py" "$PORT" "$f" "$D" > "$D/stub.log" 2>&1 &
  SP=$!
  for i in $(seq 1 60); do grep -q READY "$D/stub.log" 2>/dev/null && break; sleep 0.3; done
  cat > "$D/govern.json" <<GOV
{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "telemetry":{"enabled":true,"output_file":"t.jsonl"},
 "behavioral_sequences":{"enabled":true},
 "context_drift":{"enabled":true,"check_interval_turns":1,
   "adaptive_baseline_enabled":true,"adaptive_baseline_window":4,
   "coherence_natural_healing":0.05,
   "escalation_effectiveness_window":4,
   "reality_checkpoint":{"enabled":false}},
 "circuit_breaker":{"enabled":true,"step_up_enabled":false,
   "elevated_threshold":${ELEVATED:-0.30},"elevated_sustained":1,
   "high_threshold":0.60,"high_sustained":2,
   "critical_threshold":0.95,"critical_sustained":9,
   "deescalate_sustained":3},
 "agents":{"b":{"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:$PORT",
   "api_key_env":"FAKE_KEY_ES","max_tokens":200,"max_turns":60,
   "system_prompt":"$MANDATE"}}}
GOV
  (cd "$D" && NAAB_SIGNING_KEY="$W/k.pem" "$NAAB" --sign-governance >/dev/null 2>&1)
  cat > "$D/t.naab" <<NAABEOF
use agent
main {
    let h = agent.create("b")
    let i = 0
    while i < $TURNS { i = i + 1; let r = agent.send(h, "Continue.") }
    print("RUN_DONE")
}
NAABEOF
  (cd "$D" && timeout 200s "$NAAB" t.naab >/dev/null 2>&1)
  kill $SP 2>/dev/null
  printf "."
done
echo ""

python3 "$SCRIPT_DIR/analyze.py" "$W" "$W/fx/meta.json"
[ -n "${KEEP_WORK:-}" ] && echo "WORK=$W"
