package main

import (
	"encoding/json"
	"fmt"
	"time"
)

type Item struct {
	ID    int     `json:"id"`
	Name  string  `json:"name"`
	Value float64 `json:"value"`
}

func main() {
	items := make([]Item, 20000)
	for i := range items {
		items[i] = Item{ID: i, Name: fmt.Sprintf("item_%d", i), Value: float64(i) * 3.14}
	}
	data, _ := json.Marshal(items)
	start := time.Now()
	var parsed []Item
	json.Unmarshal(data, &parsed)
	fmt.Println(time.Since(start).Microseconds())
}
