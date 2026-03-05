import time, csv, io
data = '\n'.join(f'{i},name_{i},{i*3.14},{"active" if i%2==0 else "inactive"}' for i in range(100000))
start = time.monotonic_ns()
reader = csv.reader(io.StringIO(data))
rows = list(reader)
end = time.monotonic_ns()
print((end - start) // 1000)
