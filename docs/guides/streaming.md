# Big open worlds

Header: [cartwheel/stream.h](header:stream.h), with terrain sets in [cartwheel/terrain.h](header:terrain.h) and the loading pipeline in [cartwheel/resource.h](header:resource.h). Sample: `open_world` (a 16.4 km square world you drive or fly across). Gates: `unit-stream`, `open-world-drive`, `open-world-fly`, `stream-perf`.

A world bigger than memory is cut into square cells. Cells load, appear, become solid and go away again around the player while the game runs: there is no loading screen and no frame waits for the disk.

## Cells

[cw_stream_create](api:cw_stream_create) takes a grid (`cells_x` x `cells_z` squares of `cell_size` metres from `origin`) and a `path_format` that names each cell's file. One file per cell keeps a lazily loaded web cart from waiting on more than one cell at a time; compress them (a `.zst` or `.lz4` suffix, [Resources](guide:resources)) and they decompress on the job pool.

A cell goes through four states:

| State | What exists | Who does it |
|---|---|---|
| `NONE` | nothing | |
| `LOADING` | its file is being read and decoded in the background | [cw_stream_tick](api:cw_stream_tick) asks for it, nearest first |
| `LOADED` | what is only drawn: terrain, meshes, scatter | `on_load`, from [cw_stream_frame](api:cw_stream_frame), a few a frame; with `cell_ready`, once its background work is done too |
| `ACTIVE` | what the simulation sees: bodies, navmesh tiles, entities | `on_activate`, from [cw_stream_tick](api:cw_stream_tick) |

and back: `on_deactivate`, then `on_unload`, then the file is released (a residency budget can keep it cached, [cw_res_set_residency_budget](api:cw_res_set_residency_budget)).

`open_world` registers a resource type for its cells, so the parsing happens on a worker:

@include samples/open_world/main.c#cell_type

and opens the stream:

@include samples/open_world/main.c#stream

## Sources, radii and hysteresis

Sources ([cw_stream_source_add](api:cw_stream_source_add)) say where cells are needed: the player, the camera, other players. A cell is loaded while its square is within `load_radius` of a source and active within `active_radius`. A cell leaves a set only once it is `hysteresis` metres past the radius, so a player standing on a border does not make cells load and unload every tick.

`load_radius - active_radius` is how far the player can travel while a cell loads. Size it from your load times and your fastest speed: `open_world` measures [the numbers below](#how-fast).

## Deterministic activation

What the simulation sees changes only in [cw_stream_tick](api:cw_stream_tick), called from `fixed_update`, and only from the sources' positions. Cells activate and deactivate in cell id order on the tick their distance says, whatever the disk, the network or the job pool did. If a cell must become active and its file has not arrived, the tick waits for it and counts a **stall** ([cw_stream_stats](api:cw_stream_stats)): the frame takes longer, but the game stays the same on every host. A healthy setup has none; the start position's cells, loaded on the first tick, are counted apart (`start_cells`).

`activation_hash` folds every activation and deactivation (tick, cell, on/off). Equal hashes on two hosts mean equal histories; `unit-stream` checks it across hosts and with slow, reordered decodes.

## Terrain that streams

One height grid for a 16 km world at 2 m would be 67 million samples. A terrain set ([cw_terrain_set_create](api:cw_terrain_set_create)) holds one small terrain per loaded cell instead. Every cell terrain shares one template's layer textures ([cw_terrain_share_layers](api:cw_terrain_share_layers)), gets the same scatter layers with its own seed, builds its height fields when it is added and creates its bodies only while active. Neighbouring cells repeat their shared edge samples, and with equal definitions their detail levels line up, so cells meet without cracks. The set draws all of them from one pipeline pass.

@include samples/open_world/main.c#terrain_set

## Far from the origin

A float has 24 bits: 1 mm steps at 8 km, 8 mm at 100 km. Physics jitters and the camera shakes long before the edge of a big world. The stream keeps the world origin in double precision (`cw_dvec3`) and a local origin that everything simulated and drawn is relative to. When the first source gets `rebase_distance` from the local origin, the next tick moves the local origin by whole cells toward it and calls `on_rebase` with the shift; `open_world` moves its bodies with [cw_world3d_shift_origin](api:cw_world3d_shift_origin), and each cell gets `on_cell_rebase` with its new local position. The shift happens on a tick the sources decide, so it is deterministic, and the camera is always near the local origin, which makes the rendering camera-relative with nothing else to do. A power-of-two `cell_size` keeps every shift exact. [cw_stream_to_local](api:cw_stream_to_local) and [cw_stream_to_world](api:cw_stream_to_world) convert positions.

Box3D has no origin shift of its own, so [cw_world3d_shift_origin](api:cw_world3d_shift_origin) sets each body's transform (O(bodies), velocities, sleep and interpolation kept). Joint anchors fixed to the world do not move.

## How fast

Measured by `tests/stream-perf.mjs` on a Radeon 890M at 1920 x 1080, High preset. The flight crosses all 16.4 km at 150 m/s (about 350 cell loads, 22 origin moves); the drive covers 14 km of road at up to 25 m/s (10 minutes of game time). Native runs are player-timed (loads land when they are decoded, as in a game); romdev and wasmcart-native run the cart.

| | native | romdev cart | wasmcart-native cart |
|---|---|---|---|
| stalls, flight / drive | 0 / 0 | 0 / 0 | 0 / 0 |
| main thread per frame, flight: median, p99, worst | 0.9, 1.9, 3.1 ms | 1.2, 4.1, 29 ms | 2.6, 9.4, 148 ms |
| frames over 17.7 ms, drive | 1 (worst 19.6 ms) | 0 (worst 6.3 ms) | 2 (the first frames after the start) |
| memory over the drive | 311 MB at 3 km, 403 to 413 MB from 10 km on | 226 MB at 3 km, 320 MB from 10 km on | 226 MB at 3 km, 303 MB from 10 km on |

Memory rises while the caches and the allocator's high-water mark fill, then stays flat (at most 3 % over the last third of either run); the engine's own tracked bytes stay between 60 and 140 MB throughout. A cart's figure is its linear memory, which never shrinks.

The stream's own main-thread work stays near a millisecond while a cell loads every few frames. The flight's frames over budget natively (and a few in the carts) come from drawing the mountains at the far end at 1080p: hovering there with no streaming at all shows the same spikes (180 ms worst), so they are the scene's cost, not the stream's.

GPU time per frame and the load radius by preset (the flight, native GL, GPU timer queries on):

| Preset | load radius | cells loaded | GPU median | GPU p99 | main thread p99 |
|---|---|---|---|---|---|
| Low | 1200 m | 32 | 1.05 ms | 1.40 ms | 1.40 ms |
| Medium | 1500 m | 42 | 1.50 ms | 1.80 ms | 1.70 ms |
| High | 1800 m | 58 | 2.70 ms | 3.45 ms | 1.95 ms |
| Ultra | 2200 m | 80 | 3.35 ms | 3.95 ms | 2.30 ms |

The load radius is visual only, so hosts on different presets still activate the same cells; the active radius stays 600 m. A Vega 8 has roughly a third of the 890M's shader throughput: expect about 3 to 4 ms of GPU at Low, 5 ms at Medium and 8 to 11 ms at High.

What keeps frames flat:

- every file is read, decompressed and parsed on the job pool, as background jobs the main thread never picks up while it waits for other work ([cw_job_background](api:cw_job_background));
- a new cell's terrain builds its height fields, texels and quadtree on a background job and draws once that is done (`cell_ready` holds the cell until then);
- `on_load` runs for one cell a frame (`prepare_per_frame`), about a millisecond;
- scatter is placed on the job pool without the frame waiting for it;
- moving the origin shifts what exists (bodies, terrain, scatter) instead of rebuilding it.

`tests/stream-perf.mjs` fails on any stall or on memory still growing over the last third; its controls (a 620 m load radius must stall, a cache that never evicts must grow) prove both checks can fail.
