// Threads in the browser need SharedArrayBuffer, which needs the page to be
// "cross-origin isolated" (COOP/COEP headers). Hosts like GitHub Pages can't set
// headers, so this service worker adds them; the page reloads once to use it.
// On a server that sends the headers itself, it does nothing.
if (typeof window === "undefined") {
  self.addEventListener("install", () => self.skipWaiting());
  self.addEventListener("activate", (e) => e.waitUntil(self.clients.claim()));
  self.addEventListener("fetch", (e) => {
    const req = e.request;
    if (req.cache === "only-if-cached" && req.mode !== "same-origin") return;
    e.respondWith(
      fetch(req).then((res) => {
        if (res.status === 0) return res;  // opaque: can't be changed
        const headers = new Headers(res.headers);
        headers.set("Cross-Origin-Embedder-Policy", "require-corp");
        headers.set("Cross-Origin-Opener-Policy", "same-origin");
        headers.set("Cross-Origin-Resource-Policy", "cross-origin");
        return new Response(res.body, { status: res.status, statusText: res.statusText, headers });
      })
    );
  });
} else if (!window.crossOriginIsolated && window.isSecureContext && "serviceWorker" in navigator) {
  let tried = false;
  try {
    tried = sessionStorage.getItem("ixo-coi-reload") === "1";
  } catch (_) {}
  navigator.serviceWorker.register(document.currentScript.src).then((reg) => {
    const reload = () => {
      if (tried) return;  // one reload only, whatever happens
      try {
        sessionStorage.setItem("ixo-coi-reload", "1");
      } catch (_) {}
      location.reload();
    };
    if (reg.active && !navigator.serviceWorker.controller) reload();
    else navigator.serviceWorker.addEventListener("controllerchange", reload);
  }, (err) => console.warn("service worker:", err));
} else if (window.crossOriginIsolated) {
  try {
    sessionStorage.removeItem("ixo-coi-reload");
  } catch (_) {}
}
