// a big list of floats: mean and spread
#include <cmath>
#include <cstdio>
#include <vector>

static void run(long n) {
    std::vector<double> xs;
    double x = 0.5;
    for (long i = 0; i < n; i++) {
        x = (x * 3.9) * (1.0 - x);
        xs.push_back(x);
    }
    double mean = 0.0;
    for (double v : xs) mean += v;
    mean /= (double)xs.size();
    double spread = 0.0;
    for (double v : xs) spread += (v - mean) * (v - mean);
    std::printf("%.6f %.6f\n", mean, std::sqrt(spread / (double)xs.size()));
}

int main() { run(2000000); }
