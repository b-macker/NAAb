results = Array.new(100, 0)
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
threads = 100.times.map { |idx|
  Thread.new {
    s = 0
    10000.times { |i| s += i }
    results[idx] = s
  }
}
threads.each(&:join)
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
