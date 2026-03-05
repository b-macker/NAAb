package main

import (
	"fmt"
	"regexp"
	"time"
)

func main() {
	lines := make([]string, 100000)
	for i := range lines {
		lines[i] = fmt.Sprintf("The quick brown fox jumps over the lazy dog #%d", i)
	}
	pattern := regexp.MustCompile(`\b\w{5,}\b`)
	start := time.Now()
	count := 0
	for _, line := range lines {
		count += len(pattern.FindAllString(line, -1))
	}
	fmt.Println(time.Since(start).Microseconds())
}
