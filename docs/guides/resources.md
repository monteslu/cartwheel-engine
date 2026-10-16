# Resources and the asset pipeline

Resources are the shared data a game loads once: textures, glTF scenes and their meshes and materials, audio clips, fonts, JSON and text. The `cwpack` tool imports source assets ahead of time (KTX2 textures, meshopt-compressed meshes with LODs, re-encoded audio) and packs only what the game can reach.

Header: [cartwheel/resource.h](header:resource.h). Tool: `tools/cwpack` (source comment in [tools/cwpack/main.c](repo:tools/cwpack/main.c)). Sample: `gltf_scene` (async loads, KTX2, LODs, hot reload). Gates: `unit-resource`, `unit-hotreload`, `cwpack`, `gltf-scene`.

## Three names for one resource

- A **handle** (`cw_res`) is what code holds. It is generational: a handle to a freed resource reads as `CW_RES_STATE_NONE`, never as another resource.
- A **path**, `res://models/duck.glb` or just `models/duck.glb`, relative to the asset root. A `#` addresses a sub-resource of a loaded file: `models/duck.glb#mesh/0/1` (mesh 0, primitive 1), `#material/2`, `#texture/0`.
- A **UID**, a stable 64-bit id that survives renames. It comes from the file's `.cwimport` sidecar (`uid = 0x...`), else from the FNV-1a hash of its path ([cw_res_path_uid](api:cw_res_path_uid)). Scene files can reference resources by UID (`res(0x...)` values), so moving a file does not break the scenes that use it. `cwpack` writes a `res.index` of uid, type and path into every pack, and [cw_res_load_uid](api:cw_res_load_uid) resolves through it.

Reads go through the project layer first ([App and platform](guide:app-platform)), so a file the editor or a tool just wrote loads without a repack.

## Types

The built-in types are fixed ids, inferred from the extension when you pass 0 (`CW_RES_TYPE_AUTO`):

| Type | Extensions | Payload |
|---|---|---|
| texture | `.png .jpg .ktx2` | `t3_texture` ([cw_res_texture](api:cw_res_texture)) |
| gltf | `.glb .gltf` | [cw_gltf](api:cw_gltf), with mesh, material and texture sub-resources |
| audio clip | `.wav .ogg .opus .mp3 .flac` and tracker modules | [cw_audio_clip](api:cw_audio_clip) |
| font, json, text, binary | `.ttf .otf`, `.json`, `.txt .cwscene ...`, anything else | [cw_blob](api:cw_blob) |

Modules register more with [cw_res_register_type](api:cw_res_register_type): a `prepare` step on the main thread (ask for dependencies or extra files), a worker-safe `decode` (bytes to an intermediate; no GL, no three.c objects), and a main-thread `finish` that creates objects and uploads. Optional `reload` moves fresh data into the existing payload in place so pointers others hold stay valid.

## Ownership and lifetime

Every load returns a new reference; [cw_res_release](api:cw_res_release) gives it back. Loading a path that is already loaded (or still loading) returns the same resource with one more reference, so ten nodes using one texture share one upload. When the last reference goes the resource is not freed at once: it waits `CW_RES_FREE_DELAY_FRAMES` (2) frame boundaries, because the GPU may still be drawing with it, and a load of the same path in that window revives it. Unloading a level and loading the next one that shares assets therefore costs nothing for the shared part.

Payload pointers (`cw_res_texture`, `cw_res_gltf`, ...) are owned by the resource and valid while you hold a reference. A glTF instance ([cw_gltf_instantiate](api:cw_gltf_instantiate)) is your three.c group; it holds its own reference to the scene resource until three.c destroys it.

## Loading: sync, async and groups

[cw_res_load](api:cw_res_load) returns with the resource ready or failed; a missing file fails with the asset root named in the log. Use it at startup or behind a loading screen.

[cw_res_load_async](api:cw_res_load_async) loads in the background and finishes on the main thread at the start of a later frame, under a per-frame upload budget (32 MB and 4 ms by default, [cw_res_set_upload_budget](api:cw_res_set_upload_budget)):

- **Read.** Natively the file is read on the job pool together with its decode, so a request costs the main thread only bookkeeping. In a cart the host's asset calls belong to the main thread (a lazily loaded web cart waits there while the bytes arrive), so each frame reads a few queued files ([cw_res_set_read_budget](api:cw_res_set_read_budget): 4 files and 2 ms by default). Files of the project layer and glTF files, whose prepare step finds their dependencies, are read when requested.
- **Decode** runs on the job pool (image decode, KTX2 transcode, meshopt decode, decompression), at most [cw_res_set_max_in_flight](api:cw_res_set_max_in_flight) at once, started in finish order.
- **Finish** happens in a fixed order: by lane (lower first), then priority (higher first), then request order. Plain `cw_res_load_async` is lane 0, priority 0, so loads finish in request order; [cw_res_load_async_ex](api:cw_res_load_async_ex) sets both, so a nearby cell asked for late does not wait behind far ones. A resource whose decode ended early waits for the ones before it. The order is a pure function of the requests, which keeps the frame a resource becomes ready, and every id three.c hands out, the same on every host. Take priorities from game state (distance in fixed-tick positions), never from timing. In a deterministic run the budget counts bytes only, so readiness never depends on thread timing.

Releasing the last reference to a load in flight cancels it without waiting: work not started is dropped and a decode already running finishes on its worker, its output thrown away when it lands. With [cw_res_set_residency_budget](api:cw_res_set_residency_budget), released resources stay loaded in a cache (least recently released evicted first) while their bytes fit, so walking back to where you were is a cache hit. [cw_res_stats](api:cw_res_stats) reports what waits, runs, is cached and was evicted.


`gltf_scene` requests four files in `init`:

@include samples/gltf_scene/main.c#async_loads

with a completion callback (run on the main thread from the frame boundary, never from inside the load call):

@include samples/gltf_scene/main.c#on_loaded

At a fixed tick it calls [cw_res_wait_all](api:cw_res_wait_all), the end of a loading screen, and builds the scene, so every host draws the same frames from then on.

A [cw_res_group](api:cw_res_group) holds one reference to each load added to it, reports progress (`fraction` for a loading bar, bytes ready) and releases everything when freed. [cw_res_preload](api:cw_res_preload) fills a group from the `<scene>.preload` list `cwpack` writes beside every packed `.cwscene`: everything the scene and its sub-scenes reference, in order.

## Seeing what loaded

The `res` debug command returns every resource with its state, references, UID, sizes and timings; [cw_res_log_report](api:cw_res_log_report) logs the same. From `gltf_scene` after 20 frames (times are wall clock and vary by machine; `ktx2_target` is what this GPU transcodes to):

```sh
SDL_VIDEODRIVER=offscreen CW_FRAMES=20 CW_DEBUG_CMDS="res" build/native/samples/gltf_scene
```

```text
[info] resources: 16 ready, 6978 KB packed read, 56149 KB payload, 82 KB CPU (resource tag); read 7.7 ms, decode 392.1 ms (workers), finish 20.1 ms (main thread)
[info]   res://models/CesiumMan.glb  gltf  336 KB -> 5712 KB  read 2.6  decode 31.8  finish 3.9  ready after 475.0 ms / 1 frames
[info]   res://models/WaterBottle.glb  gltf  6612 KB -> 22029 KB  read 4.9  decode 344.9  finish 12.6  ready after 369.8 ms / 1 frames
{"live":16,"ready":16,"loading":0,"failed":0,"pending_free":0,"bytes":57496702,...,"ktx2_target":"bc7","resources":[{"path":"models/CesiumMan.glb","type":"gltf","state":"ready","refs":6,"uid":"34984dcb44c19b27",...
```

Four files became sixteen resources: each glTF registers its meshes, materials and textures as sub-resources.

## The pipeline: cwpack

A game's assets start as source files (PNG, glTF, WAV, TTF, scenes). `cwpack` imports them into runtime formats and packs them. The build runs it for an app declared with `cw_add_app(name ... PACK path/to/roots.txt)`; by hand:

```sh
build/native/tools/cwpack/cwpack --manifest samples/gltf_scene/project/roots.txt --out out/gltf_pack --pak out/gltf_scene.cwpak
```

```text
  cached  res://models/CesiumMan.glb (gltf, 438044 -> 344520 bytes)
  copy    res://models/CesiumMan.LICENSE.md (text, 858 -> 858 bytes)
  cached  res://models/BoxTextured.glb (gltf, 5956 -> 6212 bytes)
  copy    res://models/BoxTextured.LICENSE.md (text, 860 -> 860 bytes)
  cached  res://models/WaterBottle.glb (gltf, 8966700 -> 6771672 bytes)
  copy    res://models/WaterBottle.LICENSE.md (text, 774 -> 774 bytes)
  cached  res://textures/ground.png (texture, 29742 -> 23132 bytes)
cwpack: out/gltf_scene.cwpak: 8 entries, 7093017 bytes
cwpack: 8 files, 7149078 bytes (0 imported, 4 from cache)
```

### roots.txt and references

The manifest lists **roots**, and `cwpack` packs only what they reach: a glTF's external images, a scene's `[ext_scene]` sub-scenes and every `res(0x<uid>)` value in it, a localization project's catalogs and fonts. A main scene alone can be the whole manifest. Lines are `set <key> <value>` (a project-wide import setting), `<res path>` (a file beside roots.txt) or `<res path> = <source path>` (a source elsewhere, relative to roots.txt, or to the engine checkout when it starts with `@engine/`, as in `fonts/NotoSansJP.ttf = @engine/deps/fonts/noto-sans-jp.ttf`):

@include samples/gltf_scene/project/roots.txt

Per-file settings live in a sidecar, `<file>.cwimport`, with `key = value` lines (and the file's `uid`). `gltf_scene`'s ground texture becomes KTX2 with ETC1S compression:

@include samples/gltf_scene/project/textures/ground.png.cwimport

Settings the importers read: `texture.format` (`ktx2` or the source format), `texture.mode` (`etc1s` small, `uastc` high quality), `texture.quality`, `texture.effort`, `texture.mips`, `texture.color`; `mesh.optimize`, `mesh.compress` (EXT_meshopt_compression), `mesh.lods` (target ratios, e.g. `0.5 0.2`), `mesh.lod_error`; `audio.format`, `audio.bitrate`, `audio.complexity`; `font.subset`, `font.subset_chars`; `pack.compress` (pack entries: `zstd`, the default, `lz4`, `deflate` or `none`), `pack.level` (zstd 1..22, default 12; lz4 1..12, default 9).

### What the formats buy

- **KTX2** (Basis Universal) textures are small on disk and transcode at load, on a worker, to the best compressed format the context has: ASTC 4x4, BC7, ETC2, BC3, else RGBA8. Compressed textures stay compressed on the GPU, typically a quarter of RGBA8's memory or less. [cw_res_texture_info](api:cw_res_texture_info) reports the result.
- **meshopt** reorders vertices and indices for the GPU cache and compresses the buffers; decoding runs on the job pool. `mesh.lods` adds simplified index buffers that share the full mesh's vertices ([cw_mesh](api:cw_mesh)); [cw_gltf_instance_auto_lod](api:cw_gltf_instance_auto_lod) picks the coarsest level whose error stays under a pixel budget on screen.

### Compressed files

A file whose name ends in `.zst` (a zstd frame) or `.lz4` (an lz4 frame) loads as the file without that suffix: `cells/3_4.cell.zst` is a `.cell` resource. It is decompressed on the job pool with the decode (natively the read happens there too), so a cart can ship compressed files with no help from its host, and the stats count the packed bytes read. The zstd and lz4 command line tools, Node's `zlib.zstdCompressSync` and `cwpack` all write these. [cw_decompress](api:cw_decompress) and [cw_decompress_block](api:cw_decompress_block) decompress a buffer yourself, on any thread. zstd decodes at roughly 1 GB/s natively, lz4 several times faster; both decoders are BSD licensed (see `NOTICE`).

### Outputs

Every output is byte-identical for identical input:

- `DIR/assets/...`: the packed files plus `res.index`. This is a cart's asset directory.
- `--pak FILE`: the native `.cwpak`, one file with a sorted index (binary search lookup), each entry raw or compressed whole (zstd by default), kept compressed only when that saves a tenth. Entries decompress outside the pack's lock, so background reads decompress in parallel. The native platform mounts `<executable>.cwpak` automatically.
- `DIR/manifest.json`: the wasmcart cart-directory manifest, so a web cart can load files lazily over HTTP.
- `DIR/sources.txt`: every file read (what hot reload watches); `--depfile` for the build system.

Imports are cached in `.cwcache/` beside roots.txt, keyed by importer version, the settings it reads and every input byte, so a rerun only imports what changed. A missing reference or failed import exits 1 naming the file.

## Hot reload

In a native dev build an app made with `PACK` knows its own `cwpack` command. One line starts watching:

@include samples/gltf_scene/main.c#hotreload

Every few frames the engine checks the recorded sources; when one changed it reruns `cwpack` (incrementally), remounts the pack and reloads every loaded resource whose packed bytes changed, in place. Handles stay valid, [cw_res_version](api:cw_res_version) goes up, and glTF instances are rebuilt from the new template while keeping their transform and parent. Code that cached something derived from a resource compares versions. [cw_res_reload_from_memory](api:cw_res_reload_from_memory) does the same with bytes pushed by the editor or romdev, in a cart too. Code hot reload is separate: the `cwhot` host swaps a rebuilt game module while the tree and resources stay alive (`samples/hot_demo`).
