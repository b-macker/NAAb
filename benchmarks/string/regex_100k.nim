import std/[times, strutils, strformat]
var lines: seq[string]
for i in 0..<100000:
  lines.add(fmt"The quick brown fox jumps over the lazy dog {i}")
let start = cpuTime()
var count = 0
for line in lines:
  for word in line.split(' '):
    if word.len >= 5:
      count += 1
echo int((cpuTime() - start) * 1_000_000)
