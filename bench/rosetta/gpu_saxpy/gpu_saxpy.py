# y = a * x + y on the GPU over a million floats, ten times; the sum of y (with CuPy).
import cupy as cp

saxpy = cp.ElementwiseKernel("float32 a, float32 x", "float32 y", "y = a * x + y", "saxpy")
n = 1_000_000
x = (cp.arange(n) % 100).astype(cp.float32)
y = cp.zeros(n, dtype=cp.float32)
for _ in range(10):
    saxpy(cp.float32(2.0), x, y)
print(float(y.sum(dtype=cp.float64)))
