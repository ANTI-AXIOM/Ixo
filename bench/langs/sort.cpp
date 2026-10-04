// sorting numbers, sorting objects by a key, custom comparator
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct Item { long key; std::string name; };

static void run(long n) {
    int64_t x = 7;
    std::vector<long> nums;
    for (long i = 0; i < n; i++) {
        x = (x * 1103515245 + 12345) % 2147483648LL;
        nums.push_back((long)(x % 1000000));
    }
    std::vector<long> a = nums;
    std::sort(a.begin(), a.end());
    std::vector<std::unique_ptr<Item>> b;
    for (long i = 0; i < n / 4; i++) b.push_back(std::make_unique<Item>(Item{nums[i], "i"}));
    std::stable_sort(b.begin(), b.end(), [](auto& p, auto& q) { return p->key < q->key; });
    std::vector<long> c(nums.begin(), nums.begin() + n / 4);
    std::sort(c.begin(), c.end(), [](long p, long q) { return p > q; });
    std::printf("%ld %ld %ld %ld %ld %ld\n", a[0], a[n / 2], a[n - 1], b[0]->key, b.back()->key, c[0]);
}

int main() { run(500000); }
