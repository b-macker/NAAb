#!/usr/bin/env bash
# ============================================================
# test_validation_signal.sh — S22 validation_outcome CDD signal
#
# Covers agent.record_validation() and the S22 signal that folds
# external ground-truth (pytest/convergence) pass-fail into coherence.
#
# Group A: failing validation erodes coherence + fires the signal
# Group B: passing validation does not penalize
# Group C: default-on but zero-cost until a result is fed
# Group D: per-agent override disables it
# Group E: mid-run global disable = ratchet violation
# Group F: W1 — AGENT_RESPONSE carries config_name
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/validation-signal-$$"

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
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_VALSIG="fake-key-validation-signal"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

presign() {
    local pdir="$TEST_TMP/.presign"; mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"; sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""; rm -rf "$pdir"
}

start_stub() {  # $1=fixture $2=workdir
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do grep -q READY "$2/stub.log" 2>/dev/null && return 0; sleep 0.1; done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# Four on-topic code-ish responses so keyword signals stay quiet and we isolate S22
FIXTURE='{"responses": [
  {"content": "def add(a, b): return a + b  # implemented add operation", "output_tokens": 30},
  {"content": "def subtract(a, b): return a - b  # implemented subtract operation", "output_tokens": 30},
  {"content": "def multiply(a, b): return a * b  # implemented multiply operation", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # implemented divide operation", "output_tokens": 30}
]}'

mk_govern() {  # $1=port $2=extra-agent-json
    cat <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$1",
      "api_key_env": "FAKE_KEY_VALSIG", "max_tokens": 100, "max_turns": 20$2 } }
}
EOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   S22 validation_outcome signal + agent.record_validation    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: failing validation erodes coherence + fires signal
# ============================================================
echo -e "${CYAN}--- Group A: failing validation penalizes ---${NC}"
WDIR="$TEST_TMP/a"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "A-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" "" > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = agent.record_validation(h, false)
    let r2 = agent.send(h, "implement subtract")
    let _2 = agent.record_validation(h, false)
    let r3 = agent.send(h, "implement multiply")
    let c = agent.coherence(h)
    print("FINAL_COHERENCE=" + string(c))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
COH=$(echo "$OUT" | grep FINAL_COHERENCE | sed 's/.*=//')
if echo "$OUT" | grep -q "FINAL_COHERENCE"; then
    pass "A-01" "sends completed with record_validation"
else
    fail "A-01" "run failed" "$(echo "$OUT" | head -3)"
fi
if grep -qE '"event_type":"VALIDATION_RECORDED"' "$WDIR/tele.jsonl" 2>/dev/null; then
    pass "A-02" "VALIDATION_RECORDED telemetry emitted"
else
    fail "A-02" "no VALIDATION_RECORDED event"
fi
if grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "A-03" "validation_outcome fired in CDD_TURN signals_detail"
else
    fail "A-03" "validation_outcome never fired" "$(grep '"CDD_TURN"' "$WDIR/tele.jsonl" | grep -o '"signals_detail":"[^"]*"' | tail -2)"
fi
# coherence must be measurably below 1.0
if [ -n "$COH" ] && awk "BEGIN{exit !($COH < 0.95)}"; then
    pass "A-04" "failing validation drove coherence below 0.95 ($COH)"
else
    fail "A-04" "coherence not penalized" "coherence=$COH"
fi
fi

# ============================================================
# Group B: passing validation does not penalize (via S22)
# ============================================================
echo -e "${CYAN}--- Group B: passing validation is unpenalized ---${NC}"
WDIR="$TEST_TMP/b"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "B-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
# Disable keyword signals so only S22 could move coherence — passing => no S22 fire
mk_govern "$STUB_PORT" ', "context_drift_signals": {"semantic_stability": false, "instruction_recall": false, "entity_consistency": false, "mandate_alignment": false, "coherence_velocity": false, "persona_fingerprint": false}' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = agent.record_validation(h, true)
    let r2 = agent.send(h, "implement subtract")
    let _2 = agent.record_validation(h, true)
    let r3 = agent.send(h, "implement multiply")
    print("FINAL_COHERENCE=" + string(agent.coherence(h)))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
COH=$(echo "$OUT" | grep FINAL_COHERENCE | sed 's/.*=//')
if ! grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "B-01" "validation_outcome did not fire on passing results"
else
    fail "B-01" "validation_outcome fired despite passing"
fi
if [ -n "$COH" ] && awk "BEGIN{exit !($COH >= 0.999)}"; then
    pass "B-02" "coherence unpenalized with passing validation ($COH)"
else
    fail "B-02" "coherence moved despite passing validation + disabled keyword signals" "coherence=$COH"
fi
fi

# ============================================================
# Group C: default-on but zero-cost when never fed
# ============================================================
echo -e "${CYAN}--- Group C: zero-cost when never fed ---${NC}"
WDIR="$TEST_TMP/c"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "C-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" "" > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let r2 = agent.send(h, "implement subtract")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if echo "$OUT" | grep -q DONE && ! grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "C-01" "signal default-on but never fires without record_validation"
else
    fail "C-01" "validation_outcome fired with no result fed"
fi
fi

# ============================================================
# Group D: per-agent override disables the signal
# ============================================================
echo -e "${CYAN}--- Group D: per-agent override disables ---${NC}"
WDIR="$TEST_TMP/d"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "D-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" ', "context_drift_signals": {"validation_outcome": false}' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = agent.record_validation(h, false)
    let r2 = agent.send(h, "implement subtract")
    let _2 = agent.record_validation(h, false)
    let r3 = agent.send(h, "implement multiply")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if ! grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "D-01" "per-agent context_drift_signals:{validation_outcome:false} disables the signal"
else
    fail "D-01" "signal fired despite per-agent override"
fi
fi

# ============================================================
# Group E: mid-run global disable = ratchet violation
# ============================================================
echo -e "${CYAN}--- Group E: mid-run disable is a ratchet violation ---${NC}"
WDIR="$TEST_TMP/e"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "E-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
LOOSE="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"tele.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1,\"signals\":{\"validation_outcome\":false}},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"developer\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_VALSIG\",\"max_tokens\":100,\"max_turns\":20}}}"
LOOSE_SIG=$(presign "$LOOSE")
if [ -z "$LOOSE_SIG" ]; then skip "E-01" "presign failed"; stop_stub; else
export NAAB_LOOSE_JSON="$LOOSE" NAAB_LOOSE_SIG="$LOOSE_SIG"
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1, "signals": { "validation_outcome": true } },
  "capabilities": { "shell": { "enabled": true } },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_VALSIG", "max_tokens": 100, "max_turns": 20 } }
}
EOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "implement subtract")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 40s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub
if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "validation_outcome" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "E-01" "mid-run disable of validation_outcome rejected as ratchet violation"
else
    fail "E-01" "mid-run disable not rejected" "$(grep -i 'ratchet\|reload' "$WDIR/stderr.txt" 2>/dev/null | head -2)"
fi
fi
fi

# ============================================================
# Group F: W1 — AGENT_RESPONSE carries config_name
# ============================================================
echo -e "${CYAN}--- Group F: AGENT_RESPONSE attribution (W1) ---${NC}"
WDIR="$TEST_TMP/f"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "F-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" "" > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if grep -E '"event_type":"AGENT_RESPONSE"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q '"config_name":"developer"'; then
    pass "F-01" "AGENT_RESPONSE carries config_name=developer"
else
    fail "F-01" "AGENT_RESPONSE missing config_name" "$(grep '"AGENT_RESPONSE"' "$WDIR/tele.jsonl" | head -1)"
fi
fi

# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
