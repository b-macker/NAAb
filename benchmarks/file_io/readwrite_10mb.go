package main

import (
	"fmt"
	"os"
	"strings"
	"time"
)

func main() {
	data := strings.Repeat("x", 10*1024*1024)
	tmp, _ := os.CreateTemp("", "bench*.tmp")
	name := tmp.Name()
	tmp.Close()
	start := time.Now()
	os.WriteFile(name, []byte(data), 0644)
	os.ReadFile(name)
	fmt.Println(time.Since(start).Microseconds())
	os.Remove(name)
}
