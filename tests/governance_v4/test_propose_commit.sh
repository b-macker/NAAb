#!/usr/bin/env bash
# ============================================================
# test_propose_commit.sh — agent.propose / agent.commit / select_admissible
#
# Group A: propose disabled by default (fail-closed)
# Group B: propose returns N candidates with admissibility sections
# Group C: select_admissible + commit appends exactly one turn
# Group D: replay / forged nonce / stale proposal rejected
# Group E: telemetry events emitted
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/proposecommit-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_PROPOSE_TEST="fake-key-propose-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

start_stub() {
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do
        grep -q READY "$2/stub.log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

write_govern() {  # $1=workdir $2=port
    cat > "$1/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "agents": {
        "proposer": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100,
            "max_turns": 20,
            "propose_candidates_max": 3
        },
        "plain": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100,
            "max_turns": 20
        }
    }
}
GOVEOF
    sign_govern "$1"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|        agent.propose / agent.commit / select_admissible       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/work"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "candidate one about summarizing quarterly revenue data", "output_tokens": 40},
  {"content": "candidate two about summarizing quarterly revenue trends", "output_tokens": 45},
  {"content": "candidate three about summarizing revenue outlook", "output_tokens": 42},
  {"content": "post commit follow up response", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "00" "stub failed to start"; exit 1; }
write_govern "$WDIR" "$STUB_PORT"

# ============================================================
# Group A: fail-closed default
# ============================================================
echo -e "${CYAN}--- Group A: propose disabled by default ---${NC}"

cat > "$WDIR/test_a.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("plain")
    try {
        let p = agent.propose(h, "hello", 2)
        print("PROPOSE_ALLOWED")
    } catch (e) {
        print("PROPOSE_DENIED")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test_a.naab 2>&1) || true
if echo "$OUTPUT" | grep -q "PROPOSE_DENIED"; then
    pass "A-01" "agent.propose fail-closed without propose_candidates_max"
else
    fail "A-01" "propose worked without being enabled" "$(echo "$OUTPUT" | head -3)"
fi

# ============================================================
# Group B + C: propose N candidates, select, commit
# ============================================================
echo -e "${CYAN}--- Group B/C: propose, select, commit ---${NC}"

cat > "$WDIR/test_bc.naab" << 'NAABEOF'
use agent
use orchestra

main {
    let h = agent.create("proposer")
    let p = agent.propose(h, "summarize the revenue data", 3)
    print("TOTAL=" + string(p.get("total")))
    print("ADMISSIBLE=" + string(p.get("admissible_count")))
    let cands = p.get("candidates")
    print("CANDS=" + string(cands.length()))

    // No history or turns committed by propose
    let msgs_before = agent.messages(h)
    print("MSGS_BEFORE=" + string(msgs_before.length()))

    let pick = orchestra.select_admissible(p)
    print("SELECTED_INDEX=" + string(pick.get("selected_index")))

    let selected = pick.get("selected")
    let r = agent.commit(h, selected)
    print("COMMIT_CONTENT=" + string(r.get("content")))

    let msgs_after = agent.messages(h)
    print("MSGS_AFTER=" + string(msgs_after.length()))

    // Replay: committing the same proposal again must fail
    try {
        let r2 = agent.commit(h, selected)
        print("REPLAY_ALLOWED")
    } catch (e) {
        print("REPLAY_DENIED")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test_bc.naab 2>&1) || true

if echo "$OUTPUT" | grep -q "TOTAL=3" && echo "$OUTPUT" | grep -q "CANDS=3"; then
    pass "B-01" "propose returned 3 candidates"
else
    fail "B-01" "candidate count wrong" "$(echo "$OUTPUT" | grep -E 'TOTAL=|CANDS=' | head -2)"
fi
if echo "$OUTPUT" | grep -q "ADMISSIBLE=3"; then
    pass "B-02" "all clean candidates admissible (OA disabled)"
else
    fail "B-02" "admissible count wrong" "$(echo "$OUTPUT" | grep ADMISSIBLE=)"
fi
if echo "$OUTPUT" | grep -q "MSGS_BEFORE=0"; then
    pass "B-03" "propose committed no conversation state"
else
    fail "B-03" "propose leaked into history" "$(echo "$OUTPUT" | grep MSGS_BEFORE)"
fi
if echo "$OUTPUT" | grep -q "SELECTED_INDEX=[0-9]"; then
    pass "C-01" "select_admissible picked a candidate"
else
    fail "C-01" "selection failed" "$(echo "$OUTPUT" | grep SELECTED_INDEX)"
fi
if echo "$OUTPUT" | grep -q "COMMIT_CONTENT=candidate"; then
    pass "C-02" "commit returned the selected candidate content"
else
    fail "C-02" "commit content wrong" "$(echo "$OUTPUT" | grep COMMIT_CONTENT | head -1)"
fi
if echo "$OUTPUT" | grep -q "MSGS_AFTER=2"; then
    pass "C-03" "commit appended exactly one user/assistant pair"
else
    fail "C-03" "history length wrong after commit" "$(echo "$OUTPUT" | grep MSGS_AFTER)"
fi
if echo "$OUTPUT" | grep -q "REPLAY_DENIED"; then
    pass "D-01" "replayed proposal rejected (single use)"
else
    fail "D-01" "replay was allowed" "$(echo "$OUTPUT" | grep REPLAY | head -1)"
fi

# ============================================================
# Group D: forged nonce + stale proposals
# ============================================================
echo -e "${CYAN}--- Group D: forged / stale proposals ---${NC}"

cat > "$WDIR/test_d.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("proposer")
    let p = agent.propose(h, "summarize the revenue data", 2)
    let cands = p.get("candidates")
    let c0 = cands[0]

    // Forge the nonce
    let forged = {
        "__proposal": "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
        "content": string(c0.get("content"))
    }
    try {
        let r = agent.commit(h, forged)
        print("FORGE_ALLOWED")
    } catch (e) {
        print("FORGE_DENIED")
    }

    // Stale: a send() invalidates outstanding proposals
    let r1 = agent.send(h, "regular message consumes state")
    try {
        let r2 = agent.commit(h, c0)
        print("STALE_ALLOWED")
    } catch (e) {
        print("STALE_DENIED")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test_d.naab 2>&1) || true

if echo "$OUTPUT" | grep -q "FORGE_DENIED"; then
    pass "D-02" "forged proposal nonce rejected"
else
    fail "D-02" "forged nonce accepted" "$(echo "$OUTPUT" | head -5)"
fi
if echo "$OUTPUT" | grep -q "STALE_DENIED"; then
    pass "D-03" "stale proposal (superseded by send) rejected"
else
    fail "D-03" "stale proposal accepted" "$(echo "$OUTPUT" | grep STALE | head -1)"
fi

# ============================================================
# Group E: telemetry
# ============================================================
echo -e "${CYAN}--- Group E: telemetry ---${NC}"

if grep -q "AGENT_PROPOSE" "$WDIR/telemetry.jsonl" 2>/dev/null; then
    pass "E-01" "AGENT_PROPOSE telemetry emitted"
else
    fail "E-01" "no AGENT_PROPOSE telemetry"
fi
if grep -q "AGENT_PROPOSAL_COMMIT" "$WDIR/telemetry.jsonl" 2>/dev/null; then
    pass "E-02" "AGENT_PROPOSAL_COMMIT telemetry emitted"
else
    fail "E-02" "no AGENT_PROPOSAL_COMMIT telemetry"
fi

stop_stub

# ============================================================
echo ""
echo -e "${CYAN}==============================================${NC}"
echo -e "  Total: $TOTAL  ${GREEN}Pass: $PASS_COUNT${NC}  ${RED}Fail: $FAIL_COUNT${NC}  ${YELLOW}Skip: $SKIP_COUNT${NC}"
[ -n "$FAILURES" ] && echo -e "${RED}Failures:$FAILURES${NC}"
echo -e "${CYAN}==============================================${NC}"
[ $FAIL_COUNT -eq 0 ]
