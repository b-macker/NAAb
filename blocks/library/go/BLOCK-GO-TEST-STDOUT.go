package main

import "fmt"

func testStdout() string {
	fmt.Println("Hello from Go stdout!")
	fmt.Println("This should be visible if stdout fix works")
	return "Function returned successfully"
}

func printNumbers() string {
	fmt.Println("Counting: 1, 2, 3, 4, 5")
	for i := 1; i <= 5; i++ {
		fmt.Printf("Number: %d\n", i)
	}
	return "Done counting"
}

func testMath() int {
	x := 42
	y := 58
	result := x + y
	fmt.Printf("%d + %d = %d\n", x, y, result)
	return result
}

func main() {
	// Can be called as standalone program
	testStdout()
	printNumbers()
	testMath()
}
