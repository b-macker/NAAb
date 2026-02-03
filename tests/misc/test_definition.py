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

fn greet(name: string) -> string {
    return "Hello, " + name
}

struct Point {
    x: int
    y: int
}

main {
    let x: int = 42
    let result = add(x, 10)
    let message = greet("World")
}
"""

print("="*60)
print("Testing Go-to-Definition Provider")
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
                "uri": "file:///test_definition.naab",
                "languageId": "naab",
                "version": 1,
                "text": test_code
            }
        }
    })

    time.sleep(0.5)

    # Test 1: Go to definition of function 'add' from usage on line 15
    print("\n✓ Test 1: Go to definition of 'add' from line 15")
    send_message(proc, {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/definition",
        "params": {
            "textDocument": {"uri": "file:///test_definition.naab"},
            "position": {"line": 15, "character": 18}  # On 'add' in 'add(x, 10)'
        }
    })

    for i in range(3):
        response = read_response(proc)
        if not response:
            break
        if 'id' in response and response.get('id') == 2:
            print("Response:")
            print(json.dumps(response, indent=2))
            if response.get('result') and len(response['result']) > 0:
                loc = response['result'][0]
                print(f"\n✓ Definition found at:")
                print(f"   URI: {loc['uri']}")
                print(f"   Line: {loc['range']['start']['line']}")
                print(f"   Character: {loc['range']['start']['character']}")
            else:
                print("\n✗ No definition found")
            break

    time.sleep(0.2)

    # Test 2: Go to definition of variable 'x' from line 15
    print("\n✓ Test 2: Go to definition of variable 'x' from line 15")
    send_message(proc, {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "textDocument/definition",
        "params": {
            "textDocument": {"uri": "file:///test_definition.naab"},
            "position": {"line": 15, "character": 22}  # On 'x' in 'add(x, 10)'
        }
    })

    for i in range(3):
        response = read_response(proc)
        if not response:
            break
        if 'id' in response and response.get('id') == 3:
            print("Response:")
            print(json.dumps(response, indent=2))
            if response.get('result') and len(response['result']) > 0:
                loc = response['result'][0]
                print(f"\n✓ Definition found at:")
                print(f"   URI: {loc['uri']}")
                print(f"   Line: {loc['range']['start']['line']}")
                print(f"   Character: {loc['range']['start']['character']}")
            else:
                print("\n✗ No definition found")
            break

    time.sleep(0.2)

    # Test 3: Go to definition of function 'greet' from line 16
    print("\n✓ Test 3: Go to definition of function 'greet' from line 16")
    send_message(proc, {
        "jsonrpc": "2.0",
        "id": 4,
        "method": "textDocument/definition",
        "params": {
            "textDocument": {"uri": "file:///test_definition.naab"},
            "position": {"line": 16, "character": 18}  # On 'greet' in 'greet("World")'
        }
    })

    for i in range(3):
        response = read_response(proc)
        if not response:
            break
        if 'id' in response and response.get('id') == 4:
            print("Response:")
            print(json.dumps(response, indent=2))
            if response.get('result') and len(response['result']) > 0:
                loc = response['result'][0]
                print(f"\n✓ Definition found at:")
                print(f"   URI: {loc['uri']}")
                print(f"   Line: {loc['range']['start']['line']}")
                print(f"   Character: {loc['range']['start']['character']}")
            else:
                print("\n✗ No definition found")
            break

    # Shutdown
    send_message(proc, {"jsonrpc": "2.0", "id": 5, "method": "shutdown", "params": {}})
    read_response(proc)
    send_message(proc, {"jsonrpc": "2.0", "method": "exit", "params": {}})

finally:
    proc.wait(timeout=2)
    stderr = proc.stderr.read().decode()
    if stderr:
        print(f"\n{'='*60}")
        print("Server stderr (last 1500 chars):")
        print(stderr[-1500:])
