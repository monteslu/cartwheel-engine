# cartwheel

[![CI](https://github.com/monteslu/cartwheel-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/monteslu/cartwheel-engine/actions/workflows/ci.yml)

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

## Example games

Two small games live here: `games/platformer3d`, a 3D platformer built
from scene files, and `games/action2d`, a top-down 2D action game. The full
games are in
[cartwheel-examples](https://github.com/monteslu/cartwheel-examples):
BikeMesa (downhill bikes), BomberDude (an arena bomber), derby (a demolition
derby), pugball (soccer with jet-pack pugs) and spout (a spider climbing a
rain spout). They build against a pinned version of this engine and have
their own gates.

cartwheel is MIT licensed (see [LICENSE](LICENSE)). Dependencies keep their
own licenses; fonts and models used by the samples and games are listed with
their licenses next to them (`CREDITS`, `CREDITS.txt`, `*-OFL.txt`).

## Targets

| Target | How it is built | State |
|---|---|---|
| Linux x64 | `scripts/build.sh`, or the `linux-x64` export (tarball and AppImage) | built and run |
| Linux arm64 | the `linux-arm64` export, natively on an arm64 machine or cross-compiled | built and run (CI, arm64 runner) |
| Windows x64 | the `windows-x64` export, cross-compiled from Linux with llvm-mingw | built; run in CI on a Windows runner |
| macOS arm64 | the `macos` export, on a Mac with Xcode's clang | built; run in CI on a macOS runner; signed ad hoc, not notarized |
| Android arm64 | the `android-arm64` export (NDK, Gradle) | built; run on the Android emulator |
| Web | the `web` export: a static site around the wasmcart web player | built and run in Chromium |
| wasmcart | `scripts/build.sh` (see below) | built and run on the wasmcart hosts listed below |

Native builds render with WebGPU through Dawn
([native-dawn](https://github.com/monteslu/native-dawn): Vulkan on Linux and
Android, Direct3D 12 on Windows, Metal on macOS) and fall back to OpenGL ES
3.0 when WebGPU does not start: the system's GLES on Linux and Android, ANGLE
on Windows (Direct3D 11) and macOS (Metal), from
[native-gles](https://github.com/monteslu/native-gles)' releases. An exported
game picks WebGPU itself; a development build renders with GL unless
`CW_GPU=webgpu`. iOS is an Xcode project kit, not built yet. Exports are
covered in [docs/export.md](docs/export.md).

Each release also carries prebuilt SDKs, one per target: a game builds
against the SDK and compiles only its own code
([docs/sdk.md](docs/sdk.md)). [docs/shipping.md](docs/shipping.md) goes
from a game repository to published builds for every platform, and
[cartwheel-starter](https://github.com/monteslu/cartwheel-starter) is a
template repository with all of it set up.

## Repository layout

| Path | Contents |
|---|---|
| `include/cartwheel/*.h` | the public API, one header per module; the header comments are the reference |
| `src/<module>/` | the engine; `src/platform/` is the only code that touches SDL or wasmcart |
| `samples/<name>/` | small apps, one feature each; each one has gates |
| `games/<name>/` | two small games, platformer3d and action2d, each with its own gates (larger games are in [cartwheel-examples](https://github.com/monteslu/cartwheel-examples)) |
| `editor/` | the editor, itself a cartwheel app (work on it is paused; its API is unstable) |
| `editor/templates/` | project skeletons: empty 2D and 3D projects and two small platformers |
| `tests/` | `gates.json` (every gate), `gate.mjs` (the gate runner), `unit/` (unit suites), scripted gates (`*.mjs`), input scripts (`*.json`) |
| `tools/` | `run-cart.mjs` (run any app on any host), `cwproject.mjs` (build a project), `cwexport.mjs` (exports), `cwpack` (asset packer), `cwhot` (C hot reload), `cwbake` (lightmaps), `programs.mjs` (three.c program tables), `gen-api.mjs` and `gen-docs.mjs` |
| `docs/` | guides, tutorials, `CONVENTIONS.md`, and the generated `api.json`, `nodes.json` and `editor_api.json` |
| `cmake/` | the cart toolchain, dependency setup, program tables, export toolchains |
| `scripts/` | `deps.lock` (every dependency, pinned), `fetch-deps.sh`, `build.sh` |
| `patches/<dep>/` | patches applied to a dependency after it is fetched |
| `deps/`, `build/`, `out/` | fetched dependencies, build trees and test output (all gitignored) |

## Build

Requirements (Linux): cmake, ninja, a C17 compiler, SDL2, EGL and GLESv2
development packages, Node.js, curl and unzip.

```sh
scripts/fetch-deps.sh    # every dependency in scripts/deps.lock, into deps/ (WASI_SDK=... reuses an existing wasi-sdk)
scripts/build.sh         # build/native, build/cart and build/cart-wgpu
```

`TARGETS="native"` (or `cart`, `cart-wgpu`) builds one tree and
`BUILD_TYPE=Debug` makes a debug build. The script runs cmake and ninja; to
build one target, run ninja yourself:

```sh
ninja -C build/native hello              # one native app: build/native/samples/hello
ninja -C build/cart hello                # one GL cart: build/cart/carts/hello.wasc
```

Native apps land in `build/native/samples/<app>`, `build/native/games/<app>`,
`build/native/tests/unit/<app>` and `build/native/editor/<app>`.

## Building wasmcart carts

Every app (sample, game, unit suite, the editor) is also a cart.
`scripts/build.sh` builds two kinds with wasi-sdk:

- **GL cart**, `build/cart/carts/<app>.wasc`: renders with GLES 3.0 / WebGL 2.
- **Dual GL + WebGPU cart**, `build/cart-wgpu/carts/<app>.wasc`: imports both
  APIs and uses WebGPU where the host offers it, GL otherwise. Built when
  `deps/wasmcart-wgpu` and `deps/emdawnwebgpu` are present (`fetch-deps.sh`
  fetches both). The dual build packs assets with the native tree's
  `build/native/tools/cwpack/cwpack`, so build native first. An app marked
  `GL_ONLY` has no dual cart.

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

Run any app on any host with the same frame count and input script:

```sh
node tools/run-cart.mjs --host native --app hello --frames 120 --shot out/hello.png --state out/hello.json
node tools/run-cart.mjs --host chromium --app hello --frames 120 --input tests/hello-input.json
```

Hosts: `native` (the native build), `chromium` (the cart in wasmcart's web
player in headless Chromium), `wcnative` (wasmcart-native's player, built
beside this checkout or named by `CW_WCNATIVE`), `romdev` (a
[romdev](https://github.com/monteslu/romdev) server on 127.0.0.1:7331).

A **gate** runs an app on every host and checks that each one matches the
native run: frames within a pixel bound, and named debug state exactly. Each
gate also runs a control that must fail. Gates are listed in
`tests/gates.json`; unit suites (`tests/unit/test_*.c`) are apps with gates
named `unit-*`.

```sh
node tests/gate.mjs unit-core unit-scene --hosts native   # some gates, native only
node tests/gate.mjs                                       # every gate on every host
node tests/gate.mjs --gpu webgpu                          # the WebGPU gates
node tests/all.mjs                                        # everything, including the scripted gates
```

Test runs are pinned to one GPU and run without access to the desktop. Before
the first run, name the test GPU in `~/.config/cartwheel/chromium-gpu` (its
PCI address, from `lspci -D`) and a word of its name in
`~/.config/cartwheel/wgpu-adapter`; the sandbox needs bubblewrap (`bwrap`).
Without GPU hardware, `CW_TEST_GPU=software node tests/ci.mjs` runs what CI
runs, on Mesa's software renderers.
[docs/guides/testing.md](docs/guides/testing.md) has the details.

## Docs and API reference

- `include/cartwheel/*.h`: the API reference. Every public function says what
  it does, what it costs and who owns what.
- `docs/api.json`, `docs/nodes.json`, `docs/editor_api.json`: the headers and
  node types as data (`node tools/gen-api.mjs` regenerates them;
  `node tools/gen-api.mjs --check` fails on an undocumented function).
- `docs/guides/` and `docs/tutorials/`: start with
  [overview](docs/guides/overview.md) and
  [getting started](docs/guides/getting-started.md).
  `node tools/gen-docs.mjs` builds them into a static site in `build/docs`.
- [docs/CONVENTIONS.md](docs/CONVENTIONS.md): the rules for changing the engine.

## Code conventions

The full list is in [docs/CONVENTIONS.md](docs/CONVENTIONS.md). The short version:

- C17. `cw_` prefix for functions and types, `CW_` for macros and enum values.
- Every public function is documented in its header; nobody should need the
  `.c` file to use an API.
- Memory through `cw_alloc` / `cw_free` with a tag; per-frame scratch with
  `cw_frame_alloc`; objects a user holds are pool handles, not pointers.
- Simulation code is deterministic on every host: randomness from `cw_rng`,
  time from the fixed `dt`, trig and exp from `cartwheel/math.h` (never libm),
  reductions in index order. `-ffp-contract=off` everywhere; no fast-math.
- Parallel work goes through the job pool (`cw_parallel_for`); never spawn
  threads.
- State a test checks is published by name (`cw_debug_f32("player.x", &x)`)
  with `.debug = true` in the app description.
- A feature is done when it runs natively and as a cart and a gate proves
  both agree.

## Adding a sample or a game

A sample:

1. Write `samples/<name>/main.c`: `cw_main` returns a `cw_app_desc` with the
   app's callbacks (or `cw_scene_app` for a scene tree). Put its files in
   `samples/<name>/assets/`.
2. Add `cw_add_app(<name> SOURCES <name>/main.c ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/<name>/assets)`
   to `samples/CMakeLists.txt`. `GL_ONLY` keeps an app that calls GL itself
   out of the dual cart build.
3. Add a gate to `tests/gates.json`: `name`, `app`, `frames`, optionally an
   `input` script, the named `fields` that must agree, and a control.
4. If it draws 3D materials, run `node tools/programs.mjs <name> --gates` to
   add the three.c programs it needs to the samples' table
   (`samples/programs/cw_samples.txt`), and commit the generated files.
5. `node tests/gate.mjs <gate>` must pass on every host.

A game is the same under `games/<name>/`, with `cw_add_app` in
`games/CMakeLists.txt`, usually a packed asset tree (`PACK
<name>/assets/roots.txt`), its own program table (`programs/<name>.txt`) and
a `<name>_auto` build that plays its gate's input script itself for hosts
that take no input. Generated art, audio and levels come from scripts in the
game's `tools/`, committed next to their output.

A game outside this repository can be built the way
[cartwheel-examples](https://github.com/monteslu/cartwheel-examples) builds
its games: the engine pinned in its `scripts/deps.lock`, its own
`CMakeLists.txt` of `cw_add_app` calls configured as the engine's project
(`-DCW_PROJECT_DIR`), and its gates run by this repository's
`tests/gate.mjs` with `CW_PROJECT_ROOT` set to it
([docs/guides/testing.md](docs/guides/testing.md)). Or start from a template:

```sh
cp -r editor/templates/empty_3d ~/games/mygame
node tools/cwproject.mjs build ~/games/mygame all
```

See [docs/guides/templates.md](docs/guides/templates.md).
