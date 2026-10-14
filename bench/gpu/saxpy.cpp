// y = a * x + y over 16M floats: C++ -O2, one core
#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    const int n = 16000000;
    std::vector<float> x(n), y(n);
    for (int i = 0; i < n; i++) {
        x[i] = (float)(i % 1000);
        y[i] = 1.0f;
    }
    auto pass = [&] {
        for (int i = 0; i < n; i++) y[i] = 0.5f * x[i] + y[i];
    };
    pass();
    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < 10; r++) pass();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 10;
    std::printf("%g %g\ntime %f\n", y[999], y[n - 1], t);
}
