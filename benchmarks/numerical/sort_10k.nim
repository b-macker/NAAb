import std/[times, random, algorithm, sequtils]
randomize(42)
var data = newSeqWith(10000, rand(1000000))
let start = cpuTime()
data.sort()
let elapsed = cpuTime() - start
echo int(elapsed * 1_000_000)
