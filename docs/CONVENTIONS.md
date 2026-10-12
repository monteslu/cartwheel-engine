# Working on cartwheel

Read this before changing anything. It is short on purpose.

## The one rule

**Every feature runs natively and as a cart, and the gate proves both agree.**
A feature is not done until it has a sample or unit suite in
`tests/gates.json` that passes on every host:

```sh
scripts/build.sh                 # native + cart
node tests/gate.mjs <name>       # the gates you touched, then all of them
```

Hosts: `native` (the native build), `romdev` (the romdev MCP server's
wasmcart host on 127.0.0.1:7331), `chromium` (wasmcart's web host in
headless Chromium), `wcnative` (wasmcart-native's player; input-free gates
only) and `angle` (native build on ANGLE, when `CW_ANGLE_DIR` is set).

A gate compares frames (share of pixels differing by more than 16 levels)
and named debug state (exact) against the native run, and runs a CONTROL
that must fail (one frame early, other input, or an injected failure). A
gate whose control passes is a gate that cannot see anything.

## Finding your way (what an agent should read first)

- `docs/api.json`: every public function, type and macro with its comment
  (generated from the headers by `node tools/gen-api.mjs`).
- `docs/nodes.json`: every node type with its properties (type, default,
  range, enum names, doc) and signals.
- A running app with `.debug = true` answers questions itself over the debug
  channel (romdev: `wasm({op:'command', text})`; natively `CW_DEBUG_CMDS`):
  `help`, `types`, `tree`, `props <node>`, `get` / `set <node> <prop> <value>`,
  `call <node> <method>`, `spawn`, `save`, `pause` / `step <ticks>` / `resume`
  (freeze the game, run exact ticks), `screenshot <world|ui|debug>`, `changes
  <revision>`, `stats`, `vfs`. A node is `#uid` or a path. Each request runs one
  frame. Natively, `cartwheel/remote.h` launches a game as a child process and
  speaks the same requests over a socket (play-in-editor).
- Enums can be set by name: `cw_node_set(n, "primitive", cw_val_enum("SPHERE"))`.
- UI: controls are node types (`cartwheel/ui.h`, listed in `docs/nodes.json`) and
  load from .cwscene files; themes are text files (`samples/ui_assets/themes/editor.theme`);
  input scripts may carry `"text"` (typed text) besides keys, mouse and touch.
- Coordinates: Y up, -Z forward; lights and cameras face their -Z.
- Overlay a HUD on a scene app: draw with render2d in `base.render` of
  `cw_scene_app` (it runs after the 3D draw and the post chain: it is the
  render pipeline's "app" overlay pass).
- Rendering is a per-tree pass graph (`cartwheel/pipeline.h`): a module adds
  shadows, sky, fog, particles, decals or post effects as passes with a phase
  and an order; the header says how. Every tree renders as three.js r186
  does (linear colours, physical light units, linear HDR, AgX); the debug
  command `pipeline` lists the passes, their GPU ms and any program states
  three.c lacks.
- Adding a material combination: three.c draws each material with the r186
  program generated for its state, from the app's program tables
  (`cmake/programs.cmake`; samples share `samples/programs/cw_samples.txt`, a
  game or project has `programs/<app>.txt`). A state no table has is built at
  run time in native dev builds (CW_THREE_BUILDER) and skipped in carts, and
  every gate fails on it, naming the state. Fix it: run the app (or its
  gates) through `node tools/programs.mjs <app> [--gates]`, which adds the
  states to the app's manifest and regenerates (`ninja <table>_programs` does
  one table from its manifest), then commit the manifest and the generated
  `<table>_gl.c` / `<table>_wgpu.c`. No GPU is needed to generate.
- Lighting: lights are three.c's (three.js r186's lights and shadow maps).
  Sky and fog are node types (`cartwheel/environment.h`), baked lighting is
  LightmapGI and the `cwbake` tool (`cartwheel/lightmap.h`). The debug
  command `environment` reports what it did this frame.

## Layout

```
include/cartwheel/*.h   public API, one header per module; comments ARE the docs
src/<module>/*.c        implementation; src/platform is the only SDL/wasmcart user
samples/<name>/         small focused apps (one feature each)
tests/unit/test_*.c     unit suites (apps: see tests/unit/test.h)
tests/gates.json        what the gate runs
tools/run-cart.mjs      run any app on any host: frames, input script, shot, state
editor/                 the editor (a cartwheel app): core/, modules/<name>/, include/cwed/editor.h
deps/                   pinned dependencies (scripts/fetch-deps.sh); patches/<dep>/
```

New `src/<module>/*.c` files are picked up by the glob in `CMakeLists.txt`
(re-run cmake). Samples and suites are added with `cw_add_app(name SOURCES ...
[ASSETS dir])` in `samples/CMakeLists.txt` or `tests/unit/CMakeLists.txt`.

## The editor

`editor/` is the cartwheel editor, built with the engine's own UI. Read
`editor/include/cwed/editor.h` (or `docs/editor_api.json`) first. A feature is
a module: a new directory `editor/modules/<name>/` whose .c files define
`CWED_MODULE(name)`; the build generates the registry, so no shared file
changes. Every editor action is a command (`ed help` over the debug channel
lists them; `ed <command line>` runs one in a running editor, native or
cart). Undoable commands need an `.example` line: `unit-editor` round-trips
every one of them automatically. Engine features the editor needs go in the
engine (public headers), never in a module.

## C rules

- C17, `cw_` prefix for functions and types, `CW_` for macros and enums.
- Headers say what a function does, what it costs, and who owns what. A
  reader (often an agent) should never need the .c file to use an API.
- Memory: `cw_alloc`/`cw_free` with a `cw_mem_tag`; per-frame scratch with
  `cw_frame_alloc`. User-held objects are `cw_pool` entries addressed by
  generational handles, never raw pointers in public APIs.
- Errors a user can cause are logged with the fix in the message. Programmer
  errors are `CW_ASSERT(cond, "what to do instead")`.
- Hot loops are threaded SIMD C: `cw_parallel_for` (cartwheel/jobs.h) over
  flat arrays; never spawn threads.

## Determinism rules (simulation code: anything that feeds game state)

- Randomness from `cw_rng` only, seeded explicitly.
- Time from the fixed-step `dt` only; never read a clock.
- Trig/exp/log/pow from `cartwheel/math.h` (`cw_sin`, `cw_atan2`, ...):
  libm differs between glibc, musl/wasi-libc, Apple and MSVC. Rendering may
  use libm.
- Iterate in insertion order (`cw_map` and `cw_pool` already do).
- Parallel reductions write per-item or per-group results and reduce in index
  order; never accumulate in completion order.
- `-ffp-contract=off` is global; never add `-ffast-math`.
- Carts build with `-fno-vectorize`: wasi-sdk's clang 22 loop vectorizer
  miscompiles under `-msimd128` at `-O3` (tests/repro/wasm-loop-vectorize.c),
  which made a cart compute different state from native. Don't re-enable it
  without re-running that repro on the new toolchain.

## Generator scripts (JS: audio, textures, levels)

- Seeded randomness from `tools/lib/rng.mjs` (`rng(seed)`: `r()`, `r.noise()`,
  `r.u32()`). JS numbers are doubles: `(seed * 1103515245 + 12345) >>> 0`
  overflows 2^53, drops low bits and collapses into a short cycle. One copy
  of that line looped every 419 samples, so every "noise" sound in four
  games was a 114.6 Hz buzz. 32-bit integer maths uses `Math.imul`.
- `tests/js-int-math.mjs` (in `tests/all.mjs`) fails on a large-constant
  multiplication truncated to 32 bits without `Math.imul`; a line that means
  it carries `int-math-ok: <reason>` on it or just above.

## Debug state

Expose what a test or an agent needs BY NAME: `cw_debug_f32("player.x", &x)`
(cartwheel/debug.h), and set `.debug = true` in the app desc. romdev reads it
with `wasm({op:'debugState'})`; the gate compares it across hosts. Prefer a
named value over a screenshot for any assertion about game state.

## Commits and outside actions

- Commit locally, small and often, messages that say why.
- No AI attribution lines in commits.
- Never push, publish, open a PR or file an issue on any repo.
- Never put work in /tmp; scratch lives under out/ (gitignored).
- No hardcoded /home/<user> paths in anything committed.
