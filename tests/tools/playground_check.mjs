#!/usr/bin/env node
// Opens the browser playground (playground/dist, built by playground/build.sh)
// in headless Chrome or Chromium through the DevTools protocol and checks that
// programs run, fail, stop and format as they should.
//
// Usage: node tests/tools/playground_check.mjs [path/to/chrome]
//        (or set CHROME; on Linux, google-chrome / chromium on PATH also work)
import { spawn, execSync } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";

const ROOT = path.resolve(import.meta.dirname, "../..");
const DIST = path.join(ROOT, "playground", "dist");
if (!fs.existsSync(path.join(DIST, "ixo.wasm"))) {
  console.log("skipped (build the playground first: playground/build.sh)");
  process.exit(0);
}

function findChrome() {
  const given = process.argv[2] || process.env.CHROME;
  if (given) return given;
  for (const name of ["google-chrome", "chromium", "chromium-browser"]) {
    try {
      return execSync(`command -v ${name}`, { stdio: ["ignore", "pipe", "ignore"] }).toString().trim();
    } catch (_) {}
  }
  const pw = path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), ".cache"), "ms-playwright");
  if (fs.existsSync(pw)) {
    for (const d of fs.readdirSync(pw).filter((d) => d.startsWith("chromium-")).sort().reverse()) {
      for (const exe of ["chrome-win64/chrome.exe", "chrome-linux/chrome", "chrome-linux64/chrome"]) {
        if (fs.existsSync(path.join(pw, d, exe))) return path.join(pw, d, exe);
      }
    }
  }
  return null;
}
const CHROME = findChrome();
if (!CHROME) {
  console.log("skipped (no Chrome or Chromium found; pass its path)");
  process.exit(0);
}

// a static server that sends the headers threads need (cross-origin isolation);
// `plain` leaves them out, like GitHub Pages, where coi-serviceworker.js adds them
const types = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm" };
const serve = (plain) => http.createServer((req, res) => {
  const p = path.join(DIST, decodeURIComponent(new URL(req.url, "http://x").pathname).replace(/\/$/, "/index.html"));
  if (!p.startsWith(DIST) || !fs.existsSync(p)) {
    res.writeHead(404).end();
    return;
  }
  const headers = { "Content-Type": types[path.extname(p)] || "application/octet-stream" };
  if (!plain) {
    headers["Cross-Origin-Opener-Policy"] = "same-origin";
    headers["Cross-Origin-Embedder-Policy"] = "require-corp";
  }
  res.writeHead(200, headers);
  fs.createReadStream(p).pipe(res);
});
const server = serve(false), plainServer = serve(true);
await new Promise((r) => server.listen(0, "127.0.0.1", r));
await new Promise((r) => plainServer.listen(0, "127.0.0.1", r));
const BASE = `http://127.0.0.1:${server.address().port}/`;
const PLAIN = `http://127.0.0.1:${plainServer.address().port}/`;

const profile = fs.mkdtempSync(path.join(os.tmpdir(), "ixo-chrome-"));
const chrome = spawn(CHROME, ["--headless=new", "--enable-unsafe-webgpu", "--enable-features=Vulkan,WebGPUService", "--no-first-run", "--no-sandbox",
  "--remote-debugging-port=0", "--user-data-dir=" + profile, "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
// Chrome prints "DevTools listening on ws://..." once it's ready
const browserWs = await new Promise((resolve, reject) => {
  let buf = "";
  chrome.stderr.on("data", (d) => {
    buf += d;
    const m = buf.match(/DevTools listening on (ws:\/\/\S+)/);
    if (m) resolve(m[1]);
  });
  chrome.on("exit", () => reject(new Error("Chrome exited: " + buf)));
  setTimeout(() => reject(new Error("Chrome didn't start: " + buf)), 30000);
});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let ws, sessionId, nextId = 1;
const waiting = new Map();
function send(method, params = {}) {
  const id = nextId++;
  ws.send(JSON.stringify(sessionId ? { id, method, params, sessionId } : { id, method, params }));
  return new Promise((r) => waiting.set(id, r));
}
async function evaluate(expr) {
  const r = await send("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
  return r.result?.result?.value;
}
const text = async (id) => (await evaluate(`document.getElementById(${JSON.stringify(id)})?.textContent`)) || "";
const b64 = (s) => Buffer.from(s).toString("base64url");

let failed = 0;
function report(name, ok, detail) {
  console.log((ok ? "ok   " : "FAIL ") + name + (ok ? "" : `\n     ${detail}`));
  if (!ok) failed++;
}
async function waitStatus(stopAfter) {
  let st = "";
  for (let i = 0; i < 150; i++) {
    await sleep(200);
    if (stopAfter && i === stopAfter) await evaluate("document.getElementById('stop').click()");
    st = await text("status");
    if (st && !st.endsWith("…")) break;
  }
  return st;
}
async function runs(name, src, want, wantStatus, stopAfter = 0) {
  await send("Page.navigate", { url: `${BASE}?run&n=${nextId}#code=${b64(src)}` });
  const st = await waitStatus(stopAfter);
  const out = await text("out");
  report(name, out.includes(want) && st.startsWith(wantStatus), `out=${JSON.stringify(out)} status=${JSON.stringify(st)}`);
}

try {
  ws = new WebSocket(browserWs);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
  };
  const { result } = await send("Target.createTarget", { url: "about:blank" });
  sessionId = (await send("Target.attachToTarget", { targetId: result.targetId, flatten: true })).result.sessionId;
  await send("Page.enable");
  await send("Page.navigate", { url: BASE });
  await sleep(1500);
  report("the page is cross-origin isolated", (await evaluate("crossOriginIsolated")) === true, "no COOP/COEP");

  await runs("par, a loop and output",
    'par fn sq(x) = x * x\nt = sq(7)\nprint("hello", t.await)\nfor i in 1..=3\n    print(i)\n', "hello 49\n1\n2\n3\n", "done");
  await runs("a type error", 'x: int = "no"\n', "error", "exit code 1");
  await runs("a gpu fn runs through WebGPU",
    'gpu fn sq(xs: buffer[float])\n    i = gpu.index\n    xs[i] = xs[i] * xs[i]\nb = gpu.floats([1.0, 2.0, 3.0])\nsq(b)\nprint(b.list, gpu.device)\n',
    "[1.0, 4.0, 9.0] webgpu", "done");
  await runs("a panic", 'print(1)\npanic("boom")\n', "panic: boom", "exit code 1");
  await runs("an endless loop can be stopped", "while true\n    x = 1\n", "stopped", "stopped", 10);

  // input() reads the input box
  await send("Page.navigate", { url: `${BASE}?n=${nextId}#code=${b64('name = input()\nprint(f"hi {name}")\n')}` });
  await sleep(1500);
  await evaluate("document.getElementById('stdin').value = 'Ann'; document.getElementById('run').click()");
  await waitStatus();
  report("input() reads the input box", (await text("out")).includes("hi Ann"), await text("out"));

  const n = await evaluate("document.getElementById('examples').options.length");
  report(`the examples menu (${n - 1})`, n > 5, "too few examples");
  await evaluate("const s = document.getElementById('examples'); s.value = 'fizzbuzz'; s.onchange()");
  await evaluate("document.getElementById('run').click()");
  report("an example runs", (await waitStatus()).startsWith("done"), await text("out"));

  await send("Page.navigate", { url: `${BASE}?n=${nextId}#code=${b64("fn f( x )=x*2\nprint( f(2) )\n")}` });
  await sleep(1500);
  await evaluate("document.getElementById('fmt').click()");
  await waitStatus();
  const formatted = await evaluate("document.getElementById('code').value");
  report("format", formatted === "fn f(x) = x * 2\nprint(f(2))\n", JSON.stringify(formatted));

  // ixo build --target wasm: a page that runs a program (its runtime: this playground's files)
  const IXO = path.join(ROOT, "build", "release", process.platform === "win32" ? "ixo.exe" : "ixo");
  const built = path.join(DIST, "_built_check");
  if (fs.existsSync(IXO)) {
    const src = path.join(os.tmpdir(), `ixo-web-${process.pid}.ixo`);
    fs.writeFileSync(src, 'fn sq(x) = x * x\nprint("built", (1..=4).map(sq))\n');
    execSync(`"${IXO}" build "${src}" --target wasm --runtime "${DIST}" -o "${built}"`, { stdio: "ignore" });
    await send("Page.navigate", { url: `${BASE}_built_check/index.html` });
    let got = "";
    for (let i = 0; i < 60 && !got.includes("built"); i++) {
      await sleep(250);
      got = await text("out");
    }
    report("ixo build --target wasm runs in the browser", got.includes("built [1, 4, 9, 16]"), JSON.stringify(got));
    fs.rmSync(built, { recursive: true, force: true });
    fs.rmSync(src, { force: true });
  }

  // without the headers: the service worker adds them and the page reloads once
  await send("Page.navigate", { url: `${PLAIN}?run#code=${b64('print("isolated")' + "\n")}` });
  let iso = false;
  for (let i = 0; i < 50 && !iso; i++) {
    await sleep(200);
    iso = (await evaluate("crossOriginIsolated")) === true;
  }
  const st = iso ? await waitStatus() : "";
  report("on a host without the headers (service worker)",
    iso && st.startsWith("done") && (await text("out")).includes("isolated"), `isolated=${iso} status=${st}`);
} catch (e) {
  report("the browser session", false, String(e && e.stack || e));
} finally {
  chrome.kill();
  server.close();
  plainServer.close();
  try { fs.rmSync(profile, { recursive: true, force: true }); } catch (_) {}
}
console.log(failed ? `\n${failed} failed` : "\nall passed");
process.exit(failed ? 1 : 0);
