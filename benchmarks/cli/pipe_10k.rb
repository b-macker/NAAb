lines = Array.new(10000) { |i| "line #{i}: the quick brown fox" }
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
result = lines.map(&:upcase)
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
