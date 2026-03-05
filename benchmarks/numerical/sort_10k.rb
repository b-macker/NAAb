data = Array.new(10000) { rand(1000000) }
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
data.sort
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
