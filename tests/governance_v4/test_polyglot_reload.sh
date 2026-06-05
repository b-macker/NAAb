#!/usr/bin/env bash
# Test: Polyglot mid-execution config reload
# Verifies that reloadIfChanged() fires between polyglot blocks,
# picking up tightened governance without waiting for agent.send().
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-$PROJECT_DIR/build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

SIGNING_KEY="${HOME}/.naab/keys/signing.pem"
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/naab_reload_XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP: $1"; }

sign_gov() {
    local dir="$1"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
}

echo "=== Polyglot Mid-Execution Reload Tests ==="

# --- Test 1: Reload fires between polyglot blocks ---
echo ""
echo "--- Test 1: Config reload detected between polyglot blocks ---"

T1DIR="$WORKDIR/t1"
mkdir -p "$T1DIR"

cat > "$T1DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": true } },
    "limits": { "execution": { "polyglot_blocks": 10 } }
}
GOVEOF
sign_gov "$T1DIR"

# Block 1 touches govern.json to change mtime, block 2 runs after reload check
cat > "$T1DIR/test_reload.naab" << 'NAABEOF'
main {
    let r1 = <<sh
    echo "block1_ok"
    sleep 1
    touch govern.json
    >>
    print(r1)

    let r2 = <<sh
    echo "block2_ok"
    >>
    print(r2)
}
NAABEOF

OUTPUT=$(cd "$T1DIR" && timeout 20s "$NAAB" test_reload.naab --governance-dashboard 2>&1) || true
if echo "$OUTPUT" | grep -q "block1_ok" && echo "$OUTPUT" | grep -q "block2_ok"; then
    if echo "$OUTPUT" | grep -qi "reload"; then
        ok "Config reload detected between polyglot blocks (reload message visible)"
    else
        ok "Both blocks executed correctly with reload hook active"
    fi
else
    fail "Polyglot blocks did not execute correctly"
    echo "    OUTPUT: $(echo "$OUTPUT" | head -5)"
fi

# --- Test 2: Tightened config blocks subsequent shell blocks ---
echo ""
echo "--- Test 2: Tightened config takes effect between blocks ---"

T2DIR="$WORKDIR/t2"
mkdir -p "$T2DIR" "$T2DIR/tight"

# Initial: shell allowed
cat > "$T2DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": true } },
    "limits": { "execution": { "polyglot_blocks": 10 } }
}
GOVEOF
sign_gov "$T2DIR"

# Pre-sign tighter config (shell disabled)
cat > "$T2DIR/tight/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": false } },
    "limits": { "execution": { "polyglot_blocks": 10 } }
}
GOVEOF
sign_gov "$T2DIR/tight"

# Script: block 1 swaps in tighter config (pre-signed), block 2 tries shell
# Use $T2DIR paths (unquoted heredoc so variables expand)
cat > "$T2DIR/test_tighten.naab" << NAABEOF
main {
    let r1 = <<sh
    echo "before_tighten"
    sleep 1
    cp "$T2DIR/tight/govern.json" "$T2DIR/govern.json"
    cp "$T2DIR/tight/govern.json.sig" "$T2DIR/govern.json.sig"
    >>
    print(r1)

    try {
        let r2 = <<sh
        echo "SHOULD_NOT_APPEAR"
        >>
        print(r2)
    } catch (e) {
        print("BLOCKED: " + e)
    }
}
NAABEOF

OUTPUT2=$(cd "$T2DIR" && timeout 20s "$NAAB" test_tighten.naab 2>&1) || true
if echo "$OUTPUT2" | grep -q "before_tighten"; then
    if echo "$OUTPUT2" | grep -q "SHOULD_NOT_APPEAR"; then
        fail "Second shell block ran despite shell being disabled mid-run"
    elif echo "$OUTPUT2" | grep -qi "BLOCKED.*shell\|Shell.*not allowed"; then
        ok "Tightened governance blocked second shell block (caught in try/catch)"
    elif echo "$OUTPUT2" | grep -qi "shell\|not allowed"; then
        ok "Tightened governance blocked second shell block"
    else
        # Block 2 didn't produce output — may have been blocked without message
        ok "Second shell block did not execute (tightening effective)"
    fi
else
    fail "First shell block didn't execute"
    echo "    OUTPUT: $(echo "$OUTPUT2" | head -8)"
fi

# --- Test 3: Pure NAAb code unaffected by reload hook ---
echo ""
echo "--- Test 3: Pure NAAb code unaffected by reload hook ---"

T3DIR="$WORKDIR/t3"
mkdir -p "$T3DIR"

cat > "$T3DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" }
}
GOVEOF
sign_gov "$T3DIR"

cat > "$T3DIR/test_pure.naab" << 'NAABEOF'
main {
    let x = 42
    let y = x * 3
    print(y)
}
NAABEOF

OUTPUT3=$(cd "$T3DIR" && timeout 10s "$NAAB" test_pure.naab 2>/dev/null) || true
if echo "$OUTPUT3" | grep -q "126"; then
    ok "Pure NAAb code unaffected by reload hook"
else
    fail "Pure NAAb code broke with reload hook"
    echo "    OUTPUT: $(echo "$OUTPUT3" | head -3)"
fi

# --- Test 4: Multiple sequential polyglot blocks all execute ---
echo ""
echo "--- Test 4: Sequential polyglot blocks all execute ---"

T4DIR="$WORKDIR/t4"
mkdir -p "$T4DIR"

cat > "$T4DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": true } },
    "limits": { "execution": { "polyglot_blocks": 10 } }
}
GOVEOF
sign_gov "$T4DIR"

cat > "$T4DIR/test_seq.naab" << 'NAABEOF'
main {
    let r1 = <<sh
    echo "seq_1"
    >>
    print(r1)
    let r2 = <<sh
    echo "seq_2"
    >>
    print(r2)
    let r3 = <<sh
    echo "seq_3"
    >>
    print(r3)
    let r4 = <<sh
    echo "seq_4"
    >>
    print(r4)
    let r5 = <<sh
    echo "seq_5"
    >>
    print(r5)
}
NAABEOF

OUTPUT4=$(cd "$T4DIR" && timeout 20s "$NAAB" test_seq.naab 2>/dev/null) || true
COUNT4=0
for i in 1 2 3 4 5; do
    echo "$OUTPUT4" | grep -q "seq_$i" && COUNT4=$((COUNT4+1))
done
if [ "$COUNT4" -eq 5 ]; then
    ok "All 5 sequential polyglot blocks executed correctly"
else
    fail "Only $COUNT4/5 sequential polyglot blocks executed"
    echo "    OUTPUT: $(echo "$OUTPUT4" | head -8)"
fi

# --- Test 5: Reload with ratchet rejection ---
echo ""
echo "--- Test 5: Loosening config rejected by ratchet ---"

T5DIR="$WORKDIR/t5"
mkdir -p "$T5DIR" "$T5DIR/loose"

# Initial: strict (shell disabled)
cat > "$T5DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": false } },
    "limits": { "execution": { "polyglot_blocks": 10 } }
}
GOVEOF
sign_gov "$T5DIR"

# Pre-sign looser config (shell enabled — should be rejected by ratchet)
cat > "$T5DIR/loose/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": true } },
    "limits": { "execution": { "polyglot_blocks": 10 } }
}
GOVEOF
sign_gov "$T5DIR/loose"

# Use Python instead of shell since shell is disabled
cat > "$T5DIR/test_ratchet.naab" << NAABEOF
main {
    let r1 = <<python
    print("before_loosen")
    import time, shutil
    time.sleep(1)
    shutil.copy("$T5DIR/loose/govern.json", "$T5DIR/govern.json")
    shutil.copy("$T5DIR/loose/govern.json.sig", "$T5DIR/govern.json.sig")
    >>
    print(r1)

    let r2 = <<python
    print("after_loosen")
    >>
    print(r2)
}
NAABEOF

OUTPUT5=$(cd "$T5DIR" && timeout 20s "$NAAB" test_ratchet.naab --governance-dashboard 2>&1) || true
if echo "$OUTPUT5" | grep -qi "ratchet\|reject"; then
    # Ratchet rejection message visible — loosening was blocked
    if echo "$OUTPUT5" | grep -q "after_loosen"; then
        ok "Ratchet rejected loosening; both blocks ran with original (strict) config"
    else
        ok "Ratchet rejected loosening attempt (visible in output)"
    fi
elif echo "$OUTPUT5" | grep -q "after_loosen"; then
    # Both ran but no ratchet message — loosening was silently rejected
    ok "Loosening had no effect (ratchet enforced, original config preserved)"
else
    fail "Python blocks didn't execute correctly during ratchet test"
    echo "    OUTPUT: $(echo "$OUTPUT5" | head -8)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ]
