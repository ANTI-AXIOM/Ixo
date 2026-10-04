#!/usr/bin/env python3
"""Differential test for the JIT: runs programs with the JIT on and off and
checks they print the same thing and exit the same way.

Usage: python tests/tools/jit_check.py [path/to/ixo]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IXO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")


def run(path, jit):
    env = dict(os.environ)
    env["IXO_JIT"] = "1" if jit else "0"
    p = subprocess.run([IXO, path], capture_output=True, env=env, timeout=120, cwd=os.path.dirname(path), input=b"")
    return p.returncode, p.stdout.decode("utf-8", "replace").replace("\r\n", "\n")


def main():
    files = []
    for d in ("tests", "examples", "bench"):
        for dirpath, _, names in os.walk(os.path.join(ROOT, d)):
            for n in names:
                # bench/probes are slow measurement programs (bench/probes/run.py runs them)
                skip = "compile_errors" in dirpath or os.path.basename(dirpath) == "probes"
                if n.endswith(".ixo") and not skip and not n.startswith("_"):
                    files.append(os.path.join(dirpath, n))
    bad = 0
    for f in sorted(files):
        try:
            a, b = run(f, False), run(f, True)
        except subprocess.TimeoutExpired:
            print("TIMEOUT", os.path.relpath(f, ROOT))
            bad += 1
            continue
        # timing lines differ between runs; compare everything else
        strip = lambda out: "\n".join(l for l in out.split("\n") if " in " not in l or "s" not in l.split(" in ")[-1])
        if a[0] != b[0] or strip(a[1]) != strip(b[1]):
            bad += 1
            print("DIFF", os.path.relpath(f, ROOT), a[0], b[0])
    print(f"{len(files) - bad}/{len(files)} programs agree")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
