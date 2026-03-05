use std::time::Instant;
fn main() {
    let mut items = String::from("[");
    for i in 0..20000 {
        if i > 0 { items.push(','); }
        items.push_str(&format!("{{\"id\":{},\"name\":\"item_{}\",\"value\":{:.2}}}", i, i, i as f64 * 3.14));
    }
    items.push(']');
    let start = Instant::now();
    // Simple JSON tokenizer benchmark (no serde dependency)
    let mut depth = 0i32;
    let mut in_string = false;
    for c in items.chars() {
        match c {
            '"' => in_string = !in_string,
            '{' | '[' if !in_string => depth += 1,
            '}' | ']' if !in_string => depth -= 1,
            _ => {}
        }
    }
    println!("{}", start.elapsed().as_micros());
}
