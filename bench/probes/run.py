#!/usr/bin/env python3
"""Probes for runtime-efficiency problems (ROADMAP Phase 11): each one runs in Ixo
(with and without the JIT) and, where there is a .py twin, in Python, and reports the
best time and the smallest peak memory of N runs.

Usage: python bench/probes/run.py [names,...] [--runs N] [--ixo path]"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "bench", "langs"))
import run as bench  # noqa: E402  (bench/langs/run.py: timing and peak memory)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="?", default="")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--ixo", default=os.path.join(ROOT, "build", "release", "ixo" + bench.EXE))
    args = ap.parse_args()
    ixo = os.path.abspath(args.ixo)
    names = args.names.split(",") if args.names else sorted(f[:-4] for f in os.listdir(HERE) if f.endswith(".ixo"))
    print(f"{'probe':<10} {'Ixo s':>8} {'Ixo MB':>7} {'noJIT s':>8} {'noJIT MB':>8} {'Py s':>8} {'Py MB':>7}  check")
    for n in names:
        src = os.path.join(HERE, n + ".ixo")
        ix = bench.measure([ixo, src], args.runs)
        nj = bench.measure([ixo, src], args.runs, dict(os.environ, IXO_JIT="0"))
        py = (None, None, None)
        if os.path.exists(os.path.join(HERE, n + ".py")):
            py = bench.measure([sys.executable, os.path.join(HERE, n + ".py")], args.runs)
        t = lambda v: "     n/a" if v is None else f"{v:8.3f}"
        m = lambda v: "    n/a" if v is None else f"{v / 1048576:7.1f}"
        check = "ok" if py[2] is None or ix[2] == py[2] else "DIFF"
        print(f"{n:<10} {t(ix[0])} {m(ix[1])} {t(nj[0])} {m(nj[1]):>8} {t(py[0])} {m(py[1])}  {check}", flush=True)


if __name__ == "__main__":
    main()
