package main

import (
	"fmt"
	"io"
	"net"
	"time"
)

func main() {
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	addr := ln.Addr().String()
	go func() {
		conn, _ := ln.Accept()
		io.Copy(conn, conn)
		conn.Close()
	}()
	conn, _ := net.Dial("tcp", addr)
	msg := []byte(`{"id":1,"data":"hello"}`)
	buf := make([]byte, 4096)
	start := time.Now()
	for i := 0; i < 1000; i++ {
		conn.Write(msg)
		conn.Read(buf)
	}
	fmt.Println(time.Since(start).Microseconds())
	conn.Close()
	ln.Close()
}
