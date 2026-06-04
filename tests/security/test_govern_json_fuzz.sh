#!/usr/bin/env bash
# test_govern_json_fuzz.sh — Fuzz govern.json parser with malformed configs
#
# Tests that the governance config parser handles malformed input gracefully
# (error message, non-zero exit) instead of crashing (segfault, abort, unhandled exception).
#
# Every test case creates a temporary govern.json, runs a minimal .naab program,
# and checks that the process did NOT crash (exit code != 139/134/136).

PASS=0
FAIL=0
CRASH=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$LANG_DIR/build/naab-lang"
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
FUZZ_DIR="$TMPDIR/naab_fuzz_$$"

mkdir -p "$FUZZ_DIR"

# Minimal program to trigger config loading
cat > "$FUZZ_DIR/minimal.naab" << 'NAAB'
main {
    print("ok")
}
NAAB

TOTAL=0

run_fuzz() {
    local name="$1"
    local json="$2"
    TOTAL=$((TOTAL + 1))

    echo "$json" > "$FUZZ_DIR/govern.json"

    # Run with timeout to prevent hangs from huge allocations
    output=$(cd "$FUZZ_DIR" && timeout 10 "$NAAB" minimal.naab 2>&1)
    local rc=$?

    # 124 = timeout killed it (resource exhaustion — counts as crash)
    # 134 = SIGABRT, 136 = SIGFPE, 139 = SIGSEGV
    if [ $rc -eq 139 ] || [ $rc -eq 134 ] || [ $rc -eq 136 ] || [ $rc -eq 124 ]; then
        echo "  CRASH: F$TOTAL $name (exit $rc)"
        CRASH=$((CRASH + 1))
        FAIL=$((FAIL + 1))
    elif [ $rc -eq 0 ] || [ $rc -eq 1 ] || [ $rc -eq 4 ]; then
        # 0 = ran fine (config ignored/defaulted), 1 = runtime error, 4 = config error
        # All are acceptable — parser handled the bad input
        echo "  PASS:  F$TOTAL $name (exit $rc)"
        PASS=$((PASS + 1))
    elif [ $rc -eq 2 ] || [ $rc -eq 3 ]; then
        # 2 = quality gate, 3 = governance block — also acceptable
        echo "  PASS:  F$TOTAL $name (exit $rc)"
        PASS=$((PASS + 1))
    else
        # Unknown exit code — investigate but don't call it a crash
        echo "  WARN:  F$TOTAL $name (exit $rc)"
        PASS=$((PASS + 1))
    fi
}

echo "=== Governance Config Fuzz Test ==="
echo ""
echo "--- Category 1: Type Mismatches (primitives) ---"

run_fuzz "mode as int" \
    '{"mode": 123}'

run_fuzz "mode as bool" \
    '{"mode": true}'

run_fuzz "mode as null" \
    '{"mode": null}'

run_fuzz "mode as array" \
    '{"mode": ["enforce"]}'

run_fuzz "mode as object" \
    '{"mode": {"value": "enforce"}}'

run_fuzz "mode unknown string" \
    '{"mode": "turbo_enforce"}'

run_fuzz "timeout as string" \
    '{"limits": {"timeout": "sixty"}}'

run_fuzz "timeout as bool" \
    '{"limits": {"timeout": true}}'

run_fuzz "timeout as null" \
    '{"limits": {"timeout": null}}'

run_fuzz "timeout as array" \
    '{"limits": {"timeout": [30]}}'

echo ""
echo "--- Category 2: Integer Overflow/Underflow ---"

run_fuzz "timeout INT_MAX" \
    '{"limits": {"timeout": 2147483647}}'

run_fuzz "timeout INT_MAX+1" \
    '{"limits": {"timeout": 2147483648}}'

run_fuzz "timeout INT_MIN" \
    '{"limits": {"timeout": -2147483648}}'

run_fuzz "timeout huge positive" \
    '{"limits": {"timeout": 9999999999999}}'

run_fuzz "timeout huge negative" \
    '{"limits": {"timeout": -9999999999999}}'

run_fuzz "loop_iterations INT_MAX" \
    '{"mode":"enforce","limits":{"execution":{"loop_iterations": 2147483647}}}'

run_fuzz "call_depth zero" \
    '{"limits": {"execution": {"call_depth": 0}}}'

run_fuzz "array_size negative" \
    '{"limits": {"data": {"array_size": -1}}}'

run_fuzz "port out of range" \
    '{"capabilities":{"network":{"enabled":true,"allowed_ports":[99999,-1,0]}}}'

echo ""
echo "--- Category 3: Floating-Point Edge Cases ---"

run_fuzz "coherence_threshold NaN string" \
    '{"mode":"enforce","context_drift":{"enabled":true,"coherence_threshold":"NaN"}}'

run_fuzz "coherence_threshold very large" \
    '{"mode":"enforce","context_drift":{"enabled":true,"coherence_threshold":1e308}}'

run_fuzz "coherence_threshold very negative" \
    '{"mode":"enforce","context_drift":{"enabled":true,"coherence_threshold":-1e308}}'

run_fuzz "coherence_threshold zero" \
    '{"mode":"enforce","context_drift":{"enabled":true,"coherence_threshold":0.0}}'

run_fuzz "tolerance negative" \
    '{"verification":{"tolerance":-0.5}}'

run_fuzz "tolerance huge" \
    '{"verification":{"tolerance":999999.0}}'

run_fuzz "entropy threshold negative" \
    '{"entropy_check":{"enabled":true,"threshold":-1.0}}'

run_fuzz "weight values negative" \
    '{"mode":"enforce","context_drift":{"enabled":true,"weights":{"circular":-100.0,"scope_creep":-100.0}}}'

echo ""
echo "--- Category 4: Null Values Where Types Expected ---"

run_fuzz "all top-level null" \
    '{"mode":null,"limits":null,"capabilities":null}'

run_fuzz "limits.execution null" \
    '{"limits":{"execution":null}}'

run_fuzz "limits.data null" \
    '{"limits":{"data":null}}'

run_fuzz "capabilities.network null" \
    '{"capabilities":{"network":null}}'

run_fuzz "capabilities.shell null" \
    '{"capabilities":{"shell":null}}'

run_fuzz "capabilities.filesystem null" \
    '{"capabilities":{"filesystem":null}}'

run_fuzz "scoring null" \
    '{"scoring":null}'

run_fuzz "context_drift null" \
    '{"context_drift":null}'

echo ""
echo "--- Category 5: Empty Objects/Arrays ---"

run_fuzz "empty config" \
    '{}'

run_fuzz "empty limits" \
    '{"limits":{}}'

run_fuzz "empty capabilities" \
    '{"capabilities":{}}'

run_fuzz "empty languages" \
    '{"languages":{}}'

run_fuzz "empty taint_tracking" \
    '{"taint_tracking":{}}'

run_fuzz "empty drift_detection" \
    '{"drift_detection":{}}'

run_fuzz "empty context_drift" \
    '{"context_drift":{}}'

run_fuzz "empty behavioral_sequences" \
    '{"behavioral_sequences":{}}'

echo ""
echo "--- Category 6: Array Element Type Mismatches ---"

run_fuzz "allowed_ports with strings" \
    '{"capabilities":{"network":{"enabled":true,"allowed_ports":["eighty","443"]}}}'

run_fuzz "banned_functions with ints" \
    '{"languages":{"per_language":{"python":{"banned_functions":[1,2,3]}}}}'

run_fuzz "allowed languages with ints" \
    '{"languages":{"allowed":[1,2,3]}}'

run_fuzz "blocked languages with null" \
    '{"languages":{"blocked":[null,null]}}'

run_fuzz "taint sources with bools" \
    '{"taint_tracking":{"sources":[true,false]}}'

run_fuzz "taint sinks with objects" \
    '{"taint_tracking":{"sinks":[{"name":"file.write"}]}}'

echo ""
echo "--- Category 7: Enforcement Level Edge Cases ---"

run_fuzz "level as int" \
    '{"requirements":{"main_block":{"enabled":true,"level":999}}}'

run_fuzz "level as null" \
    '{"requirements":{"main_block":{"enabled":true,"level":null}}}'

run_fuzz "level unknown string" \
    '{"requirements":{"main_block":{"enabled":true,"level":"ultra_hard"}}}'

run_fuzz "level as array" \
    '{"requirements":{"main_block":{"enabled":true,"level":["hard"]}}}'

run_fuzz "level as empty object" \
    '{"requirements":{"main_block":{"enabled":true,"level":{}}}}'

echo ""
echo "--- Category 8: Nested Type Confusion ---"

run_fuzz "capabilities.network as string" \
    '{"capabilities":{"network":"yes"}}'

run_fuzz "capabilities.shell as int" \
    '{"capabilities":{"shell":42}}'

run_fuzz "capabilities.filesystem as array" \
    '{"capabilities":{"filesystem":["read","write"]}}'

run_fuzz "limits as string" \
    '{"limits":"none"}'

run_fuzz "limits as array" \
    '{"limits":[100,200]}'

run_fuzz "scoring as string" \
    '{"scoring":"enabled"}'

run_fuzz "context_drift as bool" \
    '{"context_drift":true}'

echo ""
echo "--- Category 9: return_range Edge Cases ---"

run_fuzz "return_range empty array" \
    '{"contracts":{"functions":{"test_fn":{"return_range":[]}}}}'

run_fuzz "return_range single element" \
    '{"contracts":{"functions":{"test_fn":{"return_range":[1.0]}}}}'

run_fuzz "return_range strings" \
    '{"contracts":{"functions":{"test_fn":{"return_range":["low","high"]}}}}'

run_fuzz "return_range as scalar" \
    '{"contracts":{"functions":{"test_fn":{"return_range":5.0}}}}'

echo ""
echo "--- Category 10: Deeply Nested / Structural ---"

run_fuzz "100-key per_language" \
    "$(python3 -c "
import json
d = {'languages':{'per_language':{}}}
for i in range(100):
    d['languages']['per_language'][f'lang_{i}'] = {'timeout': i}
print(json.dumps(d))
" 2>/dev/null || echo '{"languages":{"per_language":{"a":{"timeout":1},"b":{"timeout":2}}}}')"

run_fuzz "100 banned_functions" \
    "$(python3 -c "
import json
fns = [f'fn_{i}' for i in range(100)]
print(json.dumps({'languages':{'per_language':{'python':{'banned_functions':fns}}}}))
" 2>/dev/null || echo '{"languages":{"per_language":{"python":{"banned_functions":["a","b"]}}}}')"

run_fuzz "duplicate keys (last wins)" \
    '{"mode":"off","mode":"enforce"}'

echo ""
echo "--- Category 11: Agent Config Edge Cases ---"

run_fuzz "agent risk_budget huge" \
    '{"mode":"enforce","agents":{"default":{"risk_budget":999999999}}}'

run_fuzz "agent risk_budget float" \
    '{"mode":"enforce","agents":{"default":{"risk_budget":3.14}}}'

run_fuzz "agent temperature negative" \
    '{"mode":"enforce","agents":{"default":{"temperature":-1.0}}}'

run_fuzz "agent temperature huge" \
    '{"mode":"enforce","agents":{"default":{"temperature":100.0}}}'

run_fuzz "agent max_autonomous_actions null" \
    '{"mode":"enforce","agents":{"default":{"max_autonomous_actions":null}}}'

run_fuzz "agent coherence_floor as string" \
    '{"mode":"enforce","agents":{"default":{"coherence_floor":"low"}}}'

echo ""
echo "--- Category 12: Invalid JSON ---"

# These test the JSON parser itself, not the config parser
TOTAL=$((TOTAL + 1))
echo '{"mode": "enforce"' > "$FUZZ_DIR/govern.json"  # missing closing brace
output=$(cd "$FUZZ_DIR" && timeout 10 "$NAAB" minimal.naab 2>&1); rc=$?
if [ $rc -eq 139 ] || [ $rc -eq 134 ] || [ $rc -eq 136 ]; then
    echo "  CRASH: F$TOTAL truncated JSON (exit $rc)"; CRASH=$((CRASH+1)); FAIL=$((FAIL+1))
else
    echo "  PASS:  F$TOTAL truncated JSON (exit $rc)"; PASS=$((PASS+1))
fi

TOTAL=$((TOTAL + 1))
echo 'not json at all' > "$FUZZ_DIR/govern.json"
output=$(cd "$FUZZ_DIR" && timeout 10 "$NAAB" minimal.naab 2>&1); rc=$?
if [ $rc -eq 139 ] || [ $rc -eq 134 ] || [ $rc -eq 136 ]; then
    echo "  CRASH: F$TOTAL not JSON (exit $rc)"; CRASH=$((CRASH+1)); FAIL=$((FAIL+1))
else
    echo "  PASS:  F$TOTAL not JSON (exit $rc)"; PASS=$((PASS+1))
fi

TOTAL=$((TOTAL + 1))
echo '' > "$FUZZ_DIR/govern.json"  # empty file
output=$(cd "$FUZZ_DIR" && timeout 10 "$NAAB" minimal.naab 2>&1); rc=$?
if [ $rc -eq 139 ] || [ $rc -eq 134 ] || [ $rc -eq 136 ]; then
    echo "  CRASH: F$TOTAL empty file (exit $rc)"; CRASH=$((CRASH+1)); FAIL=$((FAIL+1))
else
    echo "  PASS:  F$TOTAL empty file (exit $rc)"; PASS=$((PASS+1))
fi

TOTAL=$((TOTAL + 1))
echo '[]' > "$FUZZ_DIR/govern.json"  # array instead of object
output=$(cd "$FUZZ_DIR" && timeout 10 "$NAAB" minimal.naab 2>&1); rc=$?
if [ $rc -eq 139 ] || [ $rc -eq 134 ] || [ $rc -eq 136 ]; then
    echo "  CRASH: F$TOTAL top-level array (exit $rc)"; CRASH=$((CRASH+1)); FAIL=$((FAIL+1))
else
    echo "  PASS:  F$TOTAL top-level array (exit $rc)"; PASS=$((PASS+1))
fi

TOTAL=$((TOTAL + 1))
echo '"just a string"' > "$FUZZ_DIR/govern.json"
output=$(cd "$FUZZ_DIR" && timeout 10 "$NAAB" minimal.naab 2>&1); rc=$?
if [ $rc -eq 139 ] || [ $rc -eq 134 ] || [ $rc -eq 136 ]; then
    echo "  CRASH: F$TOTAL top-level string (exit $rc)"; CRASH=$((CRASH+1)); FAIL=$((FAIL+1))
else
    echo "  PASS:  F$TOTAL top-level string (exit $rc)"; PASS=$((PASS+1))
fi

TOTAL=$((TOTAL + 1))
echo 'null' > "$FUZZ_DIR/govern.json"
output=$(cd "$FUZZ_DIR" && timeout 10 "$NAAB" minimal.naab 2>&1); rc=$?
if [ $rc -eq 139 ] || [ $rc -eq 134 ] || [ $rc -eq 136 ]; then
    echo "  CRASH: F$TOTAL top-level null (exit $rc)"; CRASH=$((CRASH+1)); FAIL=$((FAIL+1))
else
    echo "  PASS:  F$TOTAL top-level null (exit $rc)"; PASS=$((PASS+1))
fi

# ── Tool config fuzz cases ──

fuzz_tool() {
    local desc="$1"
    local json="$2"
    TOTAL=$((TOTAL + 1))
    echo "$json" > "$FUZZ_DIR/govern.json"
    output=$(cd "$FUZZ_DIR" && timeout 10 "$NAAB" minimal.naab 2>&1); rc=$?
    if [ $rc -eq 139 ] || [ $rc -eq 134 ] || [ $rc -eq 136 ]; then
        echo "  CRASH: F$TOTAL $desc (exit $rc)"; CRASH=$((CRASH+1)); FAIL=$((FAIL+1))
    else
        echo "  PASS:  F$TOTAL $desc (exit $rc)"; PASS=$((PASS+1))
    fi
}

fuzz_tool "tools_enabled: string" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","tools_enabled":"yes"}}}'
fuzz_tool "tools_enabled: null" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","tools_enabled":null}}}'
fuzz_tool "max_tool_calls_per_turn: negative" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","max_tool_calls_per_turn":-5}}}'
fuzz_tool "max_tool_calls_per_turn: overflow" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","max_tool_calls_per_turn":999999}}}'
fuzz_tool "max_tool_loop_turns: 0" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","max_tool_loop_turns":0}}}'
fuzz_tool "tool_result_max_chars: negative" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","tool_result_max_chars":-1}}}'
fuzz_tool "tool_timeout_seconds: 0" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","tool_timeout_seconds":0}}}'
fuzz_tool "tools: string not array" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","tools":"not_an_array"}}}'
fuzz_tool "tools: non-string elements" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","tools":[123,null,true]}}}'
fuzz_tool "tools: oversized name" \
    '{"agents":{"a":{"provider":"gemini","model":"m","api_key_env":"K","tools":["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"]}}}'

# --- Category 14: Codegen config fuzz ---
fuzz_tool "codegen.enabled: string" \
    '{"codegen":{"enabled":"yes"}}'
fuzz_tool "codegen.enabled: null" \
    '{"codegen":{"enabled":null}}'
fuzz_tool "codegen.max_code_size_bytes: negative" \
    '{"codegen":{"enabled":true,"max_code_size_bytes":-5}}'
fuzz_tool "codegen.max_code_size_bytes: overflow" \
    '{"codegen":{"enabled":true,"max_code_size_bytes":999999999}}'
fuzz_tool "codegen.max_code_lines: 0" \
    '{"codegen":{"enabled":true,"max_code_lines":0}}'
fuzz_tool "codegen.timeout_seconds: negative" \
    '{"codegen":{"enabled":true,"timeout_seconds":-1}}'
fuzz_tool "codegen.allowed_languages: string" \
    '{"codegen":{"enabled":true,"allowed_languages":"python"}}'
fuzz_tool "codegen.allowed_languages: non-string elements" \
    '{"codegen":{"enabled":true,"allowed_languages":[123,null]}}'
fuzz_tool "codegen.max_nesting_depth: negative" \
    '{"codegen":{"enabled":true,"max_nesting_depth":-1}}'
fuzz_tool "codegen.max_cumulative_calls: 0" \
    '{"codegen":{"enabled":true,"max_cumulative_calls":0}}'

# Cleanup
rm -rf "$FUZZ_DIR"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ($CRASH crashes) out of $TOTAL ==="

if [ $CRASH -gt 0 ]; then
    echo ""
    echo "ERROR: $CRASH test(s) caused crashes — config parser does not handle these inputs safely."
    exit 1
elif [ $FAIL -gt 0 ]; then
    echo ""
    echo "WARNING: $FAIL test(s) failed but did not crash."
    exit 1
fi

echo "  All malformed configs handled gracefully."
exit 0
