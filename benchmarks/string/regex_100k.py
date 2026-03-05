import time, re
lines = ["The quick brown fox jumps over the lazy dog #{}".format(i) for i in range(100000)]
pattern = re.compile(r'\b\w{5,}\b')
start = time.monotonic_ns()
count = sum(len(pattern.findall(line)) for line in lines)
end = time.monotonic_ns()
print((end - start) // 1000)
