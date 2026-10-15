// Counting primes below 300,000 on every core: split, map in parallel, reduce.
#include <algorithm>
#include <cstdio>
#include <future>
#include <vector>

int count_primes(int lo, int hi) {
    int n = 0;
    for (int x = std::max(lo, 2); x < hi; x++) {
        int d = 2;
        while (d * d <= x && x % d != 0) d++;
        if (d * d > x) n++;
    }
    return n;
}

int main() {
    const int step = 300000 / 8;
    std::vector<std::future<int>> parts;
    for (int i = 0; i < 8; i++) parts.push_back(std::async(std::launch::async, count_primes, i * step, (i + 1) * step));
    int total = 0;
    for (auto& p : parts) total += p.get();
    std::printf("%d\n", total);
}
