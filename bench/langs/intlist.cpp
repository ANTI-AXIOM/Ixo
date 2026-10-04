// a big list of ints kept in memory
#include <cstdio>
#include <vector>

static void run(long long n) {
    std::vector<long long> xs;
    for (long long i = 0; i < n; i++) xs.push_back(i * 7919 % 1000003);
    long long total = 0, big = 0;
    for (long long x : xs) {
        total += x;
        if (x > big) big = x;
    }
    std::printf("%zu %lld %lld\n", xs.size(), total, big);
}

int main() { run(5000000); }
