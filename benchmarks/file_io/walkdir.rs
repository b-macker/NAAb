use std::time::Instant;
use std::fs;
use std::path::PathBuf;
fn walk(dir: &PathBuf) -> usize {
    let mut count = 0;
    if let Ok(entries) = fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() { count += walk(&path); }
            else { count += 1; }
        }
    }
    count
}
fn main() {
    let home = std::env::var("HOME").unwrap_or("/tmp".to_string());
    let root = PathBuf::from(format!("{}/.naab/language/src", home));
    let start = Instant::now();
    let _ = walk(&root);
    println!("{}", start.elapsed().as_micros());
}
