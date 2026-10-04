// map / filter / reduce pipelines and calling closures
#include <algorithm>
#include <cstdio>
#include <functional>
#include <numeric>
#include <vector>

static std::function<long long(long long)> compose(std::function<long long(long long)> f, std::function<long long(long long)> g) {
    return [f, g](long long x) { return g(f(x)); };
}

static void run(long long n) {
    std::vector<long long> nums(n);
    std::iota(nums.begin(), nums.end(), 0LL);
    std::vector<long long> tripled(n);
    std::transform(nums.begin(), nums.end(), tripled.begin(), [](long long x) { return x * 3; });
    std::vector<long long> evens;
    std::copy_if(tripled.begin(), tripled.end(), std::back_inserter(evens), [](long long x) { return x % 2 == 0; });
    long long s = std::accumulate(evens.begin(), evens.end(), 0LL);
    long long big = std::count_if(nums.begin(), nums.end(), [n](long long x) { return x > n / 2; });
    auto step = compose([](long long x) { return x + 1; }, [](long long x) { return x * 2; });
    long long t = 0;
    for (long long x : nums) t += step(x);
    std::printf("%zu %lld %lld %lld\n", evens.size(), s, big, t);
}

int main() { run(1000000); }
