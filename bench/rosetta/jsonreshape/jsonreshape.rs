// Orders as JSON to a per-customer summary as JSON (with serde_json).
use serde_json::{json, Value};
use std::collections::BTreeMap;

fn main() {
    let orders: Vec<Value> = (0..5000)
        .map(|i| {
            let items: Vec<Value> = (0..i % 4 + 1).map(|j| json!({"sku": format!("s{}", j), "qty": j + 1})).collect();
            json!({"id": i, "customer": format!("c{}", i % 37), "items": items})
        })
        .collect();
    let text = serde_json::to_string(&orders).unwrap();
    let mut summary: BTreeMap<String, (i64, i64)> = BTreeMap::new();
    for o in serde_json::from_str::<Vec<Value>>(&text).unwrap() {
        let s = summary.entry(o["customer"].as_str().unwrap().to_string()).or_default();
        s.0 += 1;
        s.1 += o["items"].as_array().unwrap().iter().map(|item| item["qty"].as_i64().unwrap()).sum::<i64>();
    }
    let out: Vec<Value> = summary.iter().map(|(c, s)| json!({"customer": c, "orders": s.0, "units": s.1})).collect();
    let out = serde_json::to_string(&out).unwrap();
    println!("{} {}", out.len(), &out[..120]);
}
