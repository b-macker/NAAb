#!/usr/bin/env bash
# ============================================================
# test_code_aware_keywords.sh — Code-aware keyword extraction
#
# The shared extractor (include/naab/keyword_extract.h) splits camelCase /
# PascalCase / letter-digit boundaries into component words and filters
# code-syntax stop words, so code-only agents share a token space with
# English mandate keywords. Before this, CDD keyword-overlap signals
# (S10/S11/S13/S15) deterministically penalized code responses.
#
# Uses the local agent stub (tests/helpers/agent_stub.py) via per-agent
# api_base, so the REAL C++ extraction path is exercised without API keys.
#
# Group T1: on-mandate Python code over 6 turns -> no mandate drift, no
#           semantic instability, coherence stays high
# Group T2: off-mandate code -> mandate_alignment signal still fires
#           (code-awareness must not blind the signal)
# Group T3: identifiers ONLY reachable via camelCase splitting match the
#           mandate (proves splitting happens in the C++ path)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/codekw-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_CODEKW_TEST="fake-key-code-aware-keywords-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

start_stub() {  # $1=fixture $2=workdir
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do
        grep -q READY "$2/stub.log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# Shared govern.json template: keyword-overlap signals ON, unrelated
# signals OFF so coherence movement is attributable to S10/S11/S13/S15.
write_govern() {  # $1=workdir $2=system_prompt
    cat > "$1/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": {
        "enabled": true,
        "check_interval_turns": 1,
        "signals": {
            "response_quality": false,
            "thinking_collapse": false,
            "context_growth": false,
            "persona_fingerprint": false,
            "plan_drift": false,
            "instruction_conflict": false,
            "prompt_compliance": false,
            "response_repetition": false,
            "tool_chain_integrity": false,
            "claim_result_reconciliation": false,
            "circular_actions": false
        }
    },
    "agents": {
        "coder": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_CODEKW_TEST",
            "max_tokens": 2048,
            "max_turns": 30,
            "system_prompt": "$2"
        }
    }
}
GOVEOF
    sign_govern "$1"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Code-Aware Keyword Extraction Tests (CDD S10/S11/S13/S15)  |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group T1: on-mandate code does not degrade coherence
# ============================================================
echo -e "${CYAN}--- Group T1: on-mandate Python code, 6 turns ---${NC}"

WDIR="$TEST_TMP/t1"; mkdir -p "$WDIR"
# Six code responses that grow cumulatively (developer agents output the
# COMPLETE file each turn, like the living-script developer). Each differs
# from the last (no verbatim S21 repeats) but keyword overlap stays high,
# and identifiers use camelCase + snake_case forms of the mandate words.
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "class Calculator:\n    def __init__(self):\n        self._history = []\n    def add(self, a, b):\n        result = a + b\n        self._history.append(('add', a, b, result))\n        return result\n", "output_tokens": 60},
  {"content": "class Calculator:\n    def __init__(self):\n        self._history = []\n    def add(self, a, b):\n        result = a + b\n        self._history.append(('add', a, b, result))\n        return result\n    def subtract(self, a, b):\n        result = a - b\n        self._history.append(('subtract', a, b, result))\n        return result\n", "output_tokens": 90},
  {"content": "class Calculator:\n    def __init__(self):\n        self._history = []\n    def add(self, a, b):\n        result = a + b\n        self._history.append(('add', a, b, result))\n        return result\n    def subtract(self, a, b):\n        result = a - b\n        self._history.append(('subtract', a, b, result))\n        return result\n    def multiply(self, a, b):\n        result = a * b\n        self._history.append(('multiply', a, b, result))\n        return result\n", "output_tokens": 120},
  {"content": "class Calculator:\n    def __init__(self):\n        self._history = []\n    def add(self, a, b):\n        result = a + b\n        self._history.append(('add', a, b, result))\n        return result\n    def subtract(self, a, b):\n        result = a - b\n        self._history.append(('subtract', a, b, result))\n        return result\n    def multiply(self, a, b):\n        result = a * b\n        self._history.append(('multiply', a, b, result))\n        return result\n    def divide(self, a, b):\n        if b == 0:\n            raise ValueError('divide by zero')\n        result = a / b\n        self._history.append(('divide', a, b, result))\n        return result\n", "output_tokens": 160},
  {"content": "class Calculator:\n    def __init__(self):\n        self._history = []\n    def add(self, a, b):\n        result = a + b\n        self._history.append(('add', a, b, result))\n        return result\n    def subtract(self, a, b):\n        result = a - b\n        self._history.append(('subtract', a, b, result))\n        return result\n    def multiply(self, a, b):\n        result = a * b\n        self._history.append(('multiply', a, b, result))\n        return result\n    def divide(self, a, b):\n        if b == 0:\n            raise ValueError('divide by zero')\n        result = a / b\n        self._history.append(('divide', a, b, result))\n        return result\n    def getHistory(self):\n        return list(self._history)\n", "output_tokens": 180},
  {"content": "class Calculator:\n    def __init__(self):\n        self._history = []\n    def add(self, a, b):\n        result = a + b\n        self._history.append(('add', a, b, result))\n        return result\n    def subtract(self, a, b):\n        result = a - b\n        self._history.append(('subtract', a, b, result))\n        return result\n    def multiply(self, a, b):\n        result = a * b\n        self._history.append(('multiply', a, b, result))\n        return result\n    def divide(self, a, b):\n        if b == 0:\n            raise ValueError('divide by zero')\n        result = a / b\n        self._history.append(('divide', a, b, result))\n        return result\n    def getHistory(self):\n        return list(self._history)\n    def clearHistory(self):\n        self._history = []\n", "output_tokens": 200}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "T1-00" "stub failed to start"; exit 1; }
write_govern "$WDIR" "Build a Calculator class with add, subtract, multiply, divide methods and history tracking."

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("coder")
    let msgs = [
        "Write the add method for the calculator",
        "Add the subtract method to the calculator",
        "Add the multiply method to the calculator",
        "Add the divide method with zero handling",
        "Add history access methods to the calculator",
        "Output the complete calculator class"
    ]
    let i = 0
    while i < msgs.length() {
        let r = agent.send(h, msgs[i])
        i = i + 1
    }
    let env = agent.environment(h)
    let st = env.get("state")
    print("MANDATE_DRIFT=" + string(st.get("mandate_drift_count")))
    print("SEMANTIC_INSTABILITY=" + string(st.get("semantic_stability_count")))
    print("MANDATE_ALIGNMENT=" + string(st.get("mandate_alignment")))
    let c = st.get("coherence")
    print("COHERENCE=" + string(c))
    if c >= 0.9 { print("COHERENCE_OK") } else { print("COHERENCE_LOW") }
    let ma = st.get("mandate_alignment")
    if ma >= 0.3 { print("ALIGNMENT_OK") } else { print("ALIGNMENT_LOW") }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "MANDATE_DRIFT=0"; then
    pass "T1-01" "on-mandate code: mandate_alignment signal never fired"
else
    fail "T1-01" "mandate drift fired on on-mandate code" "$OUTPUT"
fi
if echo "$OUTPUT" | grep -q "SEMANTIC_INSTABILITY=0"; then
    pass "T1-02" "on-mandate code: semantic_stability signal never fired"
else
    fail "T1-02" "semantic instability fired on consecutive on-task code" "$OUTPUT"
fi
if echo "$OUTPUT" | grep -q "ALIGNMENT_OK"; then
    pass "T1-03" "mandate_alignment mean >= 0.3 for on-mandate code"
else
    fail "T1-03" "mandate_alignment too low for on-mandate code" "$OUTPUT"
fi
if echo "$OUTPUT" | grep -q "COHERENCE_OK"; then
    pass "T1-04" "coherence stays >= 0.9 over 6 code-only turns"
else
    fail "T1-04" "coherence degraded on on-mandate code-only agent" "$OUTPUT"
fi

# ============================================================
# Group T2: off-mandate code still trips mandate_alignment
# ============================================================
echo ""
echo -e "${CYAN}--- Group T2: off-mandate code control ---${NC}"

WDIR="$TEST_TMP/t2"; mkdir -p "$WDIR"
# Same calculator mandate, but the agent produces unrelated HTML-parser
# code with zero mandate keyword overlap. The signal must still fire —
# code-awareness must not blind CDD.
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "class HtmlParser:\n    def parseNodes(self, markup):\n        tree = ElementTree(markup)\n        return tree.walkChildren()\n", "output_tokens": 50},
  {"content": "class HtmlParser:\n    def stripTags(self, markup):\n        cleaned = removeMarkup(markup)\n        return cleaned\n", "output_tokens": 45},
  {"content": "class HtmlParser:\n    def findLinks(self, document):\n        anchors = document.selectAnchors()\n        return anchors\n", "output_tokens": 45},
  {"content": "class HtmlParser:\n    def renderTemplate(self, layout):\n        widget = LayoutWidget(layout)\n        return widget.paint()\n", "output_tokens": 45}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "T2-00" "stub failed to start"; exit 1; }
write_govern "$WDIR" "You are a Python developer. Build a Calculator class with add, subtract, multiply, divide methods and history tracking. Output the complete code."

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("coder")
    let i = 0
    while i < 4 {
        let r = agent.send(h, "Continue the work")
        i = i + 1
    }
    let env = agent.environment(h)
    let st = env.get("state")
    print("MANDATE_DRIFT=" + string(st.get("mandate_drift_count")))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "MANDATE_DRIFT=0"; then
    fail "T2-01" "mandate_alignment did NOT fire on off-mandate code (signal blinded)" "$OUTPUT"
else
    if echo "$OUTPUT" | grep -q "MANDATE_DRIFT="; then
        pass "T2-01" "mandate_alignment still fires on off-mandate code"
    else
        fail "T2-01" "test did not produce mandate drift output" "$OUTPUT"
    fi
fi

# ============================================================
# Group T3: camelCase splitting proof (C++ path)
# ============================================================
echo ""
echo -e "${CYAN}--- Group T3: camelCase splitting reaches mandate words ---${NC}"

WDIR="$TEST_TMP/t3"; mkdir -p "$WDIR"
# Response identifiers are camelCase-ONLY forms of the mandate words:
# CalculatorEngine -> calculator, getHistory -> history,
# evaluateExpression -> evaluate + expression. Without splitting, the whole
# tokens (calculatorengine, gethistory, evaluateexpression) match nothing
# and alignment would be ~0.
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "engine = CalculatorEngine()\nrecorded = engine.getHistory()\nvalue = evaluateExpression(recorded)\n", "output_tokens": 40},
  {"content": "engine = CalculatorEngine()\nrecorded = engine.getHistory()\nvalue = evaluateExpression(recorded)\nprint(value)\n", "output_tokens": 45}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "T3-00" "stub failed to start"; exit 1; }
write_govern "$WDIR" "Track the calculator history and evaluate each expression."

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("coder")
    let r = agent.send(h, "Show the evaluation code")
    r = agent.send(h, "Print the evaluated value too")
    let env = agent.environment(h)
    let st = env.get("state")
    let ma = st.get("mandate_alignment")
    print("MANDATE_ALIGNMENT=" + string(ma))
    if ma >= 0.5 { print("SPLIT_OK") } else { print("SPLIT_FAIL") }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "SPLIT_OK"; then
    pass "T3-01" "camelCase identifiers match English mandate words (alignment >= 0.5)"
else
    fail "T3-01" "camelCase splitting not effective in C++ path" "$OUTPUT"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  ${GREEN}PASS: $PASS_COUNT${NC}  ${RED}FAIL: $FAIL_COUNT${NC}  ${YELLOW}SKIP: $SKIP_COUNT${NC}  TOTAL: $TOTAL"
if [ $FAIL_COUNT -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    echo "test_code_aware_keywords.sh: FAILED"
    exit 1
fi
echo "test_code_aware_keywords.sh: ALL PASSED"
exit 0
