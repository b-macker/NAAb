import time
start = time.monotonic_ns()
objs = [{"id": i, "data": [0]*10} for i in range(1000000)]
del objs
end = time.monotonic_ns()
print((end - start) // 1000)
