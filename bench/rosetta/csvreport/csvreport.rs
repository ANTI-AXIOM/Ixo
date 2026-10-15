// Sales by region from CSV text: totals, the best product of each region, sorted.
use std::collections::BTreeMap;

fn main() {
    let names = ["north", "south", "east", "west"];
    let mut text = String::from("region,product,units,price\n");
    for i in 0..20_000 {
        text += &format!("{},p{},{},{}\n", names[i % 4], i % 13, i % 7 + 1, (i % 11) as f64 + 0.5);
    }
    let mut regions: BTreeMap<&str, Vec<(&str, i64, f64)>> = BTreeMap::new();
    for line in text.lines().skip(1) {
        let f: Vec<&str> = line.split(',').collect();
        regions.entry(f[0]).or_default().push((f[1], f[2].parse().unwrap(), f[3].parse().unwrap()));
    }
    for (region, sales) in &regions {
        let total: f64 = sales.iter().map(|s| s.1 as f64 * s.2).sum();
        let mut units: Vec<(&str, i64)> = Vec::new(); // first-seen order
        for s in sales {
            match units.iter_mut().find(|u| u.0 == s.0) {
                Some(u) => u.1 += s.1,
                None => units.push((s.0, s.1)),
            }
        }
        let best = units.iter().fold(units[0], |b, u| if u.1 > b.1 { *u } else { b });
        println!("{} {:.2} {}", region, total, best.0);
    }
}
