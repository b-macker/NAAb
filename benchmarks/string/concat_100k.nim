import std/[times, strutils, sequtils]
var parts: seq[string]
for i in 0..<100000:
  parts.add($i)
let start = cpuTime()
let result = parts.join("")
echo int((cpuTime() - start) * 1_000_000)
