// fib(30) with Ixo's dynamic model: the call goes through the module variable `fib`.
#include "dyn.hpp"
using namespace dyn;

Value globals[1];  // the module's variables: fib

static Value fib(Value* args) {
    Value n = args[0];
    if (lt(n, I(2))) return n;
    return add(call1(globals[0], sub(n, I(1))), call1(globals[0], sub(n, I(2))));
}

int main() {
    static Function f{3, 1, fib};
    globals[0] = O(&f);
    Value r = call1(globals[0], I(30));
    std::printf("%lld\n", (long long)r.i);
}
