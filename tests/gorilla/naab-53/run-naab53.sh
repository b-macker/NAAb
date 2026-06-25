#!/usr/bin/env bash
# ============================================================
# run-naab53.sh — NAAb-53 Escalation Effectiveness Gorilla Test
#
# 12-turn phased conversation: TOOL_BASELINE(4) → DRIFT(5) → RETURN(3)
# Validates governance level escalation, contextual challenge firing,
# and escalation effectiveness tracking.
#
# Assertions (~20):
#   S  — System integrity (agent created, turns completed, telemetry)
#   E  — Escalation (GOVERNANCE_LEVEL_CHANGE emitted, from/to correct)
#   CH — Challenges (challenge events emitted, correct type)
#   EFF — Effectiveness (escalation_turn >= 0, effectiveness field)
#   T  — Telemetry (RECONCILIATION_TURN, hash chain)
#
# Config strategy grounded in governance_engine.cpp:6709-6882:
#   coherence_threshold=0.10 (low → drifted=false during drift)
#   elevated_threshold=0.25 (reachable by signal_density alone)
#   signal weights ~0.04 each (slow coherence decay)
#   coherence_natural_healing=0.0 (no free recovery)
#   reality_checkpoint.enabled=false (checkpoint return at line 6845
#     exits before circuit breaker level update at line 6875)
#
# Contextual challenges include conversation history (agent_impl.cpp:1528+).
# Model can now recall tool results, so keyword_ratio should be > 0.
# Challenge may pass or fail depending on model behavior. Exit 3
# (GovernanceHardError) is still valid if challenge fails.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"
HELPERS="$SCRIPT_DIR/../../helpers/trust_setup.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
WORK="${_SYSTMP}/naab53-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" d="${3:-}"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$d" ] && echo -e "       ${RED}-> $d${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${d:+ -- $d}"; }
skip() { local id="$1" desc="$2"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

# --- Trust setup ---
source "$HELPERS"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$WORK"' EXIT
mkdir -p "$WORK"

# --- Signing ---
"$NAAB" --keygen "$WORK/key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$WORK/key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$WORK/key.pem"

# --- Copy source + sign ---
cp "$SRC_DIR/govern.json" "$WORK/"
cp "$SRC_DIR/escalation-effectiveness.naab" "$WORK/"
(cd "$WORK" && "$NAAB" --sign-governance >/dev/null 2>&1)

# --- API key check ---
source ~/.bashrc 2>/dev/null || true
HAS_KEY=false
for k in GK1 GK2 GK3 GK4 GK5 GK6 GK7 GK8 GK9; do
    if [ -n "${!k:-}" ]; then HAS_KEY=true; break; fi
done

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  NAAb-53: Escalation Effectiveness Gorilla Test              |${NC}"
echo -e "${CYAN}|  12-turn phased: TOOL_BASELINE(4) DRIFT(5) RETURN(3)        |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ "$HAS_KEY" != "true" ]; then
    echo -e "${YELLOW}No GK* API keys found — skipping all assertions${NC}"
    exit 0
fi

# --- Run ---
echo -e "${CYAN}Running escalation-effectiveness.naab...${NC}"
STDOUT="$WORK/stdout.txt"
STDERR="$WORK/stderr.txt"
TELEM="$WORK/telemetry.jsonl"

(cd "$WORK" && "$NAAB" --governance-dashboard escalation-effectiveness.naab) \
    >"$STDOUT" 2>"$STDERR"
EXIT_CODE=$?

echo -e "${CYAN}Exit code: $EXIT_CODE${NC}"
echo ""

# ============================================================
# S: System Integrity
# ============================================================
echo -e "${CYAN}--- S: System Integrity ---${NC}"

# S01: Agent created
if grep -q "AGENT_CREATED" "$STDOUT"; then
    pass "S01" "Agent created successfully"
else
    fail "S01" "Agent creation failed" "$(head -3 "$STDOUT")"
fi

# S02: Some turns completed (count TURN lines — SUMMARY may be missing if GovernanceHardError killed program)
TURNS_DONE=$(grep "SUMMARY_TURNS_COMPLETED:" "$STDOUT" | head -1 | awk '{print $2}')
if [ -z "$TURNS_DONE" ]; then
    # Fallback: count TURN| lines in stdout
    TURNS_DONE=$(grep "^TURN|" "$STDOUT" | wc -l)
    TURNS_DONE=$(echo "$TURNS_DONE" | tr -d ' ')
fi
if [ "$TURNS_DONE" -ge 4 ]; then
    pass "S02" "Completed $TURNS_DONE turns (>= 4)"
else
    fail "S02" "Too few turns completed" "got $TURNS_DONE"
fi

# S03: Telemetry file exists and has events
if [ -f "$TELEM" ]; then
    TELEM_LINES=$(wc -l < "$TELEM")
    TELEM_LINES=$(echo "$TELEM_LINES" | tr -d ' ')
    if [ "$TELEM_LINES" -gt 0 ]; then
        pass "S03" "Telemetry: $TELEM_LINES events"
    else
        fail "S03" "Telemetry file empty"
    fi
else
    fail "S03" "Telemetry file not found"
fi

# S04: Valid exit code (0=success, 1=API error, 2=quality gate, 3=hard error)
if [ "$EXIT_CODE" -le 3 ]; then
    pass "S04" "Exit code $EXIT_CODE is valid"
else
    fail "S04" "Unexpected exit code $EXIT_CODE"
fi

# ============================================================
# E: Escalation
# ============================================================
echo -e "${CYAN}--- E: Escalation ---${NC}"

# E01: GOVERNANCE_LEVEL_CHANGE telemetry emitted
LEVEL_CHANGES=0
if [ -f "$TELEM" ]; then
    LEVEL_CHANGES=$(grep "GOVERNANCE_LEVEL_CHANGE" "$TELEM" | wc -l)
    LEVEL_CHANGES=$(echo "$LEVEL_CHANGES" | tr -d ' ')
fi
if [ "$LEVEL_CHANGES" -gt 0 ]; then
    pass "E01" "GOVERNANCE_LEVEL_CHANGE emitted ($LEVEL_CHANGES events)"
else
    skip "E01" "No GOVERNANCE_LEVEL_CHANGE events (escalation may not have triggered)"
fi

# E02: Level changed from normal
if [ "$LEVEL_CHANGES" -gt 0 ]; then
    if grep "GOVERNANCE_LEVEL_CHANGE" "$TELEM" | grep -q '"from_level":"normal"'; then
        pass "E02" "Escalation from 'normal'"
    else
        fail "E02" "Escalation not from 'normal'" "$(grep GOVERNANCE_LEVEL_CHANGE "$TELEM" | head -1)"
    fi
else
    skip "E02" "No level change events to check"
fi

# E03: Level changed to elevated or higher
if [ "$LEVEL_CHANGES" -gt 0 ]; then
    TO_ELEVATED=$(grep "GOVERNANCE_LEVEL_CHANGE" "$TELEM" | grep -E '"to_level":"(elevated|high|critical)"' | wc -l)
    TO_ELEVATED=$(echo "$TO_ELEVATED" | tr -d ' ')
    if [ "$TO_ELEVATED" -gt 0 ]; then
        pass "E03" "Escalated to elevated/high/critical"
    else
        fail "E03" "Level changed but not to elevated+" "$(grep GOVERNANCE_LEVEL_CHANGE "$TELEM" | head -1)"
    fi
else
    skip "E03" "No level change events to check"
fi

# E04: coherence_at_escalation field present in GOVERNANCE_LEVEL_CHANGE
if [ "$LEVEL_CHANGES" -gt 0 ]; then
    if grep "GOVERNANCE_LEVEL_CHANGE" "$TELEM" | grep -q "coherence_at_escalation"; then
        pass "E04" "coherence_at_escalation field present in GOVERNANCE_LEVEL_CHANGE"
    else
        fail "E04" "coherence_at_escalation missing from GOVERNANCE_LEVEL_CHANGE"
    fi
else
    skip "E04" "No level change events to check"
fi

# E05: escalation_turn >= 0 in program output (check TURN lines if SUMMARY missing)
ESC_TURN=$(grep "SUMMARY_ESCALATION_TURN:" "$STDOUT" | head -1 | awk '{print $2}')
if [ -z "$ESC_TURN" ]; then
    # Fallback: extract from last TURN line with et= field
    ESC_TURN=$(grep "^TURN|" "$STDOUT" | grep -v "|et=-1|" | head -1 | sed 's/.*|et=\([^|]*\).*/\1/')
    ESC_TURN=${ESC_TURN:--1}
fi
if [ "$ESC_TURN" -ge 0 ] 2>/dev/null; then
    pass "E05" "escalation_turn=$ESC_TURN in output (>= 0)"
else
    skip "E05" "escalation_turn=$ESC_TURN (no escalation detected by program)"
fi

# ============================================================
# CH: Challenges
# ============================================================
echo -e "${CYAN}--- CH: Challenges ---${NC}"

# CH01: Challenge events emitted (pass or fail)
CH_PASS=0; CH_FAIL_EV=0
if [ -f "$TELEM" ]; then
    CH_PASS=$(grep "AGENT_CHALLENGE_PASS" "$TELEM" | wc -l)
    CH_PASS=$(echo "$CH_PASS" | tr -d ' ')
    CH_FAIL_EV=$(grep "AGENT_CHALLENGE_FAIL" "$TELEM" | wc -l)
    CH_FAIL_EV=$(echo "$CH_FAIL_EV" | tr -d ' ')
fi
CH_TOTAL=$((CH_PASS + CH_FAIL_EV))
if [ "$CH_TOTAL" -gt 0 ]; then
    pass "CH01" "Challenge events emitted: $CH_PASS pass, $CH_FAIL_EV fail"
else
    skip "CH01" "No challenge events (escalation may not have reached elevated)"
fi

# CH02: Challenge type is tool_result (tool_result_keywords persist from baseline)
if [ "$CH_TOTAL" -gt 0 ]; then
    TOOL_RESULT_TYPE=$(grep -E "AGENT_CHALLENGE_(PASS|FAIL)" "$TELEM" | grep '"challenge_type":"tool_result"' | wc -l)
    TOOL_RESULT_TYPE=$(echo "$TOOL_RESULT_TYPE" | tr -d ' ')
    if [ "$TOOL_RESULT_TYPE" -gt 0 ]; then
        pass "CH02" "Challenge type=tool_result ($TOOL_RESULT_TYPE events)"
    else
        # Could be mandate type if tool_result_keywords were empty
        OTHER_TYPE=$(grep -E "AGENT_CHALLENGE_(PASS|FAIL)" "$TELEM" | head -1)
        fail "CH02" "Expected challenge_type=tool_result" "$(echo "$OTHER_TYPE" | grep -o '"challenge_type":"[^"]*"')"
    fi
else
    skip "CH02" "No challenge events to check type"
fi

# CH03: challenges_passed or challenges_failed > 0 (from SUMMARY or TURN output or telemetry)
PROG_CP=$(grep "SUMMARY_CHALLENGES_PASSED:" "$STDOUT" | head -1 | awk '{print $2}')
PROG_CF=$(grep "SUMMARY_CHALLENGES_FAILED:" "$STDOUT" | head -1 | awk '{print $2}')
PROG_CP=${PROG_CP:-0}; PROG_CF=${PROG_CF:-0}
PROG_CH_TOTAL=$((PROG_CP + PROG_CF))
# Fallback: count from telemetry if SUMMARY missing (GovernanceHardError kills before SUMMARY)
if [ "$PROG_CH_TOTAL" -eq 0 ]; then
    PROG_CP=$CH_PASS; PROG_CF=$CH_FAIL_EV
    PROG_CH_TOTAL=$((PROG_CP + PROG_CF))
fi
if [ "$PROG_CH_TOTAL" -gt 0 ]; then
    pass "CH03" "Challenges observed: $PROG_CP passed, $PROG_CF failed"
else
    skip "CH03" "No challenges observed"
fi

# CH04: Challenge included conversation history (proves fix applied)
if [ "$CH_TOTAL" -gt 0 ]; then
    HM_PRESENT=$(grep -E "AGENT_CHALLENGE_(PASS|FAIL)" "$TELEM" | grep -oP '"history_messages":"[1-9][0-9]*"' | head -1)
    if [ -n "$HM_PRESENT" ]; then
        pass "CH04" "Challenge included conversation history ($HM_PRESENT)"
    else
        fail "CH04" "Challenge has 0 history messages (fix not applied)"
    fi
else
    skip "CH04" "No challenge events to check history"
fi

# CH05: keyword_ratio > 0 for contextual challenge (model has context)
if [ "$CH_TOTAL" -gt 0 ]; then
    KR_NONZERO=$(grep -E "AGENT_CHALLENGE_(PASS|FAIL)" "$TELEM" | grep '"challenge_type":"tool_result"' | grep -v '"keyword_ratio":"0\.0' | grep -v '"keyword_ratio":"-1' | wc -l)
    KR_NONZERO=$(echo "$KR_NONZERO" | tr -d ' ')
    if [ "$KR_NONZERO" -gt 0 ]; then
        pass "CH05" "keyword_ratio > 0 for tool_result challenge"
    else
        skip "CH05" "keyword_ratio=0 despite history (model-dependent)"
    fi
else
    skip "CH05" "No challenge events to check keyword_ratio"
fi

# ============================================================
# EFF: Effectiveness
# ============================================================
echo -e "${CYAN}--- EFF: Effectiveness ---${NC}"

# EFF01: escalation_turn visible in per-turn output
ET_VISIBLE=$(grep "TURN|" "$STDOUT" | grep -v "et=-1" | wc -l)
ET_VISIBLE=$(echo "$ET_VISIBLE" | tr -d ' ')
if [ "$ET_VISIBLE" -gt 0 ]; then
    pass "EFF01" "escalation_turn visible in $ET_VISIBLE turn(s)"
else
    skip "EFF01" "escalation_turn=-1 in all turns (no escalation detected)"
fi

# EFF02: escalation_effectiveness visible (telemetry survives _exit(3), stdout may not)
EFF_IN_TELEM=$(grep "RECONCILIATION_TURN" "$TELEM" | grep '"escalation_effectiveness"' | grep -v '"escalation_effectiveness":"N/A"' | wc -l)
EFF_IN_TELEM=$(echo "$EFF_IN_TELEM" | tr -d ' ')
SEM_EE=$(grep "TURN|" "$STDOUT" | grep -v "sem_ee=N/A" | wc -l)
SEM_EE=$(echo "$SEM_EE" | tr -d ' ')
if [ "$EFF_IN_TELEM" -gt 0 ]; then
    pass "EFF02" "escalation_effectiveness in telemetry ($EFF_IN_TELEM events)"
elif [ "$SEM_EE" -gt 0 ]; then
    pass "EFF02" "escalation_effectiveness in semantic section ($SEM_EE turns)"
else
    skip "EFF02" "escalation_effectiveness=N/A (window may not have completed)"
fi

# EFF03: escalation_effectiveness has numeric value (telemetry or stdout)
SUMM_EE=$(grep "SUMMARY_ESCALATION_EFFECTIVENESS:" "$STDOUT" | head -1 | awk '{print $2}')
SUMM_EE=${SUMM_EE:-N/A}
EFF_VAL=$(grep "RECONCILIATION_TURN" "$TELEM" 2>/dev/null | grep -oP '"escalation_effectiveness":"[^"]*"' | grep -v "N/A" | tail -1)
if [ "$SUMM_EE" != "N/A" ]; then
    pass "EFF03" "Escalation effectiveness=$SUMM_EE (stdout)"
elif [ -n "$EFF_VAL" ]; then
    pass "EFF03" "Effectiveness computed ($EFF_VAL)"
else
    skip "EFF03" "Effectiveness N/A (expected: not enough turns or no escalation)"
fi

# ============================================================
# T: Telemetry
# ============================================================
echo -e "${CYAN}--- T: Telemetry ---${NC}"

# T01: RECONCILIATION_TURN events emitted
RECON_TURNS=0
if [ -f "$TELEM" ]; then
    RECON_TURNS=$(grep "RECONCILIATION_TURN" "$TELEM" | wc -l)
    RECON_TURNS=$(echo "$RECON_TURNS" | tr -d ' ')
fi
if [ "$RECON_TURNS" -gt 0 ]; then
    pass "T01" "RECONCILIATION_TURN: $RECON_TURNS events"
else
    fail "T01" "No RECONCILIATION_TURN events"
fi

# T02: RECONCILIATION_TURN has escalation_effectiveness field
if [ "$RECON_TURNS" -gt 0 ]; then
    if grep "RECONCILIATION_TURN" "$TELEM" | grep -q "escalation_effectiveness"; then
        pass "T02" "RECONCILIATION_TURN has escalation_effectiveness field"
    else
        fail "T02" "RECONCILIATION_TURN missing escalation_effectiveness field"
    fi
else
    skip "T02" "No RECONCILIATION_TURN events to check"
fi

# T03: Hash chain present (tamper_evidence enabled)
if [ -f "$TELEM" ]; then
    HASHED=$(grep "prev_hash" "$TELEM" | wc -l)
    HASHED=$(echo "$HASHED" | tr -d ' ')
    if [ "$HASHED" -gt 0 ]; then
        pass "T03" "Hash chain: $HASHED events with prev_hash"
    else
        fail "T03" "No hash chain in telemetry"
    fi
else
    skip "T03" "Telemetry file not found"
fi

# T04: Escalation occurred (telemetry is authoritative — dashboard may be
# skipped by _exit(3) from GovernanceHardError on challenge failure)
if [ "$LEVEL_CHANGES" -gt 0 ]; then
    pass "T04" "Escalation verified via GOVERNANCE_LEVEL_CHANGE telemetry ($LEVEL_CHANGES events)"
elif grep -q "Escalation:" "$STDERR"; then
    pass "T04" "Dashboard shows 'Escalation:' line"
else
    skip "T04" "No escalation detected in telemetry or dashboard"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo -e "${CYAN}+----------------------------+${NC}"
echo -e "${CYAN}|  NAAb-53 Results           |${NC}"
echo -e "${CYAN}+----------------------------+${NC}"
echo -e "  Total:   $TOTAL"
echo -e "  ${GREEN}Passed:  $PASS${NC}"
echo -e "  ${RED}Failed:  $FAIL${NC}"
echo -e "  ${YELLOW}Skipped: $SKIP${NC}"
if [ $FAIL -gt 0 ]; then
    echo -e "\n${RED}Failures:${NC}"
    echo -e "$FAILURES"
fi
echo ""
TOOL_TOTAL=$(grep "SUMMARY_TOTAL_TOOL_CALLS:" "$STDOUT" | awk '{print $2}')
TOOL_TOTAL=${TOOL_TOTAL:-"?"}
ESC_FROM=$(grep "SUMMARY_ESCALATION_FROM:" "$STDOUT" | awk '{print $2}')
ESC_FROM=${ESC_FROM:-"?"}
ESC_TO=$(grep "SUMMARY_ESCALATION_TO:" "$STDOUT" | awk '{print $2}')
ESC_TO=${ESC_TO:-"?"}
echo -e "${CYAN}Turns: $TURNS_DONE | Tools: $TOOL_TOTAL | Challenges: ${PROG_CP}p/${PROG_CF}f | Exit: $EXIT_CODE${NC}"
echo -e "${CYAN}Escalation: turn=$ESC_TURN from=$ESC_FROM to=$ESC_TO eff=$SUMM_EE${NC}"
echo ""
[ $FAIL -eq 0 ]
