#!/usr/bin/env bash
# ============================================================
# test_level_effects.sh — the governance ladder's middle rungs now bite
#
# This file REPLACES test_level_inertness.sh, which pinned the divergence
# between docs/CLAUDE-TEMPLATE.md and the implementation and said in its own
# header: "WHEN THIS IS FIXED, LI-02 SHOULD FAIL. Do not repair it by relaxing
# the assertion — implement the documented effects, then replace LI-02 with the
# positive assertions that ELEVATED forces a per-turn CDD check and HIGH
# promotes ADVISORY to SOFT." That is what this is.
#
# THE TWO EFFECTS
#
#   ELEVATED+  context drift is analysed EVERY turn. check_interval_turns
#              defaults to 3, so at NORMAL two turns in three are never scored.
#              The configured value is left untouched and returns when the
#              level does.
#   HIGH+      an ADVISORY finding is enforced as SOFT. A warning that keeps
#              being ignored while coherence sits on the floor is a decision
#              deferred, not information.
#
# Both default TRUE. Shipping a real capability behind a default-false flag is
# the pattern that left these inert in the first place — three times over.
#
# WHY THE CONTROLS ARE THE LOAD-BEARING PART
#
# LE-01 and LE-03 assert that a run CHANGES once a level is reached. On its own
# that is weak: a scenario that reaches ELEVATED also has collapsing coherence,
# more signals firing and greater depth, any of which could plausibly explain a
# behaviour change. LE-02 and LE-04 therefore re-run the SAME scenario with the
# single effect switched off. If the difference survives that, it was not the
# effect that caused it, and the corresponding assertion is unreadable.
#
#   LE-01  ELEVATED forces per-turn CDD analysis
#   LE-02  CONTROL — with elevated_cdd_every_turn off, the interval is honoured
#          throughout the same run
#   LE-03  HIGH promotes ADVISORY to SOFT: the run is blocked, exit 3.
#          Uses a code_quality.no_secrets advisory (an AWS-shaped key in a late
#          response), NOT a drift advisory: context_drift.* is excluded from
#          promotion by design, because the level is derived from CDD coherence
#          and promoting its own report makes HIGH terminal and CRITICAL
#          unreachable. That exclusion is pinned by test_pressure_level_map.sh
#          D-01, which fails outright without it.
#   LE-04  CONTROL — with high_advisory_to_soft off, the same run completes
#   LE-05  the block message does not lie about what happened
#   LE-06  ratchet: disabling either effect mid-run is a loosening violation
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/lvleff-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

IS_WINDOWS=false
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=true ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=true

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_level_effects.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_LVLEFF="fake-key-level-effects"
sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# $1=clean-count. Clean turns are distinct and echo the instruction so nothing
# fires during calibration; a baseline that learns drift as normal absorbs it
# for the whole run and no level is ever reached.
gen_fixture() {
    python3 - "$1" "$2" "${3:--1}" <<'PYEOF'
import json, sys
n = int(sys.argv[2])
secret_from = int(sys.argv[3])
r = [{"content": "Ledger reconcile step %d: quarterly totals computed and the balance recorded." % i,
      "output_tokens": 45, "thinking_tokens": 20} for i in range(n)]
topics = ["photography lenses","mountain weather","pasta recipes","jazz history","bicycle gears",
          "tide tables","volcano types","origami folds","desert beetles","harbour cranes",
          "violin varnish","cave minerals","kite design","tram signalling","reef fishes",
          "clock escapements","dye plants","glacier moraine","seed banks","radio masts",
          "salt marshes","paper mills","bell founding","lamp oils","rope walks"]
for i, t in enumerate(topics):
    body = "Consider %s instead." % t
    # A NON-context_drift advisory, fired from inside the agent turn.
    #
    # LE-03 needs an advisory that is eligible for promotion. context_drift.*
    # is deliberately excluded (a signal's own report must not enforce the
    # level that signal produced), and the drift scenario produces nothing
    # else — so the first version of this test could not observe the effect at
    # all once that exclusion landed. Response secret scanning runs per turn in
    # agentSend(), so an AWS-shaped key in a late response gives a real
    # code_quality advisory at a controllable turn.
    if secret_from >= 0 and i >= secret_from:
        body += " Use AKIAIOSFODNN7EXAMPLE for access."
    r.append({"content": body,
              "output_tokens": max(6, 30 - i), "thinking_tokens": 0})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# $1=port $2=check_interval_turns $3=baseline window $4=level_effects json body
mk_govern() {
cat <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory",
    "check_interval_turns": $2, "adaptive_baseline_window": $3 },${5:-}
  "circuit_breaker": { "enabled": true${4} },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$1",
      "api_key_env": "FAKE_KEY_LVLEFF", "max_tokens": 200, "max_turns": 60,
      "system_prompt": "You reconcile ledger data and report quarterly totals." } }
}
EOF
}

# $1=name $2=clean $3=interval $4=window $5=level_effects $6=sends
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json" "$2" "${7:--1}"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    mk_govern "$STUB_PORT" "$3" "$4" "$5" "${8:-}" > "$WDIR/govern.json"; sign_govern "$WDIR"
    cat > "$WDIR/t.naab" <<EOF
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < $6 {
        let r = agent.send(h, "continue the ledger reconciliation")
        i = i + 1
    }
    print("DONE")
}
EOF
    (cd "$WDIR" && timeout 250s "$NAAB" t.naab > out.txt 2> err.txt); EXITC=$?
    stop_stub
    return 0
}

# "turn analyzed level" for every CDD_TURN row (including interval-skipped ones,
# which is the whole point here).
rows() {
    python3 - "$1" <<'PYEOF'
import json, sys
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") != "CDD_TURN": continue
    print("%s %s %s" % (d.get("turn"), d.get("analyzed"), d.get("governance_level")))
PYEOF
}

# Analysed-turn density before vs after the first ELEVATED row.
density() {  # $1=rows
    python3 - <<PYEOF
rows = [l.split() for l in """$1""".strip().splitlines() if l.strip()]
first_elev = None
for i, r in enumerate(rows):
    if len(r) >= 3 and r[2] in ("elevated", "high", "critical"):
        first_elev = i; break
if first_elev is None:
    print("NOELEV 0 0 0 0"); raise SystemExit
# The override is set when the level CHANGES, which happens during turn N's
# processing — after turn N's interval gate has already decided whether to
# analyse it. So the first ELEVATED turn is necessarily still on the old
# interval and the effect starts at N+1. That lag is structural (a turn cannot
# retroactively decide it should have been analysed), so it is excluded from
# the post window rather than asserted away.
pre = rows[:first_elev]; post = rows[first_elev + 1:]
pa = sum(1 for r in pre if r[1] == "true")
poa = sum(1 for r in post if r[1] == "true")
print("OK %d %d %d %d" % (pa, len(pre), poa, len(post)))
PYEOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Governance level effects: the middle rungs bite              |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
echo -e "${CYAN}--- LE-01/02: ELEVATED forces per-turn CDD analysis ---${NC}"
# interval 3, window 1: three clean sends give one analysed clean turn, the
# baseline completes, and drift from send 4 onward is scored.
D_ON=""
# BOTH arms disable high_advisory_to_soft. With it on, the effect-on arm
# reaches HIGH sooner (analysing every turn drives coherence down faster), the
# promoted advisory blocks the run, and the post-ELEVATED window collapses to a
# couple of turns — so the arms would differ by TWO factors at once and neither
# assertion would isolate anything. LE-03/04 test that effect separately.
if run_case elev_on 3 3 1 ', "level_effects": { "high_advisory_to_soft": false }' 40; then
    R_ON=$(rows "$WDIR/tele.jsonl"); D_ON=$(density "$R_ON")
fi
D_OFF=""
if run_case elev_off 3 3 1 ', "level_effects": { "elevated_cdd_every_turn": false, "high_advisory_to_soft": false }' 40; then
    R_OFF=$(rows "$WDIR/tele.jsonl"); D_OFF=$(density "$R_OFF")
fi

set -- $D_ON; ON_OK=$1; ON_PRE_A=${2:-0}; ON_PRE_N=${3:-0}; ON_POST_A=${4:-0}; ON_POST_N=${5:-0}
set -- $D_OFF; OFF_OK=$1; OFF_PRE_A=${2:-0}; OFF_PRE_N=${3:-0}; OFF_POST_A=${4:-0}; OFF_POST_N=${5:-0}

if [ "$ON_OK" != "OK" ]; then
    fail "LE-01" "Effect-on arm never reached ELEVATED — nothing to measure" "$D_ON"
elif [ "$ON_POST_N" -lt 5 ]; then
    fail "LE-01" "Too few post-ELEVATED turns to be evidence" "post window = $ON_POST_N turns"
elif [ "$ON_POST_A" -eq "$ON_POST_N" ] && [ "$ON_PRE_A" -lt "$ON_PRE_N" ]; then
    pass "LE-01" "ELEVATED switched CDD to every turn (pre $ON_PRE_A/$ON_PRE_N, post $ON_POST_A/$ON_POST_N)"
else
    fail "LE-01" "CDD did not become per-turn at ELEVATED" \
         "pre $ON_PRE_A/$ON_PRE_N, post $ON_POST_A/$ON_POST_N"
fi

# CONTROL. Same scenario, effect off: the configured interval must be honoured
# after ELEVATED too. Without this, LE-01 is satisfied by anything that happens
# to analyse more turns late in a run.
if [ "$OFF_OK" != "OK" ]; then
    fail "LE-02" "Control arm never reached ELEVATED — LE-01 is unreadable" "$D_OFF"
elif [ "$OFF_POST_A" -lt "$OFF_POST_N" ]; then
    pass "LE-02" "Control: interval honoured after ELEVATED when the effect is off ($OFF_POST_A/$OFF_POST_N)"
else
    fail "LE-02" "Interval ignored even with the effect disabled — LE-01 measures something else" \
         "post $OFF_POST_A/$OFF_POST_N"
fi

# ============================================================
echo -e "${CYAN}--- LE-03/04: HIGH promotes ADVISORY to SOFT ---${NC}"
# interval 1 so HIGH is reached inside a short run.
HI_EXIT=""; HI_ERR=""; HI_OUT=""
if run_case high_on 5 1 5 "" 25 15 ' "code_quality": { "no_secrets": "advisory" },'; then
    HI_EXIT=$EXITC; HI_ERR="$WDIR/err.txt"; HI_OUT="$WDIR/out.txt"
fi
LO_EXIT=""
if run_case high_off 5 1 5 ', "level_effects": { "high_advisory_to_soft": false }' 25 15 ' "code_quality": { "no_secrets": "advisory" },'; then
    LO_EXIT=$EXITC; LO_ERR="$WDIR/err.txt"
fi

if [ "${HI_EXIT:-0}" -eq 3 ] && grep -q "LEVEL-PROMOTED" "$HI_ERR" 2>/dev/null; then
    pass "LE-03" "HIGH promoted an advisory to SOFT and blocked the run (exit 3)"
else
    fail "LE-03" "Advisory not promoted at HIGH" \
         "exit=$HI_EXIT promoted=$(grep -c LEVEL-PROMOTED "$HI_ERR" 2>/dev/null || echo 0)"
fi

# CONTROL. Same drift, same collapse to coherence 0, effect off: the run must
# finish. Without this, LE-03 passes for any build that blocks drifting agents
# for some entirely different reason.
if [ "${LO_EXIT:-1}" -eq 0 ] && ! grep -q "LEVEL-PROMOTED" "$LO_ERR" 2>/dev/null; then
    pass "LE-04" "Control: same run completes with the effect disabled (exit 0)"
else
    fail "LE-04" "Run blocked even with promotion disabled — LE-03 measures something else" \
         "exit=$LO_EXIT promoted=$(grep -c LEVEL-PROMOTED "$LO_ERR" 2>/dev/null || echo 0)"
fi

# The caller formats the message with the level it REQUESTED, so a promoted
# advisory still carries "execution will continue" text. The engine must say
# plainly that it did not.
if grep -q "advisory findings are enforced" "$HI_OUT" 2>/dev/null || \
   grep -q "advisory findings are enforced" "$HI_ERR" 2>/dev/null; then
    pass "LE-05" "Block message states the advisory was enforced as a block"
else
    fail "LE-05" "Message still claims execution continues while blocking" \
         "$(tail -3 "$HI_OUT" 2>/dev/null)"
fi

# ============================================================
echo -e "${CYAN}--- LE-06: ratchet ---${NC}"
if $IS_WINDOWS; then
    skip "LE-06" "mid-run file swap requires POSIX file semantics"
else
WDIR="$TEST_TMP/ratchet"; mkdir -p "$WDIR"
gen_fixture "$WDIR/fixture.json" 5
if ! start_stub "$WDIR/fixture.json" "$WDIR"; then skip "LE-06" "stub failed"; else
presign() {
    local pdir="$TEST_TMP/.presign"; mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"; sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""; rm -rf "$pdir"
}
BASE="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"tele.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1,\"adaptive_baseline_window\":5},\"circuit_breaker\":{\"enabled\":true,\"level_effects\":{\"high_advisory_to_soft\":HAS}},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"worker\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_LVLEFF\",\"max_tokens\":200,\"max_turns\":60}}}"
LOOSE="${BASE/HAS/false}"
LOOSE_SIG=$(presign "$LOOSE")
if [ -z "$LOOSE_SIG" ]; then skip "LE-06" "presign failed"; stop_stub; else
export NAAB_LOOSE_JSON="$LOOSE" NAAB_LOOSE_SIG="$LOOSE_SIG"
printf '%s' "${BASE/HAS/true}" > "$WDIR/govern.json"
sign_govern "$WDIR"
cat > "$WDIR/t.naab" <<'EOF'
use agent
main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "continue the ledger reconciliation")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "continue the ledger reconciliation")
    print("DONE")
}
EOF
(cd "$WDIR" && timeout 90s "$NAAB" t.naab > rout.txt 2> rerr.txt) || true
stop_stub
if grep -qi "ratchet\|loosen" "$WDIR/rerr.txt" 2>/dev/null && \
   grep -q "high_advisory_to_soft" "$WDIR/rerr.txt" 2>/dev/null; then
    pass "LE-06" "Disabling high_advisory_to_soft mid-run rejected as a loosening"
else
    fail "LE-06" "mid-run disable not rejected" \
         "$(grep -i 'ratchet\|loosen\|reload' "$WDIR/rerr.txt" 2>/dev/null | head -2)"
fi
fi
fi
fi

# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
