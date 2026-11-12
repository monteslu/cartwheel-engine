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

### The GPU suite lock

Every run that touches the GPU goes through `node tools/with-gpu-suite.mjs <name> <command>` (the gate and `tests/all.mjs` take the same lock themselves), which holds one machine-wide lock so only one suite uses the GPU at a time. Waiting suites are served in arrival order (each writes a ticket; only the oldest live ticket may take the lock). A suite may hold the lock for at most `CW_SUITE_MAX_MINUTES` minutes (default 15; fractions allowed; 0 for no limit): past that its tests are killed, the lock is released and the run exits 124 with a message that says why, so one long run cannot starve the queue. Split long work into several calls of a few gates each. `tests/suite-lock.mjs` checks the limit and the order against a private lock directory, with a control for each.

## Continuous integration

CI (`.github/workflows/ci.yml`) runs on every push and pull request to main, on a GitHub runner with no GPU. One job fetches the pinned dependencies, builds the native tree, runs `node tools/gen-api.mjs --check` and `node tools/gen-docs.mjs --check`, and hands the build to the test jobs as an artifact. They run `tests/ci.mjs` with `CW_TEST_GPU=software`: every app gate on the native host through Mesa's llvmpipe (GL), every gate with a `webgpu` entry through lavapipe (WebGPU), and the scripted gates that need no other host. The GL gates are split over seven jobs (`node tests/ci.mjs gl-<k>of7`) and the WebGPU gates over six (`webgpu-<k>of6`), balanced by each gate's measured CI time in `tests/ci-times.json`; after adding gates or making one slower, refresh it from the latest runs with `node tools/ci-times.mjs` (needs the GitHub CLI). One more job builds every app's cart: the dual GL + WebGPU cart, the only kind there is (the gates run it on GL hosts for GL), then runs `tests/no-gl-only-carts.mjs`: every cart it made must import WebGPU as well as GL, a cart tree configured with `CW_WGPU=OFF` must stop at configure, and a GL-only control cart made on the spot must be caught. In software mode nothing is pinned, sandboxed or guarded, and the renderer checks expect llvmpipe. `native-pads` plugs virtual controllers in through uinput (`tools/vpad.c`), so CI loads the `uinput` module and makes `/dev/uinput` and the input devices it creates usable by the runner's user; on a desktop the session already grants both.

Local only (they need GPU hardware or a host CI does not have; `LOCAL_ONLY` in `tests/ci.mjs` gives the reason for each):

- the romdev, Chromium, wasmcart-native (`wcnative`) and wasmcart-libretro legs of every gate;
- `gpu-guard`, `fx-gpu-report`, `pipeline-perf`, `fx-perf-report`, `vis-perf-report`, `showcase-perf`, `nav-perf-report`, `profile-lighting`: GPU guards, timings and frame-time budgets;
- `gpu-no-display`, `gpu-cleanup`, `wcnative-pads`, `editor-channel`, `editor-play`, `editor-import`, `a11y-channel`, `net`, `plugins`, `project-templates`, `tutorials`, `docs`, `nav-determinism`, `noise-gpu`, `water-surface`, `water-see-through`, `warm-states`, `text-floor`, `fx-distort-fringe`, `decal-first-image`: they drive romdev, Chromium or wasmcart-native;
- `export`, `project-export`: the export toolchains, Wine and an Android emulator.

Run `node tests/all.mjs` on a machine with a GPU and those hosts for the full set.

## The gate

```sh
node tests/gate.mjs                                  # every gate in tests/gates.json, on every host it can run on
node tests/gate.mjs hello-input unit-scene           # some gates
node tests/gate.mjs --cpu platformer2d               # CPU only: no GPU, no suite lock (see below)
node tests/gate.mjs --gates-file mygame-gates.json   # another file in tests/ (comma separated for several; env CW_GATES_FILE)
node tests/gate.mjs unit-scene --hosts native,romdev # narrow the hosts (never adds one a gate cannot run on)
node tests/gate.mjs hello-input --hosts romdev --gpu both # romdev alone, GL and WebGPU (below)
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

Hosts by default: `native`, `romdev`, `chromium`, plus `wcnative` for gates without an input script (wasmcart-run takes no input and reports no state, so it is compared on frames only), plus `angle` when `CW_ANGLE_DIR` is set. Outputs land in `out/gate/<name>.<host>.png` and `.json`. A WebGPU run in Chromium waits for the GPU to finish each frame (and for the frame's readbacks to land) before it starts the next, as native runs and romdev do, so occlusion query counts and pixel reads arrive on the same frame on every host even after a frame of heavy GPU work such as an impostor bake. The romdev launcher explicitly selects the requested backend even for dual-capable carts; a GL gate must not rely on automatic backend selection. A romdev leg steps its frames in one call to the server and waits for the reply as long as the call can take: `CW_ROMDEV_TIMEOUT_MS` (default 300000) plus `CW_ROMDEV_FRAME_MS` (default 500) for each frame the call steps, so a 2400-frame run on a busy GPU is not cut off at five minutes. A call that runs past it fails with both names in the message.

A romdev leg sends ranges, never a request per frame (romdev is a shared server): the start requests (one each, each runs a frame), one `input` `sequence` call that holds each pad entry of the script until the next and runs to the last frame, then the frame, the state and a shutdown. A 700-frame run with a 10-entry pad script is 5 requests. Only a script with mouse, touch or wheel entries takes requests per entry. `run-cart` reports the count as `romdevCalls`, and `CW_ROMDEV_AGENT` names the caller to romdev (default `cartwheel-run-cart`).

### On romdev alone

A game's checks run through romdev, with no native run. `--hosts romdev` is that mode:

1. The game's dual cart on romdev with GL is the reference: its frame a real picture, `expect` against its state, the text floor, missing program states and prompt errors.
2. The control on romdev GL must differ from it in frame or state.
3. With `--gpu webgpu` (or `both`) the same cart runs again on WebGPU: the same checks on its own state, the named state exact against the GL run, the frame within `webgpu.vsGl` (default 0.01) and `webgpu.maxArea`, and its own control must differ from it and from GL.

The gate's `env` and `controlEnv` reach the cart as the debug channel's `env` request (below). Skipped, and said so, never counted as passing: gates whose `hosts` or `skip` leave romdev out, tool gates (`script`: they pick their hosts), `webgpu.controls` (native fault injections) and gates without a cart. The mode runs no GPU work of its own, so it does not take the GPU suite lock.

### A gate's environment on a cart host

A native run gets a gate's `env` in its process environment. A cart has none, so `run-cart --env '{"NAME":"value"}'` sends the built-in debug request `env NAME=value ... -- <request>` instead, riding on a request the host sends anyway so no host's frames move: before frame 1 with the first request on Chromium and wasmcart-native, with the first start request at frame 2 on romdev (its load runs frame 1). Every cart link wraps `getenv`, so the game's own `getenv` calls read the values with no change to the game.

A name the game read before the request arrived would run unset on romdev and agree with no other host, so the cart remembers the names read before the first `env` request and setting one fails the request, naming it (`romdev: the --env request failed: PP_ROUNDS was read before ...`). A game reads its gate switches with `cw_test_env(name)` (cartwheel/debug.h): NULL in the first frame on every host, the variable from frame 2 on, so a switch read where the mode is first needed behaves the same everywhere. A mode the game sets up in its init (a match length, a level) is applied when the switch is first seen, or given its own debug command (as coopplatformer's `level <n>`). Never cache a NULL from the first frame.

`CW_WORKERS=n` runs every host with the same job pool (n workers besides the main thread). Native runs read it directly. A cart has no environment, so Chromium's page reports n + 1 as `navigator.hardwareConcurrency`, and the GPU sandbox shows wasmcart-run n + 1 online CPUs, the hardware threads it hands the cart. `tests/nav-determinism.mjs` runs gates this way at several pool sizes.

## `tests/gates.json`

An array of gate objects. Every key `tests/gate.mjs` reads:

| Key | Meaning |
|---|---|
| `name` | the gate's name (what you pass on the command line) |
| `app` | the app target (`build/native/{samples,tests/unit,games,editor}/<app>`, `build/cart-wgpu/carts/<app>.wasc`) |
| `frames` | engine frames to run before capturing |
| `frameHz` | the virtual render rate (default 60); fixed ticks stay at the app's own rate |
| `input` | an input script in `tests/` |
| `assets` | a native asset root when it is not `samples/<app>/assets` |
| `hosts` | the hosts this gate may run on (replaces the default list) |
| `skip` | hosts to leave out (for example `romdev` for a text script) |
| `minColors` | unique colours the native frame must have (default 2) |
| `maxGt16` | the share of pixels allowed to differ by more than 16 levels (default 0.001) |
| `fields` | the state fields to compare (default: every field except the `dbg.*` mailbox) |
| `tol` | relative tolerance for numeric fields (default 0: exact) |
| `vsGl` | (top level) the romdev mode's bound on pixels over 16 levels apart between its GL and WebGPU runs, for a gate with no `webgpu` block (default 0.01) |
| `vsGlTol` | the tolerance between the GL and WebGPU runs of the romdev mode (`--hosts romdev`), for state a GPU readback rounds differently on each backend (default `tol`) |
| `glOnlyFields` | state the game measures on GL alone (a `glReadPixels` probe): the romdev mode's WebGPU run is not held to it, by `expect` or against GL |
| `expect` | `{field: value}` the native run must report exactly |
| `controlFrames` | frames for the control run (default: `frames - 1`, or `frames` with a control input) |
| `controlInput` | a different input script for the control |
| `startCmds` | debug-channel requests every host posts at load (`run-cart --start-cmds`), request i answered at the start of frame i + 2: how a test says what a real host would, e.g. `["touch_primary 1"]` for a phone |
| `controlStartCmds` | the control run's start requests (default: none, so a `startCmds` gate's control runs without them) |
| `env` | environment for every run of the gate, controls too (a control's `controlEnv` wins): how a game picks one mode for a gate, a mini-game or a skill level, say. Native runs get it as their environment, a cart as the debug channel's `env` request (a game reads the switch from frame 2: `cw_test_env`, above) |
| `controlEnv` | environment for the control run; a value `"repo:<path>"` names a repository file that is copied into `out/` first |
| `webgpu` | `true` or `{hosts, skip, maxGt16, vsGl, maxArea, controls}`: the gate also runs on WebGPU (`tests/gate.mjs --gpu webgpu`): every host against native WebGPU, native GL against native WebGPU within `vsGl` (default 0.01), the dual cart on romdev told to use GL against native GL. Each of `controls` (`{"env": {...}}`) is a native WebGPU run that must fail on the picture against both references; one with `"fields": [...]` may instead fail on those state fields (each differing from both), for a feature whose effect has left the last frame (impostors switched off on WebGPU only, at the end of a sweep that ends with every mesh near: `vis-impostor`) |
| `textFloor` | the text size floor (on by default for games: a games repository's apps, this repository's `games/` and a game's editor builds, `<game>_editor_*`): every run of the gate fails when any frame drew text under 18 px at 1080 lines, 1.7% of the canvas height (the state's `text.run_min_share`), naming the string. `false` turns it off, `true` turns it on for a sample, a number sets another floor (a share of the canvas height). See [localization and accessibility](guide:l10n-a11y), "Text size" |
| `script` | instead of all the above: a node script that runs its own checks and controls (exit 0 = pass) |
| `note` | what the gate proves, for humans; not read by the runner |

A typical gate, from `tests/gates.json`:

```text
{"name":"action2d","app":"action2d","frames":3000,"input":"action2d-pad.json",
 "controlInput":"action2d-pad-control.json","maxGt16":0.003,
 "fields":["g.state","g.score","g.hp","g.room","g.enemies","g.kills",...,"tree.nodes"],
 "expect":{"g.state":6,"g.room":3,"g.boss_hp":0,"g.deaths":0,"g.doors":2,"g.kills":9}}
```

### Text size

`node tools/check-text.mjs <screens.json> [--scales 1,2] [--shots DIR] [--gpu gl|webgpu]` walks a game's screens (each a run of the game's cart on romdev that ends on one, its `env` sent as the `env` request: the title, every menu, a stretch of play, pause, results) and reports the smallest text on each from the `text` debug request: body text under 26 px at 1080 lines (2.4%) fails unless the screens file lists it as `secondary`, and nothing may be under 18 px (1.7%). `--scales` runs every screen at each text scale, `--pref WxH` as a host that prefers that frame (1280x800 is a 7 inch handheld; romdev takes no preferred size from a client, so `--pref` needs `--host native`), `--shots` keeps each screen's last frame for a contact sheet. In a games repository it ships in the host SDK: `CW_PROJECT_ROOT=$PWD node <host sdk>/tools/check-text.mjs tests/<game>-text.json` (its input scripts are read from that repository's `tests/`). The `text-floor` gate runs it on `samples/text_size` and on that sample with a 12 px line, which must fail.

## Controls that must fail

A comparison of two broken runs looks like success, so every gate carries a control that must fail:

- **one frame early** (the default): the state and frame at N-1 must differ from N, which proves the gate sees motion;
- **other input** (`controlInput`): the run without the jump must not collect the coins;
- **an injected defect** (`controlEnv`): unit suites run the control with `CW_TEST_INJECT_FAIL=1`, which adds a failing check, so `expect: {"unit.failed": 0}` must break.

Scripted gates run their own controls and print `CONTROL PASSED` (a failure) when one does not fail.

## Flash check

`tools/flash-check.mjs` checks that a run never strobes ([Localization and accessibility](guide:l10n-a11y) has the rules and how to make flashes that pass). It checks every frame of a run from frame K (by default 2) in one-second windows. A game's run goes through romdev (`--host romdev`, the default for `--app`): the dual cart (`build/cart-wgpu`) is loaded on GL or WebGPU (`--gpu`), run to frame K-1 with the gate's start requests, environment and input script (`tools/run-cart.mjs --flash-from`), and then ONE `frame({op:'flash'})` call steps frames K to N, filters each frame as a native dump is and runs the same analysis and control on the server; a run of 1800 frames is three requests, never one a frame. Pad input and `@N` start requests inside the checked frames ride on that call; pointer or wheel input inside them cannot. `ROMDEV_URL` picks another server than 127.0.0.1:7331. `--host native` runs the native build, and `--dump` / `--dir` check frames captured already:

```text
node tools/flash-check.mjs --app fx_explosion --frames 600 [--from 60] [--input script.json] [--start-cmds "a;@300 b"] [--env '{"K":"v"}'] [--gpu webgpu]
node tools/flash-check.mjs --host native --app fx_explosion --frames 600     # the native build
node tools/flash-check.mjs --dump run.cwfd       # frames captured already (CW_FRAME_DUMP)
node tools/flash-check.mjs --dir frames/         # a directory of PNG frames, one a frame
```

- **General and red flashes** over every block of a sixth by a sixth of the screen (at every twelfth): at most 3 in any second.
- **Flashes together**: over every 10-degree field (a third by a third), a quarter of its pixels making the same change at once counts as a flash of the field, so small lights switching together add up while a moving picture does not.
- **Regular flicker** above 3 Hz and up to 30 Hz (60 frames a second sees up to 30) in any twelfth by twelfth of the screen, whatever the flashes count. Each cell is read over two-second windows (half-hertz bins), so a pulse at exactly 3 Hz is the bound and passes, and 3.5 Hz is the first rate that fails.
- `--max-rise X` also bounds the hardest two-frame rise of a block's luminance (a hard onset rather than a swell).
- The run must move (a frozen capture proves nothing), and every check runs its **control**: the same frames with an 8 Hz full-screen strobe laid over them must fail.

`CW_FRAME_DUMP=file` (native, with `CW_FRAMES`) is how the frames are captured: every frame from `CW_FRAME_DUMP_FROM` on, box-filtered down by `CW_FRAME_DUMP_SCALE` (default 8) in linear light, appended raw. A games repository calls the same tool with `CW_PROJECT_ROOT` set, or imports `tools/lib/flash.mjs` (`flashGate`, `readFrameDump`, `analyseFrames`, `withStrobe`, and `boxDown`: a cart host's RGBA frame filtered as the native dump filters it) for its own sweep. `tests/flash-check.mjs` holds the analyser to synthetic runs whose answers are known, and runs `fx_explosion` through romdev on GL and WebGPU with its frames kept (`--keep`): the numbers romdev gives must be the ones `tools/lib/flash.mjs` gives on those same frames.

## Unit suites

A unit suite is an app (`tests/unit/test_*.c`) that runs every test in `init` and publishes the counts as debug fields, so the gate compares them across hosts like any other state. `tests/unit/test.h` provides `CHECK(cond)`, `CHECK_EQ_INT(a, b)`, `FOLD(v)` (fold a result that must agree across hosts into `unit.checksum`) and `CW_TEST_SUITE("name", {"case", fn}, ...)`, which defines `cw_main`. Each suite publishes `unit.passed`, `unit.failed`, `unit.checksum` and `unit.done`; a failing check logs `file:line`.

Add a suite with `cw_add_app(unit_x SOURCES test_x.c)` in `tests/unit/CMakeLists.txt` and a gate:

```text
{"name":"unit-x","app":"unit_x","frames":2,"minColors":1,"fields":["unit.passed","unit.failed","unit.checksum","unit.done"],
 "controlFrames":2,"expect":{"unit.failed":0,"unit.done":1},"controlEnv":{"CW_TEST_INJECT_FAIL":"1"}}
```

## Scripted gates

Some checks need more than one run: several processes, a tool, a timing report. A gate with `script` runs a node script that prints `ok ...` / `FAIL ...` lines, runs its own must-fail control, and exits 0 only on success. Examples: `tests/cwpack.mjs` (asset packing), `tests/net.mjs` (cart-to-cart sessions), `tests/project-templates.mjs` ([Templates](guide:templates)), `tests/export.mjs`, the `*-perf.mjs` reports ([Profiling](guide:profiling)), and `tests/docs.mjs` / `tests/tutorials.mjs` for this site.

### The graphics settings' gates

- `quality-unit` (`tests/quality-unit.mjs`, no GPU): Resolution stays the player's on every preset and Auto across a save and a restart, every older save word restores its choice, a lever's five values apply and a four-entry lever array does not build, a slow host's Auto is on its rung within 3 seconds. Controls: `CW_TEST_QUALITY_OLD_RES`, `CW_TEST_QUALITY_V2_ONLY`, `CW_TEST_QUALITY_NO_JUMP` bring back the old behaviour one at a time, and each must fail its case.
- `quality-sim-same`: the same replay at Very Low and High simulates the same bits. Control: `CW_TEST_QUALITY_INTO_SIM`.
- `quality-grounding`: Very Low keeps the shadows High draws at the base of each caster (`CW_SHADOW_CONTACTS=1` and the `shadow_contacts` request put each caster's base on screen; `CW_TEST_NO_SHADOWS=1` draws the frame without them). A caster whose box stands around the camera (a ring of walls, a floor: the request marks it `around`) has no single base and is not judged; a long one (a wall, a fence, a row of stands: `along`) is judged at the point of its base line nearest the camera; `--centre` judges every caster at its box's middle, the old rule. `--preset` judges another preset than Very Low. Control: Very Low with no shadows.
- `quality-grounding-judge`: the judge (`tools/lib/grounding.mjs`) on made-up frames, no GPU. Controls: the old rule fails a frame wrongly over a ring of walls around the camera and a long wall; no shadows fails; a wide caster that is neither is still judged at its middle.
- `quality-range`: each preset's GPU time ([Profiling](guide:profiling)). Control: `CW_TEST_QUALITY_FLAT=1` natively; `--host romdev` measures the built cart through romdev's `frame({op:'timing'})` (control: every preset asked for High). A game's perf gates time its cart the same way with `tools/lib/romdev-timing.mjs`.
- `quality-verylow-post`: on Very Low the height fog and FXAA run inside the tonemap pass, reported as fused, and the frame is the separate passes' (`CW_TEST_NO_FUSION=1`) but for FXAA's edges. Controls: `CW_TEST_VERYLOW_POST=fog` and `=aa` leave each step out of the fused pass and must break the bound.

## Gates of another repository

A repository of games built with this engine (configured as its project with `-DCW_PROJECT_DIR`) keeps its own `tests/gates.json`, input scripts, `build/` and `out/`, laid out as here. `CW_PROJECT_ROOT=<that repository>` points `tests/gate.mjs` and `tools/run-cart.mjs` at them: gates, inputs and scripts come from its `tests/`, apps from its `build/` (a project app is found through `build/native/apps/<app>.json`), and output goes to its `out/`. [cartwheel-examples](https://github.com/monteslu/cartwheel-examples) runs its games' gates this way.

The engine source such a repository keeps has no `deps/` of its own. Its Chromium host needs wasmcart: run `scripts/fetch-deps.sh --only wasmcart-wgpu` in that copy (52 MB, the rest of the engine's dependencies stay out). Without it, `run-cart.mjs` stops and names this command.

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
| `touches` | `[{"id":1,"x":100,"y":280}]`, ids 1..9; a finger absent on the entry before lands (a tap) on this one, on every host |
| `wheel` | notches this frame only: `1` or `{"x":0,"y":-2}` |
| `text` | text typed this frame (needs text input started, as a focused text field does) |
| `pad2`, `pad3`, `pad4` | pads 1, 2 and 3 (players 2, 3 and 4): `{"buttons":["south"],"axes":{"lx":-1}}`, the same names as pad 0's `buttons` and `axes`. A pad key absent from an entry means that pad is unplugged from that frame on |

An entry without `keys`, `mouse` or `touches` means none held. Pad 0 is always connected; a 4-player script names `pad2`..`pad4` on every entry where those players play (`tests/input-players.json`). romdev's input ports are all connected, so a script that unplugs a pad while a later one stays (`pad2` absent, `pad3` given) fails there (`input-players-hotplug` skips romdev). romdev holds a script's keys on its input sequence steps (romdev 0.155.0 or later; an older server drops them and the run fails saying so); it has no text op, so gates with `text` entries skip it. Natively the script travels in `CW_INPUT`; a game can also drive itself from `cw_set_raw_input_filter`, which runs at the same point on every host.

## Naming debug state

Gates read state by name, so name it for the reader:

- `area.thing` in lower case: `player.pos`, `player.on_floor`, `coins`, `game.ticks`, `tree.nodes`, `unit.failed`.
- Expose what a test asserts, not internals that drift: counts, positions, flags, and hashes of larger state (`phys.hash`, `bodies.hash`).
- Register in `init`; the pointers must stay valid for the app's lifetime.
- The engine registers some fields itself: `cw.frame`, `cw.tick`, `cw.pad0.*` .. `cw.pad3.*` (platform: each pad's `buttons`, `axes`, `connected`), `tree.nodes`, `tree.tick`, `render.draw_calls` (scene runner), `render.skipped` (3D: every tree's; it counts draws three.c could not make, such as a state the program table lacks or a program refused as incomplete, and `tools/run-cart.mjs` fails any run where it is not 0, on every host and backend, because hosts that all skip the same draws agree with each other; `--allow-skipped` is for a test that skips on purpose), `input.hash`, `input.ticks` (input), `audio.*` (audio). A gate without `fields` compares all of them; list `fields` when some (timings, host-dependent counters) must not be compared. `dbg.*` fields are the debug channel's mailbox and are never compared.
- Natively `CW_DEBUG_DUMP=file.json` writes every field at exit; romdev reads them with `wasm({op:'debugState'})`.

@gates


### State-only gates without the GPU lock

`node tests/gate.mjs --cpu [gate ...]` runs the native host on Mesa's software renderer, takes no GPU suite lock and never touches a GPU, so it does not queue behind other suites. It runs only gates with an `expect` block (their named state is the check; frames are not compared across hosts), each with its control that must fail; gates without `expect` are listed as skipped. Use it for simulation checks (a controller, a physics course, a soak's state). A rendering gate still needs the GPU legs.

A new 3D app with no program table draws nothing for the states it lacks: the engine logs an error naming `node tools/programs.mjs <app>`, and `CW_PROGRAMS_STRICT=1` makes the app exit with status 4 on the first one (`tests/programs-loud.mjs`). A regenerated table whose WebGPU program the WGSL compiler (Tint, inside Dawn) rejects stops `tools/programs.mjs` with the state and the compiler's message: the captures run on a mock GPU that accepts anything, so each WebGPU program is compiled on Mesa's software Vulkan (lavapipe) as a host would (`tests/programs-wgsl.mjs`, with a syntax error and an unlinkable pipeline as the controls that must fail). A build whose `deps/` is off `scripts/deps.lock` stops before compiling and names the fix (`scripts/lib/check-deps.sh`; `CW_SKIP_DEPS_CHECK=1` skips it).
