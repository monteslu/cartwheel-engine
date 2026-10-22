# App and platform

A {{name}} game never writes a main loop: it returns a description of itself from `cw_main` and the engine calls it back at a fixed simulation rate. This page covers that lifecycle, the window and GL context, files (read-only assets and the writable project layer), `project.cwproj`, and the environment variables that make a native run scriptable.

Headers: [cartwheel/platform.h](header:platform.h), [cartwheel/vfs.h](header:vfs.h), [cartwheel/project.h](header:project.h), [cartwheel/process.h](header:process.h). The smallest complete app is `samples/hello`; the timing guarantees are proven by the `timing-30` and `timing-144` gates ([tests/unit/test_timing.c](repo:tests/unit/test_timing.c)).

## One code path, two loop owners

The same source builds natively (real SDL2, GLES 3.0 through Mesa or ANGLE) and as a cart (wasmcart-sdl2 on the wasmcart ABI, GL imported from the host). The only differences live in `src/platform/native.c` and `src/platform/cart.c`:

| | Native | Cart |
|---|---|---|
| Who owns the loop | `main()` runs `cw_main`, then loops | the host calls `wc_init` (runs `cw_main` and the platform's init), then `wc_render` once per frame; the app's `init` runs in the first frame |
| Clock | the performance counter, or a virtual 1/60 s clock in test runs | the host's clock, a fixed virtual clock in deterministic runs |
| Audio callback | SDL's audio thread | a job-pool thread, while the frame renders |
| Assets | a `.cwpak` and/or a loose directory | the cart's bundle |
| Job workers | `cores - 1` | the host's hardware threads - 1 (wasi-threads; 3 on a host too old to say) |

Rendering is OpenGL ES 3.0 with WebGL2 semantics everywhere ([cartwheel/gl.h](header:gl.h)): one feature level, no fallbacks.

## Graphics backends: GL and WebGPU

WebGPU is a second backend ([cartwheel/gpu.h](header:gpu.h)), chosen once at startup, never per frame. GL stays the default. Natively `CW_GPU=webgpu` (or `cw_app_desc.gpu = CW_GPU_WEBGPU`) renders through Dawn (native-dawn) on the window; carts built in the `cart-wgpu` tree import both APIs and follow the host (`WC_HOST_FLAG_GPU_WGPU`), so one cart runs on GL-only and WebGPU hosts. Asking for WebGPU where it cannot be had is an error, never a quiet fallback. Today the 2D renderer, the UI and the scene tree's 2D canvas run on WebGPU with the same pictures and state as on GL; 3D waits on three.c's WebGPU backend and logs that once. [cw_gpu_backend](api:cw_gpu_backend) says which API a run uses, [cw_gpu_adapter_info](api:cw_gpu_adapter_info) which GPU, and [cw_gpu_error_count](api:cw_gpu_error_count) how many WebGPU errors it hit (the debug field `cw.gpu.errors`; tests fail on any). Apps that clear the window themselves call [cw_gpu_clear_window](api:cw_gpu_clear_window) instead of `glClear`. An app that calls GL itself (its own readbacks, its own three.c renderer, the editor frame) is built with `cw_add_app(... GL_ONLY)`: the `cart-wgpu` tree makes no dual cart for it, so its cart is the GL one and never claims WebGPU, and natively it refuses `CW_GPU=webgpu` at startup with a message naming `GL_ONLY`; `tests/wgpu-carts.mjs` runs every dual cart the build lists (`build/cart-wgpu/carts.txt`) on WebGPU in Node and fails on any that traps. `tools/run-cart.mjs --gpu webgpu` and `tests/gate.mjs --gpu webgpu` run the same apps on WebGPU on every host.

## The app description

[cw_app_desc](api:cw_app_desc) holds the logical size, the tick rate, flags and the callbacks. `hello` returns this:

@include samples/hello/main.c#main

The fields that matter most:

- `width` / `height`: the logical size, the coordinate space of input, the 2D canvas and the UI. `cw_window_size` is logical, `cw_drawable_size` is pixels.
- `fit`, `aspect_min`, `aspect_max`: how the logical size meets a screen of another aspect, which a cart learns from its host's preferred size (RetroArch's Internal Resolution core option; a handheld's 640x480 or 1280x800 panel). `CW_FIT_LETTERBOX` (the default) keeps `width` x `height` everywhere, so the frame is the largest of that aspect that fits and a 16:9 game on a 4:3 handheld shows bars. `CW_FIT_HEIGHT` keeps the design height and lets the logical width follow the screen (Hor+): a 960x540 game is 720x540 on 4:3, 864x540 on 16:10 and 1280x540 on 21:9, the 3D camera sees more or less to the sides at the same vertical field of view, a plain 2D canvas is the wider or narrower logical size, UI anchored to the edges stays there, and there are no bars for any aspect between `aspect_min` and `aspect_max` (0 = 4:3 and 2.4, which takes in the 2560x1080 and 3440x1440 screens); beyond them the width is clamped and the host shows bars. `CW_FIT_WIDTH` is the same with the axes swapped (Vert+): a vertical shooter keeps its width and gains lines on a 4:3 screen. Lay the game out against `cw_window_size` every frame (natively the window resizes too), scale a HUD by the window height over `cw_design_size`'s height instead of a literal, and keep split-screen viewports as fractions of the window. `cw_fit_logical_size` answers the size for any screen without running, for tests and tools. Natively `CW_HOST_PREF=WxH` in the environment makes the app behave as a cart on a host preferring that frame, the fitted logical size included: the window opens at that frame, so `CW_HOST_PREF=640x480 CW_FRAMES=120 CW_SCREENSHOT=out.png` is a handheld's picture. A cart in a deterministic run never takes its host's preference, so the cross-host gates keep the design size; natively `CW_HOST_PREF` applies with `CW_FRAMES` too, which is how an aspect gate takes its pictures.
- `render_scale`: framebuffer pixels per logical unit. The 3D scene, post and 2D render at the framebuffer's resolution while game code keeps its logical coordinates, so HUD text and shapes rasterize at full resolution instead of being upscaled. 0 (the default) is auto: a cart follows the size its host prefers (`wc_host_info_t` preferred width and height, such as RetroArch's Internal Resolution core option), taking the largest scale that fits inside it, so a 960x540 game renders 960x540 at 540p, 1280x720 at 720p, 1920x1080 at 1080p and 3840x2160 at 4K, and a preference of another aspect fits the game inside it (640x480 gives 640x360, shown at the game's aspect). With no host preference, natively, and in a deterministic run (a test's pixels never depend on the host), auto outputs 1080 lines. A value above 0 is fixed and wins over the host's preference as well: an app sets it when it depends on the pixel count (a benchmark, a test reading back its pixels, the editor). A cart declares the scaled size as its framebuffer, settled in `wc_init` once the host's preference is known; natively the window opens at it (lowered to fit the display), and `CW_RENDER_SCALE` overrides it. It never reaches the simulation: pointer positions arrive logical on every host.
- `pixel_scale`: a chunky pixel look. The frame renders at 1/N of the output and is scaled up N times with nearest-neighbour, so its pixels stay square and crisp; 0 or 1 is off. The whole frame is pixelated, 3D, post and 2D, unless crisp UI is on. Where the output has a fixed size (a native window, the browser canvas, wasmcart-native without a preference) the output keeps its size and the picture is centred with black bars when the output is not a multiple of N. A cart whose host gives a preferred size hands the host the low frame itself (the preference's frame / N) and leaves the scaling to it, so it is never scaled twice; in RetroArch that is Settings > Video > Scaling > Integer Scale on, and bilinear filtering off (Settings > Video > Bilinear Filtering): a 540p Internal Resolution on a 1080p screen is then exactly 2x crisp pixels, 270p exactly 4x. `CW_PIXEL_SCALE` sets it natively. Pointer and touch positions stay logical through the scale and the bars, and the simulation never sees it.
- `ui_min_lines`: crisp UI, a choice an app offers its players (0, the default, offers none). Off, the UI is pixelated with the scene. On (`cw_crisp_ui_set(true)`, from a graphics menu), the 2D window passes (HUD, menus, text) draw at a resolution of at least this many lines while the scene keeps the render resolution: the frame is the smallest whole multiple k of the scene that reaches it, the scene is scaled up k times (still chunky) and the UI draws on top. A cart at 270p offering 540 hands its host a 540-line frame whether crisp UI is on or off, so the size never changes while it runs. "The UI" is every 2D window pass from the first one in a frame on, so draw the 3D scene first. Keep the choice in the save with `cw_crisp_ui_saved()` / `cw_crisp_ui_restore()` (one byte, as `cw_quality_saved` does). `CW_UI_MIN_LINES=N` offers N lines and turns it on natively; the `crisp_ui [1|0]` debug command turns it on or off on any host.
- `cw_render_res_label()` (cartwheel/quality.h) is the render resolution as a graphics menu shows it, "720p" or "1080p": the scene's lines (`cw_render_size`), the resolution setting applied. The output's size is the host's; the resolution setting (a share of it, see [graphics settings](rendering-3d.md#graphics-settings-and-presets)) is the player's.
- `fixed_hz` (default 60) and `max_substeps` (default 5): the simulation rate and the catch-up cap per rendered frame.
- `debug`: export the debug channel ([Debug channel](guide:debug-channel)). The gates need it; set it in every sample and game.
- `deterministic` and `net`: cart manifest flags (deterministic replay, peer connections).
- `workers`: job threads besides main, 0 = auto, below 0 = none.

Scene apps do not fill the callbacks themselves: [cw_scene_app](api:cw_scene_app) wraps a desc so the engine loads a `.cwscene`, runs the tree and renders it, and your `init` / `fixed_update` / `render` run alongside (see [Scenes](guide:scenes)).

## The frame

Every rendered frame runs, in order:

1. Advance the frame arena (`cw_frame_alloc` scratch from last frame is gone), finish async resource loads within their budget, answer pending debug-channel requests.
2. Poll devices into the raw input snapshot; the raw input filter and (natively) the `CW_INPUT` script apply here.
3. Add the elapsed time to an accumulator (a stall longer than 0.25 s is not simulated) and run `fixed_update(dt)` while a whole tick fits, at most `max_substeps` times. Input actions take a per-tick snapshot before each tick. A backlog beyond the cap is dropped instead of spiralling.
4. `update(frame_dt)` once.
5. `render(alpha)` once, where `alpha` is how far the accumulator is into the next tick (0..1).

All game state changes in `fixed_update`; `update` is for things tied to real frames (UI animation, camera smoothing that does not feed state); `render` only draws. `hello` keeps the previous tick's values and interpolates with `alpha`, so motion is smooth at any refresh rate while the simulation stays exact:

@include samples/hello/main.c#fixed_update

@include samples/hello/main.c#render

Because ticks depend only on simulated time, a run rendered at 30 Hz and one rendered at 144 Hz reach the same state after the same simulated minute. The `unit_timing` app shows it (frame counts differ, tick and state do not):

```sh
node tools/run-cart.mjs --host native --app unit_timing --frames 1801 --frame-hz 30 --state out/t30.json
node tools/run-cart.mjs --host native --app unit_timing --frames 8641 --frame-hz 144 --state out/t144.json
```

```text
{"cw.frame":1801,"cw.tick":3601,...,"ticks":3601,"pos":170.64678955078125,"rng_hash":890103754}
{"cw.frame":8641,"cw.tick":3601,...,"ticks":3601,"pos":170.64678955078125,"rng_hash":890103754}
```

The debugger's pause (`pause`, `step <ticks>` over the debug channel) sits in the same place: a paused frame still renders but runs no tick, a step frame runs exactly one.

### Warm-up: progress instead of a frozen first frame

Work a game does before its first playable frame (generating textures, building scenes, compiling shaders) can take seconds on a phone. `warmup` in the app description spreads it over frames: the engine calls it once per frame, before any tick, until it returns true, and draws a minimal loading screen meanwhile (the stage and a bar). Report progress with [cw_loading_set](api:cw_loading_set)`(done, total, stage)`; it is also the debug field `cw.loading` (`[done, total, stage]`), which the web export's loading panel reads to carry its bar past the download. A scene app ([cw_scene_app](api:cw_scene_app)) adds a shader phase after the app's own warm-up: it renders the tree's 3D scene under the loading screen a slice at a time, as many slices as fit in about 12 ms of each frame, so its programs compile a few per frame (a fast machine is through in a few frames, a slow one still shows the bar move), and the last slice runs the app's render too (fonts, 2D programs). The app's `init` runs at the start of the first frame, not at startup, so starting a cart (`wc_init`) is quick. With a spread warm-up that first frame is only the loading screen, and `init` runs in the next frame under it: a player sees the screen as soon as the platform is up, before any game asset, scene or shader. Spreading it over frames is the default on every host (native, the browser, RetroArch, wasmcart-native, Android), so the loading screen is the first thing a player sees. A test does the whole warm-up inside the first frame instead, so frame counts and input timing match a run without one: natively a test run (CW_FRAMES; `CW_WARMUP_SPREAD=1` spreads it too, to look at the screen), in a cart a deterministic run (the host flag `WC_HOST_FLAG_DETERMINISTIC`, as romdev's test loads set) or a host that sends the debug request `warmup_spread 0` before the first frame (`tools/run-cart.mjs` does on Chromium and wasmcart-run, which have no deterministic mode). `tools/run-cart.mjs --warmup-spread` keeps the spread on any host but romdev;

Optional callbacks: `event` receives raw [cw_event](api:cw_event)s (resize, text, focus, pads added), and `shutdown` runs when a native app leaves the loop ([cw_quit](api:cw_quit) is a no-op in a cart; a host owns its lifetime). Games normally read input through actions ([Input](guide:input)); [cw_input_raw](api:cw_input_raw) is the raw per-frame snapshot, and [cw_set_raw_input_filter](api:cw_set_raw_input_filter) lets an autopilot or demo player rewrite it at the same point on every host.

## Assets: read-only files

Assets are what ships with the game. Paths are relative to the asset root (`fonts/a.ttf`, never `assets/fonts/a.ttf`), `/`-separated, without `..`. Natively the engine looks, in order, at:

1. the pack named by `CW_PAK`, else `<executable>.cwpak` when it exists (written by `cwpack`, see [Resources](guide:resources));
2. loose files under `CW_ASSETS`, else `<executable>.assets` beside the binary (the build links an app's `ASSETS` directory there), else `./assets`.

A cart reads its own bundle (the same directory, packed by wasmcart-pack). [cw_asset_load](api:cw_asset_load) returns a whole file; most code loads through typed resources instead. In a cart call the asset functions from the main thread: a lazily loaded web cart suspends there while bytes arrive.

## The project layer: writable files

[cartwheel/vfs.h](header:vfs.h) is a layered, writable view for the editor and tools. Reads try, top first: an in-memory layer (every write lands here too), the project directory on disk (native, when `CW_PROJECT` or [cw_vfs_set_root](api:cw_vfs_set_root) set one), then the assets. Writes go straight to disk natively; a cart cannot write files, so its changes stay in memory and travel as one archive through the debug channel (`vfs export`, `vfs import <base64>`). Deleting a file a read-only layer still has leaves a removal mark, so the deletion travels too.

`samples/project_edit` is that round trip: a cart edits its level, `vfs export` carries the change out, a native run imports it ([tests/vfs.mjs](repo:tests/vfs.mjs)).

## project.cwproj

Projects made from an editor template carry a `project.cwproj` of `key = value` lines, read through the project layer so a game sees the same file natively and in a cart. A template's file looks like this:

@include editor/templates/empty_2d/project.cwproj

Keys the engine reads (the full list is in [cartwheel/project.h](header:project.h)):

| Key | Used by |
|---|---|
| `name`, `main_scene` | the window title, the scene `cw_scene_app` loads |
| `render.width`, `render.height`, `render.msaa`, `render.vsync`, `render.fixed_hz` | [cw_project_apply_app](api:cw_project_apply_app) |
| `input.<action> = key:space, pad:south` | [cw_project_load_input](api:cw_project_load_input) |
| `layer_names_2d`, `layer_names_3d`, `gravity_2d`, `gravity_3d`, `pixels_per_meter`, `substeps` | physics nodes |
| `app`, `sources`, `scenes`, `game_exe`, `game_cart`, `export.<preset>.*` | the editor, `tools/cwproject.mjs` and `tools/cwexport` |

A template's `main.c` wires it in `cw_main`:

@include editor/templates/empty_2d/src/main.c

[cw_project_set](api:cw_project_set) edits one key in place, keeping comments and order.

## Child processes

[cartwheel/process.h](header:process.h) starts a program and reads its merged output line by line without blocking; the editor runs builds and exports with it. It is native and POSIX only (start fails with a message in a cart and on Windows). The child runs in its own process group, so `cw_process_kill` stops everything it started. Launching a game for play-in-editor goes through [cartwheel/remote.h](header:remote.h) instead ([Debug channel](guide:debug-channel)).

## Environment variables (native)

These make a native run scriptable; `tools/run-cart.mjs --host native` sets them for you.

| Variable | Effect |
|---|---|
| `CW_FRAMES=N` | run exactly N frames on a virtual clock (`CW_FRAME_HZ`, default 60), vsync off, then exit |
| `CW_SCREENSHOT=f.png` | with `CW_FRAMES`: write the last frame |
| `CW_DEBUG_DUMP=f.json` | write the named debug state at the end of a `CW_FRAMES` run, or when the game quits |
| `CW_DEBUG_CMDS="a;b"` | with `CW_FRAMES`: after the last frame, run each debug command and print one JSON reply per line |
| `CW_INPUT=file` | drive pads, keys, mouse, wheel, touches and typed text from a script (written by `run-cart.mjs` from its JSON format) |
| `CW_ASSETS=dir`, `CW_PAK=file` | the loose asset root and the asset pack |
| `CW_PROJECT=dir` | the writable project directory |
| `CW_SEED=n` | the value `cw_host_seed` returns |
| `CW_WORKERS=n` | job worker count |
| `CW_START_PAUSED=1` | the debugger's pause after frame 1 |
| `CW_REMOTE=fd:N` or `tcp:HOST:PORT` | serve the debug channel on a socket (set by `remote.h`) |
| `CW_DEBUG=1` | print debug-level log lines |
| `CW_GPU=gl\|webgpu` | the graphics backend ([cartwheel/gpu.h](header:gpu.h)); default GL |
| `CW_WGPU_POWER=high\|low` | the WebGPU power preference (default low power); test harness sets it from the pinned GPU's class |
| `CW_WGPU_EXPECT_DEVICE=<hex id>` | WebGPU: refuse any adapter but this device id |
| `CW_WGPU_VALIDATION=1\|0` | WebGPU: keep or skip Dawn's command validation. Default: skipped in a release run, kept in a `CW_FRAMES` run and a debug build |
| `CW_GPU_TIMERS=1` | time every render pass on the GPU (GL timer queries, WebGPU timestamps) for `cw_pipeline_pass_stats` and the `pipeline` debug command; off by default, since reading the timers costs CPU every frame (the `pipeline` command also turns them on) |
| `CW_TEST_WINDOW=1` | a `CW_FRAMES` test run keeps a real window (default: SDL offscreen driver, no window, no WebGPU surface) |

A run with input and captured state:

```sh
node tools/run-cart.mjs --host native --app hello --frames 120 --input tests/hello-input.json --state out/hello.json
```

```text
{"cw.frame":120,"cw.tick":120,"cw.pad0.buttons":0,"cw.pad0.axes":[0,0,0,0,0,0],"cw.pad0.connected":1,"x":0.66666674613952637,"angle":3.000002384185791,"ticks":120}
```

The same command with `--host romdev` or `--host chromium` runs the cart; the gates compare the two.
