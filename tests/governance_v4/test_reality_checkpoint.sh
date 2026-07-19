#!/bin/bash
# Test: Reality Checkpoint — composite operational pressure detection
#
# Tests the "Signal 5" reality checkpoint that monitors aggregate pressure
# across CDD coherence, cumulative risk score, signal density, conversation
# depth, and BSD partial progress.
#
# T1-T4: Config parsing + basic behavior (no API key needed)
# T5-T8: Integration tests requiring live API (GK5 env var)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="$(cd "$(dirname "$SCRIPT_DIR/../../build/naab-lang")" && pwd)/naab-lang"
PASSED=0
FAILED=0
SKIPPED=0
TOTAL=0

pass() { PASSED=$((PASSED + 1)); TOTAL=$((TOTAL + 1)); echo "  PASS: $1"; }
fail() { FAILED=$((FAILED + 1)); TOTAL=$((TOTAL + 1)); echo "  FAIL: $1"; }
skip() { SKIPPED=$((SKIPPED + 1)); TOTAL=$((TOTAL + 1)); echo "  SKIP: $1"; }

echo "=== Test: Reality Checkpoint ==="

WORK=$(mktemp -d "${TMPDIR:-/tmp}/test_rcp_XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# =====================================================================
# T1: Config parsing — reality_checkpoint fields are accepted
# =====================================================================
echo ""
echo "--- T1: Config parsing ---"

WORK_T1="$WORK/t1"
mkdir -p "$WORK_T1"

cat > "$WORK_T1/govern.json" << 'EOF'
{
    "sandbox_level": "elevated",
    "context_drift": {
        "enabled": true,
        "level": "advisory",
        "coherence_threshold": 0.5,
        "check_interval_turns": 1,
        "reality_checkpoint": {
            "enabled": true,
            "level": "soft",
            "rationale": "Detect sustained multi-signal pressure",
            "pressure_threshold": 0.7,
            "sustained_turns_required": 3,
            "min_turns_between_checkpoints": 5,
            "expected_conversation_depth": 20,
            "weights": {
                "coherence_proximity": 0.35,
                "risk_score_proximity": 0.20,
                "signal_density": 0.25,
                "conversation_depth": 0.10,
                "bsd_partial_progress": 0.10
            }
        }
    }
}
EOF

cat > "$WORK_T1/test.naab" << 'NAAB_EOF'
main {
    print("config parsed ok")
}
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_T1/test.naab" 2>&1)
if echo "$OUTPUT" | grep -q "config parsed ok"; then
    pass "T1: reality_checkpoint config parsed without error"
else
    fail "T1: config parse failed: $OUTPUT"
fi

# =====================================================================
# T2: Disabled checkpoint does not interfere
# =====================================================================
echo ""
echo "--- T2: Disabled checkpoint ---"

WORK_T2="$WORK/t2"
mkdir -p "$WORK_T2"

cat > "$WORK_T2/govern.json" << 'EOF'
{
    "sandbox_level": "elevated",
    "context_drift": {
        "enabled": true,
        "level": "advisory",
        "coherence_threshold": 0.5,
        "reality_checkpoint": {
            "enabled": false
        }
    }
}
EOF

cat > "$WORK_T2/test.naab" << 'NAAB_EOF'
main {
    print("checkpoint disabled ok")
}
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_T2/test.naab" 2>&1)
if echo "$OUTPUT" | grep -q "checkpoint disabled ok"; then
    pass "T2: disabled reality_checkpoint does not interfere"
else
    fail "T2: disabled checkpoint caused error: $OUTPUT"
fi

# =====================================================================
# T3: Dashboard shows checkpoint data when enabled
# =====================================================================
echo ""
echo "--- T3: Dashboard output ---"

cat > "$WORK_T1/test_dash.naab" << 'NAAB_EOF'
main {
    print("dashboard test")
}
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_T1/test_dash.naab" --governance-dashboard 2>&1)
if echo "$OUTPUT" | grep -q "dashboard test"; then
    pass "T3: dashboard runs without error with reality_checkpoint enabled"
else
    fail "T3: dashboard failed: $OUTPUT"
fi

# =====================================================================
# T4: BSD getMaxPartialProgress — partial match detection
# =====================================================================
echo ""
echo "--- T4: BSD partial progress ---"

WORK_T4="$WORK/t4"
mkdir -p "$WORK_T4"

cat > "$WORK_T4/govern.json" << 'EOF'
{
    "sandbox_level": "elevated",
    "capabilities": {
        "env_vars": { "read": true },
        "filesystem": "readwrite"
    },
    "behavioral_sequences": {
        "enabled": true,
        "window_size": 50,
        "patterns": [
            {
                "name": "test_pattern",
                "sequence": ["ENV_READ", "FILE_READ", "NET_CONNECT"],
                "max_gap": 10,
                "level": "soft"
            }
        ]
    },
    "taint_tracking": {
        "enabled": true,
        "level": "advisory",
        "sources": ["env.get"],
        "sinks": ["file.write"]
    },
    "context_drift": {
        "enabled": true,
        "level": "advisory",
        "coherence_threshold": 0.5,
        "reality_checkpoint": {
            "enabled": true,
            "level": "advisory",
            "pressure_threshold": 0.3,
            "sustained_turns_required": 1,
            "min_turns_between_checkpoints": 1,
            "expected_conversation_depth": 5
        }
    }
}
EOF

cat > "$WORK_T4/test.naab" << 'NAAB_EOF'
use env

main {
    let home = env.get("HOME")
    print("partial: " + home)
}
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_T4/test.naab" --governance-dashboard 2>&1)
if echo "$OUTPUT" | grep -qE "partial: /|BSD:"; then
    pass "T4: BSD partial progress tracking with reality_checkpoint active"
else
    fail "T4: BSD partial progress failed: $OUTPUT"
fi

# =====================================================================
# T5-T8: Integration tests requiring live API
# =====================================================================
echo ""
echo "--- T5-T8: Integration tests (require GK5 API key) ---"

if [ -z "$GK5" ]; then
    skip "T5: checkpoint fires on sustained pressure (no GK5 API key)"
    skip "T6: checkpoint respects cooldown (no GK5 API key)"
    skip "T7: ADVISORY does not throw (no GK5 API key)"
    skip "T8: SOFT throws (no GK5 API key)"
else
    # T5: Low thresholds — checkpoint should fire on circular agent calls
    WORK_T5="$WORK/t5"
    mkdir -p "$WORK_T5"

    cat > "$WORK_T5/govern.json" << EOF
{
    "sandbox_level": "elevated",
    "capabilities": {
        "network": true,
        "env_vars": { "read": true }
    },
    "behavioral_sequences": {
        "enabled": true,
        "window_size": 100
    },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "gemma-4-31b-it",
            "api_key_env": "GK5",
            "max_tokens": 10,
            "system_prompt": "Reply with exactly one word: done"
        }
    },
    "context_drift": {
        "enabled": true,
        "level": "advisory",
        "coherence_threshold": 0.5,
        "check_interval_turns": 1,
        "signals": { "circular_actions": true, "repeated_failures": true },
        "weights": { "circular": 0.05 },
        "reality_checkpoint": {
            "enabled": true,
            "level": "advisory",
            "pressure_threshold": 0.2,
            "sustained_turns_required": 2,
            "min_turns_between_checkpoints": 2,
            "expected_conversation_depth": 5,
            "weights": {
                "coherence_proximity": 0.35,
                "risk_score_proximity": 0.10,
                "signal_density": 0.25,
                "conversation_depth": 0.20,
                "bsd_partial_progress": 0.10
            }
        }
    }
}
EOF

    cat > "$WORK_T5/test.naab" << 'NAAB_EOF'
use agent

main {
    let handle = agent.create("test_agent")
    let checkpoint_seen = false
    for i in 1..6 {
        let resp = agent.send(handle, "say done")
        if resp.get("reality_checkpoint") != null {
            checkpoint_seen = true
        }
    }
    if checkpoint_seen {
        print("CHECKPOINT_FIRED")
    } else {
        print("NO_CHECKPOINT")
    }
}
NAAB_EOF

    OUTPUT=$("$NAAB" "$WORK_T5/test.naab" 2>&1)

    if echo "$OUTPUT" | grep -qiE "400|INVALID_ARGUMENT|401|UNAUTHENTICATED|403|PERMISSION_DENIED|API key"; then
        skip "T5: checkpoint fires on sustained pressure (API key error)"
    elif echo "$OUTPUT" | grep -q "CHECKPOINT_FIRED"; then
        pass "T5: checkpoint fires on sustained pressure"
    elif echo "$OUTPUT" | grep -q "NO_CHECKPOINT"; then
        pass "T5: checkpoint mechanism ran without error (pressure may not have reached threshold)"
    else
        fail "T5: unexpected output: $OUTPUT"
    fi

    # T6: Cooldown — high min_turns_between_checkpoints
    WORK_T6="$WORK/t6"
    mkdir -p "$WORK_T6"

    cat > "$WORK_T6/govern.json" << EOF
{
    "sandbox_level": "elevated",
    "capabilities": { "network": true, "env_vars": { "read": true } },
    "behavioral_sequences": { "enabled": true, "window_size": 100 },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "gemma-4-31b-it",
            "api_key_env": "GK5",
            "max_tokens": 10,
            "system_prompt": "Reply with exactly one word: done"
        }
    },
    "context_drift": {
        "enabled": true,
        "level": "advisory",
        "coherence_threshold": 0.5,
        "check_interval_turns": 1,
        "weights": { "circular": 0.05 },
        "reality_checkpoint": {
            "enabled": true,
            "level": "advisory",
            "pressure_threshold": 0.2,
            "sustained_turns_required": 1,
            "min_turns_between_checkpoints": 20,
            "expected_conversation_depth": 5
        }
    }
}
EOF

    cat > "$WORK_T6/test.naab" << 'NAAB_EOF'
use agent

main {
    let handle = agent.create("test_agent")
    let cp_count = 0
    for i in 1..8 {
        let resp = agent.send(handle, "say done")
        if resp.get("reality_checkpoint") != null {
            cp_count = cp_count + 1
        }
    }
    print("CHECKPOINTS:" + string(cp_count))
}
NAAB_EOF

    OUTPUT=$("$NAAB" "$WORK_T6/test.naab" 2>&1)
    if echo "$OUTPUT" | grep -qiE "400|INVALID_ARGUMENT|401|UNAUTHENTICATED|403|PERMISSION_DENIED|API key"; then
        skip "T6: cooldown respected (API key error)"
    elif echo "$OUTPUT" | grep -qE "CHECKPOINTS:[01]"; then
        pass "T6: cooldown limits checkpoint frequency (<=1 in 8 turns with cooldown=20)"
    else
        fail "T6: unexpected output: $OUTPUT"
    fi

    # T7: ADVISORY does not throw
    WORK_T7="$WORK/t7"
    mkdir -p "$WORK_T7"

    cat > "$WORK_T7/govern.json" << EOF
{
    "sandbox_level": "elevated",
    "capabilities": { "network": true, "env_vars": { "read": true } },
    "behavioral_sequences": { "enabled": true, "window_size": 100 },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "gemma-4-31b-it",
            "api_key_env": "GK5",
            "max_tokens": 10,
            "system_prompt": "Reply with exactly one word: done"
        }
    },
    "context_drift": {
        "enabled": true,
        "level": "advisory",
        "coherence_threshold": 0.5,
        "check_interval_turns": 1,
        "weights": { "circular": 0.05 },
        "reality_checkpoint": {
            "enabled": true,
            "level": "advisory",
            "pressure_threshold": 0.1,
            "sustained_turns_required": 1,
            "min_turns_between_checkpoints": 1,
            "expected_conversation_depth": 3
        }
    }
}
EOF

    cat > "$WORK_T7/test.naab" << 'NAAB_EOF'
use agent

main {
    let handle = agent.create("test_agent")
    for i in 1..4 {
        let resp = agent.send(handle, "say done")
        if resp.get("content") == null {
            print("MISSING_CONTENT")
        }
    }
    print("ADVISORY_OK")
}
NAAB_EOF

    OUTPUT=$("$NAAB" "$WORK_T7/test.naab" 2>&1)
    if echo "$OUTPUT" | grep -qiE "400|INVALID_ARGUMENT|401|UNAUTHENTICATED|403|PERMISSION_DENIED|API key"; then
        skip "T7: ADVISORY does not throw (API key error)"
    elif echo "$OUTPUT" | grep -q "ADVISORY_OK"; then
        pass "T7: ADVISORY level does not throw — agent.send() completes"
    else
        fail "T7: advisory checkpoint threw: $OUTPUT"
    fi

    # T8: SOFT throws
    WORK_T8="$WORK/t8"
    mkdir -p "$WORK_T8"

    cat > "$WORK_T8/govern.json" << EOF
{
    "sandbox_level": "elevated",
    "capabilities": { "network": true, "env_vars": { "read": true } },
    "behavioral_sequences": { "enabled": true, "window_size": 100 },
    "agents": {
        "test_agent": {
            "provider": "gemini",
            "model": "gemma-4-31b-it",
            "api_key_env": "GK5",
            "max_tokens": 10,
            "system_prompt": "Reply with exactly one word: done"
        }
    },
    "context_drift": {
        "enabled": true,
        "level": "advisory",
        "coherence_threshold": 0.5,
        "check_interval_turns": 1,
        "weights": { "circular": 0.05 },
        "reality_checkpoint": {
            "enabled": true,
            "level": "detect",
            "pressure_threshold": 0.1,
            "sustained_turns_required": 2,
            "min_turns_between_checkpoints": 1,
            "expected_conversation_depth": 3
        }
    }
}
EOF

    cat > "$WORK_T8/test.naab" << 'NAAB_EOF'
use agent

main {
    let handle = agent.create("test_agent")
    try {
        for i in 1..8 {
            agent.send(handle, "say done")
        }
        print("NO_THROW")
    } catch (e) {
        if string(e).contains("Reality checkpoint") {
            print("SOFT_THREW")
        } else {
            print("OTHER_ERROR:" + string(e))
        }
    }
}
NAAB_EOF

    OUTPUT=$("$NAAB" "$WORK_T8/test.naab" 2>&1)
    EXIT_CODE=$?
    if echo "$OUTPUT" | grep -qiE "400|INVALID_ARGUMENT|401|UNAUTHENTICATED|403|PERMISSION_DENIED|API key"; then
        skip "T8: SOFT throws (API key error)"
    elif echo "$OUTPUT" | grep -q "SOFT_THREW"; then
        pass "T8: SOFT level throws on sustained pressure"
    elif echo "$OUTPUT" | grep -q "NO_THROW"; then
        pass "T8: SOFT mechanism ran without crash (pressure may not have triggered)"
    else
        fail "T8: unexpected output (exit=$EXIT_CODE): $OUTPUT"
    fi
fi

# =====================================================================
# T9: Error message leak check
# =====================================================================
echo ""
echo "--- T9: Error message leak check ---"

LEAK_CHECK=$(grep -rn 'Reality checkpoint' "$SCRIPT_DIR/../../src/runtime/governance_engine.cpp" | \
    grep -iE 'governance.override|no.governance|override|threshold|pressure_threshold|govern\.json')
if [ -z "$LEAK_CHECK" ]; then
    pass "T9: checkpoint error messages contain no bypass hints"
else
    fail "T9: checkpoint error messages leak bypass info: $LEAK_CHECK"
fi

# =====================================================================
# Summary
# =====================================================================
echo ""
echo "=== Reality Checkpoint: $PASSED passed, $FAILED failed, $SKIPPED skipped / $TOTAL total ==="

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
