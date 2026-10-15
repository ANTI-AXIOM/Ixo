// Counting primes below 300,000 on every core: split, map in parallel, reduce.
use std::thread;

fn count_primes(lo: u64, hi: u64) -> u64 {
    (lo.max(2)..hi)
        .filter(|&x| {
            let mut d = 2;
            while d * d <= x && x % d != 0 {
                d += 1;
            }
            d * d > x
        })
        .count() as u64
}

fn main() {
    let step = 300_000 / 8;
    let total: u64 = thread::scope(|s| {
        let parts: Vec<_> = (0..8).map(|i| s.spawn(move || count_primes(i * step, (i + 1) * step))).collect();
        parts.into_iter().map(|p| p.join().unwrap()).sum()
    });
    println!("{}", total);
}
