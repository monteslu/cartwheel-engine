# 3D rendering

{{name}} draws 3D through three.c, three.js r186 in C, and wraps it in a per-tree pass graph that adds a linear HDR workflow, sky, fog, baked lighting, culling, particles and decals. This page explains how those pieces fit together, what each costs and which sample shows it.

## The layers

There are three layers, and you can work at any of them:

- **three.c** is the renderer. Its names follow three.js: `new THREE.Mesh(geo, mat)` is `t3_mesh_new(geo, mat)`, `renderer.render(scene, camera)` is `t3_renderer_render`. Ownership is reference counting. It draws with the shader programs three.js r186's node renderer generates, so its materials, glTF, skinning, morph targets, `LOD`, `InstancedMesh` and render targets render what r186 renders. The programs come from program tables: three.c's core table for the renderer's own passes, and the app's tables for its materials (see [Program tables](#program-tables)).
- **The scene tree** ([render3d.h](header:render3d.h)) owns one three.c scene and renderer per tree. Every `Node3D` drives a three.c object (a group by default; `cw_node3d_object` returns it, `cw_node3d_set_object` replaces it), so you can hang any three.c object under a node and the tree keeps its transform, interpolated between fixed ticks.
- **The render pipeline** ([pipeline.h](header:pipeline.h)) is a small frame graph per tree. The scene runner (`cw_scene_app`) renders through it every frame; other apps call `cw_pipeline_render`. Modules add their work to it as passes.

Coordinates are Y up, -Z forward. Cameras, lights and decals face their -Z (not the origin, as three.js's default light target does): aim them with rotation or `cw_node3d_look_at`.

You do not need the tree to use three.c. The `hello` sample is a bare three.c scene in a platform app:

@include samples/hello/main.c#three_scene

`cw_render3d_window_renderer` is `t3_renderer_new` on whichever GPU API the run uses, and `cw_render3d_window_target` (in `render`, before `t3_renderer_render`) points it at this frame's window, so the same app draws on GL and on WebGPU. A dual cart may not call GL once the host has selected WebGPU, and a bare `t3_renderer_new` renderer is a GL renderer.

Most games use nodes instead: `samples/scene3d` builds a level from `.cwscene` files only, and `samples/gltf_scene` loads packed glTF models through the resource system with LOD chains.

## Program tables

three.c draws every material with the program three.js r186 generates for its state: the material kind, its features (maps, skinning, instancing, clearcoat...), fog, shadows and the light counts, written like `standard+map+rep+shadow+l1001`. The programs are captured from three.js ahead of time and compiled in as tables. The engine links three.c's core table only (output pass, backgrounds, PMREM, shadow casters), so an app carries the programs it draws and no others: a cart's wasm went from about 36 MB with three.c's full table to 7.5 MB.

A table is a manifest of states, `<name>.txt`, with the C generated from it beside it (`<name>_gl.c`, `<name>_wgpu.c`), all checked in: a build needs only a C compiler. An app gets its tables from `cw_add_app(... PROGRAMS path/to/<name>.txt)`, from `programs/<app>.txt` beside its CMakeLists.txt (what a new project uses), or from its directory's `CW_PROGRAMS` (the samples share `samples/programs/cw_samples.txt`). The engine and the editor register their own tables (`src/render/programs/cw_engine.txt`, `editor/programs/cw_editor.txt`).

When an app draws a combination its tables lack:

- a native development build builds the program on first use (`CW_THREE_BUILDER`, three.js r186 in an embedded QuickJS; a few hundred milliseconds), so nothing goes missing while you iterate;
- a cart or an exported build skips the draw, and says so loudly: an error in the log that names the exact command to run (`node tools/programs.mjs <your app>`), a red banner across the top of the picture with the same command while any state is missing (so an empty screen is never a mystery), and with `CW_PROGRAMS_STRICT=1` in the environment (a native run) the run exits with status 4 at the first missing state;
- either way the state is reported: the pipeline's warning, `cw_pipeline_missing_states`, the debug fields `cw.three.missing` and `cw.three.missing_states`, and natively `CW_PROGRAMS_LOG_DIR` or `T3_MISSING_LOG`. Every gate fails on a reported state and prints it.

To add the states, run the app through the tool, which merges what it lacked into the app's first table and regenerates:

    node tools/programs.mjs <app> [--gates]      # the app's runs (and its gates' runs), GL and WebGPU
    node tools/programs.mjs --projects <dir>     # a project outside the tree's build
    node tools/programs.mjs <app> --scenes LEVEL=a,b,c   # an app that picks its scene from the environment: one run per scene, the union of their states
    ninja -C build/native <name>_programs        # regenerate one table from its manifest

then commit the manifest and the generated C. A cart is asked for states a native run is not (the host's quality and capabilities differ, so a native survey can miss them and the shipped cart shows the red banner): survey the dual cart that ships with `node tools/programs.mjs --carts <app> [--gates]`, which runs `build/cart-wgpu/carts/<app>.wasc` on a GL host (romdev's dual cart told to render with GL, `run-cart.mjs --dual-gl`) and on a WebGPU host, reads the states the cart reports and merges them into the app's table (`tests/programs-carts.mjs` checks it, with a fully covered cart as the control). A project's carts are surveyed from its own tree: set `CW_PROJECT_ROOT` to it. The survey fails, naming the app and the host, when a run surveyed nothing (the cart did not load, never rendered a frame, or draws without the engine's pipeline, so it has no state list to read) and when there is no cart to survey; a run that surveyed nothing never reads as "no missing states". Generating captures on three.c's mock GPU (byte-identical to a real one), so it needs Node but no GPU. The tool minimizes a manifest: a program captured for more lights of each kind (and the same shadow casters) also draws scenes with fewer, so `standard+fog+l1701` replaces `standard+fog+l1001`.

Because the mock GPU accepts any shader, every regeneration then compiles the table's WebGPU programs with the WGSL compiler a WebGPU host uses (Tint, inside Dawn), on Mesa's software Vulkan (no GPU needed), and fails naming the state and the compiler's message when one is rejected: a table never ships a program that draws nothing on WebGPU (`tests/programs-wgsl.mjs`, with controls that must fail). Run it by hand on a capture directory with `node tools/lib/wgsl-check.mjs build/native/programs-cache/<table>-<hash>`. Two things to know when writing shading code of your own (an extension under `src/*/extensions` or `--extend`):

- The Dawn the engine ships stops with an internal compiler error ("swizzle view instruction still has usages after lowering") on a swizzle of a swizzle of a variable, which TSL writes for `texture(t, uv).rg` followed by `.x`. Swizzle once (`texture(t, uv).x`, or `dot(k, vec2(a, b))` for a weighted sum).
- WGSL leaves the result of arithmetic on infinity and NaN to each implementation. Never pick between two whole results with `mix(a, b, w)` where `w` is 0 or 1 and the unused side can overflow: infinity times 0 is NaN, and a NaN in one channel comes out as black in that channel (the water once lost its green this way on WebGPU only, where a raw depth texel overflowed in a debug view mixed in by 0). Use `select(cond, b, a)`, and clamp texels that hold a large "nothing here" value (a depth copy holds 3e38 where nothing was drawn) before any use.

Vertex colours (`vertex_colors` on a material) take an rgb or an rgba attribute; a glTF exporter's default `COLOR_0` is rgba and draws, with its alpha ignored (three.c 0.186.19: the generated programs read the colour only). A colour attribute of any other width is refused and the draw is reported as skipped. `samples/vertex_colors` draws both and its gate (`vertex-colors`, GL and WebGPU) has a control with a refused attribute.

Texture filtering is the same on GL and WebGPU. A texture's `anisotropy` (1 by default; 8 is a good value for floors, roads and walls seen at a low angle, where detail otherwise smears into a blur) applies on both backends when its filters are linear with a linear mip filter (`T3_LINEAR_MIPMAP_LINEAR`, the default); the GPU clamps it to what it supports (16 at most). Wrap, filter and anisotropy can be changed at any time, also on a texture from the resource system after it loaded: the next draw uses them. A resource texture's colour space is fixed when it loads (see `cw_res_texture`). `samples/texture_filtering` draws a tiled floor at a grazing angle and a wall with resource textures, at anisotropy 8; its gate (`texture-filtering`) holds GL and WebGPU to the same picture, with a control that turns anisotropy off on WebGPU alone.

## The 3D node types

| Node | What it configures |
|---|---|
| `Camera3D` | fov, near, far; the first `current` camera renders |
| `MeshInstance3D` | a primitive (BOX, SPHERE, PLANE, CYLINDER) scaled to `size`, or a `mesh` resource; material colour, metalness, roughness, emissive; shadows; visibility and baking properties |
| `DirectionalLight3D`, `OmniLight3D`, `SpotLight3D` | colour, energy, range, decay, cone, shadows, strength in volumetric fog |
| `WorldEnvironment` | background, ambient and hemisphere light, three.c fog |
| `PostProcess` | the pipeline's mode and post settings |
| `ProceduralSky`, `HeightFog`, `FogVolume3D` | sky and day cycle, height fog, volumetric fog |
| `LightmapGI` | baked indirect light |
| `ReflectionProbe3D`, `LightProbeGrid3D` | reflections of the surroundings, bounce light |
| `CSGBox3D`, `CSGCylinder3D`, `CSGSphere3D`, `CSGCombiner3D` | block-out shapes baked into one mesh (greyboxing) |
| `VisibilityManager`, `Room3D`, `Portal3D` | culling, LOD, impostors, portals |
| `GPUParticles3D`, `Trail3D`, `ParticleAttractor3D`, `ParticleCollider3D` | particles and trails |
| `Decal3D` | projected decals |

Primitives are unit shapes shared per tree and scaled to `size`, and primitives with the same material properties share one material, so same-shaped, same-looking meshes draw as one instanced batch.

Geometry built in code goes on a MeshInstance3D with [cw_mesh3d_set_procedural](api:cw_mesh3d_set_procedural), so the VisibilityManager culls it (BVH, occlusion, portals) like any other mesh. The node retains the `t3_mesh` as a child, and the mesh keeps its own local transform, materials and shadow flags; while it is attached, the node's primitive, `mesh` resource and look properties are ignored, and no LOD chain is inferred. Passing NULL goes back to the property-driven geometry. It returns false for another node type or a mesh that already has a parent.

Opaque meshes draw front to back by the centre of their world bounds rather than by their origin, which orders merged world-space geometry and instanced scenery correctly (explicit render order, transparent and skinned objects keep three.js's ordering). `CW_SORT_ORIGINS=1` sorts by origin as three.js does, to compare.

three.c lights a surface with at most 8 lights of each kind (directional, point, spot, hemisphere); lights past that, in scene order, light nothing. The engine logs a `LIGHT CAP EXCEEDED` warning naming the count, once per kind. With many lamps, keep a few real lights on the ones nearest the view and show the rest as emissive (as `samples/showcase` does). Every property, with its type, default and range, is on the node's reference page, and a running app answers `props <node>` over the debug channel.

## The pass graph

A frame runs the tree's active passes phase by phase, and inside a phase by `order`:

| Phase | Built-in and module passes |
|---|---|
| SHADOW | "sky_env" (100: sky cube), "visibility" (2000: culling, LOD, impostor bakes) |
| PREPASS | "depth_prepass" when `depth_prepass` is on (GL only: depth of the opaque queue, alpha-tested cutouts still discarding; transparent and transmissive surfaces, colour and the sky are left out) |
| OPAQUE | "scene" (0: three.c renders the tree), "impostors" (100), "occlusion_queries" (900) |
| SKY | "sky_sun" (0: the sun disk) |
| EFFECTS | "decals" (100), "particles" (200), "height_fog" (300) |
| POST | "velocity", "ssao" (100), "taa" (200), "bloom" (300), "tonemap" (500), "grade" (550), "aa" (600), "lens" (700), "debug_view" (10000) |
| OVERLAY | "canvas" (-100: the tree's 2D and UI), "app" (0: your render callback), debug draw at 1000 by convention |
| PRESENT | "visibility_restore" (1000), readbacks |

Passes declare what they read and write (`CW_RES_COLOR`, `CW_RES_DEPTH`, `CW_RES_VELOCITY`), and the graph acts on it: reading depth moves the scene offscreen and makes a depth copy for SKY and EFFECTS, reading velocity schedules the velocity pass, and colour writers in POST join a ping-pong chain of pooled targets.

A HUD on a scene app is the "app" overlay: draw with render2d in `base.render` and it lands after the 3D and the post chain. Adding a pass of your own is a descriptor and a callback. `samples/remote_target` adds a debug-draw overlay that outlines its movers:

@include samples/remote_target/main.c#debug_pass_exec

@include samples/remote_target/main.c#debug_pass_add

The header's comment walks through an EFFECTS pass that samples depth, and lists the rules a pass follows: raw GL is fine, the pipeline resets three.c's cached GL state around its own three.c renders, scratch targets come from `cw_pass_rt` and are yours until the frame ends, persistent ones (history buffers) from `cw_pipeline_rt_persistent`. Register a pass once per tree, from a node's enter_tree, a tree hook or app init; `cw_pipeline_set_pass_enabled` switches any pass, built-ins included.

Every pass can be timed on the GPU where the host has timer queries (natively; carts never use them). The timers are off until asked for, because reading them back costs the CPU every frame: `CW_GPU_TIMERS=1` turns them on from the start, and the debug command `pipeline`, which lists the passes, whether they ran and their GPU ms, turns them on from its first call. `cw_pipeline_pass_stats` gives the same in code (`gpu_ms` -1 while the timers are off).

## The workflow

Every tree renders the way three.js r186 does: colours are linear, lights are physical units (directional energy is lux-like illuminance, omni and spot energy is candela-like intensity with inverse-square falloff, windowed to 0 at `range` when it is set), the scene renders linear HDR into RGBA16F, and the "tonemap" pass applies exposure, the tone curve (AgX by default) and the sRGB transfer. With exposure 1 and AgX, sensible values are a sun of 3 to 5, room lamps of 10 to 50 a few metres away and ambient 0.05 to 0.3; real photometric values work with `exposure_ev` around -14. A light's `energy` runs from 0 to 100000, enough for the sun's real 100000 lux; a value outside that is clamped to the range and the engine logs a warning the first time it happens for that light (the same goes for every property that has a range).

Pick colours darker than a colour picker suggests: they are linear, and the tone curve desaturates bright ones (`color(1, 0, 0)` shows as about (220, 90, 75) under AgX). A scene authored for the old r128 workflow (display colours, r128's legacy light units) converts once with `node tools/convert-r128-scene.mjs <file.cwscene>`.

Each tree renders through its own pipeline; `cw_pipeline_set_output` sends it into a target instead of the screen, the pattern any render-to-texture or split-screen view uses.

## The post stack

The post settings are the fields of `cw_post_settings`, mirrored as PostProcess properties: MSAA, depth prepass, exposure (a multiplier and stops), tone curve (r186's: AGX by default, NEUTRAL, ACES_FILMIC, CINEON, REINHARD, LINEAR), energy-conserving bloom, GTAO at half resolution, TAA (Halton jitter, velocity reprojection, neighbourhood clip), FXAA or SMAA 1x, a 3D LUT grade loaded from a `.cube` file (`cw_lut_load_cube`), vignette, grain and chromatic aberration. `debug_view` shows depth, normals, AO, velocity or the bloom chain instead of the image. Nothing is lowered automatically: quality settings are the developer's.

**Depth of field** (`dof`, PostProcess properties `dof_focus_distance`, `dof_focus_range`, `dof_blur`, `dof_near_blur`) blurs what is nearer or farther than the focus, as a long lens does: a character select screen, a portrait, a cut scene. It runs on the HDR image before bloom, at full resolution: each pixel works out its blur from its view depth and gathers 32 samples on a golden-angle disk. A sample that lies behind the pixel counts only up to the pixel's own blur, so a sharp face never picks up the blurred wall behind it, while a blurred foreground still spreads over what is behind. `dof_blur` is the largest radius in pixels of a 1080 line image (it scales with the output height); `dof_near_blur` is the share of it used in front of the focus. A game usually turns it on for a menu or a close camera and off in play (set the properties on its PostProcess node from code). The Low effects preset turns it off. Cost: 32 colour and 32 depth reads a pixel at full resolution. On the Radeon 890M at 1920 x 1080 (post_stack, its depth of field segment against its plain segment, tools/perf-frame.mjs --gpu-timers, load average 10 to 12) it added about 0.5 ms of GPU time on GL and 1.2 ms on WebGPU; a Vega 8 (about a quarter of an 890M) should expect 2 to 5 ms, so keep it to menus and close cameras. Its gate is `post-dof`.

Two limits stated in the header: TAA reprojects camera motion only (moving objects rely on the neighbourhood clip), and SMAA has no diagonal pattern search yet.

`samples/post_stack` steps one static room through every effect in turn, one schedule segment each (its gates are `post-bloom`, `post-ssao`, `post-taa` and so on, each against the plain AgX segment), and measures TAA convergence over one jitter cycle.

## Lights and shadows

Lights are three.js r186 lights: directional, omni (point), spot, ambient and hemisphere, with r186 shadow maps for every light that casts them (`shadow`, `shadow_size`, biases; a mesh needs `cast_shadow` and `receive_shadow`). How many of them a surface sees depends on the tree's lighting mode ([cartwheel/lighting.h](header:lighting.h)).

### Hundreds of lights: clustered lighting

In the default FORWARD mode every light goes to three.c, which lights a surface with at most 8 lights of each kind (the rest light nothing, and the engine warns once). A street of lamps, a wall of neon signs or a firefight of muzzle flashes needs more. Add a LightingSettings node in CLUSTERED mode:

@include samples/night_street/assets/main.cwscene#Lighting

CLUSTERED is three.js r186's `ClusteredLighting`, on GL and WebGPU alike:

- Every frame the engine bins the shadowless omni and spot lights into a froxel grid over the view: 32 x 32 pixel tiles times 24 depth slices spaced exponentially between the camera's near and far, at most 64 lights a cell (r186's numbers, all settings of the node). The binning runs on the CPU on the job pool, so GLES3, which has no compute shaders, gets the same lists as WebGPU, and the lists are the same at any thread count.
- Each lit material (Lambert, Phong, Standard, Physical) draws with the "cluster" shading part: a fragment finds its cell and loops that cell's lights with r186's own BRDF and falloff. With few lights, CLUSTERED and FORWARD draw the same image (the `cluster-parity` gate compares them on both backends).
- Directional, hemisphere and ambient lights stay on three.c's path; shadowed omni and spot lights are clustered too, with their shadows in an atlas (below).

Three things go beyond r186. Spot lights are clustered too (r186 clusters only point lights), with a cone test when binning and r186's spot cone in the loop; `cluster_spot_lights` turns that off. A full cell keeps its nearest lights. And a light with `range` 0, which r186 would put in every cell, gets the range where its intensity over distance squared falls under `light_cutoff` (0.005), and fades out there as a range does: give lights a range for full control.

Costs: binning is O(cells each light reaches); the GPU pays a cell lookup per fragment plus each light of the fragment's cell, so the cost follows how many lights overlap on screen, not how many exist. Each frame uploads 64 bytes a light, 4 bytes a cell and 2 bytes a light reference. A mesh whose material carries the cluster part is drawn on its own (three.c does not auto-instance it), and a material with an extension of its own (terrain) sees only three.c's lights. Effects detail keeps 24 lights a cell on Low and 32 on Medium (64 on High and Ultra). The debug command `lighting` reports the light counts, the grid and the binning time; `cw_lighting_get_stats` gives the same in code.

The grid is public ([cartwheel/froxel.h](header:froxel.h)): `cw_lighting_froxels` returns the tree's, and any pass can build its own and bin its own volumes (fog volumes, decals, probes) into the same cells.

`samples/night_street` is a 240 m street at night: 58 street lamps, 96 neon signs, 48 shop-window spot lights, 12 muzzle flashes and four floodlights with shadows: 218 clustered lights, the floodlights' shadows in the atlas. Measured on an AMD Radeon 890M (integrated) at 1920 x 1080 with `night_street_perf` (GL: the frame with the GPU drained each frame; WebGPU: the passes' GPU time from timestamps), in ms per frame:

| Preset | GL frame | GL GPU | WebGPU GPU | Binning (CPU) |
|---|---|---|---|---|
| Low | 8.8 | 3.4 | 2.1 | 1.3 |
| Medium | 9.1 | 4.2 | 2.2 | 0.9 |
| High | 13.7 | 7.4 | 3.7 | 1.5 |
| Ultra | 12.9 | 7.0 | 3.5 | 1.4 |
| High, FORWARD (8 lights of a kind) | 3.3 | 1.5 | 1.0 | 0 |

The machine was shared while measuring (load average about 40), so the CPU side (the GL frame and the binning; binning alone takes about 0.4 ms on an idle machine) reads high. The lights are what costs: FORWARD draws the same street with 16 of its 218 lights (8 of a kind). A Vega 8 has roughly a third of the 890M's shader rate: expect 2.5 to 3 times the GPU column, so High is about 10 ms of GPU on WebGPU and over 20 ms on GL there; Medium and Low (24 and 32 lights a cluster, fewer pixels) are the presets for it.

### Shadows for many lamps: the shadow atlas

In CLUSTERED mode a shadowed omni or spot light is clustered too (`shadow_atlas`, on by default): instead of three.c's own shadow map, it gets a tile of one depth atlas, a spot light one tile and an omni light a 3 x 2 block of cube faces. A tile is as big as the light is on screen (its bounding sphere's size in pixels), at most the light's `shadow_size`, and the tiles are packed largest first; when the atlas (`shadow_atlas_size`, 4096 a side) is full, the least important lights get smaller tiles, and then none. A tile is redrawn only when its light moved or a shadow caster inside the light's range changed, and at most `shadow_updates_per_frame` faces (24) are redrawn a frame, the biggest lights first: a street of static lamps costs its shadows once. The light loop filters each light's tile with r186's PCF and the light's `shadow_bias`, `shadow_normal_bias` and shadow intensity. Turn `shadow_atlas` off to keep shadowed lights on three.c's path, as r186 does (8 of a kind at most).

### Sun shadows that reach the horizon: cascades

A DirectionalLight3D with `shadow` and `shadow_cascades` 1 to 4 draws its shadows as three.js r186's `CSMShadowNode`, in either lighting mode:

@include samples/street_sun/assets/main.cwscene#Sun

The view from the camera's near to `shadow_distance` (r186's `maxFar`; 0 is the camera's far) is split by r186's practical split. Each cascade is an orthographic map of `shadow_size` fitted around its slice of the view, with r186's width and texel snapping, so shadows do not shimmer as the camera moves; near shadows get the finest texels and the last cascade reaches the end of the view. Fragments pick their cascade by depth and cross-fade into the next (`cascade_fade`). A cascade is redrawn only when the camera moved it or a shadow caster changed.

`samples/street_sun` is the night street by day: the sun alone, four cascades reaching 250 m down the street. Its `csm-parity` gate checks, on GL and on WebGPU, that the cascades put shadows where one big shadow map over the same ground puts them.

### Terrain under lamps and cascades

A Terrain3D's materials carry their own surface part (`terrain`, `terrainlow`, `terrainfar`), and the cluster and csm parts compose with it (`terrain_cluster_csm`), so lamps light the ground and the sun's cascades fall on it. The terrain also casts into the cascades with its own vertex displacement. Any other extension a material carries keeps it alone, unlit by the parts. `terrain_lights` (terrain_demo's `lights` run) drives the car at dusk past seven clustered lamps with the sun casting four cascades. Its `terrain-lights-parity` gate checks on GL and WebGPU that the clustered lamps light the ground exactly as three.c's own lights do, and that the cascades put the terrain's shadows where one wide shadow map puts them.

The two parts share one uniform group (`lightingFrame`), so a terrain under both still fits WebGPU's four bind groups.

### Quality and costs

| Preset | Lights a cluster keeps | Sun cascades | Shadow maps | Atlas |
|---|---|---|---|---|
| Low | 24 | 2 | 1024 at most, hard | 2048 |
| Medium | 32 | 3 | 1024 at most, soft | 2048 |
| High | 64 | as authored | as authored | as set |
| Ultra | 64 | as authored | twice, up to 4096 (a cascade at most 2048) | as set |

`street_sun_perf` (1920 x 1080, the camera driving, so every cascade is redrawn every frame), ms per frame on the 890M:

| Preset | Cascades | GL frame | GL GPU | WebGPU GPU |
|---|---|---|---|---|
| Low | 2 of 1024 | 1.2 | 0.8 | 0.7 |
| Medium | 3 of 1024 | 1.5 | 1.1 | 0.9 |
| High | 4 of 2048 | 3.6 | 2.9 | 1.8 |
| Ultra | 4 of 2048 | 3.5 | 2.9 | 1.9 |
| High, one shadow map | 1 of 2048 | 1.5 | 1.1 | 1.6 |

Each cascade is at most 2048 (the four share one map twice that wide), so Ultra draws them as High does. A still camera redraws no cascade.

A redrawn atlas tile or cascade is a shadow caster pass over the scene. On the GPU, a shadowed light costs 20 depth reads where its cell is drawn, the sun 10 compare samples per pixel. On WebGPU the atlas keeps an R32Float copy beside its depth (compatibility mode reads depth only through a comparison sampler, which the light loop cannot use), and a redrawn tile is copied into it.

A mesh whose material carries the cluster or csm part is its own draw by default, so three.c's auto-instancing (same geometry, same material) stops for it. `LightingSettings.batching` turns it back on: both parts' hooks fill only per-frame values (`cw_shading_part_batching`, cartwheel/shading.h), so one instanced draw can carry a whole batch. It is off by default because, measured, it does not pay here. In `vis_city_lights` (about 4500 meshes, 64 clustered lamps and four cascades, 1080p, `tests/vis-city-batch.mjs`), batching gives the same picture with 19 draws instead of 891. Submitting the draws costs about the same CPU either way (three.c's GL draws are cheap). The scene pass costs 0.9 ms more GPU because instances lose the front-to-back order and the clustered shading runs on hidden pixels. Native frame time went from 18.3 to 22.1 ms, and Chromium from 34.2 to 39.3 ms. Turn batching on for a frame bound by draw submission, with many small meshes and cheap overdraw.

The same city shows where a clustered frame's CPU time goes. Cascades the moving camera redraws every frame are caster passes over every mesh (`lighting.csm`, 8 to 14 ms a frame on the 890M under load). Binning the lamps takes about 1 ms (`froxel.bin`). Keeping every material's parts current takes 0.5 to 1 ms for 4500 meshes (`lighting.materials`), now a lookup per material.

Light probes, reflection probes and screen-space light are in [Reflections and bounce light](#reflections-and-bounce-light).

## Graphics settings and presets

[cartwheel/quality.h](api:cw_quality_get) has five settings a player can pick one by one, and four presets that pick them all:

| Setting | Low | Medium | High (default) | Ultra |
|---|---|---|---|---|
| Resolution | 67% | 75% | 100% | 100% |
| Ambient occlusion | Off | On | On | On |
| Shadows | Low | Medium | High | Ultra |
| Anti-aliasing | FXAA | FXAA | SMAA | SMAA |
| Effects detail | Low | Medium | High | Ultra |

- **Resolution** (50%, 60%, 67%, 75%, 85%, 100%) is the biggest lever: the 3D scene and its post render at that share of the output and are scaled up with a Catmull-Rom filter, while the 2D window passes (HUD, menus, text) draw over it at the output's full size. A scene's GPU time goes with its pixel count, so 67% costs about 45% of 100%. It applies on top of `cw_app_desc.render_scale`; an app with `pixel_scale` keeps its own size. Draw the 3D scene before the first 2D window pass of a frame: that pass is where the scene is scaled up.
- **Ambient occlusion**: GTAO as the PostProcess asks for it, or none.
- **Shadows**: Low (maps of 1024 at most, hard PCF), Medium (1024 at most, soft), High (as the lights ask), Ultra (twice that, up to 4096).
- **Anti-aliasing**: Off, FXAA, or SMAA (as the PostProcess asks).
- **Effects detail**: the share of GPU particles drawn (`cw_quality_scale("particles")`; all are still simulated), the density games thin their instancing by (`cw_quality_density()`: grass, crowds, scatter), the bloom chain's length, GTAO's samples, the LOD error budget (Low and Medium pick coarser LODs sooner) and the volumetric fog grid.

High draws what the scene's PostProcess and lights ask for. Nothing here reaches the simulation.

A game adds its own render-only settings with `cw_quality_lever_add` (a name, its values' labels, the value each preset picks); they show after the engine's, change with the presets and are saved with them. Re-apply them, and re-thin instancing, from a `cw_quality_on_change` callback.

A scene that does not need everything asks for less while it is on screen with a quality scope: `cw_quality_scope_push("lagoon", caps)` takes a cap per setting (the highest value allowed, `-1` leaves a setting alone; `caps[CW_GFX_SSAO] = 0` draws the scene without ambient occlusion) and `cw_quality_scope_pop()` ends it. A scope only ever lowers what the player chose, so a player on Low stays on Low and the graphics menu still shows the player's own choice; `cw_gfx_effective(setting)` reads a setting as it is drawn now. Scopes nest (four at most) and the lowest cap wins. A game that switches between a cheap 2D scene and an expensive 3D one pushes a scope for the 3D scenes that can do without something and pops it when it leaves. In a test, `quality scope push ssao=0` and `quality scope pop` do the same from the debug channel.

The player picks from the graphics page ([cartwheel/quality_menu.h](header:quality_menu.h)), which the game shell opens from its Settings and a game opens from its own pause menu: a row for the preset (Low, Medium, High, Ultra, Auto; Custom once a setting was changed on its own), one per setting with its value (the resolution with the pixel size it renders at), the game's own, and Back. Keep the choice in the save with `cw_quality_saved` / `cw_quality_restore` (13 bits for a game with no settings of its own, 21 with four).

**Auto** holds the frame's cost under a budget. While the game says it is in play (`cw_quality_auto_measure(true)` every frame), the engine averages the cost: the GPU time of the frame's passes where the host times them cheaply (native WebGPU's timestamps), else the frame-to-frame time (GL's timer queries would cost the CPU a driver sync every frame; a cart has no GPU timers). After a couple of seconds over budget it steps down a ladder that gives up resolution first (High 100%, High 85%, Medium 75%, Medium 67%, Low 67%, Low 60%, Low 50%); where it measures GPU time it also steps back up when the cost stays well under budget, never into a level that was over it in this run. `cw_quality_auto(true, budget_ms)` makes Auto a game's starting choice (19 ms of frame time by default, three quarters of that of GPU time). The level it reached is saved with the choice.

A deterministic run starts at High whatever the save holds, with Auto off, so every host draws the same frame; a choice made in it from the menus (input every host replays alike) applies. `CW_QUALITY=low` (native) fixes a preset to compare them; the debug command `quality [low|medium|high|ultra|auto]` switches one live and `quality set <setting> <value>` one setting (`resolution 0-5`, `ssao 0-1`, `shadows 0-3`, `aa 0-2`, `effects 0-3`, or a game setting's name); `quality_menu 1` opens the graphics page.

## Sky and fog

[environment.h](header:environment.h) adds these node types (FogVolume3D under "Volumetric fog" below):

- **ProceduralSky** ray-marches single scattering (Rayleigh and Mie) into a cube map on the GPU. The sun's position comes from `time_of_day`, `latitude` and `declination`; `day_speed` runs the day cycle on the fixed tick. It drives a DirectionalLight3D (`sun`): direction, colour (reddened at sunset) and energy (zero below the horizon). The cube is the background and, PMREM-filtered, the image-based light for Standard materials. It is re-rendered only when the sun moves more than `env_update_angle`.
- **HeightFog** is exponential height fog integrated in closed form along each view ray, in the EFFECTS phase. It can take its colour from the sky's horizon and adds a glow toward the sun.

The showcase's environment is all scene-file properties:

@include samples/showcase/assets/main.cwscene#Env..Fog

Samples: `samples/sky_day` (noon, afternoon, sunset, night, then the running cycle, over balls from mirror chrome to rough paint), `samples/fog_valley` (analytic fog with the sun lobe, dense ground fog, fog off). The debug command `environment` reports the sun, sky updates and fog.

### Volumetric fog

Analytic fog is the same colour everywhere at a given depth: it cannot show a beam of light. Set `volumetric = true` on the HeightFog and the fog near the camera is lit for real: the engine divides the view into a 3D grid of cells that follows the camera (more slices close up), works out how much fog each cell holds and how much light reaches it, and adds up what each pixel sees through the cells in front of it. Each light reaches the fog through its own shadows, so sunlight through a window becomes a shaft with the window bars in it, and a spot light draws its cone with the shadows of whatever stands in it.

- Which lights: every light with `fog_energy` above 0 (the default is 1; raise it for a stronger beam, set 0 to keep a light out of the fog), at most 8. The first shadowed directional light and up to three shadowed spot lights get shadow maps of their own, fitted to the part of the scene the grid covers; omni lights light the fog without shadows.
- Light from everywhere else is the fog's colour (or the sky's horizon with `use_sky_color`) times `volumetric_ambient`. Indoors, set a dark colour so the beams stand out.
- `volumetric_distance` (64) is how far the grid reaches. Past it the analytic fog carries on, lit to match, so there is no seam. A shorter reach puts the cells closer together: sharper beams.
- `volumetric_albedo` tints the light the fog scatters; `sun_g` is how strongly it scatters forward (toward the camera when you look at a light). `sun_scatter` applies to the analytic fog only.
- `volumetric_temporal` (0.9) blends each frame with the previous ones. Each frame samples its cells at slightly different points, so the blend smooths out the steps between cells. A fast-moving light leaves a short trail; 0 turns the blend off.
- Each cell holds at most 60000 of light and an extinction of 64 per unit (the grid is half floats), and a value that is not a number becomes 0. A light too strong for the grid (a flare with a huge `fog_energy`, say) is capped where it touches the fog, and one bad frame of input (a light or camera at a position that is not a number) is dropped. The blend also takes in at most 8 times what a cell holds now, so a broken cell, or a light switched off, fades in a frame or two; without these, one overflowed or broken cell would be carried by the blend into every later frame and spread until the whole view was flat bright fog.

**FogVolume3D** places fog in one region: a `BOX` or `ELLIPSOID` of `size` (moved, turned and scaled with the node), or `WORLD` for everywhere. It has its own `density`, `albedo` (its colour under white light), `emission` and `emission_energy` (it glows, lighting nothing else), `height_falloff` (thinner higher up), `edge_fade` (soft edges) and `noise`, `noise_scale` and `noise_wind` (wisps that drift). Volumes add to each other and to a volumetric HeightFog. They work without one, but then they only show where a light reaches them or they emit. At most 16 volumes count.

```text
[node name="Fog" parent="." type="HeightFog"]
volumetric = true
density = 0.1
height_falloff = 0
use_sky_color = false
color = color(0.02, 0.025, 0.035, 1)
volumetric_distance = 30

[node name="Mist" parent="." type="FogVolume3D"]
shape = ELLIPSOID
position = vec3(-3, 0.4, -8.5)
size = vec3(7, 1.6, 3)
density = 0.5
noise = 0.7
noise_wind = vec3(0.4, 0, 0.1)
```

The cost follows the **Effects detail** setting:

| Effects detail | Low | Medium | High | Ultra |
|---|---|---|---|---|
| grid (cells across x slices) | 64 x 32 | 128 x 64 | 160 x 64 | 240 x 96 |
| shadow map per light | 256, the sun only | 512 | 1024 | 1024 |

Historical measurements before v0.14.1 (Medium used 96 x 48 and reconstruction used linear filtering), on an AMD Radeon 890M (integrated) with `fog_shafts_perf` (the sample's room, two shadowed lights and the mist, a 1920 x 1080 frame), the GPU time of the fog's own passes (building the grid, then applying it to the image), in ms per frame:

| Effects detail | Low | Medium | High | Ultra |
|---|---|---|---|---|
| native, GL | 0.29 | 0.45 | 1.22 | 2.61 |
| native, WebGPU | 0.15 | 0.28 | 0.64 | 1.83 |
| Vega 8 estimate, GL | 0.7 to 0.9 | 1.1 to 1.4 | 3.0 to 3.7 | 6.5 to 8 |
| Vega 8 estimate, WebGPU | 0.4 to 0.5 | 0.7 to 0.9 | 1.6 to 1.9 | 4.6 to 5.5 |

The WebGPU rows leave out the fog's shadow maps for its lights, which draw the scene again from each shadowed light (a scene of a few dozen draws here). In Chromium on WebGPU, High costs about 0.7 ms more GPU time per frame than the same scene with the fog analytic and the mist hidden, shadow maps included. Chromium on GL has no GPU timers, so there is no column for it. The grid is mostly bound by memory and fill, so on a Vega 8 expect about 2.5 to 3 times the 890M's time, as in the estimate rows. These historical timings are not measurements of the denser Medium grid or cubic reconstruction introduced in v0.14.1.

The number of cells down the screen follows its shape (90 at High on a 16:9 screen). Fog reconstruction uses cubic filtering across the screen to soften cell edges. Volume `noise_wind` is a world-space velocity in metres per second, independent of `noise_scale`. Volumetric fog needs a perspective camera. `cw_volumetric_fog_get_state` reports what the fog did last frame: the grid, the lights, the shadowed lights and the volumes.

A wall thinner than a cell can let a little of the light behind it into the cell in front of it. Keep sunlit air behind thin walls out of the grid's reach, or make the walls thicker than a cell is deep at that distance.

Sample: `samples/fog_shafts` (a dim room: sunlight through a cross-barred window, a spot light over a pillar, a pool of mist; then the same fog analytic; then the spot light swung).

## Baked lighting

[lightmap.h](header:lightmap.h) is an offline CPU lightmapper. MeshInstance3D nodes with `gi_mode` STATIC get a generated second UV set and a lightmap of their own holding the indirect light: occluded sky and `bounces` bounces of every light with `bake`. Direct light stays real time, so lights can still change and moving objects still cast shadows. The bake also stores a grid of L2 spherical-harmonic probes for moving objects and DYNAMIC meshes, for three.js r186's LightProbeGrid.

The bake is deterministic: a BVH with median splits, per-texel random sequences seeded from (seed, mesh, texel), scalar IEEE math with the `cw_` trig functions. The same scene gives the same bytes on every host and any thread count, and `cw_lightmap_hash` is the fingerprint.

The LightmapGI node bakes on its first frame or loads `data`, a `.cwlm` file. A bake is synchronous, so ship the file: `cwbake` loads a scene without a window or GL, bakes with its LightmapGI node's settings and writes the bytes the node would have baked.

```sh
build/native/cwbake samples/lightmap_room/assets/main.cwscene out/room.cwlm
```

```text
{"hash":"a132d3c3d6f9077c","meshes":10,"triangles":120,"texels":52864,"probes":320,"invalid_probes":3,"rays":2711487}
```

`samples/lightmap_room` bakes the same room at runtime, twice, and publishes the hash as `lm.hash_hi` / `lm.hash_lo`; its gate (`lightmap-bake`) requires the same value on every host. A ball rolls through it.

## Reflections and bounce light

[gi.h](header:gi.h) adds two node types that make a scene light itself, both captured from the scene on the GPU, on GL and WebGPU, natively and in carts:

- **ReflectionProbe3D**: shiny things reflect their real surroundings instead of the sky. A probe renders the scene around a point into a prefiltered radiance map, three.js r186's `PMREMGenerator.fromScene` with its `size`, `position` and `renderTarget` options, and every lit surface in its box reflects that. With `box_projection` on, reflections are traced to the box's faces (r186's `getParallaxCorrectNormal`), so a polished floor shows the wall where the wall is: use it in rooms and corridors and size the box to the room, a little larger so the floor and walls sit inside it. `origin_offset` moves the capture point off the box centre (out of a pillar, up from the floor) and the projection still lines up. Overlapping probes blend as Godot's do: inside its box a probe's weight falls to 0 over `blend_distance` at the faces, the probes' reflections are averaged by weight, and the sky fills the rest. Each object takes the two probes whose boxes reach it. `ambient` makes the probe the surface's diffuse ambient light too, in place of the sky's.
- **LightProbeGrid3D**: bounce light, three.js r186's `LightProbeGrid`. A grid of probes over a box, each rendering the scene into six small faces (`cube_size`, 8 pixels by default) that are projected onto L2 spherical harmonics on the GPU. Every lit surface inside the box adds the grid's irradiance at its position: a red wall tints the floor and ceiling next to it, a lit floor lifts the underside of a table. The grid bakes at load (`bounces` extra sweeps, each one more bounce, since each sweep sees the light the last one stored); `update_mode = INCREMENTAL` then refreshes `probes_per_frame` probes every frame for lights that move. Like r186's it has no visibility term, so light leaks through walls thinner than the probe spacing: keep a grid inside one room, or keep the walls thick.

The room in `samples/gi_room` has both:

@include samples/gi_room/assets/main.cwscene#RoomProbe..Bounce

How it fits together:

- Captures run in the SHADOW phase (the "gi" pass, after the sky's environment is made), so this frame's scene draws with them. Each probe keeps two targets, drawn into one while the scene samples the other, and nothing is allocated after the first capture. `update_mode` picks when a probe captures: ONCE (its first frame, and again when the sky is re-rendered), ON_DEMAND (`cw_reflection_probe_update` or `call <probe> update`), or EVERY_N_FRAMES (`update_every`). A capture sees what `cull_mask` lets it see (three.c object layers): give a cheap stand-in of a level a layer the camera does not draw and the probe captures the stand-in.
- The shading is r186 TSL in a three.c extension, the "gi" shading part ([shading.h](header:shading.h)): every Standard and Physical material in a tree with a probe or a grid carries it, and the app's program tables need its states (`standard+senv+l1101+xgi`; `tools/programs.mjs` adds them like any other). A material carries more than one engine part this way, composed by name (`xterrain_gi`). three.c never auto-instances a mesh whose material carries an extension, so each such mesh is a draw of its own.
- One LightProbeGrid3D lights a tree (the first one). Probe boxes are axis aligned: a node's rotation is ignored, its scale applies.
- Everything here is render only: nothing reaches the simulation, and `samples/gi_probe_check` and `tests/gi-probe.mjs` check the pictures against known answers (a mirror ball in a box of six coloured walls must show each wall where it is).

Quality (effects detail):

| | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Probe capture size | half of `resolution` | half | as authored | twice (up to 512) |
| EVERY_N_FRAMES probes refresh | a quarter as often | half as often | as authored | as authored |
| Captures a frame | 1 | 1 | 2 | 4 |
| Grid refresh (INCREMENTAL) | the bake only | half of `probes_per_frame` | as authored | twice |
| Grid face size | at most 8 | as authored | as authored | as authored |
## Blocking out levels with CSG

[csg.h](header:csg.h) is for laying out a level before any art exists. You build rooms, doors, pillars and ramps out of boxes, cylinders and spheres that add to or cut from each other, then bake them into one mesh.

The node types are `CSGBox3D`, `CSGCylinder3D`, `CSGSphere3D` and `CSGCombiner3D` (a group with no shape of its own). Each has an `operation`: UNION adds it, SUBTRACTION cuts it out, INTERSECTION keeps only the overlap. The topmost CSG node of a branch is the root. It starts from its own shape, then applies its CSG children in order, each with its own operation, and a child with CSG children of its own is built the same way first. A room is a box with a slightly smaller box subtracted; a door is one more subtracted box inside it:

@include samples/csg_greybox/assets/main.cwscene#Level..Window

Colours are per shape. A face that a subtracted shape cuts takes that shape's colour, so the inside of a door frame can differ from the wall. The root holds the surface settings for the whole result: `roughness`, `metalness`, `cast_shadow`, `receive_shadow`.

The booleans never run in your game. The `cwcsg` tool loads the scene, runs every root through [Manifold](https://github.com/elalish/manifold) (robust booleans that always give a closed mesh) and writes the root's `bake_file`. The game just loads that file, so every host draws exactly the same triangles:

```sh
build/native/cwcsg samples/csg_greybox/assets/main.cwscene
```

```text
{"root":"Level","file":"level.cwcsg","written":true,"shapes":10,"triangles":568,"vertices":1258,"bytes":62216,"hash":"b73b394b87e778db","input_hash":"e1cc582bc8a2a3a5","ms":3.96}
{"root":"Sketch","skipped":"no bake_file"}
```

`--assets DIR` names the asset root `bake_file` is relative to (the scene's folder by default). `--watch` bakes again every time you save the scene. `--check` writes nothing and fails when a bake file is missing or out of date, which suits a build step or CI. The same scene always gives the same bytes: positions snap to 0.1 mm, faces are flat shaded, and triangles and vertices are written in a sorted order. Round shapes use exactly `sides` (cylinders) or `segments` (spheres) facets.

While you lay out, you do not need to bake after every change. A root without a bake file draws a preview: each shape as it is, unioned shapes solid, subtracted shapes as red see-through ghosts and intersected shapes as blue ones. You see where the cuts go; the cuts themselves appear after a bake. Set `ghosts = false` on the root to hide them. If you change the shapes and forget to bake, the root keeps drawing the old file, reports `stale`, and logs a warning naming the fix.

For collision, give a StaticBody3D a CollisionShape3D with `shape = TRIMESH` and `mesh` pointing at the root. It collides with the baked mesh (the preview has no collision):

@include samples/csg_greybox/assets/main.cwscene#Collision

Costs: a baked root is one mesh and one draw call. The preview is one draw per shape. `samples/csg_greybox` (the room above, a pillar, a box rounded by intersecting it with a sphere, a ball that lands on the baked floor, and one unbaked root showing the preview) bakes in about 4 ms and costs about 0.4 ms a frame on a Radeon 890M at 1920x1080 (native: 0.38 ms on GL, 0.37 ms on WebGPU, almost all of it GPU; Chromium: 0.15 ms on GL, 0.41 ms on WebGPU). A Vega 8 should stay under 1.5 ms. CSG has nothing to scale, so the quality presets leave it alone. Its gates are `csg-greybox` (the file's and the loaded mesh's hashes are equal on every host) and `csg-tool` (two bakes are identical, and a changed operation bakes other bytes and reads as stale).

Screen-space reflections and bounce light are PostProcess settings on top of the probes, three.js r186's `SSRNode` and `SSGINode` at half resolution:

- **`ssr`**: one mirror ray a pixel is marched across the screen against the depth (r186's first-generation SSR, with its binary refinement), so a polished floor shows the chair standing on it, which a probe captured from elsewhere cannot. Metalness weights it, roughness blurs it, and it is added over the image as r186's example adds it. `ssr_max_distance` is how far a reflection reaches (view units, 1 by default: set it to the room's size), `ssr_thickness` how deep a surface counts as hit, `ssr_quality` the march steps per pixel of the ray. What is off screen cannot be reflected: the probes still cover it.
- **`ssgi`**: r186's SSILVB (a port of SSRT3): per pixel, a few slices of the depth around it are walked and every newly occluded sector of each slice takes the light of the surface occluding it, giving bounce light and ambient occlusion from what is on screen. It is composited as r186's example does, colour x AO + diffuse colour x bounce light, and denoised over 5 x 5 trace texels with depth- and normal-aware weights (r186 relies on TRAA; with the pipeline's TAA on the slices also rotate frame to frame as r186's do).

Both read a G-buffer the "gi" pass draws after the probes: the scene a second time at half resolution with a program of its own (the "gibuf" shading part) that writes the view normal, roughness and metalness instead of the lit colour and does no lighting (and, for SSGI, a third time for the diffuse colour). Only surfaces with the part (every Standard and Physical material of a tree with a probe, a grid, SSR or SSGI) are in it; anything else is not reflected and gets no bounce light.

| | Low | Medium | High | Ultra |
|---|---|---|---|---|
| SSR | off | half the march steps | as authored | 1.5 x the steps |
| SSGI | off | off | off | as authored |

The cost, GPU ms per frame on an AMD Radeon 890M (integrated) at 1280 x 720, `samples/gi_room` through `tests/gi-perf.mjs` (GL, the probes' pass averaged over frames with and without a capture; measured on a quiet machine (load 3)):

| Preset | gi pass (probes, grid, G-buffer) | SSR | SSGI | Whole frame |
|---|---|---|---|---|
| Low | 0.3 | off | off | 1.6 |
| Medium | 0.7 | 0.5 | off | 2.3 |
| High | 1.4 | 1.2 | off | 4.2 |
| Ultra | 3.7 | 1.4 | 6.5 | 14.2 |
| No probes, grid, SSR or SSGI (High) | | | | 1.8 |

The gi pass on High is two 128-pixel probe captures every other frame and the half-resolution G-buffer; Ultra doubles the capture size and budget and draws the G-buffer twice (SSGI needs the diffuse colour). Most of SSGI's time is the trace's fetches of the lit colour and the normal at the scattered steps, so it reads the colour from a half-resolution copy and the normals from a two-channel one, and its loops have fixed counts the compiler unrolls. A Vega 8 has about half the 890M's shader rate: expect about twice these numbers, so Low to High fit a 60 Hz frame there and Ultra (SSGI above all) is for discrete GPUs.


## Characters: skin, glTF materials, faces

Three pieces make characters look like people rather than painted plastic. `samples/character_skin` shows all of them on six instances of one generated glTF.

**Skin shading** ([skin_shading.h](header:skin_shading.h)) is a cheap subsurface look. It adds two terms to a MeshStandardMaterial or MeshPhysicalMaterial for every light that reaches it (the scene's lights, clustered lights, the cascaded sun), and keeps the rest of the material: sheen, clear coat, maps.

- **Wrap**: diffuse light reaches past the line where the surface turns from the light, red furthest, so the shadow side of a face goes soft and warm instead of grey. `scatter` is the colour that travels furthest; `wrap` how far (0 to 1).
- **Translucency**: light from behind shows through thin parts (ears, nostrils, fingers) as a warm glow, strongest when looking toward a light past an edge (three.js r186 MeshSSSNodeMaterial's term). `translucency` sets its strength, `distortion` how much the surface bends it, `power` how tightly it gathers.

There is no screen-space pass, no extra target and no texture: a few instructions per light per skin pixel. A light's shadow darkens both terms like the rest of its light, so the glow through an ear needs a light the head does not shadow (a rim light behind the character).

Turn it on from code with [cw_skin_shading_apply](api:cw_skin_shading_apply) on a material (or [cw_skin_shading_apply_named](api:cw_skin_shading_apply_named) for every material of that name under a model), or from the file: a glTF material with `KHR_materials_diffuse_transmission` (its factor becomes the translucency, its colour the scatter colour) or with `"extras": {"skin_shading": true}` (or an object with `scatter`, `wrap`, `translucency`, `distortion`, `power`, `ambient`) gets it when it loads through the resource system. The Low preset draws skin as plain PBR (`cw_quality_scale("skin")` is 0 there) and pays nothing; Medium and up draw it.

**glTF materials** load as r186's GLTFLoader builds them: each keeps its name (`t3_material.name`), and the factors of `KHR_materials_sheen`, `clearcoat`, `specular`, `ior`, `transmission`, `volume`, `emissive_strength`, `iridescence` and `anisotropy` make a MeshPhysicalMaterial (the clear coat normal texture too; the other extension textures are not read yet). A model generator can put cloth sheen, leather clear coat or eye gloss in the file and the game finds a material by name, with no node naming tricks.

**Faces** use morph targets on the skinned head: see [Animation](guide:animation), "Faces: morph targets". **Many characters** share one load: [cw_gltf_instantiate](api:cw_gltf_instantiate) gives each its own skeleton, animation and morph weights over the same geometry, materials and textures.

Cost on a Radeon 890M (native, the sample at 960 x 540 drawn at 1920 x 1080, two directional lights, one with shadows; GPU ms per frame, six runs each, skin on and off interleaved):

| Scene | GL | WebGPU |
|---|---|---|
| one face filling the frame, skin shading | 0.54 | 0.41 |
| the same, plain PBR | 0.52 | 0.43 |
| 6 characters (67 draws) | 0.52 | 0.40 |
| 20 characters (197 draws, 243k triangles, morphed faces), skin shading | 0.75 | 0.63 |
| the same, plain PBR | 0.70 | 0.62 |
| 20 characters, Low preset | 0.41 | 0.39 |

Skin shading costs less than the run-to-run noise (under 0.05 ms) even with a face filling the screen. A slower integrated GPU (a Vega 8 is about a quarter of an 890M) should stay under 0.2 ms for it. The 20 characters cost about 0.9 ms of CPU per frame on GL (skinning matrices and draw submission).

### Rim light: telling teams apart at range

A **rim light** ([rim_light.h](header:rim_light.h)) is a coloured glow along a surface's silhouette, where it turns away from the camera. It is added to the material's emissive light, so it shows in shadow and at night and bloom picks it up, and it fades in with distance: below `near` metres it keeps `close` of its strength, past `far` all of it. A fighter 30 m away reads as a red or blue outline against a dark wall; one at arm's length is not painted over. It also makes a "spawn protected" shimmer (set the strength every tick, it is a uniform), a pickup highlight or a menu selection glow.

```c
cw_rim_light_desc d = cw_rim_light_default();   /* red, strength 2, power 3, fading in from 6 m to 20 m */
d.color = (t3_color){ 0.25f, 0.55f, 1.0f };     /* the blue team */
cw_rim_light_apply_tree(model_root, &d);         /* every material under a model */
...
cw_rim_light_remove_tree(model_root);            /* before releasing the model: the rim keeps its materials */
```

The rim is per material: glTF instances share their file's materials, so clone one per character for a per-character colour (MeshInstance3D primitives of one look share one material in a tree too). It works with every other part (clustered lights, the cascaded sun, skin shading, GI) and costs a few instructions per pixel of the material; shadow casters are untouched and every preset draws it. `samples/rim_light` shows two lines of figures from 3 m to 40 m with red and blue rims and a pulsing one; its gate is `rim-light`.

## Visibility: culling, LOD and impostors

Without a VisibilityManager, three.c's per-object frustum test is all there is. Add one ([visibility.h](header:visibility.h)) and the "visibility" pass culls, picks LODs and bakes impostors before the scene draws; culled meshes are hidden for the frame and restored in PRESENT (and after each view of a split screen, so one player's culling never hides meshes from the next view's shadows, GI or culling), so game code never sees the flags change.

- **Frustum culling** walks a BVH over `static` meshes four boxes at a time, plus a flat SIMD pass over moving ones. Shadow casters outside the view are swept along the light so their shadows stay.
- **Occlusion culling** queries each candidate's bounding box against the depth the scene just drew and uses the answer a frame later. Boxes are inflated by the camera's motion over that latency, and new or late objects count as visible, so nothing pops in. The guard behind that is conservative: while the camera is moving and within a few steps of a drawn static box, no queries are issued at all, so a fast camera skimming past props gets frustum, distance and portal culling only (see `occlusion_guard` in visibility.h).
- **Portals**: Room3D boxes joined by Portal3D openings; from inside a room you see other rooms only through portals, each narrowing the view. A disabled portal is a shut door.
- **LOD** picks the coarsest level whose simplification error stays under `lod_error_px` on screen: cwpack LOD chains for meshes and glTF, built-in tessellations for spheres and cylinders. `visibility_range` hides a mesh beyond a distance.
- **Impostors**: past `impostor_distance` a mesh draws as an octahedral impostor (64 views baked into an atlas on first use, shared by identical instances), crossfaded over `impostor_fade` metres.

Costs per frame: the BVH walk, one pass over moving meshes and one over visible ones, one query draw per queried mesh, one instanced draw per impostor atlas; the first use of an atlas renders the mesh 64 times.

`samples/vis_city` is about 4500 meshes (buildings, lamps with visibility ranges, LOD'd trees with impostors, houses with portal doorways) walked by a camera; its `check` mode renders a second, unculled copy of the city and counts pixels that differ, so a cull that pops fails the gate. `samples/vis_impostor` sweeps the impostor distance across a grove and measures the largest frame-to-frame change. The showcase also uses three.c's own `LOD` objects directly for its terrain chunks (`samples/showcase/world.c`).

## Render layers and a first person weapon

Every `Node3D` has `layers`: a bit mask, bit n = layer n, 1 by default, -1 for every layer (lights are on every layer: a camera's layers pick what it draws, never what is lit). Setting it applies to the node and everything under it, including nodes added later, unless they set their own. A `Camera3D` draws the layers in its `cull_layers` (1 by default).

A first person weapon needs two things a plain camera cannot give: its own field of view (at 90 degrees a gun held with the world's lens stretches into the screen's corner) and no clipping into walls. Put the weapon on layer 2 and give the camera `viewmodel_layers = 2` and a `viewmodel_fov` (50 to 60 reads well):

```c
cw_node_set(gun, "layers", cw_val_int(2));
cw_node_set(camera, "viewmodel_layers", cw_val_int(2));
cw_node_set(camera, "viewmodel_fov", cw_val_float(52));
```

After the scene, the `viewmodel` pass draws those layers with a second camera at the same pose and the viewmodel lens, over the same colour and depth. It draws in the near end of the depth range, so the weapon always lands in front of the world and sorts against itself, while the scene's depth stays right everywhere else for the passes that read it (ambient occlusion, decals, particles). The weapon is lit by the scene's lights and shadows, and costs one more scene submission of the few objects on those layers (a split screen pays it once per view). The pass runs on GL and on WebGPU. The background is not redrawn.

In a split screen, hide the other players' weapons for each view with the pipeline's view hook (`cw_pipeline_set_view_hook`): each view draws every object on its layers.

## Particles and decals

[particles.h](header:particles.h) simulates GPUParticles3D on the GPU with transform feedback, one step per fixed tick. Emission is a pure function of the particle index and sim time, with random values hashed from (seed, index, cycle), so the same seed and tick count give the same particles on every host. Shapes, velocity cones, gravity, damping, curl-noise turbulence, attractors, analytic colliders and collision with the depth buffer (only what the camera sees), billboards, stretched sprites or instanced meshes, flipbooks, colour and size curves, soft particles, a GPU bitonic sort for alpha blending, and per-particle trails. Particles draw unlit in the "particles" EFFECTS pass; `energy` above 1 blooms (the scene is HDR). Costs: one transform feedback draw per system per tick, one instanced draw per system per frame, plus the sort passes when `sort` is on. GPUParticles2D runs the same emission model on the CPU for 2D (see [2D rendering](guide:rendering-2d)).

[decals.h](header:decals.h) projects Decal3D images along the node's -Z onto whatever the scene drew into depth: steps, slopes, spheres, props. MIX paints over the lit scene (signs, paint), MULTIPLY keeps lighting correct (dirt, scorch, cracks), ADD glows. All decal images share one 2048 x 2048 atlas, and `region` picks a cell of a sheet. Decals draw as instanced boxes; the GPU cost is the screen area those boxes cover. They land on anything in depth, including characters that walk through the box, so keep boxes shallow; they never land on transparent surfaces, particles or the sky.

### Decals that stick: mesh mode

Set a Decal3D's `mode` to MESH for decals that belong to an object rather than to the screen: bullet holes, stickers, a logo on a car, blood on a character, a crack in a window. When the decal spawns, the engine cuts the triangles of the meshes under its box out on the CPU and keeps them as a small mesh attached to each of those meshes, so the decal moves, turns and hides with them. On a skinned character the decal is cut from the pose of that moment and then bends with the skeleton: a sticker on an arm swings with the arm. Mesh decals also land on transparent surfaces such as glass, which the screen-space ones never reach. They land on lit surfaces under clustered lighting, cascaded sun shadows and GI as well (only surfaces whose own shader moves their vertices, terrain and water, are skipped). A MIX mesh decal on a lit surface (anything with a lighting model: Lambert, Phong, Standard, Physical) is lit like the surface under it: it takes the same lights, sun and its shadows, the same indirect light and the same fog, in forward and in clustered lighting, on rigid and skinned meshes and on glass. A sticker in a dark corner is dark, one under a lamp is bright, one in the shadow of a roof is shadowed; the decal reads the shadows the surface reads (turn on `receive_shadow` for a mesh that should take the sun's). The decal is a matte surface of its own colour with no shine. MULTIPLY decals scale the lit picture that is already there, so they follow the light without any shading of their own, and ADD decals are glows, so they stay unlit by design. A decal on an unlit (basic) material stays unlit too. Lit mesh decals draw after glass as before. The one cost is shading: a lit decal piece costs about what a mesh of the same size on that surface costs, because it runs the same lighting, so keep boxes small and prefer `max_mesh_decals` for bullet holes. `samples/fx_lit_decals` shows the three places and a lit badge on a skinned character.

Which meshes it lands on: give `target` a node path (`"../Hero"`) to stick to that node and everything under it, or leave it empty to use every mesh whose surface crosses the box. Instanced meshes (forests, particles), Terrain3D and lines are never used. Faces turned away from the projection are skipped, so a deep box through a character marks only the side it faces.

The cut happens once, when the decal enters the tree, and again when you change its box (size, its own position, rotation or scale, the target, the image or the fades) or call `cw_decal_restamp`. After that a mesh decal costs nothing on the CPU and one small draw per mesh it landed on. `max_triangles` caps one decal (1024), and `mesh_offset` lifts it off the surface (2 mm) so it does not flicker against it.

For bullet holes and footprints, spawn decals and forget them: give each a `max_mesh_decals` above 0 and the tree keeps that many, freeing the oldest when one more lands. Decals with `max_mesh_decals` 0 (the default) stay until you free them.

@include samples/fx_mesh_decals/main.c#holes

The cut is the same on every host, bit for bit, so a replay or a networked game shows every hole in the same place.

Samples: `samples/fx_particles` (a fountain colliding with depth, sparks, sorted smoke, mesh debris, an attracted ring, mesh-surface emission, a Trail3D ribbon), `samples/fx_collide` (particles coming to rest on a table they can see), `samples/fx_decals` (scorch over steps, an arrow over a ramp, cracks wrapping a sphere, sheet splats, 240 culled splats), `samples/decal_walls` (about two hundred stains from one sheet on the faces of a side-on cliff, through the full post stack; GL and WebGPU draw the same picture), `samples/fx_mesh_decals` (mesh mode: a badge and paint on a walking character, a crack on a glass pane, a glow on a spinning crate, pooled bullet holes; `fx_mesh_decals_lit` is the same scene under clustered lighting), `samples/fx_lit_decals` (stickers in the dark, under a lamp and in the sun, and badges on two characters).

## The showcase

`samples/showcase` puts it all in one scene: a 480 m valley with terrain LOD, about 1400 instanced trees, a village, 128 lamp posts that come on one by one at dusk, glowing fireflies and a campfire (the campfire and the 7 lit lamps nearest the view are real lights: three.c lights with at most 8 point lights), four skinned walkers, the procedural sky through a day cycle, height fog, campfire and chimney particles, decals, a VisibilityManager, and the post stack (GTAO, TAA, bloom, AgX, SMAA, vignette). Its gates capture morning, golden hour, dusk, night and the running cycle on every host.

```sh
node tools/run-cart.mjs --host native --app showcase --frames 230 --state out/showcase.json --shot out/showcase.png
```

Frame 230 is 22:00: the sun is below the horizon, and the lamps, windows, fire and smoke carry the image. `out/showcase.json` holds the named debug state (`sc.lamps`, `sc.particles`, `sc.vis`) and per-pass GPU times.

## Where to go next

- [pipeline.h](header:pipeline.h) for the pass context and every post setting, [environment.h](header:environment.h), [lightmap.h](header:lightmap.h), [visibility.h](header:visibility.h), [particles.h](header:particles.h), [decals.h](header:decals.h) for each module's model and costs.
- [2D rendering](guide:rendering-2d) for the 2D pass that the "canvas" overlay draws, and [Profiling](guide:profiling) for reading the per-pass GPU times.
