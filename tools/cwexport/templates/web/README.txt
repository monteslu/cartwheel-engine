@TITLE@ @VERSION@ for the web (WebGPU where the browser has it, else WebGL 2;
wasmcart web host).

Upload this whole folder to a web server. The game runs on several threads,
which browsers allow only on a cross-origin isolated page: EVERY file must be
served with these headers:

  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp

Netlify, Cloudflare Pages: the _headers file here already does it.
nginx:   add_header Cross-Origin-Opener-Policy same-origin always;
         add_header Cross-Origin-Embedder-Policy require-corp always;
Apache:  Header always set Cross-Origin-Opener-Policy "same-origin"
         Header always set Cross-Origin-Embedder-Policy "require-corp"
Caddy:   header Cross-Origin-Opener-Policy same-origin
         header Cross-Origin-Embedder-Policy require-corp
Locally: node serve.mjs   (then open http://127.0.0.1:8080/)
GitHub Pages cannot set headers: it cannot host this game.

Without the headers the page explains what is missing instead of starting
(it checks window.crossOriginIsolated). Opening index.html from disk
(file://) never works for the same reason.

Files: index.html + player.js (the page), wasmcart/ (the cart host, MIT),
game.wasm (the game's code; serve it as application/wasm so the browser
compiles it while it downloads and keeps the compiled code) and game.wasc (the
rest of the game).
