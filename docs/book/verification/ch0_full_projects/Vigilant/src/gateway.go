package main
import (
    "net/http"
    "io"
    "net"
    "sync/atomic"
    "log"
    "time"
)

var shards = []string{
    "/data/data/com.termux/files/usr/tmp/v_brain.sock",
}
var counter uint64

func handle(w http.ResponseWriter, r *http.Request) {
    idx := atomic.AddUint64(&counter, 1) % uint64(len(shards))
    sock := shards[idx]

    // Dial with a short timeout to prevent hangs
    conn, err := net.DialTimeout("unix", sock, 2*time.Second)
    if err != nil {
        http.Error(w, "Security Fabric Offline", 503)
        return
    }
    defer conn.Close()

    body, _ := io.ReadAll(r.Body)
    conn.Write(body)

    // Signal EOF to the brain
    if cw, ok := conn.(*net.UnixConn); ok {
        cw.CloseWrite()
    }

    resp, _ := io.ReadAll(conn)
    w.Header().Set("Content-Type", "application/json")
    w.Write(resp)
}

func main() {
    log.Println("[GATEWAY] Listening on :8091...")
    http.HandleFunc("/", handle)
    log.Fatal(http.ListenAndServe(":8091", nil))
}
