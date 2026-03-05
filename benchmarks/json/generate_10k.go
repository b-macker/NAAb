package main

import (
	"encoding/json"
	"fmt"
	"time"
)

func main() {
	type Item struct {
		ID    int      `json:"id"`
		Name  string   `json:"name"`
		Tags  []string `json:"tags"`
		Value float64  `json:"value"`
	}
	items := make([]Item, 10000)
	for i := range items {
		items[i] = Item{i, fmt.Sprintf("item_%d", i), []string{"t0", "t1", "t2", "t3", "t4"}, float64(i) * 2.718}
	}
	start := time.Now()
	json.Marshal(items)
	fmt.Println(time.Since(start).Microseconds())
}
