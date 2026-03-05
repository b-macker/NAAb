package main

import (
	"fmt"
	"sync"
	"time"
)

func main() {
	var wg sync.WaitGroup
	results := make([]int, 100)
	start := time.Now()
	for idx := 0; idx < 100; idx++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			s := 0
			for j := 0; j < 10000; j++ {
				s += j
			}
			results[i] = s
		}(idx)
	}
	wg.Wait()
	fmt.Println(time.Since(start).Microseconds())
}
