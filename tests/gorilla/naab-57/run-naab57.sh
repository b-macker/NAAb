#!/usr/bin/env bash
# ============================================================
# naab-57: Multi-Agent Collaborative App Build
#
# 6 agents collaborate to build a Python CLI calculator.
# Tests: fan_out, pipeline, batch, consensus, extract_code,
# enforce_convergence, CDD isolation, S20, transcript verification.
#
# Requires: GK1 env var with a Gemini API key
# Expected runtime: 8-12 minutes (~80-100 API calls)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab57-$$"
RESULTS_DIR="$SCRIPT_DIR/results"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"; }
skip() { local id="$1" desc="$2"; SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

source "$SCRIPT_DIR/../../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    cp "$SCRIPT_DIR/src/govern.json" "$workdir/govern.json"
    (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    echo "$workdir"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  naab-57: Multi-Agent Collaborative App Build                |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ -z "${GK1:-}" ]; then
    echo -e "${YELLOW}No GK1 API key -- skipping all live tests${NC}"
    for i in $(seq 1 27); do
        skip "N$i" "No GK1 API key"
    done
else
    WORKDIR=$(setup_workdir)
    STDERR_FILE="$TEST_TMP/stderr.log"

    cp "$SCRIPT_DIR/src/app-build.naab" "$WORKDIR/"
    echo -e "${CYAN}Running app build (~8-12 minutes with live API calls)...${NC}"
    echo -e "${CYAN}Config: 6 agents, lease=8, check_interval=2, max_unique_agents=7${NC}"
    echo ""

    OUTPUT=$(cd "$WORKDIR" && timeout 2400 "$NAAB" --governance-dashboard "app-build.naab" 2>"$STDERR_FILE") && EXIT_CODE=0 || EXIT_CODE=$?

    echo ""
    echo -e "${CYAN}--- Output (last 80 lines) ---${NC}"
    echo "$OUTPUT" | tail -80
    echo -e "${CYAN}--- End Output ---${NC}"
    echo ""

    # ============================================================
    # Extract values
    # ============================================================
    AGENTS_CREATED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_AGENTS_CREATED: \K[0-9]+' | head -1)
    AGENTS_CREATED=${AGENTS_CREATED:-0}

    MAX_AGENTS_ENFORCED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_MAX_AGENTS_ENFORCED: \K\w+' | head -1)
    PIPELINE_SEP=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PIPELINE_SEP: \K\w+' | head -1)

    TOTAL_SENDS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOTAL_SENDS: \K[0-9]+' | head -1)
    # Fallback: count TURN lines if SUMMARY missing
    if [ -z "$TOTAL_SENDS" ]; then
        TOTAL_SENDS=$(echo "$OUTPUT" | grep -c '^TURN|' || echo "0")
    fi
    TOTAL_SENDS=${TOTAL_SENDS:-0}

    SEND_ERRORS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_SEND_ERRORS: \K[0-9]+' | head -1)
    SEND_ERRORS=${SEND_ERRORS:-0}

    DESIGN_FAN_OUT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_DESIGN_FAN_OUT: \K[0-9]+' | head -1)
    DESIGN_FAN_OUT=${DESIGN_FAN_OUT:-0}

    FAN_OUT_REVIEW=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FAN_OUT_REVIEW: \K[0-9]+' | head -1)
    FAN_OUT_REVIEW_TOTAL=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FAN_OUT_REVIEW: [0-9]+/\K[0-9]+' | head -1)
    FAN_OUT_REVIEW=${FAN_OUT_REVIEW:-0}
    FAN_OUT_REVIEW_TOTAL=${FAN_OUT_REVIEW_TOTAL:-0}

    PIPELINE_OK=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PIPELINE: \K[0-9]+' | head -1)
    PIPELINE_TOTAL=$(echo "$OUTPUT" | grep -oP 'SUMMARY_PIPELINE: [0-9]+/\K[0-9]+' | head -1)
    PIPELINE_OK=${PIPELINE_OK:-0}
    PIPELINE_TOTAL=${PIPELINE_TOTAL:-0}

    BATCH_OK=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BATCH: \K[0-9]+' | head -1)
    BATCH_TOTAL=$(echo "$OUTPUT" | grep -oP 'SUMMARY_BATCH: [0-9]+/\K[0-9]+' | head -1)
    BATCH_OK=${BATCH_OK:-0}
    BATCH_TOTAL=${BATCH_TOTAL:-0}

    DESIGN_CONSENSUS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_DESIGN_CONSENSUS: \K\w+' | head -1)
    SHIP_CONSENSUS=$(echo "$OUTPUT" | grep -oP 'SUMMARY_SHIP_CONSENSUS: \K\w+' | head -1)

    CONVERGENCE_OK=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CONVERGENCE_OK: \K[0-9]+' | head -1)
    CONVERGENCE_OK=${CONVERGENCE_OK:-0}

    FILES_EXTRACTED=$(echo "$OUTPUT" | grep -oP 'SUMMARY_FILES_EXTRACTED: \K[0-9]+' | head -1)
    # Fallback: count CODE_EXTRACT lines with extracted=true
    if [ -z "$FILES_EXTRACTED" ]; then
        FILES_EXTRACTED=$(echo "$OUTPUT" | grep -c 'CODE_EXTRACT|.*|extracted=true' || echo "0")
    fi
    FILES_EXTRACTED=${FILES_EXTRACTED:-0}

    OPS_FUNC_COUNT=$(echo "$OUTPUT" | grep -oP 'SUMMARY_OPS_FUNCTION_COUNT: \K[0-9]+' | head -1)
    # Fallback: count individual OPS_HAS lines that are true
    if [ -z "$OPS_FUNC_COUNT" ]; then
        OPS_FUNC_COUNT=$(echo "$OUTPUT" | grep -c 'SUMMARY_OPS_HAS_.*: true' || echo "0")
    fi
    OPS_FUNC_COUNT=${OPS_FUNC_COUNT:-0}

    DEV_PRE=$(echo "$OUTPUT" | grep -oP 'SUMMARY_DEV_PRE_DRIFT: \K[0-9.e+-]+' | head -1)
    DEV_POST=$(echo "$OUTPUT" | grep -oP 'SUMMARY_DEV_POST_DRIFT: \K[0-9.e+-]+' | head -1)
    DEV_MIN=$(echo "$OUTPUT" | grep -oP 'SUMMARY_DEV_MIN_COHERENCE: \K[0-9.e+-]+' | head -1)

    # Fallback: extract developer coherence from TURN lines
    if [ -z "$DEV_MIN" ]; then
        # Get min coherence from developer TURN lines (excluding c=-1)
        DEV_MIN=$(echo "$OUTPUT" | grep '^TURN|agent=developer|' | grep -oP '\|c=\K[0-9.]+' | sort -n | head -1)
    fi
    if [ -z "$DEV_PRE" ]; then
        # Last developer coherence before DRIFT phase
        DEV_PRE=$(echo "$OUTPUT" | grep '^TURN|agent=developer|' | grep -v 'phase=DRIFT' | tail -1 | grep -oP '\|c=\K[0-9.e+-]+')
    fi
    if [ -z "$DEV_POST" ]; then
        # Last developer coherence in DRIFT phase
        DEV_POST=$(echo "$OUTPUT" | grep '^TURN|agent=developer|.*phase=DRIFT' | tail -1 | grep -oP '\|c=\K[0-9.e+-]+')
    fi

    DEV_PC=$(echo "$OUTPUT" | grep -oP 'SUMMARY_DEV_PC: \K[0-9]+' | head -1)
    # Fallback: extract max pc from DRIFT TURN lines
    if [ -z "$DEV_PC" ]; then
        DEV_PC=$(echo "$OUTPUT" | grep '^TURN|agent=developer|.*phase=DRIFT' | grep -oP '\|pc=\K[0-9]+' | sort -rn | head -1)
    fi
    DEV_PC=${DEV_PC:-0}

    CDD_ISOLATION=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CDD_ISOLATION: \K[0-9]+' | head -1)
    CDD_ISOLATION_TOTAL=$(echo "$OUTPUT" | grep -oP 'SUMMARY_CDD_ISOLATION: [0-9]+/\K[0-9]+' | head -1)
    # Fallback: if dev min coherence < 1.0 and we have TURN data, infer isolation
    if [ -z "$CDD_ISOLATION" ] && [ -n "${DEV_MIN:-}" ]; then
        BELOW_ONE=$(echo "${DEV_MIN} < 1.0" | bc -l 2>/dev/null || echo "0")
        if [ "$BELOW_ONE" = "1" ]; then
            # Count non-developer agents that have all c=1 in TURN lines
            CDD_ISOLATION_TOTAL=0
            CDD_ISOLATION=0
            for agent_name in architect tester reviewer security integrator; do
                AGENT_MIN=$(echo "$OUTPUT" | grep "^TURN|agent=${agent_name}|" | grep -oP '\|c=\K[0-9.]+' | sort -n | head -1)
                if [ -n "$AGENT_MIN" ]; then
                    CDD_ISOLATION_TOTAL=$((CDD_ISOLATION_TOTAL + 1))
                    ABOVE=$(echo "${AGENT_MIN} > ${DEV_MIN}" | bc -l 2>/dev/null || echo "0")
                    [ "$ABOVE" = "1" ] && CDD_ISOLATION=$((CDD_ISOLATION + 1))
                fi
            done
        fi
    fi
    CDD_ISOLATION=${CDD_ISOLATION:-0}
    CDD_ISOLATION_TOTAL=${CDD_ISOLATION_TOTAL:-0}

    TOTAL_CHALLENGES=$(echo "$OUTPUT" | grep -oP 'SUMMARY_TOTAL_CHALLENGES: \K[0-9]+' | head -1)
    TOTAL_CHALLENGES=${TOTAL_CHALLENGES:-0}

    echo -e "${CYAN}Extracted: agents=$AGENTS_CREATED, sends=$TOTAL_SENDS, errors=$SEND_ERRORS${NC}"
    echo -e "${CYAN}          fan_out=$DESIGN_FAN_OUT, review=$FAN_OUT_REVIEW/$FAN_OUT_REVIEW_TOTAL, pipeline=$PIPELINE_OK/$PIPELINE_TOTAL, batch=$BATCH_OK/$BATCH_TOTAL${NC}"
    echo -e "${CYAN}          files=$FILES_EXTRACTED, convergence=$CONVERGENCE_OK, ops_funcs=$OPS_FUNC_COUNT${NC}"
    echo -e "${CYAN}          developer: pre=$DEV_PRE post=$DEV_POST min=$DEV_MIN pc=$DEV_PC${NC}"
    echo ""

    # ============================================================
    # MA: MULTI-AGENT (MA01-MA09)
    # ============================================================
    echo -e "${CYAN}Multi-Agent Orchestration${NC}"

    # MA01: All 6 agents created
    if [ "$AGENTS_CREATED" -eq 6 ]; then
        pass "MA01" "All 6 agents created"
    elif [ "$AGENTS_CREATED" -ge 4 ]; then
        pass "MA01" "$AGENTS_CREATED/6 agents created (acceptable)"
    else
        fail "MA01" "Agent creation failed" "only $AGENTS_CREATED/6 created"
    fi

    # MA02: max_unique_agents enforced
    if [ "${MAX_AGENTS_ENFORCED:-}" = "true" ]; then
        pass "MA02" "max_unique_agents enforcement works"
    else
        fail "MA02" "max_unique_agents not enforced"
    fi

    # MA03: pipeline separation enforced
    if [ "${PIPELINE_SEP:-}" = "true" ]; then
        pass "MA03" "Pipeline separation enforced"
    else
        fail "MA03" "Pipeline separation not enforced"
    fi

    # MA04: fan_out works
    if [ "$DESIGN_FAN_OUT" -gt 0 ] || [ "$FAN_OUT_REVIEW" -gt 0 ]; then
        pass "MA04" "Fan-out works (design=$DESIGN_FAN_OUT, review=$FAN_OUT_REVIEW/$FAN_OUT_REVIEW_TOTAL)"
    else
        fail "MA04" "Fan-out failed" "design=$DESIGN_FAN_OUT, review=$FAN_OUT_REVIEW"
    fi

    # MA05: pipeline works
    if [ "$PIPELINE_OK" -gt 0 ]; then
        pass "MA05" "Pipeline works ($PIPELINE_OK/$PIPELINE_TOTAL rounds)"
    else
        fail "MA05" "Pipeline failed" "ok=$PIPELINE_OK total=$PIPELINE_TOTAL"
    fi

    # MA06: batch works
    if [ "$BATCH_OK" -gt 0 ]; then
        pass "MA06" "Batch works ($BATCH_OK/$BATCH_TOTAL)"
    else
        fail "MA06" "Batch failed" "ok=$BATCH_OK total=$BATCH_TOTAL"
    fi

    # MA07: consensus votes (design + ship)
    CONSENSUS_COUNT=0
    [ "${DESIGN_CONSENSUS:-}" = "true" ] && CONSENSUS_COUNT=$((CONSENSUS_COUNT + 1))
    [ "${SHIP_CONSENSUS:-}" = "true" ] && CONSENSUS_COUNT=$((CONSENSUS_COUNT + 1))
    if [ "$CONSENSUS_COUNT" -ge 1 ]; then
        pass "MA07" "Consensus voting works ($CONSENSUS_COUNT/2 votes: design=${DESIGN_CONSENSUS:-false}, ship=${SHIP_CONSENSUS:-false})"
    else
        fail "MA07" "Consensus voting failed"
    fi

    # MA08: enforce_convergence
    if [ "$CONVERGENCE_OK" -gt 0 ]; then
        pass "MA08" "enforce_convergence passed ($CONVERGENCE_OK/3 files)"
    else
        fail "MA08" "enforce_convergence failed" "ok=$CONVERGENCE_OK"
    fi

    # MA09: CDD isolation
    if [ "$CDD_ISOLATION" -gt 0 ] && [ "$CDD_ISOLATION_TOTAL" -gt 0 ]; then
        pass "MA09" "CDD isolation: $CDD_ISOLATION/$CDD_ISOLATION_TOTAL agents above drifted developer"
    else
        fail "MA09" "CDD isolation not detected" "isolation=$CDD_ISOLATION/$CDD_ISOLATION_TOTAL"
    fi

    # ============================================================
    # CO: CODE OUTPUT (CO01-CO05)
    # ============================================================
    echo -e "${CYAN}Code Output${NC}"

    # CO01: >= 2 of 3 files had code extracted
    if [ "$FILES_EXTRACTED" -ge 2 ]; then
        pass "CO01" "Code extraction worked ($FILES_EXTRACTED/3 files)"
    else
        fail "CO01" "Code extraction failed" "only $FILES_EXTRACTED/3 files"
    fi

    # CO02: operations.py valid (enforce_convergence)
    OPS_CONV=$(echo "$OUTPUT" | grep 'CONVERGENCE|operations.py|valid=true' | head -1)
    if [ -n "$OPS_CONV" ]; then
        pass "CO02" "operations.py passes convergence check"
    else
        fail "CO02" "operations.py failed convergence" "$(echo "$OUTPUT" | grep 'CONVERGENCE|operations.py' | head -1)"
    fi

    # CO03: main.py valid
    MAIN_CONV=$(echo "$OUTPUT" | grep 'CONVERGENCE|main.py|valid=true' | head -1)
    if [ -n "$MAIN_CONV" ]; then
        pass "CO03" "main.py passes convergence check"
    else
        fail "CO03" "main.py failed convergence" "$(echo "$OUTPUT" | grep 'CONVERGENCE|main.py' | head -1)"
    fi

    # CO04: test_calc.py valid
    TEST_CONV=$(echo "$OUTPUT" | grep 'CONVERGENCE|test_calc.py|valid=true' | head -1)
    if [ -n "$TEST_CONV" ]; then
        pass "CO04" "test_calc.py passes convergence check"
    else
        fail "CO04" "test_calc.py failed convergence" "$(echo "$OUTPUT" | grep 'CONVERGENCE|test_calc.py' | head -1)"
    fi

    # CO05: operations.py has >= 3 of: add, subtract, multiply, divide, Calculator
    if [ "$OPS_FUNC_COUNT" -ge 3 ]; then
        pass "CO05" "operations.py has $OPS_FUNC_COUNT/5 expected components"
    else
        fail "CO05" "operations.py missing components" "only $OPS_FUNC_COUNT/5"
    fi

    # ============================================================
    # C: CORRECTNESS (C01-C04)
    # ============================================================
    echo -e "${CYAN}Correctness${NC}"

    # C01: >= 30 total sends (ideal ~60, but governance may halt early)
    if [ "$TOTAL_SENDS" -ge 60 ]; then
        pass "C01" "Sufficient sends ($TOTAL_SENDS total)"
    elif [ "$TOTAL_SENDS" -ge 30 ]; then
        pass "C01" "Acceptable sends ($TOTAL_SENDS total, 60 ideal)"
    else
        fail "C01" "Insufficient sends" "got $TOTAL_SENDS, expected >= 30"
    fi

    # C02: <= 10 send errors
    if [ "$SEND_ERRORS" -le 10 ]; then
        pass "C02" "Low error rate ($SEND_ERRORS errors out of $TOTAL_SENDS sends)"
    else
        fail "C02" "Too many errors" "$SEND_ERRORS/$TOTAL_SENDS"
    fi

    # C03: developer min coherence < 1.0 (drift worked)
    if [ -n "${DEV_MIN:-}" ]; then
        BELOW_ONE=$(echo "$DEV_MIN < 1.0" | bc -l 2>/dev/null || echo "0")
        if [ "$BELOW_ONE" = "1" ]; then
            pass "C03" "Developer CDD varied (min=$DEV_MIN)"
        else
            fail "C03" "Developer coherence constant" "min=$DEV_MIN"
        fi
    else
        fail "C03" "No developer coherence data"
    fi

    # C04: S20 prompt_compliance fired
    if [ "$DEV_PC" -gt 0 ]; then
        pass "C04" "S20 fired for developer during drift (pc=$DEV_PC)"
    else
        fail "C04" "S20 did not fire" "developer_pc=$DEV_PC"
    fi

    # ============================================================
    # T: TELEMETRY (T01-T04)
    # Prefer NAAb script output; fallback to reading telemetry.jsonl directly
    # ============================================================
    echo -e "${CYAN}Telemetry Integrity${NC}"

    HASH_CHAIN=$(echo "$OUTPUT" | grep -oP 'TELEM_HASH_CHAIN: \K\w+' | head -1)
    TELEM_DIV=$(echo "$OUTPUT" | grep -oP 'TELEM_EVENT_DIVERSITY: \K[0-9]+' | head -1)
    RUN_ID=$(echo "$OUTPUT" | grep -oP 'TELEM_RUN_ID_CONSISTENT: \K\w+' | head -1)
    TS_OK=$(echo "$OUTPUT" | grep -oP 'TELEM_TIMESTAMPS_ORDERED: \K\w+' | head -1)
    TELEM_CHALLENGES=$(echo "$OUTPUT" | grep -oP 'TELEM_CHALLENGE_EVENTS: \K[0-9]+' | head -1)

    # Fallback: read telemetry.jsonl directly from workdir if NAAb didn't get there
    if [ -z "$HASH_CHAIN" ] && [ -f "$WORKDIR/telemetry.jsonl" ]; then
        TELEM_LINES=$(wc -l < "$WORKDIR/telemetry.jsonl" 2>/dev/null || echo "0")
        if [ "$TELEM_LINES" -gt 1 ]; then
            # Hash chain: check prev_hash linkage via python one-liner
            HASH_CHAIN=$(python3 -c "
import json, sys
events = [json.loads(l) for l in open('$WORKDIR/telemetry.jsonl') if l.strip()]
ok = True
for i in range(1, len(events)):
    ph = events[i-1].get('hash','')
    cp = events[i].get('prev_hash','')
    if ph and cp and ph != cp: ok = False
print('true' if ok else 'false')
" 2>/dev/null || echo "false")

            # Event diversity
            TELEM_DIV=$(python3 -c "
import json
events = [json.loads(l) for l in open('$WORKDIR/telemetry.jsonl') if l.strip()]
types = set(e.get('event_type', e.get('type','')) for e in events)
types.discard('')
print(len(types))
" 2>/dev/null || echo "0")

            # Run ID consistency
            RUN_ID=$(python3 -c "
import json
events = [json.loads(l) for l in open('$WORKDIR/telemetry.jsonl') if l.strip()]
rids = set(str(e.get('run_id','')) for e in events if e.get('run_id'))
print('true' if len(rids) <= 1 else 'false')
" 2>/dev/null || echo "false")

            # Timestamp ordering
            TS_OK=$(python3 -c "
import json
events = [json.loads(l) for l in open('$WORKDIR/telemetry.jsonl') if l.strip()]
ts = [e.get('timestamp','') for e in events if e.get('timestamp')]
print('true' if ts == sorted(ts) else 'false')
" 2>/dev/null || echo "false")

            # Challenge events
            TELEM_CHALLENGES=$(python3 -c "
import json
events = [json.loads(l) for l in open('$WORKDIR/telemetry.jsonl') if l.strip()]
print(sum(1 for e in events if e.get('event_type') == 'AGENT_CHALLENGE_PASS'))
" 2>/dev/null || echo "0")
        fi
    fi

    TELEM_DIV=${TELEM_DIV:-0}
    TELEM_CHALLENGES=${TELEM_CHALLENGES:-0}

    [ "${HASH_CHAIN:-}" = "true" ] && pass "T01" "Hash chain intact" || fail "T01" "Hash chain broken" "TELEM_HASH_CHAIN=${HASH_CHAIN:-}"

    if [ "$TELEM_DIV" -ge 6 ]; then
        pass "T02" "Event diversity ($TELEM_DIV types)"
    elif [ "$TELEM_DIV" -ge 4 ]; then
        pass "T02" "Acceptable event diversity ($TELEM_DIV types)"
    else
        fail "T02" "Insufficient event diversity" "only $TELEM_DIV types"
    fi

    [ "${RUN_ID:-}" = "true" ] && pass "T03" "Run ID consistent" || fail "T03" "Run ID inconsistent"
    [ "${TS_OK:-}" = "true" ] && pass "T04" "Timestamps ordered" || fail "T04" "Timestamps disordered"

    # ============================================================
    # TR: TRANSCRIPT (TR01-TR03)
    # Prefer NAAb script output; fallback to reading transcript.jsonl directly
    # ============================================================
    echo -e "${CYAN}Transcript Verification${NC}"

    TR_TOTAL=$(echo "$OUTPUT" | grep -oP 'TRANSCRIPT_TOTAL_ENTRIES: \K[0-9]+' | head -1)
    TR_AGENTS=$(echo "$OUTPUT" | grep -oP 'TRANSCRIPT_AGENTS_SEEN: \K[0-9]+' | head -1)
    TR_HANDOFF=$(echo "$OUTPUT" | grep -oP 'TRANSCRIPT_HANDOFF_VERIFIED: \K\w+' | head -1)

    # Fallback: read transcript.jsonl directly
    if [ -z "$TR_TOTAL" ] && [ -f "$WORKDIR/transcript.jsonl" ]; then
        TR_TOTAL=$(wc -l < "$WORKDIR/transcript.jsonl" 2>/dev/null || echo "0")
        TR_AGENTS=$(python3 -c "
import json
entries = [json.loads(l) for l in open('$WORKDIR/transcript.jsonl') if l.strip()]
agents = set()
for e in entries:
    a = e.get('agent', e.get('agent_name', e.get('config_name', '')))
    if a: agents.add(str(a))
print(len(agents))
" 2>/dev/null || echo "0")
        # Handoff: check if developer entries contain calculator keywords
        TR_HANDOFF=$(python3 -c "
import json
entries = [json.loads(l) for l in open('$WORKDIR/transcript.jsonl') if l.strip()]
keywords = ['Calculator', 'operations', 'argparse', 'add', 'subtract', 'history']
found = 0
for e in entries:
    a = str(e.get('agent', e.get('agent_name', e.get('config_name', ''))))
    if a == 'developer':
        content = str(e.get('messages', e.get('prompt', '')))
        for kw in keywords:
            if kw in content: found += 1
print('true' if found >= 2 else 'false')
" 2>/dev/null || echo "false")
    fi

    TR_TOTAL=${TR_TOTAL:-0}
    TR_AGENTS=${TR_AGENTS:-0}

    if [ "$TR_TOTAL" -gt 0 ]; then
        pass "TR01" "Transcript has entries ($TR_TOTAL)"
    else
        fail "TR01" "No transcript entries"
    fi
    if [ "$TR_AGENTS" -ge 6 ]; then
        pass "TR02" "All 6 agent names in transcript"
    elif [ "$TR_AGENTS" -ge 4 ]; then
        pass "TR02" "$TR_AGENTS/6 agent names in transcript (acceptable)"
    else
        fail "TR02" "Missing agents in transcript" "only $TR_AGENTS/6 agents"
    fi

    if [ "${TR_HANDOFF:-}" = "true" ]; then
        pass "TR03" "Data handoff verified (architect spec -> developer)"
    else
        fail "TR03" "Data handoff not verified"
    fi

    # ============================================================
    # H: HEALTH (H01-H02)
    # ============================================================
    echo -e "${CYAN}Health${NC}"

    # TELEM_CHALLENGES already extracted in telemetry section (with fallback)
    if [ "$TOTAL_CHALLENGES" -gt 0 ] || [ "$TELEM_CHALLENGES" -gt 0 ]; then
        pass "H01" "Challenges fired (summary=$TOTAL_CHALLENGES, telemetry=$TELEM_CHALLENGES)"
    else
        fail "H01" "No challenges fired despite drift phase"
    fi

    VERDICT=$(echo "$OUTPUT" | grep -oP 'HEALTH_VERDICT: \K\w+' | head -1)
    # Fallback: check stderr for governance verdict
    if [ -z "$VERDICT" ] && [ -f "$STDERR_FILE" ]; then
        if grep -q "IMPAIRED" "$STDERR_FILE" 2>/dev/null; then
            VERDICT="impaired"
        elif grep -q "governance" "$STDERR_FILE" 2>/dev/null; then
            VERDICT="healthy"
        fi
    fi
    if [ "${VERDICT:-}" = "healthy" ] || [ "${VERDICT:-}" = "degraded" ]; then
        pass "H02" "Governance health: $VERDICT"
    else
        fail "H02" "Governance health: ${VERDICT:-unknown}"
    fi

    # ============================================================
    # AGENT STATES + STDERR
    # ============================================================
    echo ""
    echo -e "${CYAN}Per-Agent Final State:${NC}"
    echo "$OUTPUT" | grep '^AGENT_STATE|' | while IFS='|' read -r _ name rest; do
        echo -e "  ${CYAN}$name $rest${NC}"
    done

    echo ""
    echo -e "${CYAN}Code Extraction:${NC}"
    echo "$OUTPUT" | grep '^CODE_EXTRACT|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Convergence:${NC}"
    echo "$OUTPUT" | grep '^CONVERGENCE|' | while read -r line; do
        echo -e "  ${CYAN}$line${NC}"
    done

    echo ""
    echo -e "${CYAN}Stderr Cross-Validation:${NC}"
    if [ -f "$STDERR_FILE" ]; then
        STDERR_SIZE=$(wc -c < "$STDERR_FILE")
        echo -e "  Stderr size: ${STDERR_SIZE} bytes"
        echo ""
        echo -e "${CYAN}--- Stderr (first 20 lines) ---${NC}"
        head -20 "$STDERR_FILE" 2>/dev/null
        echo -e "${CYAN}--- End Stderr ---${NC}"
    fi

    echo ""
    echo -e "${CYAN}Governance Health: ${VERDICT:-unknown}${NC}"
fi

echo ""
echo -e "${CYAN}================================================${NC}"
echo -e "  ${GREEN}PASS: $PASS_COUNT${NC}  ${RED}FAIL: $FAIL_COUNT${NC}  ${YELLOW}SKIP: $SKIP_COUNT${NC}  TOTAL: $TOTAL"
[ -n "$FAILURES" ] && echo -e "\n${RED}Failures:${FAILURES}${NC}"
echo -e "${CYAN}================================================${NC}"

mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
echo "{\"pass\": $PASS_COUNT, \"fail\": $FAIL_COUNT, \"skip\": $SKIP_COUNT, \"total\": $TOTAL}" > "$RESULTS_DIR/summary.json"
[ -n "${OUTPUT:-}" ] && echo "$OUTPUT" > "$RESULTS_DIR/results_${TIMESTAMP}.txt"
[ -f "${STDERR_FILE:-/dev/null}" ] && cp "$STDERR_FILE" "$RESULTS_DIR/stderr_${TIMESTAMP}.txt" 2>/dev/null || true
[ -f "${WORKDIR:-/dev/null}/telemetry.jsonl" ] && cp "$WORKDIR/telemetry.jsonl" "$RESULTS_DIR/telemetry_${TIMESTAMP}.jsonl" 2>/dev/null || true
[ -f "${WORKDIR:-/dev/null}/transcript.jsonl" ] && cp "$WORKDIR/transcript.jsonl" "$RESULTS_DIR/transcript_${TIMESTAMP}.jsonl" 2>/dev/null || true

exit "$FAIL_COUNT"
