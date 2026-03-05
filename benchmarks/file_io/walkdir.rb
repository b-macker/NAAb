require 'find'
home = ENV['HOME'] || '/tmp'
root = File.join(home, '.naab/language/src')
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
count = 0
Find.find(root) { |_| count += 1 }
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
puts(finish - start)
