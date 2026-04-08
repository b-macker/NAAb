#!/usr/bin/env bash
# Test script for R19 security fixes
# V-GOV-012 (Container Taint Loss), V-CONC-003 (REST API stdout race),
# V-CONC-004 (LSP diagnostic data race), V-RT-012 (Fragile block extraction),
# V-DOS-001 (BOLO unbounded input)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
NAAB="$BUILD_DIR/naab-lang"
WORK_DIR="$BUILD_DIR/_test_r19_$$"

pass=0
fail=0

check() {
    local name="$1"
    local result="$2"
    local desc="$3"
    if [ "$result" -eq 0 ]; then
        echo "  PASS [$name] $desc"
        pass=$((pass + 1))
    else
        echo "  FAIL [$name] $desc"
        fail=$((fail + 1))
    fi
}

mkdir -p "$WORK_DIR"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "=== R19 Security Fix Tests ==="
echo ""

# ---------------------------------------------------------------------------
# T-GOV12-1: Source check — interpreter.cpp contains MUTATION_METHODS
# ---------------------------------------------------------------------------
grep -q "MUTATION_METHODS" "$REPO_ROOT/src/interpreter/interpreter.cpp"
check "T-GOV12-1" $? "V-GOV-012: MUTATION_METHODS present in interpreter.cpp"

# ---------------------------------------------------------------------------
# T-GOV12-2: Runtime — a.push(tainted) marks 'a' as tainted
# ---------------------------------------------------------------------------
if [ -f "$NAAB" ]; then
    cat > "$WORK_DIR/test_gov012.naab" <<'NAAB_EOF'
main {
    let s = env.get_var("NAAB_TEST_SECRET_R19_GOV012")
    let a = []
    a.push(s)
    let x = a[0]
    io.write(x)
}
NAAB_EOF
    # Use a govern.json that enforces taint tracking with HARD level
    cat > "$WORK_DIR/govern.json" <<'GOVEOF'
{
  "mode": "block",
  "taint_tracking": {
    "enabled": true,
    "level": "hard",
    "sources": ["env.get_var", "env.get", "io.read_line"],
    "sinks": ["io.write", "io.write_file", "net.request", "shell_exec"]
  }
}
GOVEOF
    rc=0
    NAAB_TEST_SECRET_R19_GOV012="mysecret" \
        "$NAAB" --governance-config "$WORK_DIR/govern.json" \
        "$WORK_DIR/test_gov012.naab" > /dev/null 2>&1 || rc=$?
    # Expect non-zero exit — governance should block tainted sink
    if [ "$rc" -ne 0 ]; then
        check "T-GOV12-2" 0 "V-GOV-012: a.push(tainted) taints container; io.write(a[0]) blocked"
    else
        check "T-GOV12-2" 1 "V-GOV-012: expected governance block but exit code was 0"
    fi
else
    echo "  SKIP [T-GOV12-2] naab-lang binary not found at $NAAB"
fi

# ---------------------------------------------------------------------------
# T-CONC3-1: Source check — rest_api.cpp contains stdout_capture_mutex_
# ---------------------------------------------------------------------------
grep -q "stdout_capture_mutex_" "$REPO_ROOT/src/api/rest_api.cpp"
check "T-CONC3-1" $? "V-CONC-003: stdout_capture_mutex_ present in rest_api.cpp"

# ---------------------------------------------------------------------------
# T-CONC4-1: Source check — document_manager.h contains diag_mutex_
# ---------------------------------------------------------------------------
grep -q "diag_mutex_" "$REPO_ROOT/tools/naab-lsp/document_manager.h"
check "T-CONC4-1" $? "V-CONC-004: diag_mutex_ present in document_manager.h"

# ---------------------------------------------------------------------------
# T-CONC4-2: Source check — getDiagnostics() returns vector by value (not reference)
# ---------------------------------------------------------------------------
grep -q "std::vector<Diagnostic> getDiagnostics" "$REPO_ROOT/tools/naab-lsp/document_manager.h"
check "T-CONC4-2" $? "V-CONC-004: getDiagnostics() returns by value in document_manager.h"

# ---------------------------------------------------------------------------
# T-RT12-1: Source check — block_loader.cpp uses nlohmann::json::parse
# ---------------------------------------------------------------------------
grep -q "nlohmann::json::parse" "$REPO_ROOT/src/runtime/block_loader.cpp"
check "T-RT12-1" $? "V-RT-012: nlohmann::json::parse present in block_loader.cpp"

# ---------------------------------------------------------------------------
# T-DOS1-1: Source check — bolo_impl.cpp contains MAX_BOLO_SCAN_BYTES
# ---------------------------------------------------------------------------
grep -q "MAX_BOLO_SCAN_BYTES" "$REPO_ROOT/src/stdlib/bolo_impl.cpp"
check "T-DOS1-1" $? "V-DOS-001: MAX_BOLO_SCAN_BYTES present in bolo_impl.cpp"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
total=$((pass + fail))
echo "=== Results: $pass/$total passed ==="
[ "$fail" -eq 0 ]
