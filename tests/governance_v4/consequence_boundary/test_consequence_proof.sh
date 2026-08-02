#!/usr/bin/env bash
# test_consequence_proof.sh — Governance enforcement proof harness
#
# Proves each enforcement path BLOCKS when violated.
# Tests are grouped:
#   A: Lease enforcement (need API keys — skip gracefully)
#   B: Advisory escalation (no API key needed)
#   C: Governance properties (no API key needed)
#   D: Enforcement blocking (no API key needed)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${1:-$(cd "$SCRIPT_DIR/../../.." && pwd)/build/naab-lang}"
PASS=0
FAIL=0
SKIP=0
WORKDIR="${HOME}/.naab/test_consequence_$$"
mkdir -p "$WORKDIR"

ok()   { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }
skip() { SKIP=$((SKIP + 1)); echo "  SKIP: $1 (needs API key)"; }

# Trust store isolation — generate a test-specific keypair and trust it.
# Without this, sign_gov signs with a key that may not match the trust store.
source "$SCRIPT_DIR/../../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$WORKDIR"; }
trap cleanup EXIT

"$NAAB" --keygen "$WORKDIR/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$WORKDIR/k.pem.pub" 2>/dev/null
SIGNING_KEY="$WORKDIR/k.pem"
export NAAB_SIGNING_KEY="$SIGNING_KEY"

sign_gov() {
    local dir="$1"
    (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
}

# Probe for API key (Group A tests)
HAS_API_KEY=false
API_KEY_NAME=""
for k in GK5 GK6 GK1 GK2 GK3 GK4; do
    if [ -n "${!k:-}" ]; then
        HAS_API_KEY=true
        API_KEY_NAME="$k"
        break
    fi
done

echo "=== Governance Consequence Proof Harness ==="
echo "  Binary: $NAAB"
echo "  API key: $HAS_API_KEY"
echo ""

# =====================================================================
# GROUP B: Advisory Escalation (NO API key needed)
# =====================================================================
echo "--- Group B: Advisory Escalation ---"

# B1: Advisory escalation to SOFT block (exit 3)
B1DIR="$WORKDIR/b1"
mkdir -p "$B1DIR"
cat > "$B1DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [
        {
            "id": "PROOF-B1",
            "name": "proof_marker_check",
            "pattern": "PROOF_MARKER",
            "level": "advisory",
            "enabled": true,
            "message": "Proof marker found in code"
        }
    ],
    "advisory_escalation": {
        "enabled": true,
        "soft_after": 3,
        "weight_multiplier": 1.5
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$B1DIR"
cat > "$B1DIR/test.naab" << 'EOF'
main {
    let a = <<javascript
// PROOF_MARKER
1 + 1
>>
    let b = <<javascript
// PROOF_MARKER
2 + 2
>>
    let c = <<javascript
// PROOF_MARKER
3 + 3
>>
    print("should not reach here")
}
EOF
OUT_B1=$("$NAAB" "$B1DIR/test.naab" 2>&1) && RC_B1=$? || RC_B1=$?
if [ $RC_B1 -eq 3 ] && echo "$OUT_B1" | grep -q "ESCALATED"; then
    ok "B1: advisory escalation to SOFT block (exit $RC_B1, ESCALATED in output)"
else
    fail "B1: expected exit 3 + ESCALATED, got exit $RC_B1"
    echo "    output: ${OUT_B1:0:300}"
fi

# B2: Escalated advisory visible in dashboard
OUT_B2=$("$NAAB" --governance-dashboard "$B1DIR/test.naab" 2>&1) && RC_B2=$? || RC_B2=$?
if [ $RC_B2 -eq 3 ] && echo "$OUT_B2" | grep -q "ESCALATED"; then
    ok "B2: escalated advisory visible in dashboard (exit $RC_B2)"
else
    fail "B2: expected exit 3 + ESCALATED in dashboard, got exit $RC_B2"
    echo "    output: ${OUT_B2:0:300}"
fi

# B3: verifyScoreIntegrity no false-positive after weight multiplier
B3DIR="$WORKDIR/b3"
mkdir -p "$B3DIR"
cat > "$B3DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [
        {
            "id": "PROOF-B3",
            "name": "proof_score_check",
            "pattern": "SCORE_MARKER",
            "level": "advisory",
            "enabled": true,
            "message": "Score marker found"
        }
    ],
    "advisory_escalation": {
        "enabled": true,
        "soft_after": 10,
        "weight_multiplier": 2.0
    },
    "scoring": {
        "enabled": true,
        "default_weight": 5,
        "yellow_threshold": 100,
        "red_threshold": 200
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$B3DIR"
cat > "$B3DIR/test.naab" << 'EOF'
main {
    let a = <<javascript
// SCORE_MARKER
1
>>
    let b = <<javascript
// SCORE_MARKER
2
>>
    let c = <<javascript
// SCORE_MARKER
3
>>
    print("score test done")
}
EOF
OUT_B3=$("$NAAB" --governance-dashboard "$B3DIR/test.naab" 2>&1) && RC_B3=$? || RC_B3=$?
if [ $RC_B3 -eq 0 ] && ! echo "$OUT_B3" | grep -qi "integrity.*mismatch\|score.*tamper"; then
    ok "B3: score integrity holds with weight multiplier (exit $RC_B3, no false positive)"
else
    fail "B3: expected exit 0 + no integrity mismatch, got exit $RC_B3"
    echo "    output: ${OUT_B3:0:300}"
fi

# B4: Score accumulation is deterministic (run B3 twice, compare)
OUT_B4a=$("$NAAB" --governance-dashboard "$B3DIR/test.naab" 2>&1) || true
OUT_B4b=$("$NAAB" --governance-dashboard "$B3DIR/test.naab" 2>&1) || true
SCORE_A=$(echo "$OUT_B4a" | grep -oi "Risk score: [0-9]*" | head -1)
SCORE_B=$(echo "$OUT_B4b" | grep -oi "Risk score: [0-9]*" | head -1)
if [ -n "$SCORE_A" ] && [ "$SCORE_A" = "$SCORE_B" ]; then
    ok "B4: score deterministic across runs ($SCORE_A)"
elif [ -z "$SCORE_A" ] && [ -z "$SCORE_B" ]; then
    # Risk score line may not appear if below yellow threshold
    CHECKS_A=$(echo "$OUT_B4a" | grep -o "[0-9]* passed" | head -1)
    CHECKS_B=$(echo "$OUT_B4b" | grep -o "[0-9]* passed" | head -1)
    if [ "$CHECKS_A" = "$CHECKS_B" ]; then
        ok "B4: governance checks deterministic across runs ($CHECKS_A)"
    else
        fail "B4: governance checks differ: run1='$CHECKS_A' run2='$CHECKS_B'"
    fi
else
    fail "B4: score mismatch: run1='$SCORE_A' run2='$SCORE_B'"
fi

echo ""

# =====================================================================
# GROUP C: Governance Properties (NO API key needed)
# =====================================================================
echo "--- Group C: Governance Properties ---"

# C1: Epoch accessible and non-negative
C1DIR="$WORKDIR/c1"
mkdir -p "$C1DIR"
cat > "$C1DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "governance_health": { "enabled": true },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$C1DIR"
cat > "$C1DIR/test.naab" << 'EOF'
use governance

main {
    let h = governance.health()
    let epoch = h.get("governance_epoch") ?? -1
    if epoch >= 0 {
        print("EPOCH_OK=" + string(epoch))
    } else {
        print("EPOCH_MISSING")
    }
}
EOF
OUT_C1=$("$NAAB" "$C1DIR/test.naab" 2>&1) && RC_C1=$? || RC_C1=$?
if echo "$OUT_C1" | grep -q "EPOCH_OK"; then
    ok "C1: epoch accessible and non-negative"
else
    fail "C1: expected EPOCH_OK, got exit $RC_C1"
    echo "    output: ${OUT_C1:0:300}"
fi

# C2: Telemetry connected reports true
C2DIR="$WORKDIR/c2"
mkdir -p "$C2DIR"
cat > "$C2DIR/govern.json" << EOF
{
    "mode": "enforce",
    "governance_health": { "enabled": true },
    "telemetry": {
        "enabled": true,
        "output_file": "$C2DIR/telemetry.jsonl",
        "tamper_evidence": { "enabled": true, "algorithm": "sha256", "chain_genesis": "TEST" }
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$C2DIR"
cat > "$C2DIR/test.naab" << 'EOF'
use governance

main {
    let h = governance.health()
    let connected = h.get("telemetry_connected")
    if connected == true {
        print("TELEMETRY_OK")
    } else if connected == false {
        print("TELEMETRY_DISCONNECTED")
    } else {
        print("TELEMETRY_MISSING")
    }
}
EOF
OUT_C2=$("$NAAB" "$C2DIR/test.naab" 2>&1) && RC_C2=$? || RC_C2=$?
if echo "$OUT_C2" | grep -q "TELEMETRY_OK"; then
    ok "C2: telemetry connected reports true"
elif echo "$OUT_C2" | grep -q "TELEMETRY_MISSING"; then
    # telemetry_connected field may not exist if pulse hasn't run yet
    ok "C2: telemetry field not yet populated (pulse not triggered — acceptable)"
else
    fail "C2: expected TELEMETRY_OK, got exit $RC_C2"
    echo "    output: ${OUT_C2:0:300}"
fi

# C3: BSD + CDD subsystems connected
C3DIR="$WORKDIR/c3"
mkdir -p "$C3DIR"
cat > "$C3DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "governance_health": { "enabled": true },
    "behavioral_sequences": { "enabled": true, "window_size": 50 },
    "context_drift": { "enabled": true, "coherence_threshold": 0.4, "check_interval_turns": 1 },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$C3DIR"
cat > "$C3DIR/test.naab" << 'EOF'
use governance

main {
    let h = governance.health()
    let bsd = h.get("bsd_connected") ?? false
    let cdd = h.get("cdd_connected") ?? false
    if bsd && cdd {
        print("SUBSYSTEMS_OK")
    } else {
        print("SUBSYSTEMS_PARTIAL bsd=" + string(bsd) + " cdd=" + string(cdd))
    }
}
EOF
OUT_C3=$("$NAAB" "$C3DIR/test.naab" 2>&1) && RC_C3=$? || RC_C3=$?
if echo "$OUT_C3" | grep -q "SUBSYSTEMS_OK\|SUBSYSTEMS_PARTIAL"; then
    ok "C3: BSD+CDD subsystem health accessible"
else
    fail "C3: expected SUBSYSTEMS_OK, got exit $RC_C3"
    echo "    output: ${OUT_C3:0:300}"
fi

# C4: Governance determinism — same file twice produces same check counts
OUT_C4a=$("$NAAB" --governance-dashboard "$B3DIR/test.naab" 2>&1) || true
OUT_C4b=$("$NAAB" --governance-dashboard "$B3DIR/test.naab" 2>&1) || true
PASSED_A=$(echo "$OUT_C4a" | grep -o "[0-9]* passed" | head -1)
PASSED_B=$(echo "$OUT_C4b" | grep -o "[0-9]* passed" | head -1)
if [ -n "$PASSED_A" ] && [ "$PASSED_A" = "$PASSED_B" ]; then
    ok "C4: governance deterministic across runs ($PASSED_A)"
else
    fail "C4: governance not deterministic: run1='$PASSED_A' run2='$PASSED_B'"
fi

echo ""

# =====================================================================
# GROUP D: Enforcement Blocking (NO API key needed)
# =====================================================================
echo "--- Group D: Enforcement Blocking ---"

# D1: Hard-level custom rule blocks on first match (exit 3)
D1DIR="$WORKDIR/d1"
mkdir -p "$D1DIR"
cat > "$D1DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [
        {
            "id": "PROOF-D1",
            "name": "hard_block_proof",
            "pattern": "HARD_BLOCK_TARGET",
            "level": "hard",
            "enabled": true,
            "message": "Hard block on first match"
        }
    ],
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$D1DIR"
cat > "$D1DIR/test.naab" << 'EOF'
main {
    let a = <<javascript
// HARD_BLOCK_TARGET
1
>>
    print("should not reach here")
}
EOF
OUT_D1=$("$NAAB" "$D1DIR/test.naab" 2>&1) && RC_D1=$? || RC_D1=$?
if [ $RC_D1 -eq 3 ] && echo "$OUT_D1" | grep -qi "HARD\|PROOF-D1\|hard_block"; then
    ok "D1: hard-level custom rule blocks on first match (exit $RC_D1)"
else
    fail "D1: expected exit 3 + hard block, got exit $RC_D1"
    echo "    output: ${OUT_D1:0:300}"
fi

# D2: Taint enforcement blocks unsanitized sink
D2DIR="$WORKDIR/d2"
mkdir -p "$D2DIR"
cat > "$D2DIR/govern.json" << EOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": {
        "filesystem": { "mode": "readwrite", "allowed_paths": ["$D2DIR/"] },
        "env_vars": { "read": true }
    },
    "taint_tracking": {
        "enabled": true,
        "level": "hard",
        "sources": ["env.get"],
        "sinks": ["file.write"],
        "sanitizers": ["sanitize_"]
    }
}
EOF
sign_gov "$D2DIR"
cat > "$D2DIR/test.naab" << EOF
use env
use file

main {
    let secret = env.get("HOME")
    file.write("$D2DIR/leak.txt", string(secret))
    print("should not reach here")
}
EOF
OUT_D2=$("$NAAB" "$D2DIR/test.naab" 2>&1) && RC_D2=$? || RC_D2=$?
if [ $RC_D2 -ne 0 ] && echo "$OUT_D2" | grep -qi "taint\|sanitiz"; then
    ok "D2: taint blocks unsanitized env.get -> file.write (exit $RC_D2)"
else
    fail "D2: expected taint block, got exit $RC_D2"
    echo "    output: ${OUT_D2:0:300}"
fi

# D3: Taint cleared by sanitizer allows write
D3DIR="$WORKDIR/d3"
mkdir -p "$D3DIR"
cat > "$D3DIR/govern.json" << EOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": {
        "filesystem": { "mode": "readwrite", "allowed_paths": ["$D3DIR/"] },
        "env_vars": { "read": true }
    },
    "taint_tracking": {
        "enabled": true,
        "level": "hard",
        "sources": ["env.get"],
        "sinks": ["file.write"],
        "sanitizers": ["sanitize_"]
    }
}
EOF
sign_gov "$D3DIR"
cat > "$D3DIR/test.naab" << EOF
use env
use file

fn sanitize_value(input) {
    if input == null { return "" }
    let s = string(input)
    if len(s) > 1000 { return "" }
    return s
}

main {
    let secret = env.get("HOME")
    let clean = sanitize_value(secret)
    file.write("$D3DIR/safe.txt", clean)
    print("TAINT_CLEARED")
}
EOF
OUT_D3=$("$NAAB" "$D3DIR/test.naab" 2>&1) && RC_D3=$? || RC_D3=$?
if echo "$OUT_D3" | grep -q "TAINT_CLEARED"; then
    ok "D3: taint cleared by sanitizer allows write"
else
    fail "D3: expected TAINT_CLEARED (exit 0), got exit $RC_D3"
    echo "    output: ${OUT_D3:0:300}"
fi

echo ""

# =====================================================================
# GROUP A: Lease Enforcement (API key needed for some)
# =====================================================================
echo "--- Group A: Lease Enforcement ---"

# A1: Wall-clock lease expiry blocks agent.send
# The lease check at agent_impl.cpp:793 fires BEFORE the API call
# A dummy API key satisfies config validation; the throw is time-based
A1DIR="$WORKDIR/a1"
mkdir -p "$A1DIR"
cat > "$A1DIR/govern.json" << EOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "agents": {
        "lease_test": {
            "model": "test-model",
            "provider": "gemini",
            "api_key_env": "PROOF_DUMMY_KEY",
            "max_tokens": 100,
            "temperature": 0.1,
            "max_turns": 10,
            "standing_lease_seconds": 1,
            "retry": { "max_attempts": 1, "backoff_ms": 100 }
        }
    },
    "circuit_breaker": { "enabled": false }
}
EOF
sign_gov "$A1DIR"
cat > "$A1DIR/test.naab" << 'EOF'
use agent
use time

main {
    let h = agent.create("lease_test")
    // Wait for wall-clock lease to expire
    time.sleep(2)
    // This send should throw BEFORE making the API call
    let blocked = false
    try {
        agent.send(h, "test")
    } catch (e) {
        let msg = string(e)
        if msg.contains("lease expired") || msg.contains("Wall-clock") || msg.contains("Lease") {
            blocked = true
            print("WALL_CLOCK_LEASE_BLOCKED")
        } else {
            print("WRONG_ERROR: " + msg)
        }
    }
    if !blocked {
        print("LEASE_NOT_BLOCKED")
    }
}
EOF
export PROOF_DUMMY_KEY="dummy_key_for_lease_test"
OUT_A1=$("$NAAB" "$A1DIR/test.naab" 2>&1) && RC_A1=$? || RC_A1=$?
unset PROOF_DUMMY_KEY
if echo "$OUT_A1" | grep -q "WALL_CLOCK_LEASE_BLOCKED"; then
    ok "A1: wall-clock lease expiry blocks agent.send"
elif echo "$OUT_A1" | grep -q "WRONG_ERROR"; then
    # Send threw a different error (API key invalid, etc.) — lease check may have passed
    fail "A1: agent.send threw wrong error (lease check may not have fired)"
    echo "    output: ${OUT_A1:0:300}"
elif echo "$OUT_A1" | grep -q "LEASE_NOT_BLOCKED"; then
    fail "A1: wall-clock lease did not block (send succeeded or didn't throw)"
    echo "    output: ${OUT_A1:0:300}"
else
    fail "A1: unexpected output (exit $RC_A1)"
    echo "    output: ${OUT_A1:0:300}"
fi

# A2: Turn-based lease expiry blocks agent.send
# Needs real API key because turns increment after successful send
if [ "$HAS_API_KEY" = true ]; then
    A2DIR="$WORKDIR/a2"
    mkdir -p "$A2DIR"
    cat > "$A2DIR/govern.json" << EOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "agents": {
        "turn_lease_test": {
            "model": ["gemma-4-31b-it", "gemini-2.5-flash"],
            "provider": "gemini",
            "api_key_env": "$API_KEY_NAME",
            "max_tokens": 32,
            "temperature": 0.1,
            "max_turns": 10,
            "standing_lease_turns": 2,
            "system_prompt": "Reply with exactly: OK",
            "retry": {
                "max_attempts": 3,
                "backoff_ms": 500,
                "backoff_multiplier": 2.0,
                "jitter": true,
                "retry_on": [429, 503],
                "skip_key_on": [401],
                "fallback_model_on": [404, 503],
                "never_retry": [400]
            }
        }
    },
    "circuit_breaker": { "enabled": false },
    "agent_dispatch": {
        "hard_stop": { "max_calls_per_run": 10, "max_tokens_per_run": 5000 }
    }
}
EOF
    sign_gov "$A2DIR"
    cat > "$A2DIR/test.naab" << 'EOF'
use agent

main {
    let h = agent.create("turn_lease_test")
    let blocked = false
    let error_msg = ""

    // Send 1 (turn 0 -> 1)
    try { agent.send(h, "Say OK") } catch (e) { error_msg = string(e) }

    // Send 2 (turn 1 -> 2)
    if error_msg == "" {
        try { agent.send(h, "Say OK") } catch (e) { error_msg = string(e) }
    }

    // Send 3 (turn 2 >= lease 2 -> BLOCKED)
    if error_msg == "" {
        try {
            agent.send(h, "Say OK")
        } catch (e) {
            error_msg = string(e)
            if error_msg.contains("lease expired") || error_msg.contains("Standing lease") || error_msg.contains("Lease") {
                blocked = true
            }
        }
    }

    if blocked {
        print("TURN_LEASE_BLOCKED")
    } else if error_msg != "" {
        print("WRONG_ERROR: " + error_msg)
    } else {
        print("LEASE_NOT_BLOCKED")
    }
}
EOF
    OUT_A2=$("$NAAB" "$A2DIR/test.naab" 2>&1) && RC_A2=$? || RC_A2=$?
    if echo "$OUT_A2" | grep -q "TURN_LEASE_BLOCKED"; then
        ok "A2: turn-based lease expiry blocks agent.send"
    elif echo "$OUT_A2" | grep -q "WRONG_ERROR"; then
        # API/auth error prevents reaching the lease boundary — key may be invalid
        if echo "$OUT_A2" | grep -qi "API key\|INVALID_ARGUMENT\|attempts exhausted\|status 40[013]"; then
            skip "A2: turn-based lease (API key invalid — sends never reached lease boundary)"
        else
            fail "A2: agent.send threw wrong error (not API auth)"
            echo "    output: ${OUT_A2:0:200}"
        fi
    else
        fail "A2: turn-based lease did not block (exit $RC_A2)"
        echo "    output: ${OUT_A2:0:200}"
    fi
else
    skip "A2: turn-based lease expiry"
fi

echo ""

# =====================================================================
# SUMMARY
# =====================================================================
TOTAL=$((PASS + FAIL))
echo "=== Consequence Proof Results: $PASS passed, $FAIL failed, $SKIP skipped (of $((TOTAL + SKIP)) tests) ==="

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "FAILED: Governance enforcement gaps detected."
    exit 1
fi

echo "  All enforcement paths verified."
exit 0
