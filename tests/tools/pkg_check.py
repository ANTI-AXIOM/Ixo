#!/usr/bin/env python3
"""Checks the package manager (ixo init / add / install / update / remove)
against local git repositories and a folder dependency.

Usage: python tests/tools/pkg_check.py [path/to/ixo]
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IXO = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(
    ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")

ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
           GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_NOSYSTEM="1")
failures = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"\n     {detail}"))
    if not ok:
        failures.append(name)


def run(args, cwd):
    p = subprocess.run(args, cwd=cwd, capture_output=True, env=ENV, timeout=60)
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace").replace("\r\n", "\n")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def git(repo, *args):
    code, out = run(["git", *args], repo)
    assert code == 0, out
    return out.strip()


def commit(repo, msg):
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", msg)
    return git(repo, "rev-parse", "HEAD")


def url(path):
    return "file:///" + path.replace("\\", "/").lstrip("/")


def onerror(func, path, _):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def main():
    if shutil.which("git") is None:
        print("skipped (git isn't installed)")
        return 0
    tmp = tempfile.mkdtemp(prefix="ixo_pkg_")
    try:
        # util: a package with two versions (tag v1, then a newer commit)
        util = os.path.join(tmp, "util")
        os.makedirs(util)
        git(util, "init", "-q", "-b", "main")
        write(os.path.join(util, "ixo.toml"), '[package]\nname = "util"\nmain = "lib.ixo"\n')
        write(os.path.join(util, "lib.ixo"), 'fn twice*(x) = x * 2\nfn version* = "v1"\n')
        util_v1 = commit(util, "v1")
        git(util, "tag", "v1")

        # mathlib: depends on util by git
        mathlib = os.path.join(tmp, "ixo-mathlib")
        os.makedirs(mathlib)
        git(mathlib, "init", "-q", "-b", "main")
        write(os.path.join(mathlib, "ixo.toml"),
              f'[package]\nname = "mathlib"\n\n[dependencies]\nutil = {{ git = "{url(util)}" }}\n')
        write(os.path.join(mathlib, "lib.ixo"), 'use util\n\nfn quad*(x) = twice(twice(x))\n')
        write(os.path.join(mathlib, "extra", "stats.ixo"), 'fn mean*(xs) = xs.sum // xs.len\n')
        commit(mathlib, "first")

        # helpers: a plain folder
        helpers = os.path.join(tmp, "helpers")
        write(os.path.join(helpers, "helpers.ixo"), 'fn shout*(s) = s.up + "!"\n')

        app = os.path.join(tmp, "app")
        os.makedirs(app)
        code, out = run([IXO, "init"], app)
        check("init creates ixo.toml", code == 0 and os.path.exists(os.path.join(app, "ixo.toml")), out)
        code, out = run([IXO, "init"], app)
        check("init refuses a second time", code != 0 and "already exists" in out, out)

        code, out = run([IXO, "add", mathlib], app)
        check("add a git package", code == 0, out)
        toml = open(os.path.join(app, "ixo.toml"), encoding="utf-8").read()
        check("ixo.toml gets the dependency (named from the repo, without ixo-)", "mathlib = { git = " in toml, toml)
        check("its dependency is installed too",
              os.path.exists(os.path.join(app, "ixo_modules", "util", "lib.ixo")), out)
        lock = open(os.path.join(app, "ixo.lock"), encoding="utf-8").read()
        check("ixo.lock records both", "[packages.mathlib]" in lock and f'commit = "{util_v1}"' in lock, lock)

        code, out = run([IXO, "add", "../helpers"], app)
        check("add a folder", code == 0 and 'helpers = { path = "../helpers" }' in
              open(os.path.join(app, "ixo.toml"), encoding="utf-8").read(), out)
        gi = open(os.path.join(app, ".gitignore"), encoding="utf-8").read()
        check(".gitignore lists ixo_modules", "ixo_modules/" in gi, gi)

        write(os.path.join(app, "main.ixo"),
              'use mathlib\nuse mathlib.extra.stats\nuse util\nuse helpers\n\n'
              'print(quad(3), mean([1, 2, 3]), version(), shout("hi"))\n')
        code, out = run([IXO, "main.ixo"], app)
        check("the program uses them", code == 0 and out.strip() == "12 2 v1 HI!", out)

        # a newer util: install keeps the locked commit, update moves on
        write(os.path.join(util, "lib.ixo"), 'fn twice*(x) = x * 2\nfn version* = "v2"\n')
        util_v2 = commit(util, "v2")
        shutil.rmtree(os.path.join(app, "ixo_modules"), onerror=onerror)
        code, out = run([IXO, "install"], app)
        code2, out2 = run([IXO, "main.ixo"], app)
        check("install brings back the locked versions", code == 0 and out2.strip() == "12 2 v1 HI!", out + out2)
        code, out = run([IXO, "update", "util"], app)
        code2, out2 = run([IXO, "main.ixo"], app)
        check("update moves to the newest commit", code == 0 and out2.strip() == "12 2 v2 HI!", out + out2)
        lock = open(os.path.join(app, "ixo.lock"), encoding="utf-8").read()
        check("the lock follows", f'commit = "{util_v2}"' in lock, lock)

        # pin a tag
        code, out = run([IXO, "add", util, "--rev", "v1"], app)
        code2, out2 = run([IXO, "main.ixo"], app)
        check("--rev pins a tag", code == 0 and out2.strip() == "12 2 v1 HI!", out + out2)
        check("a direct pin wins over mathlib's wish", "keeping v1" in out, out)

        code, out = run([IXO, "add", util, "--rev", "nope"], app)
        check("an unknown rev is an error (and ixo.toml is left alone)",
              code != 0 and "nope" in out and 'rev = "v1"' in open(os.path.join(app, "ixo.toml"), encoding="utf-8").read(),
              out)

        # remove: its now unneeded dependency goes too (util is still direct here)
        code, out = run([IXO, "remove", "mathlib"], app)
        check("remove", code == 0 and not os.path.exists(os.path.join(app, "ixo_modules", "mathlib")), out)
        lock = open(os.path.join(app, "ixo.lock"), encoding="utf-8").read()
        check("the lock forgets it", "mathlib" not in lock and "[packages.util]" in lock, lock)
        code, out = run([IXO, "remove", "util", "helpers"], app)
        check("remove the rest", code == 0 and not os.path.exists(os.path.join(app, "ixo_modules", "util")), out)

        code, out = run([IXO, "remove", "nothing"], app)
        check("removing an unknown package is an error", code != 0 and "nothing" in out, out)
        code, out = run([IXO, "add", os.path.join(tmp, "missing.git")], app)
        check("a bad url is an error", code != 0, out)

        # two different packages with one name
        other = os.path.join(tmp, "other", "util")
        os.makedirs(other)
        git(other, "init", "-q", "-b", "main")
        write(os.path.join(other, "lib.ixo"), 'fn twice*(x) = x\n')
        commit(other, "x")
        run([IXO, "add", mathlib], app)
        code, out = run([IXO, "add", other], app)
        check("a name clash is reported", code != 0 and "two different packages" in out, out)

        outside = os.path.join(tmp, "outside")
        os.makedirs(outside)
        code, out = run([IXO, "install"], outside)
        check("install outside a project explains", code != 0 and "ixo init" in out, out)
    finally:
        shutil.rmtree(tmp, onerror=onerror)
    print("\nall passed" if not failures else f"\n{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
