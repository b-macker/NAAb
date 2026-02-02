// Test block for Rust stdout verification

fn test_stdout() -> String {
    println!("Hello from Rust stdout!");
    println!("This should be visible if stdout fix works");
    "Function returned successfully".to_string()
}

fn print_numbers() -> String {
    println!("Counting: 1, 2, 3, 4, 5");
    for i in 1..=5 {
        println!("Number: {}", i);
    }
    "Done counting".to_string()
}

fn test_math() -> i32 {
    let x = 42;
    let y = 58;
    let result = x + y;
    println!("{} + {} = {}", x, y, result);
    result
}

fn main() {
    // Can be called as standalone program
    test_stdout();
    print_numbers();
    test_math();
}
