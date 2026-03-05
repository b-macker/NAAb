import time, os, tempfile
data = "x" * (10 * 1024 * 1024)
tmp = tempfile.mktemp(suffix='.bench')
start = time.monotonic_ns()
with open(tmp, 'w') as f:
    f.write(data)
with open(tmp, 'r') as f:
    _ = f.read()
end = time.monotonic_ns()
os.unlink(tmp)
print((end - start) // 1000)
