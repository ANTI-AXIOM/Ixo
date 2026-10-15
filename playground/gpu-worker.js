// Runs gpu fn kernels through WebGPU for the playground. Ixo's thread posts a job
// ({mem, wgsl, desc, flag}) and waits on `flag` in its shared memory; this worker
// makes the WebGPU calls (they are asynchronous), copies the buffers back into
// Ixo's memory, and wakes it: 1 done, 2 no WebGPU (Ixo then uses its CPU fallback).
//
// desc: [buffers, (address, bytes) per buffer, uniform address, uniform bytes,
//        groups x, groups y, status address]
let devicePromise = null;
const pipelines = new Map();

async function getDevice() {
  if (!self.navigator || !navigator.gpu) return null;
  const adapter = await navigator.gpu.requestAdapter();
  return adapter ? await adapter.requestDevice() : null;
}

onmessage = async (e) => {
  const { mem, wgsl, desc, flag } = e.data;
  const done = (code) => {
    const f = new Int32Array(mem, flag, 1);
    Atomics.store(f, 0, code);
    Atomics.notify(f, 0);
  };
  try {
    devicePromise ||= getDevice();
    const dev = await devicePromise;
    if (!dev) return done(2);
    let pipe = pipelines.get(wgsl);
    if (!pipe) {
      const module = dev.createShaderModule({ code: wgsl });
      pipe = await dev.createComputePipelineAsync({ layout: "auto", compute: { module, entryPoint: "main" } });
      pipelines.set(wgsl, pipe);
    }
    const S = GPUBufferUsage;
    let at = 0;
    const nb = desc[at++];
    const bufs = [];
    const entries = [];
    for (let i = 0; i < nb; i++) {
      const ptr = desc[at++], bytes = desc[at++];
      const size = Math.max(4, (bytes + 3) & ~3);
      const b = dev.createBuffer({ size, usage: S.STORAGE | S.COPY_SRC | S.COPY_DST });
      const data = new Uint8Array(size);
      data.set(new Uint8Array(mem, ptr, bytes));
      dev.queue.writeBuffer(b, 0, data);
      bufs.push({ b, ptr, bytes, size });
      entries.push({ binding: i, resource: { buffer: b } });
    }
    const status = dev.createBuffer({ size: 16, usage: S.STORAGE | S.COPY_SRC | S.COPY_DST });
    dev.queue.writeBuffer(status, 0, new Uint8Array(16));
    entries.push({ binding: nb, resource: { buffer: status } });
    const uptr = desc[at++], ubytes = desc[at++];
    const usize = Math.max(16, (ubytes + 15) & ~15);
    const ucopy = new Uint8Array(usize);
    ucopy.set(new Uint8Array(mem, uptr, ubytes));
    const uni = dev.createBuffer({ size: usize, usage: S.UNIFORM | S.COPY_DST });
    dev.queue.writeBuffer(uni, 0, ucopy);
    entries.push({ binding: nb + 1, resource: { buffer: uni } });
    const gx = desc[at++], gy = desc[at++], statusPtr = desc[at++];
    const group = dev.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries });
    const enc = dev.createCommandEncoder();
    const pass = enc.beginComputePass();
    pass.setPipeline(pipe);
    pass.setBindGroup(0, group);
    pass.dispatchWorkgroups(gx, gy);
    pass.end();
    const reads = bufs.map((x) => {
      const r = dev.createBuffer({ size: x.size, usage: S.MAP_READ | S.COPY_DST });
      enc.copyBufferToBuffer(x.b, 0, r, 0, x.size);
      return r;
    });
    const sread = dev.createBuffer({ size: 16, usage: S.MAP_READ | S.COPY_DST });
    enc.copyBufferToBuffer(status, 0, sread, 0, 16);
    dev.queue.submit([enc.finish()]);
    await Promise.all([...reads.map((r) => r.mapAsync(GPUMapMode.READ)), sread.mapAsync(GPUMapMode.READ)]);
    bufs.forEach((x, i) => {
      new Uint8Array(mem, x.ptr, x.bytes).set(new Uint8Array(reads[i].getMappedRange(), 0, x.bytes));
      reads[i].unmap();
      reads[i].destroy();
      x.b.destroy();
    });
    new Uint8Array(mem, statusPtr, 12).set(new Uint8Array(sread.getMappedRange(), 0, 12));
    sread.unmap();
    sread.destroy();
    status.destroy();
    uni.destroy();
    done(1);
  } catch (err) {
    console.error("ixo gpu worker:", err);
    done(2);
  }
};
