// Mandelbrot, 1024 x 768, 256 iterations: C++ -O2, 32-bit floats, one core
#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    const int w = 1024, h = 768, iters = 256;
    std::vector<int> out(w * h);
    auto t0 = std::chrono::steady_clock::now();
    for (int p = 0; p < w * h; p++) {
        float x0 = (float)(p % w) / (float)w * 3.5f - 2.5f;
        float y0 = (float)(p / w) / (float)h * 2.0f - 1.0f;
        float x = 0, y = 0;
        int k = 0;
        while (k < iters && x * x + y * y <= 4.0f) {
            float t = x * x - y * y + x0;
            y = 2.0f * x * y + y0;
            x = t;
            k++;
        }
        out[p] = k;
    }
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    long long sum = 0;
    for (int v : out) sum += v;
    std::printf("%lld\ntime %f\n", sum, t);
}
