import time, http.server, threading, urllib.request
# Start a minimal HTTP server
handler = http.server.SimpleHTTPRequestHandler
server = http.server.HTTPServer(('127.0.0.1', 0), handler)
port = server.server_address[1]
t = threading.Thread(target=server.serve_forever, daemon=True)
t.start()
start = time.monotonic_ns()
for _ in range(100):
    urllib.request.urlopen(f'http://127.0.0.1:{port}/').read()
end = time.monotonic_ns()
server.shutdown()
print((end - start) // 1000)
