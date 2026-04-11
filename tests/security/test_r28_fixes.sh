#!/usr/bin/env bash
# Security R28 Fix Verification Tests
# V-RCE-012: Nim parenthesis-less scanner evasion
# V-RCE-013: C++ macro #include evasion
# V-RCE-014: Rust concat! macro evasion
# V-ASYNC-006: catch(...) in executeParallel
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
WORK_DIR="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_r28.XXXXXX")"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

# Create govern.json so governance doesn't block execution
cat > "$WORK_DIR/govern.json" << 'JSON'
{ "version": "4.0", "mode": "off" }
JSON

# ── V-RCE-012: Nim scanner rejects parenthesis-less invocation ─────────────

echo "=== V-RCE-012: Nim parenthesis-less scanner evasion ==="

# Test 1: slurp without parens must be rejected
cat > "$WORK_DIR/nim_slurp_noparen.naab" << 'NAAB'
main {
    let result = <<nim
const secret = slurp "/etc/passwd"
echo secret
>>
    print(result)
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/nim_slurp_noparen.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "not permitted\|slurp\|blocked\|rejected\|unsafe\|ERROR.*Nim"; then
    pass "V-RCE-012 T1: slurp without parens rejected"
else
    fail "V-RCE-012 T1: slurp without parens NOT rejected" "$OUTPUT"
fi

# Test 2: staticExec without parens must be rejected
cat > "$WORK_DIR/nim_staticexec_noparen.naab" << 'NAAB'
main {
    let result = <<nim
const out = staticExec "id"
echo out
>>
    print(result)
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/nim_staticexec_noparen.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "not permitted\|staticExec\|blocked\|rejected\|unsafe"; then
    pass "V-RCE-012 T2: staticExec without parens rejected"
else
    fail "V-RCE-012 T2: staticExec without parens NOT rejected" "$OUTPUT"
fi

# Test 3: gorge without parens must be rejected
cat > "$WORK_DIR/nim_gorge_noparen.naab" << 'NAAB'
main {
    let result = <<nim
const out = gorge "cat /etc/shadow"
>>
    print(result)
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/nim_gorge_noparen.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "not permitted\|gorge\|blocked\|rejected\|unsafe"; then
    pass "V-RCE-012 T3: gorge without parens rejected"
else
    fail "V-RCE-012 T3: gorge without parens NOT rejected" "$OUTPUT"
fi

# ── V-RCE-013: C++ macro #include evasion ──────────────────────────────────

echo "=== V-RCE-013: C++ macro #include evasion ==="

# Test 4: #include MACRO_NAME must be rejected
cat > "$WORK_DIR/cpp_macro_include.naab" << 'NAAB'
main {
    let result = <<cpp
#define EVIL_PATH </etc/passwd>
#include EVIL_PATH
int main() { return 0; }
>>
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/cpp_macro_include.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "not permitted\|macro\|blocked\|rejected\|unsafe"; then
    pass "V-RCE-013 T4: #include MACRO rejected"
else
    fail "V-RCE-013 T4: #include MACRO NOT rejected" "$OUTPUT"
fi

# Test 5: #define with absolute path value must be rejected
cat > "$WORK_DIR/cpp_define_path.naab" << 'NAAB'
main {
    let result = <<cpp
#define MY_HEADER "/etc/shadow"
#include MY_HEADER
int main() { return 0; }
>>
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/cpp_define_path.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "not permitted\|macro\|absolute\|blocked\|rejected\|unsafe"; then
    pass "V-RCE-013 T5: #define with absolute path rejected"
else
    fail "V-RCE-013 T5: #define with absolute path NOT rejected" "$OUTPUT"
fi

# ── V-RCE-014: Rust concat! macro evasion ──────────────────────────────────

echo "=== V-RCE-014: Rust concat! macro evasion ==="

# Test 6: include_str!(concat!(...)) must be rejected
cat > "$WORK_DIR/rust_concat_include.naab" << 'NAAB'
main {
    let result = <<rust
fn main() {
    let s = include_str!(concat!("/", "etc", "/", "passwd"));
    println!("{}", s);
}
>>
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/rust_concat_include.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "not permitted\|macro\|blocked\|rejected\|unsafe"; then
    pass "V-RCE-014 T6: include_str!(concat!()) rejected"
else
    fail "V-RCE-014 T6: include_str!(concat!()) NOT rejected" "$OUTPUT"
fi

# Test 7: include_bytes!(env!(...)) must be rejected
cat > "$WORK_DIR/rust_env_include.naab" << 'NAAB'
main {
    let result = <<rust
fn main() {
    let b = include_bytes!(env!("HOME"));
    println!("{:?}", b);
}
>>
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/rust_env_include.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "not permitted\|macro\|blocked\|rejected\|unsafe"; then
    pass "V-RCE-014 T7: include_bytes!(env!()) rejected"
else
    fail "V-RCE-014 T7: include_bytes!(env!()) NOT rejected" "$OUTPUT"
fi

# Test 8: Legitimate include_str!("/relative.txt") still works (shouldn't be blocked by new check)
# Note: it will fail at compile time but should NOT be blocked by the scanner
# since it uses a direct string literal (not a macro)
cat > "$WORK_DIR/rust_legit_include.naab" << 'NAAB'
main {
    let result = <<rust
fn main() {
    let s = include_str!("relative.txt");
    println!("{}", s);
}
>>
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/rust_legit_include.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "macro composition not permitted"; then
    fail "V-RCE-014 T8: legitimate include_str!(\"...\") wrongly blocked by macro check"
else
    pass "V-RCE-014 T8: legitimate include_str!(\"...\") not blocked by macro check"
fi

# ── V-ASYNC-006: catch(...) in executeParallel ─────────────────────────────

echo "=== V-ASYNC-006: Source verification ==="

# Test 9: Verify the catch block uses catch(...) not catch(const std::runtime_error&)
ASYNC_SRC="$SCRIPT_DIR/../../src/runtime/polyglot_async_executor.cpp"
if grep -q 'catch (\.\.\.)' "$ASYNC_SRC"; then
    pass "V-ASYNC-006 T9: executeParallel uses catch(...)"
else
    fail "V-ASYNC-006 T9: executeParallel does NOT use catch(...)"
fi

# ── Summary ────────────────────────────────────────────────────────────────

echo ""
echo "================================"
echo "R28 Results: $PASS passed, $FAIL failed (of $((PASS+FAIL)) tests)"
echo "================================"

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
