import std/times
type Obj = object
  id: int
  data: array[10, int]
let start = cpuTime()
var objs = newSeq[Obj](1000000)
for i in 0..<1000000:
  objs[i].id = i
objs.setLen(0)
echo int((cpuTime() - start) * 1_000_000)
