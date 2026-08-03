#!/usr/bin/env bash
# test_output_admissibility.sh — Output admissibility: post-CDD coherence gate
# Tests config parsing, ratchet enforcement, dashboard display, and level clamping.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

# EXPERIMENT (reversible in one commit). See test_absorption_degenerate.sh, which
# has carried this guard alone since before this change:
#   "Stub-backed HTTP tests hang on Windows/MSYS2 due to signal propagation and
#    process cleanup issues. Skip entirely — Linux CI validates the behavior."
# That diagnosis was made once and applied to one file out of 29 that launch the
# stub. build-windows has since stalled four times inside "CLI tests — shell
# suites": the step sits in_progress ~47 minutes, the runner is killed
# service-side, and the log archive 404s. timeout-minutes failed to fire TWICE,
# so the runner cannot enforce its own step timeout either — the cause cannot be
# read, only excluded.
#
# Read the next Windows run as the result:
#   green   -> stub-backed suites are the cause; narrow from here
#   stalls  -> they are exonerated and the cause is elsewhere in the phase
#
# Coverage is not lost: build-linux and Build & Test run every one of these in
# full, and agent-governance semantics are platform-neutral.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  test_output_admissibility.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_output_admissibility.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
    exit 0
fi

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TMPBASE="$_SYSTMP/test_oa_$$"
mkdir -p "$TMPBASE"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
"$NAAB" --keygen "$TMPBASE/test-key.pem" 2>/dev/null
"$NAAB" --trust-key "$TMPBASE/test-key.pem.pub" 2>/dev/null
SIGNING_KEY="$TMPBASE/test-key.pem"

PASS=0; FAIL=0; SKIP=0

cleanup() { [ -n "${STUB_PID:-}" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TMPBASE"; }
trap cleanup EXIT

check() {
    local id="$1" desc="$2" expected="$3" actual="$4"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [$id] $desc (expected=$expected actual=$actual)"
        FAIL=$((FAIL + 1))
    fi
}

check_grep() {
    local id="$1" desc="$2" pattern="$3" text="$4"
    if echo "$text" | grep -q "$pattern" 2>/dev/null; then
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [$id] $desc (pattern '$pattern' not found)"
        FAIL=$((FAIL + 1))
    fi
}

check_not_grep() {
    local id="$1" desc="$2" pattern="$3" text="$4"
    if echo "$text" | grep -q "$pattern" 2>/dev/null; then
        echo "  FAIL [$id] $desc (found '$pattern' but shouldn't)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    fi
}

sign_govern() {
    local dir="$1"
    (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
}

echo "=== Output Admissibility: Post-CDD Coherence Gate ==="
echo ""

# ═══════════════════════════════════════════════════════════
# OA01: Disabled by default — no OA Gate in dashboard
# ═══════════════════════════════════════════════════════════
echo "--- OA01: Disabled by default ---"
DIR="$TMPBASE/oa01"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA disabled test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
use governance

main {
    let health = governance.health()
    print("verdict=" + health.get("verdict"))
    print("OA01_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA01a" "Exits cleanly" "0" "$EC"
check_grep "OA01b" "Script runs" "OA01_DONE" "$OUTPUT"
check_not_grep "OA01c" "No OA Gate in dashboard" "OA Gate:" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA02: Config parsing — quarantine action with custom threshold
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA02: Config parsing ---"
DIR="$TMPBASE/oa02"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA config test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.85,
      "action": "quarantine"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA02_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA02a" "Exits cleanly" "0" "$EC"
check_grep "OA02b" "OA Gate in dashboard" "OA Gate:" "$OUTPUT"
check_grep "OA02c" "Threshold shown" "threshold=0.85" "$OUTPUT"
check_grep "OA02d" "Action shown" "action=quarantine" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA03: Block action with DETECT level — dashboard shows DETECT
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA03: Block action with DETECT level ---"
DIR="$TMPBASE/oa03"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA block test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.70,
      "action": "block",
      "level": "detect"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA03_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA03a" "Exits cleanly" "0" "$EC"
check_grep "OA03b" "Level shown as DETECT" "level=DETECT" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA04: ADVISORY level clamped to DETECT for block action
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA04: ADVISORY level clamped to DETECT ---"
DIR="$TMPBASE/oa04"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA advisory clamp test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.70,
      "action": "block",
      "level": "advisory"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA04_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA04a" "Exits cleanly" "0" "$EC"
check_grep "OA04b" "Level clamped to DETECT" "level=DETECT" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA05: Attest action — level shown as n/a
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA05: Attest action ---"
DIR="$TMPBASE/oa05"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA attest test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.75,
      "action": "attest"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA05_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA05a" "Exits cleanly" "0" "$EC"
check_grep "OA05b" "Action shown" "action=attest" "$OUTPUT"
check_grep "OA05c" "Level n/a for non-block" "level=n/a" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA06: Block with SOFT level — dashboard shows SOFT
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA06: Block with SOFT level ---"
DIR="$TMPBASE/oa06"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA SOFT level test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.80,
      "action": "block",
      "level": "soft"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA06_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA06a" "Exits cleanly" "0" "$EC"
check_grep "OA06b" "OA Gate enabled" "OA Gate:" "$OUTPUT"
check_grep "OA06c" "Level shown as SOFT" "level=SOFT" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA07: Block+DETECT with high threshold parses correctly
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA07: High threshold config ---"
DIR="$TMPBASE/oa07"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA high threshold test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.90,
      "action": "block",
      "level": "detect"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA07_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA07a" "Exits cleanly" "0" "$EC"
check_grep "OA07b" "Threshold parsed" "threshold=0.90" "$OUTPUT"
check_grep "OA07c" "Block+DETECT shown" "action=block" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA08: Block with HARD level — dashboard shows HARD
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA08: Block with HARD level ---"
DIR="$TMPBASE/oa08"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA HARD level test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.70,
      "action": "block",
      "level": "hard"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA08_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA08a" "Exits cleanly" "0" "$EC"
check_grep "OA08b" "Block+HARD shown" "level=HARD" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA09: Invalid action ignored — defaults to quarantine
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA09: Invalid action falls back to default ---"
DIR="$TMPBASE/oa09"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA invalid action test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.70,
      "action": "invalid_action"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA09_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA09a" "Exits cleanly" "0" "$EC"
check_grep "OA09b" "Falls back to quarantine" "action=quarantine" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA10: Threshold clamped to [0.0, 1.0]
# ═══════════════════════════════════════════════════════════
echo ""
echo "--- OA10: Threshold clamped to valid range ---"
DIR="$TMPBASE/oa10"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA threshold clamp test",
  "security": { "sandbox_level": "elevated" },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 5.0,
      "action": "quarantine"
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
    print("OA10_DONE")
}
EOF
OUTPUT=$(cd "$DIR" && "$NAAB" --governance-dashboard test.naab 2>&1) && EC=0 || EC=$?
check "OA10a" "Exits cleanly" "0" "$EC"
# threshold=5.0 should be clamped to 1.0
check_grep "OA10b" "Threshold clamped to 1.00" "threshold=1.00" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# OA11/OA12: max_quarantine_streak — default 5 terminates the
# quarantine+commit degradation loop; explicit 0 disables.
# Requires the local agent stub for real quarantined sends.
# ═══════════════════════════════════════════════════════════
export FAKE_KEY_OA_TEST="fake-key-oa-streak-test"
STUB_PID=""
start_stub() {  # $1=fixture $2=workdir
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do
        grep -q READY "$2/stub.log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null || true; wait "$STUB_PID" 2>/dev/null || true; STUB_PID=""; }

# Both sections: threshold 0.99 so the first CDD penalty (zero instruction
# recall against the evasive stub reply) makes every turn inadmissible.
write_streak_config() {  # $1=dir $2=extra_oa_fields (may be empty)
    cat > "$1/govern.json" << GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "OA quarantine streak test",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true,
      "threshold": 0.99,
      "action": "quarantine"$2
    }
  },
  "agents": {
    "test_agent": {
      "provider": "gemini",
      "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_OA_TEST",
      "max_tokens": 100,
      "max_turns": 20
    }
  }
}
GOVEOF
}

write_streak_script() {  # $1=dir
    cat > "$1/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("test_agent")
    let sent = 0
    for i in 0..10 {
        let r = agent.send(h, "please summarize the quarterly revenue figures now")
        sent = sent + 1
    }
    print("SENT=" + string(sent))
}
NAABEOF
}

echo ""
echo "--- OA11: Default max_quarantine_streak=5 hard-blocks the loop ---"
DIR="$TMPBASE/oa11"
mkdir -p "$DIR"
cat > "$DIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "evasive unrelated rambling reply", "output_tokens": 20}
]}
EOF
start_stub "$DIR/fixture.json" "$DIR" || { echo "  SKIP [OA11] stub failed to start"; SKIP=$((SKIP + 1)); }
if [ -n "$STUB_PID" ]; then
    write_streak_config "$DIR" ""
    sign_govern "$DIR"
    write_streak_script "$DIR"
    OUTPUT=$(cd "$DIR" && timeout 60s "$NAAB" test.naab 2>&1) && EC=0 || EC=$?
    stop_stub
    check "OA11a" "Streak breach exits 3 (GovernanceHardError)" "3" "$EC"
    check_grep "OA11b" "Streak error surfaced" "maximum quarantine streak" "$OUTPUT"
    check_grep "OA11c" "QUARANTINE_STREAK_EXCEEDED telemetry" "QUARANTINE_STREAK_EXCEEDED" "$(cat "$DIR/telemetry.jsonl" 2>/dev/null)"
    check_not_grep "OA11d" "Loop terminated before completing all sends" "SENT=10" "$OUTPUT"
fi

echo ""
echo "--- OA12: Explicit max_quarantine_streak=0 disables the limit ---"
DIR="$TMPBASE/oa12"
mkdir -p "$DIR"
cat > "$DIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "evasive unrelated rambling reply", "output_tokens": 20}
]}
EOF
start_stub "$DIR/fixture.json" "$DIR" || { echo "  SKIP [OA12] stub failed to start"; SKIP=$((SKIP + 1)); }
if [ -n "$STUB_PID" ]; then
    write_streak_config "$DIR" ',
      "max_quarantine_streak": 0'
    sign_govern "$DIR"
    write_streak_script "$DIR"
    OUTPUT=$(cd "$DIR" && timeout 60s "$NAAB" test.naab 2>&1) && EC=0 || EC=$?
    stop_stub
    check "OA12a" "Unlimited quarantines exit cleanly" "0" "$EC"
    check_grep "OA12b" "All sends completed despite quarantines" "SENT=10" "$OUTPUT"
    check_not_grep "OA12c" "No streak telemetry when disabled" "QUARANTINE_STREAK_EXCEEDED" "$(cat "$DIR/telemetry.jsonl" 2>/dev/null)"
fi

echo ""
echo "================================================"
echo "  PASS: $PASS  FAIL: $FAIL  SKIP: $SKIP"
echo "================================================"

exit "$FAIL"
