#!/usr/bin/env python3
"""Smoke test for `ixo dap`: drives a debug session over stdio (breakpoints,
stack, variables, stepping, evaluate, stop on panic, program output).

Usage: python tests/tools/dap_smoke.py [path/to/ixo]
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IXO = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "release", "ixo.exe" if os.name == "nt" else "ixo")

PROGRAM = """class Point(x, y)
    fn len2 = self.x * self.x + self.y * self.y

fn total(points)
    sum = 0
    for p in points
        sum += p.len2
    sum

pts = [Point(1, 2), Point(3, 4)]
print("start")
t = total(pts)
print(f"total {t}")
data = {"a": 1}
bad = data["missing"]
"""


class Client:
    def __init__(self):
        self.p = subprocess.Popen([IXO, "dap"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.seq = 1
        self.pending = []

    def send(self, command, arguments=None):
        msg = {"seq": self.seq, "type": "request", "command": command, "arguments": arguments or {}}
        self.seq += 1
        body = json.dumps(msg).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()
        return msg["seq"]

    def read(self):
        length = 0
        while True:
            line = self.p.stdout.readline().decode().strip()
            if not line:
                if length:
                    break
                continue
            if line.lower().startswith("content-length:"):
                length = int(line.split(":")[1])
        return json.loads(self.p.stdout.read(length))

    def request(self, command, arguments=None):
        seq = self.send(command, arguments)
        while True:
            msg = self.read()
            if msg["type"] == "response" and msg["request_seq"] == seq:
                return msg
            self.pending.append(msg)

    def wait_event(self, name):
        for i, m in enumerate(self.pending):
            if m["type"] == "event" and m["event"] == name:
                return self.pending.pop(i)
        while True:
            msg = self.read()
            if msg["type"] == "event" and msg["event"] == name:
                return msg
            self.pending.append(msg)

    def output(self):
        return "".join(m["body"]["output"] for m in self.pending if m["type"] == "event" and m["event"] == "output")


def main():
    failures = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what)
        if not cond:
            failures.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "prog.ixo")
        with open(path, "w", newline="\n") as fh:
            fh.write(PROGRAM)
        c = Client()
        r = c.request("initialize", {"adapterID": "ixo", "linesStartAt1": True})
        check(r["success"] and r["body"]["supportsConfigurationDoneRequest"], "initialize")
        c.wait_event("initialized")
        check(c.request("launch", {"program": path})["success"], "launch")
        r = c.request("setBreakpoints", {"source": {"path": path}, "breakpoints": [{"line": 7}]})
        check(r["body"]["breakpoints"][0]["verified"], "breakpoint set")
        c.request("setExceptionBreakpoints", {"filters": ["panic"]})
        c.request("configurationDone")

        ev = c.wait_event("stopped")
        check(ev["body"]["reason"] == "breakpoint", f"stopped at breakpoint: {ev['body']}")
        check("start" in c.output(), "program output arrives as output events")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check([f["name"] for f in st[:2]] == ["total", "<main>"] and st[0]["line"] == 7 and st[1]["line"] == 12,
              f"stack trace: {[(f['name'], f['line']) for f in st]}")
        scopes = c.request("scopes", {"frameId": st[0]["id"]})["body"]["scopes"]
        local_ref = scopes[0]["variablesReference"]
        vs = {v["name"]: v for v in c.request("variables", {"variablesReference": local_ref})["body"]["variables"]}
        check(vs.get("sum", {}).get("value") == "0" and vs.get("p", {}).get("value") == "Point(x: 1, y: 2)",
              f"locals: { {k: v['value'] for k, v in vs.items()} }")
        kids = c.request("variables", {"variablesReference": vs["points"]["variablesReference"]})["body"]["variables"]
        check([k["name"] for k in kids] == ["[0]", "[1]"], f"expand a list: {[k['name'] for k in kids]}")
        g = c.request("variables", {"variablesReference": scopes[1]["variablesReference"]})["body"]["variables"]
        check(any(v["name"] == "pts" for v in g), f"module variables: {[v['name'] for v in g]}")
        e = c.request("evaluate", {"expression": "points[1].y", "frameId": st[0]["id"]})
        check(e["success"] and e["body"]["result"] == "4", f"evaluate: {e.get('body') or e.get('message')}")
        e = c.request("evaluate", {"expression": "nope", "frameId": st[0]["id"]})
        check(not e["success"], "evaluate an unknown name fails")

        # step in: into len2
        c.request("stepIn", {"threadId": 1})
        c.wait_event("stopped")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check(st[0]["name"].endswith("len2") and st[0]["line"] == 2, f"step in: {st[0]['name']}:{st[0]['line']}")
        # step out: back in total, still line 7
        c.request("stepOut", {"threadId": 1})
        c.wait_event("stopped")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check(st[0]["name"] == "total", f"step out: {st[0]['name']}:{st[0]['line']}")
        # remove the breakpoint and step over lines of total
        c.request("setBreakpoints", {"source": {"path": path}, "breakpoints": []})
        c.request("next", {"threadId": 1})
        c.wait_event("stopped")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check(st[0]["name"] == "total" and st[0]["line"] in (6, 7, 8), f"step over: {st[0]['name']}:{st[0]['line']}")

        # continue to the panic
        c.request("continue", {"threadId": 1})
        ev = c.wait_event("stopped")
        check(ev["body"]["reason"] == "exception" and "missing" in ev["body"].get("text", ""),
              f"stopped on panic: {ev['body']}")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check(st[0]["line"] == 15, f"panic location: line {st[0]['line']}")
        c.request("continue", {"threadId": 1})
        ex = c.wait_event("exited")
        check(ex["body"]["exitCode"] == 1, f"exit code after panic: {ex['body']}")
        c.wait_event("terminated")
        check("total 30" in c.output() and "panic:" in c.output(), "output and panic message delivered")
        c.request("disconnect")
        c.p.wait(timeout=10)
        check(c.p.returncode == 0, "adapter exits after disconnect")

        # conditional breakpoint, hit count, logpoint, evaluate, set a variable
        c = Client()
        c.request("initialize", {"adapterID": "ixo"})
        c.request("launch", {"program": path})
        c.request("setBreakpoints", {"source": {"path": path}, "breakpoints": [
            {"line": 2, "condition": "self.x == 3"},             # only for Point(3, 4)
            {"line": 5, "logMessage": "starting with {points.len} points"},
            {"line": 7, "hitCondition": "2"},                   # the second time only
        ]})
        c.request("setExceptionBreakpoints", {"filters": []})
        c.request("configurationDone")
        # Point(1, 2): line 7 is hit once (no stop) and the condition is false; then line 7 again
        ev = c.wait_event("stopped")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check(st[0]["name"] == "total" and st[0]["line"] == 7, f"hit count breakpoint: {st[0]['name']}:{st[0]['line']}")
        check("starting with 2 points" in c.output(), "logpoint message")
        e = c.request("evaluate", {"expression": "points.map(_.len2).sum", "frameId": st[0]["id"], "context": "repl"})
        check(e["success"] and e["body"]["result"] == "30", f"evaluate an expression: {e.get('body') or e.get('message')}")
        e = c.request("evaluate", {"expression": "p.len2", "frameId": st[0]["id"], "context": "hover"})
        check(not e["success"], "hover doesn't run code")
        scopes = c.request("scopes", {"frameId": st[0]["id"]})["body"]["scopes"]
        r = c.request("setVariable", {"variablesReference": scopes[0]["variablesReference"], "name": "sum", "value": "sum + 1000"})
        check(r["success"] and r["body"]["value"] == "1005", f"set a variable: {r.get('body') or r.get('message')}")
        c.request("continue", {"threadId": 1})
        ev = c.wait_event("stopped")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check(st[0]["name"].endswith("len2") and st[0]["line"] == 2, f"conditional breakpoint: {st[0]['name']}:{st[0]['line']}")
        e = c.request("evaluate", {"expression": "self.x * 10 + self.y", "frameId": st[0]["id"], "context": "repl"})
        check(e["success"] and e["body"]["result"] == "34", f"evaluate with self: {e.get('body') or e.get('message')}")
        c.request("continue", {"threadId": 1})
        c.wait_event("terminated")
        check("total 1030" in c.output(), "the new value is used: " + repr(c.output()[-80:]))
        c.request("disconnect")
        c.p.wait(timeout=10)

        # debugging one @test function
        tpath = os.path.join(tmp, "t.ixo")
        with open(tpath, "w", newline="\n") as fh:
            fh.write("fn double(x) = x * 2\n\n@test\nfn doubles\n    r = double(21)\n    assert_eq(r, 42)\n")
        c = Client()
        c.request("initialize", {"adapterID": "ixo"})
        c.request("launch", {"program": tpath, "test": "doubles"})
        c.request("setBreakpoints", {"source": {"path": tpath}, "breakpoints": [{"line": 6}]})
        c.request("configurationDone")
        ev = c.wait_event("stopped")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check(st[0]["name"] == "doubles" and st[0]["line"] == 6, f"debug a test: {st[0]['name']}:{st[0]['line']}")
        c.request("continue", {"threadId": 1})
        c.wait_event("terminated")
        check("test doubles passed" in c.output(), "test result reported")
        c.request("disconnect")
        c.p.wait(timeout=10)

        # breakpoints inside par tasks stop that worker's thread
        ppath = os.path.join(tmp, "p.ixo")
        with open(ppath, "w", newline="\n") as fh:
            fh.write("par fn work(n)\n    doubled = n * 2\n    doubled + 1\n\n"
                     "t = work(20)\nprint(\"result\", t.await)\n")
        c = Client()
        c.request("initialize", {"adapterID": "ixo"})
        c.request("launch", {"program": ppath})
        c.request("setBreakpoints", {"source": {"path": ppath}, "breakpoints": [{"line": 3}]})
        c.request("configurationDone")
        ev = c.wait_event("stopped")
        tid = ev["body"]["threadId"]
        check(tid != 1, f"stopped in a par worker thread: {tid}")
        names = [t["name"] for t in c.request("threads")["body"]["threads"]]
        check(any(n.startswith("par worker") for n in names), f"threads list the workers: {names}")
        st = c.request("stackTrace", {"threadId": tid})["body"]["stackFrames"]
        check(st[0]["name"] == "work" and st[0]["line"] == 3, f"worker stack: {[(f['name'], f['line']) for f in st]}")
        scopes = c.request("scopes", {"frameId": st[0]["id"]})["body"]["scopes"]
        vs = {v["name"]: v["value"] for v in c.request("variables", {"variablesReference": scopes[0]["variablesReference"]})["body"]["variables"]}
        check(vs.get("n") == "20" and vs.get("doubled") == "40", f"worker variables: {vs}")
        e = c.request("evaluate", {"expression": "doubled", "frameId": st[0]["id"], "context": "repl"})
        check(e["success"] and e["body"]["result"] == "40", "evaluate a worker variable")
        c.request("setBreakpoints", {"source": {"path": ppath}, "breakpoints": [{"line": 2, "logMessage": "n is {n}"}]})
        c.request("continue", {"threadId": tid})
        c.wait_event("terminated")
        check("result 41" in c.output(), "the par task finishes after continuing")
        c.request("disconnect")
        c.p.wait(timeout=10)

        # stopOnEntry
        c = Client()
        c.request("initialize", {"adapterID": "ixo"})
        c.request("launch", {"program": path, "stopOnEntry": True})
        c.request("configurationDone")
        ev = c.wait_event("stopped")
        st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
        check(ev["body"]["reason"] == "step" and st[0]["line"] == 1, f"stop on entry: line {st[0]['line']}")
        c.request("pause", {"threadId": 1})
        c.request("disconnect")
        c.p.wait(timeout=10)

    print(f"\n{'all passed' if not failures else str(len(failures)) + ' failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
