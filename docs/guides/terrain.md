# Terrain

{{name}} draws heightmap terrain with CDLOD, collides it through Box3D's height field, paints it with Terrain3D's control-map layout and covers it with deterministic scatter: grass, rocks and trees. One header, one node type for the terrain and one for each scatter layer.

Header: [cartwheel/terrain.h](header:terrain.h). Node types: [Terrain3D](node:Terrain3D) and [TerrainScatter3D](node:TerrainScatter3D). The height field on its own is in [cartwheel/physics3d.h](header:physics3d.h) ([cw_heightfield3d_create](api:cw_heightfield3d_create), [cw_shape3d_heightfield](api:cw_shape3d_heightfield)). Sample: `terrain_demo`, a car driving a dirt track cut into a 2 km generated landscape. Gates: `unit-terrain` and `terrain-drive`.

## The data

A terrain is a grid of `(size_x + 1) x (size_z + 1)` samples, `vertex_spacing` metres apart. Each sample is a uint16 step between `min_height` and `max_height`, which is exactly how Box3D stores a height field, and a 32-bit control word in Terrain3D's layout:

| bits | field |
|---|---|
| 31..27 | base layer (0..31; 8 are drawn) |
| 26..22 | overlay layer |
| 21..14 | blend: the overlay's share, 0..255 |
| 13..10 | uv rotation, sixteenths of a turn |
| 9..7 | uv scale step (0 = 1x, 1..4 = +20 % steps, 5..7 = -20 % steps) |
| 2 | hole (no collision either) |

[cw_terrain_control](api:cw_terrain_control) packs a word. Every quad splits along the diagonal from its `(x, z + 1)` corner to its `(x + 1, z)` corner, as Box3D's height field does, so the drawn surface, the physics and the gameplay queries are the same triangles.

Heights come from [cw_terrain_import_heightmap](api:cw_terrain_import_heightmap) (a 16-bit grey PNG, or a raw `.r16`, resized to the samples), [cw_terrain_set_heights](api:cw_terrain_set_heights) / [cw_terrain_set_heights_m](api:cw_terrain_set_heights_m) for a rectangle, or [cw_terrain_generate](api:cw_terrain_generate) (fBm from integer hashing). Control words come from [cw_terrain_import_control](api:cw_terrain_import_control) (a Terrain3D control map as a PNG, each pixel's RGBA bytes being the word), [cw_terrain_import_splat](api:cw_terrain_import_splat) (an RGBA weight image: each sample gets its two heaviest layers) or [cw_terrain_set_control](api:cw_terrain_set_control). An edit marks what it touched, and the next update rebuilds just that: the GPU copies, the normals, and the physics regions it reaches.

`terrain_demo` paints its control map from slope and height, the way a tool would, and cuts its track in like an editor brush: the ground smoothed along a loop and blended into the hills, then written back:

@include samples/terrain_demo/main.c#cut

### From Blender

`tools/blender/export_terrain.py` exports a mesh from a `.blend` file, run headless, with no add-on to install. The mesh can be sculpted, displaced, or made with geometry nodes; modifiers are applied. The script samples it straight down on an N x N grid and writes three files:

- `heights.r16`;
- `splat.png`, from the mesh's colour attribute (RGBA = the weights of layers 0..3);
- `terrain.txt`, the [Terrain3D](node:Terrain3D) properties that match (sizes, spacing, height range), to paste into a scene.

```
blender --background world.blend --factory-startup --python tools/blender/export_terrain.py -- out/ --samples 1025
```

Blender's X is the terrain's x and its -Y the terrain's z, so the file's top view is the terrain seen from above.

## Layers

Up to 8 layers ([cw_terrain_set_layer](api:cw_terrain_set_layer) from images, [cw_terrain_set_layer_pixels](api:cw_terrain_set_layer_pixels) from memory), packed as Terrain3D packs them: albedo RGB with height in alpha, and normal RGB (OpenGL convention, +Y) with roughness in alpha. Each layer has its own uv scale in repeats per metre. The layers live in two array textures, mipmapped and repeat-wrapped by the GPU. The terrain's material ([cw_terrain_material](api:cw_terrain_material), a MeshStandardMaterial) scales the result: its colour, roughness and metalness.

## Drawing: CDLOD

The terrain is a quadtree of patches. A patch of level L covers `grid_quads << L` quads and is drawn with the same `grid_quads x grid_quads` grid, so its vertices are `2^L` samples apart. Level L is used within `lod0_range * lod_ratio^L` metres of the camera, measured in 3D so that mountains keep their detail. Over the end of each level's band its odd vertices morph vertically into the next level's triangles, so neighbouring levels meet without cracks. `lod0_range` is raised to the least that keeps neighbours within one level of each other. The `unit-terrain` gate audits every shared edge of the selection from four cameras, and checks that the same audit finds cracks when the ranges are too short. Skirts below each patch's border cover any rasterizer pinhole where a fine edge meets a coarse one.

Each frame, [cw_terrain_update](api:cw_terrain_update) (the node calls it in the pipeline's SHADOW phase) uploads what changed and selects patches on the job pool:

- the top of the tree is walked on the calling thread, and the subtrees are spread over the job pool;
- a node's four children are tested against the frustum and the range sphere at once with SIMD;
- the result is sorted front to back with a total order, so it is the same at any worker count.

Every selected patch is one instance: the near terrain is one instanced draw, the far terrain another.

Shading is three.js r186's MeshStandardMaterial, so lights, shadows, fog and the environment are three.js's own. A TSL extension of the engine's (`src/terrain/extensions`) supplies the rest:

- the vertex stage reads the heights, exactly, from an R32UI texture and morphs them;
- the fragment stage reads the control words, blends the layers, uses triplanar sampling on steep slopes, and past `far_distance` uses only a baked macro colour and the normal map;
- patches wholly past `far_distance` use a third program that reads just those two textures.

The near patches cast shadows with the same extension's caster program, morphed for the viewing camera.

## Quality

The terrain follows [cartwheel/quality.h](header:quality.h) (effects detail):

| preset | shading | far distance | ranges |
|---|---|---|---|
| Low | two layers, smoothly weighted from the four control texels around the pixel; no triplanar | x0.5 | x1 |
| Medium | Low, plus triplanar on steep slopes | x0.75 | x1 |
| High | each of the four control texels blended (up to 8 layer samples), height blending, triplanar | x1 | x1 |
| Ultra | High | x1.5 | x1.25 |

Triplanar reads only the side projection a slope faces (both only across the band where they hand over), skips the top-down one on ground steep enough that the sides carry it all, and a texel's second layer is read only where it has a share. A screen of steep canyon walls at High went from 38 ms of scene time to 8 on the Radeon 890M (racer's Dam), with the same transitions.

Scatter density follows `cw_quality_scale("grass")` or `("scatter")`. [cw_terrain_set_quality](api:cw_terrain_set_quality) pins a terrain to one level.

## Collision and queries

[cw_terrain_attach_physics](api:cw_terrain_attach_physics) makes one static body per region (`region_size` quads square). Regions share their edge samples and one height range, so they line up exactly. The `Terrain3D` node attaches to the tree's 3D world itself. [cw_terrain_height_at](api:cw_terrain_height_at) runs Box3D's own ray cast in the order a world ray cast does its arithmetic, so it equals a physics ray to the bit; the `unit-terrain` gate checks 10 000 points. [cw_terrain_raycast](api:cw_terrain_raycast) and [cw_terrain_normal_at](api:cw_terrain_normal_at) answer the rest.

Vehicles and characters need nothing special: [cw_vehicle3d](api:cw_vehicle3d_update)'s wheel rays and [cw_mover3d](api:cw_mover3d_move)'s sweeps hit the height field like any static shape. The demo's car:

@include samples/terrain_demo/main.c#car

## Scatter

A scatter layer ([cw_terrain_scatter_add](api:cw_terrain_scatter_add), or a [TerrainScatter3D](node:TerrainScatter3D) child of the terrain) places copies of a mesh in square cells:

- **Cells.** 32 m, doubled while that stays within a quarter of the layer's visible range, so a long-range layer stays a few dozen draws.
- **Placement.** A pure function of the cell and the layer's seed by integer hashing. A candidate is kept by density, the density map, the control layers it may grow on, holes and slope. The same instances land on every host at any worker count.
- **Drawing.** Each cell is one InstancedMesh per mesh LOD with its own bounds. Cells are generated, culled and posed on the job pool.
- **LOD and fade.** Instances switch LOD by distance and shrink away over the end of the visible range, so nothing pops.
- **Quality thinning.** Each instance has a rank, and quality thins a layer by rank, so thinning never reshuffles.

A glTF mesh's primitives (a trunk and a crown) draw as parts that share one instance buffer, and a cwpack LOD chain gives the LODs. A part with fewer LODs than the first (leaf cards that do not simplify) keeps drawing its coarsest one at the coarser levels.

## Performance

`terrain_demo`'s drive (frames 600 to 1200), output 1920 x 1080, each preset's own render scale, on a Radeon 890M. Measured with `tools/perf-frame.mjs`:

- GPU is the frame's timed passes (`--gpu-timers`).
- CPU is the engine's render phase. Native GL waits for the GPU inside it, so it tracks the GPU time. Native WebGPU does not; its present column is where it waits.
- Chromium's CPU column is the main thread's time in the cart's frame.

| preset | triangles | GL GPU ms | WebGPU GPU ms | GL render ms | WebGPU render ms |
|---|---|---|---|---|---|
| Low | 0.66 M | 1.5 | 1.4 | 1.6 | 1.0 |
| Medium | 0.95 M | 2.2 | 1.8 | 2.1 | 1.1 |
| High | 1.35 M | 4.0 | 3.9 | 4.0 | 1.2 |
| Ultra | 1.50 M | 4.2 | 4.1 | 4.1 | 1.5 |

Chromium at High: 4.1 ms GPU (WebGPU) and 3.5 to 3.8 ms of main-thread CPU on both backends. The terrain's own CPU share is small: patch selection about 0.02 ms and scatter posing about 0.1 ms on the job pool.

A Vega 8 (8 GCN CUs at 1.1 to 2.0 GHz, dual-channel DDR4) has a fifth to a third of the 890M's shader throughput and under half its bandwidth. Expect three to five times the GPU time:

| preset | Vega 8 GPU ms, estimated |
|---|---|
| Low (the engine's pick for that class) | 4 to 7 |
| Medium | 5.5 to 9 |
| High | 12 to 20 |
| Ultra | 12 to 21 |

Low and Medium hold 60 Hz with room for a game; High is a 30 to 60 Hz frame there.

## Bigger than one grid

A world too big for one height grid streams one terrain per cell: a terrain set ([cw_terrain_set_create](api:cw_terrain_set_create)) shares one template's layers ([cw_terrain_share_layers](api:cw_terrain_share_layers)) and scatter layers across every cell terrain, builds each cell's height fields, texels and quadtree in the background, and draws them all from one pass. [Big open worlds](guide:streaming) shows it with `open_world`, a 16.4 km world. Scatter you add yourself can use the built-in meshes through [cw_scatter_def_builtin](api:cw_scatter_def_builtin).

## Limits

- Sizes are multiples of `grid_quads`, and at most 8192 quads a side.
- 8 layers are drawn (layer ids above 7 clamp).
- A terrain moves with its node (translation only); rotation and scale are not applied.
