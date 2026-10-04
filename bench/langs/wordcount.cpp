// hash map counting with string keys
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

static void run(long n) {
    std::unordered_map<std::string, long> counts;
    int64_t x = 42;
    for (long i = 0; i < n; i++) {
        x = (x * 1103515245 + 12345) % 2147483648LL;
        counts["k" + std::to_string(x % 5000)] += 1;
    }
    long best = 0;
    for (auto& [k, v] : counts) best = std::max(best, v);
    std::printf("%zu %ld %ld\n", counts.size(), best, counts["k7"]);
}

int main() { run(1000000); }
