#!/bin/bash
# NAAb LSP Server Integration Test
# Tests: initialize, didOpen, completion, hover, definition, documentSymbol, shutdown
#
# Usage: bash tests/lsp/test_lsp_basic.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_LSP="${SCRIPT_DIR}/../../build/naab-lsp"
PASS=0
FAIL=0

if [[ ! -x "$NAAB_LSP" ]]; then
    echo "Error: naab-lsp not found at $NAAB_LSP"
    echo "Build first: cd build && cmake .. && make naab-lsp -j4"
    exit 1
fi

# Helper: wrap JSON body in Content-Length header
lsp_msg() {
    local body="$1"
    local len=${#body}
    printf "Content-Length: %d\r\n\r\n%s" "$len" "$body"
}

# Helper: send multiple LSP messages and capture all output
run_lsp_session() {
    local input="$1"
    # Run with ERROR log level to reduce noise, timeout after 5s
    echo -n "$input" | NAAB_LSP_LOG_LEVEL=ERROR timeout 5 "$NAAB_LSP" 2>/dev/null || true
}

# Helper: extract JSON responses from raw LSP output
# Strips Content-Length headers, returns one JSON per line
extract_responses() {
    local raw="$1"
    echo "$raw" | grep -oP '\{.*\}' || true
}

check() {
    local desc="$1"
    local output="$2"
    local pattern="$3"

    if echo "$output" | grep -q "$pattern"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected pattern: $pattern)"
        echo "  Output: $output"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== NAAb LSP Integration Tests ==="
echo ""

# --- Test 1: Initialize ---
echo "Test 1: Initialize handshake"

INIT_BODY='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
SHUTDOWN_BODY='{"jsonrpc":"2.0","id":99,"method":"shutdown"}'
EXIT_BODY='{"jsonrpc":"2.0","method":"exit"}'

INPUT="$(lsp_msg "$INIT_BODY")$(lsp_msg "$SHUTDOWN_BODY")$(lsp_msg "$EXIT_BODY")"
OUTPUT=$(run_lsp_session "$INPUT")

check "Server returns capabilities" "$OUTPUT" '"capabilities"'
check "Server returns name naab-lsp" "$OUTPUT" '"naab-lsp"'
check "Server returns version" "$OUTPUT" '"version"'
check "completionProvider advertised" "$OUTPUT" '"completionProvider"'
check "hoverProvider advertised" "$OUTPUT" '"hoverProvider"'
check "definitionProvider advertised" "$OUTPUT" '"definitionProvider"'

# --- Test 2: Document Open + Symbols ---
echo ""
echo "Test 2: Document open + document symbols"

# Create a test .naab file
TEST_FILE="/data/data/com.termux/files/home/.naab/language/tests/lsp/test_sample.naab"
cat > "$TEST_FILE" << 'NAAB'
fn greet(name) {
    return "Hello, " + name
}

struct Point {
    x: int
    y: int
}

main {
    let msg = greet("world")
    print(msg)
}
NAAB

FILE_URI="file://${TEST_FILE}"
# Escape the file content for JSON (replace newlines, quotes)
FILE_TEXT=$(cat "$TEST_FILE" | python3 -c "import sys,json; print(json.dumps(sys.stdin.read()))")

INIT_BODY='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
INITIALIZED_BODY='{"jsonrpc":"2.0","method":"initialized","params":{}}'
OPEN_BODY="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${FILE_URI}\",\"languageId\":\"naab\",\"version\":1,\"text\":${FILE_TEXT}}}}"
SYMBOLS_BODY="{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/documentSymbol\",\"params\":{\"textDocument\":{\"uri\":\"${FILE_URI}\"}}}"
SHUTDOWN_BODY='{"jsonrpc":"2.0","id":99,"method":"shutdown"}'
EXIT_BODY='{"jsonrpc":"2.0","method":"exit"}'

INPUT="$(lsp_msg "$INIT_BODY")$(lsp_msg "$INITIALIZED_BODY")$(lsp_msg "$OPEN_BODY")$(lsp_msg "$SYMBOLS_BODY")$(lsp_msg "$SHUTDOWN_BODY")$(lsp_msg "$EXIT_BODY")"
OUTPUT=$(run_lsp_session "$INPUT")

check "greet function found" "$OUTPUT" '"greet"'
check "Point struct found" "$OUTPUT" '"Point"'
check "main block found" "$OUTPUT" '"main"'

# --- Test 3: Hover ---
echo ""
echo "Test 3: Hover on function name"

HOVER_BODY="{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"${FILE_URI}\"},\"position\":{\"line\":10,\"character\":14}}}"

INPUT="$(lsp_msg "$INIT_BODY")$(lsp_msg "$INITIALIZED_BODY")$(lsp_msg "$OPEN_BODY")$(lsp_msg "$HOVER_BODY")$(lsp_msg "$SHUTDOWN_BODY")$(lsp_msg "$EXIT_BODY")"
OUTPUT=$(run_lsp_session "$INPUT")

check "Hover returns content" "$OUTPUT" '"contents"'

# --- Test 4: Completion ---
echo ""
echo "Test 4: Completion"

COMPLETION_BODY="{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"${FILE_URI}\"},\"position\":{\"line\":10,\"character\":4}}}"

INPUT="$(lsp_msg "$INIT_BODY")$(lsp_msg "$INITIALIZED_BODY")$(lsp_msg "$OPEN_BODY")$(lsp_msg "$COMPLETION_BODY")$(lsp_msg "$SHUTDOWN_BODY")$(lsp_msg "$EXIT_BODY")"
OUTPUT=$(run_lsp_session "$INPUT")

check "Completion returns items" "$OUTPUT" '"items"'
check "Keywords in completions" "$OUTPUT" '"let"'

# --- Test 5: Definition ---
echo ""
echo "Test 5: Go-to-definition"

# Click on "greet" call at line 10 (0-based), character 14
DEF_BODY="{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"${FILE_URI}\"},\"position\":{\"line\":10,\"character\":14}}}"

INPUT="$(lsp_msg "$INIT_BODY")$(lsp_msg "$INITIALIZED_BODY")$(lsp_msg "$OPEN_BODY")$(lsp_msg "$DEF_BODY")$(lsp_msg "$SHUTDOWN_BODY")$(lsp_msg "$EXIT_BODY")"
OUTPUT=$(run_lsp_session "$INPUT")

check "Definition returns location" "$OUTPUT" '"range"'

# --- Summary ---
echo ""
echo "=== Results ==="
TOTAL=$((PASS + FAIL))
echo "  $PASS/$TOTAL passed"

if [[ $FAIL -gt 0 ]]; then
    echo "  $FAIL FAILED"
    # Clean up
    rm -f "$TEST_FILE"
    exit 1
fi

# Clean up
rm -f "$TEST_FILE"
echo "  All tests passed!"
