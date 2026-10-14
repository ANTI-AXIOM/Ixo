// C = A * B for 512 x 512 floats: C++ -O2, the plain triple loop, one core
#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    const int n = 512;
    std::vector<float> a(n * n), b(n * n), c(n * n);
    for (int p = 0; p < n * n; p++) {
        a[p] = (float)(p % 7) - 3.0f;
        b[p] = (float)(p % 5) * 0.5f;
    }
    auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < n; r++)
        for (int k = 0; k < n; k++) {
            float s = 0;
            for (int j = 0; j < n; j++) s += a[r * n + j] * b[j * n + k];
            c[r * n + k] = s;
        }
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double sum = 0;
    for (float v : c) sum += v;
    std::printf("%g %g %g\ntime %f\n", c[0], c[n * n - 1], sum, t);
}
