// integer arithmetic in a loop
#include <cstdio>
#include <cstdint>

int main() {
    volatile int64_t n = 10000000;  // not a compile-time constant
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) total += (i * i + 3 * i) % 7;
    std::printf("%lld\n", (long long)total);
}
