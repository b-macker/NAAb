package main

import (
	"fmt"
	"time"
)

func main() {
	const N = 100
	var A, B, C [N][N]float64
	for i := 0; i < N; i++ {
		for j := 0; j < N; j++ {
			A[i][j] = float64((i*7 + j*13) % 97)
			B[i][j] = float64((i*11 + j*17) % 89)
		}
	}
	start := time.Now()
	for i := 0; i < N; i++ {
		for j := 0; j < N; j++ {
			var sum float64
			for k := 0; k < N; k++ {
				sum += A[i][k] * B[k][j]
			}
			C[i][j] = sum
		}
	}
	_ = C
	fmt.Println(time.Since(start).Microseconds())
}
