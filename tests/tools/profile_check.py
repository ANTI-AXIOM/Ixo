#!/usr/bin/env python3
"""Checks the measuring tools: --profile charges time to the right function and
line (with and without the JIT), --memprofile counts string bytes, and
IXO_JIT_STATS reports where machine code hands over to the interpreter.

Usage: python tests/tools/profile_check.py [--ixo path]"""
import argparse, os, platform, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = ".exe" if os.name == "nt" else ""

HOT = """fn light(n)
    t = 0
    for i in 0..n -> t += i % 7
    t
fn heavy(n)
    t = 0
    for i in 0..n -> t += i % 7
    t
print(light(2_000_000) + heavy(40_000_000))
"""

# 20000 strings of about 200 characters: 4 MB of text (about 1.6 MB without it)
APPEND = """fn run(n)
    xs = []
    for i in 0..n -> xs.add("ab" * 100 + str(i))
    xs.len
print(run(20_000))
"""


def run(ixo, src, args=(), env=None):
    with tempfile.NamedTemporaryFile("w", suffix=".ixo", delete=False) as f:
        f.write(src)
        path = f.name
    try:
        e = dict(os.environ, **(env or {}))
        p = subprocess.run([ixo, *args, path], capture_output=True, text=True, env=e, timeout=120)
        return p.stdout, p.stderr, os.path.basename(path)
    finally:
        os.unlink(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ixo", default=os.path.join(ROOT, "build", "release", "ixo" + EXE))
    ixo = os.path.abspath(ap.parse_args().ixo)
    failures = []

    def check(cond, what, detail=""):
        print(("ok    " if cond else "FAIL  ") + what)
        if not cond:
            failures.append(what)
            if detail:
                print("      " + detail.replace("\n", "\n      "))

    for label, env in (("jit", {}), ("interpreter", {"IXO_JIT": "0"})):
        _, err, name = run(ixo, HOT, ["--profile"], env)
        m = re.search(r"([\d.]+)%\s+[\d.]+%\s+heavy", err)
        check(m is not None and float(m.group(1)) >= 70, f"profile ({label}): heavy() gets most of the time", err)
        lines = re.findall(r"([\d.]+)%\s+(\S+:\d+)", err.split("hottest lines:")[-1])
        check(bool(lines) and lines[0][1] == f"{name}:7", f"profile ({label}): the hottest line is heavy's loop", err)

    _, err, _ = run(ixo, APPEND, ["--memprofile"])
    m = re.search(r"memory: ([\d.]+) MB allocated", err)
    check(m is not None and float(m.group(1)) >= 4.5, "memprofile counts the characters of new strings", err)

    out, err, _ = run(ixo, HOT, [], {"IXO_JIT_STATS": "1"})
    if platform.machine().lower() not in ("amd64", "x86_64", "arm64", "aarch64"):
        print("skip  IXO_JIT_STATS (no JIT on this platform)")
    else:
        check("entered machine code" in err and "to let other tasks run" in err, "IXO_JIT_STATS reports entries and exits", err)

    if failures:
        print(f"{len(failures)} check(s) failed")
        sys.exit(1)
    print("all profile checks passed")


if __name__ == "__main__":
    main()
