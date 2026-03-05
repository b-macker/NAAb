import std/[times, osproc]
let start = cpuTime()
for i in 0..<100:
  discard execCmd("true")
echo int((cpuTime() - start) * 1_000_000)
