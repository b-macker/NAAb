use std::time::Instant;
use std::net::{TcpListener, TcpStream};
use std::io::{Read, Write};
use std::thread;
fn main() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = listener.local_addr().unwrap();
    thread::spawn(move || {
        let (mut conn, _) = listener.accept().unwrap();
        let mut buf = [0u8; 4096];
        loop {
            match conn.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => { conn.write_all(&buf[..n]).unwrap(); },
                Err(_) => break,
            }
        }
    });
    let mut client = TcpStream::connect(addr).unwrap();
    let msg = br#"{"id":1,"data":"hello"}"#;
    let mut buf = [0u8; 4096];
    let start = Instant::now();
    for _ in 0..1000 {
        client.write_all(msg).unwrap();
        client.read(&mut buf).unwrap();
    }
    println!("{}", start.elapsed().as_micros());
}
