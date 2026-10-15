// Shortest distances from node 0 in a weighted graph (Dijkstra with a binary heap).
#include <cstdio>
#include <functional>
#include <queue>
#include <vector>

int main() {
    const int n = 20000;
    std::vector<std::vector<std::pair<int, int>>> edges(n);
    for (int u = 0; u < n; u++)
        edges[u] = {{(u * 3 + 1) % n, u % 17 + 1}, {(u + 7) % n, 5}, {(u * u + 11) % n, u % 23 + 2}};
    std::vector<long long> dist(n, -1);
    using Item = std::pair<long long, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    dist[0] = 0;
    heap.push({0, 0});
    while (!heap.empty()) {
        auto [d, u] = heap.top();
        heap.pop();
        if (d > dist[u]) continue;
        for (auto [v, w] : edges[u])
            if (dist[v] < 0 || d + w < dist[v]) {
                dist[v] = d + w;
                heap.push({d + w, v});
            }
    }
    long long reached = 0, total = 0;
    for (long long d : dist)
        if (d >= 0) reached++, total += d;
    std::printf("%lld %lld %lld\n", dist[n - 1], reached, total);
}
