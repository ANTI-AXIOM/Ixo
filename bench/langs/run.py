#!/usr/bin/env python3
"""Compares Ixo with C++ and Python on the same workloads: time and memory.

Each workload has a .ixo, a .py and (where the standard library allows) a .cpp
version that print the same result line. The runner compiles the C++ with
g++ -O2, runs every version a few times and keeps the best wall-clock time
and the smallest peak memory (the most RAM the process used at once).

Usage: python bench/langs/run.py [--runs N] [--ixo path] [--python path]
                                 [--cxx g++] [--only name,name] [--no-jit]
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EXE = ".exe" if os.name == "nt" else ""


PEAK = None  # the launcher that measures a program's own peak (Linux, macOS)


def peak_launcher():
    """Builds bench/langs/peak.c once. A child's maximum resident set also counts
    the memory of the process that forked it (here, Python): the small launcher
    keeps that out of the numbers."""
    global PEAK
    if PEAK is not None or os.name == "nt":
        return PEAK
    exe = os.path.join(ROOT, "build", "bench", "peak")
    src = os.path.join(HERE, "peak.c")
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        if not cc or subprocess.run([cc, "-O2", "-o", exe, src], capture_output=True).returncode != 0:
            print("note: no C compiler for bench/langs/peak.c: memory includes the runner's own", file=sys.stderr)
            PEAK = ""
            return PEAK
    PEAK = exe
    return PEAK


def run_once(cmd, env=None):
    """(seconds, peak bytes or None, exit code, output)"""
    with tempfile.TemporaryFile() as out:
        peak = None
        if os.name == "nt":
            t = time.perf_counter()
            p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, env=env)
            p.wait()
            dt = time.perf_counter() - t
            peak = windows_peak(p)
            code = p.returncode
        elif peak_launcher():
            fd, peak_file = tempfile.mkstemp()
            os.close(fd)
            t = time.perf_counter()
            code = subprocess.run([PEAK, peak_file, *cmd], stdout=out, stderr=subprocess.STDOUT, env=env).returncode
            dt = time.perf_counter() - t
            try:
                peak = int(open(peak_file).read().strip() or 0) or None
            except ValueError:
                peak = None
            os.unlink(peak_file)
        else:
            t = time.perf_counter()
            p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, env=env)
            _, status, usage = os.wait4(p.pid, 0)
            dt = time.perf_counter() - t
            code = os.waitstatus_to_exitcode(status)
            p.returncode = code
            # ru_maxrss: kilobytes on Linux, bytes on macOS
            peak = usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024
        out.seek(0)
        text = out.read().decode("utf-8", "replace").strip()
    return dt, peak, code, text


def windows_peak(p):
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
    c = Counters()
    c.cb = ctypes.sizeof(c)
    get = ctypes.windll.psapi.GetProcessMemoryInfo
    get.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    if not get(int(p._handle), ctypes.byref(c), c.cb):
        return None
    return c.PeakWorkingSetSize


def measure(cmd, runs, env=None):
    """best time, smallest peak memory, output (None time on failure)"""
    best_t, best_m, out = None, None, ""
    for _ in range(runs):
        dt, peak, code, text = run_once(cmd, env)
        if code != 0:
            return None, None, text
        out = text
        best_t = dt if best_t is None else min(best_t, dt)
        if peak is not None:
            best_m = peak if best_m is None else min(best_m, peak)
    return best_t, best_m, out


def fmt_t(t):
    return "    n/a" if t is None else f"{t:7.3f}"


def fmt_m(m):
    return "    n/a" if m is None else f"{m / 1048576:7.1f}"


def ratio(a, b):
    return "     -" if a is None or b is None or b == 0 else f"{a / b:6.1f}x"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--ixo", default=os.path.join(ROOT, "build", "release", "ixo" + EXE))
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--only", default="")
    ap.add_argument("--no-jit", action="store_true", help="also run Ixo with IXO_JIT=0")
    args = ap.parse_args()
    ixo = os.path.abspath(args.ixo)

    names = sorted(f[:-4] for f in os.listdir(HERE) if f.endswith(".ixo"))
    if args.only:
        names = [n for n in names if n in args.only.split(",")]
    build = os.path.join(ROOT, "build", "bench")
    os.makedirs(build, exist_ok=True)
    have_cxx = shutil.which(args.cxx) is not None

    rows = []
    for name in names:
        r = {"name": name}
        src = os.path.join(HERE, name + ".cpp")
        if have_cxx and os.path.exists(src):
            exe = os.path.join(build, name + EXE)
            if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
                c = subprocess.run([args.cxx, "-O2", "-std=c++20", "-o", exe, src], capture_output=True, text=True)
                if c.returncode != 0:
                    print(f"{name}: C++ build failed\n{c.stderr}")
                    exe = None
            if exe:
                r["cpp"] = measure([exe], args.runs)
        r["ixo"] = measure([ixo, os.path.join(HERE, name + ".ixo")], args.runs)
        if args.no_jit:
            r["nj"] = measure([ixo, os.path.join(HERE, name + ".ixo")], args.runs, dict(os.environ, IXO_JIT="0"))
        r["py"] = measure([args.python, os.path.join(HERE, name + ".py")], args.runs)
        outs = [r[k][2] for k in ("cpp", "ixo", "py") if k in r and r[k][0] is not None]
        r["ok"] = r["ixo"][0] is not None and len(set(outs)) == 1
        rows.append(r)
        print(f"  {name} done", file=sys.stderr, flush=True)

    def get(r, k, i):
        return r[k][i] if k in r else None

    for title, idx, fmt, unit in (("time", 0, fmt_t, "seconds"), ("peak memory", 1, fmt_m, "MB")):
        head = f"{title + ' (' + unit + ')':<22} {'C++':>7} {'Ixo':>7}"
        if args.no_jit:
            head += f" {'Ixo-nj':>7}"
        head += f" {'Python':>7}   {'Ixo/C++':>7} {'Ixo/Py':>7}"
        print()
        print(head)
        print("-" * len(head))
        total = {"cpp": 0.0, "ixo": 0.0, "nj": 0.0, "py": 0.0}
        for r in rows:
            line = f"{r['name']:<22} {fmt(get(r, 'cpp', idx))} {fmt(get(r, 'ixo', idx))}"
            if args.no_jit:
                line += f" {fmt(get(r, 'nj', idx))}"
            line += f" {fmt(get(r, 'py', idx))}   {ratio(get(r, 'ixo', idx), get(r, 'cpp', idx)):>7}"
            line += f" {ratio(get(r, 'ixo', idx), get(r, 'py', idx)):>7}"
            if not r["ok"]:
                line += "  DIFF"
            print(line)
            for k in total:
                total[k] += get(r, k, idx) or 0.0
        print("-" * len(head))
        line = f"{'total':<22} {fmt(total['cpp'])} {fmt(total['ixo'])}"
        if args.no_jit:
            line += f" {fmt(total['nj'])}"
        print(line + f" {fmt(total['py'])}")
    for r in rows:
        if not r["ok"]:
            print(f"\n{r['name']}: outputs differ")
            for k in ("cpp", "ixo", "py"):
                if k in r:
                    print(f"    {k:<4} {r[k][2][:200]}")
    print(f"\nBest of {args.runs} runs, including process start-up. Memory is the peak working set (Windows)")
    print("or maximum resident set (Linux, macOS): the most RAM the process used at once.")


if __name__ == "__main__":
    main()
