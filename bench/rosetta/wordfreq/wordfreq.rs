// The five most frequent words of a text.
use std::collections::HashMap;

fn main() {
    let words: Vec<&str> = "the quick brown fox jumps over the lazy dog and the cat sat on the mat"
        .split_whitespace()
        .collect();
    let text = (0..200_000).map(|i| words[i * 7 % words.len()]).collect::<Vec<_>>().join(" ");
    let mut counts: HashMap<&str, usize> = HashMap::new();
    for w in text.split_whitespace() {
        *counts.entry(w).or_insert(0) += 1;
    }
    let mut top: Vec<_> = counts.into_iter().collect();
    top.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(b.0)));
    for (w, n) in top.iter().take(5) {
        println!("{} {}", w, n);
    }
}
