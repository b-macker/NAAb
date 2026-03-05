require 'tempfile'
data = "x" * (10 * 1024 * 1024)
tmp = Tempfile.new('bench')
path = tmp.path
tmp.close
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
File.write(path, data)
File.read(path)
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
File.delete(path) rescue nil
puts(finish - start)
