package main

import (
	"fmt"
	"os/exec"
	"time"
)

func main() {
	start := time.Now()
	for i := 0; i < 100; i++ {
		cmd := exec.Command("true")
		cmd.Run()
	}
	fmt.Println(time.Since(start).Microseconds())
}
