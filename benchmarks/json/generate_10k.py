import time, json
items = [{"id": i, "name": f"item_{i}", "tags": [f"t{j}" for j in range(5)], "value": i * 2.718} for i in range(10000)]
start = time.monotonic_ns()
result = json.dumps(items)
end = time.monotonic_ns()
print((end - start) // 1000)
