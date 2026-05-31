#!/usr/bin/env bash
# Agent Governance Depth Tests
# Fix A: Agent response auto-taint (setLastReturnTainted in all agent functions)
# Fix B: Pipeline pressure inheritance (inherited_pressure, pipeline_inherited weight)
# Fix C: BSD agent identity (agent_handle/agent_config on RuntimeEvent, cross_agent patterns)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_AGENT="$SCRIPT_DIR/../../src/stdlib/agent_impl.cpp"
SRC_ENGINE="$SCRIPT_DIR/../../src/runtime/governance_engine.cpp"
SRC_BSD="$SCRIPT_DIR/../../src/runtime/behavioral_sequence.cpp"
SRC_CONFIG="$SCRIPT_DIR/../../src/runtime/governance_config.cpp"
SRC_INIT="$SCRIPT_DIR/../../src/cli/governance_init.cpp"
HDR_BSD="$SCRIPT_DIR/../../include/naab/behavioral_sequence.h"
HDR_GOV="$SCRIPT_DIR/../../include/naab/governance.h"

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

# ══════════════════════════════════════════════════════════════════════════════
echo "=== Fix A: Agent Response Auto-Taint ==="
# ══════════════════════════════════════════════════════════════════════════════

# T1: agentSend calls setLastReturnTainted
if grep -q 'setLastReturnTainted(true, "agent.send")' "$SRC_AGENT"; then
    pass "T1: agentSend calls setLastReturnTainted"
else
    fail "T1: agentSend missing setLastReturnTainted"
fi

# T2: agentRun calls setLastReturnTainted
if grep -q 'setLastReturnTainted(true, "agent.run")' "$SRC_AGENT"; then
    pass "T2: agentRun calls setLastReturnTainted"
else
    fail "T2: agentRun missing setLastReturnTainted"
fi

# T3: agentBatch calls setLastReturnTainted
if grep -q 'setLastReturnTainted(true, "agent.batch")' "$SRC_AGENT"; then
    pass "T3: agentBatch calls setLastReturnTainted"
else
    fail "T3: agentBatch missing setLastReturnTainted"
fi

# T4: agentFanOut delegates to agentBatch (gets taint free)
if grep -q 'agentBatch' "$SRC_AGENT"; then
    pass "T4: agentFanOut delegates to agentBatch (inherits taint)"
else
    fail "T4: agentFanOut does not delegate to agentBatch"
fi

# T5: agentPipeline calls setLastReturnTainted
if grep -q 'setLastReturnTainted(true, "agent.pipeline")' "$SRC_AGENT"; then
    pass "T5: agentPipeline calls setLastReturnTainted"
else
    fail "T5: agentPipeline missing setLastReturnTainted"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Fix B: Pipeline Pressure Inheritance ==="
# ══════════════════════════════════════════════════════════════════════════════

# T6: DriftState has inherited_pressure field
if grep -q 'double inherited_pressure' "$HDR_BSD"; then
    pass "T6: DriftState has inherited_pressure field"
else
    fail "T6: DriftState missing inherited_pressure"
fi

# T7: PressureWeights has pipeline_inherited
if grep -q 'double pipeline_inherited' "$HDR_GOV"; then
    pass "T7: PressureWeights has pipeline_inherited weight"
else
    fail "T7: PressureWeights missing pipeline_inherited"
fi

# T8: setInheritedPressure exists in governance_engine.cpp
if grep -q 'setInheritedPressure' "$SRC_ENGINE"; then
    pass "T8: setInheritedPressure exists in governance_engine.cpp"
else
    fail "T8: setInheritedPressure missing from governance_engine.cpp"
fi

# T9: checkContextDrift uses inherited pressure in composite
if grep -q 'pipeline_inherited.*inherited' "$SRC_ENGINE"; then
    pass "T9: checkContextDrift uses inherited pressure in composite"
else
    fail "T9: checkContextDrift missing inherited pressure factor"
fi

# T10: agentPipeline calls setInheritedPressure between stages
if grep -q 'setInheritedPressure' "$SRC_AGENT"; then
    pass "T10: agentPipeline calls setInheritedPressure between stages"
else
    fail "T10: agentPipeline missing setInheritedPressure call"
fi

# T10b: governance_config.cpp parses pipeline_inherited weight
if grep -q 'pipeline_inherited' "$SRC_CONFIG"; then
    pass "T10b: governance_config.cpp parses pipeline_inherited weight"
else
    fail "T10b: governance_config.cpp missing pipeline_inherited parsing"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Fix C: BSD Agent Identity ==="
# ══════════════════════════════════════════════════════════════════════════════

# T11: RuntimeEvent has agent_handle and agent_config fields
if grep -q 'int agent_handle' "$HDR_BSD" && grep -q 'std::string agent_config' "$HDR_BSD"; then
    pass "T11: RuntimeEvent has agent_handle and agent_config fields"
else
    fail "T11: RuntimeEvent missing agent identity fields"
fi

# T12: setAgentContext exists in governance_engine.cpp
if grep -q 'setAgentContext' "$SRC_ENGINE"; then
    pass "T12: setAgentContext exists in governance_engine.cpp"
else
    fail "T12: setAgentContext missing from governance_engine.cpp"
fi

# T13: emitEvent populates agent_handle and agent_config
if grep -q 'ev.agent_handle' "$SRC_ENGINE" && grep -q 'ev.agent_config' "$SRC_ENGINE"; then
    pass "T13: emitEvent populates agent_handle and agent_config"
else
    fail "T13: emitEvent missing agent identity population"
fi

# T14: SequencePattern has cross_agent field
if grep -q 'bool cross_agent' "$HDR_GOV"; then
    pass "T14: SequencePattern has cross_agent field"
else
    fail "T14: SequencePattern missing cross_agent field"
fi

# T15: recordEvent checks cross_agent before returning match
if grep -q 'pattern.cross_agent' "$SRC_BSD"; then
    pass "T15: recordEvent checks cross_agent before returning match"
else
    fail "T15: recordEvent missing cross_agent check"
fi

# T15b: agent_impl.cpp uses setAgentContext (not just setAgentTurn)
if grep -q 'setAgentContext' "$SRC_AGENT"; then
    pass "T15b: agent_impl.cpp uses setAgentContext"
else
    fail "T15b: agent_impl.cpp missing setAgentContext call"
fi

# T15c: governance_config.cpp parses cross_agent from BSD patterns
if grep -q 'cross_agent' "$SRC_CONFIG"; then
    pass "T15c: governance_config.cpp parses cross_agent flag"
else
    fail "T15c: governance_config.cpp missing cross_agent parsing"
fi

# T15d: governance_init.cpp has cross_agent_data_relay template pattern
if grep -q 'cross_agent_data_relay' "$SRC_INIT"; then
    pass "T15d: governance_init.cpp has cross_agent_data_relay pattern"
else
    fail "T15d: governance_init.cpp missing cross_agent_data_relay pattern"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Exposure Tracking ==="
# ══════════════════════════════════════════════════════════════════════════════

# T16: ExposureTrackingConfig struct exists
if grep -q 'struct ExposureTrackingConfig' "$HDR_GOV"; then
    pass "T16: ExposureTrackingConfig struct exists"
else
    fail "T16: ExposureTrackingConfig missing from governance.h"
fi

# T17: GovernanceRules has exposure_tracking field
if grep -q 'ExposureTrackingConfig exposure_tracking' "$HDR_GOV"; then
    pass "T17: GovernanceRules has exposure_tracking field"
else
    fail "T17: GovernanceRules missing exposure_tracking"
fi

# T18: recordAutonomousAction exists in governance_engine.cpp
if grep -q 'recordAutonomousAction' "$SRC_ENGINE"; then
    pass "T18: recordAutonomousAction exists in governance_engine.cpp"
else
    fail "T18: recordAutonomousAction missing from governance_engine.cpp"
fi

# T19: agent_impl.cpp calls recordAutonomousAction
if grep -q 'recordAutonomousAction' "$SRC_AGENT"; then
    pass "T19: agent_impl.cpp calls recordAutonomousAction"
else
    fail "T19: agent_impl.cpp missing recordAutonomousAction call"
fi

# T20: Dashboard has exposure line
if grep -q 'autonomous actions' "$SRC_ENGINE"; then
    pass "T20: Dashboard surfaces exposure tracking"
else
    fail "T20: Dashboard missing exposure line"
fi

# T21: governance_config.cpp parses exposure_tracking
if grep -q 'exposure_tracking' "$SRC_CONFIG"; then
    pass "T21: governance_config.cpp parses exposure_tracking"
else
    fail "T21: governance_config.cpp missing exposure_tracking parsing"
fi

# T22: governance_init.cpp has exposure_tracking template
if grep -q 'exposure_tracking' "$SRC_INIT"; then
    pass "T22: governance_init.cpp has exposure_tracking template"
else
    fail "T22: governance_init.cpp missing exposure_tracking template"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Results ==="
echo "PASS: $PASS  FAIL: $FAIL"
if [[ $FAIL -gt 0 ]]; then
    echo "SOME TESTS FAILED"
    exit 1
else
    echo "ALL TESTS PASSED"
    exit 0
fi
