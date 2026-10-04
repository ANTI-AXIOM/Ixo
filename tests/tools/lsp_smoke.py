#!/usr/bin/env python3
"""Smoke test for `ixo lsp`: drives a real session over stdio and checks the
answers to diagnostics, hover, definition, completion, symbols, formatting,
references, rename and signature help.

Usage: python tests/tools/lsp_smoke.py [path/to/ixo]
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IXO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")

DOC = """class Player(name, hp: int)
    fn hit(dmg: int)
        self.hp -= dmg

fn greet(p: Player): str
    f"hello {p.name}"

hero = Player("Ann", 100)
hero.hit(5)
print(greet(hero))
print(hero.hp.upx)
nums = [1, 2, 3]
x=1
"""


class Client:
    def __init__(self):
        self.p = subprocess.Popen([IXO, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 1

    def send(self, msg):
        body = json.dumps(msg).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def read(self):
        length = 0
        while True:
            line = self.p.stdout.readline().decode().strip()
            if not line:
                break
            if line.lower().startswith("content-length:"):
                length = int(line.split(":")[1])
        return json.loads(self.p.stdout.read(length))

    def request(self, method, params):
        mid = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": mid, "method": method, "params": params})
        while True:
            msg = self.read()
            if msg.get("id") == mid:
                return msg
            self.notifications.append(msg)

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    notifications = []


def main():
    uri = "file:///" + os.path.join(ROOT, "tests", "tools", "_lsp_doc.ixo").replace("\\", "/")
    c = Client()
    failures = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    init = c.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
    check("hoverProvider" in init["result"]["capabilities"], "initialize advertises hover")
    c.notify("initialized", {})
    c.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "ixo", "version": 1, "text": DOC}})
    diag = c.read()
    check(diag["method"] == "textDocument/publishDiagnostics", "diagnostics published on open")
    msgs = [d["message"] for d in diag["params"]["diagnostics"]]
    check(any("int has no method 'upx'" in m for m in msgs), "type error reported: " + "; ".join(msgs))
    if msgs:
        d0 = diag["params"]["diagnostics"][0]["range"]["start"]
        check(d0 == {"line": 10, "character": 14}, f"diagnostic position {d0}")

    def hover(line, ch):
        r = c.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": line, "character": ch}})
        res = r.get("result")
        return res["contents"]["value"] if res else ""

    h = hover(9, 7)
    check("fn greet(p: Player): str" in h, "hover on a function call: " + repr(h))
    h = hover(8, 6)
    check("fn hit(dmg: int)" in h, "hover on a method: " + repr(h))
    h = hover(7, 1)
    check("hero: Player" in h, "hover on a variable shows its type: " + repr(h))
    h = hover(9, 1)
    check("print" in h and "Prints" in h, "hover on a builtin shows docs: " + repr(h))

    r = c.request("textDocument/definition", {"textDocument": {"uri": uri}, "position": {"line": 8, "character": 6}})
    loc = r["result"]
    check(loc and loc["range"]["start"] == {"line": 1, "character": 7}, f"definition of a method: {loc}")

    # member completion after "hero."
    new_doc = DOC + "hero.\n"
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": new_doc}]})
    c.read()  # diagnostics
    r = c.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 13, "character": 5}})
    labels = {i["label"] for i in r["result"]}
    check({"name", "hp", "hit"} <= labels, f"member completion for a class instance: {sorted(labels)[:10]}")
    r = c.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 11, "character": 7}})
    labels = {i["label"] for i in r["result"]}
    check({"print", "hero", "greet", "fn", "fs"} <= labels, "identifier completion includes builtins, globals, keywords")

    r = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    names = [s["name"] for s in r["result"]]
    check(names[:3] == ["Player", "greet", "hero"], f"document symbols: {names}")

    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": DOC}]})
    c.read()
    r = c.request("textDocument/formatting", {"textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": True}})
    edits = r["result"]
    check(len(edits) == 1 and "x = 1" in edits[0]["newText"], "formatting returns an edit")

    # references: the class Player is used in its declaration, an annotation-free call and greet's param type
    r = c.request("textDocument/references", {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 1},
                                              "context": {"includeDeclaration": True}})
    lines = sorted(l["range"]["start"]["line"] for l in r["result"])
    check(lines == [7, 8, 9, 10], f"references of a variable: {lines}")
    r = c.request("textDocument/references", {"textDocument": {"uri": uri}, "position": {"line": 2, "character": 14},
                                              "context": {"includeDeclaration": True}})
    got = sorted((l["range"]["start"]["line"], l["range"]["start"]["character"]) for l in r["result"])
    check((0, 19) in got and (2, 13) in got and (10, 11) in got, f"references of a field: {got}")

    r = c.request("textDocument/prepareRename", {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 4}})
    check(r.get("result", {}).get("placeholder") == "greet", f"prepareRename: {r}")
    r = c.request("textDocument/rename", {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 4},
                                          "newName": "welcome"})
    edits = r["result"]["changes"].get(uri, [])
    check(sorted(e["range"]["start"]["line"] for e in edits) == [4, 9] and all(e["newText"] == "welcome" for e in edits),
          f"rename a function: {edits}")
    r = c.request("textDocument/rename", {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 4},
                                          "newName": "match"})
    check("error" in r, "rename to a keyword is refused")

    sig_doc = DOC + "greet(hero, \nhero.hit(\nprint(1, 2, \n"
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 4}, "contentChanges": [{"text": sig_doc}]})
    c.read()

    def sighelp(line, ch):
        r = c.request("textDocument/signatureHelp", {"textDocument": {"uri": uri}, "position": {"line": line, "character": ch}})
        return r.get("result") or {}

    s = sighelp(13, 12)
    check(s.get("signatures", [{}])[0].get("label", "").startswith("greet(p: Player)") and s.get("activeParameter") == 1,
          f"signature help for a function: {s}")
    s = sighelp(14, 9)
    check(s.get("signatures", [{}])[0].get("label", "").startswith("hit(dmg: int)"), f"signature help for a method: {s}")
    s = sighelp(15, 12)
    check(s.get("signatures", [{}])[0].get("label", "").startswith("print(") and s.get("activeParameter") == 2,
          f"signature help for a builtin: {s}")

    enum_doc = """enum Color
    Red
    Rgb(r, g, b)

interface Shape
    fn area

c = Color.Rgb(1, 2, 3)
"""
    c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 5}, "contentChanges": [{"text": enum_doc}]})
    c.read()
    h = hover(0, 6)
    check("enum Color" in h and "Rgb(r, g, b)" in h, "hover on an enum: " + repr(h))
    h = hover(4, 11)
    check("interface Shape" in h and "fn area" in h, "hover on an interface: " + repr(h))
    h = hover(7, 11)
    check("Color.Rgb(r, g, b)" in h, "hover on an enum variant: " + repr(h))
    r = c.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    kinds = {s["name"]: s["kind"] for s in r["result"]}
    check(kinds.get("Color") == 10 and kinds.get("Shape") == 11, f"enum / interface symbols: {kinds}")

    r = c.request("shutdown", None)
    check("result" in r, "shutdown")
    c.notify("exit", None)
    c.p.wait(timeout=10)
    check(c.p.returncode == 0, "exit code 0 after shutdown")
    print(f"\n{'all passed' if not failures else str(len(failures)) + ' failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
