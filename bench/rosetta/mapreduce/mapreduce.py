# Counting primes below 300,000 on every core: split, map in parallel, reduce.
from multiprocessing import Pool


def count_primes(bounds):
    lo, hi = bounds
    n = 0
    for x in range(max(lo, 2), hi):
        d = 2
        while d * d <= x and x % d != 0:
            d += 1
        if d * d > x:
            n += 1
    return n


if __name__ == "__main__":
    step = 300_000 // 8
    with Pool() as pool:
        print(sum(pool.map(count_primes, [(i * step, (i + 1) * step) for i in range(8)])))
