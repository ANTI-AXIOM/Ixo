// a big map with string keys
#include <cstdio>
#include <string>
#include <unordered_map>

static void run(long n) {
    std::unordered_map<std::string, long> m;
    for (long i = 0; i < n; i++) m["key" + std::to_string(i)] = i;
    long hits = 0;
    for (long i = 0; i < n; i += 3)
        if (m.count("key" + std::to_string(i))) hits++;
    std::printf("%zu %ld %ld\n", m.size(), hits, m["key12345"]);
}

int main() { run(500000); }
