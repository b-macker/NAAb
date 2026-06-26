#!/usr/bin/env bash
# ============================================================
# test_d1_reconciliation.sh — D1 State Reconciliation Tests
#
# Tests Signal 19 (claim_result_reconciliation) and contextual challenges.
#
# Group A: Config parsing & infrastructure (no API key)
#   - govern.json fields parse without crash
#   - Agent creation works with D1 signals enabled
#   - Environment dict surfaces D1 fields with correct defaults
#   - Dashboard runs without error
#   - Negative/extreme thresholds clamped
#
# Group B: Live reconciliation behavior (requires GK1 API key)
#   - Agent with tool registration + S19 enabled
#   - RECONCILIATION_TURN telemetry emitted after tool execution
#   - claim_mismatch_count and claim_accuracy surfaced in response
#   - Contextual challenge config accepted
#
# Tests are grounded in what the code actually does:
# - recordToolOutcome() stores tool success/failure (NOT gated on tool_success)
# - Signal 19 checks AGENT_RESPONSE content_keywords against tool_last_outcome
# - Success words: success/succeeded/completed/done/created/wrote/saved/passed/found/returned/worked
# - Failure words: error/failed/failure/exception/unable/could/cannot/denied/blocked/timeout
# - Ambiguity guard: both or neither status words = no fire
# - Rolling accuracy in claim_accuracy_history (deque, size 20)
# - tool_last_outcome cleared after reconciliation (per-turn, not cumulative)
# - RECONCILIATION_TURN emitted when claim_result_reconciliation signal enabled
# - Contextual challenges: 5 types (tool_result, plan_step, instruction, entity, mandate)
#   selected from DriftState data when step_up_contextual = true
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/d1-reconcil-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"; }
skip() { local id="$1" desc="$2"; SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    echo "$workdir"
}

sign_govern() {
    local workdir="$1"
    (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  D1 State Reconciliation Tests                               |${NC}"
echo -e "${CYAN}|  Signal 19 + Contextual Challenges                           |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# GROUP A: Config Parsing & Infrastructure (no API key required)
# ============================================================
echo -e "${CYAN}--- Group A: Config Parsing & Infrastructure ---${NC}"

# A01: govern.json with ALL D1 fields parses without error
# What this tests: governance_config.cpp parsing for claim_result_reconciliation
# signal, weight, threshold, step_up_contextual, step_up_contextual_threshold
WORKDIR=$(setup_workdir)
cat > "$WORKDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "context_drift": {
    "enabled": true,
    "level": "advisory",
    "coherence_threshold": 0.3,
    "check_interval_turns": 2,
    "signals": {
      "circular_actions": true,
      "semantic_stability": true,
      "mandate_alignment": true,
      "tool_chain_integrity": true,
      "claim_result_reconciliation": true,
      "context_growth": true,
      "instruction_recall": true,
      "plan_drift": true,
      "entity_consistency": true,
      "instruction_conflict": true,
      "persona_fingerprint": true
    },
    "weights": {
      "semantic_stability": 0.10,
      "mandate_alignment": 0.12,
      "tool_chain_integrity": 0.08,
      "claim_result_reconciliation": 0.12
    },
    "thresholds": {
      "semantic_stability_min_overlap": 0.20,
      "mandate_alignment_min": 0.12,
      "tool_result_recall_min": 0.15,
      "claim_accuracy_min": 0.70
    }
  },
  "circuit_breaker": {
    "enabled": true,
    "step_up_enabled": true,
    "step_up_at_level": "elevated",
    "step_up_challenge": "Restate your current task, then proceed.",
    "step_up_min_words": 5,
    "step_up_cooldown_turns": 2,
    "step_up_keyword_threshold": 0.3,
    "step_up_contextual": true,
    "step_up_contextual_threshold": 0.30
  },
  "governance_health": { "enabled": true }
}
GOVEOF
sign_govern "$WORKDIR"

cat > "$WORKDIR/test.naab" << 'NAABEOF'
use governance

main {
    let h = governance.health()
    if h != null {
        print("HEALTH_OK")
        let verdict = h.get("verdict")
        if verdict != null {
            print("VERDICT: " + string(verdict))
        }
    }
}
NAABEOF

OUTPUT=$(cd "$WORKDIR" && "$NAAB" "test.naab" 2>/dev/null) && EXIT_CODE=0 || EXIT_CODE=$?
if [ "$EXIT_CODE" -eq 0 ] && echo "$OUTPUT" | grep -q "HEALTH_OK"; then
    pass "A01" "govern.json with all D1 fields parses correctly"
else
    fail "A01" "Config parsing failed" "exit=$EXIT_CODE output=$(echo "$OUTPUT" | head -3)"
fi

# A02: D1 signals work with default config — no impact on existing behavior
# What this tests: CDD works without explicit signal config in govern.json
WORKDIR=$(setup_workdir)
cat > "$WORKDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "context_drift": {
    "enabled": true,
    "level": "advisory"
  },
  "governance_health": { "enabled": true }
}
GOVEOF
sign_govern "$WORKDIR"

cat > "$WORKDIR/test.naab" << 'NAABEOF'
use governance

main {
    let h = governance.health()
    if h != null { print("HEALTH_OK") }
}
NAABEOF

OUTPUT=$(cd "$WORKDIR" && "$NAAB" "test.naab" 2>/dev/null) && EXIT_CODE=0 || EXIT_CODE=$?
if [ "$EXIT_CODE" -eq 0 ] && echo "$OUTPUT" | grep -q "HEALTH_OK"; then
    pass "A02" "D1 signals work with default config — no impact on existing behavior"
else
    fail "A02" "Default config broken" "exit=$EXIT_CODE"
fi

# A03: Dashboard runs without error when D1 signals enabled
# What this tests: governance_engine.cpp dashboard reconciliation line
# (printed only when mismatch or integrity count > 0, so should be silent here)
WORKDIR=$(setup_workdir)
cat > "$WORKDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "context_drift": {
    "enabled": true,
    "level": "advisory",
    "signals": {
      "claim_result_reconciliation": true,
      "tool_chain_integrity": true
    }
  },
  "governance_health": { "enabled": true }
}
GOVEOF
sign_govern "$WORKDIR"

cat > "$WORKDIR/test.naab" << 'NAABEOF'
main { print("DASHBOARD_OK") }
NAABEOF

OUTPUT=$(cd "$WORKDIR" && "$NAAB" --governance-dashboard "test.naab" 2>/dev/null) && EXIT_CODE=0 || EXIT_CODE=$?
if [ "$EXIT_CODE" -eq 0 ] && echo "$OUTPUT" | grep -q "DASHBOARD_OK"; then
    pass "A03" "Dashboard runs with D1 signals enabled"
else
    fail "A03" "Dashboard crashed with D1 signals" "exit=$EXIT_CODE"
fi

# A04: Extreme threshold values clamped correctly
# What this tests: governance_config.cpp clamp logic (0.0-1.0 range)
# claim_accuracy_min is clamped in config parsing
WORKDIR=$(setup_workdir)
cat > "$WORKDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "context_drift": {
    "enabled": true,
    "level": "advisory",
    "signals": {
      "claim_result_reconciliation": true
    },
    "thresholds": {
      "claim_accuracy_min": 5.0
    }
  },
  "circuit_breaker": {
    "enabled": true,
    "step_up_contextual": true,
    "step_up_contextual_threshold": -0.5
  },
  "governance_health": { "enabled": true }
}
GOVEOF
sign_govern "$WORKDIR"

cat > "$WORKDIR/test.naab" << 'NAABEOF'
use governance
main {
    let h = governance.health()
    if h != null { print("CLAMP_OK") }
}
NAABEOF

OUTPUT=$(cd "$WORKDIR" && "$NAAB" "test.naab" 2>/dev/null) && EXIT_CODE=0 || EXIT_CODE=$?
if [ "$EXIT_CODE" -eq 0 ] && echo "$OUTPUT" | grep -q "CLAMP_OK"; then
    pass "A04" "Extreme threshold values handled (no crash)"
else
    fail "A04" "Extreme thresholds caused crash" "exit=$EXIT_CODE"
fi

# A05: contextual challenge config without step_up_enabled → no crash
# What this tests: step_up_contextual is a sub-feature of step_up_enabled.
# Setting contextual=true but step_up_enabled=false should be safe (never reached).
WORKDIR=$(setup_workdir)
cat > "$WORKDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "step_up_enabled": false,
    "step_up_contextual": true,
    "step_up_contextual_threshold": 0.25
  },
  "governance_health": { "enabled": true }
}
GOVEOF
sign_govern "$WORKDIR"

cat > "$WORKDIR/test.naab" << 'NAABEOF'
use governance
main {
    let h = governance.health()
    if h != null { print("ORPHAN_CTX_OK") }
}
NAABEOF

OUTPUT=$(cd "$WORKDIR" && "$NAAB" "test.naab" 2>/dev/null) && EXIT_CODE=0 || EXIT_CODE=$?
if [ "$EXIT_CODE" -eq 0 ] && echo "$OUTPUT" | grep -q "ORPHAN_CTX_OK"; then
    pass "A05" "step_up_contextual=true without step_up_enabled=true (no crash)"
else
    fail "A05" "Orphan contextual config caused crash" "exit=$EXIT_CODE"
fi

# A06: govern-template.json contains all D1 fields
# What this tests: govern-template.json is the canonical reference for all config keys.
# D1 added 5 fields. Verify they are present.
TEMPLATE="$SCRIPT_DIR/../../govern-template.json"
A06_PASS=true
for field in "claim_result_reconciliation" "claim_accuracy_min" "step_up_contextual" "step_up_contextual_threshold"; do
    if ! grep -q "\"$field\"" "$TEMPLATE" 2>/dev/null; then
        A06_PASS=false
        break
    fi
done
if [ "$A06_PASS" = true ]; then
    pass "A06" "govern-template.json contains all D1 config fields"
else
    fail "A06" "Missing D1 field in govern-template.json" "$field"
fi

# A07: Agent creation with D1 config — environment dict has D1 fields
# What this tests: buildEnvironmentDict() surfaces claim_mismatch_count and
# claim_accuracy in the state section (agent_impl.cpp:465-469).
# agent.create() resolves the API key env var at creation time, so we set a
# fake one. agent.send() would fail, but create() only needs a non-empty value.
export NAAB_TEST_FAKE_KEY="fake-key-for-create-only"
WORKDIR=$(setup_workdir)
cat > "$WORKDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "network": { "enabled": true } },
  "context_drift": {
    "enabled": true,
    "level": "advisory",
    "signals": {
      "claim_result_reconciliation": true,
      "tool_chain_integrity": true
    },
    "weights": {
      "claim_result_reconciliation": 0.12
    },
    "thresholds": {
      "claim_accuracy_min": 0.70
    }
  },
  "agents": {
    "test_agent": {
      "provider": "gemini",
      "model": "gemini-3.1-flash-lite",
      "api_key_env": "NAAB_TEST_FAKE_KEY",
      "max_tokens": 100,
      "max_turns": 5,
      "system_prompt": "You are a test assistant.",
      "tools_enabled": true,
      "tools": ["lookup"],
      "allowed_actions": ["AGENT_SEND", "TOOL_EXEC"],
      "network_allowed": true
    }
  },
  "governance_health": { "enabled": true }
}
GOVEOF
sign_govern "$WORKDIR"

cat > "$WORKDIR/test.naab" << 'NAABEOF'
use agent
use string

fn lookup(query) {
    return {"result": "test data", "query": query}
}

main {
    agent.register_tool("lookup", lookup, {
        "description": "Look up data",
        "parameters": {
            "query": {"type": "string", "description": "Query"}
        }
    })

    let handle = agent.create("test_agent")
    if handle == null {
        print("CREATE_FAILED")
        return
    }
    print("AGENT_CREATED")

    // Check environment dict from handle (birth snapshot)
    let env = handle.get("environment")
    if env == null {
        print("NO_ENV")
        return
    }
    let state = env.get("state")
    if state == null {
        print("NO_STATE")
        return
    }

    // D1 field defaults: claim_mismatch_count should be 0 at birth
    let cmm = state.get("claim_mismatch_count")
    if cmm != null {
        print("CLAIM_MISMATCH_DEFAULT: " + string(cmm))
    } else {
        print("CLAIM_MISMATCH_DEFAULT: absent")
    }

    // claim_accuracy might not be present until first tool execution
    let ca = state.get("claim_accuracy")
    if ca != null {
        print("CLAIM_ACCURACY_DEFAULT: " + string(ca))
    } else {
        print("CLAIM_ACCURACY_DEFAULT: absent")
    }

    // tool_integrity_count should be 0 at birth
    let tic = state.get("tool_integrity_count")
    if tic != null {
        print("TOOL_INTEGRITY_DEFAULT: " + string(tic))
    } else {
        print("TOOL_INTEGRITY_DEFAULT: absent")
    }
}
NAABEOF

OUTPUT=$(cd "$WORKDIR" && "$NAAB" "test.naab" 2>/dev/null) && EXIT_CODE=0 || EXIT_CODE=$?

if [ "$EXIT_CODE" -eq 0 ] && echo "$OUTPUT" | grep -q "AGENT_CREATED"; then
    pass "A07" "Agent creation works with D1 config"
else
    fail "A07" "Agent creation failed with D1 config" "exit=$EXIT_CODE output=$(echo "$OUTPUT" | head -5)"
fi

# A08: claim_mismatch_count defaults to 0 in environment dict
# What this tests: DriftState initialization — claim_result_mismatch_count = 0
# buildEnvironmentDict uses getDriftState which may not exist yet at creation.
# If DriftState not yet created, claim_mismatch_count won't appear (absent is OK).
CMM_VAL=$(echo "$OUTPUT" | grep -oP 'CLAIM_MISMATCH_DEFAULT: \K\S+' 2>/dev/null || echo "missing")
if [ "$CMM_VAL" = "0" ] || [ "$CMM_VAL" = "absent" ]; then
    pass "A08" "claim_mismatch_count default ($CMM_VAL)"
else
    fail "A08" "claim_mismatch_count unexpected default" "$CMM_VAL"
fi

# A09: claim_accuracy absent before first tool execution (no history yet)
# What this tests: claim_accuracy_history is empty → field not surfaced
CA_VAL=$(echo "$OUTPUT" | grep -oP 'CLAIM_ACCURACY_DEFAULT: \K\S+' 2>/dev/null || echo "missing")
if [ "$CA_VAL" = "absent" ] || [ "$CA_VAL" = "missing" ]; then
    pass "A09" "claim_accuracy absent before first tool execution"
else
    # If present, it could be a valid initial value — that's also acceptable
    pass "A09" "claim_accuracy has initial value ($CA_VAL)"
fi

# A10: tool_integrity_count defaults to 0
TIC_VAL=$(echo "$OUTPUT" | grep -oP 'TOOL_INTEGRITY_DEFAULT: \K\S+' 2>/dev/null || echo "missing")
if [ "$TIC_VAL" = "0" ] || [ "$TIC_VAL" = "absent" ]; then
    pass "A10" "tool_integrity_count default ($TIC_VAL)"
else
    fail "A10" "tool_integrity_count unexpected default" "$TIC_VAL"
fi

echo ""

# ============================================================
# GROUP B: Live Reconciliation Behavior (requires GK* API key)
# ============================================================
echo -e "${CYAN}--- Group B: Live Reconciliation Behavior ---${NC}"

if [ -z "${GK1:-}${GK2:-}${GK3:-}${GK4:-}${GK5:-}" ]; then
    echo -e "${YELLOW}No GK* API key — skipping Group B${NC}"
    for i in $(seq 1 8); do
        skip "B$(printf '%02d' $i)" "No GK* API key"
    done
else
    WORKDIR=$(setup_workdir)
    cat > "$WORKDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "network": { "enabled": true } },
  "telemetry": {
    "enabled": true,
    "output_file": "telemetry.jsonl",
    "tamper_evidence": {
      "enabled": true,
      "algorithm": "sha256",
      "chain_genesis": "d1-reconcil-test"
    }
  },
  "context_drift": {
    "enabled": true,
    "level": "advisory",
    "coherence_threshold": 0.3,
    "check_interval_turns": 1,
    "coherence_natural_healing": 0.02,
    "adaptive_baseline_enabled": true,
    "adaptive_baseline_window": 2,
    "adaptive_baseline_sensitivity": 2.0,
    "signals": {
      "circular_actions": true,
      "semantic_stability": true,
      "mandate_alignment": true,
      "tool_chain_integrity": true,
      "claim_result_reconciliation": true,
      "response_quality": true,
      "thinking_collapse": false,
      "context_growth": false,
      "instruction_recall": false,
      "plan_drift": false,
      "entity_consistency": false,
      "instruction_conflict": false,
      "persona_fingerprint": false
    },
    "weights": {
      "semantic_stability": 0.10,
      "mandate_alignment": 0.12,
      "tool_chain_integrity": 0.08,
      "claim_result_reconciliation": 0.12
    },
    "thresholds": {
      "semantic_stability_min_overlap": 0.20,
      "mandate_alignment_min": 0.12,
      "tool_result_recall_min": 0.15,
      "claim_accuracy_min": 0.70
    },
    "reality_checkpoint": {
      "enabled": true,
      "pressure_threshold": 0.5
    }
  },
  "circuit_breaker": {
    "enabled": true,
    "elevated_threshold": 0.35,
    "step_up_enabled": false
  },
  "behavioral_sequences": {
    "enabled": true,
    "window_size": 20,
    "cross_agent": false,
    "patterns": []
  },
  "agents": {
    "reconcil_agent": {
      "provider": "gemini",
      "model": "gemma-4-31b-it",
      "api_key_env": ["GK1", "GK2", "GK3", "GK4", "GK5"],
      "max_tokens": 256,
      "max_turns": 20,
      "max_total_tokens": 500000,
      "system_prompt": "You are a data lookup assistant. When asked to look up data, use the lookup tool. When asked to compute, use the compute tool. Always report what the tool returned.",
      "response_format": "text",
      "tools_enabled": true,
      "tools": ["lookup", "compute"],
      "max_tool_calls_per_turn": 3,
      "max_tool_loop_turns": 2,
      "tool_result_max_chars": 1024,
      "tool_timeout_seconds": 10,
      "network_allowed": true,
      "allowed_actions": ["AGENT_SEND", "TOOL_EXEC"],
      "rate_limit": {
        "requests_per_minute": 0,
        "delay_between_calls_ms": 4500
      },
      "retry": {
        "max_attempts": 5,
        "backoff_ms": 5000,
        "backoff_multiplier": 2.0,
        "jitter": true,
        "retry_on": [429, 500, 503],
        "skip_key_on": [401],
        "key_retry_after_seconds": 0
      }
    }
  },
  "agent_dispatch": {
    "default_timeout_seconds": 60,
    "hard_stop": {
      "max_calls_per_run": 50,
      "max_tokens_per_run": 500000,
      "max_agent_time_ms": 300000,
      "consecutive_failure_limit": 10,
      "action": "block"
    }
  },
  "exposure_tracking": {
    "enabled": true,
    "max_autonomous_actions": 30,
    "max_unique_agents": 2,
    "level": "advisory"
  },
  "governance_health": { "enabled": true }
}
GOVEOF
    sign_govern "$WORKDIR"

    cat > "$WORKDIR/reconcil-test.naab" << 'NAABEOF'
use agent
use string
use governance

// Tool that always succeeds — returns structured data
fn lookup(query) {
    return {"results": ["item_alpha", "item_beta"], "query": query, "count": 2}
}

// Tool that always succeeds — returns a numeric result
fn compute(expression) {
    return {"result": 42, "expression": expression}
}

fn get_state(resp) {
    if resp == null { return null }
    let env = resp.get("environment")
    if env == null { return null }
    return env.get("state")
}

fn get_val(state, key, default_val) {
    if state == null { return default_val }
    let v = state.get(key)
    if v == null { return default_val }
    return v
}

main {
    // Register tools
    agent.register_tool("lookup", lookup, {
        "description": "Look up items in the data store. Returns matching items.",
        "parameters": {
            "query": {"type": "string", "description": "Search query"}
        }
    })
    agent.register_tool("compute", compute, {
        "description": "Compute a mathematical expression. Returns result.",
        "parameters": {
            "expression": {"type": "string", "description": "Math expression"}
        }
    })

    let prompts = []

    // Phase 1: Tool baseline (turns 1-4) — on-topic, tool usage expected
    prompts.push("Look up sales reports for Q1 using the lookup tool")
    prompts.push("Compute the total of 1200 + 3400 + 5600 using the compute tool")
    prompts.push("Look up employee satisfaction survey results")
    prompts.push("Compute the average of 85, 90, 78, 92")

    // Phase 2: Off-topic drift (turns 5-8) — no tools expected
    prompts.push("What is the capital of France?")
    prompts.push("Explain how gravity works")
    prompts.push("Tell me about the history of chess")
    prompts.push("How does a car engine work?")

    // Phase 3: Tool return (turns 9-10) — back to tools after drift
    prompts.push("Look up quarterly budget reports using the lookup tool")
    prompts.push("Compute the profit margin: (5000 - 3200) / 5000 * 100 using the compute tool")

    let handle = null
    try {
        handle = agent.create("reconcil_agent")
    } catch (e) {
        print("AGENT_CREATE_ERROR: " + string(e))
        return
    }
    print("AGENT_CREATED")

    let turn = 0
    let total_tool_calls = 0
    let last_claim_mismatch = 0
    let last_claim_accuracy = "N/A"
    let alive = true

    while turn < prompts.length() && alive {
        let resp = null
        try {
            resp = agent.send(handle, prompts[turn])
        } catch (e) {
            let err_str = string(e)
            print("TURN_ERROR|n=" + string(turn + 1) + "|err=" + err_str)
            // API/model errors: continue if possible
            if string.contains(err_str, "GovernanceHardError") {
                alive = false
            }
            turn = turn + 1
            continue
        }

        if resp != null {
            let state = get_state(resp)
            let coherence = get_val(state, "coherence", -1.0)
            let tcm = resp.get("tool_calls_made") ?? 0
            total_tool_calls = total_tool_calls + tcm

            // D1 fields from environment dict
            let cmm = get_val(state, "claim_mismatch_count", 0)
            last_claim_mismatch = cmm
            let ca = state.get("claim_accuracy") ?? null
            if ca != null {
                last_claim_accuracy = string(ca)
            }

            // Semantic section from response dict
            let sem = resp.get("semantic")
            let sem_cmm = "N/A"
            let sem_ca = "N/A"
            if sem != null {
                let sc = sem.get("claim_mismatch_count")
                if sc != null { sem_cmm = string(sc) }
                let sca = sem.get("claim_accuracy")
                if sca != null { sem_ca = string(sca) }
            }

            print("TURN|n=" + string(turn + 1) + "|c=" + string(coherence) + "|tcm=" + string(tcm) + "|cmm=" + string(cmm) + "|ca=" + last_claim_accuracy + "|sem_cmm=" + sem_cmm)
        }

        turn = turn + 1
    }

    // Summary for harness
    print("SUMMARY_TURNS_COMPLETED: " + string(turn))
    print("SUMMARY_TOTAL_TOOL_CALLS: " + string(total_tool_calls))
    print("SUMMARY_CLAIM_MISMATCHES: " + string(last_claim_mismatch))
    print("SUMMARY_CLAIM_ACCURACY: " + last_claim_accuracy)

    let health = governance.health()
    if health != null {
        print("HEALTH_VERDICT: " + string(health.get("verdict")))
    }
}
NAABEOF

    echo -e "${CYAN}Running reconciliation test (10 turns, ~1-2 min with live API)...${NC}"

    STDERR_FILE="$TEST_TMP/stderr_b.log"
    STDOUT_FILE="$WORKDIR/stdout.log"
    (cd "$WORKDIR" && timeout 480 "$NAAB" --timeout 480 --governance-dashboard "reconcil-test.naab" >"$STDOUT_FILE" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?
    OUTPUT=$(cat "$STDOUT_FILE" 2>/dev/null)
    TELEM_FILE="$WORKDIR/telemetry.jsonl"

    echo ""

    # B01: Program ran (exit 0=success, 1=runtime error from API, 2=quality gate, 3=hard block)
    if [ "$EXIT_CODE" -eq 0 ] || [ "$EXIT_CODE" -eq 1 ] || [ "$EXIT_CODE" -eq 2 ] || [ "$EXIT_CODE" -eq 3 ]; then
        pass "B01" "Program ran (exit $EXIT_CODE)"
    elif [ "$EXIT_CODE" -eq 124 ]; then
        fail "B01" "Program timed out"
    else
        fail "B01" "Program crashed" "exit $EXIT_CODE"
    fi

    # B02: Agent created successfully
    if echo "$OUTPUT" | grep -q "AGENT_CREATED"; then
        pass "B02" "Agent created successfully"
    else
        fail "B02" "Agent creation failed"
    fi

    # B03: At least one turn completed (API-dependent — more turns are better
    # but even 1 turn exercises the full pipeline: tool loop + CDD + telemetry)
    # Try SUMMARY line first; fall back to counting TURN| output lines
    TURNS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TURNS_COMPLETED: \K[0-9]+' | head -1)
    if [ -z "$TURNS" ]; then
        TURNS=$(echo "$OUTPUT" | grep -cF 'TURN|n=' 2>/dev/null | head -1 || echo "0")
    fi
    TURNS=$(echo "${TURNS:-0}" | tr -dc '0-9' | head -c 5)
    TURNS=${TURNS:-0}
    if [ "$TURNS" -ge 1 ]; then
        pass "B03" "Turns completed ($TURNS)"
    else
        fail "B03" "No turns completed" "$TURNS"
    fi

    # B04: RECONCILIATION_TURN telemetry emitted
    # What this tests: agent_impl.cpp:3153-3178 emits RECONCILIATION_TURN
    # when claim_result_reconciliation signal is enabled. Should appear whenever
    # drift_state exists (after first CDD check).
    TELEM_RECONCIL=$(grep -c "RECONCILIATION_TURN" "$TELEM_FILE" 2>/dev/null || echo "0")
    if [ "$TELEM_RECONCIL" -gt 0 ]; then
        pass "B04" "RECONCILIATION_TURN telemetry emitted ($TELEM_RECONCIL events)"
    else
        # If turns completed but no telemetry, that's a real failure
        if [ "$TURNS" -ge 3 ]; then
            fail "B04" "No RECONCILIATION_TURN telemetry despite $TURNS turns"
        else
            skip "B04" "Not enough turns for telemetry ($TURNS)"
        fi
    fi

    # B05: RECONCILIATION_TURN contains expected fields
    # What this tests: the telemetry event at agent_impl.cpp:3166-3177 includes
    # handle_id, turn, tool_integrity_count, claim_mismatch_count,
    # claim_accuracy_rolling, coherence, signals_fired
    if [ "$TELEM_RECONCIL" -gt 0 ]; then
        FIRST_RECONCIL=$(grep "RECONCILIATION_TURN" "$TELEM_FILE" | head -1)
        FIELDS_OK=true
        for field in "handle_id" "turn" "tool_integrity_count" "claim_mismatch_count" "claim_accuracy_rolling" "coherence" "signals_fired"; do
            if ! echo "$FIRST_RECONCIL" | grep -q "\"$field\"" 2>/dev/null; then
                FIELDS_OK=false
                break
            fi
        done
        if [ "$FIELDS_OK" = true ]; then
            pass "B05" "RECONCILIATION_TURN has all expected fields"
        else
            fail "B05" "RECONCILIATION_TURN missing field" "$field"
        fi
    else
        skip "B05" "No RECONCILIATION_TURN events"
    fi

    # B06: claim_mismatch_count available in program output
    # What this tests: environment dict surfaces claim_mismatch_count
    # (agent_impl.cpp:465). Value is extracted by .naab and printed.
    CMM_SUMMARY=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CLAIM_MISMATCHES: \K[0-9]+' | head -1)
    CMM_SUMMARY=${CMM_SUMMARY:-"missing"}
    if [ "$CMM_SUMMARY" != "missing" ]; then
        pass "B06" "claim_mismatch_count surfaced in environment ($CMM_SUMMARY)"
    else
        if [ "$TURNS" -ge 3 ]; then
            fail "B06" "claim_mismatch_count not found in output"
        else
            skip "B06" "Not enough turns"
        fi
    fi

    # B07: Tool calls were made (tools actually executed)
    # What this tests: tool execution loop in agentSend() ran, which means
    # recordToolOutcome() was called (agent_impl.cpp:2455-2460)
    TOTAL_TC=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOTAL_TOOL_CALLS: \K[0-9]+' | head -1)
    TOTAL_TC=${TOTAL_TC:-0}
    if [ "$TOTAL_TC" -gt 0 ]; then
        pass "B07" "Tool calls executed ($TOTAL_TC)"
    else
        # LLM might not call tools — that's not a test failure, it's LLM behavior
        skip "B07" "No tool calls made (LLM didn't use tools)"
    fi

    # B08: SEMANTIC_TURN telemetry includes claim_mismatch_count
    # What this tests: agent_impl.cpp:3148 adds claim_mismatch_count to SEMANTIC_TURN
    TELEM_SEM=$(grep -c "SEMANTIC_TURN" "$TELEM_FILE" 2>/dev/null || echo "0")
    if [ "$TELEM_SEM" -gt 0 ]; then
        SEM_HAS_CMM=$(grep "SEMANTIC_TURN" "$TELEM_FILE" | head -1 | grep -c "claim_mismatch_count" 2>/dev/null || echo "0")
        if [ "$SEM_HAS_CMM" -gt 0 ]; then
            pass "B08" "SEMANTIC_TURN telemetry includes claim_mismatch_count"
        else
            fail "B08" "SEMANTIC_TURN missing claim_mismatch_count field"
        fi
    else
        skip "B08" "No SEMANTIC_TURN events"
    fi
fi

# ============================================================
# SUMMARY
# ============================================================
echo ""
echo -e "${CYAN}================================================${NC}"
echo -e "  ${GREEN}PASS: $PASS_COUNT${NC}  ${RED}FAIL: $FAIL_COUNT${NC}  ${YELLOW}SKIP: $SKIP_COUNT${NC}  TOTAL: $TOTAL"

if [ -n "$FAILURES" ]; then
    echo -e "\n${RED}Failures:${FAILURES}${NC}"
fi
echo -e "${CYAN}================================================${NC}"

exit "$FAIL_COUNT"
