#!/bin/bash

# Test LSP document synchronization and diagnostics

# Read the test file
FILE_CONTENT=$(cat test_lsp_document.naab | sed 's/\\/\\\\/g' | sed 's/"/\\"/g' | awk '{printf "%s\\n", $0}' | sed 's/\\n$//')

# Create didOpen message
cat << EOF | timeout 2 ./build/naab-lsp 2>&1
Content-Length: 84

{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null}}
Content-Length: 44

{"jsonrpc":"2.0","method":"initialized","params":{}}
Content-Length: $((${#FILE_CONTENT} + 150))

{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///test.naab","languageId":"naab","version":1,"text":"${FILE_CONTENT}"}}}
EOF
