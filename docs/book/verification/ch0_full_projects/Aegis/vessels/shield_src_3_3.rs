    use std::io::{self, Read};
    use std::time::Instant;

    fn bubble_sort(arr: &mut [i32]) {
        let n = arr.len();
        for i in 0..n {
            for j in 0..n - 1 - i {
                if arr[j] > arr[j + 1] {
                    arr.swap(j, j + 1);
                }
            }
        }
    }

    fn quick_sort(arr: &mut [i32]) {
        if arr.len() <= 1 { return; }
        let pivot = arr.len() - 1;
        let mut i = 0;
        for j in 0..pivot {
            if arr[j] <= arr[pivot] {
                arr.swap(i, j);
                i += 1;
            }
        }
        arr.swap(i, pivot);
        let (left, right) = arr.split_at_mut(i);
        quick_sort(left);
        quick_sort(&mut right[1..]);
    }

    fn insertion_sort(arr: &mut [i32]) {
        for i in 1..arr.len() {
            let mut j = i;
            while j > 0 && arr[j - 1] > arr[j] {
                arr.swap(j - 1, j);
                j -= 1;
            }
        }
    }

    fn main() {
        let algo_type = 0;

        let mut buffer = String::new();
        io::stdin().read_to_string(&mut buffer).unwrap();

        let mut nums: Vec<i32> = buffer.split_whitespace()
            .filter_map(|s| s.parse().ok())
            .collect();

        let start = Instant::now();

        match algo_type {
            0 => bubble_sort(&mut nums),
            1 => quick_sort(&mut nums),
            _ => insertion_sort(&mut nums),
        }

        let duration = start.elapsed();
        println!("{{\"algo\": {}, \"time_ns\": {}, \"count\": {}}}", algo_type, duration.as_nanos(), nums.len());
    }
