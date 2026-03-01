package main
import ("fmt"; "math"; "os"; "strconv")
func main() {
    if len(os.Args) < 2 { fmt.Println("READY"); return }
    val, _ := strconv.ParseFloat(os.Args[1], 64)
    v := math.Sqrt(math.Pow(val, 2) + 0.5)
    for i := 0; i < 100; i++ { v = math.Sqrt(v + 0.01) }
    fmt.Printf("%.15f", v)
}