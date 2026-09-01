#!/usr/bin/env bash
# ============================================================
# test_concurrent_key_rotation.sh — key selection under concurrent dispatch
#
# AgentTracker::key_offset (agent_impl.cpp:98) is PER HANDLE. It defaulted to
# 0 and agentCreate() never touched it, so every fresh handle started its
# round-robin at keys[0]: concurrently dispatched slots all sent to the SAME
# key, a wider key list did not spread them out, and they then advanced in
# lockstep and collided again every turn. agentCreate() now seeds key_offset
# from a process-global stagger counter, so handles start at different
# positions while the cursor stays private to each handle.
#
# Retries are a separate matter and DO rotate -- key_offset advances on each
# successful selection (:2638), so attempt N of one send uses key N. The claim
# under test is specifically about the FIRST attempt of concurrently dispatched
# handles, which is the only attempt every slot is guaranteed to make.
#
# Controls first, because "they all used one key" is unfalsifiable unless the
# harness is shown capable of recording two different ones:
#   CK-01  positive control — distinct keys ARE observable (401 forces rotation)
#   CK-02  positive control — 401 emits AGENT_KEY_DISABLED
#   CK-03  positive control — requests genuinely overlapped in wall time
#   CK-04  concurrent handles' first attempts spread across distinct keys
#   CK-07  a single-key config still works (stagger must not skip the only key)
#   CK-05  429 does NOT mark a key dead (skip_key_on defaults to [401])
#   CK-08  429 DOES throttle the key for a cooldown (AGENT_KEY_THROTTLED)
#   CK-09  a single-key agent still retries through a 429 rather than failing
#   CK-06  consecutive_failures counts per ATTEMPT, not per send
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_concurrent_key_rotation.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/ckrot-$$"

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

# Distinct VALUES matter: keys.log records what the engine actually sent, so
# two env vars holding the same string would be indistinguishable in the log
# and CK-04 would pass for a rotation that worked perfectly.
export CKROT_K1="ckrot-value-alpha"
export CKROT_K2="ckrot-value-bravo"
export CKROT_K3="ckrot-value-charlie"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# first-attempt key for each handle = the key on the FIRST request the stub saw
# from each distinct slot. With an all-success fixture every handle makes
# exactly one request, so every line in keys.log is a first attempt.
keys_used()   { awk '{print $2}' "$1/keys.log" 2>/dev/null; }
distinct_keys() { keys_used "$1" | sort -u | grep -c . ; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Key selection under concurrent dispatch (key_offset)        |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
# ── Group A: controls — rotation IS observable when it happens ──
echo -e "${CYAN}--- A: controls (the instrument can see two keys) ---${NC}"

WDIR="$TEST_TMP/a"; mkdir -p "$WDIR"
# 401 on the first attempt: is_key_skip, so the key is marked dead and the
# retry must pick a different one. Two keys in keys.log = the log works.
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"status": 401, "error": "invalid key"},
  {"content": "recovered on the second key after the first was refused", "output_tokens": 12}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "CK-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "context_drift": { "enabled": false },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": ["CKROT_K1", "CKROT_K2", "CKROT_K3"],
            "max_tokens": 100, "max_turns": 20,
            "retry": { "max_attempts": 3, "backoff_ms": 0 }
        }
    }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    let r = agent.send(h, "summarize the quarterly ledger reconciliation")
    print("CONTENT:" + r.get("content"))
}
NAABEOF
OUT_A=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

NK=$(distinct_keys "$WDIR")
if [ "$NK" -ge 2 ]; then
    pass "CK-01" "distinct keys are observable in keys.log ($NK seen)"
else
    fail "CK-01" "keys.log cannot distinguish keys — every later arm is vacuous" \
         "distinct=$NK, log=$(keys_used "$WDIR" | tr '\n' ' ')"
fi

if grep -q '"AGENT_KEY_DISABLED"' "$WDIR/telemetry.jsonl" 2>/dev/null; then
    pass "CK-02" "401 marks the key dead (AGENT_KEY_DISABLED emitted)"
else
    fail "CK-02" "401 did not emit AGENT_KEY_DISABLED" \
         "without this, CK-05's absence assertion proves nothing"
fi

# ── Group B: the claim — concurrent handles collide on keys[0] ──
echo ""
echo -e "${CYAN}--- B: concurrent dispatch key selection ---${NC}"

WDIR="$TEST_TMP/b"; mkdir -p "$WDIR"
# hold_ms keeps each slot open so the three requests are genuinely in flight
# together. An instant stub answers slot 1 before slot 2 is dispatched, and
# the arm would then measure sequential sends while claiming to measure
# concurrency -- CK-03 exists to catch exactly that.
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "alpha reviewed the reconciliation and found the ledger consistent", "output_tokens": 12, "hold_ms": 700}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "CK-00b" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "context_drift": { "enabled": false },
    "agent_dispatch": { "max_concurrent": 3, "pool_size": 3 },
    "agents": {
        "alpha": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": ["CKROT_K1", "CKROT_K2", "CKROT_K3"],
            "max_tokens": 100, "max_turns": 20,
            "retry": { "max_attempts": 1, "backoff_ms": 0 }
        },
        "bravo": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": ["CKROT_K1", "CKROT_K2", "CKROT_K3"],
            "max_tokens": 100, "max_turns": 20,
            "retry": { "max_attempts": 1, "backoff_ms": 0 }
        },
        "charlie": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": ["CKROT_K1", "CKROT_K2", "CKROT_K3"],
            "max_tokens": 100, "max_turns": 20,
            "retry": { "max_attempts": 1, "backoff_ms": 0 }
        }
    }
}
GOVEOF
sign_govern "$WDIR"
# Three DISTINCT agent configs, so nobody can attribute a shared cursor to
# handles of one config sharing tracker state.
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let ha = agent.create("alpha")
    let hb = agent.create("bravo")
    let hc = agent.create("charlie")
    let rs = agent.batch([ha, hb, hc],
        ["review the ledger", "review the ledger", "review the ledger"])
    print("BATCH_DONE:" + string(array.length(rs)))
}
NAABEOF
OUT_B=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
stop_stub

REQ_B=$(wc -l < "$WDIR/keys.log" 2>/dev/null || echo 0)
# CK-03: did any two requests actually overlap in wall time?
OVERLAP=$(awk '{n++; s[n]=$3; e[n]=$4}
    END { for (i=1;i<=n;i++) for (j=i+1;j<=n;j++)
            if (s[i] < e[j] && s[j] < e[i]) { print "yes"; exit } }' \
    "$WDIR/keys.log" 2>/dev/null)
if [ "$REQ_B" -lt 3 ]; then
    fail "CK-03" "expected 3 batch requests, saw $REQ_B" "$OUT_B"
elif [ "$OVERLAP" = "yes" ]; then
    pass "CK-03" "requests genuinely overlapped in wall time (concurrent)"
else
    fail "CK-03" "no two requests overlapped — this arm measured sequential sends" \
         "$(cat "$WDIR/keys.log" 2>/dev/null | tr '\n' '; ')"
fi

NKB=$(distinct_keys "$WDIR")
if [ "$NKB" = "3" ]; then
    pass "CK-04" "3 concurrent handles spread across all 3 keys"
elif [ "$NKB" = "1" ]; then
    fail "CK-04" "all 3 concurrent handles used ONE key — stagger is not applied" \
         "$(keys_used "$WDIR" | sort | uniq -c | tr '\n' '; ')"
elif [ "$NKB" -ge 2 ]; then
    fail "CK-04" "only $NKB of 3 keys used — stagger is partial" \
         "$(keys_used "$WDIR" | sort | uniq -c | tr '\n' '; ')"
else
    fail "CK-04" "no keys recorded" "$OUT_B"
fi

# ── CK-07: the stagger must not break a single-key config ──
# The seed is monotonic and unbounded, so a one-key agent gets offsets
# 0,1,2,... which are only correct because selection takes them modulo the
# agent's OWN key list. If that modulo were ever dropped, or the seed treated
# as an index, a single-key config would find no key and every send would
# fail -- which is the common case, not an edge case.
WDIR="$TEST_TMP/b1"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "single key config still reaches the provider correctly", "output_tokens": 10}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "CK-07" "stub failed to start"; }
cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "context_drift": { "enabled": false },
    "agent_dispatch": { "max_concurrent": 3, "pool_size": 3 },
    "agents": {
        "alpha":   { "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "CKROT_K1", "max_tokens": 100, "max_turns": 20 },
        "bravo":   { "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "CKROT_K1", "max_tokens": 100, "max_turns": 20 },
        "charlie": { "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "CKROT_K1", "max_tokens": 100, "max_turns": 20 }
    }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let ha = agent.create("alpha")
    let hb = agent.create("bravo")
    let hc = agent.create("charlie")
    let rs = agent.batch([ha, hb, hc], ["go", "go", "go"])
    print("BATCH_DONE:" + string(array.length(rs)))
}
NAABEOF
OUT_B1=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
stop_stub
REQ_B1=$(wc -l < "$WDIR/keys.log" 2>/dev/null || echo 0)
NKB1=$(distinct_keys "$WDIR")
if [ "$REQ_B1" = "3" ] && [ "$NKB1" = "1" ] && echo "$OUT_B1" | grep -q "BATCH_DONE:3"; then
    pass "CK-07" "single-key config unaffected by the stagger (3 sends, 1 key)"
else
    fail "CK-07" "single-key config broken by the stagger" \
         "requests=$REQ_B1 distinct_keys=$NKB1 out=$(echo "$OUT_B1" | tail -2 | tr '\n' ' ')"
fi

# ── Group C: what a 429 does, and how failures are counted ──
echo ""
echo -e "${CYAN}--- C: 429 handling and failure accounting ---${NC}"

WDIR="$TEST_TMP/c"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [ {"status": 429, "error": "RESOURCE_EXHAUSTED"} ]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "CK-00c" "stub failed to start"; exit 1; }
cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "context_drift": { "enabled": false },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": ["CKROT_K1", "CKROT_K2", "CKROT_K3"],
            "max_tokens": 100, "max_turns": 20,
            "retry": { "max_attempts": 3, "backoff_ms": 0 }
        }
    }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    try { agent.send(h, "review the ledger") } catch (e) { print("SEND_FAILED") }
    print("DONE")
}
NAABEOF
OUT_C=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if grep -q '"AGENT_RETRY"' "$WDIR/telemetry.jsonl" 2>/dev/null; then
    if grep -q '"AGENT_KEY_DISABLED"' "$WDIR/telemetry.jsonl" 2>/dev/null; then
        fail "CK-05" "429 marked a key dead — skip_key_on must now include 429" \
             "$(grep -o '"api_key_env":"[^\"]*"' "$WDIR/telemetry.jsonl" | tr '\n' ' ')"
    else
        pass "CK-05" "CONFIRMED: 429 never marks a key dead (skip_key_on = [401])"
    fi
else
    fail "CK-05" "no AGENT_RETRY events — the 429 arm never ran" "$OUT_C"
fi

if grep -q '"AGENT_KEY_THROTTLED"' "$WDIR/telemetry.jsonl" 2>/dev/null; then
    pass "CK-08" "429 throttles the key for a cooldown (AGENT_KEY_THROTTLED)"
else
    fail "CK-08" "429 produced no throttle — the key is neither skipped nor deprioritised" \
         "$(grep -o '"event_type":"AGENT_[A-Z_]*"' "$WDIR/telemetry.jsonl" 2>/dev/null | sort -u | tr '\n' ' ')"
fi

# CK-09: throttling must be a PREFERENCE, not a restriction. A single-key
# agent has no alternative to fall back to, so if the selector treated a
# throttle as disqualifying, attempt 2 would find no key at all and bail with
# "All API keys exhausted" after ONE request. It must instead keep retrying
# the throttled key, exactly as it did before throttling existed.
WDIR="$TEST_TMP/c1"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [ {"status": 429, "error": "RESOURCE_EXHAUSTED"} ]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "CK-09" "stub failed to start"; }
cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "context_drift": { "enabled": false },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "CKROT_K1", "max_tokens": 100, "max_turns": 20,
            "retry": { "max_attempts": 3, "backoff_ms": 0 }
        }
    }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    try { agent.send(h, "review the ledger") } catch (e) { print("SEND_FAILED") }
    print("DONE")
}
NAABEOF
OUT_C1=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
REQ_C1=$(wc -l < "$WDIR/keys.log" 2>/dev/null || echo 0)
if [ "$REQ_C1" = "3" ]; then
    pass "CK-09" "single-key agent retried through the throttle (3 attempts reached the API)"
else
    fail "CK-09" "throttle disqualified the only key — attempts stopped early" \
         "requests=$REQ_C1 (expected 3), out=$(echo "$OUT_C1" | tail -2 | tr '\n' ' ')"
fi

# CK-06: consecutive_failures is incremented inside the RETRY loop
# (agent_impl.cpp:2838, loop 2616-2949), so ONE send exhausting 5 attempts
# charges 5. With limit=3 a single send must trip the run-level hard stop.
# If the counter were per-send, one send = 1 failure < 3 and no stop fires.
WDIR="$TEST_TMP/d"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [ {"status": 429, "error": "RESOURCE_EXHAUSTED"} ]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "CK-00d" "stub failed to start"; exit 1; }
cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "context_drift": { "enabled": false },
    "agent_dispatch": { "hard_stop": { "consecutive_failure_limit": 3 } },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": ["CKROT_K1", "CKROT_K2", "CKROT_K3"],
            "max_tokens": 100, "max_turns": 20,
            "retry": { "max_attempts": 5, "backoff_ms": 0 }
        }
    }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    try { agent.send(h, "review the ledger") } catch (e) { print("SEND_FAILED") }
    print("DONE")
}
NAABEOF
OUT_D=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if grep -q '"AGENT_HARD_STOP"' "$WDIR/telemetry.jsonl" 2>/dev/null; then
    pass "CK-06" "CONFIRMED: ONE send tripped consecutive_failure_limit=3 (per-attempt)"
else
    fail "CK-06" "no hard stop from a single 5-attempt send — counter may be per-send" \
         "retries=$(grep -c '\"AGENT_RETRY\"' "$WDIR/telemetry.jsonl" 2>/dev/null || echo 0)"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}| Results: ${GREEN}${PASS_COUNT} passed${NC}, ${RED}${FAIL_COUNT} failed${NC}, ${YELLOW}${SKIP_COUNT} skipped${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}${FAILURES}"; exit 1; fi
exit 0
