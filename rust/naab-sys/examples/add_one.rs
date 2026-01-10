// Example NAAb block: add_one
// Adds 1 to the input number

use naab_sys::prelude::*;

// Simple block that adds 1 to a number
naab_block!(add_one, |args: Vec<Value>| {
    let num = get_int_arg!(args, 0, 0);
    Value::Int(num + 1)
});

// Block that concatenates strings
naab_block!(concat_strings, |args: Vec<Value>| {
    let a = get_string_arg!(args, 0, "");
    let b = get_string_arg!(args, 1, "");
    Value::String(format!("{}{}", a, b))
});

// Block that multiplies two doubles
naab_block!(multiply, |args: Vec<Value>| {
    let a = get_double_arg!(args, 0, 1.0);
    let b = get_double_arg!(args, 1, 1.0);
    Value::Double(a * b)
});

// Block that returns a boolean
naab_block!(is_positive, |args: Vec<Value>| {
    let num = get_int_arg!(args, 0, 0);
    Value::Bool(num > 0)
});

// Block with no arguments
naab_block!(hello_world, |_args: Vec<Value>| {
    Value::String("Hello from Rust!".to_string())
});

// For testing purposes when compiled as binary
#[cfg(not(test))]
fn main() {
    println!("This is a NAAb block library. Build with:");
    println!("  cargo build --example add_one --release");
    println!("");
    println!("Then use from NAAb:");
    println!("  let result = call(\"rust://./target/release/examples/libadd_one.so::add_one\", 41)");
    println!("  print(result)  // 42");
}
