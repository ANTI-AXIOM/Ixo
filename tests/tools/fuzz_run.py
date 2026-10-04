#!/usr/bin/env python3
"""Runtime fuzzer: runs mutated programs (in a temp directory) and reports
crashes of the VM itself. Panics and compile errors are fine; segfaults are not.
Programs that touch the file system or processes are left out of the corpus.

Usage: python tests/tools/fuzz_run.py [iterations] [path/to/ixo]
"""
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fuzz import IXO as DEFAULT_IXO, ROOT, mutate  # noqa: E402

ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 300
IXO = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_IXO


def corpus():
    out = []
    for base in ("tests", "examples"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, base)):
            for f in files:
                if not f.endswith(".ixo"):
                    continue
                with open(os.path.join(dirpath, f), encoding="utf-8") as fh:
                    text = fh.read()
                if any(w in text for w in ("fs.", "os.", "net.", "input(", "use ")):
                    continue
                out.append(text)
    return out


def main():
    rnd = random.Random(99)
    progs = corpus()
    tmpdir = tempfile.mkdtemp(prefix="ixo_fuzzrun_")
    path = os.path.join(tmpdir, "case.ixo")
    crashes = timeouts = 0
    for n in range(ITER):
        text = mutate(rnd.choice(progs), rnd)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        try:
            p = subprocess.run([IXO, path], capture_output=True, timeout=10, cwd=tmpdir)
        except subprocess.TimeoutExpired:
            timeouts += 1
            continue
        if p.returncode not in (0, 1, 2):
            crashes += 1
            keep = os.path.join(tmpdir, f"crash_{n}.ixo")
            with open(keep, "w", encoding="utf-8", newline="") as fh:
                fh.write(text)
            print(f"CRASH ({p.returncode}): saved {keep}")
            print("   " + p.stderr.decode("utf-8", "replace")[-300:].replace("\n", "\n   "))
    print(f"{ITER} programs, {crashes} crashes, {timeouts} timeouts (infinite loops are expected sometimes)")
    return 1 if crashes else 0


if __name__ == "__main__":
    sys.exit(main())
