package main

import (
	"encoding/csv"
	"fmt"
	"strings"
	"time"
)

func main() {
	var sb strings.Builder
	for i := 0; i < 100000; i++ {
		status := "inactive"
		if i%2 == 0 {
			status = "active"
		}
		fmt.Fprintf(&sb, "%d,name_%d,%.2f,%s\n", i, i, float64(i)*3.14, status)
	}
	data := sb.String()
	start := time.Now()
	r := csv.NewReader(strings.NewReader(data))
	records, _ := r.ReadAll()
	_ = records
	fmt.Println(time.Since(start).Microseconds())
}
