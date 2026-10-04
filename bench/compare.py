#!/usr/bin/env python3
"""A/B benchmark: runs two ixo executables alternately (so machine noise hits
both equally) and reports the best time of each per benchmark.

Usage: python bench/compare.py path/to/ixo_a path/to/ixo_b [runs]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
A, B = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
RUNS = int(sys.argv[3]) if len(sys.argv) > 3 else 7


def timed(exe, path):
    t = time.perf_counter()
    subprocess.run([exe, "--no-typecheck", path], capture_output=True)
    return time.perf_counter() - t


ta = tb = 0.0
print(f"{'':14} {'A':>8} {'B':>8}   B vs A")
for f in sorted(os.listdir(HERE)):
    if not f.endswith(".ixo") or f.startswith("_empty"):
        continue
    path = os.path.join(HERE, f)
    best_a = best_b = None
    for _ in range(RUNS):
        a = timed(A, path)
        b = timed(B, path)
        best_a = a if best_a is None else min(best_a, a)
        best_b = b if best_b is None else min(best_b, b)
    ta += best_a
    tb += best_b
    print(f"{f:<14} {best_a:8.3f} {best_b:8.3f}   {best_a / best_b:5.2f}x")
print(f"{'total':<14} {ta:8.3f} {tb:8.3f}   {ta / tb:5.2f}x")
