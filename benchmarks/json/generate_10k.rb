require 'json'
items = Array.new(10000) { |i| {id: i, name: "item_#{i}", tags: %w[t0 t1 t2 t3 t4], value: i * 2.718} }
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
result = JSON.generate(items)
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
