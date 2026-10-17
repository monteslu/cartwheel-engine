# Changelog

What each release added, changed and fixed. Releases are git tags; each one's
prebuilt SDKs (one per target, see [docs/sdk.md](docs/sdk.md)) are on its
GitHub release. The guides in `docs/guides/` cover each feature in depth.

## Unreleased

### Added
- `cw_cloth_step_many`: steps a scene's cloths as one job each, and the
  physics guide's cloth cost table.

### Changed
- The GPU guard test scales to the pinned card (an integrated GPU's VRAM
  carve-out and GTT read together), and the network test runs wasmcart-native
  players paced instead of uncapped.

## v0.11.1 (2026-10-16)

### Fixed
- The SDKs install `libcw_plmpeg`: the engine links the video decoder into
  every game, and v0.11.0's SDKs left it out, so game links failed with
  "cannot find -lcw_plmpeg".

## v0.11.0 (2026-10-16)

The largest release so far: lighting, global illumination, fog, water,
cloth, IK, world streaming and more.

### Added
- **Clustered lighting** (`cartwheel/lighting.h`, `cartwheel/froxel.h`):
  hundreds of omni and spot lights on GL and WebGPU, binned into r186
  ClusteredLighting's froxel cells on the job pool. Sample `night_street`
  (214 lights, four shadowed floodlights).
- **Cascaded sun shadows** and a **shadow atlas** for clustered lights.
  Sample `street_sun`. Clustered lights and cascades can keep
  auto-instancing (`LightingSettings.batching`).
- **Global illumination** (`cartwheel/gi.h`): reflection probes
  (`ReflectionProbe3D`), a light probe grid, and screen-space reflections and
  bounce light (PostProcess `ssr`, `ssgi`). Samples `gi_room`,
  `gi_probe_check`.
- **Volumetric fog**: a froxel grid for `HeightFog` (volumetric mode) and
  `FogVolume3D`, with light shafts through shadow tiles, on GL and WebGPU.
  Samples `fog_shafts`, `fog_valley`; per-preset costs in the 3D rendering
  guide.
- **Water** (`cartwheel/water.h`): `Water3D` with Gerstner waves, r186's
  planar reflection, refraction and flow, and `Buoyancy3D` for floating
  bodies. Sample `water_boat`; [water guide](docs/guides/water.md).
- **Decals on meshes**: `Decal3D` mesh mode clips decals onto meshes,
  skinned characters and glass. Sample `fx_mesh_decals`.
- **CSG block-out shapes** (`cartwheel/csg.h`): `CSGBox3D`, `CSGCylinder3D`,
  `CSGSphere3D` and `CSGCombiner3D`, baked by `cwcsg` with Manifold. Sample
  `csg_greybox`.
- **Cloth and ropes** (`cartwheel/cloth.h`): an XPBD solver beside Box3D,
  `Cloth3D` and `Rope3D` nodes, cloth that follows a bone. Sample
  `cloth_demo`.
- **Inverse kinematics** (`cartwheel/ik.h`): a pose layer between animation
  and skinning, deterministic solvers and skeleton modifier nodes; solves go
  to the job pool from 8 models up. Sample `anim_ik`.
- **World streaming** (`cartwheel/stream.h`): a world in cells that load and
  unload around the player, with terrain sets that stream with them. Sample
  `open_world` (a 16.4 km world); [streaming guide](docs/guides/streaming.md).
- **Background jobs** (`cw_job_done`); resource loads run as background jobs.
- **Resources**: lanes and priorities, background reads, non-blocking cancel,
  a residency cache, and packed resources compressed with zstd or lz4.
- **Video** (`cartwheel/video.h`): MPEG-1 cutscenes through pl_mpeg, YUV to
  RGB in a shader on GL and WebGPU, sound as a stream voice. Sample
  `video_cutscene`; [video guide](docs/guides/video.md).
- **Audio**: HRTF for headphones (KEMAR table, 64-tap SIMD FIR), a distance
  low-pass, and FDN reverb with reverb zones that follow the listener
  (`ReverbZone3D`).
- **Noise** (`cartwheel/noise.h`): FastNoiseLite as `cw_noise_*`,
  `gen://noise` textures and GLSL / WGSL ports checked against the CPU.
  Sample `noise_demo`; [noise guide](docs/guides/noise.md).
- **Shading parts**: several three.c generator extensions on one material.
- `cw_pass_texture_new` / `_free`: a CPU-made texture any pass program can
  sample.
- **Profiler**: zones on every engine phase, cloth steps and IK, with
  Chrome / Perfetto traces natively and from carts.
- Terrain under clustered lights and the sun's cascades.

### Changed
- A run that skips draws now fails (`render.skipped`, checked by
  `run-cart`): a missing program state can no longer pass a gate unnoticed.
- three.c v0.186.13 to v0.186.16.
- CI builds once and hands the native build to the test jobs; the gates are
  split over balanced jobs by measured time.

### Fixed
- Cart SDL2 builds copy `deps/sdl2` instead of linking it, so a worktree
  sharing a dependency checkout no longer applies the SDL patches to the
  shared tree.
- GI's SH projection writes all seven groups.
- Terrain3D's near patches draw again (three.c v0.186.16).

## v0.10.0 (2026-10-16)

### Added
- three.c v0.186.11: `PMREMGenerator.fromScene` options (size, position,
  render target), for reflection probes away from the origin.

### Fixed
- Android export gate: Enter is pressed once loading is done and the title
  takes input.

## v0.9.0 (2026-10-16)

### Added
- **Terrain** (`cartwheel/terrain.h`): heightmap data, CDLOD, Box3D
  collision, splat shading through one layer atlas, Low through Ultra
  quality, shadow casting, skirts at patch edges, and scatter layers with
  multi-part meshes. Sample `terrain_demo`;
  [terrain guide](docs/guides/terrain.md).
- Box3D height fields (`cw_heightfield3d`, `cw_shape3d_heightfield`).
- `tools/blender/export_terrain.py`: a Blender mesh to a Terrain3D heightmap
  and splat.
- The SDKs ship the engine's three.c generator extensions.

### Changed
- three.c v0.186.10: mipmapped array textures on WebGPU in compatibility
  mode.
- The build scripts and the exporter take a lock: one build of a tree at a
  time.

## v0.8.0 (2026-10-15)

### Added
- **Navigation** (`cartwheel/nav.h`): navmeshes, path queries, obstacles and
  crowds on Recast, Detour and DetourTileCache (pinned, with four
  determinism patches). Nodes `NavigationRegion3D`, `NavigationAgent3D`,
  `NavigationObstacle3D`. Samples `nav_demo`, `nav_perf`;
  [navigation guide](docs/guides/navigation.md).
- three.c v0.186.7 to v0.186.9: extension programs drawn at run time, shadow
  casters drawn with the material's extension, array and integer textures,
  and 16 textures per program on WebGPU.

### Fixed
- The WebGPU tonemap without a colour grade binds an empty LUT unit.

## v0.7.0 (2026-10-15)

### Added
- Graphics settings: render resolution, each setting on its own, and an Auto
  mode that holds a frame-time budget.

## v0.6.7 (2026-10-15)

### Fixed
- Runtime libraries (Dawn, ANGLE) are copied once per output directory before
  its apps link: two apps linking at once raced on the same files.

## v0.6.6 (2026-10-15)

### Fixed
- WebGPU windows ask SDL for no graphics API of their own (on macOS a
  flagless window was given GL).

## v0.6.5 (2026-10-15)

### Fixed
- WebGPU test runs on macOS: no Metal view on SDL's offscreen window.

## v0.6.3 (2026-10-15)

### Fixed
- Headless GL on macOS uses SDL's offscreen driver on EGL's default display
  when the driver cannot enumerate devices (ANGLE).

## v0.6.2 (2026-10-15)

### Fixed
- GL windows open without multisampling when the driver cannot make a
  multisampled surface.

## v0.6.1 (2026-10-15)

### Fixed
- macOS GL: SDL makes the GLES surface on a layer-backed view. Linux links
  GLES by its soname.

## v0.6.0 (2026-10-15)

### Added
- The game shell (`cartwheel/shell.h`): loading screen, title, player join,
  settings and pause around a game's play; [shipping guide](docs/shipping.md).

## v0.5.3 (2026-10-15)

### Added
- Dev carts that link in seconds, and a GPU-free `cwpack`.

### Changed
- SDKs are immutable: the release workflow refuses to replace an SDK a
  release already carries.

## v0.5.2 (2026-10-15)

### Fixed
- WebGPU on a window: an alpha mode the surface offers, and no start without
  a presentable surface format.

## v0.5.1 (2026-10-15)

### Fixed
- `bin/cwpack` in an SDK finds the SDK's Dawn, and `fetch-deps.sh` runs npm
  only in a wasmcart checkout.

## v0.5.0 (2026-10-15)

The first tagged release.

### Added
- A 2D and 3D game engine in C with native and wasmcart targets: scenes,
  nodes, 2D and 3D rendering (three.c, the three.js r186 API in C, on GLES 3
  and WebGPU), Box2D v3 and Box3D physics, one job pool, audio, input, text,
  UI, triggers, the debug channel and the cross-host gates.
- Prebuilt engine SDKs, one per target, built from a release tag.
- Split screen with shadow maps once a frame, an optional shared post chain
  and a hook before each view draws.
- GPU particles on WebGPU in one compute pass and one render pass for all
  systems.
- Post fusion on GL and WebGPU.
- Input scripts that drive pads 2 to 4; pads keep their players across
  hot-plug.
- `perf-frame` for another repository's apps (`CW_PROJECT_ROOT`).
- `tools/blender/render_views.py`: a model from six angles on one sheet,
  rendered headless.
- CI on Mesa's software renderers: native build, API and docs checks, GL and
  WebGPU gates, and carts.
