use std::time::Instant;
fn main() {
    let parts: Vec<String> = (0..100000).map(|i| i.to_string()).collect();
    let start = Instant::now();
    let _result: String = parts.join("");
    println!("{}", start.elapsed().as_micros());
}
