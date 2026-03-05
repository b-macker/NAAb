use std::time::Instant;
use std::process::Command;
fn main() {
    let start = Instant::now();
    for _ in 0..100 {
        Command::new("true").output().unwrap();
    }
    println!("{}", start.elapsed().as_micros());
}
