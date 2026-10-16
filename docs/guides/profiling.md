# Profiling

What {{name}} measures and how to measure a change without fooling yourself. The profiler records where each millisecond of a frame goes, on every thread, and writes it as a trace you open in a timeline viewer. Beside it, the debug channel, the stats functions in the headers and the perf reports in `tests/` give totals.

## A frame on a timeline

`cartwheel/profile.h` records named zones on the main thread, every job worker and the native audio thread. The engine already has zones on every phase of a frame:

| Zone | What it covers |
|---|---|
| `frame` | the frame's work, from its start to the end of its render submission (the wait for the swap is not in it) |
| `input`, `debug.channel`, `resources.frame` | events and input, debug requests, resource loads finishing |
| `fixed_update` | one fixed tick (one zone per tick) |
| `tree.fixed_update`, `tree.update`, `tree.pre_render` | the scene tree's phases |
| `physics2d.step`, `physics3d.step` | a physics world's step |
| `anim.player`, `anim.tree` | an AnimationPlayer or AnimationTree advancing |
| `update`, `render` | the app's update and render |
| `pipeline.render`, `render3d.prepare`, `render3d.draw` | the render pipeline, the 3D scene's preparation and draw submission |
| `pass <name>` | each render pass (`pass scene`, `pass ssao`, `pass bloom`, ...) |
| `visibility.cull` | the visibility pass's culling |
| `audio.mix`, `audio.hand_off` | the audio mixer (natively on SDL's audio thread, in a cart on the job pool), and a cart handing its frames to the host |
| `present` | the swap (natively) or a WebGPU frame's hand-back |
| `<zone>/job` | job chunks on any thread, named after the zone that submitted the job (`physics3d.step/job`) |

A game adds its own:

```c
#include "cartwheel/profile.h"

static void think(void) {
  CW_PROFILE_SCOPE("game.ai");       /* a zone until the end of the block */
  cw_parallel_for(n, 32, plan, ctx); /* its chunks show on the workers as "game.ai/job" */
}
CW_PROFILE_COUNTER("game.enemies", alive); /* a per-frame number: a counter track */
```

`samples/profile_demo` has something in every phase: 160 crates in a pile, an animation, a looping tone, and a job-pool zone of its own.

### Capturing a trace

Natively, set `CW_PROFILE_TRACE` to a file. Frames `CW_PROFILE_TRACE_START` (default 0) to that plus `CW_PROFILE_TRACE_FRAMES` (default 120) are recorded and the file is written when the last one ends, or at exit with what was recorded:

```sh
CW_FRAMES=200 CW_PROFILE_TRACE=trace.json CW_PROFILE_TRACE_START=60 CW_PROFILE_TRACE_FRAMES=60 build/native/samples/profile_demo
```

From a cart (or any running game) the debug channel does it: `profile trace <frames>` records that many frames and then answers with the trace's size, and `profile trace_get <offset>` returns the trace in pieces. `tools/cwtrace.mjs` does both through romdev and writes the file:

```sh
node tools/cwtrace.mjs build/cart/carts/profile_demo.wasc -o trace.json --frames 60 --start 10
node tools/cwtrace.mjs --session <romdev session> -o trace.json   # a cart already running in romdev
```

```text
wrote trace.json: 60 frames, 4153 events, 24 threads, 3.943 ms CPU per frame
```

Code can ask for one too: `cw_profile_trace_json(frames, &len)` returns the last frames as the same JSON.

The file is Chrome trace event JSON. Open it in [ui.perfetto.dev](https://ui.perfetto.dev) (drag the file in), `chrome://tracing` or speedscope. Each thread is a track ("main", "worker 1" ..., "audio", "GPU"), each `frame` zone carries its frame and tick numbers, and the counters (`frame cpu ms` and the game's own) are counter tracks.

### GPU time per pass in the trace

Natively, `CW_PROFILE_GPU=1` (or `profile on gpu`, `profile trace <frames> gpu`) adds a "GPU" track with each render pass's GPU time, from the timer queries below. The durations are measured; their positions are not: each frame's passes are laid end to end from the start of that frame's render (or from where the previous frame's GPU work ended). The results arrive several frames late, so the last few frames of a trace have none. The track is off unless asked for because the timer queries cost some CPU time of their own (below). Carts never have it.

### Other profile requests

| Request | Reply |
|---|---|
| `profile on [gpu]`, `profile off` | start or stop recording (the last 1024 frames and up to 65536 zones per thread are kept) |
| `profile status` | on or off, compiled in or not, frames kept, the threads seen, a trace waiting to be read |
| `profile frames [n]` | the last n frames: `[frame, cpu_ms, wall_ms, ticks]` |
| `profile top [frames] [n]` | the n zones with the most exclusive time over the last frames, averaged per frame, with inclusive time, calls and threads |
| `profile clear` | forget what was recorded |

### What it costs

Compiled out (`-DCW_PROFILER=OFF` at configure time) the macros are empty. Compiled in but off (the default), a zone is one relaxed atomic load: 0.3 ns natively, about 1 ns in a cart. On, a zone reads the clock twice and writes 32 bytes into its thread's ring: about 55 to 90 ns natively on the 890M machine and about 0.5 to 0.8 us in a cart, where the clock is a call into the host. `profile_demo` records about 23 zones a frame on the main thread natively (0.001 ms a frame) and 55 in a cart (0.03 ms a frame). Consecutive job chunks under one name on a thread are recorded as one zone, which keeps that count low. The `profile-trace` and `profile-cart` gates hold the main thread's cost under 0.1 ms a frame and check that profiling changes nothing the game simulates.



## Ask a running game

Every debug app answers `stats` with the frame and tick, the job pool's thread count and work since the last call, and memory per allocation tag. Natively, from `samples/scene3d` after 30 frames:

```sh
CW_FRAMES=30 CW_DEBUG_CMDS='stats;pipeline' build/native/samples/scene3d
```

```text
{"frame":30,"tick":30,"time":0.483333,"threads":24,"jobs":0,"chunks":0,"mem":{"general":0,"core":1125894,"scene":546328,"render":5128,"render2d":168,"physics":0,...},"mem_total":1677518,"mem_peak":1677831}
{"hdr_format":"RGBA16F","timer_query":true,"max_samples":8,"gpu_ms":0.0296,"passes":[{"name":"lights","phase":"shadow","ran":true,"gpu_ms":0.0001},...,{"name":"scene","phase":"opaque","ran":true,"gpu_ms":0.0290},{"name":"ssao","phase":"post","ran":false,"gpu_ms":-1.0000},...]}
```

The same requests work in a cart through romdev (`wasm({op:'command', text:'stats'})`). Other commands report one subsystem each:

| Command | Reports |
|---|---|
| `stats` | frame, tick, job threads, jobs and chunks run, bytes per memory tag, total and peak |
| `pipeline` | every render pass with its phase, whether it ran, and its GPU ms; the HDR format and any program states three.c lacks |
| `phys` | each physics world's body, contact and joint counts, its state hash, and the library's own step time |
| `res [substring]` | loaded resources with type, state, references and bytes, and totals |
| `environment` | sky updates, fog, probe captures and baked lighting this frame |

## GPU time per pass

Natively every render pass is wrapped in a `GL_TIME_ELAPSED` query (`EXT_disjoint_timer_query`) once timing is asked for (`CW_GPU_TIMERS=1`, the `pipeline` request, or the profiler's GPU track); otherwise no query is issued. The results are read back in a batch every 8 frames, only those at least 3 frames old, so they arrive 3 to 11 frames late. Reading GL state makes the game's thread wait for the driver (with Mesa's glthread, for every command queued so far), so reading back every frame cost `profile_demo` about 0.5 ms of CPU a frame on the 890M (0.94 ms against 0.43 with timing off); read back in batches it costs about 0.06 ms. `cw_pipeline_pass_stats` gives each pass's latest time and a 1/16 exponential average, and `cw_pipeline_gpu_ms` the total of the passes that ran. A pipeline rendered from inside another pass is not timed separately (GL has one timer query open at a time): the enclosing pass's time includes it.

Carts never issue timer queries: in Chromium on ANGLE's GL backend they made rendering nondeterministic. In a cart `timer_query` is false and every `gpu_ms` reads -1, on every cart host. To time GPU work in a cart, measure whole frames from outside (below).

## Stats in the headers

Code (a game's own HUD, a test, a report) reads the same numbers directly. Each is cheap to call and returns plain counters:

| Function | Header | What it counts |
|---|---|---|
| `cw_mem_get_stats` | `cartwheel/core.h` | bytes and allocations per tag, total, peak |
| `cw_jobs_get_stats` | `cartwheel/jobs.h` | jobs, chunks, chunks per thread |
| `cw_pipeline_pass_stats`, `cw_pipeline_gpu_ms` | `cartwheel/pipeline.h` | per-pass GPU ms (native) |
| `cw_render3d_get_stats` | `cartwheel/render3d.h` | draw calls, triangles, programs, CPU ms of the 3D draw |
| `cw_r2d_get_stats` | `cartwheel/render2d.h` | passes, draw calls, batches, vertices, tile chunks, lights, texture uploads, shaped text runs |
| `cw_visibility_get_stats` | `cartwheel/visibility.h` | culling results and the visibility pass's CPU ms |
| `cw_particles_get_stats` | `cartwheel/particles.h` | particle systems, alive particles, simulation steps, draw calls, sort passes |
| `cw_world2d_stats`, `cw_world3d_stats` | `cartwheel/physics2d.h`, `physics3d.h` | bodies, contacts, joints, the step's own time |
| `cw_audio_get_stats` | `cartwheel/audio.h` | voices, mixed frames, underruns |
| `cw_res_get_stats` | `cartwheel/resource.h` | resources by state, payload bytes, cache hits, and read / decode / finish ms |

Times from these (`step_ms`, `cpu_ms`) are host-dependent: report them, never put them in a gate's compared `fields`. Counts (draw calls, alive particles, cluster references) are deterministic and make good gate fields.

## The perf reports

Each perf report is a scripted gate that prints a table, writes it under `out/perf/`, and checks only things that cannot drift: counts, that timings exist where they should, or that a budget holds. Each has a control: a report that timed nothing (or everything), or the same budget check against an impossible budget, must fail.

| Gate | Script or app | What it measures |
|---|---|---|
| `pipeline-perf` | `tests/pipeline-perf.mjs` | per-pass GPU ms of the full post stack at 1280 x 720 |
| `fx-perf-report` | `tests/fx-perf.mjs` | 100 000 GPU particles: frame work and GPU ms per host |
| `vis-perf-report` | `tests/vis-perf.mjs` | a 4500-mesh city culled vs unculled: draw calls, meshes, cull CPU, frame work |
| `showcase-perf` | `tests/showcase-perf.mjs` | the showcase scene at 1920 x 1080 through five time-of-day segments on every host, against a 16.7 ms budget |
| `perf-nodes`, `ui-perf`, `r2d-bench` | samples | node spawn/process/free, full and incremental UI relayout of about 5000 controls, 100k sprites in three submission variants; times are `perf.*` / `bench.*` debug fields, reported and never compared |

```sh
node tests/pipeline-perf.mjs --hosts native
```

```text
pipeline GPU ms per pass, post_stack_perf 1280x720 full quality, 240 frames (16-frame average)
pass           native
scene          0.161
ssao           0.245
bloom          0.139
...
total          0.735
ok   pipeline-perf (every running pass timed natively; idle passes untimed)
```

The frame measurement the reports share: a frame's cost is render end to render end with a 1-pixel readback after each frame, so the GPU work is drained and counted (a pipelined submission rate would look impossibly fast), averaged after settling frames, with the worst frame reported too. romdev's stepping is also timed from outside, since a deterministic host may virtualize clocks.

## Measuring a change: interleaved A/B

One run of A and one run of B tells you about the machine, not the change. The method the reports use (`tests/vis-perf.mjs` is the template):

1. Build both variants as separate apps (the visibility report's `vis_city` and `vis_city_off`).
2. Run them interleaved, A, B, A, B, for several rounds, so clock changes and heat land on both sides.
3. Drop the warm-up frames, take the median frame time per run, and report the median over rounds (or the median of per-round ratios).
4. Check a count that proves A and B differ in what they do (256 lights binned in A, none in B); a report that cannot tell the variants apart fails.
5. Note the machine's load and measure again when it is quiet before claiming a win. Measure both sides before naming a difference.

Profile the build you ship: `scripts/build.sh` defaults to Release. Sanitizer and Debug builds (`BUILD_TYPE=Debug`, `scripts/sanitize.sh`) are for correctness, not timing.
