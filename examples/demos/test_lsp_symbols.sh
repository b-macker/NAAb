#!/bin/bash

# Test script for LSP document symbols

# Read the test file content
TEST_FILE="test_symbols.naab"
CONTENT=$(cat "$TEST_FILE" | jq -Rs .)

# Calculate content length for each message
INIT_MSG='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///test"}}'
INIT_LEN=$(echo -n "$INIT_MSG" | wc -c)

INITIALIZED_MSG='{"jsonrpc":"2.0","method":"initialized","params":{}}'
INITIALIZED_LEN=$(echo -n "$INITIALIZED_MSG" | wc -c)

# Create didOpen message with actual file content
DID_OPEN_MSG=$(cat <<EOF
{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///$TEST_FILE","languageId":"naab","version":1,"text":$CONTENT}}}
EOF
)
DID_OPEN_LEN=$(echo -n "$DID_OPEN_MSG" | wc -c)

# Wait a moment for parsing
sleep 0.5

# Create documentSymbol request
SYMBOLS_MSG='{"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///test_symbols.naab"}}}'
SYMBOLS_LEN=$(echo -n "$SYMBOLS_MSG" | wc -c)

# Create shutdown message
SHUTDOWN_MSG='{"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}}'
SHUTDOWN_LEN=$(echo -n "$SHUTDOWN_MSG" | wc -c)

# Create exit notification
EXIT_MSG='{"jsonrpc":"2.0","method":"exit","params":{}}'
EXIT_LEN=$(echo -n "$EXIT_MSG" | wc -c)

# Send messages to LSP server
{
    echo -ne "Content-Length: $INIT_LEN\r\n\r\n$INIT_MSG"
    sleep 0.2
    echo -ne "Content-Length: $INITIALIZED_LEN\r\n\r\n$INITIALIZED_MSG"
    sleep 0.2
    echo -ne "Content-Length: $DID_OPEN_LEN\r\n\r\n$DID_OPEN_MSG"
    sleep 0.5
    echo -ne "Content-Length: $SYMBOLS_LEN\r\n\r\n$SYMBOLS_MSG"
    sleep 0.5
    echo -ne "Content-Length: $SHUTDOWN_LEN\r\n\r\n$SHUTDOWN_MSG"
    sleep 0.2
    echo -ne "Content-Length: $EXIT_LEN\r\n\r\n$EXIT_MSG"
} | ./build/naab-lsp 2>&1 | grep -A 50 "documentSymbol"
