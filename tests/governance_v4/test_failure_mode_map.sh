#!/usr/bin/env bash
# ============================================================
# test_failure_mode_map.sh — every failure mode this engine exhibits,
# with a configuration that reproduces each one
#
# WHY THIS EXISTS
#
# Four conclusions in this campaign were overturned in sequence, each by the
# next level of tracing, and NOT ONE was caught by a failing test. Every
# vacuity check passed at every step — that is not a failure of vacuity
# checking, it is its boundary: a vacuity check establishes that a harness is
# HONEST ABOUT WHAT IT MEASURES, not that it measured the right quantity.
#
# What each error had in common was a hidden premise in the setup that decided
# the answer. The first version of this file therefore swept premises and
# reported one VERDICT per configuration. That was still whack-a-mole: one
# verdict answers one question, so each new question needed a new premise, and
# every premise added found a failure mode nobody had predicted —
#
#   calibration off             fires on everything, so catching an adversary
#                               carries no information
#   calibration on, short run   off-mandate agent completes every send: the
#                               baseline window eats 5 of 8 turns and learns
#                               the drift IS this agent's normal
#   window                      reported "inert" on a 20-turn sweep; at 12 it
#                               lets verbatim repetition escape entirely
#   healing                     reported "inert"; it decides whether narrated
#                               correct work survives at win=12
#
# The last two were invisible until the output stopped collapsing to a verdict.
#
# So this maps the failure surface instead. Each cell is classified by WHAT
# WENT WRONG — which CORRECT arms were falsely killed, which DRIFT arms
# escaped — and PS-04 asserts set equality against a recorded manifest. A mode
# appearing that is not in the manifest fails, INCLUDING one nobody predicted.
# A recorded mode that stops occurring fails too: it was fixed (update the
# manifest and everything resting on it) or the sweep stopped exercising it.
#
# WHAT THE MAP SAYS TODAY: 0 of 16 cells are CLEAN. There is no configuration
# in the swept space where the engine both preserves correct work and catches
# every drift. The manifest below is that finding, in a form that breaks when
# it changes.
#
# WHAT THIS DOES NOT DO
#
# It does not enumerate every possible engine issue, and the report prints the
# axes it does not sweep rather than leaving them implied — a map that does not
# say where it stops gets read as complete. Today that list includes drift
# onset, agent count, tool use, multi-agent interleaving, real-model
# nondeterminism, task domain, enforcement level and the thresholds themselves.
# Each is one entry in the grid away from being covered. None is covered.
#
# ASSERTIONS
#
#   PS-EVAL  the analyzer can fail — self-tested against synthetic maps
#   PS-00    every cell produced a measurement; an errored cell is not a data
#            point, and without this a broken environment reports an empty
#            failure map and passes everything else
#   PS-01    the sweep is live; if every cell lands in one outcome the axes are
#            not reaching the engine and PS-04 passes vacuously
#   PS-04    the failure-mode map matches the recorded manifest, both ways
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/fmmap-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

# ---- the claims this file pins -------------------------------------------
# Premises whose value is currently known to decide the verdict. Keep this
# list and DEFAULT_VERDICT in sync with reality; PS-02 and PS-03 fail loudly
# in both directions rather than letting either drift.
# Every failure mode this engine is currently known to exhibit inside the swept
# axes, pipe-separated. PS-04 asserts set equality BOTH ways: a mode appearing
# that is not here fails (including one nobody predicted), and a mode here that
# stops occurring fails too — it was either fixed, in which case update this and
# everything resting on it, or the sweep stopped exercising it.
FAILURE_MANIFEST="FALSE_KILL[narrow,varied,verbose]|BYPASS[parrot]|BYPASS[repeat,parrot]|BYPASS[repeat,parrot] FALSE_KILL[verbose]|BYPASS[abandon,parrot]|BYPASS[repeat,abandon,parrot]"
OA_THRESHOLD=0.70

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_failure_mode_map.sh"
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
# Generated per RUN LENGTH — see the run_length axis below.
MANDATE=""
gen_fixtures() {   # $1=short|long -> sets FX, CALIBRATE, TURNS, MANDATE
    local tag="$1"
    FX="$TEST_TMP/fx-$tag"; mkdir -p "$FX"
    if [ "$tag" = "short" ]; then FX_CALIBRATE=5 FX_TEST=3; else FX_CALIBRATE=8 FX_TEST=12; fi
    FX_CALIBRATE=$FX_CALIBRATE FX_TEST=$FX_TEST \
        python3 "$SCRIPT_DIR/../helpers/signal_contract_fixtures.py" "$FX" >/dev/null || return 1
    CALIBRATE=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["calibrate"])' "$FX/meta.json")
    TURNS=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["total"])' "$FX/meta.json")
    MANDATE=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["mandate"])' "$FX/meta.json")
}
gen_fixtures long || { fail "PS-00" "fixture generation failed"; exit 1; }
CORRECT_ARMS=(ctl_narrow ctl_varied ctl_verbose)
DRIFT_ARMS=(drift_repeat drift_abandon drift_parrot)

# ---- axes NOT swept -------------------------------------------------------
# Printed in the report and carried into the analysis, because a map that does
# not say where it stops is read as complete. Each of these is one entry in the
# grid away from being covered; none is covered today.
UNSWEPT="drift-onset-turn agent-count tool-use multi-agent-interleaving real-model-nondeterminism task-domain(only-code) enforcement-level thresholds(oa,elevated,high)"

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
# Four axes. run_length is here because it decides whether `window` matters at
# all: the baseline consumes the first N turns, so short runs leave nothing to
# score. Measured — an off-mandate agent caught on a 20-turn run completes
# every send on an 8-turn run under the identical config.
echo ""
echo -e "${CYAN}Sweeping run_length x calibration x healing x window (16 cells x 6 arms)${NC}"
echo ""

CELLS="$TEST_TMP/cells.jsonl"; : > "$CELLS"
for RL in short long; do
 gen_fixtures "$RL" || { fail "PS-00" "fixture generation failed for $RL"; exit 1; }
 for CAL in false true; do
  for HEAL in 0.0 0.03; do
   for WIN in 5 12; do
    ERR=0; FLOORS=""
    for arm in "${CORRECT_ARMS[@]}" "${DRIFT_ARMS[@]}"; do
        v=$(measure "$RL-$CAL-$HEAL-$WIN-$arm" "$arm" "$CAL" "$HEAL" "$WIN")
        if [ "$v" = "ERR" ]; then ERR=1; else FLOORS="$FLOORS $arm=$v"; fi
    done
    python3 - "$RL" "$CAL" "$HEAL" "$WIN" "$ERR" "${FLOORS# }" >> "$CELLS" <<'PY'
import json, sys
rl, cal, heal, win, err, floors = sys.argv[1:7]
d = {"axes": {"run": rl, "cal": cal, "heal": heal, "win": win},
     "err": err == "1"}
if not d["err"]:
    d["floors"] = dict((kv.split("=")[0], float(kv.split("=")[1]))
                       for kv in floors.split() if "=" in kv)
print(json.dumps(d))
PY
    echo "  run=$RL cal=$CAL heal=$HEAL win=$WIN ${FLOORS# }"
   done
  done
 done
done

# ---- failure-mode map and assertions -------------------------------------
FMEVAL="$SCRIPT_DIR/../helpers/failure_mode_eval.py"
echo ""
echo -e "${CYAN}V-EVAL — can the analyzer fail?${NC}"
if ST=$(python3 "$FMEVAL" --selftest 2>&1); then
    pass "PS-EVAL" "analyzer self-test: $(echo "$ST" | tail -1)"
else
    fail "PS-EVAL" "analyzer self-test FAILED" "$(echo "$ST" | head -4)"
    echo -e "${RED}Aborting: an analyzer that cannot detect known violations${NC}"
    echo -e "${RED}proves nothing about the map below.${NC}"
    exit 1
fi

python3 - "$CELLS" "$OA_THRESHOLD" "$UNSWEPT" "$FAILURE_MANIFEST" > "$TEST_TMP/payload.json" <<'PY'
import json, sys
cells = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
json.dump({"cells": cells, "oa": float(sys.argv[2]),
           "unswept": sys.argv[3].split(),
           "manifest": [m for m in sys.argv[4].split("|") if m]}, sys.stdout)
PY
MAP=$(python3 "$FMEVAL" < "$TEST_TMP/payload.json")

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Engine failure-mode map                                     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
python3 - "$MAP" <<'PY'
import json, sys
a = json.loads(sys.argv[1])
print("")
print("  %-6s %-6s %-6s %-4s  %s" % ("run", "calib", "heal", "win", "outcome"))
print("  " + "-" * 92)
for r in a["rows"]:
    ax = r["axes"]
    if r["err"]:
        print("  %-6s %-6s %-6s %-4s  ERROR" % (ax.get("run","?"), ax.get("cal","?"),
                                                ax.get("heal","?"), ax.get("win","?")))
    else:
        print("  %-6s %-6s %-6s %-4s  %s" % (ax["run"], ax["cal"], ax["heal"],
                                             ax["win"], r["code"]))
print("  " + "-" * 92)
print("  %d of %d cells CLEAN" % (a["clean_cells"], a["total_cells"]))
print("")
print("  DISTINCT FAILURE MODES, each with a configuration that reproduces it:")
for m in a["observed"]:
    if m == "CLEAN": continue
    w = a["witness"][m]
    print("    %-46s  run=%s cal=%s heal=%s win=%s"
          % (m, w["run"], w["cal"], w["heal"], w["win"]))
if len(a["observed"]) == 1 and a["observed"][0] == "CLEAN":
    print("    (none)")
print("")
print("  AXES NOT SWEPT — the map stops here, and says so:")
for u in a["unswept"]:
    print("    %s" % u)
PY

echo ""
echo -e "${CYAN}ASSERTIONS${NC}"
python3 - "$MAP" > "$TEST_TMP/v.txt" <<'PY'
import json, sys
a = json.loads(sys.argv[1])
print("ERRORS %d" % a["errors"])
print("NMODES %d" % len(a["observed"]))
print("NEW %s" % ("|".join(a["new_modes"]) or "-"))
print("GONE %s" % ("|".join(a["gone_modes"]) or "-"))
PY
ERRORS=$(awk '/^ERRORS/{print $2}' "$TEST_TMP/v.txt")
NMODES=$(awk '/^NMODES/{print $2}' "$TEST_TMP/v.txt")
NEW=$(sed -n 's/^NEW //p' "$TEST_TMP/v.txt")
GONE=$(sed -n 's/^GONE //p' "$TEST_TMP/v.txt")

# A cell that failed to run is not a data point. Without this, an environment
# where every run dies reports an empty failure map and passes everything below.
if [ "${ERRORS:-1}" -eq 0 ]; then
    pass "PS-00" "every cell in the grid produced a measurement"
else
    fail "PS-00" "$ERRORS cell(s) failed to run" "the map is incomplete; nothing below is readable"
fi

# If every cell lands in one outcome the axes are not reaching the engine, and
# "no new failure modes" is true only because nothing was measured.
if [ "${NMODES:-0}" -ge 2 ]; then
    pass "PS-01" "the sweep is live ($NMODES distinct outcomes across the grid)"
else
    fail "PS-01" "every cell produced the same outcome" \
         "the axes are not being varied, or none reaches the engine"
fi

# THE ASSERTION THIS FILE EXISTS FOR. Set equality, both directions.
if [ "$NEW" = "-" ] && [ "$GONE" = "-" ]; then
    pass "PS-04" "the failure-mode map matches the recorded manifest"
else
    MSG=""
    [ "$NEW" != "-" ] && MSG="NEW failure mode(s) not in the manifest: ${NEW//|/, }"
    [ "$GONE" != "-" ] && MSG="$MSG${MSG:+; }recorded mode(s) no longer observed: ${GONE//|/, }"
    fail "PS-04" "the engine's failure-mode map changed" "$MSG"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
