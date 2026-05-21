#!/usr/bin/env bash
# test_platform_fixes.sh — Regression tests for libnaab-governance platform fixes (PR #13)
#
# Covers fixes that had NO automated test coverage:
#   Fix 1:  Python subprocess fallback (Termux/ARM64)
#   Fix 3:  naab_gov_check blocked semantics (wasBlocked consistency)
#   Fix 5:  Python __del__ safety (_destroy_fn reference)
#   Fix 8:  Version alignment across components
#   Fix 9:  Framework examples sandbox_level = "standard"
#   Fix 12: REST error handler preserves route-set error messages
#
# Run: bash tests/api/test_platform_fixes.sh

PASS=0
FAIL=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB_GOV="$LANG_DIR/build/naab-gov"
NAAB_LANG="$LANG_DIR/build/naab-lang"
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"

check() {
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

check_output() {
    local desc="$1" pattern="$2"
    shift 2
    local output
    output=$("$@" 2>&1)
    if echo "$output" | grep -q "$pattern"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (pattern '$pattern' not found)"
        echo "        Got: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

check_not_output() {
    local desc="$1" pattern="$2"
    shift 2
    local output
    output=$("$@" 2>&1)
    if echo "$output" | grep -q "$pattern"; then
        echo "  FAIL: $desc (pattern '$pattern' was found but shouldn't be)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    fi
}

echo "=== Platform Fixes Regression Tests ==="
echo ""

# ─── Fix 1: Python subprocess fallback ───────────────────────────────

echo "--- Fix 1: Python subprocess fallback ---"

if [ -x "$NAAB_GOV" ] && command -v python3 >/dev/null 2>&1; then
    # T1.1: GovernanceEngine initializes (subprocess or ctypes)
    check "GovernanceEngine imports and initializes" \
        python3 -c "
import sys; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine
e = GovernanceEngine()
assert e.version, 'version should not be empty'
"

    # T1.2: Subprocess mode scans safe code correctly
    check "subprocess scan: safe code not blocked" \
        python3 -c "
import sys; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine
e = GovernanceEngine()
e.load_config_dict({
    'version': '3.0', 'mode': 'enforce',
    'restrictions': {'dangerous_calls': {'level': 'hard'}}
})
r = e.scan('python', 'x = 42\nprint(x)\n')
assert not r.get('blocked'), f'safe code should not be blocked: {r}'
"

    # T1.3: Subprocess mode scans dangerous code correctly
    check "subprocess scan: dangerous code blocked" \
        python3 -c "
import sys; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine
e = GovernanceEngine()
e.load_config_dict({
    'version': '3.0', 'mode': 'enforce',
    'restrictions': {'dangerous_calls': {'level': 'hard'}}
})
r = e.scan('python', 'import os\nos.system(\"rm -rf /\")\n')
assert r.get('blocked'), f'dangerous code should be blocked: {r}'
"

    # T1.4: GovernanceViolation raised on blocked code
    check "scan_or_raise raises GovernanceViolation" \
        python3 -c "
import sys; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine, GovernanceViolation
e = GovernanceEngine()
e.load_config_dict({
    'version': '3.0', 'mode': 'enforce',
    'restrictions': {'code_injection': {'level': 'hard'}}
})
try:
    e.scan_or_raise('python', 'eval(input())')
    assert False, 'should have raised'
except GovernanceViolation:
    pass
"

    # T1.5: load_config_dict sets is_active
    check "load_config_dict activates engine" \
        python3 -c "
import sys; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine
e = GovernanceEngine()
assert not e.is_active
e.load_config_dict({'version': '3.0', 'mode': 'enforce', 'restrictions': {}})
assert e.is_active
"
else
    echo "  SKIP: naab-gov or python3 not available"
fi

echo ""

# ─── Fix 3: naab_gov_check blocked semantics ─────────────────────────

echo "--- Fix 3: blocked semantics (wasBlocked consistency) ---"

if [ -x "$NAAB_GOV" ]; then
    FIX3_CFG="$TMPDIR/fix3_test_config.json"
    cat > "$FIX3_CFG" <<'CFGEOF'
{
    "version": "3.0", "mode": "enforce",
    "restrictions": {"dangerous_calls": {"level": "hard"}}
}
CFGEOF

    # T3.1: HARD violation → blocked:true via scan
    check_output "scan: HARD violation is blocked" '"blocked": true' \
        sh -c "echo 'import os; os.system(\"rm -rf /\")' | $NAAB_GOV check --language python --config $FIX3_CFG"

    # T3.2: Safe code → blocked:false via scan
    check_output "scan: safe code is not blocked" '"blocked": false' \
        sh -c "echo 'x = 42' | $NAAB_GOV check --language python --config $FIX3_CFG"

    rm -f "$FIX3_CFG"
else
    echo "  SKIP: naab-gov not built"
fi

echo ""

# ─── Fix 5: Python __del__ safety ────────────────────────────────────

echo "--- Fix 5: Python __del__ safety ---"

if command -v python3 >/dev/null 2>&1; then
    # T5.1: _destroy_fn is set (ctypes mode) or None (subprocess mode)
    check "GovernanceEngine has _destroy_fn attribute" \
        python3 -c "
import sys; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine
e = GovernanceEngine()
assert hasattr(e, '_destroy_fn'), 'missing _destroy_fn attribute'
# In subprocess mode, _destroy_fn should be None
# In ctypes mode, _destroy_fn should be a callable
if e._subprocess_mode:
    assert e._destroy_fn is None
else:
    assert callable(e._destroy_fn)
"

    # T5.2: __del__ does not reference self._lib directly
    check "__del__ uses _destroy_fn not _lib.naab_gov_destroy" \
        python3 -c "
import sys, inspect; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine
src = inspect.getsource(GovernanceEngine.__del__)
assert '_destroy_fn' in src, '__del__ should use _destroy_fn'
assert '_lib.naab_gov_destroy' not in src, '__del__ should NOT use _lib.naab_gov_destroy'
"

    # T5.3: Multiple create/destroy cycles don't crash
    check "rapid create/destroy cycles safe" \
        python3 -c "
import sys; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine
for i in range(20):
    e = GovernanceEngine()
    del e
"
else
    echo "  SKIP: python3 not available"
fi

echo ""

# ─── Fix 8: Version alignment ────────────────────────────────────────

echo "--- Fix 8: Version alignment ---"

if [ -x "$NAAB_GOV" ]; then
    GOV_CLI_VER=$($NAAB_GOV --version 2>&1 | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+')

    # T8.1: CLI version matches C API version
    if command -v python3 >/dev/null 2>&1; then
        check_output "CLI and Python binding versions match" "$GOV_CLI_VER" \
            python3 -c "
import sys; sys.path.insert(0, '$LANG_DIR')
from bindings.python.naab_governance import GovernanceEngine
print(GovernanceEngine().version)
"
    fi

    # T8.2: CLI version matches governance_c_api.cpp constant
    C_API_VER=$(grep 'NAAB_GOV_VERSION' "$LANG_DIR/src/api/governance_c_api.cpp" | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+')
    if [ "$GOV_CLI_VER" = "$C_API_VER" ]; then
        echo "  PASS: CLI version ($GOV_CLI_VER) matches C API version ($C_API_VER)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: CLI version ($GOV_CLI_VER) != C API version ($C_API_VER)"
        FAIL=$((FAIL + 1))
    fi

    # T8.3: pyproject.toml version matches
    PYPROJECT_VER=$(grep 'version' "$LANG_DIR/pyproject.toml" | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+')
    if [ "$GOV_CLI_VER" = "$PYPROJECT_VER" ]; then
        echo "  PASS: CLI version ($GOV_CLI_VER) matches pyproject.toml ($PYPROJECT_VER)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: CLI version ($GOV_CLI_VER) != pyproject.toml ($PYPROJECT_VER)"
        FAIL=$((FAIL + 1))
    fi

    # T8.4: Cargo.toml version matches
    CARGO_VER=$(grep 'version' "$LANG_DIR/bindings/rust/Cargo.toml" | head -1 | grep -o '[0-9]\+\.[0-9]\+\.[0-9]\+')
    if [ "$GOV_CLI_VER" = "$CARGO_VER" ]; then
        echo "  PASS: CLI version ($GOV_CLI_VER) matches Cargo.toml ($CARGO_VER)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: CLI version ($GOV_CLI_VER) != Cargo.toml ($CARGO_VER)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  SKIP: naab-gov not built"
fi

echo ""

# ─── Fix 9: Framework examples sandbox_level ─────────────────────────

echo "--- Fix 9: Framework examples sandbox_level ---"

for example in langchain_middleware.py crewai_guard.py autogen_validator.py; do
    file="$LANG_DIR/examples/python/$example"
    if [ -f "$file" ]; then
        check_not_output "$example does not use 'unrestricted'" '"unrestricted"' \
            grep "sandbox_level" "$file"
        check_output "$example uses 'standard'" '"standard"' \
            grep "sandbox_level" "$file"
    else
        echo "  SKIP: $file not found"
    fi
done

echo ""

# ─── Fix 12: REST error handler preserves route errors ────────────────

echo "--- Fix 12: REST error handler preserves route errors ---"

if [ -x "$NAAB_LANG" ] && command -v curl >/dev/null 2>&1; then
    PORT=18930
    "$NAAB_LANG" api "$PORT" --api-key "test-key-123" >"$TMPDIR/naab_fix12.log" 2>&1 &
    API_PID=$!
    cleanup_api() { kill "$API_PID" 2>/dev/null; wait "$API_PID" 2>/dev/null; rm -f "$TMPDIR/naab_fix12.log"; }
    trap cleanup_api EXIT

    # Wait for server
    for i in $(seq 1 50); do
        curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break
        sleep 0.1
    done

    if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
        # T12.1: 400 from missing fields preserves actual error message
        check_output "missing fields error preserved (not 'Endpoint not found')" \
            "Missing required fields" \
            curl -s -X POST "http://127.0.0.1:$PORT/api/v1/check" \
                -H "Authorization: Bearer test-key-123" \
                -H "Content-Type: application/json" \
                -d '{"language": "python"}'

        # T12.2: 400 from no config preserves actual error message
        check_output "no config error preserved (not 'Endpoint not found')" \
            "No governance config" \
            curl -s -X POST "http://127.0.0.1:$PORT/api/v1/check" \
                -H "Authorization: Bearer test-key-123" \
                -H "Content-Type: application/json" \
                -d '{"code": "x = 1", "language": "python"}'

        # T12.3: True 404 still returns "Endpoint not found"
        check_output "true 404 returns Endpoint not found" \
            "Endpoint not found" \
            curl -s -X GET "http://127.0.0.1:$PORT/api/v1/nonexistent" \
                -H "Authorization: Bearer test-key-123"

        # T12.4: Valid request with config returns 200
        check_output "valid request returns blocked field" \
            '"blocked"' \
            curl -s -X POST "http://127.0.0.1:$PORT/api/v1/check" \
                -H "Authorization: Bearer test-key-123" \
                -H "Content-Type: application/json" \
                -d '{"code": "x = 1", "language": "python", "config": {"version":"3.0","mode":"enforce","restrictions":{}}}'
    else
        echo "  SKIP: API server failed to start"
    fi

    cleanup_api
    trap - EXIT
else
    echo "  SKIP: naab-lang or curl not available"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] || exit 1
