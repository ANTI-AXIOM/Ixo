// functions passed around and called through a variable (std::function, the idiomatic way)
#include <cstdio>
#include <functional>

static long long apply_all(const std::function<long long(long long)>& f, long long n) {
    long long t = 0;
    for (long long i = 0; i < n; i++) t += f(i);
    return t;
}

static void run(long long n) {
    long long k = 3;
    std::function<long long(long long)> add_k = [k](long long x) { return x + k; };
    std::function<long long(long long)> sq = [](long long x) { return x * x % 1000; };
    std::printf("%lld %lld\n", apply_all(add_k, n), apply_all(sq, n));
}

int main() { run(5000000); }
