import time
N = 100
A = [[float((i*7+j*13) % 97) for j in range(N)] for i in range(N)]
B = [[float((i*11+j*17) % 89) for j in range(N)] for i in range(N)]
start = time.monotonic_ns()
C = [[sum(A[i][k]*B[k][j] for k in range(N)) for j in range(N)] for i in range(N)]
end = time.monotonic_ns()
print((end - start) // 1000)
