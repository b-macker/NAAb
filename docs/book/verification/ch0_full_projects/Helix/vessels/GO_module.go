// Helix Vessel: process_ledger (GO)
package main
import (
    "fmt"
    "math"
    "os"
    "strconv"
)

func process(val float64) float64 {
    v := val
    for i := 0; i < 100; i++ {
        v = math.Sqrt(v*v + 0.01)
    }
    return v
}

func main() {
    if len(os.Args) < 2 {
        fmt.Println("{\"status\": \"READY\", \"target\": \"GO\"}")
        return
    }
    val, _ := strconv.ParseFloat(os.Args[1], 64)
    fmt.Printf("%.15f", process(val))
}
