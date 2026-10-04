# a big list of floats: mean and spread
import math


def run(n):
    xs = []
    x = 0.5
    for i in range(n):
        x = (x * 3.9) * (1.0 - x)
        xs.append(x)
    mean = 0.0
    for v in xs:
        mean += v
    mean /= len(xs)
    spread = 0.0
    for v in xs:
        spread += (v - mean) * (v - mean)
    print(f"{mean:.6f} {math.sqrt(spread / len(xs)):.6f}")


run(2_000_000)
