package main

import (
	"fmt"
	"strconv"
	"strings"
	"time"
)

func main() {
	parts := make([]string, 100000)
	for i := range parts {
		parts[i] = strconv.Itoa(i)
	}
	start := time.Now()
	_ = strings.Join(parts, "")
	fmt.Println(time.Since(start).Microseconds())
}
