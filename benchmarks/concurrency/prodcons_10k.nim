import std/times
# Nim single-threaded producer-consumer simulation
let start = cpuTime()
var queue: seq[int]
for i in 0..<10000:
  queue.add(i)
var sum = 0
for v in queue:
  sum += v
echo int((cpuTime() - start) * 1_000_000)
