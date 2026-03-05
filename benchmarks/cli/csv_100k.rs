use std::time::Instant;
fn main() {
    let mut data = String::with_capacity(4 * 1024 * 1024);
    for i in 0..100000 {
        let status = if i % 2 == 0 { "active" } else { "inactive" };
        data.push_str(&format!("{},name_{},{:.2},{}\n", i, i, i as f64 * 3.14, status));
    }
    let start = Instant::now();
    let rows: Vec<Vec<&str>> = data.lines().map(|l| l.split(',').collect()).collect();
    let _ = rows.len();
    println!("{}", start.elapsed().as_micros());
}
