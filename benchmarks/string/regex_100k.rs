use std::time::Instant;
fn main() {
    let lines: Vec<String> = (0..100000)
        .map(|i| format!("The quick brown fox jumps over the lazy dog #{}", i))
        .collect();
    let start = Instant::now();
    let mut count = 0usize;
    for line in &lines {
        let mut in_word = false;
        let mut word_len = 0;
        for c in line.chars() {
            if c.is_alphanumeric() || c == '_' {
                word_len += 1;
                in_word = true;
            } else {
                if in_word && word_len >= 5 { count += 1; }
                word_len = 0;
                in_word = false;
            }
        }
        if in_word && word_len >= 5 { count += 1; }
    }
    println!("{}", start.elapsed().as_micros());
}
