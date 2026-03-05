import std/[times, strutils, sequtils]
var lines: seq[string]
for i in 0..<10000:
  lines.add("line " & $i & ": the quick brown fox")
let start = cpuTime()
let result = lines.mapIt(it.toUpperAscii())
echo int((cpuTime() - start) * 1_000_000)
