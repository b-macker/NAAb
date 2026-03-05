import time, threading
results = [0] * 100
def worker(idx):
    s = 0
    for i in range(10000):
        s += i
    results[idx] = s
start = time.monotonic_ns()
threads = [threading.Thread(target=worker, args=(i,)) for i in range(100)]
for t in threads: t.start()
for t in threads: t.join()
end = time.monotonic_ns()
print((end - start) // 1000)
