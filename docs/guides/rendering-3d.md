# 3D rendering

{{name}} draws 3D through three.c, three.js r187 in C, and wraps it in a per-tree pass graph that adds a linear HDR workflow, sky, fog, baked lighting, culling, particles and decals. This page explains how those pieces fit together, what each costs and which sample shows it.

## The layers

There are three layers, and you can work at any of them:

- **three.c** is the renderer. Its names follow three.js: `new THREE.Mesh(geo, mat)` is `t3_mesh_new(geo, mat)`, `renderer.render(scene, camera)` is `t3_renderer_render`. Ownership is reference counting. It draws with the shader programs three.js r187's node renderer generates, so its materials, glTF, skinning, morph targets, `LOD`, `InstancedMesh` and render targets render what r187 renders. The programs come from program tables: three.c's core table for the renderer's own passes, and the app's tables for its materials (see [Program tables](#program-tables)).
- **The scene tree** ([render3d.h](header:render3d.h)) owns one three.c scene and renderer per tree. Every `Node3D` drives a three.c object (a group by default; `cw_node3d_object` returns it, `cw_node3d_set_object` replaces it), so you can hang any three.c object under a node and the tree keeps its transform, interpolated between fixed ticks.
- **The render pipeline** ([pipeline.h](header:pipeline.h)) is a small frame graph per tree. The scene runner (`cw_scene_app`) renders through it every frame; other apps call `cw_pipeline_render`. Modules add their work to it as passes.

Coordinates are Y up, -Z forward. Cameras, lights and decals face their -Z (not the origin, as three.js's default light target does): aim them with rotation or `cw_node3d_look_at`.

You do not need the tree to use three.c. The `hello` sample is a bare three.c scene in a platform app:

@include samples/hello/main.c#three_scene

`cw_render3d_window_renderer` is `t3_renderer_new` on whichever GPU API the run uses, and `cw_render3d_window_target` (in `render`, before `t3_renderer_render`) points it at this frame's window, so the same app draws on GL and on WebGPU. A dual cart may not call GL once the host has selected WebGPU, and a bare `t3_renderer_new` renderer is a GL renderer. To draw such a renderer into a 2D render target instead (a 3D picture inside a 2D game, a minimap), make the target with depth, size the renderer to it and call [cw_render3d_texture_target](api:cw_render3d_texture_target) before `t3_renderer_render`; the 2D renderer then draws the target the right way up on GL and WebGPU (`samples/r2d_interop`). A 3D pipeline renders into a 2D target through [cw_target_rt](api:cw_target_rt) and `cw_pipeline_set_output`.

Most games use nodes instead: `samples/scene3d` builds a level from `.cwscene` files only, and `samples/gltf_scene` loads packed glTF models through the resource system with LOD chains.

## Program tables

three.c draws every material with the program three.js r187 generates for its state: the material kind, its features (maps, skinning, instancing, clearcoat...), fog, shadows and the light counts, written like `standard+map+rep+shadow+l1001`. The programs are captured from three.js ahead of time and compiled in as tables. The engine links three.c's core table only (output pass, backgrounds, PMREM, shadow casters), so an app carries the programs it draws and no others: a cart's wasm went from about 36 MB with three.c's full table to 7.5 MB.

A table is a manifest of states, `<name>.txt`, with the C generated from it beside it (`<name>_gl.c`, `<name>_wgpu.c`), all checked in: a build needs only a C compiler. An app gets its tables from `cw_add_app(... PROGRAMS path/to/<name>.txt)`, from `programs/<app>.txt` beside its CMakeLists.txt (what a new project uses), or from its directory's `CW_PROGRAMS` (the samples share `samples/programs/cw_samples.txt`). The engine and the editor register their own tables (`src/render/programs/cw_engine.txt`, `editor/programs/cw_editor.txt`).

When an app draws a combination its tables lack:

- a native development build builds the program on first use (`CW_THREE_BUILDER`, three.js r187 in an embedded QuickJS; a few hundred milliseconds), so nothing goes missing while you iterate;
- a cart or an exported build skips the draw, and says so loudly: an error per missing state naming what is not drawn (`render: NOT DRAWN: node "/root/Level/Crate" (mesh "Crate_LOD0"), MeshStandardMaterial "Painted wood": its program state standard+map+l1100 is in none of the app's program tables`: the nearest node holding the drawn object, the glTF mesh and the material, from three.c's `t3_renderer_set_missing_hook`), then an error that names the exact command to run (`node tools/programs.mjs <your app>`) and this section, a red banner across the top of the picture with the same command while any state is missing (so an empty screen is never a mystery), and with `CW_PROGRAMS_STRICT=1` in the environment (a native run) the run exits with status 4 at the first missing state;
- either way the state is reported: the pipeline's warning, `cw_pipeline_missing_states`, the debug fields `cw.three.missing` and `cw.three.missing_states` (the whole list, up to 16 KB; it was cut at 1 KB, which hid states from the checks that read it), and natively `CW_PROGRAMS_LOG_DIR` or `T3_MISSING_LOG`. Every gate fails on a reported state and prints it.

To add the states, run the app through the tool, which merges what it lacked into the app's first table and regenerates:

    node tools/programs.mjs <app> [--gates]      # the app's runs (and its gates' runs), GL and WebGPU
    node tools/programs.mjs --projects <dir>     # a project outside the tree's build
    node tools/programs.mjs <app> --scenes LEVEL=a,b,c   # an app that picks its scene from the environment: one run per scene, the union of their states
    ninja -C build/native <name>_programs        # regenerate one table from its manifest

then commit the manifest and the generated C. Every native and cart survey runs each graphics preset from the start and then switches the preset half way through a run, from High to each other preset and from each back to High (`--no-switch` leaves those runs out): a game builds content for the preset it starts on (more lights, a denser scatter, a different build of a level on High), and that content drawn with another preset's renderer state (Very Low's plain lighting model and sun-only shadows under High's omni lights, say) asks for states that no run started on one preset asks for. Those were the draws that vanished after a player changed the preset, or Auto stepped down, in play. `tests/programs-switch.mjs` holds it: a scene that adds shadow-casting omni lights on High draws everything after a switch to Very Low with a table surveyed as the tool ships, natively and as the dual cart through romdev on GL and WebGPU, and the same scene with a table surveyed `--no-switch` (the control) misses `standard+instbig+shadow+l1200+s100+xplain` and its error names the node. A cart is asked for states a native run is not (the host's quality and capabilities differ, so a native survey can miss them and the shipped cart shows the red banner): survey the dual cart that ships with `node tools/programs.mjs --carts <app> [--gates]`, which runs `build/cart-wgpu/carts/<app>.wasc` on a GL host (romdev's dual cart told to render with GL, `run-cart.mjs --dual-gl`) and on a WebGPU host, reads the states the cart reports and merges them into the app's table (`tests/programs-carts.mjs` checks it, with a fully covered cart as the control). Each cart (and each gate's run with `--gates`) is also run once as a player's host runs it, loaded with no seed (`run-cart.mjs --warmup-spread`, which on romdev now loads the cart unseeded): its warm-up spreads over frames under the loading screen and the clock is real, which a deterministic load (the whole warm-up inside frame 1) never shows. The warm-up's shader phase draws the scene a slice at a time, several slices a frame; each slice is a pipeline render of its own and now gets its own pre-render, so the cascaded sun (and anything else the tree registers before a render) is there for every slice. Before, only a frame's first slice had it: the later ones drew the sun as a plain shadowed light, states play never asks for (`standard+...+shadow+l1501+s100+xgi`), which no survey found and which kept a player's red banner up for good; and the probe grid baked during the first slice, from a scene still mostly hidden. The probe grid and reflection probes now wait for the warm-up's full render. Gate `warm-states` (sample `warm_states`: a cascaded sun, a probe grid, many meshes over few materials) loads the cart that way through romdev on GL and WebGPU and finds no missing state; its control, `CW_TEST_WARM_NO_PRERENDER=1`, drops the later slices' pre-render and must find one. A project's carts are surveyed from its own tree: set `CW_PROJECT_ROOT` to it. The survey fails, naming the app and the host, when a run surveyed nothing (the cart did not load, never rendered a frame, or draws without the engine's pipeline, so it has no state list to read) and when there is no cart to survey; a run that surveyed nothing never reads as "no missing states". Generating captures on three.c's mock GPU (byte-identical to a real one), so it needs Node but no GPU. The tool minimizes a manifest: a program captured for more lights of each kind (and the same shadow casters) also draws scenes with fewer, so `standard+fog+l1701` replaces `standard+fog+l1001`.

Because the mock GPU accepts any shader, every regeneration then compiles the table's WebGPU programs with the WGSL compiler a WebGPU host uses (Tint, inside Dawn), on Mesa's software Vulkan (no GPU needed), and fails naming the state and the compiler's message when one is rejected: a table never ships a program that draws nothing on WebGPU (`tests/programs-wgsl.mjs`, with controls that must fail). Run it by hand on a capture directory with `node tools/lib/wgsl-check.mjs build/native/programs-cache/<table>-<hash>`. It builds each pipeline with the vertex buffers three.c binds (one per geometry attribute, the instance matrix in one buffer, the instance colour in its own; three.c's `vertexBufferLayout`), so the most a program can need, an `InstancedMesh` with vertex colours, tangents and instance colours, is 7 of the 8 every WebGPU device has; a program that needs more vertex buffers or attributes than a device allows is reported as over the device limits, naming its attributes, before it ships (sample `instanced_attributes` draws every optional attribute at once on GL and WebGPU). Two things to know when writing shading code of your own (a surface, [Writing your own surface](#writing-your-own-surface), or an extension under `src/*/extensions`):

- The Dawn the engine ships stops with an internal compiler error ("swizzle view instruction still has usages after lowering") on a swizzle of a swizzle of a variable, which TSL writes for `texture(t, uv).rg` followed by `.x`. Swizzle once (`texture(t, uv).x`, or `dot(k, vec2(a, b))` for a weighted sum).
- WGSL leaves the result of arithmetic on infinity and NaN to each implementation. Never pick between two whole results with `mix(a, b, w)` where `w` is 0 or 1 and the unused side can overflow: infinity times 0 is NaN, and a NaN in one channel comes out as black in that channel (the water once lost its green this way on WebGPU only, where a raw depth texel overflowed in a debug view mixed in by 0). Use `select(cond, b, a)`, and clamp texels that hold a large "nothing here" value (a depth copy holds 3e38 where nothing was drawn) before any use.

Vertex colours (`vertex_colors` on a material) take an rgb or an rgba attribute; a glTF exporter's default `COLOR_0` is rgba and draws, with its alpha ignored (three.c 0.186.19: the generated programs read the colour only). A colour attribute of any other width is refused and the draw is reported as skipped. `samples/vertex_colors` draws both and its gate (`vertex-colors`, GL and WebGPU) has a control with a refused attribute.

Texture filtering is the same on GL and WebGPU. A texture's `anisotropy` (1 by default; 8 is a good value for floors, roads and walls seen at a low angle, where detail otherwise smears into a blur) applies on both backends when its filters are linear with a linear mip filter (`T3_LINEAR_MIPMAP_LINEAR`, the default); the GPU clamps it to what it supports (16 at most). Wrap, filter and anisotropy can be changed at any time, also on a texture from the resource system after it loaded: the next draw uses them. A resource texture's colour space is fixed when it loads (see `cw_res_texture`). `samples/texture_filtering` draws a tiled floor at a grazing angle and a wall with resource textures, at anisotropy 8; its gate (`texture-filtering`) holds GL and WebGPU to the same picture, with a control that turns anisotropy off on WebGPU alone.

## The 3D node types

| Node | What it configures |
|---|---|
| `Camera3D` | fov, near, far; the first `current` camera renders |
| `MeshInstance3D` | a primitive (BOX, SPHERE, PLANE, CYLINDER) scaled to `size`, or a `mesh` resource; material colour, metalness, roughness, emissive; `shading` STANDARD or TOON; shadows; visibility and baking properties |
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
| PREPASS | "depth_prepass" when `depth_prepass` is on (depth of the opaque queue, alpha-tested cutouts still discarding; transparent and transmissive surfaces, colour and the sky are left out; on GL it draws into a framebuffer holding only the scene's depth, so no driver runs colour work for it) |
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

Every tree renders the way three.js r187 does: colours are linear, lights are physical units (directional energy is lux-like illuminance, omni and spot energy is candela-like intensity with inverse-square falloff, windowed to 0 at `range` when it is set), the scene renders linear HDR into RGBA16F, and the "tonemap" pass applies exposure, the tone curve (AgX by default) and the sRGB transfer. With exposure 1 and AgX, sensible values are a sun of 3 to 5, room lamps of 10 to 50 a few metres away and ambient 0.05 to 0.3; real photometric values work with `exposure_ev` around -14. A light's `energy` runs from 0 to 100000, enough for the sun's real 100000 lux; a value outside that is clamped to the range and the engine logs a warning the first time it happens for that light (the same goes for every property that has a range).

Pick colours darker than a colour picker suggests: they are linear, and the tone curve desaturates bright ones (`color(1, 0, 0)` shows as about (220, 90, 75) under AgX). A scene authored for the old r128 workflow (display colours, r128's legacy light units) converts once with `node tools/convert-r128-scene.mjs <file.cwscene>`.

Each tree renders through its own pipeline; `cw_pipeline_set_output` sends it into a target instead of the screen, the pattern any render-to-texture or split-screen view uses.

## The post stack

The post settings are the fields of `cw_post_settings`, mirrored as PostProcess properties: MSAA, depth prepass, exposure (a multiplier and stops), tone curve (r187's: AGX by default, NEUTRAL, ACES_FILMIC, CINEON, REINHARD, LINEAR), energy-conserving bloom, GTAO at half resolution, TAA (Halton jitter, reprojection by the camera's and each moving mesh's motion, neighbourhood clip), FXAA or SMAA 1x, a 3D LUT grade loaded from a `.cube` file (`cw_lut_load_cube`), vignette, grain and chromatic aberration. `debug_view` shows depth, normals, AO, velocity or the bloom chain instead of the image. Nothing is lowered automatically: quality settings are the developer's.

**Depth of field** (`dof`, PostProcess properties `dof_focus_distance`, `dof_focus_range`, `dof_blur`, `dof_near_blur`) blurs what is nearer or farther than the focus, as a long lens does: a character select screen, a portrait, a cut scene. It runs on the HDR image before bloom, at full resolution: each pixel works out its blur from its view depth and gathers 32 samples on a golden-angle disk. A sample that lies behind the pixel counts only up to the pixel's own blur, so a sharp face never picks up the blurred wall behind it, while a blurred foreground still spreads over what is behind. `dof_blur` is the largest radius in pixels of a 1080 line image (it scales with the output height); `dof_near_blur` is the share of it used in front of the focus. A game usually turns it on for a menu or a close camera and off in play (set the properties on its PostProcess node from code). Very Low and Low effects detail turn it off. Cost: 32 colour and 32 depth reads a pixel at full resolution. On the Radeon 890M at 1920 x 1080 (post_stack, its depth of field segment against its plain segment, tools/perf-frame.mjs --gpu-timers, load average 10 to 12) it added about 0.5 ms of GPU time on GL and 1.2 ms on WebGPU; a Vega 8 (about a quarter of an 890M) should expect 2 to 5 ms, so keep it to menus and close cameras. Its gate is `post-dof`.

**MSAA on both backends.** `msaa` multisamples the scene pass. GL takes the sample count asked for (2, 4 or 8, up to what the driver allows); WebGPU multisamples 4x, its only count, for any of them. For an HDR scene WebGPU needs a core device: the engine asks for core WebGPU wherever the GPU has it (natively, and the cart hosts do too: romdev, wasmcart-native and the test harness's browser), so an HDR scene with ambient occlusion, decals, fog or a sky is smoothed as on GL, and the passes after the opaque phase see the multisampled depth's first sample, as GL's resolving blit gives them. On a device that offers only compatibility mode (a GLES-class GPU), WebGPU multisamples an RGBA8 scene with no depth-reading pass and draws anything else single-sampled, with a warning in the log. `cw_gpu_adapter_info()->core` says which device a run got, and `CW_WGPU_FEATURE_LEVEL=compat` runs a native build that way, to check a game still looks right there. `samples/msaa_hdr` (gate `msaa-hdr`) is a fence, wires and grass against a low sun with HDR, ambient occlusion, decals and 4x MSAA, held to GL within 0.04% of pixels. On the 890M at 1920 x 1080, 4x MSAA costs WebGPU about 0.3 to 0.6 ms of GPU time at the Low and Medium presets and 0.7 to 1.5 ms at High and Ultra (GL 0.25 to 0.8 ms), with one view or four (the views split the same pixels); budget roughly three times that on a Vega 8. The Anti-aliasing setting's Off, FXAA and SMAA turn the scene's MSAA off on both backends (Very Low, Low and Medium); Full, on High and Ultra, keeps the game's MSAA, and `cw_quality_keep_msaa(true)` keeps it on every value but Off.

**Distortion inside the tone curve.** Heat haze and shockwaves (cartwheel/explosions.h) bend the finished frame in a full-frame pass of their own. `fuse_distortion` (off by default) does the same bending inside the tonemap pass, with the same guards and rounding, and saves that pass's read and write of the HDR frame (about 0.45 ms at 1080p on the 890M); bloom then glows from the unbent frame, which is the one difference.

One limit stated in the header: SMAA has no diagonal pattern search yet.

### Motion vectors

The velocity target (`CW_RES_VELOCITY`, an RG16F image of each pixel's motion since the last frame in uv units) holds two things. The camera's motion is rebuilt from depth for the whole frame, which is exact for everything that stood still. Then every mesh that moved since the last frame is drawn over it with the scene's depth for the depth test: a node that moved or turned, a skinned mesh whose bones moved, and an instanced mesh whose instance matrices were written. Each is drawn with one small program that places the vertex where it is now and where it was a frame ago (the previous world matrix, the previous bone matrices, the previous instance matrices), so a walking character's limbs, a spinning prop and every box of an instanced swarm carry their own motion.

Nothing is tracked or drawn for what stands still: the pass compares each mesh's matrices with the last frame's, keeps the previous pose by swapping two buffers when it changed, and uploads bone and instance data only for the meshes it draws. A scene with no moving mesh pays the comparison and no draw. In `samples/motion_vectors` at 1920 x 1080 on a Radeon 890M (WebGPU) the whole velocity pass takes 0.17 ms of GPU time with the three movers drawn (a 19-bone character, 48 instances, one mesh), the same within noise as with the camera's motion alone; drawing them costs about 0.5 ms of CPU time in a development cart. The debug fields `cw.velocity.objects` and `cw.velocity.tracked` report how many meshes were drawn and how many are followed.

TAA reads it: a pixel reprojects its history along the motion of the nearest surface in its 3 x 3 neighbourhood, so the antialiased edge of a moving object follows the object. With only the camera's motion a moving character smeared and its edges broke up, because its history was read from where the background had been. Measured against a picture of the same frame rendered with 36 samples a pixel (`tests/motion-vectors.mjs`, mean error in display levels over the mover's pixels, 640 x 360, the same on GL and WebGPU):

| mover | its own motion | camera motion only |
|---|---|---|
| a bar turning 0.21 rad a frame (`samples/post_stack`) | 3.4 | 6.4 |
| a mesh turning 0.17 rad a frame | 6.1 | 8.4 |
| a skinned character walking and sliding | 6.6 | 10.4 |
| an instanced swarm on two counter-turning rings | 5.3 | 8.4 |

What keeps the camera's motion: blended materials (they write no depth), sprites, points and lines, and a morph target's change of shape (a morphed mesh still moves with its node and bones). A first person viewmodel is treated as still on the screen. A mesh seen for the first time, or again after being hidden, has no previous pose for one frame.

The pass runs only when something reads the velocity target (TAA, the VELOCITY debug view, a pass of your own that declares `CW_RES_VELOCITY`), so the Very Low and Low presets, which draw without TAA, never pay for it. `samples/motion_vectors` shows the three kinds of mover under TAA and then the velocity buffer itself (`debug_view = VELOCITY`: grey is still).

`samples/post_stack` steps one static room through every effect in turn, one schedule segment each (its gates are `post-bloom`, `post-ssao`, `post-taa` and so on, each against the plain AgX segment), and measures TAA convergence over one jitter cycle. Its last segments spin a bar in front of the camera: plain, under TAA, and as the velocity buffer.

## Lights and shadows

Lights are three.js r187 lights: directional, omni (point), spot, ambient and hemisphere, with r187 shadow maps for every light that casts them (`shadow`, `shadow_size`, biases; a mesh needs `cast_shadow` and `receive_shadow`). How many of them a surface sees depends on the tree's lighting mode ([cartwheel/lighting.h](header:lighting.h)).

### Hundreds of lights: clustered lighting

In the default FORWARD mode every light goes to three.c, which lights a surface with at most 8 lights of each kind (the rest light nothing, and the engine warns once). A street of lamps, a wall of neon signs or a firefight of muzzle flashes needs more. Add a LightingSettings node in CLUSTERED mode:

@include samples/night_street/assets/main.cwscene#Lighting

CLUSTERED is three.js r187's `ClusteredLighting`, on GL and WebGPU alike:

- Every frame the engine bins the shadowless omni and spot lights into a froxel grid over the view: 32 x 32 pixel tiles times 24 depth slices spaced exponentially between the camera's near and far, at most 64 lights a cell (r187's numbers, all settings of the node). The binning runs on the CPU on the job pool, so GLES3, which has no compute shaders, gets the same lists as WebGPU, and the lists are the same at any thread count.
- Each lit material (Lambert, Phong, Standard, Physical) draws with the "cluster" shading part: a fragment finds its cell and loops that cell's lights with r187's own BRDF and falloff. With few lights, CLUSTERED and FORWARD draw the same image (the `cluster-parity` gate compares them on both backends).
- Directional, hemisphere and ambient lights stay on three.c's path; shadowed omni and spot lights are clustered too, with their shadows in an atlas (below).
- In split screen the scene work is done once a frame, not once a view: the first view gathers the scene's omni and spot lights (those a game added with three.c directly too) and marks the lit materials in one walk, and each view then only keeps the lights its camera's layers and visibility allow, projects, bins and uploads them. A mesh's materials are looked at again only when its material slots change, so swapping a material by a plain write is seen. Four players on a large track cost the lights pass 1.3 ms of CPU a frame on GL (3.8 ms when each view walked the scene; the `profile-lighting` gate holds four views under 2.5 times one).

Three things go beyond r187. Spot lights are clustered too (r187 clusters only point lights), with a cone test when binning and r187's spot cone in the loop; `cluster_spot_lights` turns that off. A full cell keeps its nearest lights. And a light with `range` 0, which r187 would put in every cell, gets the range where its intensity over distance squared falls under `light_cutoff` (0.005), and fades out there as a range does: give lights a range for full control.

Costs: binning is O(cells each light reaches); the GPU pays a cell lookup per fragment plus each light of the fragment's cell, so the cost follows how many lights overlap on screen, not how many exist. Each frame uploads 64 bytes a light, 4 bytes a cell and 2 bytes a light reference. A mesh whose material carries the cluster part is drawn on its own unless `batching` is on (below). The part composes with the terrain's surface parts, with the engine's other parts and with a game's own parts and surfaces ([Writing your own surface](#writing-your-own-surface)): such a material is lit by the clustered lamps like any other. Effects detail keeps 24 lights a cell on Low and 32 on Medium (64 on High and Ultra). The debug command `lighting` reports the light counts, the grid and the binning time; `cw_lighting_get_stats` gives the same in code.

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

In CLUSTERED mode a shadowed omni or spot light is clustered too (`shadow_atlas`, on by default): instead of three.c's own shadow map, it gets a tile of one depth atlas, a spot light one tile and an omni light a 3 x 2 block of cube faces. A tile is as big as the light is on screen (its bounding sphere's size in pixels), at most the light's `shadow_size`, and the tiles are packed largest first; when the atlas (`shadow_atlas_size`, 4096 a side) is full, the least important lights get smaller tiles, and then none. A tile is redrawn only when its light moved or a shadow caster inside the light's range changed, and at most `shadow_updates_per_frame` faces (24) are redrawn a frame, the biggest lights first: a street of static lamps costs its shadows once. The light loop filters each light's tile with r187's PCF and the light's `shadow_bias`, `shadow_normal_bias` and shadow intensity. Turn `shadow_atlas` off to keep shadowed lights on three.c's path, as r187 does (8 of a kind at most).

### Sun shadows that reach the horizon: cascades

A DirectionalLight3D with `shadow` and `shadow_cascades` 1 to 4 draws its shadows as three.js r187's `CSMShadowNode`, in either lighting mode:

@include samples/street_sun/assets/main.cwscene#Sun

The view from the camera's near to `shadow_distance` (r187's `maxFar`; 0 is the camera's far) is split by r187's practical split. Each cascade is an orthographic map of `shadow_size` fitted around its slice of the view, with r187's width and texel snapping, so shadows do not shimmer as the camera moves; near shadows get the finest texels and the last cascade reaches the end of the view. Fragments pick their cascade by depth and cross-fade into the next (`cascade_fade`). A cascade is redrawn only when the camera moved it or a shadow caster changed.

`samples/street_sun` is the night street by day: the sun alone, four cascades reaching 250 m down the street. Its `csm-parity` gate checks, on GL and on WebGPU, that the cascades put shadows where one big shadow map over the same ground puts them.

### Real fixtures: IES profiles

A real lamp does not throw a clean cone. Its reflector and glass give it rings, hot spots and a cut-off, and manufacturers publish that shape as an IES photometric file (`.ies`, the LM-63 text format). `ies_profile` on a `SpotLight3D` or an `OmniLight3D` gives the light that shape, as three.js r187's IESSpotLight does:

```
[node name="StreetLamp" parent="." type="SpotLight3D"]
rotation = vec3(-1.5707963, 0, 0)
energy = 420
range = 16
ies_profile = res(0xf6905d0e2b1859a7)
```

The value is the file's resource (its UID is `cw_res_path_uid("lights/street.ies")`). `cwpack` checks every `.ies` file when it packs it and stops with the reason on one it cannot read; the file is read by three.js's own IESLoader rules (three.c `t3_ies_texture_parse`): the candela values normalized to the brightest direction, a single horizontal angle for a shape that is the same all the way round, or a measured range mirrored to the full circle. `energy` and `color` still set how bright and what colour the light is; the profile only shapes it.

The profile is read around the light's own axes: the tilt off its -Z (where the light points), the twist around that axis from its +Y, so turning a lamp about its axis turns its pool of light. It replaces a spot light's cone (`angle` and `penumbra` no longer shape the light; `angle` still sizes its shadow map, so keep it wide enough for the profile). On an omni light the profile covers the whole sphere, down to the up-light a lantern throws at the ceiling. Shadows, range and decay work as for any light.

IES lights are drawn by the clustered lights (`LightingSettings` `mode = CLUSTERED`): the scene's distinct profiles sit in one small atlas (a profile is 180 texels by 1 or 361 rows of half floats), and while a clustered light has a profile the lit materials draw with the `clusteries` part instead of `cluster`, so a scene without profiles pays nothing for the feature and `tools/programs.mjs` finds the new states like any other. A light with a profile costs one more row read and one filtered read of the atlas per pixel it lights, and is binned as a sphere, not a cone. With FORWARD lighting a spot light's profile is drawn by three.c's own IESSpotLight (the same shape for a light without roll), and an omni light's is not drawn (a warning says so). At most 64 different profiles a scene. `samples/ies_lights` shows a ring downlight with a shadow, a street lamp turning about its axis, a lantern on an omni light and a plain spot light; its gate is `ies-lights`.

### Terrain under lamps and cascades

A Terrain3D's materials carry their own surface part (`terrain`, `terrainlow`, `terrainfar`), and the cluster and csm parts compose with it (`terrain_cluster_csm`), so lamps light the ground and the sun's cascades fall on it. The terrain also casts into the cascades with its own vertex displacement. A game's own surfaces and parts compose the same way ([Writing your own surface](#writing-your-own-surface)). `terrain_lights` (terrain_demo's `lights` run) drives the car at dusk past seven clustered lamps with the sun casting four cascades. Its `terrain-lights-parity` gate checks on GL and WebGPU that the clustered lamps light the ground exactly as three.c's own lights do, and that the cascades put the terrain's shadows where one wide shadow map puts them.

The two parts share one uniform group (`lightingFrame`), so a terrain under both still fits WebGPU's four bind groups.

### Quality and costs

| Preset | Lights a cluster keeps | Sun cascades | Shadow maps | Atlas |
|---|---|---|---|---|
| Very Low | 16 | 1, half the distance | the sun only: half the size, 512 to 1024, hard | none (omni and spot lights cast no shadow) |
| Low | 24 | 2 | 1024 at most, hard | 2048 |
| Medium | 32 | 3 | 1024 at most, soft | 2048 |
| High | 64 | as authored | as authored | as set |
| Ultra | 64 | as authored, x1.5 the distance | twice, up to 4096 (a cascade at most 2048) | as set |

Very Low, Low and Medium also cull small shadow casters (see [Graphics settings and presets](#graphics-settings-and-presets)).

`street_sun_perf` (1920 x 1080, the camera driving, so every cascade is redrawn every frame), ms per frame on the 890M:

| Preset | Cascades | GL frame | GL GPU | WebGPU GPU |
|---|---|---|---|---|
| Low | 2 of 1024 | 1.2 | 0.8 | 0.7 |
| Medium | 3 of 1024 | 1.5 | 1.1 | 0.9 |
| High | 4 of 2048 | 3.6 | 2.9 | 1.8 |
| Ultra | 4 of 2048 | 3.5 | 2.9 | 1.9 |
| High, one shadow map | 1 of 2048 | 1.5 | 1.1 | 1.6 |

Each cascade is at most 2048 (the four share one map twice that wide), so Ultra draws them as High does. A still camera redraws no cascade.

A redrawn atlas tile or cascade is a shadow caster pass over the scene. On the GPU, a shadowed light costs 20 depth reads where its cell is drawn, the sun 5 compare samples per pixel inside its cascades and 10 across a band where two cascades fade into each other. A pixel past the last cascade's far edge reads none: the sun lights it unshadowed, as it always did there. On WebGPU the atlas keeps an R32Float copy beside its depth (compatibility mode reads depth only through a comparison sampler, which the light loop cannot use), and a redrawn tile is copied into it.

A mesh whose material carries the cluster or csm part is its own draw by default, so three.c's auto-instancing (same geometry, same material) stops for it. `LightingSettings.batching` turns it back on: both parts' hooks fill only per-frame values (`cw_shading_part_batching`, cartwheel/shading.h), so one instanced draw can carry a whole batch. It is off by default because, measured, it does not pay here. In `vis_city_lights` (about 4500 meshes, 64 clustered lamps and four cascades, 1080p, `tests/vis-city-batch.mjs`), batching gives the same picture with 19 draws instead of 891. Submitting the draws costs about the same CPU either way (three.c's GL draws are cheap). The scene pass costs 0.9 ms more GPU because instances lose the front-to-back order and the clustered shading runs on hidden pixels. Native frame time went from 18.3 to 22.1 ms, and Chromium from 34.2 to 39.3 ms. Turn batching on for a frame bound by draw submission, with many small meshes and cheap overdraw.

The same city shows where a clustered frame's CPU time goes. Cascades the moving camera redraws every frame are caster passes over every mesh (`lighting.csm`, 8 to 14 ms a frame on the 890M under load). Binning the lamps takes about 1 ms (`froxel.bin`). Keeping every material's parts current takes 0.5 to 1 ms for 4500 meshes (`lighting.materials`), now a lookup per material.

Light probes, reflection probes and screen-space light are in [Reflections and bounce light](#reflections-and-bounce-light).

## Graphics settings and presets

[cartwheel/quality.h](api:cw_quality_get) has two independent choices. The **graphics preset** (Very Low, Low, Medium, High, Ultra, Auto, or Custom once a setting was changed on its own) decides how much the engine draws. **Resolution** (the 3D scene's share of the output, 50% to 100%) is the player's alone: no preset sets it, Auto never changes it, High at 75% still reads `High` with the resolution row at 75%, and the save keeps both. A player can run Very Low at 4K or Ultra at a 75% share. The output's own size (a 720p window on a 4K screen, a 4K display) is the host's window or display setting (RetroArch: Quick Menu > Core Options > Internal Resolution); the resolution row is the 3D scene's share of that output.

| Setting | Very Low | Low | Medium | High (default) | Ultra |
|---|---|---|---|---|---|
| Ambient occlusion | Off | Off | On | On | On |
| Shadows | Minimal | Low | Medium | High | Ultra |
| Anti-aliasing | FXAA | FXAA | SMAA | Full | Full |
| Effects detail | Very Low | Low | Medium | High | Ultra |

High draws exactly what the scene's PostProcess and lights ask for, the same frame as without this module. Nothing here reaches the simulation (gate `quality-sim-same`).

- **Resolution** (50%, 60%, 67%, 75%, 85%, 100%): the 3D scene and its post render at that share of the output and are scaled up with a Catmull-Rom filter, while the 2D window passes (HUD, menus, text) and every overlay pass (`CW_PHASE_OVERLAY`) draw over it at the output's full size: the scene is scaled up into the output as the overlay phase starts, so a custom overlay pass of the game's (raw pass code too: `cw_pass_bind_target(ctx, NULL)`, `cw_pass_fullscreen`) sees the full-size output in `ctx->output`, `ctx->width` and `ctx->height`, before the 2D canvas pass as well as after it. It applies on top of `cw_app_desc.render_scale`; an app with `pixel_scale` keeps its own size. Draw the 3D scene before the first 2D window pass of a frame: that pass is where the scene is scaled up when no pipeline drew it. A frame bound by shadow casters, vertices, water grids or particle simulation gains little from it (see the table below), so the presets cut those instead.
- **The game's own scene share** (`cw_quality_scene_scale(s)`, s from 0.1 to 1): a look that wants a small scene on purpose (a stylized post effect that pixelates, posterizes or redraws the picture, a retro look) renders the 3D scene and its post at that share of the output, below the Resolution setting's 50% floor too, scaled up with the same filter, while the overlays stay at full size. It replaces the setting while it is set; `cw_quality_scene_scale(0)` gives the scene back to it. The player's Resolution choice stays in the menu and the save and applies again at 0. `cw_render_size` reports the scene's size either way. A stylized post look that is redrawn at full resolution by an overlay pass pays for the 3D scene at its small share: one such game at 1920 x 1080 on the Radeon 890M (WebGPU) took 4.38 ms of GPU time a frame at 100% and 1.79 ms at 50%. Gate `overlay-full-res`.
- **Ambient occlusion**: GTAO as the PostProcess asks for it, or none; its sample count follows effects detail.
- **Shadows**: **Minimal** draws the sun only (omni and spot lights cast none), one cascade, a map of 512 to 1024 (half the authored size), hard edges and half the shadow distance, and culls small casters. It keeps the sun's shadow under characters and props, so nothing floats (gate `quality-grounding`). **Low** is 1024 at most, hard, 2 cascades; **Medium** 1024 at most, soft, 3 cascades; **High** as the lights ask; **Ultra** twice that up to 4096, soft, the cascades' distance x1.5. **Caster culling**: a mesh whose bounding sphere's radius over its distance to the camera is under the setting's share (Minimal 0.02, about 40 pixels across at 1080p with a 60 degree view; Low 0.012; Medium 0.006; High and Ultra none) draws into no shadow map that frame. The scene's big shapes keep casting, and so does anything near the camera. Instanced sets are kept whole. The debug fields `shadow.culled` and `shadow.casters` count them for the last view.
- **Anti-aliasing**: Off, FXAA, SMAA or Full (as the PostProcess asks, the scene's MSAA included). FXAA and SMAA turn the scene's MSAA off and put that post filter in its place; a scene that asks for no AA at all keeps none, and one that asks for FXAA keeps FXAA on SMAA. Off and FXAA also draw without TAA and its velocity pass. A game whose look needs MSAA (thin lines no post filter holds) keeps it on every value but Off with `cw_quality_keep_msaa(true)`. WebGPU multisamples 1x or 4x only, so no value goes above what the scene asks.
- **Effects detail**:

| | Very Low | Low | Medium | High | Ultra |
|---|---|---|---|---|---|
| GPU particles drawn (all are simulated) | 0.25 | 0.4 | 0.7 | 1 | 1 |
| density (grass, crowds, scatter: `cw_quality_density()`) | 0.2 | 0.35 | 0.65 | 1 | 1 |
| LOD error budget | x3 | x2 | x1.5 | x1 | x0.75 |
| clustered lights a cluster keeps | x0.25 | x0.375 | x0.5 | x1 | x1 |
| bloom | off | 3 levels, the first from 4 reads | 5 levels | as authored | as authored |
| volumetric fog grid | off (plain height fog) | off (plain height fog) | 96 x 48 | 160 x 64 | 240 x 96 |
| distortion buffer | off | off | half | full | full |
| explosion lights | 0 | 0.25 | 0.5 | 1 | 1 |
| skin shading | plain | plain | on | on | on |
| lighting model (`cw_quality_scale("shading")`) | plain | full | full | full | full |
| SSR; depth of field; SSGI | off; off; off | off; off; off | half; on; off | as authored; on; off | x1.5; on; on |
| HRTF voices | 0.25 | 0.25 | 0.5 | 1 | 1 |
| water | Low's grid halved, no reflection, refraction or caustics | as before | as before | as before | as before |
| terrain | the low layer set, the near ring at Low's reach, each layer read once, the coarser levels from 0.6 of their distance | the low layer set, the near ring at Low's reach | | | |
| height fog, FXAA | inside the tone mapping pass | inside the tone mapping pass, with bloom's composite | their own passes | | |
| HDR scene | R11G11B10 float | R11G11B10 float | RGBA16F | RGBA16F | RGBA16F |
| sky cube and environment (ProceduralSky) re-rendered at most every | 8 frames | 4 frames | 2 frames | frame it changes | frame it changes |

**The plain lighting model** (Very Low). Every MeshStandardMaterial and MeshPhysicalMaterial that the lighting system marks (the ones that take its clustered lights and cascaded sun, cartwheel/shading.h) carries the `plain` shading part: the material keeps its colour, maps, roughness, metalness, normal map, emission, ambient occlusion and light map, and is lit with Lambert diffuse and a Blinn-Phong highlight whose sharpness follows the roughness. The environment and the GI probes light it with their diffuse light only, the reflection is not sampled (the specular colour takes the diffuse light through an analytic estimate of the split sum, so metals keep their tint, duller), the light probe grid reads its first two SH bands, and clearcoat, sheen, iridescence and anisotropy are left out. A material with transmission keeps the full model, so glass stays glass. On the 890M at 1080p this took the scene pass of open_world from 1.9 to 1.1 ms on GL and 2.7 to 1.3 ms on WebGPU (with the terrain's single reads). The states are new program states (`x<parts>_plain`): the survey (`tools/programs.mjs`) runs every preset, so a regenerated table has them.

**Very Low and Low post.** The height fog runs inside the tone mapping pass (its blend over the HDR value as it is read), and so does FXAA, as FXAA 3.11's console variant: four diagonal reads and the centre, two or four along an edge, no edge search, its edge test on a display brightness estimated from the HDR value and its reads averaged in a compressed space (each over one plus its exposed brightness, undone after the average) and tone mapped once, so a bright light's edge smooths as its display value would. On Low bloom's glow is composited in the same pass (the glow is gathered from the scene before the fog), and its first level is made from four reads instead of thirteen. Two full-frame passes fewer (about 0.15 to 0.25 ms at 1080p on the 890M). The picture is the separate passes' except at the edges FXAA smooths (gate `quality-verylow-post`, Very Low and Low). `CW_TEST_NO_FUSION=1` runs the passes apart.

**The small HDR scene** (Very Low and Low). The scene renders into an R11G11B10 float target, half the bytes of RGBA16F, where the device can render and blend it (GL: a complete R11F_G11F_B10F framebuffer; WebGPU: the rg11b10ufloat-renderable feature, which the engine asks for natively and a cart gets when its host's device has it). It has no alpha channel and less precision in the darks; nothing those presets draw after the scene reads its alpha. On the 890M at 1080p it took the scene pass of the jetski game from 1.46 to 1.05 ms and the arena shooter's from 3.73 to 2.84 ms at Very Low on GL. `CW_TEST_SMALL_HDR=0` (native) keeps RGBA16F.

**Volumetric fog on Low and Very Low** draws as plain height fog (the analytic fog the HeightFog node draws without `volumetric`, inside the tone mapping pass), so a scene keeps its fog on the cheaper presets without the froxel grid's cost.

A game adds its own render-only settings with `cw_quality_lever_add` (a name, its values' labels, the value each preset picks as a `cw_quality_lever_presets` struct: `(cw_quality_lever_presets){ .very_low = 0, .low = 0, .medium = 1, .high = 1, .ultra = 1 }`). The struct is passed by value, so a call written for the four presets before Very Low existed does not build instead of reading a shifted array. Levers show after the engine's settings, change with the presets and are saved with them. Re-apply them, and re-thin instancing, from a `cw_quality_on_change` callback. A game's own table indexed by `cw_quality` needs a Very Low entry: `CW_QUALITY_VERY_LOW` is first, so `q <= CW_QUALITY_LOW` reads "Low or cheaper", and an equality test against `CW_QUALITY_LOW` misses Very Low.

A scene that does not need everything asks for less while it is on screen with a quality scope: `cw_quality_scope_push("lagoon", caps)` takes a cap per setting by name (`caps[CW_GFX_SSAO] = CW_GFX_SSAO_OFF`, `caps[CW_GFX_SHADOWS] = CW_GFX_SHADOWS_LOW`; `-1` leaves a setting alone) and `cw_quality_scope_pop()` ends it. Use the names: a value's number moves when a value is added (Minimal moved every shadow value up by one). A scope only ever lowers what the player chose, so a player on Low stays on Low and the graphics menu still shows the player's own choice; `cw_gfx_effective(setting)` reads a setting as it is drawn now. Scopes nest (four at most) and the lowest cap wins. In a test, `quality scope push ssao=off` and `quality scope pop` do the same from the debug channel.

The player picks from the graphics page ([cartwheel/quality_menu.h](header:quality_menu.h)), which the game shell opens from its Settings and a game opens from its own pause menu: a row for the preset (Very Low, Low, Medium, High, Ultra, Auto; Custom once a setting other than Resolution was changed on its own), one per setting with its value (the resolution with the pixel size it renders at), the game's own, and Back. The names and values are laid out from their measured widths and centred as a pair, so a wide font at 200% text keeps every row on the screen (gate `quality-menu-wide`). Keep the choice in the save with `cw_quality_saved` / `cw_quality_restore`: the word takes 30 bits, so keep four bytes. A word an older engine wrote (a preset byte, an Auto or a Custom word) restores the choice it held (gate `quality-unit`).

**Auto** holds the frame's cost under a budget. While the game says it is in play (`cw_quality_auto_measure(true)` every frame), the engine averages the cost: the GPU time of the frame's passes where the host times them cheaply (native WebGPU's timestamps), else the frame-to-frame time on the real clock (GL's timer queries would cost the CPU a driver sync every frame; a cart has no GPU timers, and a libretro core's fixed step would hide a late frame). It walks a ladder of single changes, so a step never swaps a whole preset at once: High; High with shadows Medium; Medium; Medium with ambient occlusion off; Low; Low with effects Very Low; Very Low (each rung's game levers are its preset's). After a couple of seconds over budget it steps down one rung. On a big overshoot (the average of the first second and a half at a rung over 1.6 x the budget) it jumps to the rung expected to fit, from fixed ratios of each rung's cost to High's refined by what each rung measured in the run, so a weak device is at its level within a few seconds of play instead of stuttering down every rung (gate `quality-unit`: a 60 ms host is on Very Low within 3 seconds). Where it measures GPU time it also steps back up when the cost stays well under budget, never into a rung that was over it in this run. `cw_quality_auto(true, budget_ms)` makes Auto a game's starting choice (19 ms of frame time by default, three quarters of that of GPU time). The rung it reached is saved with the choice, and the next run starts one rung above it, since a run timed by frame time has no other way back up: a couple of seconds of play at that rung, on budget, and it is the new saved rung; over budget, Auto steps back down and the save stays as it was.

**Full resolution.** Auto never lowers Resolution unless the game calls `cw_quality_full_resolution(false)` in init: then the ladder goes on below Very Low at 85%, 75%, 67%, 60% and 50%. No preset changes Resolution either way.

A deterministic run starts at High whatever the save holds, with Auto off, so every host draws the same frame; a choice made in it from the menus (input every host replays alike) applies. `CW_QUALITY=verylow` (native; also `low`, `medium`, `high`, `ultra`) fixes a preset to compare them; the debug command `quality [verylow|low|medium|high|ultra|auto]` switches one live and `quality set <setting> <value>` one setting, the value by name or number (`resolution 50|60|67|75|85|100`, `ssao off|on`, `shadows minimal|low|medium|high|ultra`, `aa off|fxaa|smaa|full`, `effects verylow|low|medium|high|ultra`, or a game setting's name and its value's label); `quality_menu 1` opens the graphics page. `tools/quality-range.mjs` measures each app's GPU time at every preset (see [profiling](profiling.md)).

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
- Split screen: each view gets a grid of its own, lit, shadowed and blended with its own camera's previous frames, sized to its share of the screen's width (a quarter view at High is 80 x 45 cells against one full view's 160 x 90, and its sun shadow tile shrinks the same way, down to 256), so a cell covers as many pixels in every view and four views cost about what one full view does. `fog_shafts_split` shows the room from four cameras; its gate is `fog-split`. Measured on a Radeon 890M in a four-way split-screen shooter at 1920 x 1080 (the grid pass's GPU ms, one shared full-size grid before against now): GL High 2.94 to 1.63, Ultra 7.58 to 2.73; WebGPU High 1.28 to 0.43, Ultra 4.2 to 1.29; one view unchanged (GL 0.8 High, 2.25 Ultra). A Vega 8 has about a quarter of the 890M's shader rate: expect about 6.5 ms (GL) and 1.7 ms (WebGPU) for the four Ultra grids, 2.6 and 0.6 at High.
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
| grid (cells across x slices) | none: plain height fog | 128 x 64 | 160 x 64 | 240 x 96 |
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

[lightmap.h](header:lightmap.h) is an offline CPU lightmapper. MeshInstance3D nodes with `gi_mode` STATIC get a generated second UV set and a lightmap of their own holding the indirect light: occluded sky and `bounces` bounces of every light with `bake`. Direct light stays real time, so lights can still change and moving objects still cast shadows. The bake also stores a grid of L2 spherical-harmonic probes for moving objects and DYNAMIC meshes, for three.js r187's LightProbeGrid.

The bake is deterministic: a BVH with median splits, per-texel random sequences seeded from (seed, mesh, texel), scalar IEEE math with the `cw_` trig functions. The same scene gives the same bytes on every host and any thread count, and `cw_lightmap_hash` is the fingerprint.

The LightmapGI node bakes on its first frame or loads `data`, a `.cwlm` file. A bake is synchronous, so ship the file: `cwbake` loads a scene without a window or GL, bakes with its LightmapGI node's settings and writes the bytes the node would have baked.

```sh
build/native/cwbake samples/lightmap_room/assets/main.cwscene out/room.cwlm
```

```text
{"hash":"a132d3c3d6f9077c","meshes":10,"triangles":120,"texels":52864,"probes":320,"invalid_probes":3,"rays":2711487}
```

`samples/lightmap_room` bakes the same room at runtime, twice, and publishes the hash as `lm.hash_hi` / `lm.hash_lo`; its gate (`lightmap-bake`) requires the same value on every host. A ball rolls through it.

Which UVs a lightmap is read through: its own second UV set (`texture.channel = 1`), so the bake's generated charts line up on any mesh, on GL and WebGPU. A mesh whose uv0 already covers [0, 1] without overlapping, a ground plane or a terrain, can set `lightmap_uv = UV0` to lay its lightmap on that uv0 instead of generating charts.

### Fully baked lighting for weak GPUs

A scene whose scenery never moves can bake all of its light, the way games on weak GPUs light their levels, and pay a few texture reads a pixel for its floor however many lights shine on it. Everything here is opt in.

- `bake_direct` on a light puts its direct light, shadowed by the static geometry, into the lightmaps. LightmapGI `shadow_softness` (the light's radius over its distance; a directional light's angular radius) and `direct_samples` soften those shadows.
- `realtime = false` keeps a light in the bake only: it lights nothing at run time and costs nothing a frame.
- LightmapGI `shading = BAKED` draws the lightmapped meshes unlit from their lightmap: each gets a Basic copy of its material (colour, map, vertex colours, AO map, transparency), whose light is its lightmap's irradiance, the same Lambert diffuse three.js's lightMap gives a Standard material. No real-time light reaches them; lights still light the rest in real time.
- An instanced mesh under a STATIC MeshInstance3D (`cw_mesh3d_set_procedural` with a `t3_instanced_mesh`) shadows and bounces light in the bake: walls, posts, containers.
- `reflection_size` traces a small cube map from `reflection_origin` through the bake (the sky, the lit floor and walls, emissive lamp heads). `cw_lightmap_use_reflection(cw_lightmap_gi_data(gi), material, 1)` gives it to a puddle or a wet patch: one texture read, where screen-space reflections draw a G-buffer and trace every frame.
- `cw_lightmap_irradiance_below` returns the baked light on the ground under a point: a crowd that walks through a floodlight's pool or a wall's shadow takes that light (as a colour on its instances), with no GPU cost.

Moving things need their shadows and their lights shown on a ground that takes no real-time light: [splats.h](header:splats.h) draws blob shadows (the colour below multiplied by 1 - strength, stretched away from a low sun) and light pools (added light under a moving lamp, a muzzle flash, a fire: `cw_splats_add_lights` puts one under every lit, unbaked OmniLight3D), one instanced draw of ground discs per set.

A late wave of a 2000-enemy top-down game at 1080p on the 890M, its yard of six floodlights baked this way (with SSR, SSAO and volumetric fog replaced by the reflection cube, blob shadows and light shaft cards): 12.2 to 3.8 ms of GPU time on GL, 11.1 to 3.8 on WebGPU.

Sample: `samples/lightmap_yard` (a dusk yard: the sun and two bake-only floodlights in the lightmaps, a row of instanced posts in the bake, a puddle on the reflection cube, a ball lit by the ground under it with a blob shadow and a light pool). Gate `lightmap-yard`.

## Reflections and bounce light

[gi.h](header:gi.h) adds two node types that make a scene light itself, both captured from the scene on the GPU, on GL and WebGPU, natively and in carts:

- **ReflectionProbe3D**: shiny things reflect their real surroundings instead of the sky. A probe renders the scene around a point into a prefiltered radiance map, three.js r187's `PMREMGenerator.fromScene` with its `size`, `position` and `renderTarget` options, and every lit surface in its box reflects that. With `box_projection` on, reflections are traced to the box's faces (r187's `getParallaxCorrectNormal`), so a polished floor shows the wall where the wall is: use it in rooms and corridors and size the box to the room, a little larger so the floor and walls sit inside it. `origin_offset` moves the capture point off the box centre (out of a pillar, up from the floor) and the projection still lines up. Overlapping probes blend as Godot's do: inside its box a probe's weight falls to 0 over `blend_distance` at the faces, the probes' reflections are averaged by weight, and the sky fills the rest. Each object takes the two probes whose boxes reach it. `ambient` makes the probe the surface's diffuse ambient light too, in place of the sky's.
- **LightProbeGrid3D**: bounce light, three.js r187's `LightProbeGrid`. A grid of probes over a box, each rendering the scene into six small faces (`cube_size`, 8 pixels by default) that are projected onto L2 spherical harmonics on the GPU. Every lit surface inside the box adds the grid's irradiance at its position: a red wall tints the floor and ceiling next to it, a lit floor lifts the underside of a table. The grid bakes at load (`bounces` extra sweeps, each one more bounce, since each sweep sees the light the last one stored); `update_mode = INCREMENTAL` then refreshes `probes_per_frame` probes every frame for lights that move. Like r187's it has no visibility term, so light leaks through walls thinner than the probe spacing: keep a grid inside one room, or keep the walls thick.

The room in `samples/gi_room` has both:

@include samples/gi_room/assets/main.cwscene#RoomProbe..Bounce

How it fits together:

- Captures run in the SHADOW phase (the "gi" pass, after the sky's environment is made), so this frame's scene draws with them. Each probe keeps two targets, drawn into one while the scene samples the other, and nothing is allocated after the first capture. `update_mode` picks when a probe captures: ONCE (its first frame, and again when the sky is re-rendered), ON_DEMAND (`cw_reflection_probe_update` or `call <probe> update`), or EVERY_N_FRAMES (`update_every`). A capture sees what `cull_mask` lets it see (three.c object layers): give a cheap stand-in of a level a layer the camera does not draw and the probe captures the stand-in. A split-screen frame captures probes and updates the probe grid once, in its first view (frames, not views, count for EVERY_N_FRAMES and the grid budget); each view still draws its own screen-space G-buffer.
- The shading is r187 TSL in a three.c extension, the "gi" shading part ([shading.h](header:shading.h)): every Standard and Physical material in a tree with a probe or a grid carries it, and the app's program tables need its states (`standard+senv+l1101+xgi`; `tools/programs.mjs` adds them like any other). A material carries more than one engine part this way, composed by name (`xterrain_gi`). The gi part composes after the lighting parts, the clustered lights' `cluster` and the forward sun's cascades `csm` (`xcluster_gi`, `xcsm_gi`, `xcsm_plain_gi` on Very Low), and with lit mesh decals, skin shading and a game's own surfaces ([Writing your own surface](#writing-your-own-surface): `xdissolve_gi` on forward lighting, `xcluster_csm_dissolve_gi` on clustered). A part a game wrote by hand without saying it is a surface (`cw_shading_part` alone) takes the probes only together with a lighting part; while the tree has a probe or a grid the log names each material the probes leave alone, with the fix. A mesh whose material carries the gi part is a draw of its own: each mesh picks its own probes, so the part's hook reads the object.
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

Screen-space reflections and bounce light are PostProcess settings on top of the probes, three.js r187's `SSRNode` and `SSGINode` at half resolution:

- **`ssr`**: one mirror ray a pixel is marched across the screen against the depth (r187's first-generation SSR, with its binary refinement), so a polished floor shows the chair standing on it, which a probe captured from elsewhere cannot. Metalness weights it, roughness blurs it, and it is added over the image as r187's example adds it. `ssr_max_distance` is how far a reflection reaches (view units, 1 by default: set it to the room's size), `ssr_thickness` how deep a surface counts as hit, `ssr_quality` the march steps per pixel of the ray. What is off screen cannot be reflected: the probes still cover it.
- **`ssgi`**: r187's SSILVB (a port of SSRT3): per pixel, a few slices of the depth around it are walked and every newly occluded sector of each slice takes the light of the surface occluding it, giving bounce light and ambient occlusion from what is on screen. It is composited as r187's example does, colour x AO + diffuse colour x bounce light, and denoised over 5 x 5 trace texels with depth- and normal-aware weights (r187 relies on TRAA; with the pipeline's TAA on the slices also rotate frame to frame as r187's do).

Both read a G-buffer the "gi" pass draws after the probes: the scene a second time at half resolution with a program of its own (the "gibuf" shading part) that writes the view normal, roughness and metalness instead of the lit colour and does no lighting (and, for SSGI, a third time for the diffuse colour). Only surfaces with the part (every Standard and Physical material of a tree with a probe, a grid, SSR or SSGI) are in it; anything else is not reflected and gets no bounce light. The G-buffer draws holding the shadow maps (it reads none of them): the view's own render draws them, once a frame.

| | Low | Medium | High | Ultra |
|---|---|---|---|---|
| SSR | off | half the march steps | as authored | 1.5 x the steps |
| SSGI | off | off | off | as authored |

The cost, GPU ms per frame on an AMD Radeon 890M (integrated) at 1280 x 720, `samples/gi_room` through `tests/gi-perf.mjs` (GL, the probes' pass averaged over frames with and without a capture). Measured on a loaded machine (load 18, the GPU's clock lower than on a quiet one: the frame without probes took 2.85 ms here and 1.8 on a quiet machine, so read the rows against that one; [Profiling](guide:profiling) says why):

| Preset | gi pass (probes, grid, G-buffer) | SSR | SSGI | Whole frame |
|---|---|---|---|---|
| Low | 0.1 | off | off | 2.9 |
| Medium | 1.1 | 1.1 | off | 4.8 |
| High | 1.4 | 1.5 | off | 5.6 |
| Ultra | 4.7 | 1.7 | 7.4 | 19.8 |
| No probes, grid, SSR or SSGI (High) | | | | 2.85 |

The gi pass on High is two probe captures every other frame, each at the probe's own `resolution` (r187's PMREM would make it at least 256 pixels a face) and each spread over the two frames, and the half-resolution G-buffer. A capture's filtered levels are drawn together in one target (three.c 0.187.6), which with the honoured size and the spreading took a mirror-ball table's worst frame (a probe of 128 refreshed every third frame) from 8.5 to 4.3 ms on GL and from 10.0 to 4.9 ms on WebGPU at 1080p, and the probe's share of the average frame from 1.24 to 0.33 ms (GL) and 1.74 to 0.38 ms (WebGPU); Ultra doubles the capture size and budget and draws the G-buffer twice (SSGI needs the diffuse colour). Most of SSGI's time is the trace's fetches of the lit colour and the normal at the scattered steps, so it reads the colour from a half-resolution copy and the normals from a two-channel one, and its loops have fixed counts the compiler unrolls. A Vega 8 has about half the 890M's shader rate: expect about twice these numbers, so Low to High fit a 60 Hz frame there and Ultra (SSGI above all) is for discrete GPUs.


## Far views: the reversed depth buffer

`cw_app_desc.reversed_depth` stores distance the other way round in a 32-bit float buffer, which spreads precision evenly out to the horizon. Far surfaces stop flickering through each other, and a camera's `far` can sit hundreds of kilometres out with `near` still close: `samples/far_view` has two walls a metre apart 100 km away, which fight on forward depth and do not here. The picture is otherwise the same (gate `reversed-depth`).

It takes effect on WebGPU. A GL run stays on forward depth, since a cart's GL has no way to change it, so a game that ships on both should not rely on a far plane only WebGPU can hold. Shadow maps stay on forward depth and no program table grows. A pass of your own with WGSL against the scene's depth checks `cw_gpu_reversed_depth()`: clear to 0, compare with greater-equal, write clip z as `(w - z) / 2`. Its WGSL has `CW_REV` and `CW_FAR` declared; passes that read the linear depth need no change.

## Characters: skin, glTF materials, faces

Three pieces make characters look like people rather than painted plastic. `samples/character_skin` shows all of them on six instances of one generated glTF.

**Skin shading** ([skin_shading.h](header:skin_shading.h)) is a cheap subsurface look. It adds two terms to a MeshStandardMaterial or MeshPhysicalMaterial for every light that reaches it (the scene's lights, clustered lights, the cascaded sun), and keeps the rest of the material: sheen, clear coat, maps.

- **Wrap**: diffuse light reaches past the line where the surface turns from the light, red furthest, so the shadow side of a face goes soft and warm instead of grey. `scatter` is the colour that travels furthest; `wrap` how far (0 to 1).
- **Translucency**: light from behind shows through thin parts (ears, nostrils, fingers) as a warm glow, strongest when looking toward a light past an edge (three.js r187 MeshSSSNodeMaterial's term). `translucency` sets its strength, `distortion` how much the surface bends it, `power` how tightly it gathers.

There is no screen-space pass, no extra target and no texture: a few instructions per light per skin pixel. A light's shadow darkens both terms like the rest of its light, so the glow through an ear needs a light the head does not shadow (a rim light behind the character).

Turn it on from code with [cw_skin_shading_apply](api:cw_skin_shading_apply) on a material (or [cw_skin_shading_apply_named](api:cw_skin_shading_apply_named) for every material of that name under a model), or from the file: a glTF material with `KHR_materials_diffuse_transmission` (its factor becomes the translucency, its colour the scatter colour) or with `"extras": {"skin_shading": true}` (or an object with `scatter`, `wrap`, `translucency`, `distortion`, `power`, `ambient`) gets it when it loads through the resource system. Very Low and Low draw skin as plain PBR (`cw_quality_scale("skin")` is 0 there) and pay nothing; Medium and up draw it.

**glTF materials** load as r187's GLTFLoader builds them: each keeps its name (`t3_material.name`), and the factors of `KHR_materials_sheen`, `clearcoat`, `specular`, `ior`, `transmission`, `volume`, `emissive_strength`, `iridescence` and `anisotropy` make a MeshPhysicalMaterial (the clear coat normal texture too; the other extension textures are not read yet). A model generator can put cloth sheen, leather clear coat or eye gloss in the file and the game finds a material by name, with no node naming tricks.

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

The rim is per material: glTF instances share their file's materials, so clone one per character for a per-character colour (MeshInstance3D primitives of one look share one material in a tree too). It works with every other part (clustered lights, the cascaded sun, skin shading, GI) and costs a few instructions per pixel of the material; shadow casters are untouched and every preset draws it. Measured on a Radeon 890M in a split-screen shooter at 1920 x 1080, High preset, four to eight rimmed skinned characters (scene pass GPU ms, rims on against off, two interleaved runs each): GL one view 5.26 to 5.66 against 5.15 to 5.33, four views 5.2 to 5.5 against 5.21 to 5.34; WebGPU one view 4.08 to 4.7 against 4.36 to 4.75, four views 4.68 to 4.71 against 4.23. That is under the run-to-run noise on one view and about 0.45 ms on WebGPU with four views; a Vega 8 (about a quarter of an 890M) should stay under 1 ms. `samples/rim_light` shows two lines of figures from 3 m to 40 m with red and blue rims and a pulsing one; its gate is `rim-light`.

**Vertex cache order.** `cw_geometry_optimize_vertex_cache` reorders an indexed geometry's triangles for the GPU's post-transform cache (meshoptimizer's optimizeVertexCache) and reports the vertices shaded per triangle before and after. Meshes exported in modelling order can shade each vertex two or three times; already ordered ones (most of a character exporter's) gain little. Opaque meshes draw the same picture.

### Toon shading

A toon (cel) shaded surface is lit in flat tones with hard edges between them instead of a smooth falloff: three.js r187's MeshToonMaterial ([toon.h](header:toon.h)). The light a surface gets is read from a ramp with one tone a texel, so three bands paint shadow, mid tone and light. In a scene it is one property of a `MeshInstance3D`:

```
[node name="Hero" parent="." type="MeshInstance3D"]
mesh = res(0x1234)
shading = TOON
toon_bands = 3
```

`shading = TOON` applies to the node's primitive or to every material of its mesh resource or glTF scene (two nodes showing the same file still share materials). `toon_bands` is the number of tones, 2 to 16; below 2 the surface has three.js's own two soft-edged tones (0.7 and 1 of the light). Both can change in play. From code, for a model instantiated with `cw_gltf_instantiate`:

```c
cw_toon_apply_tree(model_root, 3);            /* every lit material under the model, three tones */
cw_toon_restore_tree(model_root);             /* back to the file's materials */
t3_material *m = cw_toon_material(src, 4);    /* one material; yours to release */
```

A toon material keeps the surface's colour and colour map, emissive colour, map and intensity (a glow that ignores the light: fuses, eyes, signs), normal map, ambient occlusion, light map, alpha map and cutout, vertex colours, transparency and sides. It has no roughness, metalness, reflections or highlights. Every light lights it, with shadows: ambient and hemisphere light, three.c's lights, the clustered omni and spot lights and the cascaded sun. Each light is banded on its own, so two lights give two sets of bands, and a lamp's falloff with distance stays smooth inside a band. For a ramp of your own (uneven steps), set `gradient_map` on the material: a DataTexture with both filters nearest, as three.js asks; its red channel scales the light.

It is cheaper than the physically based materials (no highlight, no environment lookup; one texture read per light). The plain lighting model of the Very Low preset, rim lights and skin shading are for the physically based materials and leave a toon one as it is. `samples/toon_shading` shows banded spheres, an emissive cylinder and two toon trees from one glTF beside their physically based twins; its gate is `toon-shading`.
### Writing your own surface

A game often needs a surface no stock material has: something that burns away, rock with no uv layout, grass that leans in the wind, a force field, lava, a toon ramp. A **surface** ([surface.h](header:surface.h)) is the way to write one and keep everything the engine does with light. You write what the surface *is* (its colour, normal, roughness, glow, holes and vertex positions) as three.js r187 TSL; the engine goes on lighting it: three.c's lights and shadows, the hundreds of clustered lamps, the sun's cascades, reflection probes and the light probe grid, screen-space reflections, fog and decals. A material with a surface whose effect is at zero draws the same pixels as the plain material (`tests/custom-surface.mjs` holds that on GL and WebGPU, on forward and clustered lighting and down to the Very Low preset).

A surface has two sides with the same name and the same parameter list.

**The module**, `shaders/<name>.mjs` in the game, exports `name`, `params` and `surface()`:

```js
// shaders/dissolve.mjs: the mesh burns away along a noise field, with a glowing edge
export const name = 'dissolve';
export const params = { amount: 'float', edge: 'color', scale: 'float' };

export function surface({ TSL, p, base, caster }) {
  const { float, vec3, floor, fract, sin, dot, mix, smoothstep, positionGeometry } = TSL;
  const hash = (q) => fract(sin(dot(q, vec3(127.1, 311.7, 74.7))).mul(43758.5453));
  const noise = (q) => {                                  // value noise of the mesh's own position
    const i = floor(q), f = fract(q), w = f.mul(f).mul(float(3).sub(f.mul(2)));
    const c = (x, y, z) => hash(i.add(vec3(x, y, z)));
    return mix(mix(mix(c(0, 0, 0), c(1, 0, 0), w.x), mix(c(0, 1, 0), c(1, 1, 0), w.x), w.y),
      mix(mix(c(0, 0, 1), c(1, 0, 1), w.x), mix(c(0, 1, 1), c(1, 1, 1), w.x), w.y), w.z);
  };
  const n = noise(positionGeometry.mul(p.scale)).mul(0.98).add(0.01).toVar();
  const mask = n.greaterThan(p.amount);                   // false cuts the pixel out
  if (caster) return { mask };                            // the shadow has the same holes
  const glow = float(1).sub(smoothstep(p.amount, p.amount.add(p.edge.a), n)).mul(smoothstep(0, 0.02, p.amount));
  return { mask, emissive: base.emissive.add(p.edge.rgb.mul(glow)) };
}
```

`surface()` gets `p` (the parameters as nodes), `base` (what the material or an earlier part gives: `color`, `normal`, `roughness`, `metalness`, `emissive`, `opacity`, and `position`, the vertex before this surface moved it), `caster` (true while the shadow caster's program is being made: only `position` and `mask` count there), `instanced`, `worldPosition` and `toLocal(v)` (below), `TSL` and `THREE`. It returns any of:

| key | what it sets | r187 |
|---|---|---|
| `color` | the albedo, vec3 or vec4 (alpha is the opacity) | `colorNode` |
| `normal` | the shading normal, view space | `normalNode` |
| `roughness`, `metalness` | float | `roughnessNode`, `metalnessNode` |
| `emissive` | light the surface gives off, vec3 (above 1 blooms) | `emissiveNode` |
| `ao` | ambient occlusion, float | `aoNode` |
| `opacity` | float (the material must be `transparent`, or use `mask`) | `opacityNode` |
| `mask` | bool: false cuts the pixel out of the picture, the depth prepass and the shadow | `maskNode` |
| `position` | the vertex, in the mesh's own space | `positionNode` |
| `output` | vec4, the last word on the lit and fogged colour (read `TSL.output`) | `outputNode` |

Everything else is the lighting's. Return only what the surface changes: a key left out keeps the material's own value, so a surface composes with the material's maps and with rim light, skin shading and the other parts. Anything not in the table is refused when the table is generated, by name.

Parameter types: `'float'`, `'vec2'`, `'vec3'`, `'vec4'`, `'color'` (linear rgba, a vec4 in the module), `'mat4'`, an array `{ type: 'vec4', count: N }` (2 to 32; `p.spots` is then a JavaScript array of nodes: read `p.spots[i]` or `p.spots.element(i)` with a number, and write the loop in JavaScript), and a texture `{ texture: true, wrap: 'repeat' | 'clamp' | 'mirror', filter: 'linear' | 'nearest', mipmaps: true, srgb: true }` (read with `TSL.texture(p.albedo, uv)`; filtering and wrapping are part of the program, so give the texture you set from C the same settings). A parameter is in a program only if `surface()` read it there.

**The C side** registers the same list once, at start, and then puts the surface on materials:

```c
#include "cartwheel/surface.h"

static const cw_surface_param dissolve[] = {
  { "amount", CW_SURFACE_FLOAT },
  { "edge", CW_SURFACE_COLOR, .value = { 6.0f, 1.6f, 0.2f, 0.06f } },   /* the default */
  { "scale", CW_SURFACE_FLOAT, .value = { 7.0f } },
};
cw_surface_register(&(cw_surface_desc){ .name = "dissolve", .params = dissolve, .param_count = 3,
                                        .node_type = "DissolveSurface3D" });
...
cw_surface_apply(material, "dissolve");                        /* or cw_surface_apply_tree(model_root, "dissolve") */
cw_surface_set_float(material, "dissolve", "amount", 0.4f);    /* any time: a memory write, sent with each draw */
```

Each parameter has its registered default, one value for every material of the surface (`cw_surface_set_global`: time, wind, a shared texture) and a value per material (`cw_surface_set`, `cw_surface_set_float`, `cw_surface_set_at` for one element of an array, `cw_surface_set_texture`), which wins; `cw_surface_unset` goes back. A texture can be replaced at any time, and its pixels changed in place: write `t->pixels`, then `t->version++`, and the next draw uploads it. A wrong name or width is refused and logged once.

With `node_type` the surface is also a **node type** whose properties are the parameters (floats, vectors, colours; a texture is a resource). Put one under a `MeshInstance3D` (or any `Node3D`) and that node's meshes draw with the surface on materials of their own, driven by the node's properties, so a scene file sets them and an AnimationPlayer keys them like any other property:

```
[node name="Orb" parent="." type="MeshInstance3D"]
primitive = SPHERE
metalness = 1
roughness = 0.1

[node name="Dissolve" parent="Orb" type="DissolveSurface3D"]
amount = 0.42
edge = color(6.0, 1.6, 0.2, 0.06)
```

Register surfaces in `cw_main`, before `cw_scene_app` returns, when a scene file uses their node types. Freeing the node gives the meshes their plain materials back. A matrix or an array parameter has no property: set it from code. While a surface node is on a mesh, change the mesh's look through the surface's parameters: the node holds a copy of the material made when it first saw it.

**The program table** names the module, and `tools/programs.mjs` does the rest: it finds the states the game draws with the surface in them and captures their programs like any others ([Program tables](#program-tables)).

```
# programs/mygame.txt
# extend: ../shaders/dissolve.mjs
# extend: ../shaders/rock.mjs
```

A surface's name is part of every state it draws (`standard+l0001+xcluster_csm_dissolve_gi`), which is why it is short: lower-case letters and digits, at most 10.

**A second example: triplanar rock with wind sway.** Two surfaces, since one decides the fragment and the other the vertices. The rock reads a tiling texture from three sides by world position, so a mesh needs no uv layout and the stone does not stretch on any face; moss patches come from an array:

```js
// shaders/rock.mjs
export const name = 'rock';
export const params = {
  amount: 'float', repeat: 'float', sharp: 'float',
  albedo: { texture: true, wrap: 'repeat', filter: 'linear', mipmaps: true, srgb: true },
  moss: { type: 'vec4', count: 4 },              // xyz: a patch's centre in the world, w: its radius
  mossTint: 'color',
};
export function surface({ TSL, p, base, caster }) {
  if (caster) return {};                         // nothing of it changes the shadow
  const { texture, vec4, float, mix, abs, pow, max, smoothstep, length, positionWorld, normalWorld } = TSL;
  const q = positionWorld.mul(p.repeat);
  const w0 = pow(abs(normalWorld), p.sharp), w = w0.div(w0.x.add(w0.y).add(w0.z));
  const stone = texture(p.albedo, q.zy).rgb.mul(w.x).add(texture(p.albedo, q.xz).rgb.mul(w.y)).add(texture(p.albedo, q.xy).rgb.mul(w.z));
  let cover = float(0);
  for (let i = 0; i < 4; i++) {
    const m = p.moss[i], r = max(m.w, 0.0001);   // (a radius of 0 is no patch; never smoothstep between equal edges)
    cover = max(cover, float(1).sub(smoothstep(r.mul(0.6), r, length(positionWorld.sub(m.xyz)))));
  }
  const rock = mix(base.color.rgb, base.color.rgb.mul(stone).mul(2.2), p.amount);
  return { color: vec4(mix(rock, p.mossTint.rgb, cover.mul(p.amount).mul(0.85)), 1),
           roughness: mix(base.roughness, float(1), cover.mul(p.amount)) };
}
```

```js
// shaders/sway.mjs: each vertex leans with a gust that travels over the world, more the higher it sits
export const name = 'sway';
export const params = { time: 'float', wind: 'vec3' };   // wind: lean in metres, gusts a second, share of the lean along z
export function surface({ TSL, p, base, toLocal, worldPosition }) {
  const { vec3, float, sin, positionGeometry } = TSL;
  const h = positionGeometry.y.add(0.5).clamp();         // 0 at the foot, 1 at the top of a unit-high mesh
  const phase = worldPosition.x.mul(0.9).add(worldPosition.z.mul(0.6));
  const push = sin(p.time.mul(p.wind.y).mul(6.2832).add(phase)).mul(p.wind.x).mul(h.mul(h));
  return { position: base.position.add(toLocal(vec3(push, float(0), push.mul(p.wind.z)))) };
}
```

```c
cw_surface_register(&(cw_surface_desc){ .name = "sway", .params = sway, .param_count = 2, .moves_vertices = true });
...
float t = (float)tick / 60.0f;
cw_surface_set_global("sway", "time", &t, 1);            /* once a frame, for every swaying material */
```

Things to know when a surface moves vertices:

- Say so (`moves_vertices = true`). The shadow caster and the depth prepass run the same `position`, so shadows and early depth follow the motion. A mesh decal cannot be clipped from such a mesh (the CPU does not see the moved surface); a screen decal still lands on it.
- `base.position` is in the mesh's own space, where a displacement is scaled and turned with the mesh: a reed scaled to 9 cm across would lean 9 % of what you asked. Write the displacement in world metres and pass it through `toLocal()`. Meshes the renderer draws as instances are placed after `position`, so there `toLocal` returns the vector as it is; `instanced` says which program is being made. An `InstancedMesh` of your own that is itself rotated or scaled needs its own conversion.
- Use `positionGeometry` (the mesh's own, unmoved vertex) for things that must not depend on how the mesh is drawn, such as the height along a reed, and `worldPosition` for a field that spans meshes, such as the gust.
- Bounds are the unmoved mesh's. Keep displacements small against the mesh, or grow its bounding sphere, or culling will drop it at the screen's edge.

**What composes, and what it costs.** A surface is a shading part ([shading.h](header:shading.h)) with the trait `CW_PART_SURFACE`, joined by name with the engine's own parts on the same material: `xcluster_csm_dissolve_gi` is the clustered lamps, the cascades, the surface and the probes in one program. A material takes up to 8 parts and the name up to 63 characters; a material that would pass either keeps what it had and the log says which parts it could not take. One program is captured per combination the game really draws, per backend, not per combination that could exist. The sample is close to a worst case, since its gate walks every lighting mode: three surfaces on forward and clustered lighting, with and without cascades and probes, on every preset, with screen-space reflections, come to 99 states, 70 of them with a surface in their name (half of those are the small shadow caster programs). That is 2.2 MB of generated C for GL and WebGPU together against 0.85 MB for the same room with plain materials, and 264 KB more in the cart (3.11 MB against 2.85 MB): under 4 KB a state. A game that stays on one lighting mode draws a fraction of that: per surface and material kind, a lit program and a caster program for each preset family it runs on. Per frame a surface costs its own shader work and one hook call a draw. On a Radeon 890M at 1920 x 1080 the sample's frame takes 1.4 to 2.4 ms of GPU time on GL and 1.2 to 2.3 ms on WebGPU from Very Low to Ultra, and the three surfaces add between nothing and 0.14 ms to it against the same room with plain materials (medians of four interleaved rounds: +0.07, +0.03, +0.14, 0.00, -0.14 ms on GL and +0.04, +0.04, -0.03, +0.06, +0.10 ms on WebGPU, Very Low to Ultra; a round varies by about 0.2 ms; `tests/custom-surface-perf.mjs` prints the table with the load and the GPU clock). A Vega 8 is about a quarter of an 890M: expect under 0.6 ms there. What you write in `surface()` is what you pay for: the triplanar rock is three texture reads a pixel, the dissolve eight hashes.

Meshes that share geometry and a material with a surface are drawn as one instanced draw when every part on the material allows it. A surface does. The clustered lamps' and cascades' parts do when `LightingSettings.batching` is on, and the gi part (a scene with a reflection probe or a probe grid) never does, since each mesh picks its own probes: in such a scene every mesh is a draw of its own, with or without a surface. `custom_surface_field` (the same program on a scene with no probes) draws two hundred reeds with the sway surface as one instanced draw, as it draws two hundred plain ones. Their shadow casters are still drawn one by one: three.c does not batch casters yet.

Limits, said plainly: a surface is for the lit mesh materials (Standard, Physical, Lambert, Phong); a mesh decal on a surface that cuts holes is drawn over the holes too; the G-buffer of screen-space reflections and bounce light keeps a surface's vertices, cut-outs, normal and roughness; lightmap-baked meshes (`gi_mode = STATIC`) take their own copy of the material and are not a place for an animated surface. The older way of writing a part by hand (an `apply()` module and a fill hook through `cw_shading_part`, which the engine's own parts use) still works and still composes with the clustered lamps and the cascades; give it `cw_shading_part_traits(name, CW_PART_SURFACE)` for the probes, the G-buffer and decals to take it too.

`samples/custom_surface` is all of this in one room: the dissolving chrome orb, the triplanar slab with its sticker, forty swaying reeds, eight clustered lamps, a sun with three cascades, a reflection probe and a probe grid. Its gates: `custom-surface` (the same picture and state on every host, GL and WebGPU) and `custom-surface-lighting` (`tests/custom-surface.mjs`: plain materials against surfaces pixel for pixel on four lighting setups, the effects, the G-buffer, the draw counts, each with a control that must fail).

## Visibility: culling, LOD and impostors

Without a VisibilityManager, three.c's per-object frustum test is all there is. Add one ([visibility.h](header:visibility.h)) and the "visibility" pass culls, picks LODs and bakes impostors before the scene draws; culled meshes are hidden for the frame and restored in PRESENT (and after each view of a split screen, so one player's culling never hides meshes from the next view's shadows, GI or culling), so game code never sees the flags change.

- **Frustum culling** walks a BVH over `static` meshes four boxes at a time, plus a flat SIMD pass over moving ones. Shadow casters outside the view are swept along the light so their shadows stay.
- **Occlusion culling** queries each candidate's bounding box against the depth the scene just drew and uses the answer a frame later. Boxes are inflated by the camera's motion over that latency, and new or late objects count as visible, so nothing pops in. The guard behind that is conservative: while the camera is moving and within a few steps of a drawn static box, no queries are issued at all, so a fast camera skimming past props gets frustum, distance and portal culling only (see `occlusion_guard` in visibility.h).
- **Portals**: Room3D boxes joined by Portal3D openings; from inside a room you see other rooms only through portals, each narrowing the view. A disabled portal is a shut door.
- **LOD** picks the coarsest level whose simplification error stays under `lod_error_px` on screen: cwpack LOD chains for meshes and glTF, built-in tessellations for spheres and cylinders. `visibility_range` hides a mesh beyond a distance.
- **Impostors**: past `impostor_distance` a mesh draws as an octahedral impostor (64 views baked into an atlas on first use, shared by identical instances), crossfaded over `impostor_fade` metres. GL and WebGPU bake the same atlas (1024 x 1024 albedo with coverage, and normals, with mip levels down to 64 x 64) and draw the same impostors, lit at draw time by the scene's directional, ambient and hemisphere lights; a mesh that is fading out draws after the impostors on both, so the impostors behind it never show through.

Costs per frame: the BVH walk, one pass over moving meshes and one over visible ones, one query draw per queried mesh, one instanced draw per impostor atlas; the first use of an atlas renders the mesh 128 times (64 views, albedo and normals). Measured on the Radeon 890M (`samples/vis_impostor`, 1920 x 1080, 49 trees as impostors; `CW_TEST_IMPOSTOR_BAKE_MS=1` logs each bake, `CW_GPU_TIMERS=1` gives `vimp.ms_impostors`): a bake takes about 1 ms on GL and 2 to 3 ms on WebGPU, and the first bake of a run, which also builds its programs, 2.5 ms and 9 to 12 ms; a Vega 8 should expect three to four times that, once per atlas. Drawing the 49 impostors costs 0.16 ms (Low) to 0.37 ms (Ultra) of GPU time on GL and 0.11 to 0.21 ms on WebGPU. Impostors pay off for meshes that cost more than that to draw: dense foliage, many materials, many instances far away. The low-poly trees of that sample are cheaper as meshes (the scene pass saves 0.07 to 0.17 ms), so measure before turning them on for simple props.

`samples/vis_city` is about 4500 meshes (buildings, lamps with visibility ranges, LOD'd trees with impostors, houses with portal doorways) walked by a camera; its `check` mode renders a second, unculled copy of the city and counts pixels that differ, so a cull that pops fails the gate. `samples/vis_impostor` sweeps the impostor distance across a grove and measures the largest frame-to-frame change. The showcase also uses three.c's own `LOD` objects directly for its terrain chunks (`samples/showcase/world.c`).

## Render layers and a first person weapon

Every `Node3D` has `layers`: a bit mask, bit n = layer n, 1 by default, -1 for every layer (lights are on every layer: a camera's layers pick what it draws, never what is lit). Setting it applies to the node and everything under it, including nodes added later, unless they set their own. A `Camera3D` draws the layers in its `cull_layers` (1 by default).

A first person weapon needs two things a plain camera cannot give: its own field of view (at 90 degrees a gun held with the world's lens stretches into the screen's corner) and no clipping into walls. Put the weapon on layer 2 and give the camera `viewmodel_layers = 2` and a `viewmodel_fov` (50 to 60 reads well):

```c
cw_node_set(gun, "layers", cw_val_int(2));
cw_node_set(camera, "viewmodel_layers", cw_val_int(2));
cw_node_set(camera, "viewmodel_fov", cw_val_float(52));
```

After the scene, the `viewmodel` pass draws those layers with a second camera at the same pose and the viewmodel lens, over the same colour and depth. It draws in the near end of the depth range, so the weapon always lands in front of the world and sorts against itself, while the scene's depth stays right everywhere else for the passes that read it (ambient occlusion, decals, particles). The weapon is lit by the scene's lights and shadows, and costs one more scene submission of the few objects on those layers (a split screen pays it once per view). The pass runs on GL and on WebGPU. The background is not redrawn. With the next three.c (v0.186.22 does not have it yet), MSAA on WebGPU leaves the scene pass's samples unresolved and the viewmodel pass resolves the colour once at its end (each render pass would resolve its own), which saves a full-screen resolve: about 0.5 ms a frame at 1920 x 1080 on a Radeon 890M, one view or four, the picture the same to the bit (`tests/split-gpu-share.mjs`).

In a split screen, hide the other players' weapons for each view with the pipeline's view hook (`cw_pipeline_set_view_hook`): each view draws every object on its layers.

## Many copies of one mesh: MultiMeshInstance3D

A [MultiMeshInstance3D](node:MultiMeshInstance3D) draws many instances of one mesh from a buffer the game fills: a forest, a field of rocks, a stadium of seats, a swarm. It is one node where the alternative is one [MeshInstance3D](node:MeshInstance3D) per copy, each with a node, a three.c object and a transform to update. The module is `cartwheel/multimesh.h` ([reference](header:multimesh.h)).

```text
[node name="Field" parent="." type="MultiMeshInstance3D"]
chunk_size = 24
cast_shadow = true
use_colors = true

[node name="Stamp" parent="Field" type="MeshInstance3D"]
position = vec3(0, 0.6, 0)
size = vec3(0.5, 1.2, 0.5)
```

The mesh is a node: `source` names it, or it is the first MeshInstance3D child. Every mesh under the source is drawn per instance with its own material, so a primitive, a mesh resource and a glTF scene of several meshes all work, and the source's own transform applies to every instance. The source itself is hidden. The buffer is filled from C:

@include samples/multimesh/main.c#fill

[cw_multimesh_set_transform](api:cw_multimesh_set_transform), [cw_multimesh_set_color](api:cw_multimesh_set_color) and [cw_multimesh_set_custom](api:cw_multimesh_set_custom) write one instance; [cw_multimesh_transforms](api:cw_multimesh_transforms) is the array to write in a loop, followed by [cw_multimesh_touch](api:cw_multimesh_touch). `fill = GRID` with `fill_count` and `fill_spacing` fills a grid from the scene file alone.

How it compares with what was already there. The engine already merges identical MeshInstance3D nodes into instanced draws by itself, and [TerrainScatter3D](node:TerrainScatter3D) generates ground cover from a density rule. Use automatic instancing when the copies are real nodes you move and query one by one, up to a few thousand; TerrainScatter3D when a rule places them on terrain; MultiMeshInstance3D when your code owns the list and it is long.

**Culling.** The instances are sorted into cubic cells of `chunk_size` metres. Each cell is one draw per mesh with its own bounds, culled against the camera and against every shadow light's view, so cells out of sight cost nothing but their test. `visible_instances`, `chunks` and `visible_chunks` report what the main camera saw last frame ([cw_multimesh_get_stats](api:cw_multimesh_get_stats) has the draws and the last rebuild time too). Small cells cull tightly and cost more draws; 16 to 32 m suits things seen from eye height. `chunk_size = 0` is one draw for everything, for a swarm that moves every tick and is all on screen: its buffer is rewritten in place with no sort:

@include samples/multimesh/main.c#swarm

**Lighting.** Instances are ordinary instanced meshes: lit by the scene's lights in forward and clustered mode, casting and receiving shadows from single maps, the atlas and cascades, and taking reflection probes and the probe grid like any moving mesh. They are not lightmapped. `use_colors` multiplies each instance's colour into the material's. Custom data is four floats an instance for game code; no built-in material reads it.

**Cost.** 100000 instances of a box (12 triangles each), about 56000 of them in view, under a shadowed sun at 1920 x 1080, on the 890M (Ryzen AI 9 HX 370) through romdev, High preset; milliseconds a frame the field adds over the same scene without it. The machine was shared while measuring (load average 25 to 35, other GPU work running), so read these to about 0.2 ms:

| | GL | WebGPU |
|---|---|---|
| GPU, 24 m cells (196 cells, 104 in view, casting shadows) | +0.7 | +1.0 |
| GPU, the same without casting shadows | +0.3 | (not measured cleanly) |
| GPU, one draw for all 100000 (`chunk_size = 0`: nothing culled) | +1.2 | +2.0 |
| CPU in the cart, 24 m cells | +0.2 | +0.7 |

A buffer that does not change costs no CPU beyond one sphere test per cell and the draws. Changing it rebuilds the cells at the next render: a sort and a copy of every instance, 36 to 55 ms for 20000 and about 96 ms for 100000 in a development cart on that machine (a one-off; do it at load, or keep what changes every tick in a `chunk_size = 0` node, whose rewrite in place is a copy with no sort: the 400-instance swarm costs under 0.05 ms a tick). The quality presets do not change a MultiMesh: thin a field yourself with `cw_quality_count` and `visible_instance_count` (instances are drawn in buffer order, so the first N of a shuffled buffer is an even thinning). On a Vega 8 (about a third of the 890M) expect about 3 ms of GPU for this field with shadows; fewer, larger cells trade culling for draws there.

`samples/multimesh` has a 20000-instance field panned by the camera, a grid filled by the scene file and a 400-instance swarm rewritten every tick. The gate `multimesh` holds that every instance is drawn from one build, that the pan changes what is in view and never shows all or none of the field, and that the swarm stays one draw; its control sets `visible_instance_count = 0` and fails. `unit-multimesh` covers the buffer, the cells and their bounds, what rebuilds when, the cells a camera sees and the scene-file round trip.

## Rendering to a texture: SubViewport

A [SubViewport](node:SubViewport) renders a picture into a texture, and that texture goes wherever a texture goes: on a mesh in the world, on a sprite, in a UI panel. It is how a security monitor, a rear-view mirror, a minimap, a portal's far side, a character portrait in a menu and a 3D model turning inside a UI panel are made. The module is `cartwheel/subviewport.h` ([reference](header:subviewport.h)).

A SubViewport renders one of two things.

**A camera of this world.** Set `camera` to a Camera3D of the same scene (one with `current = false`, so it is not the screen's camera). The whole pipeline draws the view: lights and shadows, sky, fog, particles, decals, the post chain and tone mapping, without the 2D canvas and the HUD.

```text
[node name="SecurityCam" parent="Level" type="Camera3D"]
position = vec3(0, 7, 0.3)
rotation = vec3(-1.5708, 0, 0)
current = false

[node name="Monitor" parent="." type="SubViewport"]
size = vec2(512, 288)
camera = "../Level/SecurityCam"

[node name="Screen" parent="Level" type="MeshInstance3D"]
size = vec3(2.4, 1.35, 0.06)
color = color(0, 0, 0, 1)
viewport_texture = "viewport://Monitor"
```

**A world of its own.** Set `scene` to a .cwscene asset. It is instanced into a tree the SubViewport owns, with its own cameras, lights, [WorldEnvironment](node:WorldEnvironment), [PostProcess](node:PostProcess) and 2D canvas. Nothing in it shows in the main picture and no light crosses, so a portrait is lit the way the portrait scene says wherever the player stands. A scene of 2D nodes makes it an off-screen 2D canvas. The world ticks with the main tree; game code reaches into it through the viewport, because node paths do not cross between the two trees:

@include samples/subviewport/main.c#own_world

Showing the texture:

| Where | How |
|---|---|
| a 2D node or a UI control, in a scene file | any `texture` property takes `"viewport://Name"` ([Sprite2D](node:Sprite2D), [TextureRect](node:TextureRect), [NinePatchRect](node:NinePatchRect), Polygon2D ...). Name is the SubViewport's `texture_name`, or its node name |
| a mesh, in a scene file | [MeshInstance3D](node:MeshInstance3D) `viewport_texture`. `viewport_slot = EMISSIVE` (the default) makes a screen that glows by itself: give the mesh a black `color`, and `emissive` tints or dims it. `COLOR` makes it the surface colour, lit and shadowed like paint |
| 2D and UI, from C | [cw_subviewport_texture](api:cw_subviewport_texture) is a `cw_texture` |
| any three.c material, from C | [cw_subviewport_t3_texture](api:cw_subviewport_t3_texture), with `t3_material_set_texture` |

`update_mode` says when it renders: `ALWAYS`, `WHEN_VISIBLE` (only in frames after something showed the texture: a 2D node or control drew it, or a mesh wearing it was inside a camera's view), `ONCE` (one render, then the picture holds; [cw_subviewport_request_update](api:cw_subviewport_request_update) asks for another) and `DISABLED`. `update_every = 2` renders every other frame. A monitor nobody is looking at should be `WHEN_VISIBLE`; a portrait that does not move should be `ONCE`.

Timing: a world of its own is rendered just before the frame that shows it. A camera of this world is rendered right after the frame's own picture and reuses that frame's shadow maps, light clusters and probe captures, the way a splitscreen view does, so its texture is one frame behind. That is invisible on a monitor or a minimap; a portal seen while the player's camera turns fast trails by that frame. The main picture is exactly what it would be without the SubViewport, apart from the surfaces showing the texture.

Things to know:

- The texture holds display colours (tone mapped, sRGB). On a mesh it is decoded and goes through the main picture's tone curve again, as a photograph of a screen does. So a camera view shown on a mesh is close to the world it shows but not identical in contrast: fine for a monitor, visible on a portal that should match the world beyond it seamlessly.
- A camera view runs every pass again at the viewport's size. Passes that keep one set of targets per tree for the view size (water reflections and refractions) re-allocate each frame when a camera view and the screen both draw them at different sizes: keep water out of a camera view's sight, or put the water scene in a world of its own.
- Volumetric fog keeps a grid for up to three camera views besides the screen's (fewer in splitscreen); a view past that is drawn without it. TAA never runs in a SubViewport.
- A mesh that wears a viewport's texture and is seen by that viewport's own camera shows the previous frame's picture: a monitor inside a monitor, which is what a real one does.
- A world of its own draws with the app's program table like everything else: run `tools/programs.mjs` after adding one, as after any new material.

`samples/subviewport` has all of it in one scene: a monitor showing a camera that looks straight down, a still photo taken once, a 2D canvas as the colour of a sign the sun lights, a portrait from a world of its own in a UI panel and as a sprite, and a `WHEN_VISIBLE` panel that starts behind the camera. The gate `subviewport` holds the pictures across hosts and GL against WebGPU and checks each mode's render count; its control stops the monitor and fails. `unit-subviewport` covers names, modes, sizes, worlds, camera views (the frame's own camera, output and history are untouched) and the scene-file round trip.

Cost. A camera view costs about what the screen's own 3D picture costs at the same pixel count, without the shadow maps (they are the frame's), and scales with its size; a world of its own costs its own content. Measured on the 890M (Ryzen AI 9 HX 370) through romdev, `samples/subviewport` at 1920 x 1080 (a light scene: three shapes, one shadowed sun, HDR and tone mapping), GPU milliseconds a frame added by one `ALWAYS` camera view, GL / WebGPU. The machine was shared while measuring (load average 40 to 50, other GPU work running), so read the figures to about 0.1 ms:

| Effects preset | whole frame without the view | + a 512 x 288 view | + a 1920 x 1080 view |
|---|---|---|---|
| Very Low (renders every third frame) | 0.91 / 0.91 | under 0.1 / under 0.1 | +0.43 / +0.35 |
| Low (every second frame) | 0.98 / 1.03 | under 0.1 / under 0.1 | +0.71 / +0.61 |
| Medium | 1.16 / 1.08 | +0.07 / under 0.1 | +1.34 / +1.22 |
| High | 1.13 / 1.05 | +0.11 / +0.12 | +1.41 / +1.33 |
| Ultra | 1.17 / 1.10 | +0.10 / +0.08 | +1.40 / +1.28 |

The CPU side of a camera view (the pipeline recorded once more) was 0.3 to 0.6 ms a frame in the cart on this scene; a 3D world of its own 0.4 to 0.6 ms; a 2D canvas world 0.3 to 0.5 ms. A view the size of the screen costs a little more than the screen's own picture here because the screen's figure includes no second post chain; in a heavy scene expect a camera view to cost the scene pass and post chain of a splitscreen view of that size. On a Vega 8 (about a third of the 890M) a 512 x 288 monitor in a scene this light is about 0.3 ms; size the texture to what the screen shows of it, and use `WHEN_VISIBLE`.

The Low and Very Low effects presets render `ALWAYS` and `WHEN_VISIBLE` viewports every second and every third frame they are due, so a monitor costs half and a third there. The debug command `subviewport` lists every viewport with its size, mode, render count, last CPU milliseconds and whether its texture was shown last frame.

## Particles and decals

**Burst slots.** A game that fires the same one-shot effect from many places (sparks on every hit, a flash at every barrel) used to pool a GPUParticles3D per concurrent burst, and every pooled system with a particle alive cost a simulation pass and a draw. Set `bursts` (1 to 32) on one system instead: its `amount` splits into that many slots of amount / bursts particles, and `cw_particles_burst(node, at, dir)` fires the oldest slot once at world position `at`, its particles thrown along `dir` (zero: the system's `direction`), each firing with a new seed. Every slot plays as a one-shot of the system's settings would (lifetime, explosiveness, shape, speeds, forces, colour and size over life, blending), and the effects preset thins each slot's particles, so a slot is never left out. `samples/fx_bursts` fires eight slots of sparks round a ring from one system.

[particles.h](header:particles.h) simulates GPUParticles3D on the GPU with transform feedback, one step per fixed tick. Emission is a pure function of the particle index and sim time, with random values hashed from (seed, index, cycle), so the same seed and tick count give the same particles on every host. Shapes, velocity cones, gravity, damping, curl-noise turbulence, attractors, analytic colliders and collision with the depth buffer (only what the camera sees), billboards, stretched sprites or instanced meshes, flipbooks, colour and size curves, soft particles, a GPU bitonic sort for alpha blending, and per-particle trails. Particles draw unlit in the "particles" EFFECTS pass; `energy` above 1 blooms (the scene is HDR). Costs: one transform feedback draw per system per tick, one instanced draw per system per frame, plus the sort passes when `sort` is on. GPUParticles2D runs the same emission model on the CPU for 2D (see [2D rendering](guide:rendering-2d)).

[decals.h](header:decals.h) projects Decal3D images along the node's -Z onto whatever the scene drew into depth: steps, slopes, spheres, props. MIX paints over the lit scene (signs, paint), MULTIPLY keeps lighting correct (dirt, scorch, cracks), ADD glows. All decal images share one 2048 x 2048 atlas, and `region` picks a cell of a sheet. An image goes into the atlas (decoded, with its mips) when a decal in the tree gets it or enters the tree with it, so decals set up before play load under the loading screen, and a new image touches only its own cell of the atlas (its mips are built from that cell alone), never the rest: a decal stamped mid-play with an image the atlas already holds costs nothing to load, and one with a new image costs that image's decode in the frame it is stamped. [cw_decal_preload](api:cw_decal_preload) puts an image into the atlas ahead of time for the images a game stamps in play (the explosion kit does for its scorch sheet); `cw_decal_stats.atlas_load_ms` says what the last image cost. Gate `decal-first-image` holds the stamp's frame to the cost of any other frame on GL and WebGPU through romdev. Decals draw as instanced boxes; the GPU cost is the screen area those boxes cover. They land on anything in depth, including characters that walk through the box, so keep boxes shallow; they never land on transparent surfaces, particles or the sky.

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
