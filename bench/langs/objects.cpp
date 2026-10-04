// allocate many small objects, keep them in a list, call methods
// (heap-allocated like the Ixo and Python objects)
#include <cstdio>
#include <memory>
#include <vector>

struct Point {
    long x, y;
    Point(long x, long y) : x(x), y(y) {}
    long dist2(const Point& o) const { return (x - o.x) * (x - o.x) + (y - o.y) * (y - o.y); }
    std::unique_ptr<Point> moved(long dx, long dy) const { return std::make_unique<Point>(x + dx, y + dy); }
};

static void run(long n) {
    std::vector<std::unique_ptr<Point>> pts;
    for (long i = 0; i < n; i++) pts.push_back(std::make_unique<Point>(i % 100, i % 37));
    long total = 0;
    for (long i = 1; i < n; i++) total += pts[i]->dist2(*pts[i - 1]);
    auto p = std::make_unique<Point>(0, 0);
    for (long i = 0; i < n; i++) p = p->moved(1, 2);
    std::printf("%ld %ld %ld\n", total, p->x, p->y);
}

int main() { run(1000000); }
