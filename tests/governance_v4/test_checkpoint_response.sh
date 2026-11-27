#!/usr/bin/env bash
# ============================================================
# test_checkpoint_response.sh -- A229 (Bundle 19 PR 4, the owner's decision of
# 2026-11-27): a send whose reality checkpoint fired carries `reality_checkpoint`
# and its notice, and agent.pipeline() passes that pressure to the next stage.
#
# Since #49 (30802675) the drift check records the checkpoint at the
# pre-increment event turn while the reply asked getCheckpointData() with the
# tracker's turn, one ahead, so `fired` was never true: no reply carried the
# field, and setInheritedPressure() -- the pipeline's Factor 6 -- had no caller
# that ran (A64's two setInheritedPressure rows).
#
#   CK-00  CONTROL: the checkpoint fires on this fixture, on two sends (its
#          RuleViolation rows in telemetry; the reply is not consulted)
#   CK-01  a send on which it fired carries reality_checkpoint and its notice
#   CK-01c CONTROL: a send on which it did not fire carries neither
#   CK-02  the notice names the sustained count it fired at (it read 0, because
#          firing resets the counter before it was read)
#   CK-03  agent.pipeline(): the next stage's state.inherited_pressure is the
#          first stage's pressure, its request header names it, and its
#          upstream_provenance carries it
#   CK-03c CONTROL: the same pipeline with the checkpoint disabled inherits 0,
#          so CK-03 is not a value that is always there
#
# KILL TABLE (each mutant run 2026-11-27, one build each; each turned its arm red)
#   K1  the reply asks with current_turn again (the A229 defect)   -> CK-01, CK-02, CK-03
#   K2  a fired checkpoint reports the reset counter               -> CK-02
#   K3  GovernanceEngine::setInheritedPressure does nothing        -> CK-03
#   K4  ContextDriftAnalyzer::setInheritedPressure does nothing    -> CK-03
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; isolate_trust_store
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:400}"; }
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
T="$(mktemp -d "${TMPDIR:-/tmp}/naab_ckpt_XXXXXX")"
cleanup() { stop_stub 2>/dev/null; [ -n "${CK_KEEP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT

# Pressure here is conversation depth only (turn / 20 x its weight), so a
# threshold of 0.01 with one sustained turn fires from the third send on.
gov() {  # DIR PORT CHECKPOINT_ENABLED
    cat > "$1/govern.json" <<EOF
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "behavioral_sequences": {"enabled": true},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl"},
 "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1,
   "reality_checkpoint": {"enabled": $3, "level": "advisory", "pressure_threshold": 0.01,
     "sustained_turns_required": 1, "min_turns_between_checkpoints": 0}},
 "agents": {
   "a1": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:$2",
          "api_key_env": "FAKE_CK_KEY", "max_tokens": 100, "max_turns": 20,
          "system_prompt": "You review database migrations for schema safety"},
   "a2": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:$2",
          "api_key_env": "FAKE_CK_KEY", "max_tokens": 100, "max_turns": 20,
          "system_prompt": "You summarise reviews"}}}
EOF
}
FX='{"responses": [{"content": "warm one about migrations", "output_tokens": 30}, {"content": "warm two about schema", "output_tokens": 30}, {"content": "warm three on safety", "output_tokens": 30}, {"content": "bananas grow in warm tropical places", "output_tokens": 30}, {"content": "final stage answer", "output_tokens": 20}]}'

echo "=== A229: the reality checkpoint reaches the reply and the next stage ==="

# --- sends -------------------------------------------------------------------
D="$T/sends"; mkdir -p "$D"; printf '%s' "$FX" > "$D/fx.json"
start_stub "$D/fx.json" "$D" || { echo "stub did not start"; exit 1; }
gov "$D" "$STUB_PORT" true
cat > "$D/p.naab" <<'EOF'
use agent
main {
  let h = agent.create("a1")
  for i in 0..4 {
    let r = agent.send(h, "turn " + string(i))
    let cp = r.get("reality_checkpoint")
    let n = r.get("governance_notices")
    if cp != null {
      print("T" + string(i) + " CP pressure=" + string(cp.get("pressure")) + " sustained=" + string(cp.get("sustained_turns")))
    } else {
      print("T" + string(i) + " NOCP")
    }
    if n != null {
      print("T" + string(i) + " NOTICE " + string(n))
    }
  }
}
EOF
OUT="$(cd "$D" && FAKE_CK_KEY=k timeout 60 "$NAAB" p.naab 2>&1)"; RC=$?
stop_stub

# Counted from telemetry: an advisory prints its detail once per rule, so the
# terminal shows one warning however often it fired.
fires=$(grep -c '"rule_name":"context_drift.reality_checkpoint"' "$D/tele.jsonl" 2>/dev/null)
if [ "$RC" = 0 ] && [ "${fires:-0}" -ge 2 ]; then
    pass CK-00 "the checkpoint fires on this fixture (on $fires sends, from telemetry)"
else fail CK-00 "CONTROL: the checkpoint did not fire -- the arms cannot judge the reply" "rc=$RC fires=$fires $(tail -3 <<<"$OUT")"; fi

if grep -q '^T2 CP pressure=0\.01' <<<"$OUT" && grep -q '^T3 CP pressure=0\.01' <<<"$OUT" \
   && grep -q '^T2 NOTICE .*Reality Checkpoint: pressure 0.01' <<<"$OUT"; then
    pass CK-01 "a send on which the checkpoint fired carries reality_checkpoint and its notice"
else fail CK-01 "no reality_checkpoint on a send where it fired" "$(grep -E '^T[0-9]' <<<"$OUT" | tr '\n' ' ')"; fi

if grep -q '^T0 NOCP' <<<"$OUT" && grep -q '^T1 NOCP' <<<"$OUT" && ! grep -q '^T[01] NOTICE' <<<"$OUT"; then
    pass CK-01c "sends on which it did not fire carry no field and no notice"
else fail CK-01c "CONTROL: a field or notice on a send where the checkpoint did not fire" "$(grep -E '^T[01]' <<<"$OUT" | tr '\n' ' ')"; fi

if grep -q '^T2 CP .* sustained=1$' <<<"$OUT" && grep -q '^T2 NOTICE .*sustained 1 turns' <<<"$OUT"; then
    pass CK-02 "the field and the notice give the sustained count it fired at (1), not the reset counter"
else fail CK-02 "the sustained count is not the one it fired at" "$(grep -E '^T2' <<<"$OUT" | tr '\n' ' ')"; fi

# --- pipeline ----------------------------------------------------------------
PIPE='use agent
main {
  let h1 = agent.create("a1")
  let h2 = agent.create("a2")
  agent.send(h1, "one")
  agent.send(h1, "two")
  agent.send(h1, "three")
  let r = agent.pipeline([h1, h2], "review migration 42")
  let st = agent.environment(h2).get("state")
  print("IP=" + string(st.get("inherited_pressure")))
  let prov = st.get("upstream_provenance")
  if prov != null {
    print("PROV=" + string(prov.get("pressure")))
  }
}'
pipe() {  # ID ENABLED -> OUT, dir $T/ID
    local d="$T/$1"; mkdir -p "$d"; printf '%s' "$FX" > "$d/fx.json"
    start_stub "$d/fx.json" "$d" || { OUT="nostub"; return; }
    gov "$d" "$STUB_PORT" "$2"
    printf '%s\n' "$PIPE" > "$d/p.naab"
    OUT="$(cd "$d" && FAKE_CK_KEY=k timeout 60 "$NAAB" p.naab 2>&1)"
    stop_stub
}
pipe pon true; o_on="$OUT"
pipe poff false; o_off="$OUT"
hdr="$(grep -o 'Governance pressure: [0-9.]*' "$T/pon/req_5.json" 2>/dev/null | head -1)"
if grep -q '^IP=0\.015' <<<"$o_on" && grep -q '^PROV=0\.015' <<<"$o_on" && [ "$hdr" = "Governance pressure: 0.015000" ]; then
    pass CK-03 "the next stage inherits the first stage's pressure (0.015): state, request header and upstream provenance"
else fail CK-03 "the pressure did not reach the next stage" "$(grep -E '^(IP|PROV)=' <<<"$o_on" | tr '\n' ' ') header='$hdr'"; fi

if grep -q '^IP=0$' <<<"$o_off" && grep -q '^PROV=null$' <<<"$o_off"; then
    pass CK-03c "with the checkpoint disabled the next stage inherits nothing"
else fail CK-03c "CONTROL: pressure inherited with the checkpoint disabled" "$(grep -E '^(IP|PROV)=' <<<"$o_off" | tr '\n' ' ')"; fi

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
