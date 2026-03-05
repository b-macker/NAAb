import std/[times, strutils, sequtils]
var data: string
for i in 0..<100000:
  let status = if i mod 2 == 0: "active" else: "inactive"
  data.add($i & ",name_" & $i & "," & $(float(i)*3.14) & "," & status & "\n")
let start = cpuTime()
let rows = data.splitLines().mapIt(it.split(','))
echo int((cpuTime() - start) * 1_000_000)
