#!/usr/bin/env python3
"""Checks `ixo build`: the executable it makes runs anywhere, with its modules
and packages inside.

Usage: python tests/tools/build_check.py [path/to/ixo]
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IXO = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(
    ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")
EXE = ".exe" if os.name == "nt" else ""
failures = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"\n     {detail}"))
    if not ok:
        failures.append(name)


def run(args, cwd, stdin=""):
    p = subprocess.run(args, cwd=cwd, capture_output=True, timeout=60, input=stdin.encode())
    return p.returncode, p.stdout.decode("utf-8", "replace").replace("\r\n", "\n"), \
        p.stderr.decode("utf-8", "replace").replace("\r\n", "\n")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main():
    tmp = tempfile.mkdtemp(prefix="ixo_build_")
    try:
        proj = os.path.join(tmp, "hello")
        code, out, err = run([IXO, "new", "hello"], tmp)
        assert code == 0, out + err
        # a folder package, installed into ixo_modules
        write(os.path.join(tmp, "colors", "ixo.toml"), '[package]\nname = "colors"\nmain = "colors.ixo"\n')
        write(os.path.join(tmp, "colors", "colors.ixo"), 'use colors.names\n\nfn paint*(s) = f"[{red()}]{s}"\n')
        write(os.path.join(tmp, "colors", "names.ixo"), 'fn red* = "red"\n')
        code, out, err = run([IXO, "add", "../colors"], proj)
        assert code == 0, out + err
        write(os.path.join(proj, "main.ixo"),
              'use lib.greeting\nuse colors\n\n'
              'fn main\n'
              '    print(paint(greet(args.join(" ") or "nobody")))\n'
              '    print(input().up)\n'
              '    if args.has("boom")\n'
              '        panic("boom")\n')

        code, out, err = run([IXO, "build"], proj)
        exe = os.path.join(proj, "main" + EXE)
        check("ixo build (the project's main file)", code == 0 and os.path.exists(exe), out + err)
        check("it reports the modules", "4 modules" in out, out)

        away = os.path.join(tmp, "elsewhere")
        os.makedirs(away)
        moved = os.path.join(away, "hi" + EXE)
        shutil.move(exe, moved)
        shutil.rmtree(os.path.join(proj, "ixo_modules"))  # nothing on disk is needed any more
        code, out, err = run([moved, "Ann", "Bo"], away, "quiet\n")
        check("runs anywhere, args go to the program",
              code == 0 and out == "[red]Hello, Ann Bo!\nQUIET\n", repr(out) + err)
        code, out, err = run([moved, "boom"], away, "x\n")
        check("a panic names the bundled file", code != 0 and "panic: boom" in err and "main.ixo" in err, err)
        code, out, err = run([moved, "--help"], away, "x\n")
        check("ixo's own options aren't taken", code == 0 and "Hello, --help!" in out, out + err)

        run([IXO, "install"], proj)
        code, out, err = run([IXO, "build", "main.ixo", "-o", os.path.join("out", "again")], proj)
        size1 = os.path.getsize(os.path.join(proj, "out", "again" + EXE)) if code == 0 else 0
        check("-o (a new folder)", code == 0 and size1 > 0, out + err)

        write(os.path.join(proj, "bad.ixo"), 'x: int = "no"\n')
        code, out, err = run([IXO, "build", "bad.ixo"], proj)
        check("errors stop the build", code != 0 and "fix the errors" in err and
              not os.path.exists(os.path.join(proj, "bad" + EXE)), err)

        code, out, err = run([IXO, "build"], tmp)
        check("no project, no file: usage", code != 0 and "usage" in err, err)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\nall passed" if not failures else f"\n{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
