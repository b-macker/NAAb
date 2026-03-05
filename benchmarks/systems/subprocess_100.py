import time, subprocess
start = time.monotonic_ns()
for _ in range(100):
    subprocess.run(['true'], capture_output=True)
end = time.monotonic_ns()
print((end - start) // 1000)
