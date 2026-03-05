use std::time::Instant;
use std::sync::mpsc;
use std::thread;
fn main() {
    let start = Instant::now();
    let (tx, rx) = mpsc::channel();
    let producer = thread::spawn(move || {
        for i in 0..10000 {
            tx.send(i).unwrap();
        }
    });
    let consumer = thread::spawn(move || {
        let mut sum: i64 = 0;
        for val in rx {
            sum += val;
        }
        sum
    });
    producer.join().unwrap();
    let _ = consumer.join().unwrap();
    println!("{}", start.elapsed().as_micros());
}
