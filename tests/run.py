#!/usr/bin/env python3
"""Golden-output test runner for Ixo.

Each tests/**/*.ixo file (except files in `lib` folders or starting with `_`)
is a test. Expectations are comments in the file:

    print(1 + 2)            # expect: 3
    # expect error: undefined name 'foo'      (compile error, message substring)
    # expect panic: division by zero          (runtime panic, message substring)
    # args: one two                           (command-line arguments)
    # stdin: some input                       (lines fed to standard input)

Usage: python tests/run.py [--ixo PATH] [filter...]
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPECT = re.compile(r"#\s*expect:\s?(.*)$")
EXPECT_ERROR = re.compile(r"#\s*expect error:\s*(.*)$")
EXPECT_PANIC = re.compile(r"#\s*expect panic:\s*(.*)$")
ARGS = re.compile(r"#\s*args:\s*(.*)$")
STDIN = re.compile(r"#\s*stdin:\s?(.*)$")
SKIP_ON = re.compile(r"#\s*skip on:\s*(.*)$")


def default_ixo():
    if os.environ.get("IXO"):
        return os.environ["IXO"]
    exe = "ixo.exe" if os.name == "nt" else "ixo"
    return os.path.join(ROOT, "build", "release", exe)


def find_tests(filters):
    tests = []
    base = os.path.join(ROOT, "tests")
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d != "lib" and not d.startswith("_")]
        for f in sorted(filenames):
            if not f.endswith(".ixo") or f.startswith("_"):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, ROOT).replace("\\", "/")
            if filters and not any(x in rel for x in filters):
                continue
            tests.append((rel, path))
    return sorted(tests)


def parse_expectations(path):
    exp = {"out": [], "error": [], "panic": [], "args": [], "stdin": [], "skip": []}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n").rstrip("\r")
            for key, rx in (("error", EXPECT_ERROR), ("panic", EXPECT_PANIC)):
                m = rx.search(line)
                if m:
                    exp[key].append(m.group(1).strip())
                    break
            else:
                m = EXPECT.search(line)
                if m:
                    exp["out"].append(m.group(1))
                m = ARGS.search(line)
                if m:
                    exp["args"] = m.group(1).split()
                m = STDIN.search(line)
                if m:
                    exp["stdin"].append(m.group(1))
                m = SKIP_ON.search(line)
                if m:
                    exp["skip"] += m.group(1).replace(",", " ").split()
    return exp


def run_test(ixo, path, exp):
    cmd = [ixo, path] + exp["args"]
    stdin = "\n".join(exp["stdin"]) + ("\n" if exp["stdin"] else "")
    try:
        p = subprocess.run(cmd, input=stdin.encode(), capture_output=True, timeout=30, cwd=os.path.dirname(path))
    except subprocess.TimeoutExpired:
        return ["timed out after 30s"]
    out = p.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    err = p.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
    lines = out.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    problems = []
    if exp["error"]:
        if p.returncode == 0:
            problems.append("expected a compile error, but the program ran")
        for e in exp["error"]:
            if e not in err:
                problems.append(f"expected error containing {e!r}")
        if "panic:" in err:
            problems.append("expected a compile error but got a panic")
    elif exp["panic"]:
        if p.returncode == 0:
            problems.append("expected a panic, but the program exited normally")
        for e in exp["panic"]:
            if e not in err:
                problems.append(f"expected panic containing {e!r}")
    elif p.returncode != 0:
        problems.append(f"exit code {p.returncode}")
    if lines != exp["out"]:
        problems.append("output differs:")
        n = max(len(lines), len(exp["out"]))
        for i in range(n):
            want = exp["out"][i] if i < len(exp["out"]) else "<nothing>"
            got = lines[i] if i < len(lines) else "<nothing>"
            if want != got:
                problems.append(f"  line {i + 1}: expected {want!r}")
                problems.append(f"  {' ' * len(str(i + 1))}       got {got!r}")
    if problems and err.strip():
        problems.append("stderr:")
        problems.extend("  " + l for l in err.strip().split("\n")[:15])
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ixo", default=default_ixo())
    ap.add_argument("--platform", default="native", help="skip tests marked `# skip on: <platform>` (e.g. wasm)")
    ap.add_argument("filters", nargs="*")
    args = ap.parse_args()
    args.ixo = os.path.abspath(args.ixo)
    if not os.path.exists(args.ixo):
        print(f"ixo binary not found at {args.ixo} (build it first)")
        return 2
    tests = find_tests(args.filters)
    failed = 0
    skipped = 0
    start = time.time()
    for rel, path in tests:
        exp = parse_expectations(path)
        if args.platform in exp["skip"]:
            skipped += 1
            continue
        problems = run_test(args.ixo, path, exp)
        if problems:
            failed += 1
            print(f"FAIL {rel}")
            for pr in problems:
                print("     " + pr)
    took = time.time() - start
    ran = len(tests) - skipped
    note = f", {skipped} skipped on {args.platform}" if skipped else ""
    print(f"\n{ran - failed}/{ran} tests passed ({took:.1f}s{note})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
