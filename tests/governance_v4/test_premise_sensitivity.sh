#!/usr/bin/env bash
# ============================================================
# test_premise_sensitivity.sh — which configuration decides whether
# governance detects anything?
#
# WHY THIS EXISTS
#
# Four conclusions in this campaign were overturned in sequence, each by the
# next level of tracing, and NOT ONE was caught by a failing test:
#
#   the drift signals are inverted
#     -> no lexical metric can separate correct work from drift
#       -> the signals should be removed
#         -> the test protecting them is confounded
#           -> the signals work; the calibration was switched off
#
# Every vacuity check passed at every step. That is not a failure of vacuity
# checking, it is its boundary: a vacuity check establishes that a harness is
# HONEST ABOUT WHAT IT MEASURES. It cannot tell you that you measured the
# wrong quantity.
#
# What every one of those errors had in common was a hidden premise in the
# setup that determined the answer:
#
#   adaptive_baseline_enabled: false  in a config nobody re-read
#   one process per arm, so every arm baselined on itself
#   a control fixture written with repetitive vocabulary
#   CDD_TURN.turn assumed 0-based when it is 1-based
#
# Each looked like a neutral setup detail. Each decided the result. The
# generalisable defence is not another assertion about the engine — it is to
# VARY THE PREMISE AND SEE WHETHER THE CONCLUSION MOVES. A conclusion that
# flips when a premise changes is not a finding about the engine; it is a
# finding about the configuration, and must be reported as one.
#
# WHAT THIS MEASURES
#
# One scalar, swept across a premise grid:
#
#   separation = mean(coherence floor over CORRECT arms)
#              - mean(coherence floor over DRIFT arms)
#
#   > +MARGIN   DISCRIMINATES  correct work outscores drift
#   < -MARGIN   INVERTED       drift outscores correct work
#   otherwise   BLIND          no usable difference either way
#
# Means, not min/max: with calibration on, drift_parrot is undetected and sits
# at 1.000, so a min/max separation reads as INVERTED on the strength of the
# single known gap and hides that the other two drift arms are caught. The
# parroting gap is asserted where it belongs, in test_signal_contract.sh C3/C4.
#
# WHAT IT ASSERTS
#
#   PS-01  The sweep is live. At least two cells must disagree, or the grid is
#          measuring nothing and every other assertion here is vacuous.
#   PS-02  No undeclared premise decides the answer. Every premise that flips
#          the verdict must appear in LOAD_BEARING below, and every premise in
#          LOAD_BEARING must still flip it. Both directions fail:
#            - undeclared premise flips it -> a hidden setting governs whether
#              governance works, and nobody wrote it down
#            - declared premise stops mattering -> the claim is stale and the
#              documentation built on it is now wrong
#   PS-03  The DEFAULT configuration's verdict is what this file says it is.
#          This is the assertion that would have caught the shipped
#          drift-discrimination experiment, whose README asserted a property
#          "of those signals, not of a particular scenario" while its own
#          config left the calibration off.
#
# PS-02 is the point of the file. It is a ratchet on hidden premises: it does
# not claim the engine is good or bad, it pins WHICH CONFIGURATION DECIDES,
# and fails the moment that set changes in either direction.
#
# This test is expected to PASS. Unlike test_signal_contract.sh it is not an
# acceptance gate for unbuilt work — it characterises what is true now, and
# breaks when that changes silently.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/premsens-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

# ---- the claims this file pins -------------------------------------------
# Premises whose value is currently known to decide the verdict. Keep this
# list and DEFAULT_VERDICT in sync with reality; PS-02 and PS-03 fail loudly
# in both directions rather than letting either drift.
LOAD_BEARING="calibration"
DEFAULT_VERDICT="BLIND"
MARGIN=0.15
OA_THRESHOLD=0.70

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_premise_sensitivity.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"
[ -x "$NAAB" ] || { skip "PS-00" "build/naab-lang not found"; exit 0; }

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_PS="fake-key-premise"
sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

start_stub() {
    local _fx="$1" _dir="$2" _try _i _tries=3
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) _tries=1 ;; esac
    [ -n "${WINDIR:-}" ] && _tries=1
    for _try in $(seq 1 $_tries); do
        STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
        : > "$_dir/stub.log"
        python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$_fx" "$_dir" > "$_dir/stub.log" 2>&1 &
        STUB_PID=$!
        if [ "$_tries" -eq 1 ]; then
            for _i in $(seq 1 50); do grep -q READY "$_dir/stub.log" 2>/dev/null && return 0; sleep 0.1; done
            return 1
        fi
        for _i in $(seq 1 60); do
            grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
            kill -0 "$STUB_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -9 "$STUB_PID" 2>/dev/null; STUB_PID=""
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# Phase-change fixtures, shared with test_signal_contract.sh. One run per arm:
# CALIBRATE turns of correct work, then TEST turns that continue it or drift.
FX="$TEST_TMP/fx"; mkdir -p "$FX"
python3 "$SCRIPT_DIR/../helpers/signal_contract_fixtures.py" "$FX" >/dev/null || {
    fail "PS-00" "fixture generation failed"; exit 1; }
CALIBRATE=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["calibrate"])' "$FX/meta.json")
TURNS=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["total"])' "$FX/meta.json")
MANDATE=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["mandate"])' "$FX/meta.json")
CORRECT_ARMS=(ctl_narrow ctl_varied ctl_verbose)
DRIFT_ARMS=(drift_repeat drift_abandon drift_parrot)

# ---- the premise grid ----------------------------------------------------
# Each premise is a single govern.json setting that a reasonable person would
# consider a neutral detail of the setup. That is exactly why they are here.
#   calibration  adaptive_baseline_enabled           false | true
#   healing      coherence_natural_healing            0.0  | 0.03
#   window       adaptive_baseline_window              5   | 12
PREMISES=(calibration healing window)

write_config() {  # $1=dir $2=port $3=calibration $4=healing $5=window
    cat > "$1/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": $3,
    "adaptive_baseline_window": $5,
    "adaptive_baseline_sensitivity": 2.0,
    "coherence_natural_healing": $4,
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": {
    "enabled": true, "step_up_enabled": false,
    "output_admissibility": {
      "enabled": true, "threshold": $OA_THRESHOLD,
      "action": "quarantine", "max_quarantine_streak": 0
    }
  },
  "agents": {
    "builder": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$2", "api_key_env": "FAKE_KEY_PS",
      "max_tokens": 200, "max_turns": 60,
      "system_prompt": "$MANDATE"
    }
  }
}
GOVEOF
    sign_govern "$1"
}

# echoes the TEST-phase coherence floor for one arm under one cell
measure() {  # $1=tag $2=arm $3=calibration $4=healing $5=window
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    cp "$FX/$2.json" "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "ERR"; return; }
    write_config "$d" "$STUB_PORT" "$3" "$4" "$5"
    cat > "$d/test.naab" <<NAABEOF
use agent
main {
    let h = agent.create("builder")
    let i = 0
    while i < $TURNS { i = i + 1; let r = agent.send(h, "Continue the calculator work.") }
    print("RUN_DONE")
}
NAABEOF
    local out; out=$( (cd "$d" && timeout 120s "$NAAB" test.naab 2>/dev/null) )
    stop_stub
    case "$out" in *RUN_DONE*) ;; *) echo "ERR"; return ;; esac
    python3 - "$d/tele.jsonl" "$CALIBRATE" <<'PY'
import json, os, sys
p, calibrate = sys.argv[1], int(sys.argv[2])
floor, seen = 1.0, 0
if os.path.exists(p):
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        if e.get("event_type") != "CDD_TURN" or e.get("analyzed") != "true": continue
        try: t = int(e.get("turn", -1))
        except (TypeError, ValueError): continue
        # CDD_TURN.turn is 1-BASED — verified empirically, a 20-send run emits
        # turns 1..20. Not the same counter as the agent EVENT turn, which
        # starts at 0; reading it as 0-based silently shifts the phase boundary.
        if t <= calibrate: continue
        seen += 1
        try: floor = min(floor, float(e.get("coherence", 1.0)))
        except (TypeError, ValueError): pass
print("ERR" if seen == 0 else "%.3f" % floor)
PY
}

# ---- sweep ---------------------------------------------------------------
echo ""
echo -e "${CYAN}Sweeping ${#PREMISES[@]} premises x 2 values x 6 arms${NC}"
echo -e "${CYAN}(${TURNS} turns per run, first ${CALIBRATE} calibrate and unscored)${NC}"
echo ""

RESULTS="$TEST_TMP/cells.tsv"; : > "$RESULTS"
for CAL in false true; do
 for HEAL in 0.0 0.03; do
  for WIN in 5 12; do
    CELL="cal=$CAL,heal=$HEAL,win=$WIN"
    CF=""; DF=""; ERR=0
    for arm in "${CORRECT_ARMS[@]}"; do
        v=$(measure "c-$CAL-$HEAL-$WIN-$arm" "$arm" "$CAL" "$HEAL" "$WIN")
        [ "$v" = "ERR" ] && ERR=1 || CF="$CF $v"
    done
    for arm in "${DRIFT_ARMS[@]}"; do
        v=$(measure "d-$CAL-$HEAL-$WIN-$arm" "$arm" "$CAL" "$HEAL" "$WIN")
        [ "$v" = "ERR" ] && ERR=1 || DF="$DF $v"
    done
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$CAL" "$HEAL" "$WIN" "$ERR" "${CF# }" "${DF# }" >> "$RESULTS"
    echo "  $CELL  correct[${CF# }]  drift[${DF# }]"
  done
 done
done

# ---- verdicts and assertions --------------------------------------------
# Analysis lives in tests/helpers/premise_sensitivity_eval.py so it can be
# driven by --selftest against synthetic grids. Embedded here it could only be
# run against the live sweep, where a pass is ambiguous between "the engine
# behaves as recorded" and "the check cannot fail".
EVAL="$SCRIPT_DIR/../helpers/premise_sensitivity_eval.py"
echo -e "${CYAN}V-EVAL — can the analyzer fail?${NC}"
if ST=$(python3 "$EVAL" --selftest 2>&1); then
    pass "PS-EVAL" "analyzer self-test: $(echo "$ST" | tail -1)"
else
    fail "PS-EVAL" "analyzer self-test FAILED" "$(echo "$ST" | head -4)"
    echo -e "${RED}Aborting: an analyzer that cannot detect known violations${NC}"
    echo -e "${RED}proves nothing about the grid below.${NC}"
    exit 1
fi

ANALYSIS=$(python3 "$EVAL" "$MARGIN" "$LOAD_BEARING" "$DEFAULT_VERDICT" < "$RESULTS")

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Premise sensitivity — which setting decides the verdict?    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
python3 - "$ANALYSIS" <<'PY'
import json, sys
a = json.loads(sys.argv[1])
print("")
print("  %-9s %-7s %-5s  %-9s %s" % ("calibr.", "healing", "win", "separation", "verdict"))
print("  " + "-" * 62)
for r in a["rows"]:
    if r["err"]:
        print("  %-9s %-7s %-5s  %-9s %s" % (r["cal"], r["heal"], r["win"], "n/a", "ERROR"))
    else:
        print("  %-9s %-7s %-5s  %+9.3f %s" % (r["cal"], r["heal"], r["win"], r["sep"], r["verdict"]))
print("")
for name, fl in sorted(a["flips"].items()):
    if fl:
        print("  LOAD-BEARING  %s" % name)
        for f in fl[:3]:
            print("      %s" % f)
    else:
        print("  inert         %s (verdict unchanged either way)" % name)
PY

echo ""
python3 - "$ANALYSIS" > "$TEST_TMP/verdicts.txt" <<'PY'
import json, sys
a = json.loads(sys.argv[1])
print("ERRORS %d" % a["errors"])
print("NVERDICTS %d" % len(a["verdicts"]))
print("UNDECLARED %s" % (",".join(a["undeclared"]) or "-"))
print("STALE %s" % (",".join(a["stale"]) or "-"))
dc = a["default_cell"]
print("DEFAULT %s" % (dc["verdict"] if dc else "MISSING"))
print("DEFAULT_EXPECTED %s" % a["default_expected"])
print("OBSERVED %s" % (",".join(a["load_bearing_observed"]) or "-"))
PY
ERRORS=$(awk '/^ERRORS/{print $2}' "$TEST_TMP/verdicts.txt")
NVERDICTS=$(awk '/^NVERDICTS/{print $2}' "$TEST_TMP/verdicts.txt")
UNDECLARED=$(awk '/^UNDECLARED/{print $2}' "$TEST_TMP/verdicts.txt")
STALE=$(awk '/^STALE/{print $2}' "$TEST_TMP/verdicts.txt")
DEFAULT=$(awk '/^DEFAULT /{print $2}' "$TEST_TMP/verdicts.txt")
DEFAULT_EXP=$(awk '/^DEFAULT_EXPECTED/{print $2}' "$TEST_TMP/verdicts.txt")
OBSERVED=$(awk '/^OBSERVED/{print $2}' "$TEST_TMP/verdicts.txt")

echo -e "${CYAN}ASSERTIONS${NC}"

# PS-00: a cell that failed to run is not a data point. Without this, an
# environment where every run dies reports "no premise flips the verdict" and
# passes PS-02 by measuring nothing at all.
if [ "${ERRORS:-1}" -eq 0 ]; then
    pass "PS-00" "every cell in the grid produced a measurement"
else
    fail "PS-00" "$ERRORS cell(s) failed to run" "the grid is incomplete; nothing below is readable"
fi

if [ "${NVERDICTS:-0}" -ge 2 ]; then
    pass "PS-01" "the sweep is live ($NVERDICTS distinct verdicts across the grid)"
else
    fail "PS-01" "every cell returned the same verdict" \
         "the premises are not being varied, or none of them reaches the engine — every assertion below would pass vacuously"
fi

if [ "$UNDECLARED" = "-" ] && [ "$STALE" = "-" ]; then
    pass "PS-02" "load-bearing premises are exactly as declared ($OBSERVED)"
else
    MSG=""
    [ "$UNDECLARED" != "-" ] && MSG="undeclared premise(s) decide the verdict: $UNDECLARED"
    [ "$STALE" != "-" ] && MSG="$MSG${MSG:+; }declared but no longer load-bearing: $STALE"
    fail "PS-02" "the set of premises that decide the verdict has changed" "$MSG"
fi

if [ "$DEFAULT" = "$DEFAULT_EXP" ]; then
    pass "PS-03" "the DEFAULT configuration's verdict is $DEFAULT, as recorded"
else
    fail "PS-03" "the default configuration's verdict changed" \
         "recorded $DEFAULT_EXP, measured $DEFAULT — update DEFAULT_VERDICT and every document that rests on it"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
