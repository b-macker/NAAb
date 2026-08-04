#!/usr/bin/env bash
# ============================================================
# test_instruction_recall_windowing.sh — Window-aware instruction_recall (S13)
#
# When context windowing drops older messages, the agent never sees those
# instructions, so recall must be measured only against keywords from the
# turns actually sent. Uses the local agent stub (tests/helpers/agent_stub.py)
# via the per-agent api_base override.
#
# Group W1: windowed agent recalling in-window instructions never fires S13
#           (pre-fix: fires once the full accumulated set dilutes recall < 0.10)
# Group W2: windowed agent ignoring even in-window instructions still fires S13
# Group W3: non-windowed agent — full-history recall behavior unchanged
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_instruction_recall_windowing.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/irwindow-$$"

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
export FAKE_KEY_IRW_TEST="fake-key-instruction-recall-window-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

start_stub() {  # $1=fixture $2=workdir
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

# Last cumulative instruction_recall_count seen in SEMANTIC_TURN telemetry
last_recall_count() {  # $1=telemetry file
    grep -o '"instruction_recall_count": *"[0-9]*"' "$1" 2>/dev/null \
        | tail -1 | grep -o '[0-9]*' | tail -1
}

# 14 distinct instruction keywords, one per turn. Pure lowercase alpha so the
# code-aware extractor keeps each word whole (no digit/camelCase splitting).
WORDS=(gorillamarble pelicanquartz walrusvelvet otterbanjo heronmosaic
       lynxpaprika badgercitrus falconnutmeg ibexlantern koalatrellis
       marmotjigsaw ospreyvellum pumacobalt tapirsaffron)

words_naab_list() {
    local out=""
    for w in "${WORDS[@]}"; do out="${out}${out:+, }\"$w\""; done
    echo "$out"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Instruction Recall (S13) — context windowing awareness     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group W1: windowed agent, responses recall in-window instructions.
# 14 turns of single-word prompts; response N echoes only word N.
# Windowed recall = 1/3 (window 4 = 2 history turns + current prompt),
# always >= instruction_recall_min (0.10) -> S13 must never fire.
# Pre-fix, the denominator grows to 14 and recall 1/14 < 0.10 fires.
# ============================================================
echo -e "${CYAN}--- Group W1: in-window recall does not fire S13 ---${NC}"

WDIR="$TEST_TMP/w1"; mkdir -p "$WDIR"
{
    echo '{"responses": ['
    first=1
    for w in "${WORDS[@]}"; do
        [ $first -eq 0 ] && echo ','
        first=0
        printf '  {"content": "detailed analysis of %s follows shortly", "output_tokens": 20}' "$w"
    done
    echo ''
    echo ']}'
} > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "W1-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_IRW_TEST",
            "max_tokens": 100,
            "max_turns": 20,
            "context_window": 4,
            "context_strategy": "recent"
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << NAABEOF
use agent

main {
    let words = [$(words_naab_list)]
    let h = agent.create("test_agent")
    let sent = 0
    for i in 0..14 {
        let r = agent.send(h, words[i])
        sent = sent + 1
    }
    print("SENT=" + string(sent))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "SENT=14"; then
    pass "W1-01" "14 windowed sends complete"
else
    fail "W1-01" "Windowed sends did not complete" "$(echo "$OUTPUT" | head -3)"
fi

RECALL=$(last_recall_count "$WDIR/telemetry.jsonl")
if [ "$RECALL" = "0" ]; then
    pass "W1-02" "S13 never fires when in-window instructions are recalled (count=0)"
else
    fail "W1-02" "S13 fired despite in-window recall" "instruction_recall_count=$RECALL"
fi

# ============================================================
# Group W2: windowed agent, responses ignore even in-window instructions.
# The window-aware fix must not neuter the signal.
# ============================================================
echo -e "${CYAN}--- Group W2: ignoring in-window instructions still fires S13 ---${NC}"

WDIR="$TEST_TMP/w2"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "totally unrelated rambling about zebras", "output_tokens": 20},
  {"content": "more unrelated rambling about weather patterns", "output_tokens": 20},
  {"content": "still nothing about the requested subjects", "output_tokens": 20},
  {"content": "yet another evasive nonanswer entirely", "output_tokens": 20},
  {"content": "final evasive reply ignoring everything", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "W2-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_IRW_TEST",
            "max_tokens": 100,
            "max_turns": 20,
            "context_window": 4,
            "context_strategy": "recent"
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << NAABEOF
use agent

main {
    let words = [$(words_naab_list)]
    let h = agent.create("test_agent")
    let sent = 0
    for i in 0..5 {
        let r = agent.send(h, words[i])
        sent = sent + 1
    }
    print("SENT=" + string(sent))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "SENT=5"; then
    pass "W2-01" "5 windowed sends complete"
else
    fail "W2-01" "Windowed sends did not complete" "$(echo "$OUTPUT" | head -3)"
fi

RECALL=$(last_recall_count "$WDIR/telemetry.jsonl")
if [ -n "$RECALL" ] && [ "$RECALL" -ge 1 ] 2>/dev/null; then
    pass "W2-02" "S13 still fires when in-window instructions are ignored (count=$RECALL)"
else
    fail "W2-02" "S13 did not fire despite zero recall" "instruction_recall_count=${RECALL:-missing}"
fi

# ============================================================
# Group W3: no windowing — full-history aggregate behavior unchanged.
# Same prompts/responses as W1 (recall of current word only), but the agent
# sees the full history, so recall legitimately dilutes to 1/N < 0.10.
# ============================================================
echo -e "${CYAN}--- Group W3: non-windowed full-history behavior unchanged ---${NC}"

WDIR="$TEST_TMP/w3"; mkdir -p "$WDIR"
{
    echo '{"responses": ['
    first=1
    for w in "${WORDS[@]}"; do
        [ $first -eq 0 ] && echo ','
        first=0
        printf '  {"content": "detailed analysis of %s follows shortly", "output_tokens": 20}' "$w"
    done
    echo ''
    echo ']}'
} > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "W3-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_IRW_TEST",
            "max_tokens": 100,
            "max_turns": 20
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << NAABEOF
use agent

main {
    let words = [$(words_naab_list)]
    let h = agent.create("test_agent")
    let sent = 0
    for i in 0..14 {
        let r = agent.send(h, words[i])
        sent = sent + 1
    }
    print("SENT=" + string(sent))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "SENT=14"; then
    pass "W3-01" "14 non-windowed sends complete"
else
    fail "W3-01" "Non-windowed sends did not complete" "$(echo "$OUTPUT" | head -3)"
fi

RECALL=$(last_recall_count "$WDIR/telemetry.jsonl")
if [ -n "$RECALL" ] && [ "$RECALL" -ge 1 ] 2>/dev/null; then
    pass "W3-02" "Full-history recall dilution still fires S13 without windowing (count=$RECALL)"
else
    fail "W3-02" "S13 did not fire for full-history dilution" "instruction_recall_count=${RECALL:-missing}"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS_COUNT  FAIL: $FAIL_COUNT  SKIP: $SKIP_COUNT"
echo "================================================"
[ -n "$FAILURES" ] && echo -e "Failures:$FAILURES"

exit "$FAIL_COUNT"
