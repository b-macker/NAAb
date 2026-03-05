package main

import (
	"fmt"
	"os"
	"path/filepath"
	"time"
)

func main() {
	home, _ := os.UserHomeDir()
	root := filepath.Join(home, ".naab/language/src")
	start := time.Now()
	count := 0
	filepath.Walk(root, func(_ string, _ os.FileInfo, _ error) error {
		count++
		return nil
	})
	fmt.Println(time.Since(start).Microseconds())
}
