// A command-line tool: count the words given as arguments and print the most common (with clap).
use clap::Parser;

/// Count words and print the most common ones.
#[derive(Parser)]
struct Args {
    #[arg(required = true)]
    words: Vec<String>,
    #[arg(long, default_value_t = 5)]
    top: usize,
    #[arg(long)]
    upper: bool,
}

fn main() {
    let args = Args::parse();
    let mut counts: Vec<(String, usize)> = Vec::new();
    for w in &args.words {
        let w = if args.upper { w.to_uppercase() } else { w.clone() };
        match counts.iter_mut().find(|c| c.0 == w) {
            Some(c) => c.1 += 1,
            None => counts.push((w, 1)),
        }
    }
    let width = counts.iter().map(|c| c.0.len()).max().unwrap_or(0);
    counts.sort_by(|a, b| b.1.cmp(&a.1));
    for (w, n) in counts.iter().take(args.top) {
        println!("{:<width$} {:>3} {}", w, n, "#".repeat(*n));
    }
}
