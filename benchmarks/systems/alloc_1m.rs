use std::time::Instant;
fn main() {
    let start = Instant::now();
    let mut objs: Vec<(i32, [i32; 10])> = Vec::with_capacity(1000000);
    for i in 0..1000000 {
        objs.push((i, [0; 10]));
    }
    drop(objs);
    println!("{}", start.elapsed().as_micros());
}
