# 2D rendering

{{name}} draws 2D through one batched renderer (`cartwheel/render2d.h`) that you can call directly every frame, and through a set of 2D node types (`cartwheel/scene2d.h`) that submit to the same renderer from the scene tree. This page covers both layers, how they sort and batch, and what a frame costs.

## Two ways in

- **Immediate submission.** Between `cw_r2d_begin` and `cw_r2d_end` the game submits sprites, shapes, text, tilemaps and lights. Nothing draws while you submit: `cw_r2d_end` sorts, batches and issues the draws. The game owns its textures, fonts, tilemaps and cameras (plain handles and values). Use this for custom renderers, tools and anything you want full control over.
- **Nodes.** `Sprite2D`, `Label2D`, `TileMap2D`, `Camera2D` and the rest are node types with reflected properties, so they load from `.cwscene` files, show in the inspector and answer the debug channel. The scene runner walks the tree once per frame and submits every visible canvas item. Textures and fonts are asset paths, loaded and cached by path on first draw, so a scene can be instanced before a GL context exists.

Both paths end in the same renderer, so a node game can still draw extra things with the immediate API (for example from `base.render` of `cw_scene_app`, which runs after the tree's 2D pass).

## How a pass sorts and batches

The renderer sorts by canvas layer, then z, then y (only in layers that y-sort), then submission order. The sort is stable, so equal keys keep the order you submitted them in. A batch only ends where GL state must change: a different texture array, blend mode, clip rect, or a shader change (tilemaps and lit layers).

Every texture lives in a `GL_TEXTURE_2D_ARRAY`. Loose images are packed at load time into shared 2048x2048 atlas pages (one array per filter mode), with a 1-pixel extruded border so linear filtering never bleeds a neighbour in. Sprites from different atlas pages share one draw, and solid shapes, SDF text and additive sprites batch with textured sprites because the fragment shader branches on a per-vertex mode. A typical frame is a handful of draw calls.

Costs to know:

- Submission is O(vertices) on the calling thread. `cw_r2d_sprites` (an array of sprites) fills vertices on the job pool and gives the same result as one `cw_r2d_sprite` call per element.
- `cw_r2d_end` runs one radix sort when the submission is not already in order, one upload per stream (a ring of three buffers), and one draw per batch.
- Atlas space is never reclaimed: pages only grow. Standalone textures (repeat wrap, images larger than a page, render targets) get their own array and are freed on destroy.
- Colours are straight alpha in the API and premultiplied internally; blending is premultiplied throughout.

`samples/r2d_bench` draws 100,000 animated sprites at 1280x720 and interleaves three submission variants in one run: bulk `cw_r2d_sprites`, one call per sprite, and bulk into a y-sorted layer. All 100,000 land in one draw call. Bulk submission is several times cheaper on the CPU than per-sprite calls, and the y-sorted variant pays for the sort every frame in `cw_r2d_end`. The timings are `bench.*` debug fields, reported but never compared across hosts.

`samples/r2d_sprites` shows the whole sprite and shape surface in one frame: rotation, flips, pivots, tint, additive and multiply blending, nine-slice panels (`cw_r2d_nine_slice`), lines and polygons (concave polygons are ear-clipped), textured quads, clip rects and a render target drawn as a rotating sprite.

## Canvas layers and cameras

A pass has up to `CW_R2D_MAX_LAYERS` (32) canvas layers, drawn in index order. Each layer has its own camera, viewport, parallax factor, y-sort and pixel-snap settings, and they persist across passes and frames until changed (a scene tree's canvas draw, `cw_canvas_draw` or `cw_canvas_draw_view`, puts back the settings it used when it ends). Layer 0 with no camera is screen space. A camera (`cw_camera2d`) is a plain value the game owns; `cw_camera2d_follow` smooths toward a target with frame-rate independent exponential smoothing, computed with the deterministic `cw_exp`, so a replay follows the same path on every host.

`samples/r2d_camera` sets up four layers: a parallax background, the world with a following, zooming, rotating camera, a minimap with its own viewport and camera, and a screen-space HUD.

@include samples/r2d_camera/main.c#layers

The same sample publishes its camera math (follow, screen to world and back, window to canvas) as debug fields, and the `r2d-camera` gate requires them to match exactly on every host.

## Virtual resolution and scale modes

A window pass can draw at a fixed virtual size and scale the result to the window. The scale mode decides how:

| Mode | Result |
|---|---|
| `NONE` | no virtual canvas: canvas pixels are window pixels |
| `INTEGER` | the largest whole factor that fits, centred, letterboxed (pixel-perfect) |
| `FIT` | scaled to fit keeping the aspect ratio, centred, letterboxed |
| `STRETCH` | stretched over the whole window |

The virtual canvas is nearest-filtered when scaled. `cw_r2d_window_to_canvas` undoes the scaling and letterboxing for pointer input, and `cw_r2d_canvas_rect` says where the canvas lands in the window. `samples/r2d_camera` draws a 300x170 canvas in a 640x360 window with `INTEGER`; it reads its own window back and the gate expects zero non-uniform 2x2 blocks.

In a scene, the first `CanvasSettings` node in tree order sets the same things for the tree's 2D pass: `virtual_width`, `virtual_height`, `scale_mode`, `clear_color`, `letterbox`, the `ambient` light of lit canvases, `pixel_snap` and `y_sort` of the default canvas. This is the first room of `games/action2d`:

@include games/action2d/assets/rooms/room1.cwscene#Settings

Without a `CanvasSettings` node the canvas is the window's pixels, cleared to black, with white ambient.

The virtual canvas is for Node2Ds only. Controls (`Label`, `Button` and the rest of the UI) lay out in UI units, window pixels divided by `cw_ui_scale`, even under a `CanvasLayer` and even when a virtual canvas is set: with a 320x180 canvas in a 640x360 window, a `Label` at `offset_left = 8` sits 8 window pixels (4 virtual pixels) from the edge. That keeps UI text sharp and the same size at any game resolution. For UI in virtual pixels, set `cw_ui_set_scale` to the window-to-canvas factor, or draw the UI inside your own virtual pass with `cw_ui_draw` (the `ui_hud` sample does).

A `CanvasLayer` defaults to `layer = 1` (above the default canvas) and `follow_viewport = false` (screen space, for a HUD); set `follow_viewport` for a layer that scrolls with the `Camera2D`.

## The 2D node types

Every 2D type except `CanvasLayer` and `CanvasSettings` extends `Node2D` (position, rotation, scale, skew, `z_index`, `z_relative`, `visible`, `interpolate`, `modulate`, `self_modulate`). Units are pixels, y points down, angles are radians clockwise. The node reference lists every property; the short version:

- `Sprite2D`: a texture or atlas region, or one cell of an hframes x vframes grid.
- `AnimatedSprite2D`: named animations written as one string (`"idle@3:0-1; run@12:2-5; jump:6!"`: name, optional fps, frame indices and ranges, `!` to play once). By default it advances on fixed ticks, so it is deterministic in replays. Signals `frame_changed` and `animation_finished`.
- `NinePatch2D`, `Line2D`, `Polygon2D`: panels, strokes and filled shapes. Point lists are strings of number pairs (`"0 0, 32 0, 32 16"`).
- `Label2D`: text with a font asset, SDF or bitmap mode, wrap, alignment and spacing.
- `TileMap2D`: up to 8 layers of cells in a compact text form, plus animated tile ids.
- `Camera2D`: the first current one in tree order drives every canvas that follows the viewport. Smoothing runs on fixed ticks after physics; rendering interpolates.
- `CanvasLayer`: a separate canvas in `layer` order (the default canvas is 0). It is screen space unless `follow_viewport` is set, which makes it the natural home for a HUD.
- `ParallaxLayer`: its descendants scroll at `motion_scale` of the camera. Put it under a `CanvasLayer` with a negative layer for a background.
- `Light2D` and `LightOccluder2D`: see lights below.
- `GPUParticles2D` (from `cartwheel/particles.h`): CPU-simulated, deterministic particles drawn as one sprite batch; `samples/fx_particles2d` shows fire, snow, bursts and a world-space trail.

The hero scene of `games/action2d` holds an `AnimatedSprite2D`, a hitbox and a smoothed, limited `Camera2D`:

@include games/action2d/assets/actors/hero.cwscene#Sprite..Camera

A screen-space layer is a `CanvasLayer` node; action2d's fades and flashes sit on one:

@include games/action2d/assets/main.cwscene#Overlay

### How the tree draws

`cw_canvas_draw` walks the tree once in tree order. Hidden nodes and hidden canvas layers hide their subtrees, transforms compose (interpolated between the last two fixed ticks for nodes with `interpolate`), `modulate` multiplies down and `z_index` adds down while `z_relative` is set. Each canvas becomes one render layer, so the final order is (canvas layer, z, tree order), with y inside y-sorted canvases. A frame may hold up to 30 canvases (`CanvasLayer` plus `ParallaxLayer` nodes); past that the engine warns and the extra canvases share the last layer.

After the tree's 2D pass, the overlay passes registered with `cw_canvas_add_pass` run in order, each as its own render2d pass over the frame. That is how [UI](guide:ui) composites over 2D and 3D without sharing layer slots. In the [render pipeline](guide:rendering-3d) all of this is the "canvas" pass of the overlay phase, before the app's own render callback. A tree with no canvas items and no overlay passes skips the 2D pass entirely, and a 2D-only tree never creates a three.c renderer.

A custom node type can draw in this pass by registering a draw function with `cw_canvas_register_type`. The function receives the composed transform (already set, so it submits in node-local coordinates), the inherited modulate, and the layer and z already selected. Editor viewports use `cw_canvas_draw_view` to draw the same tree into a render target with their own camera.

## Text

Fonts load from TTF or OTF through stb_truetype in two modes. SDF fonts bake glyphs once into a single-channel distance field and serve every size from that bake (corners round slightly at very large sizes). Bitmap fonts bake coverage at one pixel size, optionally thresholded to 1-bit, and are meant to be drawn at integer multiples of it. Glyphs bake on first use.

Layout is left to right with kerning (the kern table and GPOS pairs), greedy word wrap, wrapping between CJK ideographs with the common kinsoku rules, and left, centre or right alignment. `cw_font_measure` runs the same layout code as drawing, so measured boxes match drawn text to the pixel. Text containing right-to-left or complex-shaping scripts takes a shaping path (OpenType GSUB/GPOS and bidi runs); pure left-to-right text never pays for it. A codepoint the font lacks comes from its fallback chain, then the global chain the localization module sets per locale; see [localization](guide:l10n-a11y).

`samples/r2d_text` draws SDF text at several sizes from one bake, wrapped paragraphs in three alignments, a bitmap pixel font, and rotated text. Its measurements are debug fields, so the `r2d-text` gate checks that every host lays text out identically, not only that it looks alike.

## Tilemaps

A tilemap stores up to 16 layers in 32x32-tile chunks. Each chunk is static geometry in its own buffer, rebuilt only when one of its cells changes, and drawing culls chunks to the layer's view. Animated tiles cycle in the vertex shader from the pass time, so animation never rebuilds a chunk. Cells use Tiled's flag convention (flip x, flip y, transpose in the high bits), so Tiled maps load unchanged. Pass a tick-derived time in the pass desc when a replay must show the same tile frame.

`samples/r2d_tilemap` pans a zoomed camera over a 96x48 three-layer map, animates water and lava, shows every flip and rotation of an asymmetric tile, and edits one cell mid-run (one chunk rebuild, visible in the `r2d.chunk_rebuilds` stat). The `TileMap2D` node wraps the same renderer with a text cell format (`"0*20 / . . 3 3x / 1*4"`) and `cw_tilemap2d_set_cell` for code.

## 2D lights

A layer marked lit draws into an offscreen colour buffer (and a normal buffer when a normal-mapped texture was drawn), then composites as colour times light. The light buffer starts at the pass's ambient colour, and each light adds a radial falloff quad. Shadows are analytic: each occluder edge facing away from a light is extruded into the stencil buffer, which gives hard shadows exact at any resolution for two draws per shadowed light. The light buffer stores light at half intensity in RGBA8, so lights can overbright up to 2x. Normal maps attach to atlas textures with `cw_texture_set_normal_map`; without one a sprite is lit flat.

`samples/r2d_lights` lights normal-mapped bricks and flat critters with three moving coloured lights (two shadowed) over a dim ambient, with an unlit HUD layer on top. In a scene, a canvas holding a `Light2D` is drawn lit, and `LightOccluder2D` polygons cast its shadows. Pillars and a torch in action2d's first room:

@include games/action2d/assets/rooms/room1.cwscene#Pillar1..Torch1

## Sharing GL with 3D

render2d needs a current GL context (call it from `init` onward) and sets every piece of GL state it uses at each `cw_r2d_end`. When you mix it with three.c yourself, call `t3_renderer_reset_bindings` after a 2D pass and before the next 3D render so three.c's state cache matches GL again; the scene runner already does this. To put a 3D view inside a 2D scene, create a render target with `depth`, point three.c at `cw_target_fbo`, and draw the target as a sprite. `samples/r2d_interop` renders a torus knot into a target and draws it as a rotating, translucent sprite over a 3D frame.

## Seeing what it did

`cw_r2d_get_stats` reports the frame's passes, draw calls, batches, vertices, sorts, chunk draws and rebuilds, lights, atlas pages and missing glyphs. `cw_r2d_debug_register("r2d")` publishes them as debug fields, which is how the gates compare batching across hosts. `cw_canvas_get_stats` counts the tree's canvas items, layers, lights and occluders. The [debug channel](guide:debug-channel) can screenshot the world layer alone, and the [profiling](guide:profiling) guide covers GPU timings.

## Samples

| Sample | Shows |
|---|---|
| `r2d_sprites` | sprites, shapes, blend modes, nine-slice, clip, render targets |
| `r2d_text` | SDF and bitmap fonts, wrap, alignment, measurement |
| `r2d_tilemap` | chunked tilemaps, animated tiles, flips, cell edits |
| `r2d_camera` | canvas layers, cameras, parallax, minimap, `INTEGER` scaling |
| `r2d_lights` | lit layers, normal maps, shadows, ambient |
| `r2d_interop` | three.c into a render target, 2D over 3D |
| `r2d_bench` | 100,000 sprites, bulk vs single submission |
| `fx_particles2d` | `GPUParticles2D` |

The reference pages: [render2d.h](header:render2d.h), [scene2d.h](header:scene2d.h), and the node pages from `Node2D` down.
