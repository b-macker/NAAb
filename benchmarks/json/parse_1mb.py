import time, json
data = json.dumps([{"id": i, "name": f"item_{i}", "value": i * 3.14} for i in range(20000)])
start = time.monotonic_ns()
parsed = json.loads(data)
end = time.monotonic_ns()
print((end - start) // 1000)
