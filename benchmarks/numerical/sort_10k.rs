use std::time::Instant;
fn main() {
    let mut data: Vec<i32> = (0..10000).map(|i| (i * 7919 + 104729) % 1000000).collect();
    let start = Instant::now();
    data.sort();
    println!("{}", start.elapsed().as_micros());
}
