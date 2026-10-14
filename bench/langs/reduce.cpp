// whole-list sums, minimums, maximums and searches (<numeric>/<algorithm>, the idiomatic way)
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <vector>

static void run(long long n) {
    std::vector<long long> xs;
    for (long long i = 0; i < n; i++) xs.push_back(i * 7919 % 1000003);
    std::vector<double> fs;
    for (long long i = 0; i < n; i++) fs.push_back(i * 0.5);
    long long t = 0;
    for (long long k = 0; k < 20; k++) {
        t += std::accumulate(xs.begin(), xs.end(), 0LL) + *std::max_element(xs.begin(), xs.end()) -
             *std::min_element(xs.begin(), xs.end());
        if (std::find(xs.begin(), xs.end(), -k) != xs.end()) t += 1;
    }
    double fsum = std::accumulate(fs.begin(), fs.end(), 0.0);
    long long at = std::find(xs.begin(), xs.end(), 999999) - xs.begin();
    std::printf("%lld %.1f %lld\n", t, fsum, at);
}

int main() { run(5000000); }
