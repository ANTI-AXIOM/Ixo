#!/usr/bin/env python3
"""Drives the REPL's line editor with real key presses (history, Tab completion,
cursor keys) and checks the results. On Windows it uses a new console; on
Linux and macOS a pseudo-terminal.

Usage: python tests/tools/repl_keys.py [path/to/ixo]
"""
import ctypes
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IXO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")

# the session both versions type; `want` are the lines the REPL must show
WANT_RESULTS = ["21", "22", "122", "2"]


def main_posix():
    import pty
    import select
    home = os.path.join("/tmp", "ixo_repl_keys")
    os.makedirs(home, exist_ok=True)
    hist = os.path.join(home, ".ixo_history")
    if os.path.exists(hist):
        os.remove(hist)
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["HOME"] = home
        os.execv(IXO, [IXO, "repl"])
    out = bytearray()

    def drain(wait=0.2):
        end = time.time() + wait
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    out.extend(os.read(fd, 4096))
                except OSError:
                    return

    def send(s):
        os.write(fd, s.encode())
        drain()

    drain(1.0)
    up, home_key, back, tab, enter = "\x1b[A", "\x1b[H", "\x7f", "\t", "\r"
    send("value = 20" + enter)
    send("val" + tab)
    send(" + 1" + enter)            # 21
    send(up + back + "2" + enter)   # 22
    send(up + home_key + "100 + " + enter)  # 122
    send("[1, 2].le" + tab)
    send(enter)                     # 2
    send(":quit" + enter)
    drain(1.0)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out.decode("utf-8", "replace"))
    lines = [l.strip() for l in re.split(r"[\r\n]+", text) if l.strip()]
    results = [l for l in lines if not l.startswith(">>>") and not l.startswith("...") and l in WANT_RESULTS]
    failures = []
    if results != WANT_RESULTS:
        failures.append(f"results {results} != {WANT_RESULTS}")
    for want in ("value + 1", "value + 2", "100 + value + 2", "[1, 2].len"):
        if not any(l.endswith(want) for l in lines):
            failures.append(f"missing line {want!r}")
    if failures:
        sys.stderr.write("\n".join(lines) + "\n\n")
        for f in failures:
            sys.stderr.write("FAIL " + f + "\n")
    print("all passed" if not failures else f"{len(failures)} failed")
    return 1 if failures else 0


if os.name == "nt":
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class KEY_EVENT_RECORD(ctypes.Structure):
        _fields_ = [("bKeyDown", wintypes.BOOL), ("wRepeatCount", wintypes.WORD), ("wVirtualKeyCode", wintypes.WORD),
                    ("wVirtualScanCode", wintypes.WORD), ("uChar", wintypes.WCHAR),
                    ("dwControlKeyState", wintypes.DWORD)]

    class INPUT_RECORD(ctypes.Structure):
        class _U(ctypes.Union):
            _fields_ = [("KeyEvent", KEY_EVENT_RECORD), ("pad", ctypes.c_byte * 16)]
        _fields_ = [("EventType", wintypes.WORD), ("Event", _U)]


VK = {"enter": 0x0D, "tab": 0x09, "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27, "home": 0x24, "end": 0x23,
      "back": 0x08}


def main():
    if os.name != "nt":
        return main_posix()
    env = dict(os.environ)
    env["USERPROFILE"] = os.path.join(os.environ.get("TEMP", "."), "ixo_repl_keys")  # fresh history file
    os.makedirs(env["USERPROFILE"], exist_ok=True)
    hist = os.path.join(env["USERPROFILE"], ".ixo_history")
    if os.path.exists(hist):
        os.remove(hist)
    p = subprocess.Popen([IXO, "repl"], creationflags=subprocess.CREATE_NEW_CONSOLE, env=env)
    time.sleep(1.0)
    k32.FreeConsole()
    if not k32.AttachConsole(p.pid):
        print("couldn't attach to the console:", ctypes.get_last_error())
        p.kill()
        return 1
    GENERIC_RW = 0xC0000000
    SHARE = 3
    hin = k32.CreateFileW("CONIN$", GENERIC_RW, SHARE, None, 3, 0, None)
    hout = k32.CreateFileW("CONOUT$", GENERIC_RW, SHARE, None, 3, 0, None)

    def key(vk=0, ch="\0", ctrl=False):
        recs = (INPUT_RECORD * 2)()
        for i, down in enumerate((True, False)):
            recs[i].EventType = 1
            ke = recs[i].Event.KeyEvent
            ke.bKeyDown = down
            ke.wRepeatCount = 1
            ke.wVirtualKeyCode = vk
            ke.uChar = ch
            ke.dwControlKeyState = 0x0008 if ctrl else 0
        n = wintypes.DWORD()
        k32.WriteConsoleInputW(hin, recs, 2, ctypes.byref(n))

    def type_text(s):
        for c in s:
            key(ord(c.upper()) if c.isalnum() else 0, c)

    def press(name, times=1):
        for _ in range(times):
            key(VK[name], "\r" if name == "enter" else ("\t" if name == "tab" else "\0"))
        time.sleep(0.15)

    type_text("value = 20")
    press("enter")
    type_text("val")
    press("tab")                 # completes to "value"
    type_text(" + 1")
    press("enter")               # 21
    press("up")                  # recalls "value + 1"
    press("back")
    type_text("2")
    press("enter")               # 22
    press("up")
    press("home")
    type_text("100 + ")          # "100 + value + 2"
    press("enter")               # 122
    type_text("[1, 2].le")
    press("tab")                 # member completion: len
    press("enter")               # 2
    time.sleep(0.5)

    class COORD(ctypes.Structure):
        _fields_ = [("X", wintypes.SHORT), ("Y", wintypes.SHORT)]
    screen = []
    for row in range(0, 40):
        buf = ctypes.create_unicode_buffer(120)
        n = wintypes.DWORD()
        k32.ReadConsoleOutputCharacterW(hout, buf, 120, COORD(0, row), ctypes.byref(n))
        screen.append(buf.value.rstrip())
    type_text(":quit")
    press("enter")
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
    k32.FreeConsole()
    text = "\n".join(l for l in screen if l)
    failures = []
    for want in (">>> value + 1", "21", ">>> value + 2", "22", ">>> 100 + value + 2", "122", ">>> [1, 2].len", "2"):
        if not any(l.strip() == want for l in screen):
            failures.append(want)
    sys.stderr.write(text + "\n\n")
    for want in failures:
        sys.stderr.write(f"FAIL missing line {want!r}\n")
    print("all passed" if not failures else f"{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
