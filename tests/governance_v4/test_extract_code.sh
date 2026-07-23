#!/usr/bin/env bash
# ============================================================
# test_extract_code.sh — agent.extract_code() fence extraction
#
# agent.extract_code is a PURE function (no handle, no governance, no API):
# it takes a string + optional language hint and returns the extracted code.
# So this test drives it directly on literal strings with sentinel bodies —
# no stub, no keys — and asserts the documented contract:
#   "prefers matching lang hint, returns longest block if multiple found;
#    strips surrounding conversational text; returns input unchanged if no
#    fence found."
#
# Regression targets (all failed before the multi-fence rewrite):
#   T3 — a non-target fenced block BEFORE the target one (old: whole response)
#   T4 — a bare ``` fence with no language tag  (old: guard rejected it)
#   T5 — multiple matching blocks              (old: only the first)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/extract-code-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

# Single-line sentinel bodies so each extracted result is exactly one output
# line — easy to assert, and any leaked fence/prose is immediately visible.
# Quoted heredoc: backticks and \n pass through literally; NAAb's lexer turns
# \n into newlines inside the string literals.
cat > "$TEST_TMP/t.naab" <<'NAABEOF'
use agent
main {
    // T1: single python block
    print("T1|" + agent.extract_code("```python\nPYBODY1\n```", "python"))
    // T2: prose around a python block
    print("T2|" + agent.extract_code("Here is the code:\n```python\nPYBODY2\n```\nHope it helps.", "python"))
    // T3: a non-target (text) block BEFORE the target python block
    print("T3|" + agent.extract_code("Notes:\n```text\nTEXTBODY\n```\n```python\nPYBODY3\n```", "python"))
    // T4: a BARE fence (no language tag) with surrounding prose
    print("T4|" + agent.extract_code("Here:\n```\nBAREBODY\n```", "python"))
    // T5: two matching blocks of different length — expect the longer
    print("T5|" + agent.extract_code("```python\nSHORT\n```\n```python\nLONGER_BODY_HERE\n```", "python"))
    // T6: json hint selects the json block, not the python one
    print("T6|" + agent.extract_code("```json\nJSONBODY\n```\n```python\nPYBODY6\n```", "json"))
    // T7: hint matches no block; one untagged block — fallback to longest
    print("T7|" + agent.extract_code("```\nUNTAGGED\n```", "python"))
    // T8: no fence at all — return input unchanged
    print("T8|" + agent.extract_code("just plain text no fences", "python"))
}
NAABEOF

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   agent.extract_code() fence extraction (pure, no API)       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# extract_code needs no governance; run without it. Fall back to a minimal
# inline govern.json only if the agent module refuses to load unconfigured.
OUT=$(cd "$TEST_TMP" && timeout 30s "$NAAB" --no-governance t.naab 2>&1) || true
if ! echo "$OUT" | grep -q "^T1|"; then
    cat > "$TEST_TMP/govern.json" <<'GEOF'
{ "version": "5.0", "mode": "advisory", "security": { "sandbox_level": "elevated" } }
GEOF
    OUT=$(cd "$TEST_TMP" && timeout 30s "$NAAB" t.naab 2>&1) || true
fi

if ! echo "$OUT" | grep -q "^T1|"; then
    fail "T0" "extract_code test harness did not run" "$(echo "$OUT" | head -4)"
else
    pass "T0" "test program executed"

    check() {  # $1=id $2=expected exact value after the pipe $3=desc
        local got; got=$(echo "$OUT" | grep "^$1|" | head -1 | sed "s/^$1|//")
        if [ "$got" = "$2" ]; then
            pass "$1" "$3"
        else
            fail "$1" "$3" "expected [$2] got [$got]"
        fi
    }

    check "T1" "PYBODY1"          "single python block extracted"
    check "T2" "PYBODY2"          "prose stripped around python block"
    check "T3" "PYBODY3"          "target block found past a leading non-target fence"
    check "T4" "BAREBODY"         "bare (no-language) fence extracted"
    check "T5" "LONGER_BODY_HERE" "longest of multiple matching blocks"
    check "T6" "JSONBODY"         "json hint selects the json block"
    check "T7" "UNTAGGED"         "fallback to longest block when hint matches none"
    check "T8" "just plain text no fences" "no fence — input returned unchanged"

    # Explicit anti-leak assertions for the two headline bugs.
    if echo "$OUT" | grep "^T3|" | grep -q "TEXTBODY\|\`\`\`"; then
        fail "T3-leak" "T3 leaked the non-target block or fence markers"
    else
        pass "T3-leak" "T3 output free of the text block and fence markers"
    fi
    if echo "$OUT" | grep "^T4|" | grep -q "Here:\|\`\`\`"; then
        fail "T4-leak" "T4 leaked prose or fence markers"
    else
        pass "T4-leak" "T4 output free of prose and fence markers"
    fi
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
