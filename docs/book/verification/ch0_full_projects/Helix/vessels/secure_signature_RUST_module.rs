// Helix Vessel: " + func_name + " (RUST)
use std::env;

fn process(mut v: f64) -> f64  {
    for _ in 0..100  { v = (v * v + 0.01).sqrt();  }
    v
 }

fn main()  {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2  {
        println!("{{\"status\": \"READY\", \"target\": \"RUST\" }}");
        return;
     }
    let val: f64 = args[1].parse().unwrap();
    print!("{:.15}", process(val));
 }
