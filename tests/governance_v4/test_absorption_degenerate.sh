#!/usr/bin/env bash
# ============================================================
# test_absorption_degenerate.sh — S23 response_degenerate + adaptive
# absorption cap + propose candidate diversity
#
# Group A: S23 defaults OFF — terse responses fire nothing (no behavior
#          change for existing configs).
# Group B: S23 on, adaptive baselining off — degenerate responses pay the
#          flat penalty.
# Group C: S23 on + adaptive baselining, unlimited absorption (limit 0) —
#          structurally terse agents self-absorb after the baseline window
#          (the terse-by-design judge case; no per-agent override needed).
# Group D: C + adaptive_absorption_limit=2 — persistent absorbed firing
#          starts paying base_penalty after 2 consecutive absorbed checks
#          (the "baseline normalized my drift" gap from the Jul 19 run).
# Group E: raising adaptive_absorption_limit mid-run = ratchet violation
#          (0 = unlimited = loosest).
# Group F: agent.propose() candidate diversity — temperature steps per
#          candidate (visible in the stub's captured request bodies).
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/absorb-degen-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

IS_WINDOWS=false
if [[ "$(uname -s)" == MINGW* ]] || [[ "$(uname -s)" == MSYS* ]] || [[ -n "${WINDIR:-}" ]]; then IS_WINDOWS=true; fi

# Stub-backed HTTP tests hang on Windows/MSYS2 due to signal propagation and
# process cleanup issues. Skip entirely — Linux CI validates the behavior.
if $IS_WINDOWS; then
    echo "  SKIP: test_absorption_degenerate.sh — stub-backed tests not supported on Windows/MSYS2"
    exit 0
fi

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
export FAKE_KEY_ABSDEG="fake-key-absorb-degen"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

start_stub() {  # $1=fixture $2=workdir
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do grep -q READY "$2/stub.log" 2>/dev/null && return 0; sleep 0.1; done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# Six terse (2-token) responses — degenerate by the S23 threshold (8 tokens).
TERSE_FIXTURE='{"responses": [
  {"content": "ok", "output_tokens": 2},
  {"content": "ok", "output_tokens": 2},
  {"content": "ok", "output_tokens": 2},
  {"content": "ok", "output_tokens": 2},
  {"content": "ok", "output_tokens": 2},
  {"content": "ok", "output_tokens": 2}
]}'

# All signals off except (optionally) response_degenerate; adaptive baseline
# and absorption limit parameterized.
mk_govern() {  # $1=port $2=response_degenerate(true/false) $3=adaptive(true/false) $4=absorption limit $5=extra agent fields
    cat <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": $3, "adaptive_baseline_window": 2,
    "adaptive_baseline_sensitivity": 2.0,
    "adaptive_absorption_limit": $4,
    "signals": {
      "circular_actions": false, "repeated_failures": false, "scope_creep": false,
      "intent_contradictions": false, "coherence_velocity": false,
      "response_quality": false, "thinking_collapse": false,
      "semantic_stability": false, "mandate_alignment": false,
      "context_growth": false, "instruction_recall": false, "plan_drift": false,
      "entity_consistency": false, "instruction_conflict": false,
      "persona_fingerprint": false, "tool_chain_integrity": false,
      "claim_result_reconciliation": false, "prompt_compliance": false,
      "response_repetition": false, "validation_outcome": false,
      "response_degenerate": $2
    }
  },
  "agents": { "reviewer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$1",
      "api_key_env": "FAKE_KEY_ABSDEG", "max_tokens": 100, "max_turns": 20${5:-} } }
}
EOF
}

SIX_SENDS='use agent
main {
    let h = agent.create("reviewer")
    let i = 0
    while i < 6 {
        let r = agent.send(h, "review the code please")
        i = i + 1
    }
    print("FINAL_COHERENCE=" + string(agent.coherence(h)))
}'

run_case() {  # $1=workdir $2=s23 $3=adaptive $4=limit → sets OUT, COH; returns 1 on stub failure
    local wdir="$1"
    mkdir -p "$wdir"
    printf '%s' "$TERSE_FIXTURE" > "$wdir/fixture.json"
    start_stub "$wdir/fixture.json" "$wdir" || return 1
    mk_govern "$STUB_PORT" "$2" "$3" "$4" "" > "$wdir/govern.json"; sign_govern "$wdir"
    printf '%s' "$SIX_SENDS" > "$wdir/test.naab"
    OUT=$(cd "$wdir" && timeout 60s "$NAAB" test.naab 2>&1) || true
    stop_stub
    COH=$(echo "$OUT" | grep FINAL_COHERENCE | sed 's/.*=//')
    return 0
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  S23 response_degenerate + absorption cap + propose diversity |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: S23 defaults OFF (config omits it entirely via default false)
# ============================================================
echo -e "${CYAN}--- Group A: default off — terse responses fire nothing ---${NC}"
if run_case "$TEST_TMP/a" false false 0; then
if [ -n "${COH:-}" ] && awk "BEGIN{exit !($COH == 1.0)}"; then
    pass "A-01" "Coherence untouched with S23 off ($COH)"
else
    fail "A-01" "Default-off regression" "coherence=$COH"
fi
if ! grep -E '"event_type":"CDD_TURN"' "$TEST_TMP/a/tele.jsonl" 2>/dev/null | grep -q 'response_degenerate'; then
    pass "A-02" "response_degenerate absent from telemetry when disabled"
else
    fail "A-02" "signal fired while disabled"
fi
else skip "A-01" "stub failed"; fi

# ============================================================
# Group B: S23 on, no baselining — flat penalty
# ============================================================
echo -e "${CYAN}--- Group B: enabled without baselining — pays flat ---${NC}"
if run_case "$TEST_TMP/b" true false 0; then
if [ -n "${COH:-}" ] && awk "BEGIN{exit !($COH < 0.85)}"; then
    pass "B-01" "Degenerate responses penalized without baselining ($COH)"
else
    fail "B-01" "No penalty for degenerate responses" "coherence=$COH"
fi
if grep -E '"event_type":"CDD_TURN"' "$TEST_TMP/b/tele.jsonl" 2>/dev/null | grep -q 'response_degenerate'; then
    pass "B-02" "response_degenerate fired in CDD_TURN signals_detail"
else
    fail "B-02" "signal never fired"
fi
else skip "B-01" "stub failed"; fi

# ============================================================
# Group C: baselining + unlimited absorption — terse-by-design self-absorbs
# ============================================================
echo -e "${CYAN}--- Group C: baselining absorbs terse-by-design agents ---${NC}"
COH_C=""
if run_case "$TEST_TMP/c" true true 0; then
COH_C="$COH"
if [ -n "${COH_C:-}" ] && awk "BEGIN{exit !($COH_C == 1.0)}"; then
    pass "C-01" "Structural terseness fully absorbed under baselining ($COH_C)"
else
    fail "C-01" "Baseline did not absorb structural terseness" "coherence=$COH_C"
fi
else skip "C-01" "stub failed"; fi

# ============================================================
# Group D: absorption cap — persistent absorbed firing resumes paying
# ============================================================
echo -e "${CYAN}--- Group D: absorption cap catches persistent absorbed drift ---${NC}"
if run_case "$TEST_TMP/d" true true 2; then
COH_D="$COH"
if [ -n "${COH_D:-}" ] && awk "BEGIN{exit !($COH_D < 1.0)}"; then
    pass "D-01" "Absorption cap resumed penalties after 2 absorbed checks ($COH_D)"
else
    fail "D-01" "Cap never triggered" "coherence=$COH_D"
fi
if [ -n "${COH_C:-}" ] && [ -n "${COH_D:-}" ] && awk "BEGIN{exit !($COH_D < $COH_C)}"; then
    pass "D-02" "Capped run strictly below uncapped run ($COH_D < $COH_C)"
else
    fail "D-02" "No difference between capped and uncapped" "c=$COH_C d=$COH_D"
fi
else skip "D-01" "stub failed"; fi

# ============================================================
# Group E: raising the absorption limit mid-run = ratchet violation
# ============================================================
echo -e "${CYAN}--- Group E: raising absorption limit is a ratchet violation ---${NC}"
if $IS_WINDOWS; then
    skip "E-01" "mid-run file swap requires POSIX file semantics"
else
WDIR="$TEST_TMP/e"; mkdir -p "$WDIR"
printf '%s' "$TERSE_FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || skip "E-01" "stub failed"
if [ -n "$STUB_PID" ]; then
presign() {
    local pdir="$TEST_TMP/.presign"; mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"; sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""; rm -rf "$pdir"
}
LOOSE="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"tele.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1,\"adaptive_baseline_enabled\":true,\"adaptive_absorption_limit\":10},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"reviewer\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_ABSDEG\",\"max_tokens\":100,\"max_turns\":20}}}"
LOOSE_SIG=$(presign "$LOOSE")
if [ -z "$LOOSE_SIG" ]; then skip "E-01" "presign failed"; stop_stub; else
export NAAB_LOOSE_JSON="$LOOSE" NAAB_LOOSE_SIG="$LOOSE_SIG"
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": true, "adaptive_absorption_limit": 2 },
  "capabilities": { "shell": { "enabled": true } },
  "agents": { "reviewer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_ABSDEG", "max_tokens": 100, "max_turns": 20 } }
}
EOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("reviewer")
    let r1 = agent.send(h, "review the code please")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "review the code please")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 40s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub
if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "adaptive_absorption_limit" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "E-01" "mid-run raise of adaptive_absorption_limit rejected as ratchet violation"
else
    fail "E-01" "mid-run raise not rejected" "$(grep -i 'ratchet\|reload' "$WDIR/stderr.txt" 2>/dev/null | head -2)"
fi
fi
fi
fi

# ============================================================
# Group F: propose candidate temperature diversity
# ============================================================
echo -e "${CYAN}--- Group F: propose candidates get stepped temperatures ---${NC}"
WDIR="$TEST_TMP/f"; mkdir -p "$WDIR"
printf '%s' '{"responses": [
  {"content": "candidate one implementation of divide", "output_tokens": 30},
  {"content": "candidate two implementation of divide", "output_tokens": 30},
  {"content": "candidate three implementation of divide", "output_tokens": 30}
]}' > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || skip "F-01" "stub failed"
if [ -n "$STUB_PID" ]; then
mk_govern "$STUB_PORT" false false 0 ', "propose_candidates_max": 3' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("reviewer")
    let prop = agent.propose(h, "implement divide", 3)
    print("CANDIDATES=" + string(len(prop.get("candidates") ?? [])))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if echo "$OUT" | grep -q "CANDIDATES=3"; then
    pass "F-01" "propose returned 3 candidates"
else
    fail "F-01" "propose failed" "$(echo "$OUT" | tail -2)"
fi
# Candidate 0 keeps default temp 1.0 (omitted from payload); candidates 1-2
# carry stepped temperatures 1.15 / 1.30 in the captured request bodies.
TEMPS=$(cat "$WDIR"/req_*.json 2>/dev/null | grep -o '"temperature": *[0-9.]*' | sort -u)
if echo "$TEMPS" | grep -q "1.15" && echo "$TEMPS" | grep -q "1.3"; then
    pass "F-02" "Stepped temperatures present in candidate requests ($(echo $TEMPS | tr '\n' ' '))"
else
    fail "F-02" "No temperature diversity in requests" "temps=[$TEMPS]"
fi
fi

# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
