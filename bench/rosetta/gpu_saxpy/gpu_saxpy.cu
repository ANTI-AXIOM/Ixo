// y = a * x + y on the GPU over a million floats, ten times; the sum of y (with CUDA).
#include <cstdio>
#include <vector>

__global__ void saxpy(float a, const float* x, float* y, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) y[i] = a * x[i] + y[i];
}

int main() {
    const int n = 1000000;
    std::vector<float> hx(n), hy(n);
    for (int i = 0; i < n; i++) hx[i] = i % 100;
    float *x, *y;
    cudaMalloc(&x, n * sizeof(float));
    cudaMalloc(&y, n * sizeof(float));
    cudaMemcpy(x, hx.data(), n * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemset(y, 0, n * sizeof(float));
    for (int k = 0; k < 10; k++) saxpy<<<(n + 255) / 256, 256>>>(2.0f, x, y, n);
    cudaMemcpy(hy.data(), y, n * sizeof(float), cudaMemcpyDeviceToHost);
    double sum = 0;
    for (float v : hy) sum += v;
    std::printf("%.1f\n", sum);
    cudaFree(x);
    cudaFree(y);
}
