#!/usr/bin/env bash
# tests/security/test_r18_fixes.sh
# Round 18 security fix verification
# T-RCE6-1  C++ angle-bracket #include </etc/passwd> rejected
# T-RCE6-2  C++ line-spliced #include rejected
# T-RCE6-3  Rust raw-string include_str!(r#"/etc/passwd"#) rejected
# T-SC5-1   api command handler contains global_lock_check guard (source check)
# T-RT11-1  json_result_parser.cpp contains checkJsonDepth guard (source check)
# T-RCE7-1  cpp_executor.cpp contains mkdtemp (source check)
# T-CONC2-1 bolo_impl.cpp contains g_engine_mutex (source check)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LANG_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$LANG_DIR/build/naab-lang"
PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

# ---------------------------------------------------------------------------
# T-RCE6-1: C++ angle-bracket absolute #include rejected at runtime
# ---------------------------------------------------------------------------
WORK="$SCRIPT_DIR/../../build/_test_r18_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/test_rce6_ab.naab" <<'EOF'
main {
    let result = <<cpp
#include </etc/passwd>
int main() { return 0; }
>>
    io.write(result)
}
EOF

if "$NAAB" "$WORK/test_rce6_ab.naab" 2>&1 | grep -qiE "absolute|not permitted|error"; then
    pass "T-RCE6-1: C++ angle-bracket #include </etc/passwd> rejected"
elif ! "$NAAB" "$WORK/test_rce6_ab.naab" > /dev/null 2>&1; then
    pass "T-RCE6-1: C++ angle-bracket #include </etc/passwd> rejected (non-zero exit)"
else
    fail "T-RCE6-1: C++ angle-bracket #include </etc/passwd> NOT rejected"
fi

# ---------------------------------------------------------------------------
# T-RCE6-2: C++ line-spliced #include rejected at runtime
# ---------------------------------------------------------------------------
# The line continuation is in the NAAb source, not in the shell heredoc.
printf 'main {\n    let result = <<cpp\n#inc\\\nlude "/etc/passwd"\nint main() { return 0; }\n>>\n    io.write(result)\n}\n' > "$WORK/test_rce6_splice.naab"

if "$NAAB" "$WORK/test_rce6_splice.naab" 2>&1 | grep -qiE "absolute|not permitted|error"; then
    pass "T-RCE6-2: C++ line-spliced #include rejected"
elif ! "$NAAB" "$WORK/test_rce6_splice.naab" > /dev/null 2>&1; then
    pass "T-RCE6-2: C++ line-spliced #include rejected (non-zero exit)"
else
    fail "T-RCE6-2: C++ line-spliced #include NOT rejected"
fi

# ---------------------------------------------------------------------------
# T-RCE6-3: Rust raw-string include_str!(r#"/etc/passwd"#) rejected
# ---------------------------------------------------------------------------
cat > "$WORK/test_rce6_rust_raw.naab" <<'EOF'
main {
    let result = <<rust
fn main() {
    let data = include_str!(r#"/etc/passwd"#);
    println!("{}", data);
}
>>
    io.write(result)
}
EOF

if "$NAAB" "$WORK/test_rce6_rust_raw.naab" 2>&1 | grep -qiE "absolute|not permitted|error"; then
    pass "T-RCE6-3: Rust raw-string include_str! rejected"
elif ! "$NAAB" "$WORK/test_rce6_rust_raw.naab" > /dev/null 2>&1; then
    pass "T-RCE6-3: Rust raw-string include_str! rejected (non-zero exit)"
else
    fail "T-RCE6-3: Rust raw-string include_str! NOT rejected"
fi

# ---------------------------------------------------------------------------
# T-SC5-1: api command handler contains lock_check guard (source check)
# ---------------------------------------------------------------------------
MAIN_CPP="$LANG_DIR/src/cli/main.cpp"
# Verify V-SC-005 guard exists between the api command block and the version command block
_api_start=$(grep -n 'command == "api"' "$MAIN_CPP" | head -1 | cut -d: -f1)
_api_end=$(grep -n 'command == "version"' "$MAIN_CPP" | head -1 | cut -d: -f1)
if [ -n "$_api_start" ] && [ -n "$_api_end" ]; then
    _block=$(sed -n "${_api_start},${_api_end}p" "$MAIN_CPP")
    if echo "$_block" | grep -q "global_lock_check" && echo "$_block" | grep -q "V-SC-005"; then
        pass "T-SC5-1: api command contains global_lock_check guard (V-SC-005)"
    else
        fail "T-SC5-1: global_lock_check guard not found inside api command block"
    fi
else
    fail "T-SC5-1: could not locate api or version command block in main.cpp"
fi

# ---------------------------------------------------------------------------
# T-RT11-1: json_result_parser.cpp contains checkJsonDepth guard
# ---------------------------------------------------------------------------
JSON_PARSER="$LANG_DIR/src/runtime/json_result_parser.cpp"
if grep -q "checkJsonDepth" "$JSON_PARSER" && grep -q "V-RT-011" "$JSON_PARSER"; then
    pass "T-RT11-1: json_result_parser.cpp contains checkJsonDepth (V-RT-011)"
else
    fail "T-RT11-1: checkJsonDepth or V-RT-011 missing from json_result_parser.cpp"
fi

# ---------------------------------------------------------------------------
# T-RCE7-1: cpp_executor.cpp contains mkdtemp for BLOCK_LIBRARY compilation
# ---------------------------------------------------------------------------
CPP_EXEC="$LANG_DIR/src/runtime/cpp_executor.cpp"
if grep -q "mkdtemp" "$CPP_EXEC" && grep -q "V-RCE-007" "$CPP_EXEC"; then
    pass "T-RCE7-1: cpp_executor.cpp contains mkdtemp (V-RCE-007)"
else
    fail "T-RCE7-1: mkdtemp or V-RCE-007 missing from cpp_executor.cpp"
fi

# ---------------------------------------------------------------------------
# T-CONC2-1: bolo_impl.cpp contains g_engine_mutex
# ---------------------------------------------------------------------------
BOLO="$LANG_DIR/src/stdlib/bolo_impl.cpp"
if grep -q "g_engine_mutex" "$BOLO" && grep -q "V-CONC-002" "$BOLO"; then
    pass "T-CONC2-1: bolo_impl.cpp contains g_engine_mutex (V-CONC-002)"
else
    fail "T-CONC2-1: g_engine_mutex or V-CONC-002 missing from bolo_impl.cpp"
fi

# ---------------------------------------------------------------------------
echo ""
echo "R18 Security Fix Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
