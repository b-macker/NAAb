use std::time::Instant;
fn main() {
    let start = Instant::now();
    let mut result = String::with_capacity(1024 * 1024);
    result.push('[');
    for i in 0..10000 {
        if i > 0 { result.push(','); }
        result.push_str(&format!(
            "{{\"id\":{},\"name\":\"item_{}\",\"tags\":[\"t0\",\"t1\",\"t2\",\"t3\",\"t4\"],\"value\":{:.3}}}",
            i, i, i as f64 * 2.718
        ));
    }
    result.push(']');
    println!("{}", start.elapsed().as_micros());
}
