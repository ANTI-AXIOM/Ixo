#!/usr/bin/env python3
"""Checks that every ```ixo code block in README.md and docs/*.md compiles
(`ixo check`) and runs without panicking. Blocks marked ```ixo-fragment are
skipped (they depend on other files).

Usage: python tests/tools/doc_check.py [path/to/ixo]
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IXO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")
BLOCK = re.compile(r"```ixo\n(.*?)```", re.S)


def main():
    files = [os.path.join(ROOT, "README.md")] + sorted(glob.glob(os.path.join(ROOT, "docs", "*.md")))
    tmp = tempfile.mkdtemp(prefix="ixo_docs_")
    failed = total = 0
    for f in files:
        text = open(f, encoding="utf-8").read()
        for i, m in enumerate(BLOCK.finditer(text)):
            total += 1
            line = text[: m.start()].count("\n") + 2
            path = os.path.join(tmp, f"block_{total}.ixo")
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(m.group(1))
            where = f"{os.path.relpath(f, ROOT)}:{line}"
            p = subprocess.run([IXO, "check", path], capture_output=True, timeout=30)
            if p.returncode != 0:
                failed += 1
                print(f"FAIL {where} (check)\n   " + p.stderr.decode("utf-8", "replace").strip().replace("\n", "\n   "))
                continue
            if "expect panic" in m.group(1):
                continue  # demonstrates a panic on purpose
            p = subprocess.run([IXO, path], capture_output=True, timeout=30, cwd=tmp)
            if p.returncode != 0:
                failed += 1
                print(f"FAIL {where} (run)\n   " + p.stderr.decode("utf-8", "replace").strip().replace("\n", "\n   "))
    print(f"{total - failed}/{total} code blocks ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
