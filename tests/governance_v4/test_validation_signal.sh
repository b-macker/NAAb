#!/usr/bin/env bash
# ============================================================
# test_validation_signal.sh — S22 validation_outcome CDD signal
#
# Covers agent.record_validation() and the S22 signal that folds
# external ground-truth (pytest/convergence) pass-fail into coherence.
#
# Group A: failing validation erodes coherence + fires the signal
# Group B: passing validation does not penalize
# Group C: default-on but zero-cost until a result is fed
# Group D: per-agent override disables it
# Group E: mid-run global disable = ratchet violation
# Group F: W1 — AGENT_RESPONSE carries config_name
# Group G: fail→pass transition earns partial recovery credit
# Group H: pass-spam earns no extra credit
# Group I: mid-run raise of validation_recovery_amount = ratchet violation
# Group J: detail arg — keywords recorded + validation challenge type
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/validation-signal-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

IS_WINDOWS=false
if [[ "$(uname -s)" == MINGW* ]] || [[ "$(uname -s)" == MSYS* ]] || [[ -n "${WINDIR:-}" ]]; then
    IS_WINDOWS=true
fi

# EXPERIMENT (reversible in one commit). build-windows has stalled inside
# "CLI tests — shell suites" four times: the step sits in_progress ~47 minutes,
# the runner is killed service-side, and the log archive 404s. timeout-minutes
# has now failed to fire TWICE, so the runner cannot enforce its own step
# timeout — we cannot read our way to the cause, only change the outcome.
#
# A live observation caught it hanging at the test_challenge_discrimination.sh
# header, immediately after another stub-backed suite passed. These 9 suites are
# the only ones that run a Python HTTP server and talk to it from a native
# Windows binary under MSYS2, so they are the region to exclude first.
#
# Read the next Windows run as the result:
#   green      -> these suites are implicated; narrow from 9
#   stalls     -> they are exonerated; the cause is elsewhere in the phase
#
# Coverage is not lost: build-linux and Build & Test both run every one of these
# in full, and what they test (agent governance semantics) is platform-neutral.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  test_validation_signal.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_validation_signal.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
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
export FAKE_KEY_VALSIG="fake-key-validation-signal"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

presign() {
    local pdir="$TEST_TMP/.presign"; mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"; sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""; rm -rf "$pdir"
}

start_stub() {  # $1=fixture $2=workdir
    # Port is picked at random with no bind check, and the readiness wait used to
    # be a flat 5s. Both fail on a loaded CI runner: a collision (or a lingering
    # TIME_WAIT socket) leaves the stub dead, and python3 startup + bind can
    # exceed 5s. Either way every later assertion in the suite fails for a reason
    # that has nothing to do with what the test measures. Three consecutive CI
    # runs failed this way, each in a DIFFERENT stub-backed suite, none of them
    # reproducible locally.
    local _fx="$1" _dir="$2" _try _i _tries=3
    # POSIX only. Under MSYS2 this retry path is actively harmful, and it is the
    # failure path specifically — a stub that comes up promptly never enters it,
    # which is why Windows passed until the retry itself was added:
    #   - `wait` after a plain TERM can block forever. run-all-tests.sh already
    #     warns that native Windows binaries under MSYS2 ignore TERM and that
    #     plain `timeout` "can wait forever"; a process tree holding an
    #     unkillable child is also why the runner could not enforce its own
    #     step timeout or finalize the step.
    #   - fork/exec costs ~50-100ms there, and the loop spawns grep + kill +
    #     sleep per iteration, so 3x300 iterations is dominated by spawning
    #     rather than by the 30s of intended waiting.
    # Windows keeps the single 5s attempt that ran green for many jobs.
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) _tries=1 ;; esac
    [ -n "${WINDIR:-}" ] && _tries=1
    for _try in $(seq 1 $_tries); do
        STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
        : > "$_dir/stub.log"
        python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$_fx" "$_dir" > "$_dir/stub.log" 2>&1 &
        STUB_PID=$!
        if [ "$_tries" -eq 1 ]; then
            for _i in $(seq 1 50); do
                grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
                sleep 0.1
            done
            return 1
        fi
        # 30s, not 5s — a slow start is not a failed start. Sleep 0.5 keeps the
        # spawn count near the original despite the longer ceiling.
        for _i in $(seq 1 60); do
            grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
            kill -0 "$STUB_PID" 2>/dev/null || break
            sleep 0.5
        done
        # SIGKILL, and no unbounded wait: reaping is not worth a hang.
        kill -9 "$STUB_PID" 2>/dev/null; STUB_PID=""
    done
    echo "  start_stub: no READY after 3 port attempts — stub log tail:" >&2
    tail -3 "$_dir/stub.log" >&2 2>/dev/null
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# Four on-topic code-ish responses so keyword signals stay quiet and we isolate S22
FIXTURE='{"responses": [
  {"content": "def add(a, b): return a + b  # implemented add operation", "output_tokens": 30},
  {"content": "def subtract(a, b): return a - b  # implemented subtract operation", "output_tokens": 30},
  {"content": "def multiply(a, b): return a * b  # implemented multiply operation", "output_tokens": 30},
  {"content": "def divide(a, b): return a / b  # implemented divide operation", "output_tokens": 30}
]}'

mk_govern() {  # $1=port $2=extra-agent-json
    cat <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$1",
      "api_key_env": "FAKE_KEY_VALSIG", "max_tokens": 100, "max_turns": 20$2 } }
}
EOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   S22 validation_outcome signal + agent.record_validation    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: failing validation erodes coherence + fires signal
# ============================================================
echo -e "${CYAN}--- Group A: failing validation penalizes ---${NC}"
WDIR="$TEST_TMP/a"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "A-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" "" > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = agent.record_validation(h, false)
    let r2 = agent.send(h, "implement subtract")
    let _2 = agent.record_validation(h, false)
    let r3 = agent.send(h, "implement multiply")
    let c = agent.coherence(h)
    print("FINAL_COHERENCE=" + string(c))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
COH=$(echo "$OUT" | grep FINAL_COHERENCE | sed 's/.*=//')
if echo "$OUT" | grep -q "FINAL_COHERENCE"; then
    pass "A-01" "sends completed with record_validation"
else
    fail "A-01" "run failed" "$(echo "$OUT" | head -3)"
fi
if grep -qE '"event_type":"VALIDATION_RECORDED"' "$WDIR/tele.jsonl" 2>/dev/null; then
    pass "A-02" "VALIDATION_RECORDED telemetry emitted"
else
    fail "A-02" "no VALIDATION_RECORDED event"
fi
if grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "A-03" "validation_outcome fired in CDD_TURN signals_detail"
else
    fail "A-03" "validation_outcome never fired" "$(grep '"CDD_TURN"' "$WDIR/tele.jsonl" | grep -o '"signals_detail":"[^"]*"' | tail -2)"
fi
# coherence must be measurably below 1.0
if [ -n "$COH" ] && awk "BEGIN{exit !($COH < 0.95)}"; then
    pass "A-04" "failing validation drove coherence below 0.95 ($COH)"
else
    fail "A-04" "coherence not penalized" "coherence=$COH"
fi
fi

# ============================================================
# Group B: passing validation does not penalize (via S22)
# ============================================================
echo -e "${CYAN}--- Group B: passing validation is unpenalized ---${NC}"
WDIR="$TEST_TMP/b"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "B-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
# Disable keyword signals so only S22 could move coherence — passing => no S22 fire
mk_govern "$STUB_PORT" ', "context_drift_signals": {"semantic_stability": false, "instruction_recall": false, "entity_consistency": false, "mandate_alignment": false, "coherence_velocity": false, "persona_fingerprint": false}' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = agent.record_validation(h, true)
    let r2 = agent.send(h, "implement subtract")
    let _2 = agent.record_validation(h, true)
    let r3 = agent.send(h, "implement multiply")
    print("FINAL_COHERENCE=" + string(agent.coherence(h)))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
COH=$(echo "$OUT" | grep FINAL_COHERENCE | sed 's/.*=//')
if ! grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "B-01" "validation_outcome did not fire on passing results"
else
    fail "B-01" "validation_outcome fired despite passing"
fi
if [ -n "$COH" ] && awk "BEGIN{exit !($COH >= 0.999)}"; then
    pass "B-02" "coherence unpenalized with passing validation ($COH)"
else
    fail "B-02" "coherence moved despite passing validation + disabled keyword signals" "coherence=$COH"
fi
fi

# ============================================================
# Group C: default-on but zero-cost when never fed
# ============================================================
echo -e "${CYAN}--- Group C: zero-cost when never fed ---${NC}"
WDIR="$TEST_TMP/c"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "C-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" "" > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let r2 = agent.send(h, "implement subtract")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if echo "$OUT" | grep -q DONE && ! grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "C-01" "signal default-on but never fires without record_validation"
else
    fail "C-01" "validation_outcome fired with no result fed"
fi
fi

# ============================================================
# Group D: per-agent override disables the signal
# ============================================================
echo -e "${CYAN}--- Group D: per-agent override disables ---${NC}"
WDIR="$TEST_TMP/d"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "D-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" ', "context_drift_signals": {"validation_outcome": false}' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = agent.record_validation(h, false)
    let r2 = agent.send(h, "implement subtract")
    let _2 = agent.record_validation(h, false)
    let r3 = agent.send(h, "implement multiply")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if ! grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "D-01" "per-agent context_drift_signals:{validation_outcome:false} disables the signal"
else
    fail "D-01" "signal fired despite per-agent override"
fi
fi

# ============================================================
# Group E: mid-run global disable = ratchet violation
# ============================================================
echo -e "${CYAN}--- Group E: mid-run disable is a ratchet violation ---${NC}"
if $IS_WINDOWS; then
    skip "E-01" "mid-run file swap requires POSIX file semantics"
else
WDIR="$TEST_TMP/e"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "E-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
LOOSE="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"tele.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1,\"signals\":{\"validation_outcome\":false}},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"developer\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_VALSIG\",\"max_tokens\":100,\"max_turns\":20}}}"
LOOSE_SIG=$(presign "$LOOSE")
if [ -z "$LOOSE_SIG" ]; then skip "E-01" "presign failed"; stop_stub; else
export NAAB_LOOSE_JSON="$LOOSE" NAAB_LOOSE_SIG="$LOOSE_SIG"
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1, "signals": { "validation_outcome": true } },
  "capabilities": { "shell": { "enabled": true } },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_VALSIG", "max_tokens": 100, "max_turns": 20 } }
}
EOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "implement subtract")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 40s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub
if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "validation_outcome" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "E-01" "mid-run disable of validation_outcome rejected as ratchet violation"
else
    fail "E-01" "mid-run disable not rejected" "$(grep -i 'ratchet\|reload' "$WDIR/stderr.txt" 2>/dev/null | head -2)"
fi
fi
fi
fi

# ============================================================
# Group F: W1 — AGENT_RESPONSE carries config_name
# ============================================================
echo -e "${CYAN}--- Group F: AGENT_RESPONSE attribution (W1) ---${NC}"
WDIR="$TEST_TMP/f"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "F-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" "" > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if grep -E '"event_type":"AGENT_RESPONSE"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q '"config_name":"developer"'; then
    pass "F-01" "AGENT_RESPONSE carries config_name=developer"
else
    fail "F-01" "AGENT_RESPONSE missing config_name" "$(grep '"AGENT_RESPONSE"' "$WDIR/tele.jsonl" | head -1)"
fi
fi

# ============================================================
# Group G+H: fail→pass recovery credit; pass-spam earns nothing
# ============================================================
echo -e "${CYAN}--- Group G+H: recovery on fail→pass transition ---${NC}"
WDIR="$TEST_TMP/g"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "G-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
# Disable keyword signals so only S22 (and its recovery) moves coherence
mk_govern "$STUB_PORT" ', "context_drift_signals": {"semantic_stability": false, "instruction_recall": false, "entity_consistency": false, "mandate_alignment": false, "coherence_velocity": false, "persona_fingerprint": false, "instruction_conflict": false, "context_growth": false, "response_repetition": false, "prompt_compliance": false}' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _f = agent.record_validation(h, false)
    let r2 = agent.send(h, "implement subtract")
    let c_after_fail = agent.coherence(h)
    let _p = agent.record_validation(h, true)
    let r3 = agent.send(h, "implement multiply")
    let c_after_pass = agent.coherence(h)
    let _p2 = agent.record_validation(h, true)
    let r4 = agent.send(h, "implement divide")
    let c_after_spam = agent.coherence(h)
    print("C_FAIL=" + string(c_after_fail))
    print("C_PASS=" + string(c_after_pass))
    print("C_SPAM=" + string(c_after_spam))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
CF=$(echo "$OUT" | grep '^C_FAIL=' | sed 's/.*=//')
CP=$(echo "$OUT" | grep '^C_PASS=' | sed 's/.*=//')
CS=$(echo "$OUT" | grep '^C_SPAM=' | sed 's/.*=//')
if [ -n "$CF" ] && [ -n "$CP" ] && awk "BEGIN{exit !($CP > $CF + 0.05)}"; then
    pass "G-01" "fail→pass transition recovered coherence ($CF -> $CP)"
else
    fail "G-01" "no recovery on fail→pass" "c_fail=$CF c_pass=$CP"
fi
if [ -n "$CP" ] && awk "BEGIN{exit !($CP < 1.0)}"; then
    pass "G-02" "recovery is partial — coherence stays below 1.0 ($CP)"
else
    fail "G-02" "recovery fully erased the failure" "c_pass=$CP"
fi
if grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_recovery=+'; then
    pass "G-03" "validation_recovery credit surfaced in CDD_TURN penalties_detail"
else
    fail "G-03" "no validation_recovery in telemetry" "$(grep '"CDD_TURN"' "$WDIR/tele.jsonl" | grep -o '"penalties_detail":"[^"]*"' | tail -2)"
fi
# H: the second recorded pass (no preceding failure) must earn nothing
if [ -n "$CP" ] && [ -n "$CS" ] && awk "BEGIN{exit !($CS <= $CP + 0.001)}"; then
    pass "H-01" "pass-spam earned no extra credit ($CP -> $CS)"
else
    fail "H-01" "repeated pass pumped coherence" "c_pass=$CP c_spam=$CS"
fi
fi

# ============================================================
# Group I: mid-run RAISE of validation_recovery_amount = ratchet violation
# ============================================================
echo -e "${CYAN}--- Group I: raising recovery credit is a ratchet violation ---${NC}"
if $IS_WINDOWS; then
    skip "I-01" "mid-run file swap requires POSIX file semantics"
else
WDIR="$TEST_TMP/i"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "I-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
LOOSE_I="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"tele.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1,\"validation_recovery_amount\":0.5},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"developer\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_VALSIG\",\"max_tokens\":100,\"max_turns\":20}}}"
LOOSE_I_SIG=$(presign "$LOOSE_I")
if [ -z "$LOOSE_I_SIG" ]; then skip "I-01" "presign failed"; stop_stub; else
export NAAB_LOOSE_I_JSON="$LOOSE_I" NAAB_LOOSE_I_SIG="$LOOSE_I_SIG"
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1, "validation_recovery_amount": 0.075 },
  "capabilities": { "shell": { "enabled": true } },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_VALSIG", "max_tokens": 100, "max_turns": 20 } }
}
EOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_I_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_I_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "implement subtract")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 40s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub
if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "validation_recovery_amount" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "I-01" "mid-run raise of validation_recovery_amount rejected as ratchet violation"
else
    fail "I-01" "mid-run raise not rejected" "$(grep -i 'ratchet\|reload' "$WDIR/stderr.txt" 2>/dev/null | head -2)"
fi
fi
fi
fi

# ============================================================
# Group J: detail arg — keywords recorded + validation challenge type
# ============================================================
echo -e "${CYAN}--- Group J: failure detail grounds the validation challenge ---${NC}"
WDIR="$TEST_TMP/j"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "J-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
# Escalation staging (mirrors test_challenge_first_turn.sh): S22 firing feeds
# signal_density, which alone drives the level to ELEVATED, where step-up fires.
cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "signals": {
      "circular_actions": false, "repeated_failures": false, "scope_creep": false,
      "intent_contradictions": false, "coherence_velocity": false,
      "response_quality": false, "thinking_collapse": false,
      "semantic_stability": false, "mandate_alignment": false,
      "context_growth": false, "instruction_recall": false, "plan_drift": false,
      "entity_consistency": false, "instruction_conflict": false,
      "persona_fingerprint": false, "tool_chain_integrity": false,
      "claim_result_reconciliation": false, "prompt_compliance": false,
      "response_repetition": false, "validation_outcome": true
    },
    "reality_checkpoint": {
      "enabled": false, "pressure_threshold": 0.5, "signal_density_divisor": 1,
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
    "deescalate_sustained": 5,
    "step_up_enabled": true, "step_up_contextual": true,
    "step_up_at_level": "elevated"
  },
  "agents": { "developer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_VALSIG", "max_tokens": 100, "max_turns": 20 } }
}
EOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement divide")
    let _f = agent.record_validation(h, false, "FAILED test_divide_by_zero: divide raised ZeroDivisionError, expected ValueError for zero divisor")
    let r2 = agent.send(h, "fix the divide operation")
    let r3 = agent.send(h, "add divide docstring")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if grep -E '"event_type":"VALIDATION_RECORDED"' "$WDIR/tele.jsonl" 2>/dev/null | grep -qE '"detail_keywords":"[1-9]'; then
    pass "J-01" "VALIDATION_RECORDED carries detail_keywords count"
else
    fail "J-01" "no detail_keywords in VALIDATION_RECORDED" "$(grep 'VALIDATION_RECORDED' "$WDIR/tele.jsonl" | head -1)"
fi
CHAL_J=$(grep -E '"event_type":"AGENT_CHALLENGE_(PASS|FAIL)"' "$WDIR/tele.jsonl" 2>/dev/null | head -1)
if [ -n "$CHAL_J" ]; then
    pass "J-02" "step-up challenge fired after validation-failure escalation"
    if echo "$CHAL_J" | grep -q '"challenge_type":"validation"'; then
        pass "J-03" "challenge type is 'validation' (competence, grounded in the defect)"
    else
        fail "J-03" "challenge did not use validation type" "$CHAL_J"
    fi
else
    fail "J-02" "no challenge fired" "$(grep -c CDD_TURN "$WDIR/tele.jsonl" 2>/dev/null) CDD turns"
    skip "J-03" "no challenge to inspect"
fi
fi

# ============================================================
# Group K: a pass must not erase an unconsumed FAILURE
#
# The latch is one slot consumed by the next recordTurn, which assumes one
# validation per turn. Nothing guarantees that — recordTurn only runs on an
# AGENT_RESPONSE, so anything that stops a send from completing leaves the
# result unconsumed and the next record_validation used to overwrite it.
#
# A pass landing on an unconsumed failure erased it outright: the pass consumed
# with no penalty, and no recovery credit either since the failure was never
# consumed to set last_consumed_validation_failed. It left nothing at all —
# not a penalty, not even a fired count in signals_detail.
#
# Live run 15 recorded four failures and four passes, scored none of them, and
# finished at coherence 1.0 with 4 of 4 features rejected and 11 send errors.
# The worse the run went, the more ground truth was thrown away.
#
# Here both results are recorded BETWEEN the same pair of sends, so exactly one
# recordTurn consumes them. The failure must survive.
# ============================================================
echo -e "${CYAN}--- Group K: pass does not erase an unconsumed failure ---${NC}"
WDIR="$TEST_TMP/k"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "K-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
# Only S22 moves coherence, so any change is attributable to it.
mk_govern "$STUB_PORT" ', "context_drift_signals": {"semantic_stability": false, "instruction_recall": false, "entity_consistency": false, "mandate_alignment": false, "coherence_velocity": false, "persona_fingerprint": false, "instruction_conflict": false, "context_growth": false, "response_repetition": false, "prompt_compliance": false}' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    // Both recorded before the next send: one recordTurn consumes them.
    let f = agent.record_validation(h, false, "FAILED test_add: returned None instead of the sum")
    let p = agent.record_validation(h, true)
    print("FAIL_APPLIED=" + string(f.get("applied")))
    print("PASS_APPLIED=" + string(p.get("applied")))
    let r2 = agent.send(h, "implement subtract")
    print("C_AFTER=" + string(agent.coherence(h)))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
C_AFTER=$(echo "$OUT" | grep C_AFTER= | sed 's/.*=//')

# The failure must have been scored. Under the old behaviour the pass
# overwrote it and coherence stayed at 1.0.
if [ -n "$C_AFTER" ] && python3 -c "import sys; sys.exit(0 if float('${C_AFTER:-1}') < 0.999 else 1)" 2>/dev/null; then
    pass "K-01" "Unconsumed failure survived the pass and was scored (coherence $C_AFTER)"
else
    fail "K-01" "Pass erased the unconsumed failure" \
         "coherence=$C_AFTER — the failure was recorded and never scored"
fi

if echo "$OUT" | grep -q 'PASS_APPLIED=false'; then
    pass "K-02" "Superseded pass reported applied=false to the caller"
else
    fail "K-02" "Superseded pass not reported" "$(echo "$OUT" | grep APPLIED=)"
fi

if grep '"event_type":"VALIDATION_RECORDED"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q '"applied":"false"'; then
    pass "K-03" "Supersession visible in VALIDATION_RECORDED telemetry"
else
    fail "K-03" "No applied=false in telemetry" \
         "$(grep -c '"event_type":"VALIDATION_RECORDED"' "$WDIR/tele.jsonl" 2>/dev/null) records"
fi

# The failure is scored once, not once per recorded result.
if echo "$OUT" | grep -q 'FAIL_APPLIED=true'; then
    pass "K-04" "The failure itself was applied normally"
else
    fail "K-04" "Failure was not applied" "$(echo "$OUT" | grep APPLIED=)"
fi
fi

# ============================================================
# Group L: a pass carrying LESS evidence earns no recovery credit
#
# Group G established that a fail->pass transition credits coherence back. That
# credit is the exploit path for test erosion: an agent that fails pytest, then
# deletes the failing test, then passes, is rewarded for the deletion. A pytest
# exit code cannot tell the two apart — a suite with no tests left passes — so
# the caller reports how many checks actually ran and a pass that sheds evidence
# is refused the credit.
#
# Critically the outstanding failure is NOT consumed by a shrink-pass, so a
# later honest pass can still earn the recovery. Otherwise one shrink would
# launder the failure away for free, which is a cheaper exploit than the one
# being closed.
#
# Same fixture, same signal set and same shape as Group G, so the only
# difference between G-01 recovering and L-01 not recovering is the evidence
# count.
#
# LIVE STATUS (runs 21-23): the channel is confirmed end to end — evidence
# counts reach the engine and S22 fires and recovers on real pytest outcomes —
# but the SHRINK path has never been exercised live. Every live run so far
# reported monotonically non-decreasing counts, so nothing has yet presented
# this logic with a suite that shed checks. Groups L and M remain the only
# evidence it works.
# ============================================================
echo -e "${CYAN}--- Group L: a shrinking pass earns no recovery ---${NC}"
WDIR="$TEST_TMP/l"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "L-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" ', "context_drift_signals": {"semantic_stability": false, "instruction_recall": false, "entity_consistency": false, "mandate_alignment": false, "coherence_velocity": false, "persona_fingerprint": false, "instruction_conflict": false, "context_growth": false, "response_repetition": false, "prompt_compliance": false}' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    let r1 = agent.send(h, "implement add")
    // Baseline: 12 tests, failing.
    let _f = agent.record_validation(h, false, "assert failed", 12)
    let r2 = agent.send(h, "implement subtract")
    let c_after_fail = agent.coherence(h)
    // "Fixed" by deleting tests: passes, but on 4 checks instead of 12.
    let _p = agent.record_validation(h, true, null, 4)
    let r3 = agent.send(h, "implement multiply")
    let c_after_shrink = agent.coherence(h)
    // An honest pass afterwards: evidence back up. The failure is still
    // outstanding, so this one must earn the credit the shrink did not.
    let _p2 = agent.record_validation(h, true, null, 14)
    let r4 = agent.send(h, "implement divide")
    let c_after_honest = agent.coherence(h)
    print("C_FAIL=" + string(c_after_fail))
    print("C_SHRINK=" + string(c_after_shrink))
    print("C_HONEST=" + string(c_after_honest))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
LF=$(echo "$OUT" | grep '^C_FAIL=' | sed 's/.*=//')
LS=$(echo "$OUT" | grep '^C_SHRINK=' | sed 's/.*=//')
LH=$(echo "$OUT" | grep '^C_HONEST=' | sed 's/.*=//')
if [ -n "$LF" ] && [ -n "$LS" ] && awk "BEGIN{exit !($LS <= $LF + 0.001)}"; then
    pass "L-01" "shrinking pass earned no recovery ($LF -> $LS)"
else
    fail "L-01" "a pass with less evidence still recovered coherence" \
         "c_fail=$LF c_shrink=$LS — deleting the failing test is being rewarded"
fi
if [ -n "$LS" ] && [ -n "$LH" ] && awk "BEGIN{exit !($LH > $LS + 0.05)}"; then
    pass "L-02" "the later honest pass still earned the credit ($LS -> $LH)"
else
    fail "L-02" "shrink consumed the outstanding failure" \
         "c_shrink=$LS c_honest=$LH — one shrink laundered the failure away"
fi
if grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_credit_withheld=evidence_shrank'; then
    pass "L-03" "withheld credit is named in CDD_TURN penalties_detail"
else
    fail "L-03" "a refused credit is reported as an absence" \
         "indistinguishable from a turn where no validation ran"
fi
if grep -E '"event_type":"VALIDATION_RECORDED"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q '"evidence_count":"4"'; then
    pass "L-04" "evidence_count recorded in VALIDATION_RECORDED telemetry"
else
    fail "L-04" "evidence_count missing from telemetry" \
         "$(grep '"VALIDATION_RECORDED"' "$WDIR/tele.jsonl" 2>/dev/null | tail -1)"
fi
fi

# ============================================================
# Group M: erosion costs coherence on its own, and survives a reset
#
# Group L removed the REWARD for shedding tests. That is not enough by itself:
# an agent that never failed has no credit to lose, so it could erode a passing
# suite for free. A pass carrying less evidence than the last one is therefore
# scored as an S22 failure outright.
#
# M-02 is the interaction the trace caught before this was written. The living
# script calls agent.reset(handle) when coherence falls below a floor, and
# resetDriftState() wipes DriftState wholesale. If the evidence count went with
# it, an eroding agent would trip the reset and return with nothing to shrink
# from — the signal would be weakest immediately after it first worked. The
# count is preserved across reset for that reason; behavioural history is not.
# ============================================================
echo -e "${CYAN}--- Group M: shedding evidence costs coherence ---${NC}"
WDIR="$TEST_TMP/m"; mkdir -p "$WDIR"
printf '%s' "$FIXTURE" > "$WDIR/fixture.json"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "M-00" "stub failed"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
mk_govern "$STUB_PORT" ', "context_drift_signals": {"semantic_stability": false, "instruction_recall": false, "entity_consistency": false, "mandate_alignment": false, "coherence_velocity": false, "persona_fingerprint": false, "instruction_conflict": false, "context_growth": false, "response_repetition": false, "prompt_compliance": false}' > "$WDIR/govern.json"; sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("developer")
    // Never fails. Every outcome below is a PASS — only the evidence moves.
    let r1 = agent.send(h, "implement add")
    let _a = agent.record_validation(h, true, null, 20)
    let r2 = agent.send(h, "implement subtract")
    let c_base = agent.coherence(h)
    // Passing on half the checks. No credit exists to withhold here.
    let _b = agent.record_validation(h, true, null, 10)
    let r3 = agent.send(h, "implement multiply")
    let c_eroded = agent.coherence(h)
    // A reset clears behavioural history. The evidence baseline must survive it,
    // or the next shrink has nothing to measure against.
    agent.reset(h)
    let r4 = agent.send(h, "implement divide")
    let _c = agent.record_validation(h, true, null, 3)
    let r5 = agent.send(h, "implement modulo")
    let c_post_reset = agent.coherence(h)
    print("C_BASE=" + string(c_base))
    print("C_ERODED=" + string(c_eroded))
    print("C_POSTRESET=" + string(c_post_reset))
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
MB=$(echo "$OUT" | grep '^C_BASE=' | sed 's/.*=//')
ME=$(echo "$OUT" | grep '^C_ERODED=' | sed 's/.*=//')
MP=$(echo "$OUT" | grep '^C_POSTRESET=' | sed 's/.*=//')
if [ -n "$MB" ] && [ -n "$ME" ] && awk "BEGIN{exit !($ME < $MB - 0.01)}"; then
    pass "M-01" "a passing run that shed checks lost coherence ($MB -> $ME)"
else
    fail "M-01" "shedding checks on a passing suite cost nothing" \
         "c_base=$MB c_eroded=$ME — erosion is free for an agent that never failed"
fi
if [ -n "$MP" ] && awk "BEGIN{exit !($MP < 1.0)}"; then
    pass "M-02" "evidence baseline survived agent.reset() — post-reset shrink still scored ($MP)"
else
    fail "M-02" "reset erased the evidence baseline" \
         "c_post_reset=$MP — resetting a degraded agent clears its erosion history"
fi
if grep -E '"event_type":"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -q 'validation_outcome'; then
    pass "M-03" "S22 named in penalties_detail for the shrink"
else
    fail "M-03" "shrink penalty not attributed to validation_outcome" \
         "$(grep '"CDD_TURN"' "$WDIR/tele.jsonl" 2>/dev/null | grep -o '"penalties_detail":"[^"]*"' | tail -2)"
fi
fi

# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
