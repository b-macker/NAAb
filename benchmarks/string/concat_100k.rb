parts = Array.new(100000) { |i| i.to_s }
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
result = parts.join
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
