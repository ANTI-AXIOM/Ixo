#!/usr/bin/env python3
"""Rosetta: everyday tasks written in Ixo, Python, C++ and Rust, counted and timed.

Each task directory holds the same program in every language (<task>.ixo, .py, .cpp or
.cu, .rs) and expected.txt, the output all of them must print. The runner:
- counts lines, words and tokens the same way for every language: comments and blank
  lines are left out, a string literal is one token, an operator like `->` or `+=` is one;
- runs what this machine can run (Ixo, Python, C++ with g++, Rust with rustc), checks the
  output and keeps the best time and the smallest peak memory of --runs runs;
- prints the tables (--md: as Markdown, for docs/rosetta.md).

A task's args.txt holds its command-line arguments. A version that needs something the
machine lacks (CuPy, CUDA, nlohmann/json, a Rust crate) is counted but not run.

--safety checks the planted mistakes in safety/: each strict Ixo file must be rejected
by `ixo check`, and the sheet lists what each language rejects before running.

Usage: python bench/rosetta/run.py [--runs N] [--only a,b] [--count-only] [--md]
                                   [--ixo path] [--cxx g++] [--safety]
"""
import argparse, importlib.util, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXE = ".exe" if os.name == "nt" else ""
LANGS = [("ixo", (".ixo",)), ("python", (".py",)), ("c++", (".cpp", ".cu")), ("rust", (".rs",))]

_spec = importlib.util.spec_from_file_location("langs_run", os.path.join(ROOT, "bench", "langs", "run.py"))
langs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(langs)

# ------------------------------------------------------------------ counting

STRING = r'[rbfu]*"""[\s\S]*?"""|[rbfu]*"(?:[^"\\\n]|\\.)*"'
CHAR = r"'(?:[^'\\\n]|\\.)*'"
LIFETIME = r"'\w+\b(?!')"
OPS = r"\.\.=|<<=|>>=|->|=>|==|!=|<=|>=|\+=|-=|\*=|/=|%=|&=|\|=|::|\.\.|&&|\|\||<<|>>|:>|<:|\*\*|//"


def quoted(ext):
    return STRING + "|" + (LIFETIME + "|" + CHAR if ext == ".rs" else CHAR)


def code_of(text, ext):
    """The source without comments (strings kept)."""
    comment = r"#[^\n]*" if ext in (".py", ".ixo") else r"//[^\n]*|/\*[\s\S]*?\*/"
    return re.sub(f"({quoted(ext)})|{comment}", lambda m: m.group(1) or "", text)


def count(path):
    """(lines, words, tokens) of a source file."""
    ext = os.path.splitext(path)[1]
    code = code_of(open(path, encoding="utf-8").read(), ext)
    lines = [l for l in code.splitlines() if l.strip()]
    tokens = re.findall(f"{quoted(ext)}|{OPS}|\\w+|\\S", code)
    return len(lines), sum(len(l.split()) for l in lines), len(tokens)


# ------------------------------------------------------------------ running

def needs(lang, src):
    """What the machine lacks to run this version, or None."""
    text = open(src, encoding="utf-8").read()
    if lang == "python":
        for mod in re.findall(r"^(?:import|from) (\w+)", text, re.M):
            if importlib.util.find_spec(mod) is None:
                return f"needs {mod}"
    if lang == "c++":
        if src.endswith(".cu") and not shutil.which("nvcc"):
            return "needs CUDA"
        if "nlohmann/" in text:
            return "needs nlohmann/json"
    if lang == "rust":
        if not shutil.which("rustc"):
            return "no rustc"
        crates = [c for c in re.findall(r"^use (\w+)::", text, re.M) if c not in ("std", "core", "alloc")]
        if crates:
            return "needs crate " + ", ".join(sorted(set(crates)))
    return None


def command(lang, src, args, build_dir):
    """The command that runs a version (building it first), or a reason it can't run."""
    reason = needs(lang, src)
    if reason:
        return reason
    if lang == "ixo":
        return [args.ixo, src]
    if lang == "python":
        return [sys.executable, src]
    exe = os.path.join(build_dir, os.path.splitext(os.path.basename(src))[0] + "_" + lang.replace("+", "p") + EXE)
    if lang == "c++" and src.endswith(".cu"):
        build = ["nvcc", "-O2", "-o", exe, src]
    elif lang == "c++":
        if not shutil.which(args.cxx):
            return f"no {args.cxx}"
        # (Windows: static, so the C++ runtime found on PATH at run time doesn't matter)
        build = [args.cxx, "-O2", "-std=c++20", "-o", exe, src] + (["-static"] if os.name == "nt" else ["-pthread"])
    else:
        build = ["rustc", "-O", "--edition", "2021", "-o", exe, src]
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        r = subprocess.run(build, capture_output=True, text=True)
        if r.returncode != 0:
            return "build failed: " + (r.stderr.strip().splitlines() or ["?"])[0][:80]
    return [exe]


def run_task(task, args, build_dir):
    folder = os.path.join(HERE, task)
    expected = open(os.path.join(folder, "expected.txt"), encoding="utf-8").read().strip()
    extra = []
    if os.path.exists(os.path.join(folder, "args.txt")):
        extra = open(os.path.join(folder, "args.txt"), encoding="utf-8").read().split()
    results = {}
    for lang, exts in LANGS:
        src = next((os.path.join(folder, task + e) for e in exts if os.path.exists(os.path.join(folder, task + e))), None)
        if not src:
            continue
        size = count(src)
        cmd = command(lang, src, args, build_dir) if not args.count_only else "not run"
        if isinstance(cmd, str):
            results[lang] = (size, None, None, cmd)
            continue
        best_t = best_m = None
        status = "ok"
        with tempfile.TemporaryDirectory() as work:
            here = os.getcwd()
            os.chdir(work)
            try:
                for _ in range(args.runs):
                    t, peak, code, out = langs.run_once(cmd + extra)
                    if code != 0 or out.replace("\r\n", "\n").strip() != expected:
                        status = f"wrong output (exit {code}): {out.splitlines()[-1][:60] if out else ''}"
                        break
                    best_t = t if best_t is None else min(best_t, t)
                    best_m = peak if best_m is None or (peak and peak < best_m) else best_m
            finally:
                os.chdir(here)
        results[lang] = (size, best_t, best_m, status)
    return results


# ------------------------------------------------------------------ safety

# Planted mistakes and what each language does with them before the program runs:
# "rejected" (compile error), "warning", "impossible" (can't be written), or "runtime".
SAFETY = {
    "nil":      ("a missing nil check",          "runtime",  "rejected",   "runtime"),
    "error":    ("an ignored error",             "runtime",  "warning",    "runtime"),
    "race":     ("a race on shared data",        "runtime",  "rejected",   "runtime"),
    "free":     ("a use after free",             "impossible", "rejected", "runtime"),
    "index":    ("an index out of range",        "runtime",  "rejected",   "warning"),
    "match":    ("a missing match case",         "runtime",  "rejected",   "warning"),
    "close":    ("a forgotten close",            "warning",  "impossible", "impossible"),
}
NOTES = """- Python: `nil` is None (AttributeError when used), an ignored error is an exception
  nobody catches, a race is a lost update between threads; a forgotten close is a
  ResourceWarning at best.
- Rust: an ignored `Result` is the `unused_must_use` warning; an index out of range is
  rejected for arrays indexed by constants, and panics at run time otherwise.
- C++: `-Wall` warns about a missing enum case and some constant out-of-range indexes;
  RAII closes files."""


def safety(args):
    folder = os.path.join(HERE, "safety")
    rows = []
    for key, (what, py, rs, cpp) in SAFETY.items():
        src = os.path.join(folder, key + ".ixo")
        if not os.path.exists(src):
            ixo = "?"
        else:
            r = subprocess.run([args.ixo, "check", src], capture_output=True, text=True)
            text = open(src, encoding="utf-8").read()
            if "# impossible" in text:
                ixo = "impossible" if r.returncode == 0 else "rejected"
            else:
                ixo = "rejected" if r.returncode != 0 else "NOT REJECTED"
        rows.append((what, ixo, py, rs, cpp))
    print("| planted mistake | strict Ixo | Python | Rust | C++ |")
    print("|---|---|---|---|---|")
    for row in rows:
        print("| " + " | ".join(row) + " |")
    print()
    print(NOTES)
    return all(r[1] in ("rejected", "impossible") for r in rows)


# ------------------------------------------------------------------ report

def fmt_size(s):
    return f"{s[1]} ({s[0]}, {s[2]})"


def fmt_time(r):
    size, t, peak, status = r
    if status != "ok":
        return status
    return f"{t:.2f}s {peak / 1e6:.0f}MB" if peak else f"{t:.2f}s"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--only", default="")
    ap.add_argument("--count-only", action="store_true")
    ap.add_argument("--md", action="store_true")
    ap.add_argument("--ixo", default=os.path.join(ROOT, "build", "release", "ixo" + EXE))
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--safety", action="store_true")
    args = ap.parse_args()
    if args.safety:
        sys.exit(0 if safety(args) else 1)
    tasks = sorted(d for d in os.listdir(HERE) if os.path.exists(os.path.join(HERE, d, "expected.txt")))
    if args.only:
        tasks = [t for t in tasks if t in args.only.split(",")]
    build_dir = os.path.join(ROOT, "build", "rosetta")
    os.makedirs(build_dir, exist_ok=True)
    all_results = {}
    for task in tasks:
        all_results[task] = run_task(task, args, build_dir)
        if not args.md:
            print(task, *(f"{l}: {fmt_time(r)}" for l, r in all_results[task].items()), sep="  ", file=sys.stderr)

    names = [l for l, _ in LANGS]
    print("Words per task (lines, tokens); the last columns compare Ixo's words with each language's:\n")
    print("| task | " + " | ".join(names) + " | Ixo/Python | Ixo/Rust | Ixo/C++ |")
    print("|---" * (len(names) + 4) + "|")
    ratios = {"python": [], "rust": [], "c++": []}
    totals = {l: [0, 0, 0] for l in names}
    for task, res in all_results.items():
        cells = [fmt_size(res[l][0]) if l in res else "" for l in names]
        rs = []
        for other in ("python", "rust", "c++"):
            if "ixo" in res and other in res:
                q = res["ixo"][0][1] / res[other][0][1]
                ratios[other].append((q, task))
                rs.append(f"{q:.2f}")
            else:
                rs.append("")
        for l in names:
            if l in res:
                totals[l] = [a + b for a, b in zip(totals[l], res[l][0])]
        print(f"| {task} | " + " | ".join(cells) + " | " + " | ".join(rs) + " |")
    print("| **total** | " + " | ".join(fmt_size(totals[l]) for l in names) + " | | | |")
    print()
    for other, qs in ratios.items():
        if qs:
            mean = sum(q for q, _ in qs) / len(qs)
            worst = max(qs)
            print(f"- Ixo/{other} words: mean {mean:.2f}, most {worst[0]:.2f} ({worst[1]})")
    if args.count_only:
        return
    print("\nBest time and peak memory:\n")
    print("| task | " + " | ".join(names) + " |")
    print("|---" * (len(names) + 1) + "|")
    for task, res in all_results.items():
        print(f"| {task} | " + " | ".join(fmt_time(res[l]) if l in res else "" for l in names) + " |")
    bad = [(t, l) for t, res in all_results.items() for l, r in res.items() if r[3].startswith("wrong")]
    if bad:
        print("\nwrong output:", ", ".join(f"{t}/{l}" for t, l in bad), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
