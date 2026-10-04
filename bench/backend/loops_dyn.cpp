// integer arithmetic in a loop, with Ixo's dynamic model
#include "dyn.hpp"
using namespace dyn;

__attribute__((noinline)) static Value run(Value n) {
    Value total = I(0);
    for (Value i = I(0); lt(i, n); i = add(i, I(1)))
        total = add(total, mod(add(mul(i, i), mul(I(3), i)), I(7)));
    return total;
}

int main() {
    volatile int64_t n = 10000000;
    std::printf("%lld\n", (long long)run(I(n)).i);
}
