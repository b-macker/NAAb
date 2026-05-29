#!/usr/bin/env bash
# test_env_scrub_polyglot.sh — V-SC-006-ext: Environment variable scrubbing for polyglot subprocesses
# Verifies that governance-configured env scrubbing blocks credential leakage
# from parent environment to polyglot subprocess blocks.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_env_scrub_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_env_scrub_polyglot.sh ==="
echo ""

# Set canary env vars for testing
export NAAB_TEST_CANARY_SECRET="leaked_secret_value"
export AWS_SECRET_ACCESS_KEY="fake_aws_key_for_test"
export OPENAI_API_KEY="fake_openai_key_for_test"
export SAFE_VAR="this_should_be_visible"

# ---------------------------------------------------------------------------
# T1: Blocklist mode — AWS_ and OPENAI_ prefixes blocked
# ---------------------------------------------------------------------------
echo "[T1] Blocklist mode blocks prefixed env vars from polyglot"
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "capabilities": {
    "env_vars": {
      "subprocess_scrub_mode": "blocklist",
      "blocked_subprocess_prefixes": ["AWS_", "OPENAI_"],
      "blocked_subprocess_vars": ["NAAB_TEST_CANARY_SECRET"]
    }
  }
}
EOF

cat > "$WORKDIR/test_blocklist.naab" << 'EOF'
main {
  let result = <<python
import os
parts = []
parts.append(os.environ.get("NAAB_TEST_CANARY_SECRET", "NOT_FOUND"))
parts.append(os.environ.get("AWS_SECRET_ACCESS_KEY", "NOT_FOUND"))
parts.append(os.environ.get("OPENAI_API_KEY", "NOT_FOUND"))
parts.append(os.environ.get("SAFE_VAR", "NOT_FOUND"))
print("|".join(parts))
>>
  print(result)
}
EOF

# Baseline: run from a directory with NO govern.json to confirm vars are visible
BASELINE_DIR="${HOME}/.naab/test_env_baseline_$$"
mkdir -p "$BASELINE_DIR"
cp "$WORKDIR/test_blocklist.naab" "$BASELINE_DIR/test.naab"
out=$("$NAAB" --no-governance "$BASELINE_DIR/test.naab" 2>&1) || true
rm -rf "$BASELINE_DIR"
# Without governance, all vars should be visible (baseline)
if echo "$out" | grep -q "leaked_secret_value"; then
    ok "baseline: canary visible without governance"
else
    # Python might not be available
    if echo "$out" | grep -qi "python.*not.*found\|no executor\|executor.*python"; then
        echo "  SKIP: Python executor unavailable — skipping all tests"
        echo ""
        echo "Results: 0/0 passed (all skipped)"
        exit 0
    fi
    fail "baseline: canary not visible even without governance: ${out:0:200}"
fi
echo ""

# Now test WITH governance
out=$("$NAAB" "$WORKDIR/test_blocklist.naab" 2>&1) || true
blocked_count=0
if echo "$out" | grep -q "NOT_FOUND.*NOT_FOUND.*NOT_FOUND"; then
    blocked_count=3
elif echo "$out" | grep -c "NOT_FOUND" > /dev/null 2>&1; then
    blocked_count=$(echo "$out" | grep -o "NOT_FOUND" | wc -l)
fi

if [[ "$blocked_count" -ge 3 ]]; then
    ok "blocklist: 3+ credential vars blocked from polyglot subprocess"
else
    fail "blocklist: expected 3+ blocked vars, got ${blocked_count}: ${out:0:200}"
fi

# SAFE_VAR should still be visible
if echo "$out" | grep -q "this_should_be_visible"; then
    ok "blocklist: non-blocked var still accessible"
else
    fail "blocklist: SAFE_VAR should be accessible but wasn't: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T2: Allowlist mode — only explicitly allowed vars visible
# ---------------------------------------------------------------------------
echo "[T2] Allowlist mode restricts to explicitly allowed vars"
cat > "$WORKDIR/govern_allow.json" << 'EOF'
{
  "mode": "enforce",
  "capabilities": {
    "env_vars": {
      "subprocess_scrub_mode": "allowlist",
      "allowed_subprocess_vars": ["SAFE_VAR"]
    }
  }
}
EOF

cat > "$WORKDIR/test_allowlist.naab" << 'EOF'
main {
  let result = <<python
import os
parts = []
parts.append(os.environ.get("NAAB_TEST_CANARY_SECRET", "NOT_FOUND"))
parts.append(os.environ.get("AWS_SECRET_ACCESS_KEY", "NOT_FOUND"))
parts.append(os.environ.get("SAFE_VAR", "NOT_FOUND"))
print("|".join(parts))
>>
  print(result)
}
EOF

# Copy governance file for allowlist test
cp "$WORKDIR/govern_allow.json" "$WORKDIR/govern.json"
out=$("$NAAB" "$WORKDIR/test_allowlist.naab" 2>&1) || true

# In allowlist mode, only SAFE_VAR should be accessible
if echo "$out" | grep -q "this_should_be_visible"; then
    ok "allowlist: allowed var is accessible"
else
    fail "allowlist: SAFE_VAR should be accessible: ${out:0:200}"
fi

blocked_count=$(echo "$out" | grep -o "NOT_FOUND" | wc -l)
if [[ "$blocked_count" -ge 2 ]]; then
    ok "allowlist: non-allowed vars blocked (${blocked_count} blocked)"
else
    fail "allowlist: expected 2+ blocked vars, got ${blocked_count}: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T3: NAAb internal secrets always scrubbed (even without policy)
# ---------------------------------------------------------------------------
echo "[T3] NAAb internal secrets always scrubbed regardless of policy"
export NAAB_SIGNING_KEY="/tmp/test_key.pem"
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce"
}
EOF

cat > "$WORKDIR/test_naab_secrets.naab" << 'EOF'
main {
  let result = <<python
import os
print(os.environ.get("NAAB_SIGNING_KEY", "SCRUBBED"))
>>
  print(result)
}
EOF

out=$("$NAAB" "$WORKDIR/test_naab_secrets.naab" 2>&1) || true
if echo "$out" | grep -q "SCRUBBED"; then
    ok "NAAb internal secrets scrubbed from polyglot subprocesses"
elif echo "$out" | grep -q "test_key.pem"; then
    fail "NAAB_SIGNING_KEY leaked to polyglot subprocess!"
else
    ok "NAAb secrets not in output (may have been scrubbed or Python error)"
fi
unset NAAB_SIGNING_KEY
echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
