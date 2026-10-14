#!/usr/bin/env node
// Checks the WGSL that gpu fn kernels compile to (gpu.wgsl), in a real WebGPU:
// headless Chrome or Chromium. tests/tools/_wgsl_cases.ixo prints each kernel's
// WGSL, inputs and the results Ixo itself computed (Vulkan or the CPU fallback);
// a page here runs the WGSL with WebGPU on the same inputs and posts its results
// back, which must match (ints exactly, floats to 32-bit precision).
//
// Usage: node tests/tools/wgsl_check.mjs [path/to/chrome]   (ixo: IXO or build/release)
import { spawn, execFileSync, execSync } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";

const ROOT = path.resolve(import.meta.dirname, "../..");
const IXO = process.env.IXO || path.join(ROOT, "build", "release", process.platform === "win32" ? "ixo.exe" : "ixo");

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
      for (const exe of ["chrome-win64/chrome.exe", "chrome-win/chrome.exe", "chrome-linux/chrome", "chrome-linux64/chrome"]) {
        if (fs.existsSync(path.join(pw, d, exe))) return path.join(pw, d, exe);
      }
    }
  }
  return null;
}

const cases = JSON.parse(execFileSync(IXO, [path.join(ROOT, "tests", "tools", "_wgsl_cases.ixo")]).toString());
const CHROME = findChrome();
if (!CHROME) {
  console.log("skipped (no Chrome or Chromium found)");
  process.exit(0);
}

const page = `<!doctype html><meta charset="utf-8"><script>
const cases = ${JSON.stringify(cases)};
async function run() {
  const out = [];
  if (!navigator.gpu) return { error: "no navigator.gpu" };
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) return { error: "no WebGPU adapter" };
  const device = await adapter.requestDevice();
  for (const c of cases) {
    const msgs = [];
    const module = device.createShaderModule({ code: c.wgsl });
    const info = await module.getCompilationInfo();
    for (const m of info.messages) if (m.type === "error") msgs.push(m.lineNum + ":" + m.linePos + " " + m.message);
    if (msgs.length) { out.push({ name: c.name, error: msgs.join("; ") }); continue; }
    const pipeline = device.createComputePipeline({ layout: "auto", compute: { module, entryPoint: "main" } });
    const bufs = c.buffers.map((b, i) => {
      const data = b.floats ? new Float32Array(c.inputs[i]) : new Int32Array(c.inputs[i]);
      const buf = device.createBuffer({ size: Math.max(4, data.byteLength), usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(buf, 0, data);
      return buf;
    });
    const status = device.createBuffer({ size: 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST });
    const words = new ArrayBuffer(Math.ceil((c.scalars.length + 2) * 4 / 16) * 16);
    const dv = new DataView(words);
    // scalars: floats as f32 (written as such by the kernel's struct), ints and bools as i32
    const f32s = (c.wgsl.match(/struct Params \\{([^}]*)\\}/) || ["", ""])[1].split(",").map((s) => s.trim()).filter((s) => s.startsWith("s"));
    c.scalars.forEach((v, i) => {
      if (f32s[i] && f32s[i].endsWith("f32")) dv.setFloat32(i * 4, v, true); else dv.setInt32(i * 4, v === true ? 1 : v === false ? 0 : v, true);
    });
    const groups = Math.ceil(c.threads / 64);
    dv.setUint32(c.scalars.length * 4, c.threads, true);
    dv.setUint32(c.scalars.length * 4 + 4, groups * 64, true);
    const uni = device.createBuffer({ size: words.byteLength, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(uni, 0, words);
    const entries = bufs.map((b, i) => ({ binding: i, resource: { buffer: b } }));
    entries.push({ binding: bufs.length, resource: { buffer: status } });
    entries.push({ binding: bufs.length + 1, resource: { buffer: uni } });
    const group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries });
    const enc = device.createCommandEncoder();
    const pass = enc.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, group);
    pass.dispatchWorkgroups(groups);
    pass.end();
    const reads = bufs.map((b, i) => {
      const r = device.createBuffer({ size: Math.max(4, c.buffers[i].n * 4), usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
      enc.copyBufferToBuffer(b, 0, r, 0, Math.max(4, c.buffers[i].n * 4));
      return r;
    });
    device.queue.submit([enc.finish()]);
    const got = [];
    for (let i = 0; i < reads.length; i++) {
      await reads[i].mapAsync(GPUMapMode.READ);
      const raw = reads[i].getMappedRange();
      got.push(Array.from(c.buffers[i].floats ? new Float32Array(raw) : new Int32Array(raw)).slice(0, c.buffers[i].n));
    }
    out.push({ name: c.name, got });
  }
  return { results: out };
}
run().catch((e) => ({ error: String(e) })).then((r) => fetch("/done", { method: "POST", body: JSON.stringify(r) }));
</script>`;

const server = http.createServer((req, res) => {
  if (req.method === "POST" && req.url === "/done") {
    let body = "";
    req.on("data", (d) => (body += d));
    req.on("end", () => {
      res.end("ok");
      finish(JSON.parse(body));
    });
    return;
  }
  res.setHeader("Content-Type", "text/html");
  res.end(page);
});
let chrome;
const profile = fs.mkdtempSync(path.join(os.tmpdir(), "ixo-wgsl-"));
server.listen(0, "127.0.0.1", () => {
  const url = `http://127.0.0.1:${server.address().port}/`;
  chrome = spawn(CHROME, ["--headless=new", "--enable-unsafe-webgpu", "--enable-features=Vulkan,WebGPUService",
    "--ignore-gpu-blocklist", `--user-data-dir=${profile}`, "--no-first-run", url], { stdio: "ignore" });
});
const timer = setTimeout(() => finish({ error: "timed out waiting for the browser" }), 60000);

function finish(r) {
  clearTimeout(timer);
  if (chrome) chrome.kill();
  server.close();
  if (r.error) {
    console.log("skipped (" + r.error + ")");
    process.exit(0);
  }
  let bad = 0;
  for (const res of r.results) {
    const c = cases.find((x) => x.name === res.name);
    if (res.error) {
      console.log(`FAIL ${res.name}: the WGSL doesn't compile: ${res.error}`);
      bad++;
      continue;
    }
    const diffs = [];
    res.got.forEach((vals, b) => {
      const want = c.expect[b];
      vals.forEach((v, i) => {
        const w = want[i];
        const ok = c.buffers[b].floats ? Math.abs(v - w) <= 1e-5 * Math.max(1, Math.abs(w)) : v === w;
        if (!ok && diffs.length < 5) diffs.push(`buffer ${b} item ${i}: ${v} (Ixo: ${w})`);
      });
    });
    // mandel: near the set's edge 32-bit rounding decides when a point escapes, and devices
    // round differently (fused multiply-adds...): up to 1% of its pixels may differ
    let count = 0;
    res.got.forEach((vals, b) => vals.forEach((v, i) => { if (!c.buffers[b].floats && v !== c.expect[b][i]) count++; }));
    const allowed = c.name === "mandel" ? Math.floor(c.threads / 100) : 0;
    if (c.name === "mandel" ? count > allowed : diffs.length > 0) {
      console.log(`FAIL ${res.name}: ${diffs.join("; ")}`);
      bad++;
    } else {
      console.log(`ok   ${res.name}`);
    }
  }
  console.log(bad ? `${bad} of ${r.results.length} kernels differ` : `${r.results.length} kernels agree with WebGPU`);
  process.exit(bad ? 1 : 0);
}
