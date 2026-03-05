import time, socket, threading, json
def server_fn(srv):
    conn, _ = srv.accept()
    while True:
        data = conn.recv(4096)
        if not data: break
        conn.sendall(data)
    conn.close()
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', 0))
port = srv.getsockname()[1]
srv.listen(1)
t = threading.Thread(target=server_fn, args=(srv,), daemon=True)
t.start()
client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
client.connect(('127.0.0.1', port))
msg = json.dumps({"id": 1, "data": "hello"}).encode()
start = time.monotonic_ns()
for _ in range(1000):
    client.sendall(msg)
    client.recv(4096)
end = time.monotonic_ns()
client.close()
srv.close()
print((end - start) // 1000)
