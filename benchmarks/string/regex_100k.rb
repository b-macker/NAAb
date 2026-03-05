lines = Array.new(100000) { |i| "The quick brown fox jumps over the lazy dog ##{i}" }
pattern = /\b\w{5,}\b/
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
count = lines.sum { |line| line.scan(pattern).length }
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
