import std/[times, threadpool]
{.experimental: "parallel".}
proc worker(idx: int): int =
  var s = 0
  for i in 0..<10000:
    s += i
  return s

let start = cpuTime()
var results: array[100, int]
for i in 0..<100:
  results[i] = worker(i)
echo int((cpuTime() - start) * 1_000_000)
