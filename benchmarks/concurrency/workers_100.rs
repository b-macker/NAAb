use std::time::Instant;
use std::thread;
fn main() {
    let start = Instant::now();
    let handles: Vec<_> = (0..100).map(|_| {
        thread::spawn(|| {
            let mut s: i64 = 0;
            for i in 0..10000 { s += i; }
            s
        })
    }).collect();
    let _results: Vec<_> = handles.into_iter().map(|h| h.join().unwrap()).collect();
    println!("{}", start.elapsed().as_micros());
}
