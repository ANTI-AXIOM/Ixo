// Runs one Ixo program in a Web Worker (the page makes a new one per run, and
// Stop just terminates it). Messages in:  {cmd: "run" | "fmt", source, stdin}
// Messages out: {type: "out" | "err", text}, {type: "formatted", source},
//               {type: "exit", code, ms}, {type: "fatal", text}
importScripts("ixo.js");

let pending = { out: "", err: "" };
let timer = 0;
function flush() {
  timer = 0;
  for (const type of ["out", "err"]) {
    if (pending[type]) postMessage({ type, text: pending[type] });
    pending[type] = "";
  }
}
function emit(type, line) {
  pending[type] += line + "\n";
  if (pending.out.length + pending.err.length > 64 * 1024) flush();
  else if (!timer) timer = setTimeout(flush, 30);
}

// Ixo's threads (par workers) start from this same file, named "em-pthread":
// there ixo.js sets itself up and handles the messages.
if (self.name !== "em-pthread") onmessage = async (e) => {
  const { cmd, source, stdin } = e.data;
  if (!self.crossOriginIsolated) {
    postMessage({ type: "fatal", text: "This page needs cross-origin isolation (for threads); reload it once." });
    return;
  }
  const input = new TextEncoder().encode(stdin || "");
  let at = 0;
  const started = performance.now();
  let mod;
  let finished = false;
  const done = (code) => {
    if (finished) return;
    finished = true;
    flush();
    if (cmd === "fmt" && code === 0) {
      postMessage({ type: "formatted", source: mod.FS.readFile("/main.ixo", { encoding: "utf8" }) });
    }
    postMessage({ type: "exit", code, ms: Math.round(performance.now() - started) });
  };
  try {
    mod = await createIxo({
      print: (s) => emit("out", s),
      printErr: (s) => emit("err", s),
      preRun: [(m) => {
        m.FS.init(() => (at < input.length ? input[at++] : null), null, null);
        m.FS.writeFile("/main.ixo", source);
        m.FS.chdir("/");
      }],
      onExit: done,
      onAbort: (what) => {
        flush();
        postMessage({ type: "fatal", text: "ixo stopped: " + what });
      },
    });
    mod.callMain(cmd === "fmt" ? ["fmt", "/main.ixo"] : ["/main.ixo"]);
  } catch (err) {
    flush();
    // exit() unwinds with an ExitStatus; anything else is a real failure
    if (err && err.name === "ExitStatus") done(err.status);
    else postMessage({ type: "fatal", text: String(err && err.stack || err) });
  }
};
