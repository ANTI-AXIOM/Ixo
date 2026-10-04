// prime sieve with Ixo's dynamic model: a list of 16-byte values, checked indexing
#include "dyn.hpp"
using namespace dyn;

__attribute__((noinline)) static Value run(Value n) {
    auto* l = new List{2, std::vector<Value>((size_t)add(n, I(1)).i, B(true))};
    Value flags = O(l);
    index(flags, I(0)) = B(false);
    index(flags, I(1)) = B(false);
    Value i = I(2);
    while (le(mul(i, i), n)) {
        if (truthy(index(flags, i))) {
            Value j = mul(i, i);
            while (le(j, n)) {
                index(flags, j) = B(false);
                j = add(j, i);
            }
        }
        i = add(i, I(1));
    }
    Value count = I(0);
    for (Value& f : l->items)
        if (truthy(f)) count = add(count, I(1));
    return count;
}

int main() {
    volatile int64_t n = 2000000;
    std::printf("%lld\n", (long long)run(I(n)).i);
}
