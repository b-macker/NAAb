#!/usr/bin/env bash
# ============================================================
# test_level_attribution.sh — a handle's row says WHOSE pressure set its level
#
# THE PREMISE THIS FILE CORRECTS
#
# test_escalation_effectiveness.sh carried, as a known-unfixed caveat:
#
#     "Only the acting handle records the transition, while governance_level_
#      is engine-global — siblings whose scrutiny changed have no record of it."
#
# Re-derived rather than inherited, that is no longer true. governance_level_ is
# still one engine-global atomic — verified, one write site and seventeen reads —
# but deescalate_pressure_handle_ records WHICH handle raised or held it, and
# every handle's CDD_TURN row carries it. The caveat predates that field.
#
# What remained was that the row named a bare numeric handle id, so answering
# "which agent put me here" needed a join against another handle's rows. That is
# resolved to the agent's config name here.
#
# WHAT IS DELIBERATELY NOT CHANGED
#
# The level stays ENGINE-GLOBAL for enforcement. Making it per-handle would mean
# a sibling keeps acting while another agent sits at CRITICAL — and CRITICAL
# means "all autonomous actions suspended", a system-wide safety stance, not a
# per-agent one. That change errs toward FALSE REASSURANCE, which is the
# dangerous direction for a protection, so it is not made on the strength of an
# observability complaint. The global choice is also deliberate and documented
# at the declaration of deescalate_pressure_handle_.
#
# So: attribution is a REPORTING gap and is fixed as one. Enforcement semantics
# are unspecified, and unspecified things need a decision, not a patch.
#
#   LA-01  setup: one agent drifts, its sibling stays clean and stays coherent
#   LA-02  the level rises, and the SIBLING's own row reports the raised level
#          (this is what makes the attribution necessary — the sibling is
#          governed by a level it did nothing to earn)
#   LA-03  the sibling's row names the handle that caused it
#   LA-04  ...and resolves that handle to the agent's config name
#   LA-05  NEGATIVE CONTROL — before any escalation, attribution is "none", not
#          a stale or invented name. Without this, LA-03/04 pass for an
#          implementation that hardcodes the first agent it sees.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/lvlattr-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_level_attribution.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_LA="fake-key-level-attribution"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

WDIR="$TEST_TMP/run"; mkdir -p "$WDIR"
# The stub serves responses in REQUEST order, and the script interleaves
# drifter/sibling strictly, so pairs line up: odd = drifter, even = sibling.
python3 - "$WDIR/fixture.json" <<'PYEOF'
import json, sys
clean = "Ledger reconcile: quarterly totals computed and the balance recorded for part %d."
r = []
for i in range(5):
    r.append({"content": clean % (i*2),   "output_tokens": 45, "thinking_tokens": 20})
    r.append({"content": clean % (i*2+1), "output_tokens": 45, "thinking_tokens": 20})
drift = ["photography lenses","mountain weather","pasta recipes","jazz history","bicycle gears",
         "tide tables","volcano types","origami folds","desert beetles","harbour cranes"]
for i, t in enumerate(drift):
    r.append({"content": "Consider %s." % t, "output_tokens": max(6, 18-i), "thinking_tokens": 0})
    r.append({"content": clean % (100+i),    "output_tokens": 45, "thinking_tokens": 20})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF

start_stub "$WDIR/fixture.json" "$WDIR" || { skip "LA-01" "stub failed to start"; exit 0; }
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_window": 5, "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true,
    "elevated_threshold": 0.40, "elevated_sustained": 1,
    "high_threshold": 0.95, "critical_threshold": 0.99,
    "level_effects": { "high_advisory_to_soft": false } },
  "agents": {
    "drifter": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_LA",
      "max_tokens": 200, "max_turns": 40,
      "system_prompt": "You reconcile ledger data and report quarterly totals." },
    "sibling": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_LA",
      "max_tokens": 200, "max_turns": 40,
      "system_prompt": "You reconcile ledger data and report quarterly totals." }
  }
}
EOF
(cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
cat > "$WDIR/t.naab" <<'NAABEOF'
use agent
main {
    let a = agent.create("drifter")
    let b = agent.create("sibling")
    let i = 0
    while i < 15 {
        let ra = agent.send(a, "continue the ledger reconciliation")
        let rb = agent.send(b, "continue the ledger reconciliation")
        i = i + 1
    }
    print("DONE")
}
NAABEOF
(cd "$WDIR" && timeout 250s "$NAAB" t.naab > out.txt 2> err.txt) || true
stop_stub

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Level attribution across handles                            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# rows for one config: "turn level coherence src_handle src_config"
rows_for() {
    python3 - "$WDIR/tele.jsonl" "$1" <<'PYEOF'
import json, sys
want = sys.argv[2]
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") != "CDD_TURN" or d.get("analyzed") != "true": continue
    if d.get("config_name") != want: continue
    print("%s %s %s %s %s" % (d.get("turn"), d.get("governance_level"), d.get("coherence"),
                              d.get("deescalate_pressure_handle", "?"),
                              d.get("level_source_config", "<absent>")))
PYEOF
}

DR=$(rows_for drifter)
SB=$(rows_for sibling)

DR_MIN=$(echo "$DR" | awk '{print $3}' | sort -g | head -1)
SB_MIN=$(echo "$SB" | awk '{print $3}' | sort -g | head -1)
if [ -n "$DR_MIN" ] && [ -n "$SB_MIN" ] \
   && awk "BEGIN{exit !($DR_MIN <= 0.2)}" && awk "BEGIN{exit !($SB_MIN >= 0.99)}"; then
    pass "LA-01" "drifter collapsed ($DR_MIN) while the sibling stayed coherent ($SB_MIN)"
else
    fail "LA-01" "the two arms did not diverge — attribution is not exercised" \
         "drifter min $DR_MIN, sibling min $SB_MIN"
fi

# The sibling is governed by a level it did not earn. That is the whole reason
# attribution has to be on ITS row rather than only on the drifter's.
SB_ELEV=$(echo "$SB" | awk '$2!="normal"' | wc -l)
if [ "$SB_ELEV" -ge 3 ]; then
    pass "LA-02" "the clean sibling's own rows report the raised level ($SB_ELEV turns)"
else
    fail "LA-02" "the sibling never saw a raised level" \
         "levels: $(echo "$SB" | awk '{print $2}' | sort -u | tr '\n' ' ')"
fi

SB_SRC_H=$(echo "$SB" | awk '$2!="normal"{print $4}' | sort -u | tr '\n' ' ')
if [ "$(echo $SB_SRC_H)" = "1" ]; then
    pass "LA-03" "the sibling's raised-level rows name the causing handle (id 1)"
else
    fail "LA-03" "attribution handle wrong or missing on the sibling's rows" \
         "got '$SB_SRC_H' — expected the drifter's handle id"
fi

SB_SRC_C=$(echo "$SB" | awk '$2!="normal"{print $5}' | sort -u | tr '\n' ' ')
if [ "$(echo $SB_SRC_C)" = "drifter" ]; then
    pass "LA-04" "...and resolves it to the agent name (drifter), so the row needs no join"
else
    fail "LA-04" "level_source_config wrong, absent, or unresolved" \
         "got '$SB_SRC_C' — <absent> means the field was not emitted"
fi

# NEGATIVE CONTROL. Before anything escalates, attribution must say "none".
# Without this, LA-03/04 are satisfied by hardcoding the first agent created.
PRE_C=$(echo "$SB" | awk '$2=="normal"{print $5}' | sort -u | tr '\n' ' ')
PRE_N=$(echo "$SB" | awk '$2=="normal"' | wc -l)
if [ "$PRE_N" -ge 2 ] && [ "$(echo $PRE_C)" = "none" ]; then
    pass "LA-05" "control: at NORMAL the source is 'none', not a stale or invented name"
elif [ "$PRE_N" -lt 2 ]; then
    fail "LA-05" "no pre-escalation turns to check — the control is not exercised" "$PRE_N turns"
else
    fail "LA-05" "attribution present before any escalation" \
         "got '$PRE_C' at NORMAL — the field is not tracking the actual cause"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
