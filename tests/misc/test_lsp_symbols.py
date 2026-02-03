#!/usr/bin/env python3
import subprocess
import json
import time
import sys

def send_message(proc, message):
    """Send a JSON-RPC message to the LSP server."""
    content = json.dumps(message)
    header = f"Content-Length: {len(content)}\r\n\r\n"
    full_message = header + content
    proc.stdin.write(full_message.encode())
    proc.stdin.flush()
    time.sleep(0.1)

def read_response(proc):
    """Read a JSON-RPC response from the LSP server."""
    # Read headers
    headers = {}
    while True:
        line = proc.stdout.readline().decode().strip()
        if not line:
            break
        if ':' in line:
            key, value = line.split(':', 1)
            headers[key.strip()] = value.strip()

    # Read content
    content_length = int(headers.get('Content-Length', 0))
    if content_length > 0:
        content = proc.stdout.read(content_length).decode()
        return json.loads(content)
    return None

# Start LSP server
proc = subprocess.Popen(
    ['./build/naab-lsp'],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE
)

try:
    # Read test file
    with open('test_symbols.naab', 'r') as f:
        test_content = f.read()

    # 1. Initialize
    print("Sending initialize request...")
    init_msg = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "processId": None,
            "rootUri": "file:///test"
        }
    }
    send_message(proc, init_msg)
    response = read_response(proc)
    print(f"Initialize response: {json.dumps(response, indent=2)}")

    # 2. Initialized notification
    print("\nSending initialized notification...")
    send_message(proc, {
        "jsonrpc": "2.0",
        "method": "initialized",
        "params": {}
    })

    # 3. Open document
    print("\nSending didOpen notification...")
    send_message(proc, {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": "file:///test_symbols.naab",
                "languageId": "naab",
                "version": 1,
                "text": test_content
            }
        }
    })

    # Check for diagnostics
    time.sleep(0.3)

    # Wait for diagnostics to be published
    time.sleep(0.5)

    # 4. Request document symbols
    print("\nSending documentSymbol request...")
    symbols_msg = {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/documentSymbol",
        "params": {
            "textDocument": {
                "uri": "file:///test_symbols.naab"
            }
        }
    }
    send_message(proc, symbols_msg)

    # Read all responses/notifications
    print("\nReading responses...")
    for i in range(3):  # Read up to 3 messages
        response = read_response(proc)
        if response:
            if 'id' in response and response.get('id') == 2:
                print(f"\nDocument symbols response:")
                print(json.dumps(response, indent=2))
                break
            else:
                print(f"\nNotification/other message:")
                print(json.dumps(response, indent=2))

    # 5. Shutdown
    print("\nShutting down...")
    send_message(proc, {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "shutdown",
        "params": {}
    })
    read_response(proc)

    # 6. Exit
    send_message(proc, {
        "jsonrpc": "2.0",
        "method": "exit",
        "params": {}
    })

finally:
    proc.wait(timeout=2)
    stderr = proc.stderr.read().decode()
    if stderr:
        print(f"\nServer stderr:\n{stderr}")
