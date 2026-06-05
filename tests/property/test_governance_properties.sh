#!/usr/bin/env bash
# Property-Based Test: Governance System Properties
#
# PROPERTIES:
#   P1: Epoch is accessible and non-negative
#   P2: Score determinism — same input = same score across runs
#   P3: Advisory count determinism — same input = same advisory count
#   P4: Taint transparency — clean code passes under hard taint enforcement
#   P5: Score integrity — no integrity mismatch for standard scoring
#   P6: Governance transparency — pure computation output unchanged by governance

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB_BIN:-$(cd "$SCRIPT_DIR/../.." && pwd)/build/naab-lang}"
PASS=0
FAIL=0
WORKDIR="${HOME}/.naab/test_govprop_$$"
mkdir -p "$WORKDIR"
trap 'rm -rf "$WORKDIR"' EXIT

# Sign govern.json in a directory (trusted keys require signatures)
SIGNING_KEY="${HOME}/.naab/keys/signing.pem"
sign_gov() {
    local dir="$1"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
}

ok()   { PASS=$((PASS + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL: $1"; }

echo "--- Invariant 6: Governance Properties ---"

# P1: Epoch accessible and non-negative
P1DIR="$WORKDIR/p1"
mkdir -p "$P1DIR"
cat > "$P1DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "governance_health": { "enabled": true },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$P1DIR"
cat > "$P1DIR/test.naab" << 'EOF'
use governance

main {
    let h = governance.health()
    let epoch = h.get("governance_epoch") ?? -1
    if epoch >= 0 {
        print("EPOCH_OK")
    } else {
        print("EPOCH_FAIL")
    }
}
EOF
OUT=$("$NAAB" "$P1DIR/test.naab" 2>&1) && RC=$? || RC=$?
if echo "$OUT" | grep -q "EPOCH_OK"; then
    ok "P1: epoch accessible and non-negative"
else
    fail "P1: epoch not accessible or negative (exit $RC)"
fi

# P2: Score determinism — same file + same govern.json = same score
P2DIR="$WORKDIR/p2"
mkdir -p "$P2DIR"
cat > "$P2DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [
        {
            "id": "DET-RULE",
            "name": "determinism_marker",
            "pattern": "DET_CHECK",
            "level": "advisory",
            "enabled": true,
            "message": "Determinism check"
        }
    ],
    "scoring": {
        "enabled": true,
        "default_weight": 5,
        "yellow_threshold": 100,
        "red_threshold": 200
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$P2DIR"
cat > "$P2DIR/test.naab" << 'EOF'
main {
    let a = <<javascript
// DET_CHECK
1 + 1
>>
    let b = <<javascript
// DET_CHECK
2 + 2
>>
    print("done")
}
EOF
OUT_A=$("$NAAB" --governance-dashboard "$P2DIR/test.naab" 2>&1) || true
OUT_B=$("$NAAB" --governance-dashboard "$P2DIR/test.naab" 2>&1) || true
SCORE_A=$(echo "$OUT_A" | grep -oi "Risk score: [0-9]*" | head -1)
SCORE_B=$(echo "$OUT_B" | grep -oi "Risk score: [0-9]*" | head -1)
if [ -n "$SCORE_A" ] && [ "$SCORE_A" = "$SCORE_B" ]; then
    ok "P2: score deterministic across runs ($SCORE_A)"
elif [ -z "$SCORE_A" ] && [ -z "$SCORE_B" ]; then
    # Both below yellow threshold — compare check counts instead
    CK_A=$(echo "$OUT_A" | grep -o "[0-9]* passed" | head -1)
    CK_B=$(echo "$OUT_B" | grep -o "[0-9]* passed" | head -1)
    if [ "$CK_A" = "$CK_B" ]; then
        ok "P2: governance deterministic (below scoring threshold, $CK_A)"
    else
        fail "P2: check counts differ: run1='$CK_A' run2='$CK_B'"
    fi
else
    fail "P2: score mismatch: run1='$SCORE_A' run2='$SCORE_B'"
fi

# P3: Advisory count determinism
ADV_A=$(echo "$OUT_A" | grep -oc "WARNING\|ADVISORY\|advisory" || true)
ADV_B=$(echo "$OUT_B" | grep -oc "WARNING\|ADVISORY\|advisory" || true)
if [ "$ADV_A" = "$ADV_B" ]; then
    ok "P3: advisory count deterministic ($ADV_A occurrences)"
else
    fail "P3: advisory count differs: run1=$ADV_A run2=$ADV_B"
fi

# P4: Taint transparency — clean code (no taint sources) passes with hard taint
P4DIR="$WORKDIR/p4"
mkdir -p "$P4DIR"
cat > "$P4DIR/govern.json" << EOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": {
        "filesystem": { "mode": "readwrite", "allowed_paths": ["$P4DIR/"] }
    },
    "taint_tracking": {
        "enabled": true,
        "level": "hard",
        "sources": ["env.get", "file.read"],
        "sinks": ["file.write"],
        "sanitizers": ["sanitize_"]
    }
}
EOF
sign_gov "$P4DIR"
cat > "$P4DIR/test.naab" << EOF
use file

main {
    let data = "clean static data"
    file.write("$P4DIR/clean.txt", data)
    print("TAINT_CLEAN")
}
EOF
OUT=$("$NAAB" "$P4DIR/test.naab" 2>&1) && RC=$? || RC=$?
if echo "$OUT" | grep -q "TAINT_CLEAN"; then
    ok "P4: clean code passes under hard taint (no false positive)"
else
    fail "P4: clean code blocked by taint (exit $RC)"
fi

# P5: Score integrity — no mismatch for standard scoring (no escalation)
P5DIR="$WORKDIR/p5"
mkdir -p "$P5DIR"
cat > "$P5DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [
        {
            "id": "INTEG-RULE",
            "name": "integrity_marker",
            "pattern": "INTEG_CHECK",
            "level": "advisory",
            "enabled": true,
            "message": "Integrity check"
        }
    ],
    "scoring": {
        "enabled": true,
        "default_weight": 10,
        "yellow_threshold": 100,
        "red_threshold": 500
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$P5DIR"
cat > "$P5DIR/test.naab" << 'EOF'
main {
    let a = <<javascript
// INTEG_CHECK
1
>>
    let b = <<javascript
// INTEG_CHECK
2
>>
    let c = <<javascript
// INTEG_CHECK
3
>>
    print("integrity test done")
}
EOF
OUT=$("$NAAB" --governance-dashboard "$P5DIR/test.naab" 2>&1) && RC=$? || RC=$?
if [ $RC -eq 0 ] && ! echo "$OUT" | grep -qi "integrity.*mismatch\|score.*tamper"; then
    ok "P5: score integrity holds (no mismatch, exit $RC)"
else
    fail "P5: score integrity issue (exit $RC)"
fi

# P6: Governance transparency — pure computation produces correct result
P6DIR="$WORKDIR/p6"
mkdir -p "$P6DIR"
cat > "$P6DIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$P6DIR"
cat > "$P6DIR/test.naab" << 'EOF'
main {
    let sum = 0
    for i in 1..=10 {
        sum = sum + i
    }
    print(string(sum))
}
EOF
OUT_A=$("$NAAB" "$P6DIR/test.naab" 2>/dev/null) && RC_A=$? || RC_A=$?
OUT_B=$("$NAAB" "$P6DIR/test.naab" 2>/dev/null) && RC_B=$? || RC_B=$?
if [ "$OUT_A" = "55" ] && [ "$OUT_A" = "$OUT_B" ] && [ $RC_A -eq 0 ]; then
    ok "P6: governance transparency (correct result, deterministic across runs)"
else
    fail "P6: governance altered output (run1='$OUT_A' run2='$OUT_B' expected='55')"
fi

# Summary
echo ""
TOTAL=$((PASS + FAIL))
echo "  Governance Properties: $PASS/$TOTAL passed"
if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
