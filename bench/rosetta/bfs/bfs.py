# Shortest path through a grid maze with breadth-first search.
from collections import deque

n = 400


def wall(x, y):
    return (x * 7 + y * 13 + x * y) % 9 == 4


dist = {(0, 0): 0}
queue = deque([(0, 0)])
while queue:
    x, y = queue.popleft()
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        p = (x + dx, y + dy)
        if 0 <= p[0] < n and 0 <= p[1] < n and not wall(*p) and p not in dist:
            dist[p] = dist[(x, y)] + 1
            queue.append(p)
print(dist[(n - 1, n - 1)], len(dist))
