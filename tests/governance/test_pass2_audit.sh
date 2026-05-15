#!/usr/bin/env bash
# test_pass2_audit.sh — Governance Pass 2: Post-Execution Validation Audit
set -uo pipefail
# Note: no set -e — tests check exit codes and output patterns manually

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

# Skip on platforms without working polyglot executors (Windows CI)
# Check if naab-lang can actually execute a python block (pybind11 must be built)
PROBE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pass2_probe_XXXXXX")
cat > "$PROBE_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"off"}
EOF
cat > "$PROBE_DIR/probe.naab" <<'EOF'
main { let x = <<python
print("ok")
>>
print(x) }
EOF
PROBE_OUT=$("$NAAB" --no-governance "$PROBE_DIR/probe.naab" 2>&1 || true)
rm -rf "$PROBE_DIR"
if echo "$PROBE_OUT" | grep -q "No executor found\|not available"; then
    echo "  test_pass2_audit.sh: SKIPPED (no python/shell executor on this platform)"
    exit 0
fi

PASS=0; FAIL=0; TOTAL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); }

WORK_DIR="${HOME}/.naab/pass2_test_$$"
mkdir -p "$WORK_DIR"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

# Governance config: advisory mode with taint tracking
cat > "$WORK_DIR/govern.json" <<'GOVEOF'
{
  "mode": "audit",
  "sandbox_level": "unrestricted",
  "taint_tracking": {
    "enabled": true,
    "level": "advisory",
    "sources": ["env.get", "io.read_line", "file.read", "polyglot_output"],
    "sinks": ["shell_exec", "file.write"],
    "sanitizers": ["sanitize_"]
  }
}
GOVEOF

echo "=== test_pass2_audit.sh ==="

# ---------------------------------------------------------------------------
# Test 1: Compact output when no findings
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/test_clean.naab" <<'EOF'
main {
    let x = 42
    print(x)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test_clean.naab 2>&1)
if echo "$OUTPUT" | grep -q "Governance: PASS"; then
    ok "compact output on clean execution"
else
    fail "compact output on clean execution"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
# Test 2: Determinism audit — shell block with date
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/test_determ.naab" <<'EOF'
main {
    let result = <<shell
date
echo "done"
>>
    print(result)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test_determ.naab 2>&1 || true)
if echo "$OUTPUT" | grep -q "Determinism"; then
    ok "determinism audit detects date command"
else
    fail "determinism audit detects date command"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
# Test 3: Determinism audit — python random
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/test_random.naab" <<'EOF'
main {
    let result = <<python
import random
print(random.randint(1, 100))
>>
    print(result)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test_random.naab 2>&1 || true)
if echo "$OUTPUT" | grep -q "random function"; then
    ok "determinism audit detects random in python"
else
    fail "determinism audit detects random in python"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
# Test 4: Clean python block — no determinism finding
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/test_pure.naab" <<'EOF'
main {
    let result = <<python
print(2 + 2)
>>
    print(result)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test_pure.naab 2>&1 || true)
if echo "$OUTPUT" | grep -q "Determinism"; then
    fail "pure math should not trigger determinism"
    echo "    Got: $OUTPUT"
else
    ok "pure math does not trigger determinism"
fi

# ---------------------------------------------------------------------------
# Test 5: Resource usage reported
# ---------------------------------------------------------------------------
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test_determ.naab 2>&1 || true)
if echo "$OUTPUT" | grep -q "Resource Usage"; then
    ok "resource usage section in report"
else
    fail "resource usage section in report"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
# Test 6: Multiple polyglot blocks — correct block count
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/test_multi.naab" <<'EOF'
main {
    let a = <<python
import random
print(random.randint(1, 10))
>>
    let b = <<shell
date
echo "hello"
>>
    print(a)
    print(b)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test_multi.naab 2>&1 || true)
if echo "$OUTPUT" | grep -q "2 blocks executed"; then
    ok "multi-block: correct block count"
else
    fail "multi-block: correct block count"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
# Test 7: Coverage section present
# ---------------------------------------------------------------------------
if echo "$OUTPUT" | grep -q "Coverage"; then
    ok "coverage section in report"
else
    fail "coverage section in report"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
# Test 8: Verdict line present
# ---------------------------------------------------------------------------
if echo "$OUTPUT" | grep -q "Verdict:"; then
    ok "verdict line in report"
else
    fail "verdict line in report"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
# Test 9: Taint flow — tainted var reaching shell sink
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/test_taint.naab" <<'EOF'
main {
    let user_data = <<python
print("user input")
>>
    let result = <<shell[user_data]
echo $user_data
>>
    print(result)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test_taint.naab 2>&1 || true)
if echo "$OUTPUT" | grep -q "Taint.*tracking.*violation\|taint flow\|runtime"; then
    ok "taint flow audit — tainted var reaches shell sink"
else
    fail "taint flow audit — tainted var reaches shell sink"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
# Test 10: Pass 2 runs automatically (no flag needed)
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/test_auto.naab" <<'EOF'
main {
    let x = <<shell
date
>>
    print(x)
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" test_auto.naab 2>&1 || true)
if echo "$OUTPUT" | grep -q "Governance"; then
    ok "pass 2 runs automatically without flags"
else
    fail "pass 2 runs automatically without flags"
    echo "    Got: $OUTPUT"
fi

# ---------------------------------------------------------------------------
echo ""
echo "Results: $PASS passed, $FAIL failed, $TOTAL total"
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
