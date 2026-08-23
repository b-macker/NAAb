#!/usr/bin/env bash
# ============================================================
# test_level_inertness.sh — the governance ladder's middle rungs do not do what
# the documentation says they do.
#
# THIS TEST PINS A KNOWN DIVERGENCE, NOT A CONTRACT.
#
# docs/CLAUDE-TEMPLATE.md:1305-1307 specifies all four rungs:
#
#   "System-wide governance levels (NORMAL/ELEVATED/HIGH/CRITICAL) based on
#    sustained composite pressure. Effects: ELEVATED = CDD check every turn,
#    HIGH = ADVISORY escalates to SOFT, CRITICAL = all agent admission denied."
#
# Traced against the implementation:
#
#   CRITICAL  IMPLEMENTED — checkCriticalSuspension() denies admission and the
#             tool loop breaks. Verified.
#   ELEVATED  NOT IMPLEMENTED — the CDD gate is
#             `turn_number - last_checked_turn < config_->check_interval_turns`,
#             a pure config value. behavioral_sequence.cpp contains ZERO
#             references to the governance level.
#   HIGH      NOT IMPLEMENTED — advisory escalation is triggered by
#             `occurrence >= esc.soft_after`, the repeat count of the SAME RULE,
#             with no level involvement. advisory_escalation.enabled also
#             defaults false, so even that path is off.
#
# Corroborated by an exhaustive trace of every reader of governance_level_:
# each is a label (environment, ADMISSION_EVAL, CDD_TURN, transcript), gated on
# circuit_breaker.step_up_enabled (default FALSE), or CRITICAL-only. Neither
# documented effect appears in that list.
#
# CONSEQUENCE, verified below: with step_up_enabled at its default, escalating
# NORMAL -> ELEVATED -> HIGH changes nothing measurable — identical coherence
# trace, identical quarantine count, identical request count. The middle of the
# ladder is ornamental on a stock config, and work in this repo has been built
# assuming otherwise (examples/living-script_v3 exists to "make the
# circuit-breaker ladder observable").
#
# WHEN THIS IS FIXED, LI-02 SHOULD FAIL. That is the point. Do not "repair" it
# by relaxing the assertion — implement the documented effects, then replace
# LI-02 with the positive assertions that ELEVATED forces a per-turn CDD check
# and HIGH promotes ADVISORY to SOFT.
#
# WHY THE POSITIVE CONTROL IS THE LOAD-BEARING PART
#
# LI-02 asserts an ABSENCE — two runs are the same. An absence is worthless
# without proof the comparison could have found a difference: a harness that
# compares the wrong thing, or two runs that were never really different, gives
# exactly the same "identical" answer. LI-01 therefore runs the SAME comparison
# with step_up_enabled TRUE, where the level demonstrably does something (a
# challenge fires and the run ends at turn 8 rather than 25), and requires it to
# differ. If LI-01 fails, LI-02 is unreadable and says so.
#
#   LI-00  every arm ran to completion
#   LI-01  POSITIVE CONTROL — with step_up_enabled, escalation changes behaviour
#   LI-02  THE FINDING — with the default, escalation changes nothing
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"; else _SYSTMP="${TMPDIR:-/tmp}"; fi
TEST_TMP="${_SYSTMP}/levinert-$$"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_level_inertness.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
[ -x "$NAAB" ] || { skip "LI-00" "build/naab-lang not found"; exit 0; }

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem" FAKE_KEY_LI=fake
python3 "$SCRIPT_DIR/../../examples/drift-discrimination/gen_fixture.py" "$TEST_TMP/fx.json" >/dev/null
TURNS=25

# $1=tag  $2=elevated/high threshold  $3=step_up_enabled
run_arm() {
    local tag="$1" thr="$2" su="$3"
    local D="$TEST_TMP/$tag"; mkdir -p "$D"
    local PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$PORT" "$TEST_TMP/fx.json" "$D" > "$D/stub.log" 2>&1 &
    local SP=$!
    for i in $(seq 1 60); do grep -q READY "$D/stub.log" 2>/dev/null && break; sleep 0.3; done
    cat > "$D/govern.json" <<GOV
{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "telemetry":{"enabled":true,"output_file":"t.jsonl"},
 "behavioral_sequences":{"enabled":true},
 "context_drift":{"enabled":true,"check_interval_turns":1,
   "adaptive_baseline_enabled":true,"adaptive_baseline_window":4,
   "coherence_natural_healing":0.05,"reality_checkpoint":{"enabled":false}},
 "circuit_breaker":{"enabled":true,
   "step_up_enabled":$su,"step_up_at_level":"elevated","step_up_cooldown_turns":1,
   "elevated_threshold":$thr,"elevated_sustained":1,
   "high_threshold":$thr,"high_sustained":2,
   "critical_threshold":0.99,"critical_sustained":99,
   "output_admissibility":{"enabled":true,"threshold":0.70,
     "action":"quarantine","max_quarantine_streak":0}},
 "agents":{"b":{"provider":"gemini","model":"stub-model","api_base":"http://127.0.0.1:$PORT",
   "api_key_env":"FAKE_KEY_LI","max_tokens":200,"max_turns":60,
   "system_prompt":"Build a Calculator class with add, subtract, multiply and divide methods, each recording an entry in a history log."}}}
GOV
    (cd "$D" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1)
    cat > "$D/t.naab" <<NAABEOF
use agent
main {
    let h = agent.create("b")
    let i = 0
    while i < $TURNS { i = i + 1; let r = agent.send(h, "Continue.") }
    print("RUN_DONE")
}
NAABEOF
    (cd "$D" && timeout 180s "$NAAB" t.naab >/dev/null 2>&1)
    kill $SP 2>/dev/null
}

# escalate vs never-escalate, at each step_up setting
run_arm off_esc   0.30 false
run_arm off_noesc 0.99 false
run_arm on_esc    0.30 true
run_arm on_noesc  0.99 true

CMP=$(python3 - "$TEST_TMP" <<'PY'
import json, os, sys, glob
W = sys.argv[1]
def summarize(tag):
    p = os.path.join(W, tag, "t.jsonl")
    coh, quar, lv, chal = [], 0, 0, 0
    if not os.path.exists(p):
        return None
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        t = e.get("event_type")
        if t == "CDD_TURN" and e.get("analyzed") == "true":
            coh.append(round(float(e.get("coherence", 1.0)), 4))
        elif t == "OUTPUT_INADMISSIBLE": quar += 1
        elif t == "GOVERNANCE_LEVEL_CHANGE": lv += 1
        elif t in ("AGENT_CHALLENGE_PASS", "AGENT_CHALLENGE_FAIL",
                   "AGENT_CHALLENGE_SKIPPED"): chal += 1
    reqs = len(glob.glob(os.path.join(W, tag, "req_*.json")))
    return {"coh": coh, "quar": quar, "levels": lv, "challenges": chal, "reqs": reqs}

out = {t: summarize(t) for t in ("off_esc", "off_noesc", "on_esc", "on_noesc")}
print(json.dumps(out))
PY
)

echo ""
python3 - "$CMP" <<'PY'
import json, sys
d = json.loads(sys.argv[1])
print("  %-10s %-8s %-6s %-11s %-6s %s" % ("arm","levels","quar","challenges","reqs","coherence turns"))
print("  " + "-"*72)
for t in ("off_esc","off_noesc","on_esc","on_noesc"):
    v = d.get(t)
    if not v: print("  %-10s NO TELEMETRY" % t); continue
    print("  %-10s %-8d %-6d %-11d %-6d %d" % (t, v["levels"], v["quar"], v["challenges"], v["reqs"], len(v["coh"])))
PY
echo ""

V=$(python3 - "$CMP" <<'PY'
import json, sys
d = json.loads(sys.argv[1])
def ok(t): return d.get(t) and len(d[t]["coh"]) > 0
print("RAN %d" % sum(1 for t in ("off_esc","off_noesc","on_esc","on_noesc") if ok(t)))
if all(ok(t) for t in ("off_esc","off_noesc","on_esc","on_noesc")):
    # the escalating arms must actually escalate, or neither comparison means anything
    print("ESCALATED %d" % (1 if d["off_esc"]["levels"] > 0 and d["on_esc"]["levels"] > 0 else 0))
    # positive control: with step-up on, does escalation change anything?
    on_diff = (d["on_esc"]["challenges"] != d["on_noesc"]["challenges"]
               or d["on_esc"]["reqs"] != d["on_noesc"]["reqs"])
    print("ON_DIFFERS %d" % (1 if on_diff else 0))
    print("ON_DETAIL challenges %d vs %d, reqs %d vs %d"
          % (d["on_esc"]["challenges"], d["on_noesc"]["challenges"],
             d["on_esc"]["reqs"], d["on_noesc"]["reqs"]))
    off_same = (d["off_esc"]["coh"] == d["off_noesc"]["coh"]
                and d["off_esc"]["quar"] == d["off_noesc"]["quar"])
    print("OFF_SAME %d" % (1 if off_same else 0))
    print("OFF_DETAIL quarantines %d vs %d, coherence identical %s"
          % (d["off_esc"]["quar"], d["off_noesc"]["quar"],
             d["off_esc"]["coh"] == d["off_noesc"]["coh"]))
PY
)
RAN=$(echo "$V" | awk '/^RAN/{print $2}')
ESCALATED=$(echo "$V" | awk '/^ESCALATED/{print $2}')
ON_DIFFERS=$(echo "$V" | awk '/^ON_DIFFERS/{print $2}')
OFF_SAME=$(echo "$V" | awk '/^OFF_SAME/{print $2}')
ON_DETAIL=$(echo "$V" | sed -n 's/^ON_DETAIL //p')
OFF_DETAIL=$(echo "$V" | sed -n 's/^OFF_DETAIL //p')

if [ "${RAN:-0}" -eq 4 ] && [ "${ESCALATED:-0}" -eq 1 ]; then
    pass "LI-00" "all four arms ran, and both escalating arms reached a higher level"
else
    fail "LI-00" "arms incomplete (ran=$RAN escalated=$ESCALATED)" "nothing below is readable"
    echo -e "${RED}Aborting.${NC}"; exit 1
fi

# POSITIVE CONTROL. Without this, "identical" below is equally well explained by
# a comparison that cannot see a difference at all.
if [ "${ON_DIFFERS:-0}" -eq 1 ]; then
    pass "LI-01" "positive control: with step_up_enabled, escalation changes behaviour ($ON_DETAIL)"
else
    fail "LI-01" "escalation changed nothing even with step_up_enabled" \
         "$ON_DETAIL — the comparison cannot detect a level effect, so LI-02 proves nothing"
    echo -e "${RED}Aborting: an absence assertion needs a control that fires.${NC}"
    exit 1
fi

if [ "${OFF_SAME:-0}" -eq 1 ]; then
    pass "LI-02" "KNOWN DIVERGENCE pinned: ELEVATED/HIGH have none of their documented effects ($OFF_DETAIL)"
else
    fail "LI-02" "the level now has an effect below CRITICAL — the divergence may be FIXED" \
         "$OFF_DETAIL — if ELEVATED now forces a per-turn CDD check and HIGH promotes ADVISORY to SOFT (docs/CLAUDE-TEMPLATE.md:1305), replace LI-02 with positive assertions for each rather than relaxing it"
fi

echo ""
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
