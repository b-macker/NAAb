use std::env;
fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 { println!("READY"); return; }
    let mut v: f64 = args[1].parse().unwrap();
    v = (v.powi(2) + 0.5).sqrt();
    for _ in 0..100 { v = (v + 0.01).sqrt(); }
    print!("{:.15}", v);
}