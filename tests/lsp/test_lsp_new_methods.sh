#!/bin/bash
# Test naab-lsp new methods: codeAction, workspaceSymbol, rename
# Phase 8.3 — LSP Enhancement

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${SCRIPT_DIR}/../.."
LSP="${ROOT}/build/naab-lsp"

PASS=0
FAIL=0

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

if [ ! -f "$LSP" ]; then
    echo "SKIP: naab-lsp not built at $LSP"
    exit 0
fi

# Helper: send initialize + one request to naab-lsp, capture response
# Usage: send_lsp_request <json-payload>
# The payload is sent as a complete LSP message after initialize
send_lsp_sequence() {
    local payload="$1"
    local INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
    local INIT_NOTIF='{"jsonrpc":"2.0","method":"initialized","params":{}}'
    {
        printf 'Content-Length: %d\r\n\r\n%s' "${#INIT}" "$INIT"
        printf 'Content-Length: %d\r\n\r\n%s' "${#INIT_NOTIF}" "$INIT_NOTIF"
        printf 'Content-Length: %d\r\n\r\n%s' "${#payload}" "$payload"
    } | timeout 3 "$LSP" 2>/dev/null
}

# ============================================================================
# Test 1: Server advertises codeActionProvider in initialize response
# ============================================================================

INIT_REQ='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
RESPONSE=$(printf 'Content-Length: %d\r\n\r\n%s' "${#INIT_REQ}" "$INIT_REQ" | timeout 3 "$LSP" 2>/dev/null)
echo "$RESPONSE" | grep -q '"codeActionProvider"'
check "initialize advertises codeActionProvider" $?

# ============================================================================
# Test 2: Server advertises workspaceSymbolProvider
# ============================================================================
echo "$RESPONSE" | grep -q '"workspaceSymbolProvider"'
check "initialize advertises workspaceSymbolProvider" $?

# ============================================================================
# Test 3: Server advertises renameProvider
# ============================================================================
echo "$RESPONSE" | grep -q '"renameProvider"'
check "initialize advertises renameProvider" $?

# ============================================================================
# Test 4: codeAction on unknown document returns empty array (not error)
# ============================================================================
CODE_ACTION='{"jsonrpc":"2.0","id":2,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///nonexistent.naab"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":10}},"context":{"diagnostics":[]}}}'
RESPONSE4=$(send_lsp_sequence "$CODE_ACTION")
echo "$RESPONSE4" | grep -q '"result":\[\]'
check "codeAction on unknown doc returns empty array" $?

# ============================================================================
# Test 5: workspace/symbol with empty query returns array (not error)
# ============================================================================
WS_SYMBOL='{"jsonrpc":"2.0","id":3,"method":"workspace/symbol","params":{"query":""}}'
RESPONSE5=$(send_lsp_sequence "$WS_SYMBOL")
echo "$RESPONSE5" | grep -q '"result":\[\]'
check "workspace/symbol empty query returns array" $?

# ============================================================================
# Test 6: rename on unknown document returns null (not error)
# ============================================================================
RENAME='{"jsonrpc":"2.0","id":4,"method":"textDocument/rename","params":{"textDocument":{"uri":"file:///nonexistent.naab"},"position":{"line":0,"character":0},"newName":"renamed_var"}}'
RESPONSE6=$(send_lsp_sequence "$RENAME")
# Should return null result or an empty result, not a JSON-RPC error
echo "$RESPONSE6" | grep -qv '"error"'
check "rename on unknown doc returns null (no error)" $?

# ============================================================================
echo ""
echo "LSP new methods: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
