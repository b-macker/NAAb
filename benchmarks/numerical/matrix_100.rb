N = 100
a = Array.new(N) { |i| Array.new(N) { |j| ((i*7+j*13) % 97).to_f } }
b = Array.new(N) { |i| Array.new(N) { |j| ((i*11+j*17) % 89).to_f } }
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
c = Array.new(N) { |i|
  Array.new(N) { |j|
    sum = 0.0
    N.times { |k| sum += a[i][k] * b[k][j] }
    sum
  }
}
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
