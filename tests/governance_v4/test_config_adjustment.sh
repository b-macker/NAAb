#!/usr/bin/env bash
# ============================================================
# test_config_adjustment.sh — Living-script findings: engine fixes
#
# Uses the local agent stub (tests/helpers/agent_stub.py) via the per-agent
# api_base override so reload behavior is testable without live API keys.
#
# Group A: min_tokens floor — effective request budget = max(max_tokens, min_tokens)
# Group B: accepted mid-run reload emits CONFIG_ADJUSTMENT telemetry (accepted=true)
#          and the scoped CDD rate-window reset doesn't break subsequent sends
# Group C: loosened reload (min_tokens lowered) rejected by ratchet and emits
#          CONFIG_ADJUSTMENT (accepted=false, reason=ratchet)
# Group D: rate_normalized_floor parses; lowering it mid-run is a ratchet
#          violation; enabling rate_normalized mid-run is a ratchet violation
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_config_adjustment.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/configadj-$$"

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
export FAKE_KEY_CONFIGADJ_TEST="fake-key-config-adjustment-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

# Pre-sign JSON content out-of-band; echoes the signature.
# (NAAB_SIGNING_KEY is scrubbed from subprocess env per V-SC-006, so mid-run
# reload tests must write a pre-computed .sig from the shell block.)
presign() {  # $1=json content
    local pdir="$TEST_TMP/.presign"
    mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"
    sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""
    rm -rf "$pdir"
}

# Repointed at the shared hardened launcher (checksheet item D1, "repoint on
# touch"). The local copy this replaces picked a port with `(RANDOM % 20000) +
# 20000`, once, with no bind check and no retry -- at FIVE call sites in this
# one suite, so it rolled that die five times per run. On a loaded runner a
# collision or a lingering TIME_WAIT socket leaves the stub dead, and then every
# assertion in the group fails for a reason that has nothing to do with what the
# suite measures. That is what took CI red on 3643ccb, a docs-only commit, while
# the same code passed six consecutive local runs.
#
# The shared launcher retries the pick, verifies the bind, and resolves
# agent_stub.py relative to itself rather than the caller's $SCRIPT_DIR.
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Config Adjustment Tests (min_tokens / scoped reset / floor) |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: min_tokens floor on effective request budget
# ============================================================
echo -e "${CYAN}--- Group A: min_tokens floor ---${NC}"

WDIR="$TEST_TMP/group_a"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [{"content": "alpha response", "output_tokens": 20}]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "A-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CONFIGADJ_TEST",
            "max_tokens": 100,
            "min_tokens": 512,
            "max_turns": 20
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let env = agent.environment(h)
    print("MIN_TOKENS=" + string(env.get("limits").get("min_tokens")))
    let r1 = agent.send(h, "first message")
    print("CONTENT=" + string(r1.get("content")))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "MIN_TOKENS=512"; then
    pass "A-01" "min_tokens parses and is exposed in agent environment limits"
else
    fail "A-01" "min_tokens missing from environment limits" "$OUTPUT"
fi

if echo "$OUTPUT" | grep -q "CONTENT=alpha response"; then
    pass "A-02" "send succeeds with min_tokens > max_tokens (floor wins, not an error)"
else
    fail "A-02" "send failed with min_tokens set" "$OUTPUT"
fi

if grep -q '"maxOutputTokens": *512' "$WDIR/req_1.json" 2>/dev/null; then
    pass "A-03" "request budget floored: maxOutputTokens = max(max_tokens=100, min_tokens=512)"
else
    fail "A-03" "request did not use min_tokens floor" "$(cat "$WDIR/req_1.json" 2>/dev/null | head -c 300)"
fi

# ============================================================
# Group B: accepted reload → CONFIG_ADJUSTMENT + scoped reset
# ============================================================
echo ""
echo -e "${CYAN}--- Group B: accepted reload emits CONFIG_ADJUSTMENT ---${NC}"

WDIR="$TEST_TMP/group_b"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "first stub reply", "output_tokens": 20},
  {"content": "second stub reply", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "B-00" "stub failed to start"; stop_stub; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "version": "5.0",
    "mode": "enforce",
    "sandbox_level": "unrestricted",
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "context_drift": { "enabled": true, "check_interval_turns": 1 },
    "capabilities": { "shell": { "enabled": true } },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CONFIGADJ_TEST",
            "max_tokens": 200,
            "max_turns": 20,
            "system_prompt": "You are a helpful research assistant."
        }
    }
}
GOVEOF

# Reload with a changed system_prompt (behavior-affecting, not ratcheted) and a
# tightened max_tokens. Must keep api_base identical (mid-run change = violation).
CHANGED_JSON="{\"version\":\"5.0\",\"mode\":\"enforce\",\"sandbox_level\":\"unrestricted\",\"update_reason\":\"operator adjustment test\",\"telemetry\":{\"enabled\":true,\"output_file\":\"telemetry.jsonl\"},\"context_drift\":{\"enabled\":true,\"check_interval_turns\":1},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"test_agent\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_CONFIGADJ_TEST\",\"max_tokens\":150,\"max_turns\":20,\"system_prompt\":\"You are a meticulous code reviewer.\"}}}"
CHANGED_SIG=$(presign "$CHANGED_JSON")
if [ -z "$CHANGED_SIG" ]; then
    skip "B-00" "failed to pre-sign changed govern.json"
    stop_stub
else
export NAAB_PRESIGNED_JSON="$CHANGED_JSON"
export NAAB_PRESIGNED_SIG="$CHANGED_SIG"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let r1 = agent.send(h, "hello")
    print("R1=" + string(r1.get("content")))

    // Operator adjusts the config mid-run (pre-signed out-of-band)
    // sleep first: reload detection is mtime-based with 1s granularity, and the
    // stub responds fast enough that the write can land in the load second
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_PRESIGNED_JSON" > govern.json
printf '%s' "$NAAB_PRESIGNED_SIG" > govern.json.sig
>>

    // Reload is picked up before this send; scoped reset must not break it
    let r2 = agent.send(h, "and again")
    print("R2=" + string(r2.get("content")))
    let notices = r2.get("governance_notices")
    print("HAS_NOTICES=" + string(notices != null))
}
NAABEOF

sign_govern "$WDIR"
OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub

if echo "$OUTPUT" | grep -q "R1=first stub reply" && echo "$OUTPUT" | grep -q "R2=second stub reply"; then
    pass "B-01" "sends succeed before and after mid-run reload (scoped reset is non-fatal)"
else
    fail "B-01" "send sequence across reload failed" "$OUTPUT $(tail -3 "$WDIR/stderr.txt" 2>/dev/null)"
fi

if grep -q "Config reloaded mid-run" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "B-02" "reload accepted"
else
    fail "B-02" "reload not applied" "$(tail -5 "$WDIR/stderr.txt" 2>/dev/null)"
fi

TELEM="$WDIR/telemetry.jsonl"
if grep -q '"event_type":"CONFIG_ADJUSTMENT"' "$TELEM" 2>/dev/null; then
    pass "B-03" "CONFIG_ADJUSTMENT telemetry event emitted"
else
    fail "B-03" "no CONFIG_ADJUSTMENT event in telemetry" "$(tail -3 "$TELEM" 2>/dev/null)"
fi

ADJ_LINE=$(grep '"event_type":"CONFIG_ADJUSTMENT"' "$TELEM" 2>/dev/null | tail -1)
if echo "$ADJ_LINE" | grep -q '"accepted":"true"'; then
    pass "B-04" "event marks reload as accepted"
else
    fail "B-04" "event missing accepted=true" "$ADJ_LINE"
fi

if echo "$ADJ_LINE" | grep -q '"changed_agents":"[^"]*test_agent'; then
    pass "B-05" "event lists the changed agent (system_prompt/max_tokens diff detected)"
else
    fail "B-05" "changed_agents does not include test_agent" "$ADJ_LINE"
fi

if echo "$ADJ_LINE" | grep -q 'operator adjustment test'; then
    pass "B-06" "event carries update_reason"
else
    fail "B-06" "update_reason missing from event" "$ADJ_LINE"
fi
fi

# ============================================================
# Group C: loosened reload rejected → CONFIG_ADJUSTMENT (rejected)
# ============================================================
echo ""
echo -e "${CYAN}--- Group C: min_tokens ratchet + rejected-reload telemetry ---${NC}"

WDIR="$TEST_TMP/group_c"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [{"content": "stub reply", "output_tokens": 20}]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "C-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "version": "5.0",
    "mode": "enforce",
    "sandbox_level": "unrestricted",
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "capabilities": { "shell": { "enabled": true } },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CONFIGADJ_TEST",
            "max_tokens": 1024,
            "min_tokens": 512,
            "max_turns": 20
        }
    }
}
GOVEOF

# Loosened: min_tokens floor lowered 512 → 64 (ratchet violation)
LOOSENED_JSON="{\"version\":\"5.0\",\"mode\":\"enforce\",\"sandbox_level\":\"unrestricted\",\"telemetry\":{\"enabled\":true,\"output_file\":\"telemetry.jsonl\"},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"test_agent\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_CONFIGADJ_TEST\",\"max_tokens\":1024,\"min_tokens\":64,\"max_turns\":20}}}"
LOOSENED_SIG=$(presign "$LOOSENED_JSON")
export NAAB_PRESIGNED_JSON="$LOOSENED_JSON"
export NAAB_PRESIGNED_SIG="$LOOSENED_SIG"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let r1 = agent.send(h, "hello")
    print("R1_OK=true")

    // sleep first: reload detection is mtime-based with 1s granularity, and the
    // stub responds fast enough that the write can land in the load second
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_PRESIGNED_JSON" > govern.json
printf '%s' "$NAAB_PRESIGNED_SIG" > govern.json.sig
>>

    try {
        let r2 = agent.send(h, "bye")
        print("R2_NOTICES=" + string(r2.get("governance_notices") != null))
    } catch (e) {
        print("R2_NOTICES=false")
    }
}
NAABEOF

sign_govern "$WDIR"
OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub

if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "min_tokens" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "C-01" "lowering min_tokens mid-run rejected as ratchet violation"
else
    fail "C-01" "min_tokens loosening not rejected" "$(grep -i 'reload' "$WDIR/stderr.txt" 2>/dev/null | head -3)"
fi

TELEM="$WDIR/telemetry.jsonl"
REJ_LINE=$(grep '"event_type":"CONFIG_ADJUSTMENT"' "$TELEM" 2>/dev/null | tail -1)
if echo "$REJ_LINE" | grep -q '"accepted":"false"' && echo "$REJ_LINE" | grep -q '"reason":"ratchet"'; then
    pass "C-02" "rejected reload emits CONFIG_ADJUSTMENT (accepted=false, reason=ratchet)"
else
    fail "C-02" "no rejected CONFIG_ADJUSTMENT event" "$REJ_LINE"
fi

if echo "$REJ_LINE" | grep -q 'min_tokens'; then
    pass "C-03" "rejected event names the violated field"
else
    fail "C-03" "violation detail missing min_tokens" "$REJ_LINE"
fi

# ============================================================
# Group D: rate_normalized_floor parse + ratchet
# ============================================================
echo ""
echo -e "${CYAN}--- Group D: rate_normalized_floor ---${NC}"

WDIR="$TEST_TMP/group_d"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [{"content": "stub reply", "output_tokens": 20}]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "D-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "version": "5.0",
    "mode": "enforce",
    "sandbox_level": "unrestricted",
    "context_drift": { "enabled": true, "rate_normalized": true, "rate_normalized_floor": 0.5 },
    "capabilities": { "shell": { "enabled": true } },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CONFIGADJ_TEST",
            "max_tokens": 200,
            "max_turns": 20
        }
    }
}
GOVEOF

# Loosened: floor lowered 0.5 → 0.1
FLOOR_JSON="{\"version\":\"5.0\",\"mode\":\"enforce\",\"sandbox_level\":\"unrestricted\",\"context_drift\":{\"enabled\":true,\"rate_normalized\":true,\"rate_normalized_floor\":0.1},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"test_agent\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_CONFIGADJ_TEST\",\"max_tokens\":200,\"max_turns\":20}}}"
FLOOR_SIG=$(presign "$FLOOR_JSON")
export NAAB_PRESIGNED_JSON="$FLOOR_JSON"
export NAAB_PRESIGNED_SIG="$FLOOR_SIG"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let r1 = agent.send(h, "hello")
    print("R1_OK=true")

    // sleep first: reload detection is mtime-based with 1s granularity, and the
    // stub responds fast enough that the write can land in the load second
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_PRESIGNED_JSON" > govern.json
printf '%s' "$NAAB_PRESIGNED_SIG" > govern.json.sig
>>

    try {
        let r2 = agent.send(h, "bye")
        print("R2_DONE=true")
    } catch (e) {
        print("R2_DONE=true")
    }
}
NAABEOF

sign_govern "$WDIR"
OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub

if echo "$OUTPUT" | grep -q "R1_OK=true"; then
    pass "D-01" "rate_normalized + rate_normalized_floor config parses and runs"
else
    fail "D-01" "config with rate_normalized_floor failed to run" "$OUTPUT $(tail -3 "$WDIR/stderr.txt" 2>/dev/null)"
fi

if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "rate_normalized_floor" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "D-02" "lowering rate_normalized_floor mid-run rejected as ratchet violation"
else
    fail "D-02" "floor loosening not rejected" "$(grep -i 'reload' "$WDIR/stderr.txt" 2>/dev/null | head -3)"
fi

# Enabling rate_normalized mid-run (false→true) strictly reduces penalties = loosening
WDIR="$TEST_TMP/group_d2"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [{"content": "stub reply", "output_tokens": 20}]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "D-03" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "version": "5.0",
    "mode": "enforce",
    "sandbox_level": "unrestricted",
    "context_drift": { "enabled": true, "rate_normalized": false },
    "capabilities": { "shell": { "enabled": true } },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CONFIGADJ_TEST",
            "max_tokens": 200,
            "max_turns": 20
        }
    }
}
GOVEOF

ENABLE_JSON="{\"version\":\"5.0\",\"mode\":\"enforce\",\"sandbox_level\":\"unrestricted\",\"context_drift\":{\"enabled\":true,\"rate_normalized\":true},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"test_agent\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_CONFIGADJ_TEST\",\"max_tokens\":200,\"max_turns\":20}}}"
ENABLE_SIG=$(presign "$ENABLE_JSON")
export NAAB_PRESIGNED_JSON="$ENABLE_JSON"
export NAAB_PRESIGNED_SIG="$ENABLE_SIG"

cp "$TEST_TMP/group_d/test.naab" "$WDIR/test.naab"
sign_govern "$WDIR"
OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub

if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "rate_normalized" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "D-03" "enabling rate_normalized mid-run rejected (penalty dilution = loosening)"
else
    fail "D-03" "rate_normalized enable not rejected" "$(grep -i 'reload' "$WDIR/stderr.txt" 2>/dev/null | head -3)"
fi

# ============================================================
# Results
# ============================================================
echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC} (of $TOTAL)"
[ -n "$FAILURES" ] && echo -e "  Failures:$FAILURES"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ]
