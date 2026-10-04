#include <cstdio>

static long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main() { std::printf("%ld\n", fib(30)); }
