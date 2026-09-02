#!/usr/bin/env bash
# ============================================================
# test_agent_event_attribution.sh — events belong to the agent that raised them
#
# emitEvent() stamped ev.turn and ev.agent_handle from current_agent_turn_ /
# current_agent_handle_, two PROCESS-GLOBAL atomics written by setAgentContext().
# agent.batch() and fan_out() run several sends concurrently on pool threads, so
# an event was attributed to whichever agent had most recently set the context
# rather than the one that raised it.
#
# checkContextDrift() already filters the turn bucket to "this handle's events"
# (governance_engine.cpp) -- correct code reading a corrupted field. Measured
# with six agents each making exactly ONE call per round:
#
#     handle=1 events=4 fp='AR:65bdf062AR:fd45e466ASAS'   two siblings' responses
#     handle=2 events=0 fp=''                             none of its own
#     handle=3 events=1 fp='AS'                           its send, no response
#
# Degenerate buckets ('AS', '') repeat across turns and fire S1 circular_actions
# on agents that repeated nothing -- a different pair each run, because it
# follows thread scheduling. The stamp is now thread-local.
#
#   AE-01  6 concurrent agents, all DISTINCT responses => no circular fires
#   AE-02  control — all IDENTICAL responses => circular fires for ALL of them
#   AE-03  control — coherence is uniform across peers doing the same thing
#   AE-04  ONE parrot among six working agents: only the parrot is charged
#
# AE-02 is the load-bearing arm, for two reasons. It rules out the likeliest bad
# "fix" -- an S1 that no longer fires at all, which AE-01 cannot distinguish from
# a correct one. And it is the RELIABLE detector: the defect is timing-dependent,
# so AE-01 only catches it sometimes. Measured over three runs against the
# pre-fix build:
#
#     run 1   AE-01 FAIL (1 spurious)   AE-02 FAIL (3 of 6 detected)
#     run 2   AE-01 pass                AE-02 FAIL (0 of 6 detected)
#     run 3   AE-01 FAIL (1 spurious)   AE-02 FAIL (2 of 6 detected)
#
# That also corrects the framing this investigation started with. The louder
# symptom was a false POSITIVE -- circular firing on agents that repeated
# nothing. The more consistent symptom is a false NEGATIVE: six agents emitting
# byte-identical responses went entirely undetected, because each one's bucket
# held its siblings' distinct answers instead of its own duplicate. Do not drop
# AE-02 as redundant; AE-01 alone passes under the bug one run in three.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_agent_event_attribution.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/aeattr-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_AEATTR="fake-key-agent-event-attribution"
sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Agent events belong to the agent that raised them            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# $1 = arm dir, $2 = "distinct" | "identical"
run_arm() {
    local arm="$1" mode="$2"
    local W="$TEST_TMP/$arm"; mkdir -p "$W"
    if [ "$mode" = "identical" ]; then
        cat > "$W/fixture.json" << 'EOF'
{"responses": [
  {"content": "identical repeated answer about ipv4 octets and dotted decimal notation", "output_tokens": 30}
]}
EOF
    else
        python3 - "$W/fixture.json" << 'PYEOF'
import json, sys
# 12 responses, all different from each other AND from the same agent's own
# previous turn, so nothing legitimately circular exists in this arm.
r = [{"content": "response %d covering ipv4 %s validation with distinct wording %s"
        % (i, ["octets","masks","ranges","parsing","edges","limits"][i % 6], "y" * i),
      "output_tokens": 30 + i} for i in range(1, 13)]
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
    fi
    start_stub "$W/fixture.json" "$W" || return 1
    python3 - "$W/govern.json" "$STUB_PORT" << 'PYEOF'
import json, sys
port = sys.argv[2]
names = ["coordinator", "researcher", "architect", "builder", "critic", "optimizer"]
agents = {n: {"provider": "gemini", "model": "stub-model",
              "api_base": "http://127.0.0.1:%s" % port,
              "api_key_env": "FAKE_KEY_AEATTR",
              "max_tokens": 100, "max_turns": 20} for n in names}
json.dump({
    "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    "telemetry": {"enabled": True, "output_file": "telemetry.jsonl"},
    "behavioral_sequences": {"enabled": True},
    "context_drift": {"enabled": True, "level": "advisory", "check_interval_turns": 1},
    "agent_dispatch": {"max_concurrent": 3, "pool_size": 3},
    "agents": agents,
}, open(sys.argv[1], "w"), indent=2)
PYEOF
    sign_govern "$W"
    cat > "$W/test.naab" << 'NAABEOF'
use agent
main {
    let hs = [agent.create("coordinator"), agent.create("researcher"),
              agent.create("architect"),   agent.create("builder"),
              agent.create("critic"),      agent.create("optimizer")]
    agent.batch(hs, ["a1", "b1", "c1", "d1", "e1", "f1"])
    agent.batch(hs, ["a2", "b2", "c2", "d2", "e2", "f2"])
    print("DONE")
}
NAABEOF
    ARM_OUT=$(cd "$W" && timeout 120s "$NAAB" test.naab 2>&1) || true
    stop_stub
    ARM_CIRC=$(grep '"CDD_TURN"' "$W/telemetry.jsonl" 2>/dev/null | grep -c 'circular=' || true)
    [ -z "$ARM_CIRC" ] && ARM_CIRC=0
    ARM_TURNS=$(grep -c '"CDD_TURN"' "$W/telemetry.jsonl" 2>/dev/null || true)
    [ -z "$ARM_TURNS" ] && ARM_TURNS=0
    ARM_COH=$(grep '"CDD_TURN"' "$W/telemetry.jsonl" 2>/dev/null | grep -o '"coherence":"[^"]*"' | sort -u | tr '\n' ' ')
    return 0
}

# ── AE-01: distinct responses must not fire circular ──
if run_arm "distinct" "distinct"; then
    if [ "$ARM_TURNS" -lt 12 ]; then
        fail "AE-01" "expected 12 CDD_TURN events, saw $ARM_TURNS" "$(echo "$ARM_OUT" | tail -2)"
    elif [ "$ARM_CIRC" = "0" ]; then
        pass "AE-01" "6 concurrent agents, 12 distinct responses, no circular firing"
    else
        fail "AE-01" "circular fired $ARM_CIRC times on wholly distinct responses" \
             "events are still being attributed across handles"
    fi
else
    skip "AE-01" "stub failed to start"
fi

# ── AE-02: control — genuine repetition must still fire ──
if run_arm "identical" "identical"; then
    if [ "$ARM_CIRC" -ge 6 ]; then
        pass "AE-02" "control: identical responses fire circular for all 6 agents ($ARM_CIRC)"
    else
        fail "AE-02" "identical responses fired circular only $ARM_CIRC times — S1 may be disabled" \
             "without this, AE-01 passes for a signal that never fires at all"
    fi
    # AE-03: peers doing the identical thing must be scored identically. Under
    # the bug, coherence split arbitrarily by thread scheduling.
    UNIQ_COH=$(echo "$ARM_COH" | tr ' ' '\n' | grep -c . || true)
    if [ "$UNIQ_COH" -le 2 ]; then
        pass "AE-03" "control: peers doing the same thing score the same ($ARM_COH)"
    else
        fail "AE-03" "peers doing identical work scored $UNIQ_COH different coherences" "$ARM_COH"
    fi
else
    skip "AE-02" "stub failed to start"; skip "AE-03" "stub failed to start"
fi

# ── AE-04: discrimination, not uniformity ──
# AE-01/02 move every agent together: all distinct, or all identical. That
# cannot tell a discriminating engine from one that fires uniformly on whatever
# is in front of it. This arm is the case users actually have -- six agents
# doing real work and one that repeats itself -- and it requires the engine to
# charge exactly one of seven.
#
# It is also the arm that caught a live run reporting the OPPOSITE of both
# halves at once: the repeating agent scored a clean 1.0 while two agents
# producing distinct 3000-char answers were charged circular. That pairing is
# the attribution defect's signature, since a corrupted bucket both invents
# repetition where there is none and hides it where there is.
#
# Because it asserts BOTH directions -- the parrot is charged AND the six are
# not -- it catches the defect whichever way scheduling throws it. Measured
# over three runs against the pre-fix build it failed 3/3, where AE-01 (which
# can only see the false-positive half) failed 2/3:
#
#     run 1  parrot charged, but 2 working agents charged too
#     run 2  parrot charged, but 3 working agents charged too
#     run 3  parrot charged, but 3 working agents charged too
#
# The live run that prompted this arm failed the other assertion instead, with
# the parrot uncharged. Keep both halves.
WDIR="$TEST_TMP/parrot"; mkdir -p "$WDIR"
python3 - "$WDIR/fixture.json" << 'PYEOF'
import json, sys
# The parrot is routed by a token planted in its system_prompt, so it gets the
# same answer every turn while the others draw from the distinct pool.
parrot = {"responses": [{"content": "I am the parrot and this is my identical "
                                    "unchanging reply every single turn",
                         "output_tokens": 20}]}
others = [{"content": "distinct answer %d about ipv4 %s validation, different wording %s"
             % (i, ["octets","masks","ranges","parsing","edges","limits"][i % 6], "z" * i),
           "output_tokens": 30 + i} for i in range(1, 13)]
json.dump({"routes": {"PARROT-TOKEN-XYZ": parrot}, "responses": others},
          open(sys.argv[1], "w"))
PYEOF
if start_stub "$WDIR/fixture.json" "$WDIR"; then
    python3 - "$WDIR/govern.json" "$STUB_PORT" << 'PYEOF'
import json, sys
port = sys.argv[2]
base = {"provider": "gemini", "model": "stub-model",
        "api_base": "http://127.0.0.1:%s" % port,
        "api_key_env": "FAKE_KEY_AEATTR", "max_tokens": 100, "max_turns": 20}
agents = {n: dict(base) for n in
          ["coordinator", "researcher", "architect", "builder", "critic", "optimizer"]}
agents["parrot"] = dict(base,
    system_prompt="PARROT-TOKEN-XYZ reply with the identical sentence every turn")
json.dump({
    "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    "telemetry": {"enabled": True, "output_file": "telemetry.jsonl"},
    "behavioral_sequences": {"enabled": True},
    "context_drift": {"enabled": True, "level": "advisory", "check_interval_turns": 1},
    "agent_dispatch": {"max_concurrent": 3, "pool_size": 3},
    "agents": agents,
}, open(sys.argv[1], "w"), indent=2)
PYEOF
    sign_govern "$WDIR"
    cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let hs = [agent.create("coordinator"), agent.create("researcher"),
              agent.create("architect"),   agent.create("builder"),
              agent.create("critic"),      agent.create("optimizer"),
              agent.create("parrot")]
    agent.batch(hs, ["a1", "b1", "c1", "d1", "e1", "f1", "g1"])
    agent.batch(hs, ["a2", "b2", "c2", "d2", "e2", "f2", "g2"])
    print("DONE")
}
NAABEOF
    P_OUT=$(cd "$WDIR" && timeout 120s "$NAAB" test.naab 2>&1) || true
    stop_stub
    # Turn-2 CDD_TURN for the parrot, and for everyone else.
    P_LINE=$(grep '"CDD_TURN"' "$WDIR/telemetry.jsonl" 2>/dev/null \
             | grep '"config_name":"parrot"' | grep '"turn":"2"')
    OTHER_PEN=$(grep '"CDD_TURN"' "$WDIR/telemetry.jsonl" 2>/dev/null \
             | grep -v '"config_name":"parrot"' | grep -c 'circular=\|response_repetition=' || true)
    [ -z "$OTHER_PEN" ] && OTHER_PEN=0
    P_FIRED=$(echo "$P_LINE" | grep -c 'response_repetition=' || true)
    [ -z "$P_FIRED" ] && P_FIRED=0

    if [ -z "$P_LINE" ]; then
        fail "AE-04" "no turn-2 CDD_TURN for the parrot" "$(echo "$P_OUT" | tail -2)"
    elif [ "$P_FIRED" = "0" ]; then
        fail "AE-04" "the parrot repeated itself verbatim and was NOT charged" \
             "$(echo "$P_LINE" | grep -o '\"coherence\":\"[^\"]*\"\|\"signals_detail\":\"[^\"]*\"' | tr '\n' ' ')"
    elif [ "$OTHER_PEN" != "0" ]; then
        fail "AE-04" "parrot charged, but $OTHER_PEN working agents were charged too" \
             "discrimination failed — the engine is firing on siblings' events"
    else
        pass "AE-04" "only the parrot is charged; six working agents untouched"
    fi
else
    skip "AE-04" "stub failed to start"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}| Results: ${GREEN}${PASS_COUNT} passed${NC}, ${RED}${FAIL_COUNT} failed${NC}, ${YELLOW}${SKIP_COUNT} skipped${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}${FAILURES}"; exit 1; fi
exit 0
