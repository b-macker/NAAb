package main

import (
	"fmt"
	"time"
)

type Obj struct {
	ID   int
	Data [10]int
}

func main() {
	start := time.Now()
	objs := make([]Obj, 1000000)
	for i := range objs {
		objs[i].ID = i
	}
	objs = nil
	_ = objs
	fmt.Println(time.Since(start).Microseconds())
}
