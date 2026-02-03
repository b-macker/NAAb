#!/usr/bin/env python3
import subprocess
import json
import time

def send_message(proc, message):
    content = json.dumps(message)
    header = f"Content-Length: {len(content)}\r\n\r\n"
    full_message = header + content
    proc.stdin.write(full_message.encode())
    proc.stdin.flush()
    time.sleep(0.1)

def read_response(proc):
    headers = {}
    while True:
        line = proc.stdout.readline().decode().strip()
        if not line:
            break
        if ':' in line:
            key, value = line.split(':', 1)
            headers[key.strip()] = value.strip()

    content_length = int(headers.get('Content-Length', 0))
    if content_length > 0:
        content = proc.stdout.read(content_length).decode()
        return json.loads(content)
    return None

proc = subprocess.Popen(
    ['./build/naab-lsp'],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE
)

try:
    with open('test_symbols_simple.naab', 'r') as f:
        test_content = f.read()

    print("Test file content:")
    print(test_content)
    print("\n" + "="*60)

    # Initialize
    send_message(proc, {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {"processId": None, "rootUri": "file:///test"}
    })
    read_response(proc)

    # Initialized
    send_message(proc, {
        "jsonrpc": "2.0",
        "method": "initialized",
        "params": {}
    })

    # Open document
    send_message(proc, {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": "file:///test_symbols_simple.naab",
                "languageId": "naab",
                "version": 1,
                "text": test_content
            }
        }
    })

    time.sleep(0.5)

    # Request symbols
    send_message(proc, {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/documentSymbol",
        "params": {
            "textDocument": {"uri": "file:///test_symbols_simple.naab"}
        }
    })

    # Read responses
    for i in range(5):
        response = read_response(proc)
        if not response:
            break
        if 'id' in response and response.get('id') == 2:
            print("\n✓ Document Symbols Response:")
            print(json.dumps(response, indent=2))

            # Analyze result
            if response.get('result'):
                symbols = response['result']
                print(f"\n✓ Found {len(symbols)} symbols:")
                for sym in symbols:
                    print(f"  - {sym.get('name')} ({sym.get('kind')})")
                    if 'children' in sym and sym['children']:
                        for child in sym['children']:
                            print(f"    - {child.get('name')} ({child.get('kind')})")
            break
        elif 'method' in response:
            print(f"\nNotification: {response['method']}")
            if response['method'] == 'textDocument/publishDiagnostics':
                diags = response['params'].get('diagnostics', [])
                if diags:
                    print(f"  ⚠ {len(diags)} diagnostic(s):")
                    for diag in diags:
                        print(f"    - {diag.get('message')}")

    # Shutdown
    send_message(proc, {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}})
    read_response(proc)
    send_message(proc, {"jsonrpc": "2.0", "method": "exit", "params": {}})

finally:
    proc.wait(timeout=2)
    stderr = proc.stderr.read().decode()
    if stderr:
        print(f"\n{'='*60}")
        print("Server stderr:")
        print(stderr)
