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

# Test code with symbols
test_code = """fn add(a: int, b: int) -> int {
    return a + b
}

main {
    let x: int = 42
    let result = add(x, 10)
}
"""

print("="*60)
print("Testing Hover Provider")
print("="*60)
print("\nTest code:")
print(test_code)
print("="*60)

proc = subprocess.Popen(
    ['./build/naab-lsp'],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE
)

try:
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
                "uri": "file:///test_hover.naab",
                "languageId": "naab",
                "version": 1,
                "text": test_code
            }
        }
    })

    time.sleep(0.5)

    # Test 1: Hover over variable 'x' (line 5, character 8)
    print("\n✓ Test 1: Hover over variable 'x' (line 5)")
    send_message(proc, {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/hover",
        "params": {
            "textDocument": {"uri": "file:///test_hover.naab"},
            "position": {"line": 5, "character": 8}
        }
    })

    for i in range(3):
        response = read_response(proc)
        if not response:
            break
        if 'id' in response and response.get('id') == 2:
            print("Response:")
            print(json.dumps(response, indent=2))
            if response.get('result'):
                print("\n✓ Hover content:")
                print(response['result']['contents']['value'])
            break

    time.sleep(0.2)

    # Test 2: Hover over function 'add' (line 0, character 3)
    print("\n✓ Test 2: Hover over function 'add' (line 0)")
    send_message(proc, {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "textDocument/hover",
        "params": {
            "textDocument": {"uri": "file:///test_hover.naab"},
            "position": {"line": 0, "character": 3}
        }
    })

    for i in range(3):
        response = read_response(proc)
        if not response:
            break
        if 'id' in response and response.get('id') == 3:
            print("Response:")
            print(json.dumps(response, indent=2))
            if response.get('result'):
                print("\n✓ Hover content:")
                print(response['result']['contents']['value'])
            break

    # Shutdown
    send_message(proc, {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": {}})
    read_response(proc)
    send_message(proc, {"jsonrpc": "2.0", "method": "exit", "params": {}})

finally:
    proc.wait(timeout=2)
    stderr = proc.stderr.read().decode()
    if stderr:
        print(f"\n{'='*60}")
        print("Server stderr:")
        print(stderr)
