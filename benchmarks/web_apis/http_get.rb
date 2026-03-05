require 'socket'
require 'net/http'
# Start minimal TCP server
server = TCPServer.new('127.0.0.1', 0)
port = server.addr[1]
t = Thread.new {
  100.times do
    client = server.accept
    client.gets
    client.print "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
    client.close
  end
}
start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
100.times do
  Net::HTTP.get(URI("http://127.0.0.1:#{port}/"))
end
finish = Process.clock_gettime(Process::CLOCK_MONOTONIC, :microsecond)
t.join(2)
server.close rescue nil
puts(finish - start)
