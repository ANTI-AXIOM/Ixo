// Shortest distances from node 0 in a weighted graph (Dijkstra with a binary heap).
use std::cmp::Reverse;
use std::collections::BinaryHeap;

fn main() {
    let n: usize = 20_000;
    let edges: Vec<[(usize, i64); 3]> = (0..n)
        .map(|u| [((u * 3 + 1) % n, (u % 17 + 1) as i64), ((u + 7) % n, 5), ((u * u + 11) % n, (u % 23 + 2) as i64)])
        .collect();
    let mut dist: Vec<Option<i64>> = vec![None; n];
    let mut heap = BinaryHeap::from([Reverse((0i64, 0usize))]);
    dist[0] = Some(0);
    while let Some(Reverse((d, u))) = heap.pop() {
        if d > dist[u].unwrap() {
            continue;
        }
        for &(v, w) in &edges[u] {
            if dist[v].map_or(true, |dv| d + w < dv) {
                dist[v] = Some(d + w);
                heap.push(Reverse((d + w, v)));
            }
        }
    }
    let reached: Vec<i64> = dist.iter().flatten().copied().collect();
    println!("{} {} {}", dist[n - 1].unwrap(), reached.len(), reached.iter().sum::<i64>());
}
