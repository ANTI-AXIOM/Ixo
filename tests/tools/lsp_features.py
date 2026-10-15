#!/usr/bin/env python3
"""Language server features beyond the basics: docstrings, quick fixes, inlay
hints, semantic tokens, snippets and workspace-wide references and rename.
Uses a temporary workspace with a module that is never opened in the editor.

Usage: python tests/tools/lsp_features.py [path/to/ixo]
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lsp_smoke  # noqa: E402  (Client, IXO)

GEOMETRY = '''# Area of a w by h rectangle.
fn area*(w, h) = w * h

fn secret(x) = x
'''

MAIN = '''use lib.geometry

fn greet(name, loud)
    """Says hello.

    Shouts when loud is true."""
    if loud -> return name.up
    name

interface Shape
    fn area
    fn describe = "shape"

class Box(w, h) is Shape
    fn size = self.w

enum Color
    Red
    Green
    Rgb(r, g, b)

fn name(c: Color) = match c
    Color.Red -> "red"

total = area(2, 3)
msg = greet("ann", true)
print(totl)
print(geometry.secret(1))
print(volume(1, 2))
for i in [1, 2]
    print(i)
scale = 2.5
'''

VOLUME = '''fn volume*(a, b) = a * b
'''


def uri_of(path):
    return "file:///" + path.replace("\\", "/")


def main():
    failures = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    with tempfile.TemporaryDirectory() as ws:
        os.makedirs(os.path.join(ws, "lib"))
        os.makedirs(os.path.join(ws, "extra"))
        for rel, text in (("lib/geometry.ixo", GEOMETRY), ("main.ixo", MAIN), ("extra/solids.ixo", VOLUME)):
            with open(os.path.join(ws, rel), "w", newline="\n") as fh:
                fh.write(text)
        main_path = os.path.join(ws, "main.ixo")
        uri = uri_of(main_path)
        c = lsp_smoke.Client()
        init = c.request("initialize", {"processId": None, "rootUri": uri_of(ws), "capabilities": {}})
        caps = init["result"]["capabilities"]
        check(caps.get("inlayHintProvider") and "semanticTokensProvider" in caps and "codeActionProvider" in caps,
              "capabilities advertised")
        c.notify("initialized", {})
        c.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "ixo", "version": 1, "text": MAIN}})
        diag = c.read()
        diags = diag["params"]["diagnostics"]
        msgs = [d["message"] for d in diags]

        def hover(line, ch):
            r = c.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": line, "character": ch}})
            res = r.get("result")
            return res["contents"]["value"] if res else ""

        h = hover(25, 7)
        check("Says hello." in h and "Shouts when loud" in h, "docstring in hover: " + repr(h))
        h = hover(24, 9)
        check("Area of a w by h rectangle." in h, "comment above a declaration in hover: " + repr(h))

        def actions_for(substr):
            d = next((d for d in diags if substr in d["message"]), None)
            if not d:
                return None, []
            r = c.request("textDocument/codeAction", {"textDocument": {"uri": uri}, "range": d["range"],
                                                      "context": {"diagnostics": [d]}})
            return d, r["result"]

        d, acts = actions_for("doesn't define 'area'")
        text = acts[0]["edit"]["changes"][uri][0]["newText"] if acts else ""
        check("fn area" in text and "fn area(" not in text and "todo" in text,
              f"quick fix adds interface methods (same form as the interface): {text!r}")
        d, acts = actions_for("doesn't handle")
        text = acts[0]["edit"]["changes"][uri][0]["newText"] if acts else ""
        check("Color.Green -> todo" in text and "Color.Rgb(r, g, b) -> todo" in text, f"quick fix adds match arms: {text!r}")
        d, acts = actions_for("undefined name 'totl'")
        check(any(a["title"] == "Change to 'total'" for a in acts), f"did-you-mean fix: {[a['title'] for a in acts]}")
        d, acts = actions_for("undefined name 'volume'")
        titles = [a["title"] for a in acts]
        check("Import 'volume' from extra.solids" in titles, f"import fix: {titles}")
        d, acts = actions_for("mark it with *")
        edits = acts[0]["edit"]["changes"] if acts else {}
        geo = [k for k in edits if k.endswith("geometry.ixo")]
        check(bool(geo) and edits[geo[0]][0]["newText"] == "*" and edits[geo[0]][0]["range"]["start"] == {"line": 3, "character": 9},
              f"export fix edits the other module: {edits}")

        # old spellings: a quick fix each, and one for the whole file
        old_uri = uri_of(os.path.join(ws, "old.ixo"))
        old_text = "var n = 1\nok = !false && true\nfor i in 0..<3, 1 -> print(i)\n"
        c.notify("textDocument/didOpen", {"textDocument": {"uri": old_uri, "languageId": "ixo", "version": 1,
                                                           "text": old_text}})
        od = c.read()
        while od.get("method") != "textDocument/publishDiagnostics" or od["params"]["uri"] != old_uri:
            od = c.read()
        old = [d for d in od["params"]["diagnostics"] if d["message"].startswith("deprecated:")]
        check(len(old) >= 5, f"old spellings are reported: {[d['message'] for d in old]}")
        if old:
            r = c.request("textDocument/codeAction", {"textDocument": {"uri": old_uri}, "range": old[0]["range"],
                                                      "context": {"diagnostics": old}})
            titles = [a["title"] for a in r["result"]]
            check("Use the new spelling" in titles, f"quick fix for an old spelling: {titles}")
            whole = [a for a in r["result"] if a["title"].startswith("Update every old spelling")]
            new_text = whole[0]["edit"]["changes"][old_uri][0]["newText"] if whole else ""
            check(new_text == "n = 1\nok = not false and true\nfor i in 0..3 by 1 -> print(i)\n",
                  f"the whole file updated: {new_text!r}")

        # strict code: its errors come with quick fixes
        strict_uri = uri_of(os.path.join(ws, "strict.ixo"))
        strict_text = ('$strict\nfs.write("a.txt", "x")\nf = fs.open("a.txt")\nfn g(v) = v\nh = g\n'
                       'lib = ffi.load("c")\n')
        c.notify("textDocument/didOpen", {"textDocument": {"uri": strict_uri, "languageId": "ixo", "version": 1,
                                                           "text": strict_text}})
        sd = c.read()
        while sd.get("method") != "textDocument/publishDiagnostics" or sd["params"]["uri"] != strict_uri:
            sd = c.read()
        sdiags = sd["params"]["diagnostics"]

        def strict_fix(substr):
            d = next((d for d in sdiags if substr in d["message"]), None)
            if not d:
                return None
            r = c.request("textDocument/codeAction", {"textDocument": {"uri": strict_uri}, "range": d["range"],
                                                      "context": {"diagnostics": [d]}})
            acts = [a for a in r["result"] if a["title"] == "Apply the suggested fix"]
            return acts[0]["edit"]["changes"][strict_uri][0]["newText"] if acts else None
        check(strict_fix("is ignored") == "_ = ", "strict: an ignored error gets _ = ")
        check(strict_fix("bind what fs.open") == "use ", "strict: fs.open gets use")
        check(strict_fix("needs a type in strict code") == ": any", "strict: a parameter gets : any")
        check(strict_fix("reaches raw memory") == "unsafe -> ", "strict: ffi gets unsafe ->")

        r = c.request("textDocument/inlayHint", {"textDocument": {"uri": uri},
                                                  "range": {"start": {"line": 0, "character": 0}, "end": {"line": 40, "character": 0}}})
        labels = {(h["position"]["line"], h["label"]) for h in r["result"]}
        check((31, ": float") in labels, f"type hint for a variable: {sorted(labels)}")
        check((25, "name:") in labels and (25, "loud:") in labels, "parameter name hints")
        check((29, ": int") in labels, "type hint for a loop variable")

        r = c.request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})
        data = r["result"]["data"]
        legend = caps["semanticTokensProvider"]["legend"]["tokenTypes"]
        toks, line, col = [], 0, 0
        for i in range(0, len(data), 5):
            dl, dc, ln, ty, mods = data[i:i + 5]
            line += dl
            col = col + dc if dl == 0 else dc
            toks.append((line, col, ln, legend[ty]))
        check((2, 3, 5, "function") in toks, "semantic token: function declaration")
        check((13, 6, 3, "class") in toks and (9, 10, 5, "interface") in toks and (16, 5, 5, "enum") in toks,
              "semantic tokens: class, interface, enum")
        check((17, 4, 3, "enumMember") in toks, "semantic token: enum variant")
        check((6, 7, 4, "parameter") in toks, "semantic token: parameter use")

        # snippets, and all variants after `match c`
        doc2 = MAIN + "fn show(c: Color)\n    match c\n        \n"
        c.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": doc2}]})
        c.read()
        r = c.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 34, "character": 8}})
        items = r["result"]
        arms = next((i for i in items if i["label"] == "all Color variants"), None)
        check(arms is not None and "Color.Rgb(r, g, b) -> todo" in arms["insertText"], "completion: all enum variants")
        check(any(i.get("insertTextFormat") == 2 and i["filterText"] == "interface" for i in items), "completion: snippets")

        # workspace-wide: references to `area` include the unopened module and main.ixo
        r = c.request("textDocument/references", {"textDocument": {"uri": uri}, "position": {"line": 24, "character": 9},
                                                  "context": {"includeDeclaration": True}})
        files = sorted({os.path.basename(l["uri"]) for l in r["result"]})
        check(files == ["geometry.ixo", "main.ixo"], f"references across the workspace: {files}")
        r = c.request("textDocument/rename", {"textDocument": {"uri": uri}, "position": {"line": 24, "character": 9},
                                              "newName": "surface"})
        changes = r["result"]["changes"]
        check(any(k.endswith("geometry.ixo") for k in changes) and uri in changes, "rename edits the unopened module")

        c.request("shutdown", None)
        c.notify("exit", None)
        c.p.wait(timeout=10)
    print(f"\n{'all passed' if not failures else str(len(failures)) + ' failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
