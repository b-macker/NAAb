require 'json'
data = JSON.generate(Array.new(20000) { |i| {id: i, name: "item_#{i}", value: i * 3.14} })
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
parsed = JSON.parse(data)
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
