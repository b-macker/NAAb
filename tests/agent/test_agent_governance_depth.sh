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

# T23: checkAdmission exists (pre-call prevention, not post-call intervention)
if grep -q 'checkAdmission' "$SRC_ENGINE"; then
    pass "T23: checkAdmission exists in governance_engine.cpp"
else
    fail "T23: checkAdmission missing from governance_engine.cpp"
fi

# T24: agent_impl.cpp calls checkAdmission BEFORE the API call. The provider
# entry point was renamed from callAgentMultiTurn to
# runtime::callAgentWithStatus/WithTools — anchor on the first send site.
if grep -q 'checkAdmission' "$SRC_AGENT"; then
    ADMISSION_LINE=$(grep -n 'checkAdmission' "$SRC_AGENT" | head -1 | cut -d: -f1)
    API_CALL_LINE=$(grep -n 'runtime::callAgentWith' "$SRC_AGENT" | head -1 | cut -d: -f1)
    if [[ -n "$API_CALL_LINE" && "$ADMISSION_LINE" -lt "$API_CALL_LINE" ]]; then
        pass "T24: checkAdmission is PRE-CALL (line $ADMISSION_LINE < $API_CALL_LINE)"
    else
        fail "T24: checkAdmission is POST-CALL (should be before API call)"
    fi
else
    fail "T24: agent_impl.cpp missing checkAdmission call"
fi

# T25: checkAdmission uses projection (count+1), not mutation
if grep -A20 'GovernanceEngine::checkAdmission' "$SRC_ENGINE" | grep -q 'projected_count\|+ 1'; then
    pass "T25: checkAdmission uses projection (no state mutation)"
else
    fail "T25: checkAdmission missing projection logic"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Grounding Admissibility ==="
# ══════════════════════════════════════════════════════════════════════════════

# T26: ExposureTrackingConfig has coherence_floor field
if grep -q 'double coherence_floor' "$HDR_GOV"; then
    pass "T26: ExposureTrackingConfig has coherence_floor field"
else
    fail "T26: ExposureTrackingConfig missing coherence_floor"
fi

# T27: checkAdmission checks coherence_floor (grounding test)
if grep -q 'coherence_floor' "$SRC_ENGINE"; then
    pass "T27: checkAdmission checks coherence_floor"
else
    fail "T27: checkAdmission missing coherence_floor grounding check"
fi

# T28: checkAdmission reads coherence_score from DriftState (not just exposure counts)
if grep -A20 'coherence_floor' "$SRC_ENGINE" | grep -q 'coherence_score'; then
    pass "T28: checkAdmission reads coherence_score from DriftState"
else
    fail "T28: checkAdmission not reading coherence_score"
fi

# T29: governance_config.cpp parses coherence_floor
if grep -q 'coherence_floor' "$SRC_CONFIG"; then
    pass "T29: governance_config.cpp parses coherence_floor"
else
    fail "T29: governance_config.cpp missing coherence_floor parsing"
fi

# T30: governance_init.cpp has coherence_floor in template
if grep -q 'coherence_floor' "$SRC_INIT"; then
    pass "T30: governance_init.cpp has coherence_floor in template"
else
    fail "T30: governance_init.cpp missing coherence_floor in template"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Vocabulary Contraction (Narrowing Paths) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T31: ContextDriftConfig::Signals has vocabulary_contraction
if grep -q 'bool vocabulary_contraction' "$HDR_GOV"; then
    pass "T31: CDD signals has vocabulary_contraction"
else
    fail "T31: CDD signals missing vocabulary_contraction"
fi

# T32: ContextDriftConfig::Weights has vocabulary_contraction
if grep -q 'double vocabulary_contraction' "$HDR_GOV"; then
    pass "T32: CDD weights has vocabulary_contraction"
else
    fail "T32: CDD weights missing vocabulary_contraction"
fi

# T33: DriftState has per_turn_types for sliding window
if grep -q 'per_turn_types' "$HDR_BSD"; then
    pass "T33: DriftState has per_turn_types sliding window"
else
    fail "T33: DriftState missing per_turn_types"
fi

# T34: DriftState has vocabulary_contraction_count
if grep -q 'vocabulary_contraction_count' "$HDR_BSD"; then
    pass "T34: DriftState has vocabulary_contraction_count"
else
    fail "T34: DriftState missing vocabulary_contraction_count"
fi

# T35: behavioral_sequence.cpp implements vocabulary contraction detection
if grep -q 'vocabulary_contraction' "$SRC_BSD"; then
    pass "T35: behavioral_sequence.cpp implements vocabulary contraction"
else
    fail "T35: behavioral_sequence.cpp missing vocabulary contraction"
fi

# T36: behavioral_sequence.cpp compares early vs recent entropy
if grep -q 'early_entropy' "$SRC_BSD" && grep -q 'recent_entropy' "$SRC_BSD"; then
    pass "T36: vocabulary contraction compares early vs recent entropy"
else
    fail "T36: vocabulary contraction missing early/recent comparison"
fi

# T37: governance_config.cpp parses vocabulary_contraction signal and weight
if grep -q 'vocabulary_contraction' "$SRC_CONFIG"; then
    pass "T37: governance_config.cpp parses vocabulary_contraction"
else
    fail "T37: governance_config.cpp missing vocabulary_contraction parsing"
fi

# T38: governance_engine.cpp surfaces vocabulary_contraction in CDD report
if grep -q 'vocabulary_contraction' "$SRC_ENGINE"; then
    pass "T38: governance_engine.cpp surfaces vocabulary_contraction in report"
else
    fail "T38: governance_engine.cpp missing vocabulary_contraction in report"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase A: BSD Taint Wiring + Restriction Events ==="
# ══════════════════════════════════════════════════════════════════════════════

# T39: BSD has taint_bypass_via_agent default pattern
if grep -q 'taint_bypass_via_agent' "$SRC_BSD"; then
    pass "T39: BSD has taint_bypass_via_agent pattern"
else
    fail "T39: BSD missing taint_bypass_via_agent pattern"
fi

# T40: taint_bypass_via_agent uses TAINT_VIOLATION steps
if grep -A5 'taint_bypass_via_agent' "$SRC_BSD" | grep -q 'TAINT_VIOLATION'; then
    pass "T40: taint_bypass_via_agent matches on TAINT_VIOLATION"
else
    fail "T40: taint_bypass_via_agent not matching TAINT_VIOLATION"
fi

# T41: BSD has repeated_taint_violations default pattern
if grep -q 'repeated_taint_violations' "$SRC_BSD"; then
    pass "T41: BSD has repeated_taint_violations pattern"
else
    fail "T41: BSD missing repeated_taint_violations pattern"
fi

# T42: Per-agent restriction violations emit CHECK_FAILED events
if grep -q 'agent_restriction:shell_blocked' "$SRC_AGENT"; then
    pass "T42: Shell restriction emits CHECK_FAILED event"
else
    fail "T42: Shell restriction missing CHECK_FAILED emission"
fi

# T43: Blocked path restriction emits CHECK_FAILED event
if grep -q 'agent_restriction:blocked_path' "$SRC_AGENT"; then
    pass "T43: Blocked path restriction emits CHECK_FAILED event"
else
    fail "T43: Blocked path restriction missing CHECK_FAILED emission"
fi

# T44: Allowed path restriction emits CHECK_FAILED event
if grep -q 'agent_restriction:path_not_allowed' "$SRC_AGENT"; then
    pass "T44: Allowed path restriction emits CHECK_FAILED event"
else
    fail "T44: Allowed path restriction missing CHECK_FAILED emission"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase B: Coherence Velocity & Acceleration (F1) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T45: DriftState has coherence_history deque
if grep -q 'std::deque<double> coherence_history' "$HDR_BSD"; then
    pass "T45: DriftState has coherence_history deque"
else
    fail "T45: DriftState missing coherence_history"
fi

# T46: DriftState has coherence_velocity field
if grep -q 'double coherence_velocity' "$HDR_BSD"; then
    pass "T46: DriftState has coherence_velocity field"
else
    fail "T46: DriftState missing coherence_velocity"
fi

# T47: DriftState has coherence_acceleration field
if grep -q 'double coherence_acceleration' "$HDR_BSD"; then
    pass "T47: DriftState has coherence_acceleration field"
else
    fail "T47: DriftState missing coherence_acceleration"
fi

# T48: CDD Signals has coherence_velocity flag
if grep -q 'bool coherence_velocity' "$HDR_GOV"; then
    pass "T48: CDD Signals has coherence_velocity flag"
else
    fail "T48: CDD Signals missing coherence_velocity"
fi

# T49: CDD Weights has coherence_velocity weight
if grep -q 'double coherence_velocity' "$HDR_GOV"; then
    pass "T49: CDD Weights has coherence_velocity weight"
else
    fail "T49: CDD Weights missing coherence_velocity"
fi

# T50: PressureWeights has coherence_acceleration
if grep -q 'double coherence_acceleration' "$HDR_GOV"; then
    pass "T50: PressureWeights has coherence_acceleration weight"
else
    fail "T50: PressureWeights missing coherence_acceleration"
fi

# T51: behavioral_sequence.cpp implements Signal 6 (coherence velocity).
# The hardcoded -0.15 threshold became configurable (thresholds.velocity_drop),
# so accept either the signal comment or the configurable comparison.
if grep -qi 'Signal 6.*coherence velocity' "$SRC_BSD" || \
   grep -q 'coherence_velocity < config_->thresholds.velocity_drop' "$SRC_BSD"; then
    pass "T51: behavioral_sequence.cpp implements Signal 6"
else
    fail "T51: behavioral_sequence.cpp missing Signal 6"
fi

# T52: behavioral_sequence.cpp tracks coherence_history
if grep -q 'coherence_history.push_back' "$SRC_BSD"; then
    pass "T52: behavioral_sequence.cpp tracks coherence_history"
else
    fail "T52: behavioral_sequence.cpp missing coherence_history tracking"
fi

# T53: governance_engine.cpp has Factor 7 (coherence acceleration)
if grep -q 'coherence_acceleration.*accel_factor\|Factor 7.*coherence' "$SRC_ENGINE"; then
    pass "T53: governance_engine.cpp has Factor 7 in composite"
else
    fail "T53: governance_engine.cpp missing Factor 7"
fi

# T54: Dashboard shows velocity in CDD line
if grep -q 'vel=.*accel=' "$SRC_ENGINE"; then
    pass "T54: Dashboard shows velocity and acceleration"
else
    fail "T54: Dashboard missing velocity/acceleration display"
fi

# T55: governance_config.cpp parses coherence_velocity signal
if grep -q 'coherence_velocity' "$SRC_CONFIG"; then
    pass "T55: governance_config.cpp parses coherence_velocity"
else
    fail "T55: governance_config.cpp missing coherence_velocity parsing"
fi

# T56: governance_config.cpp parses coherence_acceleration pressure weight
if grep -q 'coherence_acceleration' "$SRC_CONFIG"; then
    pass "T56: governance_config.cpp parses coherence_acceleration"
else
    fail "T56: governance_config.cpp missing coherence_acceleration parsing"
fi

# T57: DriftState has turns_analyzed for rate normalization
if grep -q 'int turns_analyzed' "$HDR_BSD"; then
    pass "T57: DriftState has turns_analyzed field"
else
    fail "T57: DriftState missing turns_analyzed"
fi

# T58: behavioral_sequence.cpp increments turns_analyzed
if grep -q 'turns_analyzed++' "$SRC_BSD"; then
    pass "T58: behavioral_sequence.cpp increments turns_analyzed"
else
    fail "T58: behavioral_sequence.cpp missing turns_analyzed increment"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase B: Action Entropy (F2) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T59: DriftState has initial_entropy for baseline
if grep -q 'double initial_entropy' "$HDR_BSD"; then
    pass "T59: DriftState has initial_entropy baseline"
else
    fail "T59: DriftState missing initial_entropy"
fi

# T60: behavioral_sequence.cpp uses Shannon entropy (log2)
if grep -q 'log2' "$SRC_BSD"; then
    pass "T60: behavioral_sequence.cpp uses Shannon entropy (log2)"
else
    fail "T60: behavioral_sequence.cpp missing Shannon entropy"
fi

# T61: behavioral_sequence.cpp computes entropy per window half
if grep -q 'early_entropy' "$SRC_BSD" && grep -q 'recent_entropy' "$SRC_BSD"; then
    pass "T61: vocabulary contraction uses entropy comparison"
else
    fail "T61: vocabulary contraction missing entropy comparison"
fi

# T62: behavioral_sequence.cpp includes <cmath>
if grep -q '#include <cmath>' "$SRC_BSD"; then
    pass "T62: behavioral_sequence.cpp includes <cmath>"
else
    fail "T62: behavioral_sequence.cpp missing <cmath> include"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase B: Rate-Normalized Signals (F5) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T63: ContextDriftConfig has rate_normalized flag
if grep -q 'bool rate_normalized' "$HDR_GOV"; then
    pass "T63: ContextDriftConfig has rate_normalized flag"
else
    fail "T63: ContextDriftConfig missing rate_normalized"
fi

# T64: behavioral_sequence.cpp has rate normalization helper
if grep -q 'rate_normalized' "$SRC_BSD"; then
    pass "T64: behavioral_sequence.cpp implements rate normalization"
else
    fail "T64: behavioral_sequence.cpp missing rate normalization"
fi

# T65: governance_config.cpp parses rate_normalized
if grep -q 'rate_normalized' "$SRC_CONFIG"; then
    pass "T65: governance_config.cpp parses rate_normalized"
else
    fail "T65: governance_config.cpp missing rate_normalized parsing"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase B: Coherence Recovery (F15) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T66: ContextDriftConfig has coherence_recovery_amount
if grep -q 'double coherence_recovery_amount' "$HDR_GOV"; then
    pass "T66: ContextDriftConfig has coherence_recovery_amount"
else
    fail "T66: ContextDriftConfig missing coherence_recovery_amount"
fi

# T67: ContextDriftConfig has coherence_natural_healing
if grep -q 'double coherence_natural_healing' "$HDR_GOV"; then
    pass "T67: ContextDriftConfig has coherence_natural_healing"
else
    fail "T67: ContextDriftConfig missing coherence_natural_healing"
fi

# T68: ContextDriftAnalyzer has resetCoherence method
if grep -q 'void resetCoherence' "$HDR_BSD"; then
    pass "T68: ContextDriftAnalyzer has resetCoherence method"
else
    fail "T68: ContextDriftAnalyzer missing resetCoherence"
fi

# T69: GovernanceEngine has recoverCoherence method
if grep -q 'void recoverCoherence' "$HDR_GOV"; then
    pass "T69: GovernanceEngine has recoverCoherence"
else
    fail "T69: GovernanceEngine missing recoverCoherence"
fi

# T70: behavioral_sequence.cpp implements resetCoherence
if grep -q 'ContextDriftAnalyzer::resetCoherence' "$SRC_BSD"; then
    pass "T70: behavioral_sequence.cpp implements resetCoherence"
else
    fail "T70: behavioral_sequence.cpp missing resetCoherence impl"
fi

# T71: behavioral_sequence.cpp implements natural healing
if grep -q 'coherence_natural_healing' "$SRC_BSD"; then
    pass "T71: behavioral_sequence.cpp implements natural healing"
else
    fail "T71: behavioral_sequence.cpp missing natural healing"
fi

# T72: governance_engine.cpp forwards recoverCoherence
if grep -q 'recoverCoherence' "$SRC_ENGINE"; then
    pass "T72: governance_engine.cpp forwards recoverCoherence"
else
    fail "T72: governance_engine.cpp missing recoverCoherence"
fi

# T73: agent_impl.cpp calls recoverCoherence at pipeline transitions
if grep -q 'recoverCoherence' "$SRC_AGENT"; then
    pass "T73: agent_impl.cpp calls recoverCoherence at pipeline transitions"
else
    fail "T73: agent_impl.cpp missing recoverCoherence call"
fi

# T74: governance_config.cpp parses coherence_recovery_amount
if grep -q 'coherence_recovery_amount' "$SRC_CONFIG"; then
    pass "T74: governance_config.cpp parses coherence_recovery_amount"
else
    fail "T74: governance_config.cpp missing coherence_recovery_amount parsing"
fi

# T75: governance_config.cpp parses coherence_natural_healing
if grep -q 'coherence_natural_healing' "$SRC_CONFIG"; then
    pass "T75: governance_config.cpp parses coherence_natural_healing"
else
    fail "T75: governance_config.cpp missing coherence_natural_healing parsing"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase C: Dependency Depth Tracking (F3) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T76: DriftState has pipeline_depth field
if grep -q 'int pipeline_depth' "$HDR_BSD"; then
    pass "T76: DriftState has pipeline_depth field"
else
    fail "T76: DriftState missing pipeline_depth"
fi

# T77: ExposureTrackingConfig has max_pipeline_depth
if grep -q 'int max_pipeline_depth' "$HDR_GOV"; then
    pass "T77: ExposureTrackingConfig has max_pipeline_depth"
else
    fail "T77: ExposureTrackingConfig missing max_pipeline_depth"
fi

# T78: GovernanceEngine has setPipelineDepth
if grep -q 'setPipelineDepth' "$HDR_GOV"; then
    pass "T78: GovernanceEngine has setPipelineDepth"
else
    fail "T78: GovernanceEngine missing setPipelineDepth"
fi

# T79: governance_engine.cpp implements setPipelineDepth
if grep -q 'setPipelineDepth' "$SRC_ENGINE"; then
    pass "T79: governance_engine.cpp implements setPipelineDepth"
else
    fail "T79: governance_engine.cpp missing setPipelineDepth"
fi

# T80: checkAdmission checks pipeline depth
if grep -q 'max_pipeline_depth\|t_pipeline_depth' "$SRC_ENGINE"; then
    pass "T80: checkAdmission checks pipeline depth"
else
    fail "T80: checkAdmission missing pipeline depth check"
fi

# T81: agent_impl.cpp tracks pipeline depth
if grep -q 't_pipeline_depth' "$SRC_AGENT"; then
    pass "T81: agent_impl.cpp tracks pipeline depth"
else
    fail "T81: agent_impl.cpp missing pipeline depth tracking"
fi

# T82: governance_config.cpp parses max_pipeline_depth
if grep -q 'max_pipeline_depth' "$SRC_CONFIG"; then
    pass "T82: governance_config.cpp parses max_pipeline_depth"
else
    fail "T82: governance_config.cpp missing max_pipeline_depth parsing"
fi

# T83: governance_init.cpp has max_pipeline_depth template
if grep -q 'max_pipeline_depth' "$SRC_INIT"; then
    pass "T83: governance_init.cpp has max_pipeline_depth template"
else
    fail "T83: governance_init.cpp missing max_pipeline_depth template"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase C: Pipeline Separation of Duties (F7) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T84: PipelineSeparationConfig struct exists
if grep -q 'struct PipelineSeparationConfig' "$HDR_GOV"; then
    pass "T84: PipelineSeparationConfig struct exists"
else
    fail "T84: PipelineSeparationConfig missing"
fi

# T85: GovernanceRules has pipeline_separation
if grep -q 'PipelineSeparationConfig pipeline_separation' "$HDR_GOV"; then
    pass "T85: GovernanceRules has pipeline_separation"
else
    fail "T85: GovernanceRules missing pipeline_separation"
fi

# T86: agent_impl.cpp checks pipeline separation
if grep -q 'pipeline_separation' "$SRC_AGENT"; then
    pass "T86: agent_impl.cpp checks pipeline separation"
else
    fail "T86: agent_impl.cpp missing pipeline separation check"
fi

# T87: governance_config.cpp parses pipeline_separation
if grep -q 'pipeline_separation' "$SRC_CONFIG"; then
    pass "T87: governance_config.cpp parses pipeline_separation"
else
    fail "T87: governance_config.cpp missing pipeline_separation parsing"
fi

# T88: governance_init.cpp has pipeline_separation template
if grep -q 'pipeline_separation' "$SRC_INIT"; then
    pass "T88: governance_init.cpp has pipeline_separation template"
else
    fail "T88: governance_init.cpp missing pipeline_separation template"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase C: Per-Agent Risk Budget (F8) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T89: AgentConfig has risk_budget field
if grep -q 'int risk_budget' "$HDR_GOV"; then
    pass "T89: AgentConfig has risk_budget field"
else
    fail "T89: AgentConfig missing risk_budget"
fi

# T90: GovernanceEngine has consumeRiskBudget
if grep -q 'consumeRiskBudget' "$HDR_GOV"; then
    pass "T90: GovernanceEngine has consumeRiskBudget"
else
    fail "T90: GovernanceEngine missing consumeRiskBudget"
fi

# T91: GovernanceEngine has getRemainingBudget
if grep -q 'getRemainingBudget' "$HDR_GOV"; then
    pass "T91: GovernanceEngine has getRemainingBudget"
else
    fail "T91: GovernanceEngine missing getRemainingBudget"
fi

# T92: governance_engine.cpp implements consumeRiskBudget
if grep -q 'consumeRiskBudget' "$SRC_ENGINE"; then
    pass "T92: governance_engine.cpp implements consumeRiskBudget"
else
    fail "T92: governance_engine.cpp missing consumeRiskBudget"
fi

# T93: checkAdmission checks risk budget
if grep -q 'getRemainingBudget\|risk budget' "$SRC_ENGINE"; then
    pass "T93: checkAdmission checks risk budget"
else
    fail "T93: checkAdmission missing risk budget check"
fi

# T94: governance_config.cpp parses risk_budget per agent
if grep -q 'risk_budget' "$SRC_CONFIG"; then
    pass "T94: governance_config.cpp parses risk_budget"
else
    fail "T94: governance_config.cpp missing risk_budget parsing"
fi

# T95: Dashboard surfaces budget status
if grep -q 'Budget:' "$SRC_ENGINE" || grep -q 'remaining.*consumed' "$SRC_ENGINE"; then
    pass "T95: Dashboard surfaces budget status"
else
    fail "T95: Dashboard missing budget display"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase C: Checkpoint Cooldown (F12) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T96: ExposureTrackingConfig has checkpoint_cooldown_turns
if grep -q 'int checkpoint_cooldown_turns' "$HDR_GOV"; then
    pass "T96: ExposureTrackingConfig has checkpoint_cooldown_turns"
else
    fail "T96: ExposureTrackingConfig missing checkpoint_cooldown_turns"
fi

# T97: checkAdmission checks checkpoint cooldown
if grep -q 'checkpoint_cooldown_turns' "$SRC_ENGINE"; then
    pass "T97: checkAdmission checks checkpoint cooldown"
else
    fail "T97: checkAdmission missing checkpoint cooldown check"
fi

# T98: governance_config.cpp parses checkpoint_cooldown_turns
if grep -q 'checkpoint_cooldown_turns' "$SRC_CONFIG"; then
    pass "T98: governance_config.cpp parses checkpoint_cooldown_turns"
else
    fail "T98: governance_config.cpp missing checkpoint_cooldown_turns parsing"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase D: Barrier Health Monitoring (F4) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T99: GovernanceHealthConfig struct exists
if grep -q 'struct GovernanceHealthConfig' "$HDR_GOV"; then
    pass "T99: GovernanceHealthConfig struct exists"
else
    fail "T99: GovernanceHealthConfig missing"
fi

# T100: GovernanceRules has governance_health
if grep -q 'GovernanceHealthConfig governance_health' "$HDR_GOV"; then
    pass "T100: GovernanceRules has governance_health"
else
    fail "T100: GovernanceRules missing governance_health"
fi

# T101: GovernanceEngine has checkGovernanceHealth
if grep -q 'checkGovernanceHealth' "$HDR_GOV"; then
    pass "T101: GovernanceEngine has checkGovernanceHealth"
else
    fail "T101: GovernanceEngine missing checkGovernanceHealth"
fi

# T102: governance_engine.cpp implements checkGovernanceHealth
if grep -q 'checkGovernanceHealth' "$SRC_ENGINE"; then
    pass "T102: governance_engine.cpp implements checkGovernanceHealth"
else
    fail "T102: governance_engine.cpp missing checkGovernanceHealth"
fi

# T103: governance_config.cpp parses governance_health
if grep -q 'governance_health' "$SRC_CONFIG"; then
    pass "T103: governance_config.cpp parses governance_health"
else
    fail "T103: governance_config.cpp missing governance_health parsing"
fi

# T104: governance_init.cpp has governance_health template
if grep -q 'governance_health' "$SRC_INIT"; then
    pass "T104: governance_init.cpp has governance_health template"
else
    fail "T104: governance_init.cpp missing governance_health template"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase D: Graduated Circuit Breakers (F6) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T105: GovernanceLevel enum exists
if grep -q 'enum class GovernanceLevel' "$HDR_GOV"; then
    pass "T105: GovernanceLevel enum exists"
else
    fail "T105: GovernanceLevel enum missing"
fi

# T106: CircuitBreakerConfig struct exists
if grep -q 'struct CircuitBreakerConfig' "$HDR_GOV"; then
    pass "T106: CircuitBreakerConfig struct exists"
else
    fail "T106: CircuitBreakerConfig missing"
fi

# T107: GovernanceRules has circuit_breaker
if grep -q 'CircuitBreakerConfig circuit_breaker' "$HDR_GOV"; then
    pass "T107: GovernanceRules has circuit_breaker"
else
    fail "T107: GovernanceRules missing circuit_breaker"
fi

# T108: GovernanceEngine has getGovernanceLevel
if grep -q 'getGovernanceLevel' "$HDR_GOV"; then
    pass "T108: GovernanceEngine has getGovernanceLevel"
else
    fail "T108: GovernanceEngine missing getGovernanceLevel"
fi

# T109: governance_engine.cpp updates governance level
if grep -q 'governance_level_' "$SRC_ENGINE"; then
    pass "T109: governance_engine.cpp updates governance level"
else
    fail "T109: governance_engine.cpp missing governance level update"
fi

# T110: checkAdmission denies at CRITICAL level
if grep -q 'CRITICAL' "$SRC_ENGINE"; then
    pass "T110: checkAdmission denies at CRITICAL level"
else
    fail "T110: checkAdmission missing CRITICAL denial"
fi

# T111: governance_config.cpp parses circuit_breaker
if grep -q 'circuit_breaker' "$SRC_CONFIG"; then
    pass "T111: governance_config.cpp parses circuit_breaker"
else
    fail "T111: governance_config.cpp missing circuit_breaker parsing"
fi

# T112: governance_init.cpp has circuit_breaker template
if grep -q 'circuit_breaker' "$SRC_INIT"; then
    pass "T112: governance_init.cpp has circuit_breaker template"
else
    fail "T112: governance_init.cpp missing circuit_breaker template"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase D: Governance Event Entropy (F16) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T113: GovernanceEngine has computeGovernanceEntropy
if grep -q 'computeGovernanceEntropy' "$HDR_GOV"; then
    pass "T113: GovernanceEngine has computeGovernanceEntropy"
else
    fail "T113: GovernanceEngine missing computeGovernanceEntropy"
fi

# T114: governance_engine.cpp implements computeGovernanceEntropy
if grep -q 'computeGovernanceEntropy' "$SRC_ENGINE"; then
    pass "T114: governance_engine.cpp implements computeGovernanceEntropy"
else
    fail "T114: governance_engine.cpp missing computeGovernanceEntropy"
fi

# T115: GovernanceHealthConfig has governance_entropy_warning
if grep -q 'governance_entropy_warning' "$HDR_GOV"; then
    pass "T115: GovernanceHealthConfig has governance_entropy_warning"
else
    fail "T115: GovernanceHealthConfig missing governance_entropy_warning"
fi

# T116: checkGovernanceHealth checks entropy
if grep -q 'governance_entropy_warning\|computeGovernanceEntropy' "$SRC_ENGINE"; then
    pass "T116: checkGovernanceHealth checks entropy"
else
    fail "T116: checkGovernanceHealth missing entropy check"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase D: Decision Trace Coherence (F17) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T117: GovernanceEngine has checkDecisionTraceCoherence
if grep -q 'checkDecisionTraceCoherence' "$HDR_GOV"; then
    pass "T117: GovernanceEngine has checkDecisionTraceCoherence"
else
    fail "T117: GovernanceEngine missing checkDecisionTraceCoherence"
fi

# T118: governance_engine.cpp implements checkDecisionTraceCoherence
if grep -q 'checkDecisionTraceCoherence' "$SRC_ENGINE"; then
    pass "T118: governance_engine.cpp implements checkDecisionTraceCoherence"
else
    fail "T118: governance_engine.cpp missing checkDecisionTraceCoherence"
fi

# T119: GovernanceEngine has agent_decision_traces_ storage
if grep -q 'agent_decision_traces_' "$HDR_GOV"; then
    pass "T119: GovernanceEngine has decision trace storage"
else
    fail "T119: GovernanceEngine missing decision trace storage"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase E: Inverse Signal Detection (F9) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T120: DriftState has granted_capabilities
if grep -q 'granted_capabilities' "$HDR_BSD"; then
    pass "T120: DriftState has granted_capabilities"
else
    fail "T120: DriftState missing granted_capabilities"
fi

# T121: DriftState has exercised_capabilities
if grep -q 'exercised_capabilities' "$HDR_BSD"; then
    pass "T121: DriftState has exercised_capabilities"
else
    fail "T121: DriftState missing exercised_capabilities"
fi

# T122: CDD Signals has capability_underutilization
if grep -q 'bool capability_underutilization' "$HDR_GOV"; then
    pass "T122: CDD Signals has capability_underutilization"
else
    fail "T122: CDD Signals missing capability_underutilization"
fi

# T123: behavioral_sequence.cpp implements capability tracking
if grep -q 'exercised_capabilities' "$SRC_BSD"; then
    pass "T123: behavioral_sequence.cpp implements capability tracking"
else
    fail "T123: behavioral_sequence.cpp missing capability tracking"
fi

# T124: governance_config.cpp parses capability_underutilization
if grep -q 'capability_underutilization' "$SRC_CONFIG"; then
    pass "T124: governance_config.cpp parses capability_underutilization"
else
    fail "T124: governance_config.cpp missing capability_underutilization"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase E: Temporal Coupling Detection (F10) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T125: TemporalCouplingConfig struct exists
if grep -q 'struct TemporalCouplingConfig' "$HDR_GOV"; then
    pass "T125: TemporalCouplingConfig struct exists"
else
    fail "T125: TemporalCouplingConfig missing"
fi

# T126: GovernanceRules has temporal_coupling
if grep -q 'TemporalCouplingConfig temporal_coupling' "$HDR_GOV"; then
    pass "T126: GovernanceRules has temporal_coupling"
else
    fail "T126: GovernanceRules missing temporal_coupling"
fi

# T127: GovernanceEngine has checkTemporalCoupling
if grep -q 'checkTemporalCoupling' "$HDR_GOV"; then
    pass "T127: GovernanceEngine has checkTemporalCoupling"
else
    fail "T127: GovernanceEngine missing checkTemporalCoupling"
fi

# T128: governance_engine.cpp implements checkTemporalCoupling
if grep -q 'checkTemporalCoupling' "$SRC_ENGINE"; then
    pass "T128: governance_engine.cpp implements checkTemporalCoupling"
else
    fail "T128: governance_engine.cpp missing checkTemporalCoupling"
fi

# T129: governance_config.cpp parses temporal_coupling
if grep -q 'temporal_coupling' "$SRC_CONFIG"; then
    pass "T129: governance_config.cpp parses temporal_coupling"
else
    fail "T129: governance_config.cpp missing temporal_coupling parsing"
fi

# T130: governance_init.cpp has temporal_coupling template
if grep -q 'temporal_coupling' "$SRC_INIT"; then
    pass "T130: governance_init.cpp has temporal_coupling template"
else
    fail "T130: governance_init.cpp missing temporal_coupling template"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase E: Cross-Block Taint Gate (F14) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T131: TaintTrackingConfig has gate_cross_block
if grep -q 'bool gate_cross_block' "$HDR_GOV"; then
    pass "T131: TaintTrackingConfig has gate_cross_block"
else
    fail "T131: TaintTrackingConfig missing gate_cross_block"
fi

# T132: TaintTrackingConfig has cross_block_level
if grep -q 'cross_block_level' "$HDR_GOV"; then
    pass "T132: TaintTrackingConfig has cross_block_level"
else
    fail "T132: TaintTrackingConfig missing cross_block_level"
fi

# T133: auditCrossBlockFlows uses gate_cross_block
if grep -q 'gate_cross_block' "$SRC_ENGINE"; then
    pass "T133: auditCrossBlockFlows uses gate_cross_block"
else
    fail "T133: auditCrossBlockFlows missing gate_cross_block"
fi

# T134: governance_config.cpp parses gate_cross_block
if grep -q 'gate_cross_block' "$SRC_CONFIG"; then
    pass "T134: governance_config.cpp parses gate_cross_block"
else
    fail "T134: governance_config.cpp missing gate_cross_block parsing"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase E: Capability Utilization (F18) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T135: ExposureTrackingConfig has min_capability_utilization
if grep -q 'double min_capability_utilization' "$HDR_GOV"; then
    pass "T135: ExposureTrackingConfig has min_capability_utilization"
else
    fail "T135: ExposureTrackingConfig missing min_capability_utilization"
fi

# T136: ExposureTrackingConfig has utilization_check_after_turns
if grep -q 'int utilization_check_after_turns' "$HDR_GOV"; then
    pass "T136: ExposureTrackingConfig has utilization_check_after_turns"
else
    fail "T136: ExposureTrackingConfig missing utilization_check_after_turns"
fi

# T137: governance_config.cpp parses min_capability_utilization
if grep -q 'min_capability_utilization' "$SRC_CONFIG"; then
    pass "T137: governance_config.cpp parses min_capability_utilization"
else
    fail "T137: governance_config.cpp missing min_capability_utilization"
fi

# ══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Phase E: Semantic Stability (F19) ==="
# ══════════════════════════════════════════════════════════════════════════════

# T138: CDD Signals has semantic_stability
if grep -q 'bool semantic_stability' "$HDR_GOV"; then
    pass "T138: CDD Signals has semantic_stability"
else
    fail "T138: CDD Signals missing semantic_stability"
fi

# T139: CDD Weights has semantic_stability
if grep -q 'double semantic_stability' "$HDR_GOV"; then
    pass "T139: CDD Weights has semantic_stability"
else
    fail "T139: CDD Weights missing semantic_stability"
fi

# T140: DriftState has prev_response_keywords
if grep -q 'prev_response_keywords' "$HDR_BSD"; then
    pass "T140: DriftState has prev_response_keywords"
else
    fail "T140: DriftState missing prev_response_keywords"
fi

# T141: governance_config.cpp parses semantic_stability
if grep -q 'semantic_stability' "$SRC_CONFIG"; then
    pass "T141: governance_config.cpp parses semantic_stability"
else
    fail "T141: governance_config.cpp missing semantic_stability"
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
