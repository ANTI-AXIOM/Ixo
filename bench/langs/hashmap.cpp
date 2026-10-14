// counting with int keys (std::unordered_map, the idiomatic way)
#include <cstdio>
#include <unordered_map>

static void run(long long n) {
    std::unordered_map<long long, long long> counts;
    for (long long i = 0; i < n; i++) counts[(i * 7919) % 100003]++;
    std::printf("%zu %lld %lld\n", counts.size(), counts[0], counts[5]);
}

int main() { run(3000000); }
