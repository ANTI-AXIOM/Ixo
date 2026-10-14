"""GPU kernels against the CPU: each workload as a gpu fn on the GPU (Vulkan), the
same gpu fn on the CPU fallback (all cores, IXO_GPU=cpu), plain Ixo loops (the JIT,
one core) and C++ -O2 (one core). Times are the work itself (each program measures
it with clock()), not start-up or building the kernel.

Usage: python bench/gpu/run.py [--ixo path] [--cxx g++]
"""
import argparse
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def timed(cmd, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    m = re.search(r"time ([0-9.eE+-]+)", p.stdout)
    if p.returncode != 0 or not m:
        return None, (p.stdout + p.stderr).strip().splitlines()[-1:]
    return float(m.group(1)), [l for l in p.stdout.splitlines() if not l.startswith("time ")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ixo", default=os.path.join(ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo"))
    ap.add_argument("--cxx", default="g++")
    args = ap.parse_args()
    out = os.path.join(ROOT, "build", "bench")
    os.makedirs(out, exist_ok=True)
    dev = subprocess.run([args.ixo, os.path.join(HERE, "device.ixo")], capture_output=True, text=True).stdout.strip()
    print(f"GPU: {dev}\n")
    print(f"{'workload':10s} {'GPU':>9s} {'GPU on CPU':>11s} {'plain Ixo':>10s} {'C++':>9s}   results")
    for name in ("mandel", "saxpy", "matmul"):
        gpu, r1 = timed([args.ixo, os.path.join(HERE, name + ".ixo")])
        cpu, r2 = timed([args.ixo, os.path.join(HERE, name + ".ixo")], dict(os.environ, IXO_GPU="cpu"))
        plain, r3 = timed([args.ixo, os.path.join(HERE, name + "_plain.ixo")])
        exe = os.path.join(out, "gpu_" + name + (".exe" if os.name == "nt" else ""))
        c = subprocess.run([args.cxx, "-O2", "-std=c++20", "-o", exe, os.path.join(HERE, name + ".cpp")],
                           capture_output=True, text=True)
        cxx, r4 = timed([exe]) if c.returncode == 0 else (None, ["(no C++ compiler)"])
        f = lambda t: f"{t:9.4f}" if t is not None else "      n/a"
        res = " | ".join(" ".join(r) for r in (r1, r2, r3, r4))
        print(f"{name:10s} {f(gpu)} {f(cpu):>11s} {f(plain):>10s} {f(cxx)}   {res}")


if __name__ == "__main__":
    main()
