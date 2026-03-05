package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"time"
)

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte("ok"))
	})
	listener, _ := net.Listen("tcp", "127.0.0.1:0")
	srv := &http.Server{Handler: mux}
	go srv.Serve(listener)
	addr := listener.Addr().String()

	start := time.Now()
	for i := 0; i < 100; i++ {
		resp, _ := http.Get("http://" + addr + "/")
		io.ReadAll(resp.Body)
		resp.Body.Close()
	}
	fmt.Println(time.Since(start).Microseconds())
	srv.Close()
}
