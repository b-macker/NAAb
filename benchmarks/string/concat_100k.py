import time
parts = [str(i) for i in range(100000)]
start = time.monotonic_ns()
result = ''.join(parts)
end = time.monotonic_ns()
print((end - start) // 1000)
