// Shortest path through a grid maze with breadth-first search.
#include <cstdio>
#include <queue>
#include <utility>
#include <vector>

int main() {
    const int n = 400;
    auto wall = [](int x, int y) { return (x * 7 + y * 13 + x * y) % 9 == 4; };
    std::vector<int> dist(n * n, -1);
    std::queue<std::pair<int, int>> queue;
    dist[0] = 0;
    queue.push({0, 0});
    int reached = 1;
    const int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
    while (!queue.empty()) {
        auto [x, y] = queue.front();
        queue.pop();
        for (int k = 0; k < 4; k++) {
            int px = x + dx[k], py = y + dy[k];
            if (px >= 0 && px < n && py >= 0 && py < n && !wall(px, py) && dist[px * n + py] < 0) {
                dist[px * n + py] = dist[x * n + y] + 1;
                queue.push({px, py});
                reached++;
            }
        }
    }
    std::printf("%d %d\n", dist[n * n - 1], reached);
}
