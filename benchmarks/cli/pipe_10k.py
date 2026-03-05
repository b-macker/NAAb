import time, sys
lines = [f"line {i}: the quick brown fox\n" for i in range(10000)]
start = time.monotonic_ns()
result = [l.upper() for l in lines]
end = time.monotonic_ns()
print((end - start) // 1000)
