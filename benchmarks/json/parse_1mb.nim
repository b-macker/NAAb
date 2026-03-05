import std/[times, json, strformat]
var arr = newJArray()
for i in 0..<20000:
  arr.add(%*{"id": i, "name": fmt"item_{i}", "value": float(i) * 3.14})
let data = $arr
let start = cpuTime()
discard parseJson(data)
echo int((cpuTime() - start) * 1_000_000)
