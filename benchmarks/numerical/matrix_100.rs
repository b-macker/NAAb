use std::time::Instant;
fn main() {
    const N: usize = 100;
    let mut a = [[0.0f64; N]; N];
    let mut b = [[0.0f64; N]; N];
    let mut c = [[0.0f64; N]; N];
    for i in 0..N {
        for j in 0..N {
            a[i][j] = ((i*7 + j*13) % 97) as f64;
            b[i][j] = ((i*11 + j*17) % 89) as f64;
        }
    }
    let start = Instant::now();
    for i in 0..N {
        for j in 0..N {
            let mut sum = 0.0;
            for k in 0..N { sum += a[i][k] * b[k][j]; }
            c[i][j] = sum;
        }
    }
    println!("{}", start.elapsed().as_micros());
}
