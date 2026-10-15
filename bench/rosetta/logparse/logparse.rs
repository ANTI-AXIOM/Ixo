// Requests per status code and the three slowest endpoints, from access log lines.
use std::collections::BTreeMap;

fn main() {
    let paths = ["/", "/login", "/api/items", "/api/orders", "/static/app.js"];
    let codes = [200, 200, 404, 500, 302];
    let log: Vec<String> = (0..50_000)
        .map(|i| format!("10.0.0.{} GET {} {} {}ms", i % 50, paths[i % 5], codes[i * 3 % 5], i * 37 % 900))
        .collect();
    let mut by_status: BTreeMap<String, u32> = BTreeMap::new();
    let mut total: BTreeMap<String, (u64, u64)> = BTreeMap::new();
    for line in &log {
        let f: Vec<&str> = line.split_whitespace().collect();
        *by_status.entry(f[3].to_string()).or_insert(0) += 1;
        let e = total.entry(f[2].to_string()).or_insert((0, 0));
        e.0 += f[4].trim_end_matches("ms").parse::<u64>().unwrap();
        e.1 += 1;
    }
    for (s, n) in &by_status {
        println!("{} {}", s, n);
    }
    let mut avg: Vec<(String, f64)> = total.into_iter().map(|(p, (t, n))| (p, t as f64 / n as f64)).collect();
    avg.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap());
    for (p, a) in avg.iter().take(3) {
        println!("{} {:.1}", p, a);
    }
}
