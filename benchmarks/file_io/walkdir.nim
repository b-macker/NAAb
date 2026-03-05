import std/[times, os]
let home = getHomeDir()
let root = home / ".naab" / "language" / "src"
let start = cpuTime()
var count = 0
for path in walkDirRec(root):
  count += 1
echo int((cpuTime() - start) * 1_000_000)
