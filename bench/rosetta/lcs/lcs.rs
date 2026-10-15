// Length of the longest common subsequence of two DNA strings (dynamic programming).
fn dna(mut x: u64, n: usize) -> Vec<u8> {
    (0..n)
        .map(|_| {
            x = (x * 1103515245 + 12345) % 2_147_483_648;
            b"ACGT"[((x >> 16) & 3) as usize]
        })
        .collect()
}

fn main() {
    let (a, b) = (dna(1, 1500), dna(2, 1500));
    let mut prev = vec![0; b.len() + 1];
    for &ca in &a {
        let mut row = vec![0; b.len() + 1];
        for (j, &cb) in b.iter().enumerate() {
            row[j + 1] = if ca == cb { prev[j] + 1 } else { prev[j + 1].max(row[j]) };
        }
        prev = row;
    }
    println!("{}", prev[b.len()]);
}
