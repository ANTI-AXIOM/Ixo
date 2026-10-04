#!/usr/bin/env python3
"""Estimates what an LLVM backend would give Ixo (Linux or macOS, needs clang++).

For fib, loops, sieve and nbody it times:
  - Ixo as it is: the JIT, and the interpreter (IXO_JIT=0)
  - LLVM without types: *_dyn.cpp (Ixo's dynamic model, see dyn.hpp) by clang -O2
  - LLVM with types:    bench/langs/*.cpp (plain C++) by clang -O2
and how long clang -O2 takes to compile each dynamic version (what a JIT built
on LLVM would spend compiling). Best of N runs, wall-clock seconds.

Usage: python3 bench/backend/run.py [--runs N] [--ixo path] [--cxx clang++]"""
import argparse, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LANGS = os.path.join(ROOT, "bench", "langs")


def best(cmd, runs, env=None):
    t_best, out = None, None
    for _ in range(runs):
        t = time.perf_counter()
        p = subprocess.run(cmd, capture_output=True, text=True, env=env)
        dt = time.perf_counter() - t
        if p.returncode != 0:
            return None, (p.stdout + p.stderr).strip()[:200]
        out = p.stdout.strip()
        t_best = dt if t_best is None else min(t_best, dt)
    return t_best, out


def compile_cpp(cxx, src, exe):
    t = time.perf_counter()
    r = subprocess.run([cxx, "-O2", "-std=c++20", "-o", exe, src], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{cxx} failed on {src}:\n{r.stderr}")
    return time.perf_counter() - t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--ixo", default=os.path.join(ROOT, "build", "release", "ixo"))
    ap.add_argument("--cxx", default="clang++")
    args = ap.parse_args()
    if not shutil.which(args.cxx):
        sys.exit(f"needs {args.cxx} (LLVM's C++ compiler)")
    out_dir = os.path.join(ROOT, "build", "backend")
    os.makedirs(out_dir, exist_ok=True)
    gxx = shutil.which("g++")

    head = f"{'workload':<9} {'Ixo interp':>10} {'Ixo JIT':>8} {'LLVM dyn':>9} {'g++ dyn':>8} {'LLVM typed':>10}   {'JIT/LLVM dyn':>12} {'JIT/typed':>9} {'clang -O2':>9}"
    print(head)
    print("-" * len(head))
    for name in ("fib", "loops", "sieve", "nbody"):
        dyn_src = os.path.join(HERE, name + "_dyn.cpp")
        dyn_exe = os.path.join(out_dir, name + "_dyn")
        typed_exe = os.path.join(out_dir, name + "_typed")
        ctime = compile_cpp(args.cxx, dyn_src, dyn_exe)
        compile_cpp(args.cxx, os.path.join(LANGS, name + ".cpp"), typed_exe)
        g_t = None
        if gxx:
            compile_cpp(gxx, dyn_src, dyn_exe + "_gcc")
            g_t, _ = best([dyn_exe + "_gcc"], args.runs)
        ixo_src = os.path.join(LANGS, name + ".ixo")
        i_t, i_out = best([args.ixo, ixo_src], args.runs, dict(os.environ, IXO_JIT="0"))
        j_t, j_out = best([args.ixo, ixo_src], args.runs)
        d_t, d_out = best([dyn_exe], args.runs)
        t_t, t_out = best([typed_exe], args.runs)
        same = len({i_out, j_out, d_out, t_out}) == 1
        f = lambda x: "    n/a" if x is None else f"{x:7.3f}"
        r = lambda a, b: "     -" if not a or not b else f"{a / b:5.1f}x"
        print(f"{name:<9} {f(i_t):>10} {f(j_t):>8} {f(d_t):>9} {f(g_t):>8} {f(t_t):>10}   {r(j_t, d_t):>12} "
              f"{r(j_t, t_t):>9} {ctime * 1000:7.0f}ms" + ("" if same else "  DIFF"))
        if not same:
            for k, v in (("interp", i_out), ("jit", j_out), ("dyn", d_out), ("typed", t_out)):
                print(f"    {k:<7} {v}")
    print("\nTimes include process start-up (Ixo about 0.01 s). 'clang -O2' is the time to compile")
    print("the dynamic version: roughly what an LLVM-based JIT would spend before running it.")


if __name__ == "__main__":
    main()
