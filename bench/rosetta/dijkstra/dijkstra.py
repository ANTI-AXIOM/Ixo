# Shortest distances from node 0 in a weighted graph (Dijkstra with a binary heap).
import heapq

n = 20_000
edges = [[((u * 3 + 1) % n, u % 17 + 1), ((u + 7) % n, 5), ((u * u + 11) % n, u % 23 + 2)] for u in range(n)]
dist = {0: 0}
heap = [(0, 0)]
while heap:
    d, u = heapq.heappop(heap)
    if d > dist[u]:
        continue
    for v, w in edges[u]:
        if v not in dist or d + w < dist[v]:
            dist[v] = d + w
            heapq.heappush(heap, (d + w, v))
print(dist[n - 1], len(dist), sum(dist.values()))
