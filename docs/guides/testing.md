# Testing

Every feature runs natively and as a cart, and a gate proves both agree. This page covers the gate file, the hosts, controls, unit suites, scripted gates, input scripts and how to name debug state so a gate can read it.

## Before the first run

Test runs are pinned to one GPU and cannot reach the desktop: every gate runs inside a bubblewrap (`bwrap`) mount namespace that shows only the test GPU, with SDL's offscreen driver and no display variables. Name the GPU once:

```sh
mkdir -p ~/.config/cartwheel
echo 0000:03:00.0 > ~/.config/cartwheel/chromium-gpu   # the test GPU's PCI address (lspci -D)
echo 7600 > ~/.config/cartwheel/wgpu-adapter           # a word of its device name, as WebGPU reports it
```

`CW_CHROMIUM_GPU` and `CW_WGPU_ADAPTER` override the files. A run without them refuses to start and says so.

## Continuous integration

CI (`.github/workflows/ci.yml`) runs on every push and pull request to main, on a GitHub runner with no GPU. It fetches the pinned dependencies, builds the native tree, the GL carts and the dual GL + WebGPU carts, runs `node tools/gen-api.mjs --check` and `node tools/gen-docs.mjs --check`, and runs `tests/ci.mjs` with `CW_TEST_GPU=software`: every app gate on the native host through Mesa's llvmpipe (GL), every gate with a `webgpu` entry through lavapipe (WebGPU), and the scripted gates that need no other host (the GL gates are split over three jobs, `node tests/ci.mjs gl-<k>of3`). In software mode nothing is pinned, sandboxed or guarded, and the renderer checks expect llvmpipe. `native-pads` plugs virtual controllers in through uinput (`tools/vpad.c`), so CI loads the `uinput` module and makes `/dev/uinput` and the input devices it creates usable by the runner's user; on a desktop the session already grants both.

Local only (they need GPU hardware or a host CI does not have; `LOCAL_ONLY` in `tests/ci.mjs` gives the reason for each):

- the romdev, Chromium, wasmcart-native (`wcnative`) and wasmcart-libretro legs of every gate;
- `gpu-guard`, `fx-gpu-report`, `pipeline-perf`, `fx-perf-report`, `vis-perf-report`, `showcase-perf`: GPU guards, timings and frame-time budgets;
- `gpu-no-display`, `gpu-cleanup`, `wcnative-pads`, `editor-channel`, `editor-play`, `editor-import`, `a11y-channel`, `net`, `plugins`, `project-templates`, `tutorials`, `docs`: they drive romdev, Chromium or wasmcart-native;
- `export`, `project-export`: the export toolchains, Wine and an Android emulator.

Run `node tests/all.mjs` on a machine with a GPU and those hosts for the full set.

## The gate

```sh
node tests/gate.mjs                                  # every gate in tests/gates.json, on every host it can run on
node tests/gate.mjs hello-input unit-scene           # some gates
node tests/gate.mjs unit-scene --hosts native,romdev # narrow the hosts (never adds one a gate cannot run on)
```

```text
ok   hello-input (native, romdev)
ok   unit-scene (native, romdev)
```

A failing gate prints `FAIL <name>` and one indented line per problem (a host whose run failed, a frame difference over the bound, the first differing state fields, or `CONTROL PASSED`). Gates run in file order.

For each gate the runner starts `tools/run-cart.mjs` on every eligible host in parallel with the same app, frame count and input script, then:

1. Checks the native frame is a real picture (at least `minColors` unique colours) and the native state is not empty.
2. Checks `expect` against the native state.
3. Compares every other host against native: the share of pixels differing by more than 16 levels must stay under `maxGt16`, and the named state must be equal (within `tol` for numbers) on every host that reports state.
4. Runs the **control** natively, which must differ from the reference in frame or state. A gate whose control passes cannot see anything, and fails.

Hosts by default: `native`, `romdev`, `chromium`, plus `wcnative` for gates without an input script (wasmcart-run takes no input and reports no state, so it is compared on frames only), plus `angle` when `CW_ANGLE_DIR` is set. Outputs land in `out/gate/<name>.<host>.png` and `.json`.

## `tests/gates.json`

An array of gate objects. Every key `tests/gate.mjs` reads:

| Key | Meaning |
|---|---|
| `name` | the gate's name (what you pass on the command line) |
| `app` | the app target (`build/native/{samples,tests/unit,games,editor}/<app>`, `build/cart/carts/<app>.wasc`) |
| `frames` | engine frames to run before capturing |
| `frameHz` | the virtual render rate (default 60); fixed ticks stay at the app's own rate |
| `input` | an input script in `tests/` |
| `assets` | a native asset root when it is not `samples/<app>/assets` |
| `hosts` | the hosts this gate may run on (replaces the default list) |
| `skip` | hosts to leave out (for example `romdev` for a keyboard script) |
| `minColors` | unique colours the native frame must have (default 2) |
| `maxGt16` | the share of pixels allowed to differ by more than 16 levels (default 0.001) |
| `fields` | the state fields to compare (default: every field except the `dbg.*` mailbox) |
| `tol` | relative tolerance for numeric fields (default 0: exact) |
| `expect` | `{field: value}` the native run must report exactly |
| `controlFrames` | frames for the control run (default: `frames - 1`, or `frames` with a control input) |
| `controlInput` | a different input script for the control |
| `startCmds` | debug-channel requests every host posts at load (`run-cart --start-cmds`), request i answered at the start of frame i + 2: how a test says what a real host would, e.g. `["touch_primary 1"]` for a phone |
| `controlStartCmds` | the control run's start requests (default: none, so a `startCmds` gate's control runs without them) |
| `controlEnv` | environment for the control run; a value `"repo:<path>"` names a repository file that is copied into `out/` first |
| `script` | instead of all the above: a node script that runs its own checks and controls (exit 0 = pass) |
| `note` | what the gate proves, for humans; not read by the runner |

A typical gate, from `tests/gates.json`:

```text
{"name":"action2d","app":"action2d","frames":3000,"input":"action2d-pad.json",
 "controlInput":"action2d-pad-control.json","maxGt16":0.003,
 "fields":["g.state","g.score","g.hp","g.room","g.enemies","g.kills",...,"tree.nodes"],
 "expect":{"g.state":6,"g.room":3,"g.boss_hp":0,"g.deaths":0,"g.doors":2,"g.kills":9}}
```

## Controls that must fail

A comparison of two broken runs looks like success, so every gate carries a control that must fail:

- **one frame early** (the default): the state and frame at N-1 must differ from N, which proves the gate sees motion;
- **other input** (`controlInput`): the run without the jump must not collect the coins;
- **an injected defect** (`controlEnv`): unit suites run the control with `CW_TEST_INJECT_FAIL=1`, which adds a failing check, so `expect: {"unit.failed": 0}` must break.

Scripted gates run their own controls and print `CONTROL PASSED` (a failure) when one does not fail.

## Unit suites

A unit suite is an app (`tests/unit/test_*.c`) that runs every test in `init` and publishes the counts as debug fields, so the gate compares them across hosts like any other state. `tests/unit/test.h` provides `CHECK(cond)`, `CHECK_EQ_INT(a, b)`, `FOLD(v)` (fold a result that must agree across hosts into `unit.checksum`) and `CW_TEST_SUITE("name", {"case", fn}, ...)`, which defines `cw_main`. Each suite publishes `unit.passed`, `unit.failed`, `unit.checksum` and `unit.done`; a failing check logs `file:line`.

Add a suite with `cw_add_app(unit_x SOURCES test_x.c)` in `tests/unit/CMakeLists.txt` and a gate:

```text
{"name":"unit-x","app":"unit_x","frames":2,"minColors":1,"fields":["unit.passed","unit.failed","unit.checksum","unit.done"],
 "controlFrames":2,"expect":{"unit.failed":0,"unit.done":1},"controlEnv":{"CW_TEST_INJECT_FAIL":"1"}}
```

## Scripted gates

Some checks need more than one run: several processes, a tool, a timing report. A gate with `script` runs a node script that prints `ok ...` / `FAIL ...` lines, runs its own must-fail control, and exits 0 only on success. Examples: `tests/cwpack.mjs` (asset packing), `tests/net.mjs` (cart-to-cart sessions), `tests/project-templates.mjs` ([Templates](guide:templates)), `tests/export.mjs`, the `*-perf.mjs` reports ([Profiling](guide:profiling)), and `tests/docs.mjs` / `tests/tutorials.mjs` for this site.

## Gates of another repository

A repository of games built with this engine (configured as its project with `-DCW_PROJECT_DIR`) keeps its own `tests/gates.json`, input scripts, `build/` and `out/`, laid out as here. `CW_PROJECT_ROOT=<that repository>` points `tests/gate.mjs` and `tools/run-cart.mjs` at them: gates, inputs and scripts come from its `tests/`, apps from its `build/` (a project app is found through `build/native/apps/<app>.json`), and output goes to its `out/`. [cartwheel-examples](https://github.com/monteslu/cartwheel-examples) runs its games' gates this way.

`tests/post-fusion.mjs` (post fusion draws what the separate passes draw, on GL and WebGPU) takes the apps to check as arguments, `app[:frame[:preset]]`, and reads `CW_PROJECT_ROOT` the same way.

## Everything at once

```sh
node tests/all.mjs      # gate.mjs, then channel.mjs, vfs.mjs, hot.mjs, remote.mjs
scripts/ci.sh           # build both trees, regenerate and check the API data, tests/all.mjs, sanitizers
QUICK=1 scripts/ci.sh   # the same without the sanitizer pass
```

| Script | What it proves |
|---|---|
| `tests/channel.mjs` | the same debug-channel requests give the same replies natively (`CW_DEBUG_CMDS`) and in a romdev cart; a request for a missing node must fail |
| `tests/vfs.mjs` | the project file layer round-trips |
| `tests/hot.mjs` | `cwhot` swaps code at an exact frame and the state carries on; without the swap the result differs |
| `tests/remote.mjs` | the remote game protocol on every transport |
| `scripts/sanitize.sh` | every unit suite and sample for a few frames under ASan + UBSan; `SAN=thread` for TSan on the job system |

`ROMDEV_PORT` picks another romdev server (default 7331).

## Input scripts

An input script is JSON: a list of entries, each setting the WHOLE input state from its frame on (frames are 1-based engine frames). From `tests/hello-input.json`:

```text
[{"frame":10,"buttons":["south"],"axes":{"lx":1}},{"frame":60,"buttons":[],"axes":{"lx":-1}},{"frame":90}]
```

| Key | Value |
|---|---|
| `buttons` | pad 0 buttons: `south east west north back guide start l3 r3 l r up down left right` |
| `axes` | `lx ly rx ry lt rt`, -1..1 (triggers 0..1) |
| `keys` | held keys: names (`space`, `return`, `escape`, `left`, `a`..`z`, `0`..`9`, `f1`..`f12`, `lshift`, ...) or SDL scancodes |
| `mouse` | `{"x":320,"y":180,"buttons":["left"]}` in integer cart pixels (pointer slot 0) |
| `touches` | `[{"id":1,"x":100,"y":280}]`, ids 1..9 |
| `wheel` | notches this frame only: `1` or `{"x":0,"y":-2}` |
| `text` | text typed this frame (needs text input started, as a focused text field does) |
| `pad2`, `pad3`, `pad4` | pads 1, 2 and 3 (players 2, 3 and 4): `{"buttons":["south"],"axes":{"lx":-1}}`, the same names as pad 0's `buttons` and `axes`. A pad key absent from an entry means that pad is unplugged from that frame on |

An entry without `keys`, `mouse` or `touches` means none held. Pad 0 is always connected; a 4-player script names `pad2`..`pad4` on every entry where those players play (`tests/input-players.json`). romdev's input ports are all connected, so a script that unplugs a pad while a later one stays (`pad2` absent, `pad3` given) fails there (`input-players-hotplug` skips romdev). romdev has no keyboard op, so gates with key scripts skip it. Natively the script travels in `CW_INPUT`; a game can also drive itself from `cw_set_raw_input_filter`, which runs at the same point on every host.

## Naming debug state

Gates read state by name, so name it for the reader:

- `area.thing` in lower case: `player.pos`, `player.on_floor`, `coins`, `game.ticks`, `tree.nodes`, `unit.failed`.
- Expose what a test asserts, not internals that drift: counts, positions, flags, and hashes of larger state (`phys.hash`, `bodies.hash`).
- Register in `init`; the pointers must stay valid for the app's lifetime.
- The engine registers some fields itself: `cw.frame`, `cw.tick`, `cw.pad0.*` .. `cw.pad3.*` (platform: each pad's `buttons`, `axes`, `connected`), `tree.nodes`, `tree.tick`, `render.draw_calls` (scene runner), `input.hash`, `input.ticks` (input), `audio.*` (audio). A gate without `fields` compares all of them; list `fields` when some (timings, host-dependent counters) must not be compared. `dbg.*` fields are the debug channel's mailbox and are never compared.
- Natively `CW_DEBUG_DUMP=file.json` writes every field at exit; romdev reads them with `wasm({op:'debugState'})`.

@gates
