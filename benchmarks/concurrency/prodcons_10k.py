import time, queue, threading
q = queue.Queue()
result = [0]
def producer():
    for i in range(10000):
        q.put(i)
    q.put(None)
def consumer():
    s = 0
    while True:
        item = q.get()
        if item is None: break
        s += item
    result[0] = s
start = time.monotonic_ns()
p = threading.Thread(target=producer)
c = threading.Thread(target=consumer)
p.start(); c.start()
p.join(); c.join()
end = time.monotonic_ns()
print((end - start) // 1000)
