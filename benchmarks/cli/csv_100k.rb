require 'csv'
data = Array.new(100000) { |i| "#{i},name_#{i},#{(i*3.14).round(2)},#{i.even? ? 'active' : 'inactive'}" }.join("\n")
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
rows = CSV.parse(data)
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
