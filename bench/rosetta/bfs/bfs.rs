// Shortest path through a grid maze with breadth-first search.
use std::collections::VecDeque;

fn main() {
    let n: i32 = 400;
    let wall = |x: i32, y: i32| (x * 7 + y * 13 + x * y) % 9 == 4;
    let mut dist = vec![-1i32; (n * n) as usize];
    let mut queue = VecDeque::from([(0, 0)]);
    dist[0] = 0;
    let mut reached = 1;
    while let Some((x, y)) = queue.pop_front() {
        for (dx, dy) in [(1, 0), (-1, 0), (0, 1), (0, -1)] {
            let (px, py) = (x + dx, y + dy);
            if px >= 0 && px < n && py >= 0 && py < n && !wall(px, py) && dist[(px * n + py) as usize] < 0 {
                dist[(px * n + py) as usize] = dist[(x * n + y) as usize] + 1;
                queue.push_back((px, py));
                reached += 1;
            }
        }
    }
    println!("{} {}", dist[(n * n - 1) as usize], reached);
}
