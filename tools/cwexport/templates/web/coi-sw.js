// cartwheel web export: cross-origin isolation for static hosts that cannot
// send headers (GitHub Pages and the like). Shipped only when the export
// preset sets coi_shim = true. The page registers this service worker, which
// adds the two isolation headers (and a same-origin resource policy) to every
// same-origin response, then reloads once so the page is served through it.
// A server that sends the headers itself never needs it.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (new URL(req.url).origin !== self.location.origin) return; // other origins: untouched
  if (req.cache === 'only-if-cached' && req.mode !== 'same-origin') return;
  e.respondWith((async () => {
    const res = await fetch(req);
    if (res.status === 0 || res.type === 'opaque') return res;
    const headers = new Headers(res.headers); // Content-Length stays: the loading bar keeps its percentage
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(res.body, { status: res.status, statusText: res.statusText, headers });
  })());
});
