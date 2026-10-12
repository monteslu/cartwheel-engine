# Profiling

What {{name}} measures today and how to measure a change without fooling yourself. There is no profiler window yet: the numbers come from the debug channel, from stats functions in the headers, and from the perf reports in `tests/`.

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

Natively every render pass is wrapped in a `GL_TIME_ELAPSED` query (`EXT_disjoint_timer_query`). Results arrive 1 to 3 frames late; `cw_pipeline_pass_stats` gives each pass's latest time and a 1/16 exponential average, and `cw_pipeline_gpu_ms` the total of the passes that ran. A pipeline rendered from inside another pass is not timed separately (GL has one timer query open at a time): the enclosing pass's time includes it.

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
