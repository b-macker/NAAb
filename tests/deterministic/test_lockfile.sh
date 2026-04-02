#!/bin/bash
# Test naab.lock deterministic build support
# Phase 8.4 — Deterministic Builds

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${SCRIPT_DIR}/../.."
NAAB="${ROOT}/build/naab-lang"

PASS=0
FAIL=0

# Use a temp directory inside the project (no /tmp on Termux)
TMPDIR="${ROOT}/.test_lockfile_$$"
mkdir -p "$TMPDIR"

cleanup() {
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

check() {
    local name="$1"
    local result="$2"
    if [ "$result" = "0" ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -f "$NAAB" ]; then
    echo "SKIP: naab-lang not built at $NAAB"
    exit 0
fi

# Create a minimal govern.json + test script in TMPDIR
cat > "${TMPDIR}/govern.json" << 'EOF'
{
  "version": "1.0",
  "mode": "advisory"
}
EOF

cat > "${TMPDIR}/hello.naab" << 'EOF'
main {
    print("hello lockfile test")
}
EOF

mkdir -p "${TMPDIR}/.naab"

# On MSYS2/Windows, python3 is native Windows and can't resolve POSIX paths
LOCK_PY="${TMPDIR}/.naab/naab.lock"
if command -v cygpath &>/dev/null; then
    LOCK_PY=$(cygpath -m "${TMPDIR}/.naab/naab.lock")
fi

# ============================================================================
# Test 1: --lock creates naab.lock file
# ============================================================================
"$NAAB" "${TMPDIR}/hello.naab" --lock 2>/dev/null
check "--lock creates .naab/naab.lock" "$([ -f "${TMPDIR}/.naab/naab.lock" ]; echo $?)"

# ============================================================================
# Test 2: naab.lock is valid JSON
# ============================================================================
if [ -f "${TMPDIR}/.naab/naab.lock" ]; then
    python3 -c "import json,sys; json.load(open('${LOCK_PY}'))" 2>/dev/null
    check "naab.lock is valid JSON" $?
else
    FAIL=$((FAIL + 1))
    echo "  FAIL: naab.lock is valid JSON (file not created)"
fi

# ============================================================================
# Test 3: naab.lock contains naab_version and platform fields
# ============================================================================
if [ -f "${TMPDIR}/.naab/naab.lock" ]; then
    python3 -c "
import json
data = json.load(open('${LOCK_PY}'))
assert 'naab_version' in data, 'missing naab_version'
assert 'platform' in data, 'missing platform'
assert 'runtimes' in data, 'missing runtimes'
print('ok')
" 2>/dev/null | grep -q 'ok'
    check "naab.lock has naab_version, platform, runtimes fields" $?
else
    FAIL=$((FAIL + 1))
    echo "  FAIL: naab.lock has required fields (file not created)"
fi

# ============================================================================
# Test 4: --lock-check with matching lock succeeds (exit 0)
# ============================================================================
"$NAAB" "${TMPDIR}/hello.naab" --lock 2>/dev/null          # Write lock first
"$NAAB" "${TMPDIR}/hello.naab" --lock-check 2>/dev/null    # Check against itself
check "--lock-check with consistent lock exits 0" $?

# ============================================================================
# Test 5: --lock-path flag accepts custom lockfile path
# ============================================================================
CUSTOM_LOCK="${TMPDIR}/custom.lock"
"$NAAB" "${TMPDIR}/hello.naab" --lock --lock-path "$CUSTOM_LOCK" 2>/dev/null
check "--lock-path writes to custom path" "$([ -f "$CUSTOM_LOCK" ]; echo $?)"

# ============================================================================
echo ""
echo "Lockfile tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
