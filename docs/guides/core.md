# Core: memory, containers and determinism

The core module is the layer every other module stands on: tagged allocation, per-frame scratch, pools addressed by generational handles, insertion-ordered maps, interned strings, a portable RNG and hash, deterministic math, and the one job pool the whole engine shares. None of it touches SDL, GL or the wasmcart ABI, so it behaves the same in a native build and in a cart.

The headers are [cartwheel/base.h](header:base.h), [cartwheel/core.h](header:core.h), [cartwheel/math.h](header:math.h) and [cartwheel/jobs.h](header:jobs.h). The unit suites `unit-core` and `unit-math` ([tests/unit/test_core.c](repo:tests/unit/test_core.c), [tests/unit/test_math.c](repo:tests/unit/test_math.c)) exercise all of it on every host.

## Logging and asserts

`cw_info`, `cw_warn` and `cw_error` go to stderr natively and to `wc_log` in a cart, where romdev stamps each line with its frame. Debug-level lines are dropped natively unless `CW_DEBUG` is set. A log listener ([cw_log_add_listener](api:cw_log_add_listener)) sees every line after the platform printed it; the editor's Output panel is one. Listeners run on the thread that logged, so they must be quick and thread-safe.

Two kinds of failure, two tools:

- A mistake a user can make (a missing file, a bad property name) is logged with the fix in the message, and the program carries on.
- A programmer error is `CW_ASSERT(cond, "what to do instead")`. It is compiled out under `NDEBUG`. `CW_FATAL` always aborts; in a cart it traps, which every host reports.

## Memory

### Tagged allocation

Long-lived memory goes through [cw_alloc](api:cw_alloc) and [cw_free](api:cw_free) with a [cw_mem_tag](api:cw_mem_tag) (`CW_MEM_SCENE`, `CW_MEM_PHYSICS`, `CW_MEM_GAME`, ...). The tag costs nothing at the call site and buys a per-subsystem byte count that the profiler, the editor and the debug channel all read. Game code uses `CW_MEM_GAME`. The `stats` debug command prints the counters; this is `hello` after 30 frames:

```sh
SDL_VIDEODRIVER=offscreen CW_FRAMES=30 CW_DEBUG_CMDS="stats" build/native/samples/hello
```

```text
{"frame":30,"tick":30,"time":0.483333,"threads":24,"jobs":0,"chunks":0,"mem":{"general":0,"core":390,"scene":0,"render":0,"render2d":0,"physics":0,"audio":0,"anim":0,"ui":0,"resource":0,"script":0,"game":0,"editor":0},"mem_total":390,"mem_peak":3125}
```

`hello` draws with three.c directly, so almost nothing is engine-owned; a scene app shows real numbers under `scene`, `render` and `resource`.

### Frame scratch

[cw_frame_alloc](api:cw_frame_alloc), `cw_frame_calloc` and [cw_frame_printf](api:cw_frame_printf) bump-allocate from the calling thread's frame arena. There is no free: the arena resets lazily the first time that thread allocates in a new frame. Use it for anything that dies within the frame: formatted label text (`games/action2d` sets its score label with `cw_frame_printf("SCORE %d", ...)` every frame), a temporary sort buffer, a path built for one load.

Every thread has its own arena (1 MiB blocks, chained as needed), so a job can allocate scratch without locking. Never keep a frame pointer past the frame, and never hand one to an API that stores it (resource paths are copied, so passing scratch there is fine; `games/action2d` loads its sounds that way).

Arenas you own ([cw_arena](api:cw_arena)) work the same way with a reset you control: a level-load arena, a per-room arena.

### Pools and generational handles

Objects a user holds are entries in a [cw_pool](api:cw_pool): fixed-size elements in chunks (addresses are stable), addressed by a 64-bit handle that packs `{index, generation}`. Freeing an element bumps its slot's generation, so an old handle reads as `NULL` from [cw_pool_get](api:cw_pool_get) instead of silently pointing at whatever reused the slot. Handle 0 is never issued.

Public APIs never return raw pointers to pooled objects. Each kind of handle is its own struct type (`CW_HANDLE_TYPE`), so a node handle does not compile where a body handle is expected. Nodes (`cw_node`), resources and physics bodies are all handles of this shape. `CW_HANDLE_IS_NULL` is the null check; a stale handle is not null, it simply resolves to nothing. A live handle always has a nonzero generation, so a handle cut to 32 bits (kept in a `uint32_t`) is recognisable: `CW_HANDLE_IS_TRUNCATED` tests for it and [cw_pool_get](api:cw_pool_get) logs an error naming the fix (keep the handle struct, or use the module's slot accessor) instead of answering as for a stale handle.

Iteration is by slot (`cw_pool_at`), which is deterministic for a deterministic sequence of allocations and frees.

### Arrays and maps

`CW_ARRAY(T)` is a typed growable array (`cw_array_push`, `cw_array_remove_swap`, `cw_array_free`). [cw_map](api:cw_map) is an open-addressing `u64 -> u64` map. String keys go through `cw_hash_str` or are interned pointers. Iteration is in insertion order, and removal keeps the order of the rest: walk `keys[i]` and `vals[i]` for `i < count`, skipping entries where `cw_map_live` is false. `cw_map_size` is the live count.

## Strings, hashing, random numbers

- [cw_intern](api:cw_intern) returns one pointer per distinct string, alive for the process. Interned strings compare with `==`; node type names and string property values are interned inside the engine.
- [cw_hash_bytes](api:cw_hash_bytes), `cw_hash_str` and the FNV-1a `cw_hash32` are stable across hosts and never pointer-based, so a hash of game state can be compared between a native run and a cart run (the gates do exactly that).
- [cw_rng](api:cw_rng) is xoshiro128**: 16 bytes of state, identical sequences on every host. Seed it explicitly ([cw_rng_seed](api:cw_rng_seed)); a host may deliver a seed (`cw_host_seed`, `CW_SEED` natively). Keep one generator per purpose (level generation, loot, AI) so adding a roll in one does not shift the others.

`fx_decals` scatters 240 decals from one seeded generator, which is why every host renders the same field:

@include samples/fx_decals/main.c#rng_spawn

## Deterministic math

[cartwheel/math.h](header:math.h) holds `cw_vec2` helpers, scalar helpers (`cw_lerp`, `cw_move_toward`, `cw_smoothstep`), easing curves and transcendentals: [cw_sin](api:cw_sin), `cw_cos`, `cw_sincos`, [cw_atan2](api:cw_atan2), `cw_acos`, `cw_asin`, `cw_exp`, `cw_ln`, `cw_pow`, `cw_wrap_angle`. 3D math is three.c's (`t3_vec3`, `t3_quat`, `t3_mat4`).

The reason the transcendentals exist: libm's `sinf`, `atan2f`, `expf` and `powf` give different last bits on glibc, wasi-libc (musl), Apple and MSVC. The `cw_` versions use only `+ - * /` and `sqrt`, which IEEE 754 makes exact everywhere given `-ffp-contract=off` (set engine-wide). They are a few ulp from the true value, which games never notice, and bit-identical on every host, which replays and lockstep netplay need. The natural log is `cw_ln` because `cw_log` is logging.

The rule is simple: anything that feeds game state uses `cw_` math; rendering may use libm, because pixels are compared with a tolerance and state is not.

## The job pool

The engine has one thread pool ([cartwheel/jobs.h](header:jobs.h)). Box2D, Box3D, culling, animation, transforms, particles and asset decoding all run on it; nothing else spawns threads. Natively it uses pthreads, in a cart wasi-threads, with the machine's hardware threads - 1 workers by default (a cart learns the count from its host, `wc_host_info_t.hardware_threads`; a host too old to say gets 3). `cw_app_desc.workers` sets the count (0 = auto, below 0 = none) and `CW_WORKERS` overrides it natively.

Two job shapes:

- range jobs, `fn(begin, end, worker, ctx)` over `[0, count)` in chunks of at least `min_batch` items;
- single jobs, `fn(ctx)`, run once.

[cw_parallel_for](api:cw_parallel_for) is a range job plus a wait. The waiting thread never parks while work is queued: it runs pending chunks (its own first) until its job is done, so a job that waits on its own sub-jobs cannot deadlock, and the main thread never blocks on a condition variable (a browser main thread could not).

`r2d_bench` animates 100,000 sprites this way. The worker function is a pure function of the item index and the tick, writing only its own slots:

@include samples/r2d_bench/main.c#animate

and the call site is one line in `render`:

@include samples/r2d_bench/main.c#dispatch

Choosing `min_batch`: the pool aims for about four chunks per thread, never smaller than `min_batch`. Set it so a chunk is at least a few microseconds of work (thousands of cheap items, tens of expensive ones); below that the claim overhead dominates. `cw_jobs_get_stats` (and `stats` above) report jobs, chunks and chunks per thread.

## Determinism rules

Simulation code (anything that feeds game state) follows these, and the gates prove it by comparing named state between native and cart runs:

1. Randomness from `cw_rng` only, seeded explicitly.
2. Time from the fixed-step `dt` only; never read a clock (`cw_time` is for display).
3. Transcendentals from `cartwheel/math.h`, never libm.
4. Iterate in insertion order. `cw_map` and `cw_pool` already do; never iterate by pointer value or hash order.
5. Parallel work writes per-item results. Chunk boundaries depend on the worker count, which differs between hosts (and `CW_WORKERS`), so a reduction must not depend on how items were chunked: write per-item values and reduce them in index order on one thread.
6. `-ffp-contract=off` is global; never add `-ffast-math`.

Physics follows the same rules internally (see [Physics](guide:physics)); `physics-determinism` replays upstream Box2D and Box3D scenes with 1 to 5 workers and requires upstream's hashes on every host.

## Related

- [App and platform](guide:app-platform): the frame loop that advances the frame arena and runs fixed ticks.
- [Debug channel](guide:debug-channel): `stats` and named debug state.
