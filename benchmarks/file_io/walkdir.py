import time, os
start = time.monotonic_ns()
count = sum(len(files) for _, _, files in os.walk(os.path.expanduser('~/.naab/language/src')))
end = time.monotonic_ns()
print((end - start) // 1000)
