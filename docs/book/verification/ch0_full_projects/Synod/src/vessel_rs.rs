    use std::io::{self, Write};
    use std::fs;
    use std::thread;
    use std::time::Duration;

    // Fixed Point: 18 decimals
    const SCALE: u128 = 1_000_000_000_000_000_000;

    fn parse(s: &str) -> u128 {
        let parts: Vec<&str> = s.split('.').collect();
        let int_part: u128 = parts[0].parse().unwrap_or(0);
        let frac_part_str = if parts.len() > 1 { parts[1] } else { "" };
        let mut frac_padded = String::from(frac_part_str);
        while frac_padded.len() < 18 { frac_padded.push('0'); }
        let frac_part: u128 = frac_padded[..18].parse().unwrap_or(0);
        int_part * SCALE + frac_part
    }

    fn format(v: u128) -> String {
        let int_part = v / SCALE;
        let frac_part = v % SCALE;
        format!("{}.{:018}", int_part, frac_part)
    }

    fn main() {
        println!("[RS] Vessel Online. Polling SHM...");
        let shm_path = "/data/data/com.termux/files/usr/tmp/synod_shm_RS";

        loop {
            // Check for Work Order (Input File)
            let input_path = "/data/data/com.termux/files/usr/tmp/synod_in";
            if let Ok(data) = fs::read_to_string(input_path) {
                // Parse "OP A B"
                let parts: Vec<&str> = data.split_whitespace().collect();
                if parts.len() == 3 {
                    let a = parse(parts[1]);
                    let b = parse(parts[2]);
                    let res = match parts[0] {
                        "+" => a + b,
                        "-" => a - b,
                        "*" => (a * b) / SCALE,
                        "/" => (a * SCALE) / b,
                        _ => 0,
                    };

                    // Write Result to Output Slot
                    let out = format(res);
                    fs::write(shm_path, out).unwrap();
                }
            }
            thread::sleep(Duration::from_millis(5));
        }
    }
