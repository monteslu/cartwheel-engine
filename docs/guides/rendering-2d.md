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

**Covered pixels are not shaded.** Layers draw back to front with blending, so under a full screen of art every pixel would be shaded once per layer: the sky, each parallax layer, the back wall, the tiles in front of it. In the offscreen canvases (the HDR canvas of a pass with `.post`, the buffers of a lit run) the renderer skips that work. Each submission gets a depth from its place in the sorted order, a group of layers first draws the fully opaque texels (alpha exactly 1) of its opaque art (alpha-blended sprites of a texture whose texels are mostly opaque, solid shapes, tilemaps of an opaque tileset; translucent art hides nothing and is left out of this step) nearest first into a depth buffer with colour off, and then draws as always, back to front, testing that depth, so a pixel behind an opaque one is never shaded. An opaque texel replaces what is under it, so the picture is the same to the bit; only work that could not show is skipped. A group ends wherever something reads what was drawn: a `screen_read` or distortion layer, the end of a lit run, the chain's HUD layer. Particles and sprite instances take no part (never culled, never culling). A window canvas without a chain draws as before. `CW_R2D_CULL=0` (natively) turns it off, to compare.

## Sprite instances

`cw_r2d_instances` draws many sprites of one texture in one instanced draw: grass, a star field, a swarm, bullets, debris, a crowd. Each instance is 24 bytes (`cw_sprite_instance`: where, turn, scale, colour, a frame index into up to 32 source rects, flips) instead of a sprite's 4 vertices and 6 indices built on the CPU (136 bytes); the vertex stage builds the quads. The instances of one call draw in array order and sort as one submission at the current layer and z (no y-sorting inside the call). They blend, take intensity, emission, flash and wind sway, and are lit in lit layers like sprites. Feed them per frame (`.instances`, `.count`: copied at the call, one upload for the pass) or keep them on the GPU in a `cw_r2d_instance_buffer` written once (or a range now and then) and drawn every frame for nothing on the CPU. On the 890M, 200,000 moving sprites cost 10.3 ms (GL) / 15.5 ms (WebGPU) of CPU as sprites, 0.9 / 2.8 ms as per-frame instances, 0.04 / 0.5 ms from a kept buffer; the GPU time (about 5 ms, the pixels) is the same. `r2d_stress` cases `inst200k` and `instkept200k`.

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
| `EXPAND` | `FIT`'s scale over the whole window: the canvas grows to the window's aspect instead of bars (a 320x180 canvas is 240x180 on a 4:3 screen, 432x180 on 21:9; `cw_r2d_canvas_size` says). Camera2D limits still hold, so a level's edge is never shown past |

The virtual canvas is nearest-filtered when scaled, unless `scale_filter` says otherwise: `CW_SCALE_FILTER_SHARP` draws each canvas pixel as a crisp block of its true width and blends only the window pixel on a block's edge with its neighbour, so pixel art at a scale that is not a whole number (`FIT`, `EXPAND`) neither shimmers as the camera moves (some pixels a window pixel wider than others) nor blurs; `CW_SCALE_FILTER_LINEAR` smooths it (a painted canvas). `r2d_camera` with `CW_R2D_CAMERA=sharp` shows it. `cw_r2d_window_to_canvas` undoes the scaling and letterboxing for pointer input, and `cw_r2d_canvas_rect` says where the canvas lands in the window. `samples/r2d_camera` draws a 300x170 canvas in a 640x360 window with `INTEGER`; it reads its own window back and the gate expects zero non-uniform 2x2 blocks.

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

Baking on first use costs a frame the first time a glyph appears (a score's new digit, a dialogue line). To move that work to loading, preload during init or warmup on the main rendering thread: [cw_font_preload_range](api:cw_font_preload_range) bakes an inclusive codepoint range (`cw_font_preload_range(font, 32, 126)` for English UI; surrogates are skipped and the end is clamped to U+10FFFF), and [cw_font_preload_text](api:cw_font_preload_text) bakes what a UTF-8 string needs, including fallback glyphs and the contextual forms the shaping path picks, which a range cannot anticipate. Use the text call for localized strings. Both fill the same caches drawing does and draw nothing; repeating them costs nothing, and a NULL or stale font or NULL text is a no-op. Preload only what the game or current locale uses, since every glyph takes atlas memory, and call again after changing fallback fonts or the locale. An SDF preload serves every size; a bitmap font's at its bake size. The `text.glyph_bake` profiler zone shows bakes that still land mid-game.

Layout is left to right with kerning (the kern table and GPOS pairs), greedy word wrap, wrapping between CJK ideographs with the common kinsoku rules, and left, centre or right alignment. `cw_font_measure` runs the same layout code as drawing, so measured boxes match drawn text to the pixel. Text containing right-to-left or complex-shaping scripts takes a shaping path (OpenType GSUB/GPOS and bidi runs); pure left-to-right text never pays for it. A codepoint the font lacks comes from its fallback chain, then the global chain the localization module sets per locale; see [localization](guide:l10n-a11y).

**Text size.** The player's text scale multiplies every size passed to the text calls, measuring included, and the engine checks each frame that nothing is drawn under the size floor; a style's `fixed_size` marks art text that is drawn as asked and not checked, and `own_backdrop` marks text on the game's own panel (the player's Text background setting adds nothing to it). The rules, the numbers and the check are in [localization and accessibility](guide:l10n-a11y), "Text size". `samples/text_size` is a HUD laid out from measured text at every scale, with the floor's control.

`samples/r2d_text` draws SDF text at several sizes from one bake, wrapped paragraphs in three alignments, a bitmap pixel font, and rotated text. Its measurements are debug fields, so the `r2d-text` gate checks that every host lays text out identically, not only that it looks alike.

## Tilemaps

A tilemap stores up to 16 layers in 32x32-tile chunks. Each chunk is static geometry in its own buffer, rebuilt only when one of its cells changes, and drawing culls chunks to the layer's view. Animated tiles cycle in the vertex shader from the pass time, so animation never rebuilds a chunk. Cells use Tiled's flag convention (flip x, flip y, transpose in the high bits), so Tiled maps load unchanged. Pass a tick-derived time in the pass desc when a replay must show the same tile frame.

`samples/r2d_tilemap` pans a zoomed camera over a 96x48 three-layer map, animates water and lava, shows every flip and rotation of an asymmetric tile, and edits one cell mid-run (one chunk rebuild, visible in the `r2d.chunk_rebuilds` stat). The `TileMap2D` node wraps the same renderer with a text cell format (`"0*20 / . . 3 3x / 1*4"`) and `cw_tilemap2d_set_cell` for code.

## 2D lights

A layer marked lit draws into an offscreen colour buffer (plus a normal and a material buffer when a normal map, a material map or emission was drawn into it), then composites as colour times light. The lights are tiled: the CPU sorts the run's lights into 32-pixel screen tiles and one full-screen pass gives each pixel the ambient colour plus every light of its tile, reading the pixel's normal and material once and writing it once, however many lights overlap there. 500 lights are one draw and about 60 WebGPU calls, where they were 500 draws and over 3000 calls.

A `cw_light2d` is a point light by default: a radial falloff (`falloff` is the exponent of 1 minus distance over radius), normal-mapped shading from its `height` above the plane, an optional `cone` (with `angle` and `cone_soft`) for a spot, and `directional` for a sun or moon whose parallel light covers the whole layer from `angle`.

**Shadows** come from a distance field of the occluders: `cw_r2d_occluder` outlines, and art drawn after `cw_r2d_set_occluder(true)` (sprites, shapes, text and tilemaps cast shadows wherever their alpha is over half, so ground tiles need no polygons). The occluder outlines of the run are filled into a mask at the canvas size, and a jump flood (about 12 full-screen passes at the field's size) turns the mask into the nearest occluder pixel at every pixel, once per run whatever the number of lights. Each lit pixel then marches toward its light through the field, taking big steps in open space (sphere tracing). `softness` widens the penumbra the further the pixel is from the occluder, as a light of some size makes it; 0 is a hard edge. The cost no longer grows with occluder edges: 500 shadowed lights over 120 occluders went from 8 ms of CPU and 6000 WebGPU calls a frame to about 1 ms and 280 calls. An occluder's own pixels stay lit, as its face; set the layer's `occluder_depth` to let light reach only that deep into occluders, so ground and walls glow at the edge a light falls on and stay dark inside.

The field's size and the march steps follow the effects detail of the [graphics settings](guide:rendering-3d): Ultra the canvas size and 48 steps, High and Medium half the canvas and 32 or 24 steps, Low and Very Low a quarter (never under 320 pixels across) and 16 or 12 steps. On Low the lights are shaded into a light buffer half the canvas size each way, on Very Low a third (never under 320 texels across): a shadowed light then costs a quarter or a ninth of its pixels. Each canvas pixel takes the four nearest light texels, weighed by how alike its surface (its normal, inside or outside an occluder) is to the one each texel was shaded at, so shadow edges and bevels stay where they are instead of bleeding. High and Ultra shade every pixel, as before.

**Light textures.** A point or spot light with a `texture` (and optionally `texture_src`) multiplies its colour by that image laid over its radius square and turned by its `angle`: light through window panes, a lantern's cage, stained glass, a torch's pattern. The textures are ordinary images (atlas regions); the lights of one lit run must take theirs from one texture array (images of one filter mode pack into one atlas), and a light whose texture lives elsewhere draws without it (warned once). It costs one filtered read per textured light per pixel it reaches. A `Light2D` node takes an image path in `texture`. `r2d_hdr` with `CW_R2D_HDR=cookie` shines its torches and spot lamp through a window.

`volumetric` adds the light the air scatters toward the eye: shafts where a light passes between occluders, a glow round a lamp, added on top of the lit layers and over empty background too.

**Materials.** Normal maps attach to atlas textures with `cw_texture_set_normal_map`; without one a sprite is lit flat. A normal map turns with what draws it: a sprite's `rotation`, `flip_x` and `flip_y`, an instance's rotation and flip flags, a particle's spin and a turned camera all turn its normals with the picture, so a torch on the left lights the left edge of a sprite however it is turned (`samples/r2d_normal_turn` and its test hold turned sprites and instances to sprites drawn unturned with the normals turned beforehand, on GL and WebGPU). An unturned sprite's normals are written exactly as before. A material map (`cw_texture_set_material_map`) adds per texel emission (red: the share of the colour that ignores the lights), specular (green: how strongly lights glint off it) and gloss (blue: how tight the glint is). `cw_r2d_set_emissive` gives later draws emission without a map: a lamp, a screen, lava. Keep cloth, stone and skin near zero specular; wet rock, metal and glass carry it.

**Global illumination.** A pass with `.gi` lights its lit layers with the light that bounces round the scene: what glows lights what is near it in its own colour, light that falls on an occluder bounces off in the occluder's colour, and occluders cast soft shadows from all of it, with no light placed. It uses radiance cascades over a distance field of the occluders and the glowing pixels, at a share of the canvas size (off on Very Low, an eighth on Low, a quarter on Medium and High, half on Ultra). Each frame's result feeds the next frame's bounce.

`samples/r2d_lights` lights normal-mapped bricks and flat critters with three moving coloured lights (two shadowed) over a dim ambient, with an unlit HUD layer on top. `samples/r2d_hdr` puts the rest together: a stone wall with normal and material maps, glowing crystals, torches with soft shadows, a spot lamp, moonlight through a gap in the ceiling, global illumination, a reflecting pool, heat haze and a shockwave, under the post chain. In a scene, a canvas holding a `Light2D` is drawn lit, and `LightOccluder2D` polygons cast its shadows. Pillars and a torch in action2d's first room:

@include games/action2d/assets/rooms/room1.cwscene#Pillar1..Torch1

## HDR and the post chain

A pass with `.post` (a `cw_r2d_post`) draws its layers under `post->hud_layer` into a high dynamic range canvas (RGBA16F where the host renders it), so colour can go past 1: `cw_r2d_set_intensity` multiplies later draws (1 to 16), lights go as bright as asked, additive glows stack. The chain then turns that canvas into the pass's output: exposure, bloom (what passes a soft-knee threshold, blurred over up to 8 halvings with a 13-tap filtered downsample and a tent upsample), a tone curve, a grade (contrast, saturation, lift, gamma, gain, tint), a vignette and a dither below one 8-bit level so smooth gradients do not band. The default tone curve, `CW_R2D_TONE_SOFT`, leaves everything under `tone_knee` exactly as drawn and rolls off above it, so 0..1 art is untouched; `ACES`, `REINHARD` and `CLAMP` are there too. Layers from `hud_layer` up draw after the chain, straight into the output: a HUD over a glowing world. Each pass has its own chain, so two regions can be graded differently. A NULL `post` draws exactly as before.

Bloom follows the effects detail: none on Very Low, at most 3 levels on Low and 5 on Medium. On Low and Very Low the HDR canvas is R11G11B10 float (half the bytes of RGBA16F, no alpha) where the host renders it, the pass clears to an opaque colour and no layer reads the screen; global illumination's field never exceeds the pass's own `resolution` below High (half of it on Low), and GPU particles draw the effects detail's share of each emitter (a quarter on Very Low, 0.4 on Low, 0.7 on Medium: the same first particles of the emitter, so the scene thins rather than changes).

**Lookup tables.** `cw_r2d_post.lut` maps every finished display colour (after the tone curve, the grade and the vignette, before the dither) through a 3D colour table, `lut_amount` of the way: the whole grade of a film still in one image. Tables are strip images, `size` square slices side by side (red across a slice, green down, blue picks the slice): `cw_r2d_lut_load` reads one from a PNG and `cw_r2d_lut_from_strip` from pixels. To author one, draw the identity strip (`cw_r2d_lut_pixels(CW_R2D_LUT_IDENTITY, 32, px)`) into a screenshot, grade the screenshot in any image editor, cut the strip back out and load it. `cw_r2d_lut_preset` gives built-in looks: `WARM`, `COOL`, `TEAL_ORANGE`, `NOIR`, `SEPIA`, `BLEACH` (bleach bypass: washed out, hard contrast) and `MOONLIGHT`. It costs two filtered reads a pixel in the chain's final pass. `r2d_hdr` with `CW_R2D_HDR=lut` shows the teal and orange table. On the 890M at 1080p the chain with 6 bloom levels costs about 0.6 ms of GPU time, 0.16 ms without bloom.

## Reading and bending the screen

A layer with `screen_read` copies the canvas as drawn before it into `cw_r2d_screen_texture()`, which its draws can sample (UVs 0..1 over the canvas): a pool that mirrors the world above it, frosted glass, a lens. A layer with `distortion` set bends the picture instead of drawing colour: each pixel's red pushes the canvas right (0.5 is no push), green pushes it down, both times its alpha, up to `distortion` canvas pixels, and overlapping draws add up. Scroll `cw_r2d_noise_texture()` over a fire for heat haze or over water for ripples; `cw_r2d_shockwave` draws a ring that pushes outward. Either flag makes the pass draw offscreen (the HDR canvas with `.post`) and copy to its target at the end. A distortion layer bends only what was drawn before it: when a layer above it (still under the HUD) draws, the canvas so far is bent first (one full-screen pass) and the layers above draw over it straight, a lattice in front of heat haze stays sharp. Distortion layers drawn last, just under the HUD, cost nothing extra: the chain's final pass bends as it reads. `r2d_hdr` with `CW_R2D_HDR=front` draws a lattice above the distortion layer.

## Motion: wind and trails

Sprites submitted after `cw_r2d_set_sway(share)` bend their top edge in the pass's `wind` (strength in pixels, wave speed and length, gusts): grass, reeds, banners, treetops sway in the vertex stage at no CPU cost. `cw_r2d_trail` draws a ribbon along a path, its width and colour going from the head's to the tail's, with an optional texture along it: comet tails, sword swipes, tyre marks, the smear behind a dash. `samples/r2d_motion` shows a meadow in the wind, glowing comets and a dashed trail.

## GPU particles

`cw_r2d_particles` draws an emitter's particles in one instanced draw with no state anywhere: each particle is a pure function of the time (its own phase and life, then per life a spawn point, direction, speed and spin from a hash), moving in closed form under gravity and linear drag, with an optional wobble. Nothing is simulated or uploaded per particle, so a million sparks cost only their pixels; they sort, blend, take intensity and emission, and are lit in lit layers like sprites. Use them for a scene's ambience (embers, rain, snow, dust, fireflies, fountains); the scene's `GPUParticles2D` (simulated on the CPU, with collisions and sub-emitters) stays the tool for gameplay particles.

## Scaling HD art

A LINEAR texture made with `cw_texture_desc.mipmaps` packs into atlas pages that carry three smaller copies (half, quarter and eighth size, box filtered on the CPU at load), so art drawn smaller than its pixels (HD art on a 720p screen, a zoomed-out camera, a far parallax layer) stays smooth instead of shimmering. These pages leave 8-pixel gutters round each image so the smaller copies never take a neighbour's colour. `samples/r2d_motion` draws one fine-detailed image at 1/2 to 1/8 size with and without them.

## Sharing GL with 3D

render2d needs a current GL context (call it from `init` onward) and sets every piece of GL state it uses at each `cw_r2d_end`. When you mix it with three.c yourself, call `t3_renderer_reset_bindings` after a 2D pass and before the next 3D render so three.c's state cache matches GL again; the scene runner already does this. To put a 3D view inside a 2D scene, create a render target with `depth`, point three.c at `cw_target_fbo`, and draw the target as a sprite. `samples/r2d_interop` renders a torus knot into a target and draws it as a rotating, translucent sprite over a 3D frame.

## Seeing what it did

Natively, `CW_R2D_TIMERS=1` times each part of `cw_r2d_end` on the GPU (GL time queries, WebGPU timestamps) into the debug fields `r2d_gpu.draw`, `.cull` (the opaque depth pre-passes), `.field`, `.lights`, `.gi_source` and `.gi` (global illumination's source pass and the rest), `.composite` and `.post` (ms a frame; `tools/r2d-perf.mjs --timers` prints them). `cw_r2d_get_stats` reports the frame's passes, draw calls, batches, vertices, sorts, chunk draws and rebuilds, lights, atlas pages, missing glyphs, the WebGPU calls the renderer made (`gpu_calls`: in a cart each is an import call, and a weak host holds 60 fps under about 8000 a frame for the whole game) and the global illumination's cascades. `tools/r2d-perf.mjs` runs `samples/r2d_stress` case by case (sprites from 10k to 200k, lit sprites under 1 to 500 lights with and without shadows, tilemaps, particles, text, the post chain) on GL and WebGPU at any size and reports the CPU time, the frame time, the draws and the WebGPU calls of each. `cw_r2d_debug_register("r2d")` publishes them as debug fields, which is how the gates compare batching across hosts. `cw_canvas_get_stats` counts the tree's canvas items, layers, lights and occluders. The [debug channel](guide:debug-channel) can screenshot the world layer alone, and the [profiling](guide:profiling) guide covers GPU timings.

## Samples

| Sample | Shows |
|---|---|
| `r2d_sprites` | sprites, shapes, blend modes, nine-slice, clip, render targets |
| `r2d_text` | SDF and bitmap fonts, wrap, alignment, measurement |
| `r2d_tilemap` | chunked tilemaps, animated tiles, flips, cell edits |
| `r2d_camera` | canvas layers, cameras, parallax, minimap, `INTEGER` scaling, the sharp filter at any scale |
| `r2d_lights` | lit layers, normal maps, shadows, ambient |
| `r2d_normal_turn` | turned and flipped normal-mapped sprites and instances, lit by their turned normals |
| `r2d_hdr` | the post chain, emission, material maps, soft shadows, spot, directional and volumetric lights, global illumination, screen reads, distortion (and layers above it), lookup tables |
| `r2d_motion` | wind sway, trails, mipmapped atlas pages |
| `r2d_stress` | the perf cases of `tools/r2d-perf.mjs`, sprite instances, a side-scrolling HDR scene (`sidescroll`) |
| `r2d_interop` | three.c into a render target, 2D over 3D |
| `r2d_bench` | 100,000 sprites, bulk vs single submission |
| `fx_particles2d` | `GPUParticles2D` |

The reference pages: [render2d.h](header:render2d.h), [scene2d.h](header:scene2d.h), and the node pages from `Node2D` down.
