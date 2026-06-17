#!/usr/bin/env bash
# test_invariant_enforcement.sh — State Machine Invariant Tests
#
# Verifies governance state machines maintain their invariants.
# Tests catch bugs where individual decisions are correct but sequences
# violate contracts.
#
# 25 invariant tests across 5 state machines:
#   PULSE:  Governance health verdict (5 tests — SKIP: requires agent turns)
#   RATCH:  Ratchet enforcement (5 tests)
#   ESC:    Advisory escalation (5 tests)
#   LEASE:  Standing lease TTL (5 tests — SKIP: requires API key)
#   EPOCH:  Evidence epoch (5 tests)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../../build/naab-lang}"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TMPBASE="$_SYSTMP/test_invariant_$$"
mkdir -p "$TMPBASE"

source "$SCRIPT_DIR/../../helpers/trust_setup.sh"
setup_isolated_trust
"$NAAB" --keygen "$TMPBASE/test-key.pem" 2>/dev/null
"$NAAB" --trust-key "$TMPBASE/test-key.pem.pub" 2>/dev/null
SIGNING_KEY="$TMPBASE/test-key.pem"

PASS=0; FAIL=0; SKIP=0

cleanup() { teardown_isolated_trust; rm -rf "$TMPBASE"; }
trap cleanup EXIT

ok()   { PASS=$((PASS + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL [$1] $2"; }
skip() { SKIP=$((SKIP + 1)); echo "  SKIP [$1] $2"; }

sign_dir() {
    local dir="$1"
    (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
}

echo "=== State Machine Invariant Tests ==="
echo "  Binary: $NAAB"
echo ""

# =====================================================================
# PULSE VERDICT (5 tests) — require agent turns, skip without API key
# =====================================================================
echo "--- Pulse Verdict (requires agent turns — skipping) ---"
for P in 01 02 03 04 05; do
    skip "I-PULSE-$P" "requires agent turns (no API key)"
done

echo ""

# =====================================================================
# RATCHET ENFORCEMENT (5 tests)
# =====================================================================
echo "--- Ratchet Enforcement ---"

# I-RATCH-01: Mid-run tightening accepted → capability revoked after reload
# Ratchet checks capabilities.shell.enabled (governance_config.cpp:2921)
R1DIR="$TMPBASE/ratch01"
mkdir -p "$R1DIR/tight"
cat > "$R1DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["shell", "python"] },
    "capabilities": { "shell": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R1DIR"
cat > "$R1DIR/tight/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["shell", "python"] },
    "capabilities": { "shell": { "enabled": false } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R1DIR/tight"
cat > "$R1DIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$R1DIR/tight/govern.json", "$R1DIR/govern.json")
shutil.copy("$R1DIR/tight/govern.json.sig", "$R1DIR/govern.json.sig")
print("capability tightened")
>>
    print(r1)
    let r2 = <<python
print("reload triggered")
>>
    print(r2)
    let r3 = <<shell
echo "should be blocked"
>>
    print(r3)
}
NAABEOF
OUT=$(cd "$R1DIR" && timeout 30s "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if [ $RC -eq 3 ]; then
    ok "I-RATCH-01" "capability tightening accepted → shell blocked (exit 3)"
else
    fail "I-RATCH-01" "expected exit 3, got $RC"
fi

# I-RATCH-02: Capability loosening rejected → ratchet message
# capabilities.shell.enabled: false→true is a ratchet violation
R2DIR="$TMPBASE/ratch02"
mkdir -p "$R2DIR/loose"
cat > "$R2DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "capabilities": { "shell": { "enabled": false } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R2DIR"
cat > "$R2DIR/loose/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "capabilities": { "shell": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R2DIR/loose"
cat > "$R2DIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$R2DIR/loose/govern.json", "$R2DIR/govern.json")
shutil.copy("$R2DIR/loose/govern.json.sig", "$R2DIR/govern.json.sig")
print("tried to loosen capability")
>>
    print(r1)
    let r2 = <<python
print("still strict")
>>
    print(r2)
}
NAABEOF
OUT=$(cd "$R2DIR" && timeout 30s "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if echo "$OUT" | grep -qi "ratchet"; then
    ok "I-RATCH-02" "capability loosening rejected with ratchet message"
else
    fail "I-RATCH-02" "expected ratchet rejection (exit $RC)"
fi

# I-RATCH-03: Numeric limit ratchet — tighten loop_iterations mid-run
R3DIR="$TMPBASE/ratch03"
mkdir -p "$R3DIR/tight"
cat > "$R3DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "limits": { "execution": { "loop_iterations": 100 } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R3DIR"
cat > "$R3DIR/tight/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "limits": { "execution": { "loop_iterations": 5 } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R3DIR/tight"
cat > "$R3DIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$R3DIR/tight/govern.json", "$R3DIR/govern.json")
shutil.copy("$R3DIR/tight/govern.json.sig", "$R3DIR/govern.json.sig")
print("limits tightened")
>>
    print(r1)
    let r2 = <<python
print("reload triggered")
>>
    print(r2)
    let i = 0
    while i < 50 {
        i = i + 1
    }
    print("loop done: " + string(i))
}
NAABEOF
OUT=$(cd "$R3DIR" && timeout 30s "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if [ $RC -eq 3 ]; then
    ok "I-RATCH-03" "numeric limit tightened → loop blocked (exit 3)"
else
    fail "I-RATCH-03" "expected exit 3, got $RC"
fi

# I-RATCH-04: Network tightening mid-run, python unaffected
# capabilities.network.enabled: true→false is tightening (accepted by ratchet)
R4DIR="$TMPBASE/ratch04"
mkdir -p "$R4DIR/tight"
cat > "$R4DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "capabilities": { "network": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R4DIR"
cat > "$R4DIR/tight/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "capabilities": { "network": { "enabled": false } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R4DIR/tight"
cat > "$R4DIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$R4DIR/tight/govern.json", "$R4DIR/govern.json")
shutil.copy("$R4DIR/tight/govern.json.sig", "$R4DIR/govern.json.sig")
print("network disabled")
>>
    print(r1)
    let r2 = <<python
print("python still works after network tightening")
>>
    print(r2)
}
NAABEOF
OUT=$(cd "$R4DIR" && timeout 30s "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if [ $RC -eq 0 ] && echo "$OUT" | grep -q "python still works after network tightening"; then
    ok "I-RATCH-04" "network tightened → python unaffected"
else
    fail "I-RATCH-04" "expected exit 0 with python output (exit $RC)"
fi

# I-RATCH-05: Numeric limit loosening rejected
# loop_iterations: 5 → 0 (unlimited) is a ratchet violation
R5DIR="$TMPBASE/ratch05"
mkdir -p "$R5DIR/loose"
cat > "$R5DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "limits": { "execution": { "loop_iterations": 5 } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R5DIR"
cat > "$R5DIR/loose/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "limits": { "execution": { "loop_iterations": 0 } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$R5DIR/loose"
cat > "$R5DIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$R5DIR/loose/govern.json", "$R5DIR/govern.json")
shutil.copy("$R5DIR/loose/govern.json.sig", "$R5DIR/govern.json.sig")
print("tried to loosen limits")
>>
    print(r1)
    let r2 = <<python
print("still strict")
>>
    print(r2)
}
NAABEOF
OUT=$(cd "$R5DIR" && timeout 30s "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if echo "$OUT" | grep -qi "ratchet"; then
    ok "I-RATCH-05" "numeric limit loosening rejected with ratchet message"
else
    fail "I-RATCH-05" "expected ratchet rejection (exit $RC)"
fi

echo ""

# =====================================================================
# ADVISORY ESCALATION (5 tests)
# =====================================================================
echo "--- Advisory Escalation ---"

# I-ESC-01: 1st advisory = base weight (exit 0)
E1DIR="$TMPBASE/esc01"
mkdir -p "$E1DIR"
cat > "$E1DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "ESC-01", "name": "esc_marker",
        "pattern": "ESC_MARKER", "level": "advisory",
        "enabled": true, "message": "Escalation test"
    }],
    "advisory_escalation": {
        "enabled": true, "soft_after": 3, "weight_multiplier": 1.5
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$E1DIR"
cat > "$E1DIR/test.naab" << 'EOF'
main {
    let r = <<javascript
// ESC_MARKER
1
>>
    print("1st advisory OK")
}
EOF
OUT=$(cd "$E1DIR" && "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if [ $RC -eq 0 ]; then
    ok "I-ESC-01" "1st advisory = base weight (exit 0)"
else
    fail "I-ESC-01" "1st advisory blocked (exit $RC)"
fi

# I-ESC-02: 2nd advisory = multiplied weight (still exit 0 if < soft_after)
E2DIR="$TMPBASE/esc02"
mkdir -p "$E2DIR"
cat > "$E2DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "ESC-02", "name": "esc_marker2",
        "pattern": "ESC_MARKER_2", "level": "advisory",
        "enabled": true, "message": "Escalation test 2"
    }],
    "advisory_escalation": {
        "enabled": true, "soft_after": 5, "weight_multiplier": 1.5
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$E2DIR"
cat > "$E2DIR/test.naab" << 'EOF'
main {
    let a = <<javascript
// ESC_MARKER_2
1
>>
    let b = <<javascript
// ESC_MARKER_2
2
>>
    print("2nd advisory OK")
}
EOF
OUT=$(cd "$E2DIR" && "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if [ $RC -eq 0 ]; then
    ok "I-ESC-02" "2nd advisory = multiplied weight (still under soft_after)"
else
    fail "I-ESC-02" "2nd advisory blocked (exit $RC)"
fi

# I-ESC-03: N-th advisory escalates to SOFT (exit 3 when >= soft_after)
E3DIR="$TMPBASE/esc03"
mkdir -p "$E3DIR"
cat > "$E3DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "ESC-03", "name": "esc_marker3",
        "pattern": "ESC_MARKER_3", "level": "advisory",
        "enabled": true, "message": "Escalation to SOFT"
    }],
    "advisory_escalation": {
        "enabled": true, "soft_after": 3, "weight_multiplier": 1.5
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$E3DIR"
cat > "$E3DIR/test.naab" << 'EOF'
main {
    let a = <<javascript
// ESC_MARKER_3
1
>>
    let b = <<javascript
// ESC_MARKER_3
2
>>
    let c = <<javascript
// ESC_MARKER_3
3
>>
    print("should not reach")
}
EOF
OUT=$(cd "$E3DIR" && "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if [ $RC -eq 3 ] && echo "$OUT" | grep -qi "ESCALATED"; then
    ok "I-ESC-03" "N-th advisory escalates to SOFT (exit 3, ESCALATED)"
else
    fail "I-ESC-03" "expected exit 3 + ESCALATED, got exit $RC"
    echo "    output: ${OUT:0:300}"
fi

# I-ESC-04: Different advisory IDs escalate independently
E4DIR="$TMPBASE/esc04"
mkdir -p "$E4DIR"
cat > "$E4DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [
        {
            "id": "ESC-04A", "name": "marker_a",
            "pattern": "ESC_MARKER_A", "level": "advisory",
            "enabled": true, "message": "Marker A"
        },
        {
            "id": "ESC-04B", "name": "marker_b",
            "pattern": "ESC_MARKER_B", "level": "advisory",
            "enabled": true, "message": "Marker B"
        }
    ],
    "advisory_escalation": {
        "enabled": true, "soft_after": 3, "weight_multiplier": 1.5
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$E4DIR"
cat > "$E4DIR/test.naab" << 'EOF'
main {
    let a1 = <<javascript
// ESC_MARKER_A
1
>>
    let b1 = <<javascript
// ESC_MARKER_B
2
>>
    let a2 = <<javascript
// ESC_MARKER_A
3
>>
    let b2 = <<javascript
// ESC_MARKER_B
4
>>
    print("interleaved advisories OK")
}
EOF
OUT=$(cd "$E4DIR" && "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if [ $RC -eq 0 ]; then
    ok "I-ESC-04" "interleaved advisories don't cross-escalate (exit 0)"
else
    fail "I-ESC-04" "interleaved advisories cross-escalated (exit $RC)"
fi

# I-ESC-05: Escalation disabled = no multiplier, no SOFT escalation
E5DIR="$TMPBASE/esc05"
mkdir -p "$E5DIR"
cat > "$E5DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "ESC-05", "name": "no_esc",
        "pattern": "NO_ESC_MARKER", "level": "advisory",
        "enabled": true, "message": "No escalation"
    }],
    "advisory_escalation": {
        "enabled": false
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$E5DIR"
cat > "$E5DIR/test.naab" << 'EOF'
main {
    let a = <<javascript
// NO_ESC_MARKER
1
>>
    let b = <<javascript
// NO_ESC_MARKER
2
>>
    let c = <<javascript
// NO_ESC_MARKER
3
>>
    let d = <<javascript
// NO_ESC_MARKER
4
>>
    print("no escalation")
}
EOF
OUT=$(cd "$E5DIR" && "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if [ $RC -eq 0 ]; then
    ok "I-ESC-05" "escalation disabled = no SOFT escalation (exit 0)"
else
    fail "I-ESC-05" "escalation disabled but still blocked (exit $RC)"
fi

echo ""

# =====================================================================
# STANDING LEASE (5 tests) — require API key, skip gracefully
# =====================================================================
echo "--- Standing Lease (requires API key — skipping) ---"
for L in 01 02 03 04 05; do
    skip "I-LEASE-$L" "requires API key for agent.send()"
done

echo ""

# =====================================================================
# EVIDENCE EPOCH (5 tests)
# =====================================================================
echo "--- Evidence Epoch ---"

# I-EPOCH-01: Epoch accessible via governance.health()
EP1DIR="$TMPBASE/epoch01"
mkdir -p "$EP1DIR"
cat > "$EP1DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "governance_health": { "enabled": true },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$EP1DIR"
cat > "$EP1DIR/test.naab" << 'EOF'
use governance
main {
    let h = governance.health()
    let epoch = h.get("governance_epoch") ?? -1
    if epoch >= 0 {
        print("EPOCH=" + string(epoch))
    } else {
        print("EPOCH_MISSING")
    }
}
EOF
OUT=$(cd "$EP1DIR" && "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if echo "$OUT" | grep -q "EPOCH="; then
    ok "I-EPOCH-01" "epoch accessible via governance.health()"
else
    fail "I-EPOCH-01" "epoch not accessible (exit $RC, output: ${OUT:0:200})"
fi

# I-EPOCH-02: Epoch is non-negative integer
EPOCH_VAL=$(echo "$OUT" | grep -o 'EPOCH=[0-9]*' | cut -d= -f2)
if [ -n "$EPOCH_VAL" ] && [ "$EPOCH_VAL" -ge 0 ] 2>/dev/null; then
    ok "I-EPOCH-02" "epoch is non-negative ($EPOCH_VAL)"
else
    fail "I-EPOCH-02" "epoch not a valid non-negative integer"
fi

# I-EPOCH-03: governance.health() returns verdict field
EP3DIR="$TMPBASE/epoch03"
mkdir -p "$EP3DIR"
cat > "$EP3DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "governance_health": { "enabled": true },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$EP3DIR"
cat > "$EP3DIR/test.naab" << 'EOF'
use governance
main {
    let h = governance.health()
    let verdict = h.get("verdict") ?? "MISSING"
    print("VERDICT=" + string(verdict))
}
EOF
OUT=$(cd "$EP3DIR" && "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if echo "$OUT" | grep -qi "VERDICT=healthy\|VERDICT=degraded\|VERDICT=impaired"; then
    VERDICT=$(echo "$OUT" | grep -oi 'VERDICT=[a-z]*' | head -1 | cut -d= -f2)
    ok "I-EPOCH-03" "governance.health() returns verdict ($VERDICT)"
else
    fail "I-EPOCH-03" "verdict not in health output (exit $RC, output: ${OUT:0:200})"
fi

# I-EPOCH-04: Epoch is deterministic across runs (same config = same epoch)
EP4DIR="$TMPBASE/epoch04"
mkdir -p "$EP4DIR"
cat > "$EP4DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "governance_health": { "enabled": true },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$EP4DIR"
cat > "$EP4DIR/test.naab" << 'EOF'
use governance
main {
    let h = governance.health()
    let epoch = h.get("governance_epoch") ?? -1
    print("EPOCH=" + string(epoch))
}
EOF
OUT1=$(cd "$EP4DIR" && "$NAAB" test.naab 2>&1) && RC1=$? || RC1=$?
OUT2=$(cd "$EP4DIR" && "$NAAB" test.naab 2>&1) && RC2=$? || RC2=$?
EPOCH1=$(echo "$OUT1" | grep -o 'EPOCH=[0-9]*' | cut -d= -f2)
EPOCH2=$(echo "$OUT2" | grep -o 'EPOCH=[0-9]*' | cut -d= -f2)
if [ -n "$EPOCH1" ] && [ "$EPOCH1" = "$EPOCH2" ]; then
    ok "I-EPOCH-04" "epoch deterministic across runs ($EPOCH1)"
else
    fail "I-EPOCH-04" "epoch differs: run1=$EPOCH1 run2=$EPOCH2"
fi

# I-EPOCH-05: governance.health() includes instrumentation fields
EP5DIR="$TMPBASE/epoch05"
mkdir -p "$EP5DIR"
cat > "$EP5DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "governance_health": { "enabled": true },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$EP5DIR"
cat > "$EP5DIR/test.naab" << 'EOF'
use governance
main {
    let h = governance.health()
    let active = h.get("active") ?? false
    let total = h.get("total_checks") ?? -1
    let passes = h.get("consecutive_passes") ?? -1
    if active && total >= 0 && passes >= 0 {
        print("INSTRUMENTED=true")
    } else {
        print("INSTRUMENTED=false")
    }
}
EOF
OUT=$(cd "$EP5DIR" && "$NAAB" test.naab 2>&1) && RC=$? || RC=$?
if echo "$OUT" | grep -q "INSTRUMENTED=true"; then
    ok "I-EPOCH-05" "governance.health() includes instrumentation fields"
else
    fail "I-EPOCH-05" "instrumentation fields missing (output: ${OUT:0:200})"
fi

echo ""

# =====================================================================
# SUMMARY
# =====================================================================
TOTAL=$((PASS + FAIL + SKIP))
echo "=== Invariant Results: $PASS passed, $FAIL failed, $SKIP skipped (of $TOTAL tests) ==="

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "FAILED: State machine invariant violations detected."
    exit 1
fi

echo "  All testable invariants verified."
exit 0
