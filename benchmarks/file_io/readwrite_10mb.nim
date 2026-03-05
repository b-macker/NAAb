import std/[times, os, strutils]
let data = repeat('x', 10 * 1024 * 1024)
let path = getTempDir() / "bench_nim_rw.tmp"
let start = cpuTime()
writeFile(path, data)
discard readFile(path)
echo int((cpuTime() - start) * 1_000_000)
removeFile(path)
