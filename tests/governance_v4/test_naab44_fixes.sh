#!/usr/bin/env bash
# test_naab44_fixes.sh — naab-44 governance gap fixes
# Verifies 4 structural fixes:
#   A: Circuit breaker decoupled from reality_checkpoint
#   B: Challenge failure throws GovernanceHardError (uncatchable)
#   C: Diminishing recovery config parsing
#   D: Config field depth validation

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TMPBASE="$_SYSTMP/test_naab44_$$"
mkdir -p "$TMPBASE"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
"$NAAB" --keygen "$TMPBASE/test-key.pem" 2>/dev/null
"$NAAB" --trust-key "$TMPBASE/test-key.pem.pub" 2>/dev/null
SIGNING_KEY="$TMPBASE/test-key.pem"

PASS=0; FAIL=0; SKIP=0

cleanup() { teardown_isolated_trust; rm -rf "$TMPBASE"; }
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

check_not_contains() {
    local id="$1" desc="$2" file="$3" pattern="$4"
    if grep -q "$pattern" "$file" 2>/dev/null; then
        echo "  FAIL [$id] $desc (found '$pattern' in output)"
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

echo "=== naab-44 Governance Gap Fixes ==="
echo ""

# ═══════════════════════════════════════════════════════════
# Group A: Circuit breaker decoupled from reality_checkpoint
# ═══════════════════════════════════════════════════════════
echo "--- Group A: Circuit breaker decoupling ---"

# A-01: reality_checkpoint disabled + circuit_breaker enabled → governance.health() works
ADIR="$TMPBASE/a01"
mkdir -p "$ADIR"
cat > "$ADIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "CB decoupled from RC",
  "context_drift": {
    "enabled": true,
    "coherence_threshold": 0.3,
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": { "enabled": true },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write" } }
}
EOF
sign_govern "$ADIR"

cat > "$ADIR/test.naab" <<'NAABEOF'
use governance

main {
    let h = governance.health()
    print("health_verdict: " + h.get("verdict"))
    print("health_ok: true")
}
NAABEOF

OUT_A01=$("$NAAB" "$ADIR/test.naab" 2>/dev/null) || true
check_grep "A-01" "governance.health() works with RC disabled + CB enabled" "health_ok: true" "$OUT_A01"

# A-02: both disabled → governance_level stays normal
ADIR2="$TMPBASE/a02"
mkdir -p "$ADIR2"
cat > "$ADIR2/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Both disabled baseline",
  "context_drift": {
    "enabled": true,
    "coherence_threshold": 0.3,
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": { "enabled": false },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write" } }
}
EOF
sign_govern "$ADIR2"

cat > "$ADIR2/test.naab" <<'NAABEOF'
use governance

main {
    let h = governance.health()
    print("governance_level: " + string(h.get("governance_level")))
}
NAABEOF

OUT_A02=$("$NAAB" "$ADIR2/test.naab" 2>/dev/null) || true
# governance_level might be 0, "normal", or null (when CB disabled — no level computed)
if echo "$OUT_A02" | grep -q "governance_level: normal" 2>/dev/null || \
   echo "$OUT_A02" | grep -q "governance_level: 0" 2>/dev/null || \
   echo "$OUT_A02" | grep -q "governance_level: null" 2>/dev/null; then
    echo "  PASS [A-02] governance_level is normal/0/null when both disabled"
    PASS=$((PASS + 1))
else
    echo "  FAIL [A-02] governance_level is normal/0/null when both disabled (got: $OUT_A02)"
    FAIL=$((FAIL + 1))
fi

# A-03: RC disabled + CB enabled → pulse verdict computed
ADIR3="$TMPBASE/a03"
mkdir -p "$ADIR3"
cat > "$ADIR3/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Pulse without RC",
  "context_drift": {
    "enabled": true,
    "coherence_threshold": 0.3,
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": { "enabled": true },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write" } }
}
EOF
sign_govern "$ADIR3"

cat > "$ADIR3/test.naab" <<'NAABEOF'
use governance

main {
    let h = governance.health()
    let v = h.get("verdict")
    if v != null {
        print("has_verdict: true")
    } else {
        print("has_verdict: false")
    }
}
NAABEOF

OUT_A03=$("$NAAB" "$ADIR3/test.naab" 2>/dev/null) || true
check_grep "A-03" "pulse verdict computed with RC disabled" "has_verdict: true" "$OUT_A03"

echo ""

# ═══════════════════════════════════════════════════════════
# Group B: Challenge failure is uncatchable (GovernanceHardError)
# ═══════════════════════════════════════════════════════════
echo "--- Group B: Challenge failure uncatchable ---"

# B-01: HARD governance block via blocked env var → try/catch cannot catch → exit 3
BDIR="$TMPBASE/b01"
mkdir -p "$BDIR"
cat > "$BDIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Uncatchable test",
  "capabilities": {
    "filesystem": { "mode": "read_write" },
    "env_vars": {
      "blocked_read": ["FORBIDDEN_VAR"]
    }
  },
  "security": { "sandbox_level": "elevated" }
}
EOF
sign_govern "$BDIR"

cat > "$BDIR/test.naab" <<'NAABEOF'
use env

main {
    try {
        let v = env.get("FORBIDDEN_VAR")
        print("CATCH_BYPASSED")
    } catch (e) {
        print("CAUGHT: " + str(e))
    }
    print("AFTER_TRY")
}
NAABEOF

set +e
"$NAAB" "$BDIR/test.naab" > "$TMPBASE/b01_out.txt" 2>"$TMPBASE/b01_err.txt"
B01_EXIT=$?
set -e

check "B-01" "HARD block exits with code 3 (uncatchable)" "3" "$B01_EXIT"
check_not_contains "B-02" "try/catch body not executed" "$TMPBASE/b01_out.txt" "CAUGHT"
check_not_contains "B-03" "post-try code not reached" "$TMPBASE/b01_out.txt" "AFTER_TRY"

echo ""

# ═══════════════════════════════════════════════════════════
# Group C: Diminishing recovery + config parsing
# ═══════════════════════════════════════════════════════════
echo "--- Group C: Diminishing recovery config ---"

# C-01: coherence_recovery_cap: 0.95 parses without error
CDIR="$TMPBASE/c01"
mkdir -p "$CDIR"
cat > "$CDIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Recovery cap test",
  "context_drift": {
    "enabled": true,
    "coherence_threshold": 0.3,
    "coherence_recovery_amount": 0.2,
    "coherence_recovery_cap": 0.95
  },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write" } }
}
EOF
sign_govern "$CDIR"

cat > "$CDIR/test.naab" <<'NAABEOF'
main {
    print("config_parsed: true")
}
NAABEOF

set +e
OUT_C01=$("$NAAB" "$CDIR/test.naab" 2>/dev/null)
C01_EXIT=$?
set -e
check "C-01" "coherence_recovery_cap: 0.95 parses without error" "0" "$C01_EXIT"

# C-02: coherence_recovery_cap: 1.0 (backward compat) parses
CDIR2="$TMPBASE/c02"
mkdir -p "$CDIR2"
cat > "$CDIR2/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Recovery cap backward compat",
  "context_drift": {
    "enabled": true,
    "coherence_threshold": 0.3,
    "coherence_recovery_cap": 1.0
  },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write" } }
}
EOF
sign_govern "$CDIR2"

cat > "$CDIR2/test.naab" <<'NAABEOF'
main {
    print("config_parsed: true")
}
NAABEOF

set +e
OUT_C02=$("$NAAB" "$CDIR2/test.naab" 2>/dev/null)
C02_EXIT=$?
set -e
check "C-02" "coherence_recovery_cap: 1.0 (backward compat) parses" "0" "$C02_EXIT"

# C-03: coherence_natural_healing > 0 parses
CDIR3="$TMPBASE/c03"
mkdir -p "$CDIR3"
cat > "$CDIR3/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Natural healing test",
  "context_drift": {
    "enabled": true,
    "coherence_threshold": 0.3,
    "coherence_natural_healing": 0.05
  },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write" } }
}
EOF
sign_govern "$CDIR3"

cat > "$CDIR3/test.naab" <<'NAABEOF'
main {
    print("config_parsed: true")
}
NAABEOF

set +e
OUT_C03=$("$NAAB" "$CDIR3/test.naab" 2>/dev/null)
C03_EXIT=$?
set -e
check "C-03" "coherence_natural_healing: 0.05 parses without error" "0" "$C03_EXIT"

echo ""

# ═══════════════════════════════════════════════════════════
# Group D: Config field depth validation
# ═══════════════════════════════════════════════════════════
echo "--- Group D: Config depth validation ---"

# D-01: Full config with all new fields → governance.health() doesn't crash
DDIR="$TMPBASE/d01"
mkdir -p "$DDIR"
cat > "$DDIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Full field depth test",
  "context_drift": {
    "enabled": true,
    "coherence_threshold": 0.3,
    "coherence_recovery_amount": 0.2,
    "coherence_recovery_cap": 0.95,
    "coherence_natural_healing": 0.02,
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": {
    "enabled": true,
    "elevated_threshold": 0.4,
    "high_threshold": 0.6,
    "critical_threshold": 0.8
  },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write" } }
}
EOF
sign_govern "$DDIR"

cat > "$DDIR/test.naab" <<'NAABEOF'
use governance

main {
    let h = governance.health()
    print("health_verdict: " + h.get("verdict"))
    print("depth_ok: true")
}
NAABEOF

set +e
OUT_D01=$("$NAAB" "$DDIR/test.naab" 2>/dev/null)
D01_EXIT=$?
set -e

check "D-01" "full config with all new fields runs without crash" "0" "$D01_EXIT"
check_grep "D-02" "governance.health() returns after full config" "depth_ok: true" "$OUT_D01"

# D-03: No reality_checkpoint section + circuit_breaker enabled → uses struct defaults
DDIR3="$TMPBASE/d03"
mkdir -p "$DDIR3"
cat > "$DDIR3/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "CB without RC section",
  "context_drift": {
    "enabled": true,
    "coherence_threshold": 0.3
  },
  "circuit_breaker": { "enabled": true },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write" } }
}
EOF
sign_govern "$DDIR3"

cat > "$DDIR3/test.naab" <<'NAABEOF'
use governance

main {
    let h = governance.health()
    print("struct_defaults_ok: true")
}
NAABEOF

set +e
OUT_D03=$("$NAAB" "$DDIR3/test.naab" 2>/dev/null)
D03_EXIT=$?
set -e

check "D-03" "CB enabled without RC section uses struct defaults" "0" "$D03_EXIT"

echo ""
echo "=== naab-44 Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ "$FAIL" -gt 0 ]; then
    echo "  SOME TESTS FAILED"
    exit 1
fi

echo "  test_naab44_fixes.sh: ALL PASSED"
exit 0
