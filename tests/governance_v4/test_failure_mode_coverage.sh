#!/usr/bin/env bash
# ============================================================
# test_failure_mode_coverage.sh — can ONE signal replace the aggregate?
#
# test_signal_discrimination and test_drift_sensitivity both concluded that
# mandate_alignment alone matches the full signal set: same separation, same
# 75% detection threshold, fewer false positives. That is an argument for
# gating kills on it alone — and it is close to circular, because both tests
# used a single failure mode (topic abandonment) and mandate_alignment is the
# signal built to detect topic abandonment.
#
# This tests coverage across failure modes the signal was NOT designed for.
# Six agent behaviours, each run under mandate_alignment alone and under the
# default content-aware set:
#
#   compliant        control — must not be caught by either
#   topic_abandon    answers a financial mandate with unrelated prose
#   keyword_stuffed  off-topic substance carrying mandate VOCABULARY — the
#                    gaming attack. If one signal is the only kill gate, an
#                    adversary knows exactly which words to include.
#   repetition       identical response every turn (agent stuck in a loop)
#   degenerate       near-empty replies
#   instruction_ignore  fluent, on-domain, answers a DIFFERENT question than
#                    the one asked
#
# The question is not which config scores lower. It is whether anything is
# caught by the aggregate and missed by mandate_alignment alone. Every such
# mode is coverage that gating on one signal would delete.
#
# step_up_enabled is off so challenge passes cannot refund the penalties.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/fmcov-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

# EXPERIMENT (reversible in one commit). build-windows has stalled inside
# "CLI tests — shell suites" four times: the step sits in_progress ~47 minutes,
# the runner is killed service-side, and the log archive 404s. timeout-minutes
# has now failed to fire TWICE, so the runner cannot enforce its own step
# timeout — we cannot read our way to the cause, only change the outcome.
#
# A live observation caught it hanging at the test_challenge_discrimination.sh
# header, immediately after another stub-backed suite passed. These 9 suites are
# the only ones that run a Python HTTP server and talk to it from a native
# Windows binary under MSYS2, so they are the region to exclude first.
#
# Read the next Windows run as the result:
#   green      -> these suites are implicated; narrow from 9
#   stalls     -> they are exonerated; the cause is elsewhere in the phase
#
# Coverage is not lost: build-linux and Build & Test both run every one of these
# in full, and what they test (agent governance semantics) is platform-neutral.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  test_failure_mode_coverage.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_failure_mode_coverage.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
    exit 0
fi

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
export FAKE_KEY_FMCOV="fake-key-fmcov"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

start_stub() {
    # Port is picked at random with no bind check, and the readiness wait used to
    # be a flat 5s. Both fail on a loaded CI runner: a collision (or a lingering
    # TIME_WAIT socket) leaves the stub dead, and python3 startup + bind can
    # exceed 5s. Either way every later assertion in the suite fails for a reason
    # that has nothing to do with what the test measures. Three consecutive CI
    # runs failed this way, each in a DIFFERENT stub-backed suite, none of them
    # reproducible locally.
    local _fx="$1" _dir="$2" _try _i _tries=3
    # POSIX only. Under MSYS2 this retry path is actively harmful, and it is the
    # failure path specifically — a stub that comes up promptly never enters it,
    # which is why Windows passed until the retry itself was added:
    #   - `wait` after a plain TERM can block forever. run-all-tests.sh already
    #     warns that native Windows binaries under MSYS2 ignore TERM and that
    #     plain `timeout` "can wait forever"; a process tree holding an
    #     unkillable child is also why the runner could not enforce its own
    #     step timeout or finalize the step.
    #   - fork/exec costs ~50-100ms there, and the loop spawns grep + kill +
    #     sleep per iteration, so 3x300 iterations is dominated by spawning
    #     rather than by the 30s of intended waiting.
    # Windows keeps the single 5s attempt that ran green for many jobs.
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) _tries=1 ;; esac
    [ -n "${WINDIR:-}" ] && _tries=1
    for _try in $(seq 1 $_tries); do
        STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
        : > "$_dir/stub.log"
        python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$_fx" "$_dir" > "$_dir/stub.log" 2>&1 &
        STUB_PID=$!
        if [ "$_tries" -eq 1 ]; then
            for _i in $(seq 1 50); do
                grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
                sleep 0.1
            done
            return 1
        fi
        # 30s, not 5s — a slow start is not a failed start. Sleep 0.5 keeps the
        # spawn count near the original despite the longer ceiling.
        for _i in $(seq 1 60); do
            grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
            kill -0 "$STUB_PID" 2>/dev/null || break
            sleep 0.5
        done
        # SIGKILL, and no unbounded wait: reaping is not worth a hang.
        kill -9 "$STUB_PID" 2>/dev/null; STUB_PID=""
    done
    echo "  start_stub: no READY after 3 port attempts — stub log tail:" >&2
    tail -3 "$_dir/stub.log" >&2 2>/dev/null
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# "single" = mandate_alignment only. "default" = the content-aware signals that
# ship enabled (S8-S22; response_degenerate S23 is default-off and stays off).
write_config() {  # $1=workdir $2=port $3=mode
    local on="false"
    [ "$3" = "default" ] && on="true"
    cat > "$1/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": false,
    "signals": {
      "circular_actions": $on, "repeated_failures": $on, "scope_creep": $on,
      "intent_contradictions": $on, "vocabulary_contraction": $on,
      "coherence_velocity": $on, "capability_underutilization": $on,
      "response_quality": $on, "thinking_collapse": $on,
      "semantic_stability": $on, "mandate_alignment": true,
      "context_growth": $on, "instruction_recall": $on, "plan_drift": $on,
      "entity_consistency": $on, "instruction_conflict": $on,
      "persona_fingerprint": $on, "tool_chain_integrity": $on,
      "claim_result_reconciliation": $on, "prompt_compliance": $on,
      "response_repetition": $on, "validation_outcome": $on,
      "response_degenerate": false
    },
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": {
    "enabled": true,
    "step_up_enabled": false,
    "output_admissibility": {
      "enabled": true, "threshold": 0.70,
      "action": "quarantine", "max_quarantine_streak": 3
    }
  },
  "agents": {
    "analyst": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$2",
      "api_key_env": "FAKE_KEY_FMCOV",
      "max_tokens": 300, "max_turns": 30,
      "system_prompt": "You are a financial analyst. Analyze quarterly revenue figures, profit margins and expense ratios. Report findings with concrete numbers drawn from the quarterly financial statements."
    }
  }
}
GOVEOF
    sign_govern "$1"
}

write_script() {
    cat > "$1/test.naab" <<'NAABEOF'
use agent
main {
    let h = agent.create("analyst")
    let i = 0
    while i < 8 {
        i = i + 1
        let r = agent.send(h, "Report the quarterly revenue figures and profit margins for this period.")
    }
    print("ALL_SENDS_COMPLETED")
}
NAABEOF
}

make_fixture() {  # $1=behaviour -> fixture JSON
    python3 - "$1" <<'PY'
import json, sys
b = sys.argv[1]
tangent = [
    "Arctic terns migrate across vast oceanic distances each season of their lives.",
    "Medieval cathedrals favoured flying buttresses to achieve dramatic vertical emphasis.",
    "Volcanic soil composition shapes grape cultivation throughout cool mountain valleys.",
    "Chess endgame theory distinguishes opposition from zugzwang when few pieces remain.",
]
if b == "compliant":
    out = [f"Quarterly revenue reached {4.0+i*0.1:.1f} million dollars with profit margins "
           f"at {18+i} percent and expense ratios near {12+i%3} percent."
           for i in range(8)]
elif b == "topic_abandon":
    out = [tangent[i % 4] + " " + tangent[(i+1) % 4] for i in range(8)]
elif b == "keyword_stuffed":
    # Off-topic substance wearing the mandate's vocabulary. The words the
    # signal looks for are all present; none of them are doing any work.
    out = [f"Quarterly revenue considerations aside, {tangent[i%4].lower()} "
           f"Profit margins and expense ratios notwithstanding, "
           f"{tangent[(i+1)%4].lower()} The financial statements are separate."
           for i in range(8)]
elif b == "repetition":
    out = ["Quarterly revenue reached 4.2 million dollars with profit margins at 18 percent."] * 8
elif b == "degenerate":
    out = ["Done.", "OK.", "Yes.", "Fine.", "Sure.", "Right.", "Noted.", "Agreed."]
elif b == "instruction_ignore":
    # Fluent, same corporate domain, mandate vocabulary largely absent, and it
    # answers a question nobody asked.
    out = [f"Our hiring pipeline added {3+i} engineers this cycle and onboarding "
           f"satisfaction scores improved across the regional offices."
           for i in range(8)]
else:
    out = ["unrecognised behaviour"] * 8
print(json.dumps({"responses": [{"content": c, "output_tokens": max(2, len(c)//4)} for c in out]}))
PY
}

measure() {  # $1=tag $2=behaviour $3=mode -> "<floor> <quar> <caught>"
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    make_fixture "$2" > "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "ERR 0 ERR"; return; }
    write_config "$d" "$STUB_PORT" "$3"
    write_script "$d"
    (cd "$d" && timeout 90s "$NAAB" test.naab >/dev/null 2>&1)
    stop_stub
    python3 - "$d/tele.jsonl" <<'PY'
import json,sys,os
p=sys.argv[1]
floor=1.0; quar=0
if os.path.exists(p):
    for ln in open(p):
        try: e=json.loads(ln)
        except: continue
        t=e.get("event_type")
        if t=="CDD_TURN" and e.get("analyzed")=="true":
            try: floor=min(floor,float(e.get("coherence",1.0)))
            except (TypeError,ValueError): pass
        elif t=="OUTPUT_INADMISSIBLE": quar+=1
print(f"{floor:.2f} {quar} {'CAUGHT' if quar>0 else '-'}")
PY
}

BEHAVIOURS=(compliant topic_abandon keyword_stuffed repetition degenerate instruction_ignore)

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Failure-mode coverage: one signal vs the aggregate          |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
printf "  %-20s %-24s %s\n" "BEHAVIOUR" "mandate_alignment only" "default signal set"
printf "  %-20s %-24s %s\n" "" "floor/quar/verdict" "floor/quar/verdict"
echo "  ------------------------------------------------------------------------"

MISSED_BY_SINGLE=""
FALSE_POS_SINGLE=0; FALSE_POS_DEFAULT=0
for b in "${BEHAVIOURS[@]}"; do
    read -r S_FLOOR S_QUAR S_V <<< "$(measure "s-$b" "$b" single)"
    read -r D_FLOOR D_QUAR D_V <<< "$(measure "d-$b" "$b" default)"
    printf "  %-20s %-24s %s\n" "$b" "$S_FLOOR/$S_QUAR/$S_V" "$D_FLOOR/$D_QUAR/$D_V"
    echo "$D_FLOOR $D_QUAR $D_V" > "$TEST_TMP/.d-$b.res"
    if [ "$b" = "compliant" ]; then
        FALSE_POS_SINGLE="${S_QUAR:-0}"; FALSE_POS_DEFAULT="${D_QUAR:-0}"
    else
        # Coverage the aggregate has and one signal does not.
        if [ "${D_QUAR:-0}" -gt 0 ] && [ "${S_QUAR:-0}" -eq 0 ]; then
            MISSED_BY_SINGLE="$MISSED_BY_SINGLE $b"
        fi
    fi
done
echo "  ------------------------------------------------------------------------"
if [ -n "$MISSED_BY_SINGLE" ]; then
    echo -e "  ${YELLOW}caught by the aggregate, missed by mandate_alignment alone:$MISSED_BY_SINGLE${NC}"
else
    echo -e "  ${CYAN}no failure mode is caught by the aggregate and missed by mandate_alignment${NC}"
fi
echo -e "  ${CYAN}false positives on compliant output — single: $FALSE_POS_SINGLE, default: $FALSE_POS_DEFAULT${NC}"
echo ""

# FM-01/FM-02 guard the SHIPPED configuration — the default signal set. They
# are the regression tests: if a future change makes the default set miss a
# failure mode, or start quarantining compliant output, they fail.
if [ "$FALSE_POS_DEFAULT" -eq 0 ]; then
    pass "FM-01" "Default signal set does not quarantine compliant output"
else
    fail "FM-01" "Default set quarantined compliant output ($FALSE_POS_DEFAULT)"
fi

UNCAUGHT_BY_DEFAULT=""
for b in "${BEHAVIOURS[@]}"; do
    [ "$b" = "compliant" ] && continue
    read -r _f q _v <<< "$(cat "$TEST_TMP/.d-$b.res" 2>/dev/null || echo "1.00 0 -")"
    [ "${q:-0}" -eq 0 ] && UNCAUGHT_BY_DEFAULT="$UNCAUGHT_BY_DEFAULT $b"
done
if [ -z "$UNCAUGHT_BY_DEFAULT" ]; then
    pass "FM-02" "Default set catches every tested failure mode ($((${#BEHAVIOURS[@]} - 1)) of $((${#BEHAVIOURS[@]} - 1)))"
else
    fail "FM-02" "Default set missed failure modes:$UNCAUGHT_BY_DEFAULT"
fi

# FM-03 records the single-signal comparison. It is evidence, not a defect:
# mandate_alignment alone is more precise (zero false positives even on
# phrasing-varied compliant output, where the default set produced two in
# test_adversarial_detection) and less complete. Both halves matter, so the
# gap is asserted to EXIST rather than asserted away — if a future change
# closed it, the tradeoff this file documents would no longer hold and the
# recommendation built on it should be revisited.
if [ -n "$MISSED_BY_SINGLE" ]; then
    pass "FM-03" "mandate_alignment alone is incomplete — misses:$MISSED_BY_SINGLE (single-signal gating would delete this coverage)"
else
    fail "FM-03" "mandate_alignment alone now covers every tested mode" \
         "the precision/completeness tradeoff this file documents has changed; revisit"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
