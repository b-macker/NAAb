package main

import (
	"fmt"
	"strings"
	"time"
)

func main() {
	lines := make([]string, 10000)
	for i := range lines {
		lines[i] = fmt.Sprintf("line %d: the quick brown fox", i)
	}
	start := time.Now()
	for i := range lines {
		lines[i] = strings.ToUpper(lines[i])
	}
	fmt.Println(time.Since(start).Microseconds())
}
