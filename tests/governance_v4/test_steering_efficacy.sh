#!/usr/bin/env bash
# ============================================================
# test_steering_efficacy.sh — does the engine's steering actually steer?
#
# THE QUESTION, AND WHY IT WAS UNANSWERABLE
#
# The engine has two mechanisms for pulling a drifting agent back:
#
#   mandate reinforcement   prepends "[Task Reminder: <system_prompt>]" every
#                           N turns. Gate is `turn % interval == 0` — a pure
#                           interval, so it fires during the adaptive baseline
#                           window like any other turn.
#   coherence correction    prepends graduated text once coherence falls below
#                           a threshold. Gate is `coherence < threshold`.
#
# Whether either one WORKS had never been measured, and could not be: every
# fixture in this campaign replays fixed responses regardless of what the
# engine injects, so a run in which steering worked perfectly and a run in
# which it did nothing produce byte-identical output. The claim was untested in
# both directions.
#
# agent_stub.py routes on request BODY CONTENT, so a fixture can answer
# differently once an injected marker appears. That separates:
#
#   IS THE LOOP WIRED     does the marker reach the request body, and does the
#                         engine's own measurement notice the agent coming back?
#                         <- this file answers that
#   DOES A MODEL COMPLY   <- this file does NOT answer that and does not claim to
#
# THE ARMS
#
#   responsive   returns to the task when it sees an injection marker
#   inert        ignores markers and keeps drifting
#
# Both drift identically until the first injection, so any divergence after it
# is caused by the injection and nothing else.
#
# A NOTE ON WHAT A NEGATIVE RESULT WOULD MEAN
#
# If the responsive arm does not recover, that is ambiguous between "steering
# is not wired" and "the marker strings drifted out of sync with
# agent_impl.cpp". SF-01 resolves it: it asserts a marker actually reached a
# logged request body BEFORE any recovery claim is read. Without that, a
# renamed marker would silently report "steering does not work".
#
# ASSERTIONS
#
#   SF-01  a steering marker reached the request body (positive control — every
#          claim below is unreadable without it)
#   SF-02  the two arms actually diverged (proves routing fired; without this
#          "no difference" is explained equally well by a broken fixture)
#   SF-03  the drift is detectable at all — the inert arm's coherence falls
#          (positive control on the scenario)
#   SF-04  the engine NOTICES recovery: responsive-arm coherence ends higher
#          than inert-arm coherence
#
# SF-04 is the finding. SF-01 to SF-03 are what make it readable.
#
# READ THE RATES, NOT JUST THE PASS
#
# The pass/fail says steering is wired. The decision-relevant number is the
# RATE ASYMMETRY printed beside each arm, and it is easy to misread the arms
# without it.
#
# Measured: recovery arrives at exactly +0.030/turn — precisely
# coherence_natural_healing — while an average drift turn costs -0.185. One
# damage turn therefore takes ~6.5 clean turns to undo, and a floored agent
# needs ~25 clean turns to become admissible again. Break-even is a drift rate
# of about 13% of turns.
#
# The responsive arm here drifts on every UNMARKED turn (~43%), because the
# stub has no memory and the fixture can only express "complies on the turn it
# is told". It is therefore below break-even BY CONSTRUCTION OF THE FIXTURE,
# and its decline says nothing about whether the engine can recover an agent
# that stays corrected. Independent evidence that it can:
# examples/drift-discrimination with calibration on climbs 0.030 -> 0.360 over
# 12 sustained correct turns, the same +0.0275/turn.
#
# The engine defect this exposes is NOT that steering fails. It is that the
# block decision has no memory of the steer: an agent climbing at the
# configured rate is indistinguishable, to every gate, from one sitting at the
# same coherence and not climbing. escalation_effectiveness measures exactly
# that difference and nothing reads it.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/steereff-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_steering_efficacy.sh"
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
[ -x "$NAAB" ] || { skip "SF-00" "build/naab-lang not found"; exit 0; }

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_STEER="fake-key-steering"
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

TURNS=14
FX="$TEST_TMP/fx"; mkdir -p "$FX"
python3 "$SCRIPT_DIR/../helpers/steering_fixtures.py" "$FX" "$TURNS" >/dev/null || {
    fail "SF-00" "fixture generation failed"; exit 1; }
MANDATE=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["mandate"])' "$FX/meta.json")

# Both steering mechanisms are enabled so the test covers the preventive and
# the reactive path in one run. Reinforcement interval 3 guarantees an
# injection lands early, INCLUDING inside the adaptive baseline window — that
# is the case the reactive path structurally cannot reach, since coherence
# stays near its ceiling while the statistical signals are suppressed.
run_arm() {  # $1 = responsive|inert
    local arm="$1"
    local d="$TEST_TMP/$arm"
    mkdir -p "$d"
    start_stub "$FX/$arm.json" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    cat > "$d/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": true, "adaptive_baseline_window": 5,
    "coherence_natural_healing": 0.03,
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": {
    "enabled": true, "step_up_enabled": false,
    "mandate_reinforcement_enabled": true,
    "mandate_reinforcement_interval": 3,
    "coherence_correction_enabled": true,
    "coherence_correction_threshold": 0.85,
    "coherence_correction_cooldown_turns": 2,
    "output_admissibility": {
      "enabled": true, "threshold": 0.70,
      "action": "quarantine", "max_quarantine_streak": 0
    }
  },
  "agents": {
    "builder": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_STEER",
      "max_tokens": 200, "max_turns": 40,
      "system_prompt": "$MANDATE"
    }
  }
}
GOVEOF
    sign_govern "$d"
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
    case "$out" in *RUN_DONE*) ;; *) echo "RUN_FAIL"; return ;; esac
    echo "OK"
}

echo ""
echo -e "${CYAN}Running both arms (${TURNS} turns each)${NC}"
R_RESP=$(run_arm responsive); echo "  responsive: $R_RESP"
R_INERT=$(run_arm inert);     echo "  inert:      $R_INERT"
if [ "$R_RESP" != "OK" ] || [ "$R_INERT" != "OK" ]; then
    fail "SF-00" "an arm did not complete (responsive=$R_RESP inert=$R_INERT)" \
         "nothing below is readable"
    exit 1
fi

# ---- analysis -------------------------------------------------------------
ANALYSIS=$(python3 - "$TEST_TMP/responsive" "$TEST_TMP/inert" "$FX/meta.json" <<'PY'
import glob, json, os, sys

resp_dir, inert_dir, meta_path = sys.argv[1], sys.argv[2], sys.argv[3]
meta = json.load(open(meta_path))
markers, drift = meta["markers"], meta["drift"]

def requests(d):
    out = []
    for p in sorted(glob.glob(os.path.join(d, "req_*.json")),
                    key=lambda x: int(os.path.basename(x)[4:-5])):
        try: out.append(open(p).read())
        except Exception: pass
    return out

def cdd(d):
    rows = []
    p = os.path.join(d, "tele.jsonl")
    if not os.path.exists(p): return rows
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        if e.get("event_type") == "CDD_TURN" and e.get("analyzed") == "true":
            try: rows.append((int(e.get("turn", -1)), float(e.get("coherence", 1.0)),
                              e.get("baseline_state", "?")))
            except (TypeError, ValueError): pass
    return sorted(rows)

def injections(d):
    p, n = os.path.join(d, "tele.jsonl"), 0
    if not os.path.exists(p): return 0
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        if e.get("event_type") == "MANDATE_INJECTION": n += 1
    return n

res = {}
for name, d in (("responsive", resp_dir), ("inert", inert_dir)):
    reqs = requests(d)
    marked = [i + 1 for i, b in enumerate(reqs) if any(m in b for m in markers)]
    rows = cdd(d)
    res[name] = {
        "requests": len(reqs),
        "marked_turns": marked,
        "injections": injections(d),
        "coherence": [c for _, c, _ in rows],
        "final": rows[-1][1] if rows else None,
        "min": min((c for _, c, _ in rows), default=None),
        "states": [s for _, _, s in rows],
    }

# Divergence: the arms share a response stream until steering changes it.
def replies(d):
    out = []
    for p in sorted(glob.glob(os.path.join(d, "req_*.json")),
                    key=lambda x: int(os.path.basename(x)[4:-5])):
        out.append(p)
    return out
res["drift_line"] = drift
print(json.dumps(res))
PY
)

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Steering efficacy                                           |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
python3 - "$ANALYSIS" <<'PY'
import json, sys
a = json.loads(sys.argv[1])
for name in ("responsive", "inert"):
    r = a[name]
    print("")
    print("  %s" % name.upper())
    print("    requests logged      %s" % r["requests"])
    print("    MANDATE_INJECTION    %s" % r["injections"])
    print("    turns carrying a marker in the request body: %s"
          % (r["marked_turns"] or "NONE"))
    print("    coherence            %s" % " ".join("%.2f" % c for c in r["coherence"]))
    print("    min / final          %s / %s" % (r["min"], r["final"]))
    c = r["coherence"]
    gains = [c[i]-c[i-1] for i in range(1, len(c)) if c[i] > c[i-1]]
    losses = [c[i-1]-c[i] for i in range(1, len(c)) if c[i] < c[i-1]]
    if gains and losses:
        g, l = sum(gains)/len(gains), sum(losses)/len(losses)
        print("    recovery %+.4f/turn over %d turns   damage -%.4f/turn over %d turns"
              % (g, len(gains), l, len(losses)))
        print("    asymmetry            one damage turn costs %.1f clean turns" % (l/g))
        print("    floor -> admissible  %.0f clean turns at this recovery rate" % (0.70/g))
        print("    break-even drift     %.0f%% of turns" % (100.0*g/(g+l)))
PY

echo ""
echo -e "${CYAN}ASSERTIONS${NC}"
python3 - "$ANALYSIS" > "$TEST_TMP/v.txt" <<'PY'
import json, sys
a = json.loads(sys.argv[1])
r, i = a["responsive"], a["inert"]
print("MARKED_RESP %d" % len(r["marked_turns"]))
print("MARKED_INERT %d" % len(i["marked_turns"]))
print("INJ_RESP %d" % r["injections"])
print("COH_RESP %s" % (r["final"] if r["final"] is not None else "NA"))
print("COH_INERT %s" % (i["final"] if i["final"] is not None else "NA"))
print("MIN_INERT %s" % (i["min"] if i["min"] is not None else "NA"))
print("DIVERGED %d" % (1 if r["coherence"] != i["coherence"] else 0))
PY
MARKED_RESP=$(awk '/^MARKED_RESP/{print $2}' "$TEST_TMP/v.txt")
INJ_RESP=$(awk '/^INJ_RESP/{print $2}' "$TEST_TMP/v.txt")
COH_RESP=$(awk '/^COH_RESP/{print $2}' "$TEST_TMP/v.txt")
COH_INERT=$(awk '/^COH_INERT/{print $2}' "$TEST_TMP/v.txt")
MIN_INERT=$(awk '/^MIN_INERT/{print $2}' "$TEST_TMP/v.txt")
DIVERGED=$(awk '/^DIVERGED/{print $2}' "$TEST_TMP/v.txt")

# SF-01 — POSITIVE CONTROL. Everything below is unreadable without it: if no
# marker ever reached a request body, "the responsive arm did not recover" is
# explained just as well by a marker string that drifted out of sync with
# agent_impl.cpp as by steering being unwired.
if [ "${MARKED_RESP:-0}" -gt 0 ] && [ "${INJ_RESP:-0}" -gt 0 ]; then
    pass "SF-01" "steering reached the request body ($MARKED_RESP marked request(s), $INJ_RESP MANDATE_INJECTION event(s))"
else
    fail "SF-01" "no steering marker reached any request body" \
         "marked=$MARKED_RESP injections=$INJ_RESP — check the marker strings against agent_impl.cpp before reading anything else"
    echo -e "${RED}Aborting: without an injection there is nothing to measure.${NC}"
    exit 1
fi

# SF-03 — POSITIVE CONTROL on the scenario. If the inert arm never loses
# coherence, the drift is undetectable in this config and a recovery gap would
# be meaningless.
if python3 -c 'import sys; sys.exit(0 if sys.argv[1]!="NA" and float(sys.argv[1]) < 0.70 else 1)' "$MIN_INERT"; then
    pass "SF-03" "the drift is detectable — inert arm coherence fell to $MIN_INERT"
else
    fail "SF-03" "the inert arm never lost coherence (min=$MIN_INERT)" \
         "the scenario expresses no detectable drift, so a recovery gap would mean nothing"
fi

# SF-02 — the arms must actually differ. Without this, "no gap" is explained
# equally well by routing never firing as by steering not working.
if [ "${DIVERGED:-0}" -eq 1 ]; then
    pass "SF-02" "the arms diverged — routing on the injection marker fired"
else
    fail "SF-02" "responsive and inert produced identical coherence traces" \
         "the fixture routing did not fire; the arms were never different"
fi

# SF-04 — THE FINDING.
if python3 -c 'import sys; sys.exit(0 if sys.argv[1]!="NA" and sys.argv[2]!="NA" and float(sys.argv[1]) > float(sys.argv[2]) else 1)' "$COH_RESP" "$COH_INERT"; then
    pass "SF-04" "the engine notices recovery — responsive $COH_RESP vs inert $COH_INERT"
else
    fail "SF-04" "steering produced no measurable recovery (responsive $COH_RESP, inert $COH_INERT)" \
         "an agent that returns to task after being told scores no better than one that ignores it"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
