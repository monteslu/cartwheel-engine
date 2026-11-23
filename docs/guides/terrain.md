# Terrain

{{name}} draws heightmap terrain with CDLOD, collides it through Box3D's height field, paints it with Terrain3D's control-map layout and covers it with deterministic scatter: grass, rocks and trees. One header, one node type for the terrain and one for each scatter layer.

Header: [cartwheel/terrain.h](header:terrain.h). Node types: [Terrain3D](node:Terrain3D) and [TerrainScatter3D](node:TerrainScatter3D). The height field on its own is in [cartwheel/physics3d.h](header:physics3d.h) ([cw_heightfield3d_create](api:cw_heightfield3d_create), [cw_shape3d_heightfield](api:cw_shape3d_heightfield)). Samples: `terrain_demo`, a car driving a dirt track cut into a 2 km generated landscape, and `terrain_detail`, a flat field of flagstones for the near ground's options. Gates: `unit-terrain`, `terrain-drive` and `terrain-detail`.

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

Shading is three.js r187's MeshStandardMaterial, so lights, shadows, fog and the environment are three.js's own. A TSL extension of the engine's (`src/terrain/extensions`) supplies the rest:

- the vertex stage reads the heights, exactly, from an R32UI texture and morphs them;
- the fragment stage reads the control words, blends the layers, uses triplanar sampling on steep slopes, and past `far_distance` uses only a baked macro colour and the normal map;
- patches wholly past `far_distance` use a third program that reads just those two textures.

A side projection repeats, and on a big cliff that repeat shows as a grid. Set `side_detile` (0 to 1, off by default) and each cell of the projection reads the layer at a larger scale and its own offset, with the original scale as fine grain, joined along the rock's own cracks, while the rock takes colour over tens of metres and strata that follow height. It costs seven texture reads for two on a steep face, only where it is on.

The near patches cast shadows with the same extension's caster program, morphed for the viewing camera.

## The near ground

Four things decide how painted ground holds up from a play camera 2 to 60 m away. Every one is off or at its old value by default, and a terrain that sets none of them draws exactly as before, pixel for pixel. `samples/terrain_detail` shows each on a flat field of flagstones, and the `terrain-detail` gate measures each from the picture.

**Rotation and scale per texel.** A control word's uv rotation and scale (bits 13..7) can change from one texel to the next: paint a different turn on every block of ground and the same tile stops lining up into rows. Where the texels round a point disagree, each layer is read once for each different rotation and scale among them, and the reads are blended by the same bilinear weights, by height (the stones of one rotation stand over the joints of the other, at the layer's blend sharpness), so the change fades across one texel instead of drawing a straight seam where the nearest texel changes. The gradients are rotated and scaled with each read, so there is no mip seam either. Where the four texels agree (all ground painted with one rotation, the usual case), the shading is the single read it always was.

Each read costs what the layer costs, so ground where neighbouring texels differ pays up to four times the layer reads (two where only two rotations meet, along a block's edge). Painting a rotation per block of a few metres leaves most texels agreeing; painting a random rotation into every texel makes the whole field pay.

**Blend sharpness.** Where two layers meet, the one whose height (the albedo's alpha) stands out wins, and the two share only where their weighted heights are within `blend_sharpness` of each other (0..1 of the height range; 0.15 by default, Terrain3D's value). Smaller is a crisper edge that follows the higher layer's shapes (sand only in a cobble road's joints), larger a softer one. A layer can set its own with [cw_terrain_set_layer_blend_sharpness](api:cw_terrain_set_layer_blend_sharpness) (node property `layer<i>_blend_sharpness`); a pair uses the mean of its two.

**Macro variation.** A single layer over a wide field reads as wallpaper from a play camera: every few metres of ground looks like every other. Two options break it up, both off by default:

- `macro_tint` (0..1): the near ground's brightness, and a little of its hue, varied by world-space noise this much either way, over features `macro_tint_scale` metres across (40 by default). It costs a few arithmetic instructions a pixel and no texture reads.
- `macro_layer` (0..1): a second read of each layer with a tile `macro_layer_scale` times larger (4 by default), blended in by height from `macro_layer_near` metres to its full share at `macro_layer_far` (0 and 60 by default). The ground shows the layer's own stones at two sizes side by side, so the small tile's repeat stops lining up into a grid at a distance. It costs a second set of layer reads where it is drawn.

**Parallax.** A layer with a parallax depth ([cw_terrain_set_layer_parallax](api:cw_terrain_set_layer_parallax), node property `layer<i>_parallax`, metres from the top of its height to the bottom) is drawn with parallax occlusion within `parallax_distance` metres (30 by default, fading out over the last 30% of it): the eye's ray is marched into the layer's height and the layer is read where it meets it, so stones stand up from their joints and shift against each other as the view turns. A few centimetres suits gravel and cobbles. It applies to the top-down projection (cliffs' side projections stay flat). The steps per pixel are the quality's: none at Very Low and Low, 4 at Medium, 8 at High and 16 at Ultra; each step is one read of the layer.

What they cost on the Radeon 890M at 1920 x 1080 (`tests/terrain-detail-perf.mjs`: `terrain_detail` from its play camera, flagstones over two thirds of the frame, release build, GPU ms of the frame over the same frame without the option, the least of 3 interleaved rounds):

| option | GL ms | WebGPU ms |
|---|---|---|
| rotation blend (a quarter turn on every other 4 m block, against the same field's nearest-texel rotation) | 0.62 | 0.80 |
| macro_tint 0.35 | 0.63 | 0.72 |
| macro_layer 0.6 | 1.00 | 0.89 |
| both macro options | 1.04 | 0.94 |
| parallax 0.08 m, Medium (4 steps) | 1.03 | 0.90 |
| parallax 0.08 m, High (8 steps) | 1.20 | 1.13 |
| parallax 0.08 m, Ultra (16 steps) | 1.77 | 1.66 |

The frame without them took 1.7 to 1.9 ms on either backend. Most of a single option's cost is the program that carries the options, about 0.6 ms in that frame whichever option asks for it: a terrain draws with it (the `terrainfx` surface parts) only while it uses one of them, and with the program it always had otherwise, so a terrain that uses none pays nothing. Everything on at once as a release cart on romdev (WebGPU, the 890M) took the same frame from 1.75 to 4.59 ms.

Every option's settings live: [cw_terrain_set_shading](api:cw_terrain_set_shading) takes them from a definition, and a Terrain3D node applies a change of its properties at once.

## Quality

The terrain follows [cartwheel/quality.h](header:quality.h) (effects detail):

| preset | shading | far distance | ranges | parallax steps |
|---|---|---|---|---|
| Low | two layers, smoothly weighted from the four control texels around the pixel; no triplanar | x0.5 | x1 | none |
| Medium | Low, plus triplanar on steep slopes | x0.75 | x1 | 4 |
| High | Medium's shading | x1 | x1 | 8 |
| Ultra | High | x1.5 | x1.25 | 16 |

The painted layers tile without repeats: the top-down projection is cut into small triangular cells, each reading the layer at an offset of its own, blended across the joins. Each map is read twice, not once a cell: a cell's share is its barycentric weight less a floor (a fifth, or the least of the three weights where that is more), so the least-weighted cell has no share and is never read, and the two reads go out together. Away from a triangle's middle that is the three-cell blend with each cell fading a little sooner; round the middle the other two carry it. The cells and their offsets are those of the three-read blend it replaced, and in Ember Throne's regions the ground looks the same at native scale (a pebbled path is a little crisper, with no ghost of a second offset under it).

Which two layers a point blends is worked out on the CPU, a cell of the control map at a time: the `pair` texture holds, for each cell between four control texels, the layers in it (up to three) and each one's share at its four texels, rebuilt with the control map. The shader reads the nearest control texel and that cell, and a layer's weight at the point is one dot product, where it once searched seven candidates over all four texels.

Every level weighs the two strongest layers from the four control texels round a pixel and height-blends them once (High once blended each texel apart and mixed the four bilinearly, which drew diamonds where layers met). Triplanar reads only the side projection a slope faces (both only across the band where they hand over), skips the top-down one on ground steep enough that the sides carry it all, and a texel's second layer is read only where it has a share. A screen of steep canyon walls at High went from 38 ms of scene time to 8 on the Radeon 890M (racer's Dam), with the same transitions.

Scatter density follows `cw_quality_scale("grass")` or `("scatter")`. [cw_terrain_set_quality](api:cw_terrain_set_quality) pins a terrain to one level.

## The node

A [Terrain3D](node:Terrain3D) node owns its terrain's place and visibility, like any other 3D node:

- **Place.** The terrain is centred on the node, and moving the node moves it (translation only), physics bodies and scatter included. Move the node, not the terrain: the node sets the terrain's origin every tick, so [cw_terrain_set_origin](api:cw_terrain_set_origin) on a node's terrain is undone at the next tick, and the engine warns once, naming the node.
- **Visibility.** The terrain and its scatter are drawn, and cast shadows, only while the node and every Node3D above it are visible. A hidden terrain is not selected or posed either, so it costs no CPU time per frame. It still collides, and the height queries still answer.

## Collision and queries

[cw_terrain_attach_physics](api:cw_terrain_attach_physics) makes one static body per region (`region_size` quads square). Regions share their edge samples and one height range, so they line up exactly. The `Terrain3D` node attaches to the tree's 3D world itself. [cw_terrain_height_at](api:cw_terrain_height_at) runs Box3D's own ray cast in the order a world ray cast does its arithmetic, so it equals a physics ray to the bit; the `unit-terrain` gate checks 10 000 points. [cw_terrain_raycast](api:cw_terrain_raycast) and [cw_terrain_normal_at](api:cw_terrain_normal_at) answer the rest.

Vehicles and characters need nothing special: [cw_vehicle3d](api:cw_vehicle3d_update)'s wheel rays and [cw_mover3d](api:cw_mover3d_move)'s sweeps hit the height field like any static shape. The demo's car:

@include samples/terrain_demo/main.c#car

## Scatter

A scatter layer ([cw_terrain_scatter_add](api:cw_terrain_scatter_add), or a [TerrainScatter3D](node:TerrainScatter3D) child of the terrain) places copies of a mesh in square cells:

- **Cells.** 32 m, doubled while that stays within a quarter of the layer's visible range, so a long-range layer stays a few dozen draws.
- **Placement.** A pure function of the cell and the layer's seed by integer hashing. A candidate is kept by density, the density map, the control layers it may grow on, holes and slope. The same instances land on every host at any worker count.
- **Drawing.** Each cell is one InstancedMesh per mesh LOD with its own bounds. Cells are generated, culled and posed on the job pool.
- **LOD and fade.** Instances switch LOD by distance and shrink away over the end of the visible range (`fade_mode = SHRINK`), or dissolve there and crossfade between LODs (`DITHER`, below).
- **Quality thinning.** Each instance has a rank, and quality thins a layer by rank, so thinning never reshuffles.

A glTF mesh's primitives (a trunk and a crown) draw as parts that share one instance buffer, and a cwpack LOD chain gives the LODs. A part with fewer LODs than the first (leaf cards that do not simplify) keeps drawing its coarsest one at the coarser levels.

### Grass and foliage

The built-in `GRASS` shape is a tuft of 12 curved blades of three segments each, tapering from 3 cm at the root to a point and leaning out by a fifth to three fifths of their height, drawn both ways round with one normal bent toward the sky, so a blade is lit the same from either side. Its vertex colours run from a dark root to a lighter tip, a few blades drier than the rest; its far LOD has 6 wider straight blades that cover the same ground. Each instance's colour varies (`tint_jitter`, 0.25 for GRASS: brightness, and a dry yellow or a deep green), so no two tufts match.

A layer takes the foliage of [foliage.h](header:foliage.h) ([Foliage](guide:rendering-3d#foliage-wind-smooth-cut-outs-leaves-lit-from-both-sides)) on its own copies of its materials. All off (the defaults), a layer draws as before.

| Setting | What |
|---|---|
| `wind_strength`, `wind_frequency`, `wind_direction`, `wind_gust`, `wind_gust_scale` | wind: the top of the mesh leans up to `wind_strength` metres, with a phase from each instance's place, so neighbours never sway together |
| `smooth_alpha` | leaf and grass cards (alpha-tested meshes): alpha to coverage under MSAA, hashed alpha that TAA resolves without it |
| `two_sided_lighting` | double-sided meshes lit with the front face's normal on the back |
| `fade_mode = DITHER`, `lod_blend` | instances dissolve over the last `fade` metres instead of shrinking, and each LOD crossfades with the next over `lod_blend` metres round `lod_distance` (an instance in the band is drawn by both, by complementary dithers): no pop |
| `tint_jitter` | per-instance colour variation (an instance colour) |
| `root_from_terrain`, `root_height` | each instance's lower part takes the terrain's colour under it ([cw_terrain_albedo_at](api:cw_terrain_albedo_at): its layers' mean albedo by the control map, times the terrain's colour), blending into the mesh's own colour above `root_height` |

Wind is deterministic (the engine's frame clock), the same in every view of a split screen and in the shadows. `cw_terrain_scatter_cell_colors` gives a cell's instance colours, as `cw_terrain_scatter_cell` gives their places. Measured on the Radeon 890M with `samples/foliage_field` (gate `foliage`, on every host and both backends): wind 0.1 moves the tips of 1 m stems 0.07 m and 0.2 moves them 0.14 m, with neighbouring stems uncorrelated (-0.12; 0.99 when forced into one phase); `smooth_alpha` cuts the shimmer of a field of alpha-tested cards under a slow pan by 46% under MSAA 4x and by 31% under TAA; `two_sided_lighting` lights a card's back within 0.001 of its front (0.38 darker without); `root_from_terrain` puts every root within the jitter of the terrain's colour; DITHER cuts the largest single-frame change at a LOD switch from 22/255 to 8/255. What each costs on a 1920 x 1080 frame of a dense field (`tests/foliage-perf.mjs`, a busy machine, so read them as about): wind 0.5 ms on GL and WebGPU over 30 000 tufts (the part's per-pixel work included), `smooth_alpha` 0.4 to 0.7 ms, DITHER 0.5 to 0.9 ms, `root_from_terrain` 0.5 ms on GL and within the noise on WebGPU.

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

Where two painted layers blend, the second layer costs a second set of reads, and they are dear: the layers sit in one array texture read with 8x anisotropic filtering (the filtering that keeps ground sharp at a glancing angle), and on WebGPU (Mesa's Vulkan driver, under Dawn) anisotropic fetches from a different layer of the array than the first cost about twice what they cost on GL, while fetching other texels of the same layer costs the same on both. On a ground that blends two layers across the whole view at 1080p High on the 890M, the second layer took about 1.0 ms of GPU time on GL and 2.0 ms on WebGPU. The generated shaders have the same structure on both and compile to code of the same size, and the cost follows the anisotropic fetches: with anisotropic filtering forced off in both drivers it is 0.2 and 0.3 ms. The second layer's top-down normal is read only where its height blend gives it a share, which cut it to about 0.9 ms on GL and 1.7 ms on WebGPU in that view. And the second layer is not read at all where its height blend cannot give it a share: a layer's height is at most 1, so where the first layer's weighted height stands out over the second's share by more than the blend's sharpness, the second gets exactly nothing whatever it holds (about 0.1 ms of scene time on GL and 0.1 to 0.2 ms on WebGPU in Ember Throne's meadow, wood and spire, the same frames). A game that paints a broad blend everywhere pays this on the whole screen; one that keeps blends to the edges between layers pays it only there.

Where a lit terrain pixel's time goes, measured by switching pieces of the shader off one at a time in Ember Throne's meadow (terrain over most of a 1080p High frame, the 890M, scene pass): the layer reads are most of it. Dropping the painted layers altogether (the far colour everywhere) saves about 1.3 to 1.5 ms on GL and 1.8 to 2.3 ms on WebGPU. Of that, the stochastic tiling (three reads a layer instead of one, so no tile repeats) is about 0.75 ms on GL and 1.1 ms on WebGPU, and it is the reads: keeping its hash and cell math but reading once costs the same as plain tiling. Two reads in place of three (0.39.0) took the meadow's scene pass from 4.43 to 3.71 ms on GL and 4.06 to 3.80 ms on WebGPU (interleaved rounds on a loaded machine, the GPU's clock near 900 MHz, so these are larger than at speed). The second layer is about 0.3 to 0.4 ms, the search for it among the four control texels under 0.05 ms, the triplanar sides on that gentle ground under the noise. The lighting is smaller: the sun's shadow filter about 0.1 to 0.15 ms, the whole sun light about 0.2 to 0.25 ms, the clustered lamps under 0.1 ms on GL (on WebGPU within the run-to-run noise). With the depth prepass on (Ember Throne has it), each pixel is shaded once.

## Bigger than one grid

A world too big for one height grid streams one terrain per cell: a terrain set ([cw_terrain_set_create](api:cw_terrain_set_create)) shares one template's layers ([cw_terrain_share_layers](api:cw_terrain_share_layers)) and scatter layers across every cell terrain, builds each cell's height fields, texels and quadtree in the background, and draws them all from one pass. [Big open worlds](guide:streaming) shows it with `open_world`, a 16.4 km world. Scatter you add yourself can use the built-in meshes through [cw_scatter_def_builtin](api:cw_scatter_def_builtin).

## Limits

- Sizes are multiples of `grid_quads`, and at most 8192 quads a side.
- 8 layers are drawn (layer ids above 7 clamp).
- A terrain moves with its node (translation only); rotation and scale are not applied.
