# Overview

{{name}} is a general-purpose 2D and 3D game engine written in C. Every game is built once and runs natively and as a wasmcart cart, and a gate proves the two agree.

## The stack

| Layer | What it is |
|---|---|
| Platform | SDL2. Native builds link real SDL2; carts link wasmcart-sdl2, the same SDL2 API on the wasmcart ABI. Only `src/platform/` touches either. |
| 3D rendering | three.c: three.js r186 in C, drawing with r186's generated shader programs on GLES 3.0 (WebGL2 in a browser) and WebGPU, plus the engine's pass graph for shadows, sky, fog, particles, decals and post effects. |
| 2D rendering | the engine's own batched renderer (`cartwheel/render2d.h`) and 2D node types. |
| Physics | Box2D v3 and Box3D, wrapped as worlds, queries, movers and node types. |
| Threads | one job pool (`cartwheel/jobs.h`) shared by physics, culling, animation, particles and asset decoding. Natively pthreads, in a cart wasi-threads; the same code. |
| Text | stb_truetype and kb_text_shape for shaping, bidi and CJK. |

There is one control flow on every target: the engine owns the frame and calls the game back (`init`, `fixed_update`, `update`, `render`). A game never writes a main loop. See [Architecture](guide:architecture).

Rendering is GLES 3.0 / WebGL2 (ANGLE where a platform needs it) or WebGPU (Dawn natively; in a cart, the host's WebGPU). The GL path runs everywhere; WebGPU is used where it is available.

## Design

A game is C written directly against the public headers, checked by running it. That shapes the rest:

- **The C API is the authoring surface.** Every public function is documented in its header (what it does, what it costs, who owns what), and the headers are exported as data in `docs/api.json`. Node types and their properties are in `docs/nodes.json`.
- **A running game answers questions.** With `.debug = true`, an app exposes named state (`cw_debug_f32("player.x", &x)`) and a text request/reply channel: `tree`, `props`, `get`, `set`, `call`, `pause`, `step`, `screenshot` and more. The same channel works natively and in a cart. See [The debug channel](guide:debug-channel).
- **Everything is text.** Scenes (`.cwscene`), project settings (`project.cwproj`), input maps, themes and trigger rules are line-based text that diffs in git and can be written and reviewed without a GUI.
- **No scripting VM.** Gameplay is C plus data-driven [triggers](guide:triggers) ("when X then Y" rules stored as nodes). There is no Lua or GDScript layer for now.
- **Verification is built in.** Fixed-step simulation is deterministic across hosts, so the same input script gives the same named state on every host. See [Testing](guide:testing).

The editor is paused while the engine and tooling mature; scenes and projects are text files edited directly.

## Hosts

The same app runs on these hosts, and `tools/run-cart.mjs` drives any of them with the same frame count and input script:

| Host | What runs | Notes |
|---|---|---|
| `native` | the native build (`build/native`) | the reference for every comparison |
| `romdev` | the cart in the romdev MCP server's wasmcart host (127.0.0.1:7331) | deterministic stepping, debug state, the debug channel through `wasm({op:'command'})` |
| `chromium` | the cart in wasmcart's web host, in headless Chromium | cross-origin isolated (threads need `SharedArrayBuffer`) |
| `wcnative` | the cart in wasmcart-native's player | frames only: no input scripts and no debug state |
| `angle` | the native build on ANGLE | only when `CW_ANGLE_DIR` is set |

Shipping builds (Linux, Windows, Android, web, macOS and iOS project kits) come from the exporters; see [Exporting](guide:export).

## What is in the repository

| Path | Contents |
|---|---|
| `include/cartwheel/*.h` | the public API, one header per module; the comments are the documentation |
| `src/<module>/` | the implementation |
| `samples/<name>/` | small apps, one feature each; every one is a gate |
| `games/` | the larger games that forced each phase (a 3D platformer, a 2D action game) |
| `editor/templates/` | project skeletons to copy (see [Templates](guide:templates)) |
| `tests/` | `gates.json`, the gate runner, unit suites (`tests/unit/`) and scripted gates |
| `tools/` | `run-cart.mjs`, `cwproject.mjs` (build a project), `cwexport.mjs`, `cwpack` (assets), `cwhot` (C hot reload), `cwbake` (lightmaps), `gen-api.mjs`, `gen-docs.mjs` |
| `docs/` | these pages, `api.json`, `nodes.json` |
| `deps/`, `patches/` | pinned dependencies (fetched by `scripts/fetch-deps.sh`) and the local patches applied to them |

Every public header, with its one-line summary:

@headers

## Where to go next

- [Getting started](guide:getting-started): build, run a sample on every host, start a project.
- [Your first 2D game](tutorial:first-2d) and [your first 3D game](tutorial:first-3d): a small game from an empty template.
- [Multiplayer with rollback](tutorial:multiplayer-rollback) and [Localizing a game](tutorial:localize).
- [The debug channel](guide:debug-channel): inspect and drive a running game.
- [Coming from Godot](guide:porting-from-godot): the concept map.
- [Working on {{name}}](guide:conventions): the rules for changing the engine itself.
