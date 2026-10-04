// prime sieve: list writes and reads
#include <cstdio>
#include <vector>

int main() {
    long n = 2000000;
    std::vector<bool> flags(n + 1, true);
    flags[0] = flags[1] = false;
    for (long i = 2; i * i <= n; i++)
        if (flags[i])
            for (long j = i * i; j <= n; j += i) flags[j] = false;
    long count = 0;
    for (bool f : flags) count += f;
    std::printf("%ld\n", count);
}
