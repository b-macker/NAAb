package main

import (
	"fmt"
	"time"
)

func main() {
	ch := make(chan int, 100)
	done := make(chan int)

	go func() {
		for i := 0; i < 10000; i++ {
			ch <- i
		}
		close(ch)
	}()

	go func() {
		s := 0
		for v := range ch {
			s += v
		}
		done <- s
	}()

	start := time.Now()
	ch2 := make(chan int, 100)
	done2 := make(chan int)
	go func() {
		for i := 0; i < 10000; i++ {
			ch2 <- i
		}
		close(ch2)
	}()
	go func() {
		s := 0
		for v := range ch2 {
			s += v
		}
		done2 <- s
	}()
	<-done2
	fmt.Println(time.Since(start).Microseconds())
	<-done
}
