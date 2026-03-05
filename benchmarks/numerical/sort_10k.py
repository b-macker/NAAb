import time, random
random.seed(42)
data = [random.randint(0, 1000000) for _ in range(10000)]
start = time.monotonic_ns()
sorted(data)
end = time.monotonic_ns()
print((end - start) // 1000)
