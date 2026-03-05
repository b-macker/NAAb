import std/[times, json, strformat]
var items = newJArray()
for i in 0..<10000:
  items.add(%*{"id": i, "name": fmt"item_{i}", "tags": ["t0","t1","t2","t3","t4"], "value": float(i) * 2.718})
let start = cpuTime()
let result = $items
echo int((cpuTime() - start) * 1_000_000)
