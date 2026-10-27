# Architecture

How the engine is put together: the layers, who owns the frame, how objects are held, how work is spread over threads without losing determinism, and how a frame becomes pixels. Everything here can be checked in `include/cartwheel/` and `src/`.

## Layers

| Layer | Where | What it owns |
|---|---|---|
| Platform | `src/platform/` (`cartwheel/platform.h`) | the loop, window and GL context, raw input, time, audio output, assets. The only code that touches SDL or the wasmcart ABI. |
| Core | `src/core/`, `src/jobs/`, `src/debug/` (`core.h`, `jobs.h`, `math.h`, `debug.h`, `vfs.h`) | tagged memory, arenas, pools, maps, strings, hashing, RNG, the job pool, named debug state and the debug channel, the project file layer |
| Modules | `src/<module>/` | input, physics, audio, resources, render (3D pipeline, lighting, environment, lightmaps), render2d, fx, visibility, animation, UI, networking, l10n, remote |
| Engine model | `src/scene/` (`scene.h`, `scene_file.h`, `scene2d.h`, `physics_nodes.h`, `triggers.h`, `project.h`) | nodes, the tree, reflection, signals, methods, scene files, the scene runner, node types for every module |
| Tools | `tools/` | packing assets, building projects, exporting, hot reload, baking lightmaps, generating the API data and this site |

A game uses the public headers only. The [header list](guide:overview) has one line per header.

## Who owns the frame

A game provides `cw_main`, which returns a `cw_app_desc` (title, logical size, `fixed_hz`, `max_substeps`, flags, callbacks) and is called back. Natively `cw_main` runs from `main()` and the platform loops until quit; in a cart it runs from `wc_init()` and every `wc_render()` from the host is one frame. Both paths call the same frame function (`src/platform/sdl_common.c`):

0. In the first frame, the app's `init` (startup itself stays light). An app with a warm-up and a spread warm-up draws only the loading screen in that frame and runs `init` in the next ([app platform](guide:app-platform)).
1. Clamp the frame delta to 0.25 s (a stall is not simulated), reset the frame arena, finish async resource loads under the upload budget.
2. Answer the debug channel. If the debugger's pause is on and no step is owed, poll events, render and stop here.
3. Poll SDL events into the raw input snapshot (and the input filter, `cw_set_raw_input_filter`).
4. Fixed steps: while the accumulator holds a tick and fewer than `max_substeps` (default 5) ran, take the per-tick action snapshot and call `fixed_update(dt)` with the fixed `dt` (default 1/60 s). A backlog beyond the cap is dropped instead of spiralling.
5. `update(dt)` once, with the frame's delta.
6. `render(alpha)`, where alpha is the accumulator over the fixed step, for interpolation.

`cw_tick_index` counts fixed ticks and `cw_frame_index` rendered frames. Simulation belongs in `fixed_update`: it is the part that runs the same number of times with the same `dt` on every host, at any display rate.

`cw_frame_dt()` is the seconds from the previous rendered frame to this one, for a `render` callback that animates something by wall time (a scrolling backdrop, a particle trail, a fade) without differencing `cw_time()` by hand. It is the same value on every host (a frame that lasted one fixed step reads exactly `cw_fixed_dt()`, a stall is clamped to a quarter second). It is render-only: nothing the simulation reads may depend on it.

Apps built on the node model use `cw_scene_app(main_scene, &base)`, which supplies those callbacks: `fixed_update` runs the tree's fixed phase (module hooks, then nodes in tree order) and then `base.fixed_update`; `update` runs the tree's update phase; `render` runs `pre_render` and the tree's render pipeline, with `base.render` as its last overlay pass (a HUD drawn there lands on top).

@include samples/hot_demo/main.c#main

## Memory, handles and ownership

- Long-lived memory goes through `cw_alloc` / `cw_free` with a `cw_mem_tag`, so bytes per subsystem are always known (`cw_mem_get_stats`, the `stats` debug command).
- Per-frame scratch is `cw_frame_alloc`: a per-thread bump arena, reset every frame, never freed by hand.
- Anything a user holds is a `cw_pool` entry addressed by a generational handle: 64 bits of index and generation, typed per kind (`cw_node`, `cw_res`, ...). A stale handle reads as null, never as another object. 0 is the null handle.
- Maps (`cw_map`) and pools iterate in insertion order, so walking a container is deterministic.
- Headers say who owns each returned pointer. Resources are reference counted: every load returns a new reference, and the last release queues the resource for a few frames before freeing it (the GPU may still be drawing with it).

## The job pool and determinism

There is one thread pool (`cartwheel/jobs.h`). Box2D, Box3D, culling, animation, transforms, particles and asset decoding all use it; nothing spawns threads of its own. Natively it runs on pthreads, in a cart on wasi-threads; either way the workers default to the machine's hardware threads - 1 (a cart asks its host, `wc_host_info_t.hardware_threads`; a host too old to say gets 3). `CW_WORKERS` overrides it natively.

- `cw_parallel_for(count, min_batch, fn, ctx)` splits `[0, count)` into chunks and waits. A waiting thread runs other chunks instead of sleeping, and the main thread never blocks on a condition variable (a browser's main thread cannot).
- Chunk boundaries depend only on the count, the batch size and the worker count, never on timing. A job that writes per-chunk results and reduces them in chunk order gives the same answer every run.

Simulation code (anything that feeds game state) follows the rules in [Working on {{name}}](guide:conventions): randomness from `cw_rng` seeded explicitly (each tree has one, `cw_tree_rng`), time from the fixed `dt` only, trig and exp from `cartwheel/math.h` (libm differs between platforms), insertion-ordered iteration, ordered reductions, and `-ffp-contract=off` everywhere with no fast-math. With those rules the same input stream gives the same state on native x86_64 and on wasm32, which is what every gate checks.

## Nodes and the tree

A node is a named element of a tree with a type. A type (`cw_node_type`, registered once with `cw_node_type_register`) declares its parent type, the size of its own data, its properties, its signals and its lifecycle callbacks (`init`, `enter_tree`, `ready`, `fixed_update`, `update`, `pre_render`, `exit_tree`, `destroy`, plus `prop_changed` and `children_changed`). Each type in the chain keeps its own data block (`cw_node_data(n, "Node3D")`); parent callbacks run first on the way in and last on the way out.

@include samples/hot_demo/main.c#node_type

- **Processing order** is tree order, depth first. Adding or freeing nodes while a phase runs is queued and applied at the end of the phase.
- **Reflection.** Every property is reachable by name with typed values (`cw_node_set`, `cw_node_get_prop`, `cw_value`), and every value has one text form (`vec3(1, 2, 3)`, `color(1, 0, 0, 1)`, an enum name). That single path serves scene files, animation tracks, tweens, triggers, the debug channel and networking.
- **Signals** (`cw_connect`, `cw_emit`): immediate, deferred (run at the end of the phase in emission order) or one-shot. Physics events always arrive deferred, in the libraries' deterministic order.
- **Methods** (`cw_node_type_add_method`): named C functions with a typed signature, callable by name from the debug channel, triggers and scene connections.
- **Groups** (`cw_node_add_group`, `cw_tree_group`) return nodes in tree order.
- **Pause**: the tree's own pause flag (a pause menu) with per-node pause modes, separate from the debugger's freeze.
- **Per-tree state**: modules keep their per-tree data (physics worlds, the render scene, the pipeline) in tree slots (`cw_tree_slot`) and run per-phase work as tree hooks (`cw_tree_add_hook`; physics steps before nodes see the results).

Scenes are `.cwscene` text: one property per line, only values that differ from the defaults (or from an instanced scene), nodes in tree order, a load-then-save round trip byte for byte. Instancing keeps a link to the source so only overrides are saved. See [Scenes and nodes](guide:scenes).

## Resources

A resource (`cartwheel/resource.h`) is shared, immutable data: textures, meshes, materials, glTF scenes, audio clips, fonts, text and JSON, plus types modules register. It has a handle, a path (`models/duck.glb`, sub-resources as `models/duck.glb#mesh/0/1`) and a stable 64-bit UID. `cw_res_load` is synchronous; `cw_res_load_async` decodes on the job pool and finishes on the main thread under a per-frame upload budget, always in request order, so the frame a resource becomes ready is the same on every host. Files are read through the project layer (`cartwheel/vfs.h`) first, then the packed assets. See [Resources](guide:resources).

## The render pipeline

Each tree that draws 3D owns a pipeline (`cartwheel/pipeline.h`), a small frame graph of passes run phase by phase and, within a phase, by `order`:

| Phase | Built-in and module passes |
|---|---|
| `SHADOW` | `sky_env` (the sky cube), `visibility` (culling, LOD, impostor bakes) |
| `PREPASS` | `depth_prepass` |
| `OPAQUE` | `scene` (three.c renders the tree's scene), `impostors`, `occlusion_queries` |
| `SKY` | sky and atmosphere |
| `EFFECTS` | `decals`, `particles`, fog volumes |
| `POST` | `velocity`, `ssao`, `taa`, `bloom`, `tonemap`, `grade`, `aa`, `lens`, `debug_view` |
| `OVERLAY` | `canvas` (the tree's 2D and UI), `app` (the app's render callback), debug draw at 1000 |
| `PRESENT` | readbacks, captures, `visibility_restore` |

A module adds a pass with `cw_pipeline_add_pass` (phase, order, the resources it reads and writes, an `active` switch and an `execute`). Every tree renders as three.js r186 does (linear HDR, physical light units, AgX); a `PostProcess` node holds the settings. The `pipeline` debug command lists the passes that ran and, natively, their GPU time. See [3D rendering](guide:rendering-3d) and [2D rendering](guide:rendering-2d).

GL is single-threaded: one context, used from the main thread. Everything before submission (culling, animation, particles' CPU work) is parallel.

## Native and cart: what differs

Nothing, for game code. The same source builds both, and the differences live in `src/platform/native.c` and `src/platform/cart.c`:

| | Native | Cart |
|---|---|---|
| Loop | the platform loops | the host calls `wc_render()` once per frame |
| Clock | SDL's performance counter (`CW_FRAMES` runs use a fixed virtual clock) | the host's clock (fixed in deterministic runs) |
| Threads | pthreads | wasi-threads (shared memory) |
| Audio | SDL's audio thread calls the fill callback | a job-pool thread calls it while the frame renders; `wc_render` hands the frames to the host at its end |
| Assets | the `.cwpak` beside the executable, then loose files | the cart's bundle (lazily over HTTP on the web) |
| Writing files | a project directory on disk | an in-memory layer, carried in and out as one archive (`vfs export` / `vfs import`) |
| Debug channel | `CW_DEBUG_CMDS`, or a socket (`CW_REMOTE`) | mailbox fields in the wasmcart debug state |
| GPU pass timing | timer queries, when asked for (`CW_GPU_TIMERS=1`, the `pipeline` command) | none (`gpu_ms` reads -1) |

`CW_CART` is 1 in a cart build for the rare engine code that needs to know.
