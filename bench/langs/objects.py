# allocate many small objects, keep them in a list, call methods
class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def dist2(self, o):
        return (self.x - o.x) * (self.x - o.x) + (self.y - o.y) * (self.y - o.y)

    def moved(self, dx, dy):
        return Point(self.x + dx, self.y + dy)


def run(n):
    pts = []
    for i in range(n):
        pts.append(Point(i % 100, i % 37))
    total = 0
    for i in range(1, n):
        total += pts[i].dist2(pts[i - 1])
    p = Point(0, 0)
    for i in range(n):
        p = p.moved(1, 2)
    print(total, p.x, p.y)


run(1_000_000)
