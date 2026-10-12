# cartwheel

A 2D and 3D game engine written in C. The same game source builds as a
native executable and as a [wasmcart](https://github.com/wasmcart/wasmcart)
cart (`.wasc`), a sandboxed WebAssembly game that runs in a browser, in Node,
in a native player and in RetroArch.

The stack:

- SDL2 for windows, input and audio (wasmcart-sdl2 inside carts).
- [three.c](https://github.com/monteslu/three.c), the three.js r186 API in C,
  for 3D rendering on GLES 3.0 / WebGL 2 and on WebGPU.
- [Box2D v3](https://github.com/erincatto/box2d) and
  [Box3D](https://github.com/erincatto/box3d) for physics.
- One job pool for physics, culling, animation, particles and asset decoding:
  pthreads natively, wasi-threads in a cart.
- stb_truetype and kb_text_shape for text, shaping and bidi.

Games are written in C against the headers in `include/cartwheel/`, which
double as the API documentation. Scenes, project settings, input maps and
themes are plain text files.

## Targets

| Target | How it is built | State |
|---|---|---|
| Linux x64 | `scripts/build.sh`, or the `linux-x64` export (tarball and AppImage, glibc 2.31 floor) | built and run |
| Linux arm64 | the `linux-arm64` export (cross-compiled) | builds; not run on arm64 hardware |
| Windows x64 | the `windows-x64` export, cross-compiled from Linux with llvm-mingw, GLES through ANGLE (D3D11) | builds; run under Wine, not yet on real Windows |
| Android arm64 | the `android-arm64` export (NDK, Gradle) | builds; run on the Android emulator |
| Web | the `web` export: a static site around the wasmcart web player | built and run in Chromium |
| wasmcart | `scripts/build.sh` (see below) | built and run on the wasmcart hosts listed below |

macOS and iOS are not supported yet: the exporter writes Xcode project kits
for them, but they have not been built. Exports are covered in
[docs/export.md](docs/export.md).

## Build

Requirements (Linux): cmake, ninja, a C17 compiler, SDL2, EGL and GLESv2
development packages, and Node.js (packing carts and the test hosts).

```sh
scripts/fetch-deps.sh    # pinned dependencies into deps/ (WASI_SDK=... reuses an existing wasi-sdk)
scripts/build.sh         # build/native, build/cart and, when its deps are present, build/cart-wgpu
```

`TARGETS="native"` (or `cart`, `cart-wgpu`) builds one tree, `BUILD_TYPE=Debug`
a debug build. Native samples land in `build/native/samples/<app>`.

## Building wasmcart carts

Every app (sample, test, the editor) is also a cart. `scripts/build.sh` builds two
kinds with wasi-sdk:

- **GL cart**, `build/cart/carts/<app>.wasc`: renders with GLES 3.0 / WebGL 2.
- **Dual GL + WebGPU cart**, `build/cart-wgpu/carts/<app>.wasc`: imports both
  APIs and uses WebGPU where the host offers it, GL otherwise. Built when
  `deps/wasmcart-wgpu` and `deps/emdawnwebgpu` are present (`fetch-deps.sh`
  fetches both). The dual build needs the native tree first (it packs assets
  with `build/native/tools/cwpack`). An app marked `GL_ONLY` has no dual cart.

```sh
TARGETS="native cart cart-wgpu" scripts/build.sh
npx wasmcart build/cart/carts/showcase.wasc
npx wasmcart build/cart-wgpu/carts/showcase.wasc
```

Hosts that run them:

- [wasmcart](https://github.com/wasmcart/wasmcart): the web player and the
  Node player (`npx wasmcart <cart>.wasc`).
- [wasmcart-native](https://github.com/wasmcart/wasmcart-native): a native
  player (`wasmcart-run`).
- [wasmcart-libretro](https://github.com/wasmcart/wasmcart-libretro): a
  libretro core for RetroArch. It runs a cart's GL path.

## Running and testing

```sh
node tools/run-cart.mjs --host native|chromium|wcnative|romdev --app hello --frames 120 \
     [--input script.json] [--shot out.png] [--state out.json]
node tests/gate.mjs [name]   # run an app on every host and compare frames and named state
```

A running app built with `.debug = true` answers requests over a text debug
channel (`tree`, `get`, `set`, `call`, `step`, `screenshot` and more), natively
and in a cart. The gates use it to check that every host agrees. See
[docs/guides/testing.md](docs/guides/testing.md) and
[docs/guides/debug-channel.md](docs/guides/debug-channel.md).

## Repository layout

| Path | Contents |
|---|---|
| `include/cartwheel/` | the public API, one header per module |
| `src/` | the engine |
| `samples/` | small apps, one feature each |
| `editor/` | the editor (work on it is paused; its API is unstable) |
| `tools/` | cart runner, project builder, exporters, asset packer (`cwpack`), hot reload (`cwhot`), lightmap baker (`cwbake`), doc generators |
| `tests/` | unit suites and the cross-host gates (`tests/gates.json`) |
| `docs/` | guides and tutorials; `node tools/gen-docs.mjs` builds them into a static site |
| `scripts/`, `patches/` | dependency pins (`scripts/deps.lock`), fetch and build scripts, patches applied to dependencies |

Start with [docs/guides/getting-started.md](docs/guides/getting-started.md)
and [docs/guides/overview.md](docs/guides/overview.md).

Third-party fonts and models used by the samples are listed with their
licenses in `CREDITS.txt` and `*-OFL.txt` files beside them.
