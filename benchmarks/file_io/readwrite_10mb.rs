use std::time::Instant;
use std::fs;
use std::io::Write;
fn main() {
    let data = "x".repeat(10 * 1024 * 1024);
    let tmp = format!("{}/bench_{}.tmp", std::env::temp_dir().display(), std::process::id());
    let start = Instant::now();
    {
        let mut f = fs::File::create(&tmp).unwrap();
        f.write_all(data.as_bytes()).unwrap();
    }
    let _ = fs::read_to_string(&tmp).unwrap();
    println!("{}", start.elapsed().as_micros());
    let _ = fs::remove_file(&tmp);
}
