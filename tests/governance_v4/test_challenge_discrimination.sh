#!/usr/bin/env bash
# ============================================================
# test_challenge_discrimination.sh — does the step-up challenge discriminate?
#
# Nine groups in test_challenge_fail_path cover challenge MECHANICS: fail
# paths, streaks, ratchets, scoring modes, infrastructure skips. None of them
# ask the question the challenge exists to answer — can an agent that still
# holds its context be told apart from one that has lost it?
#
# That is the same gap test_adversarial_detection closed for CDD. Until it is
# measured, any change to challenge scoring is being made blind, and a change
# that makes challenges easier to pass is indistinguishable from one that
# makes them easier to FAKE.
#
# The step-up challenge is a liveness probe gating lease renewal — a Kerberos
# TGT analog. Its job is to be hard to fake. So the test holds answer QUALITY
# fixed and varies only what should be irrelevant:
#
#   A  small context, answer names every core attribute      -> must PASS
#   B  small context, answer is off-topic                    -> must FAIL
#   C  LARGE context, answer names every core attribute      -> ?
#
# A and B together establish the probe carries information at all. C is the
# open question. The entity's context is built by extractEntityContext as
# "every other keyword in the response where the entity appeared", so a
# verbose agent gives its own entities enormous contexts. C's agent names
# every defining attribute of the entity — the same answer that passes in A —
# while its earlier responses happened to also contain fifty incidental terms.
#
# C fails: the probe is sensitive to response verbosity, not only to context
# retention. Live data bounds how much that matters — see CD-03's comment. The
# short version is that entity challenges score kr=1.000 at denominators of
# 106-138 in real runs, so a large expected set is NOT unpassable; a concise
# answer against one is simply disadvantaged.
#
# max_challenge_failures is high throughout: this test observes verdicts, it
# does not want a kill truncating the run.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/chaldisc-$$"

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
        echo "  test_challenge_discrimination.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_challenge_discrimination.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
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
export FAKE_KEY_CHALDISC="fake-key-chaldisc"

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

# entity_consistency both escalates (disjoint contexts fire it every turn) and
# supplies the challenge context. instruction_recall and validation_outcome are
# off so the challenge falls through priorities 0-3 to the entity type.
# behavioral_sequences must be enabled or the circuit breaker never escalates.
write_config() {  # $1=workdir $2=port
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
      "circular_actions": false, "repeated_failures": false, "scope_creep": false,
      "intent_contradictions": false, "vocabulary_contraction": false,
      "coherence_velocity": false, "capability_underutilization": false,
      "response_quality": false, "thinking_collapse": false,
      "semantic_stability": false, "mandate_alignment": false,
      "context_growth": false, "instruction_recall": false, "plan_drift": false,
      "entity_consistency": true, "instruction_conflict": false,
      "persona_fingerprint": false, "tool_chain_integrity": false,
      "claim_result_reconciliation": false, "prompt_compliance": false,
      "response_repetition": false, "validation_outcome": false,
      "response_degenerate": false
    },
    "reality_checkpoint": {
      "enabled": false, "pressure_threshold": 0.5, "signal_density_divisor": 1,
      "weights": {
        "coherence_proximity": 0, "risk_score_proximity": 0,
        "signal_density": 1.0, "conversation_depth": 0,
        "bsd_partial_progress": 0, "pipeline_inherited": 0,
        "coherence_acceleration": 0, "codegen_pressure": 0,
        "bsd_eviction_pressure": 0, "semantic_deviation": 0
      }
    }
  },
  "circuit_breaker": {
    "enabled": true, "elevated_threshold": 0.5, "elevated_sustained": 1,
    "deescalate_sustained": 9,
    "step_up_enabled": true, "step_up_contextual": true,
    "step_up_at_level": "elevated",
    "step_up_cooldown_turns": 0,
    "step_up_contextual_threshold": 0.15,
    "max_challenge_failures": 99
  },
  "agents": {
    "worker": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$2",
      "api_key_env": "FAKE_KEY_CHALDISC",
      "max_tokens": 400, "max_turns": 40,
      "system_prompt": "You maintain a data processing pipeline. Report on its stages and behaviour."
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
    let h = agent.create("worker")
    let i = 0
    while i < 6 {
        i = i + 1
        try {
            let r = agent.send(h, "Describe the pipeline and its current behaviour.")
        } catch (e) {
            print("SEND_BLOCKED")
        }
    }
    print("DONE")
}
NAABEOF
}

# $1 = context width ("small" | "large"), $2 = answer ("good" | "bad")
#
# The entity "pipeline" appears in every response with three CORE attributes
# that always recur — what the entity stably means. In the large variant each
# response additionally carries fifty incidental terms that never repeat,
# which extractEntityContext folds into the entity's context anyway.
#
# The "good" answer names the entity and all three core attributes: the same
# sentence in both variants. The "bad" answer is fluent and entirely unrelated.
make_fixture() {
    python3 - "$1" "$2" <<'PY'
import json, sys
width, answer = sys.argv[1], sys.argv[2]
core = "staging buffering checkpointing"
# 60 distinct digit-free tokens: ten per turn, disjoint between turns.
WORDS = [a + b for a in ("quar","zeph","marl","brix","toln","vesk","dorn","plin","grum","syln")
               for b in ("adon","evik","ombu","irna","ulax","ythe")]

# TWO context-building responses, then the answer.
#
# The challenge fires as soon as escalation completes, not at some turn the
# fixture chooses. S15 needs a prior sighting, so it first fires on response 2,
# escalation lands, and send 3 draws the challenge — consuming response INDEX 2.
# A first attempt put the good/bad answer at index 6 and it was never reached:
# both scenarios scored an identical 0.333, because both were grading the same
# context-building response. Placing the answer at index 2 (and letting the stub
# repeat it thereafter) makes the measured verdict the answer's, whichever send
# the challenge lands on.
resp = []
for i in range(2):
    # S15 fires when a sighting's context resembles none of the recent ones, so
    # BOTH widths need turn-to-turn variation or escalation never happens.
    #
    # The varying terms carry no digits deliberately. A first attempt used
    # rotating00item00-style names, and the keyword extractor ALSO emits
    # component words at letter-digit boundaries ("rotating", "item") — shared
    # across every turn, which lifted overlap to ~0.26, just over
    # entity_context_min_overlap (0.25), and the small scenario never escalated.
    varying = " ".join(WORDS[i * 10:(i + 1) * 10])
    filler = ""
    if width == "large":
        # Incidental co-occurrence: never repeated, contributes nothing to what
        # the entity MEANS, and extractEntityContext folds it in regardless.
        filler = " " + " ".join(f"incidental{i:02d}term{j:02d}" for j in range(50))
    resp.append(f"The pipeline performs {core} alongside {varying}.{filler}")

good = "The pipeline performs staging buffering and checkpointing."
bad  = "Arctic terns migrate across vast oceanic distances every season."
resp.append(good if answer == "good" else bad)
print(json.dumps({"responses": [{"content": c, "output_tokens": max(8, len(c)//4)} for c in resp]}))
PY
}

# echoes "<verdict> <kr> <denominator> <sets>"
measure() {  # $1=tag $2=width $3=answer
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    make_fixture "$2" "$3" > "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL 0 0 0"; return; }
    write_config "$d" "$STUB_PORT"
    write_script "$d"
    (cd "$d" && timeout 90s "$NAAB" test.naab >/dev/null 2>&1)
    stop_stub
    python3 - "$d/tele.jsonl" <<'PY'
import json,sys,os
p=sys.argv[1]
verdict="NONE"; kr="-"; den="-"; sets="-"
if os.path.exists(p):
    for ln in open(p):
        try: e=json.loads(ln)
        except: continue
        t=e.get("event_type")
        if t in ("AGENT_CHALLENGE_PASS","AGENT_CHALLENGE_FAIL") and e.get("challenge_type")=="entity":
            verdict = "PASS" if t.endswith("PASS") else "FAIL"
            kr=e.get("keyword_ratio","-"); den=e.get("expected_keyword_count","-")
            sets=e.get("expected_set_count","-")
            break   # first entity challenge is the one under test
print(f"{verdict} {kr} {den} {sets}")
PY
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Challenge discrimination: context held vs context lost      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

read -r A_V A_KR A_DEN A_SETS <<< "$(measure a small good)"
read -r B_V B_KR B_DEN B_SETS <<< "$(measure b small bad)"
read -r C_V C_KR C_DEN C_SETS <<< "$(measure c large good)"

printf "  %-34s %-7s %-10s %-10s %s\n" "SCENARIO" "VERDICT" "kr" "denom" "sets"
printf "  %-34s %-7s %-10s %-10s %s\n" "A small context, good answer" "$A_V" "$A_KR" "$A_DEN" "$A_SETS"
printf "  %-34s %-7s %-10s %-10s %s\n" "B small context, off-topic answer" "$B_V" "$B_KR" "$B_DEN" "$B_SETS"
printf "  %-34s %-7s %-10s %-10s %s\n" "C LARGE context, SAME good answer" "$C_V" "$C_KR" "$C_DEN" "$C_SETS"
echo ""

if [ "$A_V" = "NONE" ] || [ "$B_V" = "NONE" ]; then
    skip "CD-01" "No entity challenge fired — staging did not reach step-up"
    skip "CD-02" "No entity challenge fired"
    skip "CD-03" "No entity challenge fired"
else
    if [ "$A_V" = "PASS" ]; then
        pass "CD-01" "Context-holding agent clears the probe (kr=$A_KR of $A_DEN)"
    else
        fail "CD-01" "Context-holding agent FAILED the probe (kr=$A_KR of $A_DEN)" \
             "an agent naming every core attribute cannot pass — the probe rejects correct answers"
    fi

    if [ "$B_V" = "FAIL" ]; then
        pass "CD-02" "Context-lost agent is caught (kr=$B_KR of $B_DEN)"
    else
        fail "CD-02" "Off-topic answer PASSED the probe (kr=$B_KR)" \
             "the challenge carries no information — it cannot be used as a gate"
    fi

    # CD-03 records a real property, and its significance is BOUNDED by live
    # evidence — read both halves before acting on it.
    #
    # The property: A and C send the identical answer and find the identical
    # keywords. Only C's EARLIER responses were more verbose, which
    # extractEntityContext folds into the entity's context wholesale, so the
    # denominator moves 15 -> 67 and the verdict flips. Concise correct answers
    # are penalised relative to verbose ones.
    #
    # What it is NOT: evidence that large denominators are unpassable. That
    # inference was drawn from run 17's four challenge failures and refuted by
    # run 17's sixty-five passes. Across 46 live challenges, entity challenges
    # scored kr=1.000 at denominators of 106, 120, 137 and 138, and the
    # instruction type produced BOTH a pass (kr=0.239) and a fail (kr=0.122) at
    # the same denominator of 213. Every live failure was marginal against the
    # 0.15 threshold (0.082-0.130), none structural. Real agents answer at
    # length, so the ratio stays workable.
    #
    # So: do not "fix" the expected-set construction on the strength of this
    # assertion alone. It measures a sensitivity, not a broken probe. If a
    # future change makes the verdict width-stable that is an improvement and
    # this assertion should be inverted — but the case for making that change
    # has to come from somewhere other than here.
    if [ "$A_V" = "PASS" ] && [ "$C_V" = "FAIL" ]; then
        pass "CD-03" "Width sensitivity recorded: same answer, verdict flips on context width" \
             ""
        echo -e "       ${YELLOW}A=$A_V kr=$A_KR of $A_DEN | C=$C_V kr=$C_KR of $C_DEN${NC}"
        echo -e "       ${YELLOW}identical answer, identical keywords found; only the denominator moved${NC}"
        echo -e "       ${YELLOW}live data bounds this: entity scored kr=1.000 at denom 106-138,${NC}"
        echo -e "       ${YELLOW}and denom=213 produced both a pass and a fail. Not unpassable.${NC}"
    elif [ "$C_V" = "$A_V" ]; then
        fail "CD-03" "Verdict is now width-stable — the defect appears FIXED" \
             "A=$A_V kr=$A_KR/$A_DEN, C=$C_V kr=$C_KR/$C_DEN. Invert this assertion: it exists to record a defect that no longer reproduces."
    else
        fail "CD-03" "Unexpected verdict combination" \
             "A=$A_V kr=$A_KR/$A_DEN | C=$C_V kr=$C_KR/$C_DEN"
    fi
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
