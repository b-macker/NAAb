import std/times
const N = 100
var a, b, c: array[N, array[N, float]]
for i in 0..<N:
  for j in 0..<N:
    a[i][j] = float((i*7 + j*13) mod 97)
    b[i][j] = float((i*11 + j*17) mod 89)
let start = cpuTime()
for i in 0..<N:
  for j in 0..<N:
    var sum = 0.0
    for k in 0..<N:
      sum += a[i][k] * b[k][j]
    c[i][j] = sum
echo int((cpuTime() - start) * 1_000_000)
