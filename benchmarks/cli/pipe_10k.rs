use std::time::Instant;
fn main() {
    let lines: Vec<String> = (0..10000)
        .map(|i| format!("line {}: the quick brown fox", i))
        .collect();
    let start = Instant::now();
    let _upper: Vec<String> = lines.iter().map(|l| l.to_uppercase()).collect();
    println!("{}", start.elapsed().as_micros());
}
