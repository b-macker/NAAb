#!/usr/bin/env bash
# ============================================================
# test_propose_commit.sh — agent.propose / agent.commit / select_admissible
#
# Group A: propose disabled by default (fail-closed)
# Group B: propose returns N candidates with admissibility sections
# Group C: select_admissible + commit appends exactly one turn
# Group D: replay / forged nonce / stale proposal rejected
# Group E: telemetry events emitted
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/proposecommit-$$"

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
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_PROPOSE_TEST="fake-key-propose-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

start_stub() {
    local attempt
    for attempt in 1 2 3 4 5; do
        STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
        : > "$2/stub.log"
        python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" >> "$2/stub.log" 2>&1 &
        STUB_PID=$!
        for _ in $(seq 1 50); do
            grep -q READY "$2/stub.log" 2>/dev/null && return 0
            kill -0 "$STUB_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

write_govern() {  # $1=workdir $2=port
    cat > "$1/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "agents": {
        "proposer": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100,
            "max_turns": 20,
            "propose_candidates_max": 3
        },
        "plain": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100,
            "max_turns": 20
        }
    }
}
GOVEOF
    sign_govern "$1"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|        agent.propose / agent.commit / select_admissible       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

WDIR="$TEST_TMP/work"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "candidate one about summarizing quarterly revenue data", "output_tokens": 40},
  {"content": "candidate two about summarizing quarterly revenue trends", "output_tokens": 45},
  {"content": "candidate three about summarizing revenue outlook", "output_tokens": 42},
  {"content": "post commit follow up response", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "00" "stub failed to start"; exit 1; }
write_govern "$WDIR" "$STUB_PORT"

# ============================================================
# Group A: fail-closed default
# ============================================================
echo -e "${CYAN}--- Group A: propose disabled by default ---${NC}"

cat > "$WDIR/test_a.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("plain")
    try {
        let p = agent.propose(h, "hello", 2)
        print("PROPOSE_ALLOWED")
    } catch (e) {
        print("PROPOSE_DENIED")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test_a.naab 2>&1) || true
if echo "$OUTPUT" | grep -q "PROPOSE_DENIED"; then
    pass "A-01" "agent.propose fail-closed without propose_candidates_max"
else
    fail "A-01" "propose worked without being enabled" "$(echo "$OUTPUT" | head -3)"
fi

# ============================================================
# Group B + C: propose N candidates, select, commit
# ============================================================
echo -e "${CYAN}--- Group B/C: propose, select, commit ---${NC}"

cat > "$WDIR/test_bc.naab" << 'NAABEOF'
use agent
use orchestra

main {
    let h = agent.create("proposer")
    let p = agent.propose(h, "summarize the revenue data", 3)
    print("TOTAL=" + string(p.get("total")))
    print("ADMISSIBLE=" + string(p.get("admissible_count")))
    let cands = p.get("candidates")
    print("CANDS=" + string(cands.length()))

    // No history or turns committed by propose
    let msgs_before = agent.messages(h)
    print("MSGS_BEFORE=" + string(msgs_before.length()))

    let pick = orchestra.select_admissible(p)
    print("SELECTED_INDEX=" + string(pick.get("selected_index")))

    let selected = pick.get("selected")
    let r = agent.commit(h, selected)
    print("COMMIT_CONTENT=" + string(r.get("content")))

    let msgs_after = agent.messages(h)
    print("MSGS_AFTER=" + string(msgs_after.length()))

    // Replay: committing the same proposal again must fail
    try {
        let r2 = agent.commit(h, selected)
        print("REPLAY_ALLOWED")
    } catch (e) {
        print("REPLAY_DENIED")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test_bc.naab 2>&1) || true

if echo "$OUTPUT" | grep -q "TOTAL=3" && echo "$OUTPUT" | grep -q "CANDS=3"; then
    pass "B-01" "propose returned 3 candidates"
else
    fail "B-01" "candidate count wrong" "$(echo "$OUTPUT" | grep -E 'TOTAL=|CANDS=' | head -2)"
fi
if echo "$OUTPUT" | grep -q "ADMISSIBLE=3"; then
    pass "B-02" "all clean candidates admissible (OA disabled)"
else
    fail "B-02" "admissible count wrong" "$(echo "$OUTPUT" | grep ADMISSIBLE=)"
fi
if echo "$OUTPUT" | grep -q "MSGS_BEFORE=0"; then
    pass "B-03" "propose committed no conversation state"
else
    fail "B-03" "propose leaked into history" "$(echo "$OUTPUT" | grep MSGS_BEFORE)"
fi
if echo "$OUTPUT" | grep -q "SELECTED_INDEX=[0-9]"; then
    pass "C-01" "select_admissible picked a candidate"
else
    fail "C-01" "selection failed" "$(echo "$OUTPUT" | grep SELECTED_INDEX)"
fi
if echo "$OUTPUT" | grep -q "COMMIT_CONTENT=candidate"; then
    pass "C-02" "commit returned the selected candidate content"
else
    fail "C-02" "commit content wrong" "$(echo "$OUTPUT" | grep COMMIT_CONTENT | head -1)"
fi
if echo "$OUTPUT" | grep -q "MSGS_AFTER=2"; then
    pass "C-03" "commit appended exactly one user/assistant pair"
else
    fail "C-03" "history length wrong after commit" "$(echo "$OUTPUT" | grep MSGS_AFTER)"
fi
if echo "$OUTPUT" | grep -q "REPLAY_DENIED"; then
    pass "D-01" "replayed proposal rejected (single use)"
else
    fail "D-01" "replay was allowed" "$(echo "$OUTPUT" | grep REPLAY | head -1)"
fi

# ============================================================
# Group D: forged nonce + stale proposals
# ============================================================
echo -e "${CYAN}--- Group D: forged / stale proposals ---${NC}"

cat > "$WDIR/test_d.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("proposer")
    let p = agent.propose(h, "summarize the revenue data", 2)
    let cands = p.get("candidates")
    let c0 = cands[0]

    // Forge the nonce
    let forged = {
        "__proposal": "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
        "content": string(c0.get("content"))
    }
    try {
        let r = agent.commit(h, forged)
        print("FORGE_ALLOWED")
    } catch (e) {
        print("FORGE_DENIED")
    }

    // Stale: a send() invalidates outstanding proposals
    let r1 = agent.send(h, "regular message consumes state")
    try {
        let r2 = agent.commit(h, c0)
        print("STALE_ALLOWED")
    } catch (e) {
        print("STALE_DENIED")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test_d.naab 2>&1) || true

if echo "$OUTPUT" | grep -q "FORGE_DENIED"; then
    pass "D-02" "forged proposal nonce rejected"
else
    fail "D-02" "forged nonce accepted" "$(echo "$OUTPUT" | head -5)"
fi
if echo "$OUTPUT" | grep -q "STALE_DENIED"; then
    pass "D-03" "stale proposal (superseded by send) rejected"
else
    fail "D-03" "stale proposal accepted" "$(echo "$OUTPUT" | grep STALE | head -1)"
fi

# ============================================================
# Group E: telemetry
# ============================================================
echo -e "${CYAN}--- Group E: telemetry ---${NC}"

if grep -q "AGENT_PROPOSE" "$WDIR/telemetry.jsonl" 2>/dev/null; then
    pass "E-01" "AGENT_PROPOSE telemetry emitted"
else
    fail "E-01" "no AGENT_PROPOSE telemetry"
fi
if grep -q "AGENT_PROPOSAL_COMMIT" "$WDIR/telemetry.jsonl" 2>/dev/null; then
    pass "E-02" "AGENT_PROPOSAL_COMMIT telemetry emitted"
else
    fail "E-02" "no AGENT_PROPOSAL_COMMIT telemetry"
fi

stop_stub

# ============================================================
# Group F: the step-up gate is the LEASE, not the governance level
#
# Live run 12 refused agent.propose() for a handle holding 19 turns of valid
# lease after 47 passed challenges, zero failures, coherence 0.96 and no
# quarantine. The gate denied on the engine-global governance level, which no
# action the handle takes can lower — passing a challenge renews the lease and
# recovers coherence but de-escalation needs deescalate_sustained calm pressure
# samples. So the error message's remedy ("call agent.send() first") could not
# satisfy the condition the gate checked, and the living script's re-auth path
# was structurally incapable of working.
#
# F-01 is the run 12 case. F-02 and F-03 are the anti-regression pins: an
# expired lease must still deny, and a config with NO lease configured must
# keep the old level test rather than fall through to permanently allowed.
#
# Escalation staging mirrors test_challenge_fail_path.sh — S22 fires once from
# a recorded validation failure and signal_density alone drives the level to
# ELEVATED. step_up_cooldown_turns is high so no challenge is attempted; this
# group tests the gate, not the challenge.
# ============================================================
echo -e "${CYAN}--- Group F: propose gated on lease, not governance level ---${NC}"

write_govern_leased() {  # $1=workdir $2=port $3=lease_turns $4=lease_seconds
    cat > "$1/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": {
        "enabled": true, "level": "advisory", "check_interval_turns": 1,
        "signals": {
            "circular_actions": false, "repeated_failures": false,
            "scope_creep": false, "intent_contradictions": false,
            "vocabulary_contraction": false, "coherence_velocity": false,
            "response_quality": false, "thinking_collapse": false,
            "semantic_stability": false, "mandate_alignment": false,
            "context_growth": false, "instruction_recall": false,
            "plan_drift": false, "entity_consistency": false,
            "instruction_conflict": false, "persona_fingerprint": false,
            "tool_chain_integrity": false, "claim_result_reconciliation": false,
            "prompt_compliance": false, "response_repetition": false,
            "validation_outcome": true
        },
        "reality_checkpoint": {
            "enabled": false, "pressure_threshold": 0.5,
            "signal_density_divisor": 1,
            "weights": {
                "coherence_proximity": 0, "risk_score_proximity": 0,
                "signal_density": 1.0, "conversation_depth": 0,
                "bsd_partial_progress": 0, "pipeline_inherited": 0,
                "coherence_acceleration": 0, "codegen_pressure": 0,
                "bsd_eviction_pressure": 0, "semantic_deviation": 0
            }
        }
    },
    "circuit_breaker": {
        "enabled": true, "elevated_threshold": 0.5, "elevated_sustained": 1,
        "deescalate_sustained": 9,
        "step_up_enabled": true, "step_up_at_level": "elevated",
        "step_up_cooldown_turns": 999
    },
    "agents": {
        "proposer": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100, "max_turns": 30,
            "propose_candidates_max": 3,
            "standing_lease_turns": $3, "standing_lease_seconds": $4
        }
    }
}
GOVEOF
    sign_govern "$1"
}

run_gate_case() {  # $1=name $2=lease_turns $3=lease_seconds -> echoes PROPOSE_OK / PROPOSE_DENIED / other
    local d="$TEST_TMP/gate-$1"; mkdir -p "$d"
    cp "$WDIR/fixture.json" "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    write_govern_leased "$d" "$STUB_PORT" "$2" "$3"
    cat > "$d/t.naab" << 'EOF'
use agent
main {
    let h = agent.create("proposer")
    let r1 = agent.send(h, "summarize quarterly revenue")
    let _v = agent.record_validation(h, false, "FAILED test_revenue: totals did not reconcile against the ledger")
    let r2 = agent.send(h, "summarize quarterly revenue again")
    try {
        let p = agent.propose(h, "summarize quarterly revenue once more", 2)
        print("PROPOSE_OK|candidates=" + string((p.get("candidates") ?? []).length()))
    } catch (e) {
        if string(e).contains("step-up") { print("PROPOSE_DENIED") }
        else { print("PROPOSE_OTHER|" + string(e)) }
    }
}
EOF
    local out
    out=$( (cd "$d" && timeout 60s "$NAAB" t.naab 2>/dev/null) \
        | grep -oE 'PROPOSE_OK\|candidates=[0-9]+|PROPOSE_DENIED|PROPOSE_OTHER.*' | head -1 )
    # Staging check from telemetry, not from the script: a gate test whose
    # escalation never fired would pass vacuously — the allowed case looks
    # identical to the old behavior when the level never reached the trigger.
    local lvl
    lvl=$(grep -o '"to_level":"[a-z]*"' "$d/telemetry.jsonl" 2>/dev/null | tail -1 | cut -d'"' -f4)
    echo "LEVEL=${lvl:-none} ${out:-<no output>}"
    stop_stub
}

# F-01: elevated level, VALID lease -> propose must be allowed (the run 12 case)
R_VALID=$(run_gate_case valid 20 600)
if ! echo "$R_VALID" | grep -qE 'LEVEL=(elevated|high|critical)'; then
    fail "F-01" "Staging never escalated — gate was not exercised" "got: ${R_VALID:-<no output>}"
elif echo "$R_VALID" | grep -q 'PROPOSE_OK'; then
    pass "F-01" "Elevated level with a valid lease permits propose ($R_VALID)"
else
    fail "F-01" "Valid lease still denied at elevated level" "got: ${R_VALID:-<no output>}"
fi

# F-02: lease expires after 1 turn -> must still deny (anti-regression)
R_EXPIRED=$(run_gate_case expired 1 0)
if echo "$R_EXPIRED" | grep -q 'PROPOSE_DENIED'; then
    pass "F-02" "Expired lease still denies propose"
else
    fail "F-02" "Expired lease no longer denies — gate weakened" "got: ${R_EXPIRED:-<no output>}"
fi

# F-03: no lease configured -> level test retained (anti-regression)
R_NOLEASE=$(run_gate_case nolease 0 0)
if ! echo "$R_NOLEASE" | grep -qE 'LEVEL=(elevated|high|critical)'; then
    fail "F-03" "Staging never escalated — level test was not exercised" "got: ${R_NOLEASE:-<no output>}"
elif echo "$R_NOLEASE" | grep -q 'PROPOSE_DENIED'; then
    pass "F-03" "No-lease config keeps the governance-level test"
else
    fail "F-03" "No-lease config fell through to permanently allowed" "got: ${R_NOLEASE:-<no output>}"
fi

# ============================================================
# Group G: the lease is enforced at the COMMIT boundary, not just at propose
#
# propose() gates the lease when candidates are generated, but commit is where
# the transition becomes real and the deliberation gap between them is the
# point of propose/commit. Only the WALL-CLOCK half can fire: turns cannot
# advance between propose and commit, so a turn-based lease that passed
# propose is still valid at commit by construction. G-02 holds that claim
# honest — without it the gate could be over-broad and nothing would notice.
# ============================================================
echo ""
echo -e "${CYAN}--- Group G: lease enforced at the commit boundary ---${NC}"

write_govern_commit_lease() {  # $1=workdir $2=port $3=lease_seconds $4=lease_turns $5=step_up
    cat > "$1/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "circuit_breaker": { "enabled": true, "step_up_enabled": $5 },
    "agents": {
        "proposer": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100,
            "max_turns": 30,
            "propose_candidates_max": 2,
            "standing_lease_seconds": $3,
            "standing_lease_turns": $4
        }
    }
}
GOVEOF
    sign_govern "$1"
}

# Propose, let the wall clock run past the lease, then commit.
run_commit_lease_case() {  # $1=name $2=lease_seconds $3=lease_turns $4=step_up
    local d="$TEST_TMP/glease-$1"; mkdir -p "$d"
    cp "$WDIR/fixture.json" "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    write_govern_commit_lease "$d" "$STUB_PORT" "$2" "$3" "$4"
    cat > "$d/t.naab" << 'EOF'
use agent
main {
    let h = agent.create("proposer")
    let p = agent.propose(h, "summarize quarterly revenue", 1)
    let cands = p.get("candidates") ?? []
    if cands.length() == 0 { print("NO_CANDIDATES") }
    time.sleep(4)
    try {
        let c = agent.commit(h, cands[0])
        print("COMMIT_OK|turns=" + string(agent.usage(h).get("turns") ?? 0))
    } catch (e) {
        print("COMMIT_DENIED|stepup=" + string(string(e).contains("step-up")))
    }
}
EOF
    local out
    out=$( (cd "$d" && timeout 90s "$NAAB" t.naab 2>/dev/null) \
        | grep -oE 'COMMIT_OK\|turns=[0-9]+|COMMIT_DENIED\|stepup=(true|false)|NO_CANDIDATES' | tail -1 )
    echo "${out:-<no output>}"
    stop_stub
}

# G-01: wall-clock lease expires during the gap -> commit must refuse, and the
# message must carry the "step-up" substring the re-auth pattern matches on.
R_G1=$(run_commit_lease_case wallclock 2 0 true)
if echo "$R_G1" | grep -q 'COMMIT_DENIED|stepup=true'; then
    pass "G-01" "Wall-clock lease expiry blocks commit with the re-auth message"
else
    fail "G-01" "Commit landed under an expired wall-clock lease" "got: $R_G1"
fi

# G-02: turn-based lease only -> commit must still succeed. Guards the no-op
# claim; a failure here means the gate is firing where it provably cannot.
R_G2=$(run_commit_lease_case turnonly 0 20 true)
if echo "$R_G2" | grep -q 'COMMIT_OK|turns=1'; then
    pass "G-02" "Turn-only lease still commits (gate is not over-broad)"
else
    fail "G-02" "Turn-lease commit was refused — gate is over-broad" "got: $R_G2"
fi

# G-03: step-up disabled -> no re-auth path exists, so the refusal must use the
# hard wording rather than telling the caller to run a challenge it cannot run.
R_G3=$(run_commit_lease_case nostepup 2 0 false)
if echo "$R_G3" | grep -q 'COMMIT_DENIED|stepup=false'; then
    pass "G-03" "Step-up disabled yields the hard lease-expired refusal"
else
    fail "G-03" "Wrong refusal wording with step-up disabled" "got: $R_G3"
fi

# ============================================================
# Group H: a proposal cannot be committed across a config reload
#
# commit() deliberately does NOT call reloadIfChanged() — an accepted reload
# re-baselines mandate_keywords via onAgentConfigChanged() and would score an
# already-generated candidate against a mandate it never received, besides
# dangling `config` through the atomic rules_ptr_ swap. Refusing across a reload
# is the substitute: decline to commit under conditions the candidate was never
# evaluated against, rather than re-scoring it under them.
#
# Two mechanics this test depends on, both verified the hard way:
#   - reloadIfChanged() compares mtime at SECOND granularity, so the rewrite
#     must land in a later second than the initial load or no reload occurs.
#   - subprocess env is scrubbed, so the re-sign must be passed --signing-key
#     explicitly; NAAB_SIGNING_KEY does not survive into the child.
# ============================================================
echo ""
echo -e "${CYAN}--- Group H: proposal invalidated by a config reload ---${NC}"

write_govern_reload() {  # $1=workdir $2=port
    cat > "$1/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "languages": { "allowed": ["python"] },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "circuit_breaker": { "enabled": true, "step_up_enabled": true },
    "agents": {
        "proposer": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100,
            "max_turns": 30,
            "propose_candidates_max": 2
        }
    }
}
GOVEOF
    sign_govern "$1"
}

run_reload_case() {  # $1=name $2=do_reload(0|1)
    local d="$TEST_TMP/reload-$1"; mkdir -p "$d"
    cp "$WDIR/fixture.json" "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    write_govern_reload "$d" "$STUB_PORT"
    cat > "$d/t.naab" << 'EOF'
use agent
use file
use json
use process
main {
    let h = agent.create("proposer")
    let p = agent.propose(h, "summarize quarterly revenue", 1)
    if env.get("DO_RELOAD", "") == "1" {
        // mtime is second-granular; without this the rewrite is invisible.
        time.sleep(2)
        let raw = process.run("cat", ["govern.json"])
        let cfg = json.parse(raw.stdout)
        cfg["agents"]["proposer"]["max_tokens"] = 80   // tightening: ratchet-safe
        file.write("_pending.json", json.stringify(cfg, 2))
        let mv = process.run("mv", ["_pending.json", "govern.json"])
        // Subprocess env is scrubbed — pass the key explicitly.
        let sg = process.run(env.get("NAAB_BIN", "naab-lang"),
                             ["--signing-key", env.get("NAAB_KEY", ""), "--sign-governance"])
        // A polyglot block calls reloadIfChanged(), bumping reload_count_.
        let r = <<python
result = 1
>>
    }
    try {
        let c = agent.commit(h, (p.get("candidates") ?? [])[0])
        print("COMMIT_OK|turns=" + string(agent.usage(h).get("turns") ?? 0))
    } catch (e) {
        print("COMMIT_DENIED|config_changed=" + string(string(e).contains("configuration changed")))
    }
}
EOF
    local out
    out=$( (cd "$d" && DO_RELOAD="$2" NAAB_BIN="$NAAB" NAAB_KEY="$NAAB_SIGNING_KEY" \
            timeout 90s "$NAAB" t.naab 2>/dev/null) \
        | grep -oE 'COMMIT_OK\|turns=[0-9]+|COMMIT_DENIED\|config_changed=(true|false)' | tail -1 )
    echo "${out:-<no output>}"
    stop_stub
}

# H-01: reload lands between propose and commit -> commit must refuse.
R_H1=$(run_reload_case changed 1)
if echo "$R_H1" | grep -q 'COMMIT_DENIED|config_changed=true'; then
    pass "H-01" "Config reload between propose and commit invalidates the proposal"
else
    fail "H-01" "Proposal committed across a config reload" "got: $R_H1"
fi

# H-02: no reload -> commit must still succeed. Without this the stamp could be
# refusing every commit and H-01 would still pass.
R_H2=$(run_reload_case unchanged 0)
if echo "$R_H2" | grep -q 'COMMIT_OK|turns=1'; then
    pass "H-02" "Commit still succeeds when the config did not change"
else
    fail "H-02" "Commit refused with no config change — stamp is over-broad" "got: $R_H2"
fi

# ============================================================
# Group I: propose consults the lease even with step-up disabled
#
# The lease test used to sit inside `if (cb.step_up_enabled)`, and
# step_up_enabled defaults to FALSE — so by default propose did not consult the
# lease at all, while send() hard-throws and commit() (Group G) checks it
# unconditionally. Propose was the only entry point where expired authority
# passed silently.
# ============================================================
echo ""
echo -e "${CYAN}--- Group I: propose lease gate independent of step-up ---${NC}"

run_propose_lease_case() {  # $1=name $2=lease_seconds $3=step_up
    local d="$TEST_TMP/please-$1"; mkdir -p "$d"
    cp "$WDIR/fixture.json" "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    write_govern_commit_lease "$d" "$STUB_PORT" "$2" 0 "$3"
    cat > "$d/t.naab" << 'EOF'
use agent
main {
    let h = agent.create("proposer")
    time.sleep(4)
    try {
        let p = agent.propose(h, "summarize quarterly revenue", 1)
        print("PROPOSE_OK|candidates=" + string((p.get("candidates") ?? []).length()))
    } catch (e) {
        print("PROPOSE_DENIED|stepup=" + string(string(e).contains("step-up")))
    }
}
EOF
    local out
    out=$( (cd "$d" && timeout 90s "$NAAB" t.naab 2>/dev/null) \
        | grep -oE 'PROPOSE_OK\|candidates=[0-9]+|PROPOSE_DENIED\|stepup=(true|false)' | tail -1 )
    echo "${out:-<no output>}"
    stop_stub
}

# I-01: the hole. Expired lease + step-up disabled must now deny, with the hard
# wording since there is no challenge to point the caller at.
R_I1=$(run_propose_lease_case nostepup 2 false)
if echo "$R_I1" | grep -q 'PROPOSE_DENIED|stepup=false'; then
    pass "I-01" "Expired lease denies propose with step-up disabled"
else
    fail "I-01" "Expired lease passed propose with step-up disabled" "got: $R_I1"
fi

# I-02: no lease configured + step-up disabled -> propose must still work. This
# is the shape two existing test configs use, and guards the no-op claim.
R_I2=$(run_propose_lease_case nolease 0 false)
if echo "$R_I2" | grep -q 'PROPOSE_OK'; then
    pass "I-02" "No lease + step-up disabled still permits propose (not over-broad)"
else
    fail "I-02" "Unleased config denied — hoisted check is over-broad" "got: $R_I2"
fi

# I-03: anti-regression on Group F — with step-up on the renewable wording stays.
R_I3=$(run_propose_lease_case stepup 2 true)
if echo "$R_I3" | grep -q 'PROPOSE_DENIED|stepup=true'; then
    pass "I-03" "Expired lease keeps the step-up wording when step-up is enabled"
else
    fail "I-03" "Refusal wording changed with step-up enabled" "got: $R_I3"
fi


# ============================================================
# Group J: a CRITICAL suspension holds at the COMMIT boundary
#
# checkAdmission() denies at CRITICAL — "all autonomous actions suspended" —
# and propose() is gated on it. commit() was not, so a candidate generated at
# NORMAL could still land after the engine reached CRITICAL, appending history
# and advancing the turn at the moment every action is meant to be suspended.
#
# Reachable without the proposing handle doing anything: governance_level_ is a
# single engine-global atomic driven by whichever handle last took a turn, while
# s_pending_proposals is keyed per handle — so a SIBLING agent escalates across
# the deliberation gap without invalidating the proposal. Same shape as the
# lease (Group G) and the config generation (Group H).
# ============================================================
echo ""
echo -e "${CYAN}--- Group J: CRITICAL suspension at the commit boundary ---${NC}"

# $3 sets BOTH circuit_breaker.critical_threshold and reality_checkpoint's
# pressure_threshold. They are separate gates in series: consecutive_high_pressure_turns
# only increments while composite >= pressure_threshold, and the CRITICAL target
# needs composite >= critical_threshold AND consecutive >= critical_sustained.
# Setting only the circuit_breaker half leaves consecutive pinned at 0, so the
# level never moves — which is what the first version of this test did.
write_govern_critical() {  # $1=workdir $2=port $3=threshold(both) $4=critical_sustained
    cat > "$1/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "exposure_tracking": { "enabled": true, "level": "hard" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": {
        "enabled": true, "level": "advisory", "check_interval_turns": 1,
        "reality_checkpoint": {
            "enabled": false,
            "pressure_threshold": $3,
            "signal_density_divisor": 1
        }
    },
    "circuit_breaker": {
        "enabled": true,
        "elevated_threshold": 0.5, "elevated_sustained": 1,
        "critical_threshold": $3, "critical_sustained": $4,
        "deescalate_sustained": 99
    },
    "agents": {
        "proposer": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100, "max_turns": 30, "propose_candidates_max": 2
        },
        "sibling": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$2",
            "api_key_env": "FAKE_KEY_PROPOSE_TEST",
            "max_tokens": 100, "max_turns": 30
        }
    }
}
GOVEOF
    sign_govern "$1"
}

run_critical_case() {  # $1=name $2=critical_threshold $3=critical_sustained
    local d="$TEST_TMP/crit-$1"; mkdir -p "$d"
    cp "$WDIR/fixture.json" "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    write_govern_critical "$d" "$STUB_PORT" "$2" "$3"
    cat > "$d/t.naab" << 'EOF'
use agent
main {
    let a = agent.create("proposer")
    let b = agent.create("sibling")
    // Propose FIRST, while the level is still NORMAL. propose() makes no CDD
    // mutation, so it cannot escalate the level itself.
    let p = agent.propose(a, "summarize quarterly revenue", 2)
    print("PROPOSED|candidates=" + string((p.get("candidates") ?? []).length()))
    // The SIBLING takes turns. Its CDD analysis drives the engine-global level;
    // agent a does nothing, so its proposal stays valid.
    // Exactly ONE sibling send. Two sends and the SECOND one is itself denied by
    // checkAdmission() once the first escalated — the process dies before the
    // commit, stderr still says CRITICAL, and this test passes without the
    // commit gate existing at all. It did, until the degraded case caught it.
    let _s1 = agent.send(b, "describe the ledger reconciliation process")
    // Proof that execution actually reached the commit. Without this marker,
    // "blocked earlier" and "blocked at commit" are the same observation.
    print("PRE_COMMIT")
    try {
        let c = agent.commit(a, (p.get("candidates") ?? [])[0])
        print("COMMIT_OK|turn=" + string(c.get("turn") ?? -1))
    } catch (e) {
        if string(e).contains("CRITICAL") { print("COMMIT_DENIED_CRITICAL") }
        else { print("COMMIT_OTHER|" + string(e)) }
    }
}
EOF
    # The CRITICAL refusal is a GovernanceHardError: checkCriticalSuspension()
    # enforces at HARD, exactly as checkAdmission() does at propose, so NAAb
    # try/catch cannot catch it and main.cpp exits 3. Detect it by exit code and
    # stderr rather than by a caught exception — an earlier version of this test
    # looked only for a catchable error and reported the hard block as "commit
    # landed", which is the inverse of the truth.
    local out rc=0 raw
    raw=$( (cd "$d" && timeout 60s "$NAAB" t.naab 2>&1) ) || rc=$?
    out=$( echo "$raw" | grep -oE 'PROPOSED\|candidates=[0-9]+|PRE_COMMIT|COMMIT_OK\|turn=-?[0-9]+|COMMIT_DENIED_CRITICAL|COMMIT_OTHER.*' | tr '\n' ' ' )
    if echo "$raw" | grep -q "governance level CRITICAL"; then out="$out HARDBLOCK_CRITICAL"; fi
    out="$out rc=$rc"
    # Staging read from telemetry, not inferred: a gate test whose escalation
    # never fired would pass vacuously — the denied case and "the level never
    # got there" are the same observation without this.
    local lvl
    lvl=$(grep -o '"to_level":"[a-z]*"' "$d/telemetry.jsonl" 2>/dev/null | tail -1 | cut -d'"' -f4)
    echo "LEVEL=${lvl:-none} ${out:-<no output>}"
    stop_stub
}

# J-01 — the defect. Escalate to CRITICAL across the deliberation gap; the
# commit must be refused even though the proposal was generated legitimately.
R_CRIT=$(run_critical_case escalate 0.0 1)
if ! echo "$R_CRIT" | grep -q 'PROPOSED|candidates='; then
    fail "J-01" "propose never produced candidates — commit gate not exercised" "got: ${R_CRIT:-<no output>}"
elif ! echo "$R_CRIT" | grep -q 'LEVEL=critical'; then
    fail "J-01" "staging never reached CRITICAL — gate was not exercised" "got: ${R_CRIT:-<no output>}"
elif ! echo "$R_CRIT" | grep -q 'PRE_COMMIT'; then
    fail "J-01" "execution never reached the commit — a blocked sibling send is not this gate" \
         "got: ${R_CRIT:-<no output>}"
elif echo "$R_CRIT" | grep -qE 'COMMIT_DENIED_CRITICAL|HARDBLOCK_CRITICAL'; then
    pass "J-01" "CRITICAL reached across the gap refuses the commit ($R_CRIT)"
else
    fail "J-01" "commit landed at CRITICAL — a suspended action became real" "got: ${R_CRIT:-<no output>}"
fi

# J-02 — the control. Without escalation the same script must commit, or J-01
# proves only that commits fail, not that the suspension is what stopped it.
R_CALM=$(run_critical_case calm 0.99 99)
if echo "$R_CALM" | grep -q 'LEVEL=critical'; then
    skip "J-02" "control escalated to CRITICAL anyway — cannot isolate the gate"
elif echo "$R_CALM" | grep -q 'COMMIT_OK'; then
    pass "J-02" "Control: without CRITICAL the same commit succeeds ($R_CALM)"
else
    fail "J-02" "Control commit failed for another reason — J-01 is unattributable" "got: ${R_CALM:-<no output>}"
fi

# ============================================================
echo ""
echo -e "${CYAN}==============================================${NC}"
echo -e "  Total: $TOTAL  ${GREEN}Pass: $PASS_COUNT${NC}  ${RED}Fail: $FAIL_COUNT${NC}  ${YELLOW}Skip: $SKIP_COUNT${NC}"
[ -n "$FAILURES" ] && echo -e "${RED}Failures:$FAILURES${NC}"
echo -e "${CYAN}==============================================${NC}"
[ $FAIL_COUNT -eq 0 ]
