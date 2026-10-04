#!/usr/bin/env python3
"""Runs the benchmarks and prints the best time of N runs.
Usage: python bench/run.py [runs] [path/to/ixo]"""
import os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 3
IXO = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")
total = 0.0
for f in sorted(os.listdir(HERE)):
    if not f.endswith(".ixo"):
        continue
    best = None
    for _ in range(RUNS):
        t = time.perf_counter()
        out = subprocess.run([IXO, "--no-typecheck", os.path.join(HERE, f)], capture_output=True, text=True)
        dt = time.perf_counter() - t
        best = dt if best is None else min(best, dt)
    total += best
    print(f"{f:<14} {best:7.3f}s   {out.stdout.strip()[:60]}")
print(f"{'total':<14} {total:7.3f}s")
