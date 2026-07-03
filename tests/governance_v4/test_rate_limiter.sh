#!/usr/bin/env bash
# test_rate_limiter.sh — V-GOV-RATE: Rate limiter enforcement
# Verifies that checkPolyglotRate(), checkStdlibRate(), checkFileOpsRate()
# are correctly wired and enforced.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_rate_limiter_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_rate_limiter.sh ==="
echo ""

# ---------------------------------------------------------------------------
# T1: Polyglot rate limit — 1 per second, execute 5 rapidly
# ---------------------------------------------------------------------------
echo "[T1] Polyglot rate limit enforcement"
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": {
    "rate": {
      "max_polyglot_per_second": 1
    }
  }
}
EOF

cat > "$WORKDIR/test_polyglot_rate.naab" << 'EOF'
main {
  let a = <<python
print("1")
>>
  let b = <<python
print("2")
>>
  let c = <<python
print("3")
>>
}
EOF

out=$("$NAAB" "$WORKDIR/test_polyglot_rate.naab" 2>&1) || true
if echo "$out" | grep -qi "rate limit\|rate.*exceeded"; then
    ok "polyglot rate limit fires"
else
    fail "polyglot rate limit should fire with 3 blocks at 1/sec: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T2: Polyglot rate limit disabled (0 = unlimited)
# ---------------------------------------------------------------------------
echo "[T2] Polyglot rate limit disabled when 0"
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": {
    "rate": {
      "max_polyglot_per_second": 0
    }
  }
}
EOF

cat > "$WORKDIR/test_polyglot_nolimit.naab" << 'EOF'
main {
  let a = <<python
print("1")
>>
  let b = <<python
print("2")
>>
}
EOF

out=$("$NAAB" "$WORKDIR/test_polyglot_nolimit.naab" 2>&1) || true
if echo "$out" | grep -qi "rate limit"; then
    fail "rate limit should NOT fire when max=0: ${out:0:200}"
else
    ok "rate limit disabled when max=0"
fi
echo ""

# ---------------------------------------------------------------------------
# T3: Stdlib rate limit
# ---------------------------------------------------------------------------
echo "[T3] Stdlib rate limit enforcement"
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": {
    "rate": {
      "max_stdlib_calls_per_second": 2
    }
  }
}
EOF

cat > "$WORKDIR/test_stdlib_rate.naab" << 'EOF'
use array
main {
  let a = [1, 2, 3]
  let x = array.length(a)
  let y = array.length(a)
  let z = array.length(a)
  let w = array.length(a)
  let v = array.length(a)
  print(x)
}
EOF

out=$("$NAAB" "$WORKDIR/test_stdlib_rate.naab" 2>&1) || true
if echo "$out" | grep -qi "rate limit\|rate.*exceeded"; then
    ok "stdlib rate limit fires"
else
    fail "stdlib rate limit should fire with 5 calls at 2/sec: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T4: File ops rate limit
# ---------------------------------------------------------------------------
echo "[T4] File ops rate limit enforcement"
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": {
    "sandbox_level": "elevated"
  },
  "limits": {
    "rate": {
      "max_file_ops_per_second": 1
    }
  }
}
EOF

# Create a test file to read
echo "test content" > "$WORKDIR/testfile.txt"

# The native Windows binary can't resolve MSYS-style absolute paths embedded
# in .naab sources; convert to a mixed (C:/...) path under MSYS/Git Bash
NAAB_WORKDIR="$WORKDIR"
if command -v cygpath &>/dev/null; then
    NAAB_WORKDIR=$(cygpath -m "$WORKDIR")
fi

cat > "$WORKDIR/test_file_rate.naab" << NAABEOF
use file
main {
  let a = file.read("$NAAB_WORKDIR/testfile.txt")
  let b = file.read("$NAAB_WORKDIR/testfile.txt")
  let c = file.read("$NAAB_WORKDIR/testfile.txt")
  print(a)
}
NAABEOF

out=$("$NAAB" "$WORKDIR/test_file_rate.naab" 2>&1) || true
if echo "$out" | grep -qi "rate limit\|rate.*exceeded"; then
    ok "file ops rate limit fires"
else
    fail "file ops rate limit should fire with 3 reads at 1/sec: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T5: VM path — polyglot rate limit (VM is default)
# ---------------------------------------------------------------------------
echo "[T5] VM polyglot rate limit (default engine)"
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": {
    "rate": {
      "max_polyglot_per_second": 1
    }
  }
}
EOF

# Same test as T1 but explicitly confirm VM is default
out=$("$NAAB" "$WORKDIR/test_polyglot_rate.naab" 2>&1) || true
if echo "$out" | grep -qi "rate limit\|rate.*exceeded"; then
    ok "VM polyglot rate limit fires"
else
    fail "VM polyglot rate limit should fire: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T6: Tree-walker path — polyglot rate limit
# ---------------------------------------------------------------------------
echo "[T6] Tree-walker polyglot rate limit"
out=$("$NAAB" --tree-walk "$WORKDIR/test_polyglot_rate.naab" 2>&1) || true
if echo "$out" | grep -qi "rate limit\|rate.*exceeded"; then
    ok "tree-walker polyglot rate limit fires"
else
    fail "tree-walker polyglot rate limit should fire: ${out:0:200}"
fi
echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
