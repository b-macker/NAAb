// Vigilant/scanner/shield.rs
// PHASE 1 (v3.0): IRON-CLAD DETERMINISTIC SHIELD (FIXED)

use std::io::{Read, Write};
use std::os::unix::net::UnixListener;
use std::fs;

#[derive(Debug)]
struct Finding {
    pii_type: String,
}

fn normalize_text(input: &str) -> String {
    input.chars()
        .filter(|c| {
            !('\u{200B}'..='\u{200F}').contains(c) && 
            !('\u{feff}'..='\u{feff}').contains(c)
        })
        .map(|c| c.to_ascii_lowercase())
        .collect()
}

fn luhn_check(cc_number: &str) -> bool {
    let digits: Vec<u32> = cc_number.chars().filter_map(|c| c.to_digit(10)).collect();
    if digits.len() < 13 || digits.len() > 19 { return false; }
    let sum: u32 = digits.iter().rev().enumerate().map(|(i, &d)| {
        if i % 2 == 1 {
            let double = d * 2;
            if double > 9 { double - 9 } else { double }
        } else { d }
    }).sum();
    sum % 10 == 0
}

fn scan_pii(raw_text: &str) -> Vec<Finding> {
    let mut findings = Vec::new();
    let text = normalize_text(raw_text);
    
    // 1. IMPROVED SSN SCAN: Strip noise first
    let only_digits: String = text.chars().filter(|c| c.is_ascii_digit()).collect();
    // Catch exactly 9 digits anywhere in the stream
    if only_digits.len() >= 9 {
        findings.push(Finding { pii_type: "ID_SSN".to_string() });
    }

    // 2. CREDIT CARDS (Luhn)
    let mut i = 0;
    let bytes = text.as_bytes();
    while i < bytes.len() {
        if bytes[i].is_ascii_digit() {
            let mut j = i;
            let mut raw = String::new();
            while j < bytes.len() && (bytes[j].is_ascii_digit() || bytes[j] == b'-' || bytes[j] == b' ' || bytes[j] == b'.') {
                if bytes[j].is_ascii_digit() { raw.push(bytes[j] as char); }
                j += 1;
            }
            if raw.len() >= 13 && raw.len() <= 19 && luhn_check(&raw) {
                findings.push(Finding { pii_type: "FIN_CREDIT_CARD".to_string() });
                i = j; continue;
            }
        }
        i += 1;
    }

    findings
}

fn main() {
    let socket_path = "/data/data/com.termux/files/usr/tmp/v_s.sock";
    let _ = fs::remove_file(socket_path);
    
    let listener = UnixListener::bind(socket_path).expect("SHIELD_UDS_BIND_FAIL");

    for stream in listener.incoming() {
        if let Ok(mut stream) = stream {
            let mut buffer = String::new();
            if let Ok(_) = stream.read_to_string(&mut buffer) {
                let findings = scan_pii(&buffer);
                let response = format!("[{}]", findings.iter()
                    .map(|f| format!("{{\"type\":\"{}\"}}", f.pii_type))
                    .collect::<Vec<_>>().join(","));
                let _ = stream.write_all(response.as_bytes());
            }
        }
    }
}
