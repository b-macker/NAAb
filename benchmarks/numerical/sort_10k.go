package main

import (
	"fmt"
	"math/rand"
	"sort"
	"time"
)

func main() {
	data := make([]int, 10000)
	for i := range data {
		data[i] = rand.Intn(1000000)
	}
	start := time.Now()
	sort.Ints(data)
	fmt.Println(time.Since(start).Microseconds())
}
