#!/usr/bin/env bash
# ============================================================
# test_pulse_uniformity.sh — what does the governance pulse's
# "suspiciously uniform" signal actually measure?
#
# The pulse degrades when governance has not objected to anything for
# consecutive_passes_suspicion in a row. The intent is a detection-bypass
# tripwire: if the engine never has a word to say about an agent for fifty
# turns, either the agent is spotless or the instrumentation is not looking.
#
# It was counting the wrong thing. Every recordPass() site in the engine is a
# static source, capability, plugin or runtime-pin check — there is no
# recordPass anywhere on the agent-behaviour path, because CDD, BSD, admission
# and output admissibility only call enforce(), and only on failure. So the
# counter measured how much clean SOURCE had been scanned. Gating it on
# agent_governance_active_ did not help: that flag is a one-way latch set at
# the first agent.create(), so every codegen.run() and polyglot block after an
# agent exists fed ~5 uniform passes straight in. A codegen-heavy run crossed
# the threshold on volume alone and sat at DEGRADED for the rest of the
# process, with no agent misbehaviour anywhere in the picture — which is how
# three consecutive living-script runs ended degraded at coherence 1.0 while
# emitting zero health warnings.
#
# The counter now ticks once per analyzed agent turn and resets on any
# enforcement, so the threshold is denominated in turns, as its name implies.
#
#   PU-01  codegen volume, few agent turns   -> must NOT degrade  (the defect)
#   PU-02  long clean agent run              -> MUST degrade      (still alive)
#   PU-03  a blocked turn resets the streak  -> must NOT degrade
#   PU-04  every degraded verdict names its own cause
#   PU-05  every verdict transition leaves a telemetry record, carrying
#          the streak that caused it rather than the post-reset zero
#
# PU-01 and PU-04 fail against the pre-fix binary; PU-02 and PU-03 guard the
# fix against being a silent disable.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_pulse_uniformity.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${PULSEUNIF_TMP:-${_SYSTMP}/pulseunif-$$}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

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

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_PULSEUNIF="fake-key-pulseunif"

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not available — skipping"; exit 0
fi

source "$SCRIPT_DIR/../helpers/stub_launch.sh"  # D1: shared hardened launcher

# Distinct, substantive responses so no CDD signal has cause to fire — the
# scenarios must differ only in codegen volume and turn count, never in whether
# the agent looked drifty.
make_fixture() {  # $1=path
    python3 - "$1" <<'PY'
import json, sys
topics = [
    "ingestion adapters normalise inbound records before the schema gate",
    "the schema gate rejects malformed payloads and emits a rejection receipt",
    "checkpointing writes offsets so a restart resumes without replay",
    "backpressure throttles producers when the sink queue depth climbs",
    "the dead letter queue retains poison messages for manual triage",
    "partition assignment rebalances when a consumer leaves the group",
    "compaction collapses superseded keys during the nightly maintenance window",
    "the metrics exporter publishes lag and throughput to the collector",
    "credential rotation swaps sink tokens without draining the pipeline",
    "schema evolution admits additive field changes but blocks removals",
    "replay tooling reruns a bounded offset range into a shadow sink",
    "the audit sidecar mirrors every rejection into cold storage",
]
resps = []
for i in range(120):
    t = topics[i % len(topics)]
    resps.append({
        "content": "Batch %d: %s. Observed throughput nominal, no anomalies raised "
                   "during this window and the downstream sink acknowledged every "
                   "committed offset." % (i, t),
        "input_tokens": 140, "output_tokens": 60,
    })
json.dump({"responses": resps}, open(sys.argv[1], "w"))
PY
}

# $1=workdir $2=port $3=suspicion $4=codegen_enabled(true|false)
write_config() {
    cat > "$1/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true, "patterns": [
    { "name": "inert_never_matches", "sequence": ["ENV_READ", "NET_CONNECT", "ENV_READ"], "max_gap": 1, "level": "advisory" }
  ] },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "coherence_threshold": 0.0,
    "adaptive_baseline_enabled": false
  },
  "governance_health": {
    "enabled": true,
    "check_after_turns": 3,
    "governance_entropy_warning": 0.5,
    "consecutive_passes_suspicion": $3,
    "impaired_degraded_turns": 2,
    "impaired_signal_count": 2,
    "pulse_cooldown_turns": 2
  },
  "codegen": { "enabled": $4, "languages": ["python"] },
  "circuit_breaker": { "enabled": true },
  "agents": {
    "worker": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$2",
      "api_key_env": "FAKE_KEY_PULSEUNIF",
      "max_tokens": 400, "max_turns": 200,
      "system_prompt": "You maintain a streaming data pipeline. Report on its stages, throughput and failure handling."
    }
  }
}
GOVEOF
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

# Emits a TURN| line per turn plus one FINAL| line. The pulse sawtooths —
# reaching DEGRADED resets the streak, which lets it recover two turns later —
# so a terminal sample says nothing about whether the tripwire ever tripped.
# $1=workdir $2=turns $3=codegen_calls_per_turn
write_script() {
    cat > "$1/run.naab" <<NAABEOF
use agent
use governance
use codegen

main {
    let h = agent.create("worker")
    let i = 0
    while (i < $2) {
        agent.send(h, "Report on pipeline batch " + string(i) + ", covering throughput and any rejections.")
        let c = 0
        while (c < $3) {
            try { codegen.run("python", "print('ok')") } catch (e) { }
            c = c + 1
        }
        let t = governance.health()
        print("TURN|verdict=" + t.get("verdict") + "|passes=" + string(t.get("consecutive_passes")) + "|why=" + t.get("degradation_reasons"))
        i = i + 1
    }
    let g = governance.health()
    let out = "FINAL|verdict=" + g.get("verdict") + "|passes=" + string(g.get("consecutive_passes")) + "|why=" + g.get("degradation_reasons")
    print(out)
}
NAABEOF
}

run_scenario() {  # $1=name $2=turns $3=cg_per_turn $4=suspicion $5=codegen_enabled
    local wd="$TEST_TMP/$1"
    mkdir -p "$wd"
    make_fixture "$wd/fixture.json"
    start_stub "$wd/fixture.json" "$wd" || { echo "stub failed"; return 1; }
    write_config "$wd" "$STUB_PORT" "$4" "$5"
    write_script "$wd" "$2" "$3"
    (cd "$wd" && "$NAAB" run.naab 2>"$wd/err.txt" | grep -E '^(TURN|FINAL)\|') > "$wd/out.txt"
    stop_stub
    cat "$wd/out.txt"
}

echo -e "${CYAN}=== Governance pulse: what does 'uniform passes' measure? ===${NC}"
echo ""

# ------------------------------------------------------------------
# PU-01 — codegen volume must not degrade the pulse
# 6 agent turns, 3 codegen calls each. Suspicion is 20: comfortably above the
# turn count, far below the ~90 static passes the codegen scanning produces.
# The BSD default patterns are replaced with a single inert one (an empty
# array falls back to the defaults) so codegen_rapid_fire cannot reset the
# streak every turn and mask the accumulation being tested.
# ------------------------------------------------------------------
echo -e "${CYAN}PU-01: codegen volume, few agent turns${NC}"
OUT1=$(run_scenario cgvol 6 3 20 true)
NTURN1=$(echo "$OUT1" | grep -c '^TURN|')
BAD1=$(echo "$OUT1" | grep -c 'verdict=degraded\|verdict=impaired')
MAXP1=$(echo "$OUT1" | grep -oP 'passes=\K[0-9]+' | sort -n | tail -1)
if [ "${NTURN1:-0}" -lt 6 ]; then
    fail "PU-01" "scenario produced $NTURN1 pulse samples, expected 6" \
         "$(tail -3 "$TEST_TMP/cgvol/err.txt" 2>/dev/null)"
elif [ "${BAD1:-0}" -eq 0 ]; then
    pass "PU-01" "codegen volume did not degrade the pulse (6 turns, max streak ${MAXP1:-0})"
else
    fail "PU-01" "pulse degraded on codegen volume alone ($BAD1 of $NTURN1 samples)" \
         "the streak is counting scanned source, not agent turns"
fi
# The streak must be bounded by the turn count, not by the ~180 static checks
# that scanning six codegen calls per turn produces.
if [ -n "${MAXP1:-}" ] && [ "$MAXP1" -le 8 ]; then
    pass "PU-01b" "clean-turn streak bounded by turns (max $MAXP1 <= 8)"
elif [ -n "${MAXP1:-}" ]; then
    fail "PU-01b" "streak reached $MAXP1 across 6 agent turns" \
         "consecutive_passes is still counting per-check, not per-turn"
else
    skip "PU-01b" "no streak value"
fi
echo ""

# ------------------------------------------------------------------
# PU-02 — a long clean agent run must still trip the tripwire
# 30 turns, no codegen, suspicion 5.
# ------------------------------------------------------------------
echo -e "${CYAN}PU-02: long clean agent run${NC}"
OUT2=$(run_scenario clean 30 0 5 false)
NTURN2=$(echo "$OUT2" | grep -c '^TURN|')
DEG2=$(echo "$OUT2" | grep -c 'verdict=degraded\|verdict=impaired')
WHY2=$(echo "$OUT2" | grep -oP 'why=\K[a-z_,]+' | sort -u | tr '\n' ' ')
if [ "${NTURN2:-0}" -lt 30 ]; then
    fail "PU-02" "scenario produced $NTURN2 pulse samples, expected 30" \
         "$(tail -3 "$TEST_TMP/clean/err.txt" 2>/dev/null)"
elif [ "${DEG2:-0}" -gt 0 ]; then
    pass "PU-02" "long clean run trips the tripwire ($DEG2 of $NTURN2 samples degraded)"
else
    fail "PU-02" "30 clean turns past a suspicion of 5 never degraded" \
         "the uniformity signal is dead, not merely re-scoped"
fi
if echo "${WHY2:-}" | grep -q "uniform_passes"; then
    pass "PU-02b" "degradation attributed to uniform_passes"
elif [ "${DEG2:-0}" -gt 0 ]; then
    fail "PU-02b" "degraded for a reason other than uniform_passes: '${WHY2:-}'" \
         "the scenario is not testing the signal it claims to test"
else
    skip "PU-02b" "never degraded"
fi
echo ""

# ------------------------------------------------------------------
# PU-03 — enforcement resets the streak
# Same 30 turns and suspicion 5, but CDD enforces every turn (coherence
# threshold raised so the repetitive stub trips it), so the streak can never
# accumulate past 1 and the pulse must stay healthy.
# ------------------------------------------------------------------
echo -e "${CYAN}PU-03: enforcement resets the streak${NC}"
WD3="$TEST_TMP/blocked"
mkdir -p "$WD3"
python3 - "$WD3/fixture.json" <<'PY'
import json, sys
# Verbatim-identical responses: response_repetition fires every turn, so CDD
# has something to enforce on each one.
r = {"content": "Same answer.", "input_tokens": 140, "output_tokens": 60}
json.dump({"responses": [dict(r) for _ in range(120)]}, open(sys.argv[1], "w"))
PY
make_fixture_noop=1
start_stub "$WD3/fixture.json" "$WD3" >/dev/null
cat > "$WD3/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true, "patterns": [
    { "name": "inert_never_matches", "sequence": ["ENV_READ", "NET_CONNECT", "ENV_READ"], "max_gap": 1, "level": "advisory" }
  ] },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "coherence_threshold": 0.99, "adaptive_baseline_enabled": false
  },
  "governance_health": {
    "enabled": true, "check_after_turns": 3,
    "governance_entropy_warning": 0.5,
    "consecutive_passes_suspicion": 5,
    "impaired_degraded_turns": 2, "impaired_signal_count": 2,
    "pulse_cooldown_turns": 2
  },
  "circuit_breaker": { "enabled": true },
  "agents": {
    "worker": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_PULSEUNIF",
      "max_tokens": 400, "max_turns": 200,
      "system_prompt": "You maintain a streaming data pipeline. Report on its stages, throughput and failure handling."
    }
  }
}
GOVEOF
(cd "$WD3" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
write_script "$WD3" 30 0
(cd "$WD3" && "$NAAB" run.naab 2>"$WD3/err.txt" | grep -E '^(TURN|FINAL)\|') > "$WD3/out.txt"
stop_stub
OUT3=$(cat "$WD3/out.txt")
NTURN3=$(echo "$OUT3" | grep -c '^TURN|')
MAXP3=$(echo "$OUT3" | grep -oP 'passes=\K[0-9]+' | sort -n | tail -1)
WHY3=$(echo "$OUT3" | grep -oP 'why=\K[a-z_,]+' | sort -u | tr '\n' ' ')
if [ "${NTURN3:-0}" -lt 30 ]; then
    fail "PU-03" "scenario produced $NTURN3 pulse samples, expected 30" \
         "$(tail -3 "$WD3/err.txt" 2>/dev/null)"
elif [ "${MAXP3:-0}" -le 5 ]; then
    pass "PU-03" "enforcement kept the streak under the threshold (max $MAXP3)"
else
    fail "PU-03" "streak reached $MAXP3 despite enforcement every turn" \
         "a blocked turn is being counted as a clean turn"
fi
if echo "${WHY3:-}" | grep -q "uniform_passes"; then
    fail "PU-03b" "uniform_passes fired on a run that was blocked every turn" \
         "the reset is not reaching the signal"
else
    pass "PU-03b" "uniform_passes did not fire on a continuously-blocked run"
fi
echo ""

# ------------------------------------------------------------------
# PU-04 — a degraded verdict must name its cause
# Reuses PU-02's run: whatever the verdict, the reason field and the verdict
# must agree. An unattributable DEGRADED is what made this defect cost ten
# runs to find.
# ------------------------------------------------------------------
echo -e "${CYAN}PU-04: degradation is attributable${NC}"
UNATTRIB=$(echo "$OUT2" | grep -E 'verdict=(degraded|impaired)' | grep -c 'why=$')
if [ "${DEG2:-0}" -eq 0 ]; then
    skip "PU-04" "PU-02 never degraded — nothing to attribute"
elif [ "${UNATTRIB:-0}" -eq 0 ]; then
    pass "PU-04" "every degraded sample names its cause ($DEG2 samples, reasons: ${WHY2:-})"
else
    fail "PU-04" "$UNATTRIB of $DEG2 degraded samples carry an empty reason field" \
         "governance.health() cannot explain its own verdict"
fi
echo ""

# ------------------------------------------------------------------
# PU-05 — a verdict transition must emit PULSE_TRANSITION
# The pulse sawtooths, so a degrade followed by a recovery between two
# governance.health() samples used to be visible only as an unexplained
# two-step jump in the evidence epoch. Reconstructing one from a live run took
# four rounds of forensics; the transitions are now events.
# ------------------------------------------------------------------
echo -e "${CYAN}PU-05: transitions are recorded${NC}"
TELE="$TEST_TMP/clean/tele.jsonl"
if [ ! -f "$TELE" ]; then
    fail "PU-05" "no telemetry from the PU-02 scenario"
else
    PT=$(python3 - "$TELE" <<'PYEOF'
import json, sys
rows = []
for ln in open(sys.argv[1]):
    try: e = json.loads(ln)
    except Exception: continue
    if e.get("event_type") == "PULSE_TRANSITION":
        rows.append("%s->%s why=%s passes=%s" % (e.get("from_verdict"), e.get("to_verdict"),
                                                 e.get("degradation_reasons"),
                                                 e.get("consecutive_passes")))
print(len(rows))
for r in rows[:4]: print(r)
PYEOF
)
    PT_N=$(echo "$PT" | head -1)
    if [ "${PT_N:-0}" -gt 0 ]; then
        pass "PU-05" "$PT_N PULSE_TRANSITION events ($(echo "$PT" | sed -n 2p))"
    else
        fail "PU-05" "pulse degraded $DEG2 times but emitted no PULSE_TRANSITION" \
             "a verdict transition that leaves no record is only recoverable from the epoch counter"
    fi
    # A uniform_passes degradation must report the streak that caused it. The
    # epoch boundary zeroes consecutive_passes at the transition, so reading it
    # after the fact reports 0 for every transition and makes the stated cause
    # unfalsifiable — live run 19 recorded exactly that.
    PU_STREAK=$(echo "$PT" | grep -m1 -- "->degraded why=uniform_passes" | grep -oP 'passes=\K[0-9]+')
    if [ -z "${PU_STREAK:-}" ]; then
        skip "PU-05c" "no uniform_passes degradation in this run"
    elif [ "$PU_STREAK" -gt 5 ]; then
        pass "PU-05c" "degradation reports the streak that caused it (passes=$PU_STREAK > suspicion 5)"
    else
        fail "PU-05c" "uniform_passes degradation reports passes=$PU_STREAK, at or below the threshold of 5" \
             "the streak is being read after the epoch boundary reset it"
    fi

    # Both directions must be recorded — recording only the degrade would leave
    # a recovery indistinguishable from a verdict that never moved.
    if echo "$PT" | grep -q -- "->healthy"; then
        pass "PU-05b" "recovery transitions recorded, not just degradations"
    elif [ "${PT_N:-0}" -gt 0 ]; then
        fail "PU-05b" "only degradations recorded, no recovery" \
             "an unpaired transition still leaves the epoch arithmetic unexplainable"
    else
        skip "PU-05b" "no transitions"
    fi
fi

echo ""

echo "────────────────────────────────────────"
echo -e "Passed:  ${GREEN}${PASS_COUNT}${NC}"
echo -e "Failed:  ${RED}${FAIL_COUNT}${NC}"
echo -e "Skipped: ${YELLOW}${SKIP_COUNT}${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}${FAILURES}"
    exit 1
fi
exit 0
