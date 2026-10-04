#!/usr/bin/env python3
"""Robustness fuzzer: mutates existing .ixo files and checks that `ixo check`
and `ixo fmt` never crash or hang, whatever the input.

Usage: python tests/tools/fuzz.py [iterations] [path/to/ixo]
"""
import os
import random
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ITER = int(sys.argv[1]) if len(sys.argv) > 1 else 300
IXO = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")

SNIPPETS = ["(", ")", "[", "]", "{", "}", ":", ",", ".", "->", "=", "==", "!", "?", "_", "_a", "fn", "class",
            "if", "else", "match", "for", "in", "while", "return", "retry", "fork", "with tasks", "self", "\n",
            "\n    ", "    ", '"', "'", 'f"{', "}", "#", "..", "..<", ":>", "<:", "*", "**", "-", "1", "1.5",
            "nil", "x", "Point", "use", "@test", "$jit", "|", "as", "..rest", "\t", "\\", "\"\"\"",
            "interface", "enum", "par", "defer", " is ", "@arena", "@cache c", "@dual d = []", "fn +(o)",
            "fn -()", "fn [](i)", "fn []=(i, v)", "Color.Red", "Shape.Rect(w, h)"]


def sources():
    out = []
    for base in ("tests", "examples"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, base)):
            for f in files:
                if f.endswith(".ixo"):
                    with open(os.path.join(dirpath, f), encoding="utf-8") as fh:
                        out.append(fh.read())
    return out


def mutate(src, rnd):
    s = src
    for _ in range(rnd.randint(1, 4)):
        op = rnd.random()
        if not s:
            s = rnd.choice(SNIPPETS)
            continue
        i = rnd.randrange(len(s))
        j = min(len(s), i + rnd.randint(1, 20))
        if op < 0.3:
            s = s[:i] + s[j:]
        elif op < 0.6:
            s = s[:i] + rnd.choice(SNIPPETS) + s[i:]
        elif op < 0.8:
            lines = s.split("\n")
            a, b = rnd.randrange(len(lines)), rnd.randrange(len(lines))
            lines[a], lines[b] = lines[b], lines[a]
            s = "\n".join(lines)
        else:
            s = s[:i] + s[i:j] * rnd.randint(2, 4) + s[j:]
    return s


def main():
    rnd = random.Random(1234)
    corpus = sources()
    crashes = 0
    tmpdir = tempfile.mkdtemp(prefix="ixo_fuzz_")
    path = os.path.join(tmpdir, "case.ixo")
    for n in range(ITER):
        text = mutate(rnd.choice(corpus), rnd)
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        for cmd in ([IXO, "check", path], [IXO, "fmt", "--check", path]):
            try:
                p = subprocess.run(cmd, capture_output=True, timeout=20)
                bad = p.returncode not in (0, 1, 2)
            except subprocess.TimeoutExpired:
                bad = True
                p = None
            if bad:
                crashes += 1
                keep = os.path.join(tmpdir, f"crash_{n}.ixo")
                with open(keep, "w", encoding="utf-8", newline="") as fh:
                    fh.write(text)
                code = "timeout" if p is None else p.returncode
                print(f"CRASH ({code}) in `{cmd[1]}`: saved {keep}")
    print(f"{ITER} cases, {crashes} crashes")
    return 1 if crashes else 0


if __name__ == "__main__":
    sys.exit(main())
