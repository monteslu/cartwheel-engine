# Changelog

What each release added, changed and fixed. Releases are git tags; each one's
prebuilt SDKs (one per target, see [docs/sdk.md](docs/sdk.md)) are on its
GitHub release. The guides in `docs/guides/` cover each feature in depth.

## v0.39.2 (2026-11-09)

### Changed
- **GL's depth prepass costs about a fifth less.** It draws into a framebuffer that holds only the scene's depth. With the scene's colour target bound and colour writes masked off, Mesa's radeonsi still ran much of each colour program's fragment work for the prepass (the sun's cascaded shadow filter above all: the prepass read 0.35 ms with the sun's shadow off and 0.50 with it on, where WebGPU's pipelines, which know at creation that they write no colour, read 0.33 to 0.34 either way). The depth written is the same, so the picture is too (Ember Throne's meadow, native GL frames 300 and 420 byte-identical with and without; the romdev GL and WebGPU frames of its cart identical to the last release's). Ember Throne's meadow on the 890M at 1080p High, six interleaved rounds (load 2 to 3, the GPU at 1380 to 1450 MHz): the GL prepass 0.50 to 0.40 ms. `CW_TEST_PREPASS_COLOR=1` (native) draws the prepass into the scene target as before, the timing control (0.50 ms in the same rounds).

### Fixed
- **`tools/run-cart.mjs`: a long romdev run is no longer cut off at five minutes.** Its romdev calls went through Node's fetch, which stops waiting for a reply after 300 s, so a 2400-frame step on a busy GPU pool failed with "fetch failed". The calls now go over node:http and wait `CW_ROMDEV_TIMEOUT_MS` (default 300000) plus `CW_ROMDEV_FRAME_MS` (default 500) for each frame a call steps; a call past that fails naming both (the testing guide says so). Control: `CW_ROMDEV_TIMEOUT_MS=2000 CW_ROMDEV_FRAME_MS=1` fails the load at 2 s.

## v0.39.1 (2026-11-09)

### Fixed
- **The sun's shadow maps are drawn once a frame with screen-space reflections or SSGI on, not twice.** The "gi" pass draws the half-resolution G-buffer before the view's own render, and that render drew every armed shadow map (with the surfaces the G-buffer leaves out missing), so the pass re-armed them and the view's render drew them again. The G-buffer now draws holding the maps (`t3_renderer_set_shadow_hold`, as the probe faces have since v0.39.0): it reads none of them, and the view's render draws them once. The frame looks the same (the maps it samples were always the second drawing). arenafps's Foundry in four-view split screen (a 2048 sun map, SSR on): 2 shadow-map draws a frame before, 1 after; the "gi" pass's GPU time 1.75 to 1.16 ms on GL on the 890M (mean of two runs each, on a machine under load: indicative). `CW_TEST_GBUF_SHADOWS=1` (native) draws them in the G-buffer as before, the timing control.

## v0.39.0 (2026-11-09)

### Changed
- **Reflection probe captures cost a fraction of what they did, and no frame pays for a whole one.** three.c 0.187.6 draws a capture's filtered levels side by side in one target and copies them into the cube (each of the 30 face-and-level passes used to wait for the one before it), and copies level 0 instead of filtering it. A ReflectionProbe3D now captures at its own `resolution` (r187's `fromScene` raised every capture to 256 a face, so a 64 or 128 probe paid for 256). An EVERY_N_FRAMES probe refreshed every 2 frames or more spreads each capture over those frames (a few faces of the scene a frame, the filter in a frame of its own); a first capture, a new size or a request is still made whole. The faces use the shadow maps as the last frame drew them (`t3_renderer_set_shadow_hold`), so a capture draws no shadow pass of its own. A mirror-ball pinball table at 1080p High on the 890M (a 128 probe every third frame): the worst frame from 8.5 to 4.3 ms on GL and from 10.0 to 4.9 ms on WebGPU, the window's mean from 3.5 to 3.0 ms (GL) and 4.3 to 3.3 ms (WebGPU). A 128 capture reflects a little less sharply than the 256 it used to be raised to; set `resolution = 256` to keep that, now at about two thirds of its old cost.
- **Terrain: two reads a map for the stochastic tiling, not three.** The least-weighted of a point's three cells has no share and is not read; the other two are read together. The cells and their offsets are unchanged, the ground looks the same (a pebbled path or a flagstone floor is a little crisper: no ghost of a third offset under it), and Ember Throne's meadow scene pass went from 4.43 to 3.71 ms on GL and 4.06 to 3.80 ms on WebGPU (interleaved rounds, a loaded machine, the GPU near 900 MHz). Which two layers a point blends is worked out on the CPU per control cell (a new `pair` texture, rebuilt with the control map): the shader reads one control texel and that cell instead of searching seven candidates over four texels. Projects with terrain regenerate their program tables to get both (an older table still draws as before).
- **The sun disk is drawn only where it can be** (the scissored box round its disk), on GL and WebGPU: the same pixels lit, nothing else shaded. Ember Throne's meadow: `sky_sun` 0.07 to 0.04 ms on GL, 0.29 to 0.11 ms on WebGPU. What WebGPU's pass timers still give `sky_sun` beyond that is the wait for the scene before it: with the sun's pass skipped, that time shows in the next pass instead.

### Fixed
- **WebGPU pass timers time the water's refraction.** three.c's render of the scene under the water ran on the frame encoder with no timestamps, so the "water" pass read only its depth copy (0.03 ms) where GL read the whole render; it is now timed as a span of the pass (0.22 ms in Ember Throne's marsh). GL's water pass there (0.40 ms) is that render and the frame's shadow maps, which the frame's first render draws (the depth prepass when there is no refraction).

### Added
- **`tools/lib/gpu-clock.mjs`**: the test GPU's shader clock sampled while a run measures GPU time (`gpuClock`, `clockLine`). An integrated GPU's clock follows how busy it is, so a CPU-bound run on a loaded machine reads every pass at the floor clock (600 to 700 MHz of the 890M's 2900) and two to three times slower. The profiling guide's new section says how to time GPU work so the numbers hold: A and B back to back in one hold of the suite lock, the clock and the load beside every time, medians of window means, a control that must fail. Neither `CW_WGPU_POWER` nor a pool's `CW_GPU_POWER` moves the clock (measured: the same runs at the same clocks).
- `samples/gi_probe_check`: `GI_EVERY` and `GI_RES`; `tests/gi-probe.mjs` checks captures spread over frames against a whole one, and a 64 capture against the known walls.

## v0.38.9 (2026-11-08)

### Removed
- **Screen shake.** No camera shakes in cartwheel: a hit, a landing or a blast is felt in the pads (`cw_input_rumble`) and seen in the effects. The shell's Settings page has no Screen shake row. `cw_shell_screen_shake()` is deprecated and returns 0 at any stored level, so a game that multiplied its shake by it shakes nothing. `cw_shell_shake_level` and `cw_shell_set_shake_level` are deprecated and only keep the level an older save stored (a save keeps its layout and a game's save migration can still read it). `cw_shake_offset` is deprecated: `cw_explosion_shake` is now documented as a strength for a pad's rumble, never a camera offset. The shell unit test checks 0 at every level with reduced motion on and off, and that the stored level survives an init; its control (the old Full answer of 1) fails. The explosions, game shell and accessibility guides say so.

## v0.38.8 (2026-11-08)

### Fixed
- **The explosion kit's flash no longer strobes.** Its pooled light jumped to full brightness on the blast's first frame and flickered about 11 times a second (its energy times 0.9 + 0.1 sin(70 t)) as it faded. It now swells over the new `flash_rise` property (eased, 0.08 s by default, at most half the flash) and falls smoothly to nothing at `flash_duration`: one rise and one fall, nothing that oscillates. One `Explosion3D` takes a flash at most every third of a second, so a looping or quickly refired blast cannot strobe either, and each flash runs on its own curve to its end, even when its node fires again or leaves the tree (a refire used to leave the old light lit at whatever level it had reached). `tests/explosion.mjs` traces the flash energy every frame (`FXE_TRACE=1` in `fx_explosion`) and holds it to a swell over at least 3 frames from under a quarter of its peak followed by only a fall; the old curve fails the same judge. Games that use the kit (hordeshooter, crawler, arenafps and fighter in more-cartwheel-examples; towers in even-more-cartwheel-examples) get the new curve when they update; destructo sets its flash to 0 and is unchanged.
- **Engine samples and the action2d game no longer flicker or flash.** `night_street`'s muzzle flashes switched on and off about 3.5 times a second; each now swells over 5 ticks and fades over 15, 1.5 times a second. `rim_light`'s pulsing rim beat 4.3 times a second and now about once. `r2d_hdr`'s torches carried a 4.9 Hz wobble, now 1.2 Hz; `showcase`'s campfire light changed 12 to 40 times a second, now under 3. action2d's hero blinked on and off 5 times a second while invulnerable and now turns translucent; its full-screen white boss flash (95 % on the first frame) and red hurt tint (35 % on the first frame) now swell softly to 16 % and 14 %. The accessibility guide lists what the engine's own effects do.
- **2D normal maps turn with their sprites.** A rotated or flipped normal-mapped sprite was lit as if unturned (src/render2d): its normal map's x and y were written to the normal buffer as they were, whatever way the sprite faced. The lit sprite, instance and particle shaders now take where the texture's axes point on the canvas from the screen derivatives of the uv and of the canvas position, and turn the normal into them, on GL and WebGPU: `rotation`, `flip_x`, `flip_y`, an instance's rotation and flip flags, a particle's spin and a turned camera all count. An unturned, unflipped sprite writes exactly what it did. New sample `r2d_normal_turn` and gate `r2d-normal-turn`: turned sprites and instances (30, 90, 137 and 200 degrees, each flip, both, a flip then a turn) match unturned sprites whose normal maps hold the turned normal, within 3 levels on both APIs; the unturned normals (the old lighting) are at least 12 levels off in every row.
- **WebGPU pass timestamps time every pass.** While the pass timers ran, every draw opened its own render pass with its own pair of timestamps, so a pass of a few hundred draws used up the frame's 256 timestamps and the passes after it (velocity, TAA, tonemap and the rest of the post chain) went untimed: `gpu_ms` undercounted on WebGPU against GL. Draws into one target now share a render pass and one pair while timing too (an open render pass closes when the timed pass changes, so each pass keeps its own time), clears are timed with the draws after them, the frame holds 1024 timestamps, and running out says so once. The decals pass and the sky's sun disk, which record render passes of their own, were never timed on WebGPU (they read 0 ms); they are timed now. Gate `pass-timing`: `fx_pipeline` with 600 draws in its scene pass (its decals and particles after it) and `post_stack`'s full segment (velocity, TAA, tonemap) are timed on WebGPU for every pass GL times; its control (`CW_TEST_TIMING_SPLIT=1`, a render pass per draw as before) leaves passes untimed.
- **A big 2D stream on WebGPU goes up in pieces.** render2d writes its vertex and index streams to the GPU at most 4 MB at a time (one 6 MB write was seen to abort inside Dawn), never grows a stream buffer past the device's largest buffer, and a pass too big for that buffer is not drawn and logs one clear error instead of losing the device. Gate `r2d-big-stream`: `r2d_stress` sprites200k (a 22 MB vertex stream) draws on WebGPU as on GL, and with `CW_TEST_R2D_STREAM_MAX` lowered under the stream the pass is skipped with the error and the run ends normally.

## v0.38.7 (2026-11-08)

### Fixed
- **The flash check no longer calls a 3 Hz pulse flicker.** Three flashes a second is the bound itself, but the one-second spectrum's 1 Hz bins put any slow wobble of a game scene (a light swaying, a camera bob) at 3 Hz, so real games failed on it. The flicker measure now reads two-second windows every half second (half-hertz bins) and counts a peak from 3.5 Hz up; a peak that drifts one bin between windows still counts as held. The `flash-check` gate adds a small light pulsing at exactly 3 Hz (passes) and at 4 Hz (fails), and `node tests/flash-check.mjs --synthetic` runs the synthetic cases without a GPU.

## v0.38.6 (2026-11-08)

### Added
- **A flash check: `tools/flash-check.mjs`.** It runs an app natively for N frames on the fixed clock (with a gate's input script and start requests) and holds every frame to the photosensitive flash thresholds of WCAG 2.2 success criterion 2.3.1 and the Harding test: at most three general flashes and three red flashes in any second over a sixth by a sixth of the screen, the same for small lights that switch together over a quarter of a 10-degree field, and no regular flicker from 3 to 30 Hz in any twelfth by twelfth of the screen. `--max-rise` can also bound the hardest two-frame onset. A frozen capture fails (it proves nothing), and every check runs its control: the same frames with an 8 Hz full-screen strobe laid over them must fail. `tools/lib/flash.mjs` is the analyser for a games repository's own sweep (`flashGate`, `analyseFrames`, `readFrameDump`, `withStrobe`); it also reads a directory of PNG frames. The `flash-check` gate holds it to synthetic runs with known answers (a moving high-contrast picture, a swell and fade, a 2 Hz pulse and random sparkles pass; a full-screen strobe, a red strobe, dots switching together, a small light flickering at 8.6 Hz, a hard onset and a frozen run each fail on their measure) and checks fx_explosion's blasts, which pass.
- **`CW_FRAME_DUMP`** (native, with `CW_FRAMES`): every frame from `CW_FRAME_DUMP_FROM` on, box-filtered down by `CW_FRAME_DUMP_SCALE` (default 8) in linear light and appended raw to one file, on GL and WebGPU. It is how the flash check sees every frame of a run.
- **The accessibility guide has a section on flashes**: the thresholds, and how to make a flash that passes (rise over a few frames and fade over many, rate-cap anything that retriggers, blink slowly and softly, no regular flicker on lights). The testing guide documents the check.

## v0.38.5 (2026-11-08)

### Fixed
- **The terrain's second layer is not read where it cannot show.** Where two layers blend, the height blend gives the second layer exactly nothing wherever the first layer's weighted height stands out over the second's share by more than the blend's sharpness, since a layer's height is at most 1. The second layer's albedo (three to six texture reads, more on a steep face) was still read there to learn its height. That bound is now tested first, and the second layer is read only where it could get a share. WebGPU frames are the same to the bit as before (terrain_demo, open_world, Ember Throne at three places, jetski, skyfighter, tuxracer). The skip itself is exact on GL too: against a build with the same shader that never skips, every GL and WebGPU frame checked matches to the bit.
- **The terrain's side projections read the normal map with gradients taken before any branch.** The triplanar weights read the terrain's normal map inside branches that differ pixel to pixel, with the derivatives the hardware takes across a pixel's neighbours; GLSL leaves those undefined there (WGSL's uniformity rule forbids them, and r187 programs turn its check off), so a driver may return anything at the edges of such a branch. They now read it with explicit gradients (`textureGrad`, `textureSampleGrad`) of the same coordinates, taken before the branches. WebGPU frames are unchanged; GL frames move where the old reads were undefined: none in open_world, tuxracer and Ember Throne's wood, 24 to 164 pixels by one or two levels in Ember Throne's spire and skyfighter, 51 pixels (up to 21 levels) in Ember Throne's meadow, 213 (up to 25, along hill silhouettes) in terrain_demo and 1664 (up to 7) in jetski, of a 1080p frame. The terrain guide's Performance section has the attribution of the terrain's cost.

### Measured (Radeon 890M, native, 1080p High, Ember Throne's bench; scene pass GPU time, median of 7 interleaved runs)

| Place | GL before | GL after | WebGPU before | WebGPU after |
|---|---|---|---|---|
| meadow | 3.27 ms | 3.16 ms | 3.51 ms | 3.30 ms |
| wood | 3.22 ms | 3.13 ms | 3.65 ms | 3.44 ms |
| spire | 3.07 ms | 3.00 ms | 3.29 ms | 3.20 ms |

### Moving a game to v0.38.5
- Regenerate the program tables of any app that draws a Terrain3D (`ninja programs`, or `tools/programs.mjs --table`): the terrain states' shaders changed.

## v0.38.4 (2026-11-08)

### Fixed
- **A dev build counts from the nearest semver tag.** `tools/lib/app-version.mjs` took the nearest `v*` tag reachable from HEAD; when that tag was not semver (`v1.4`, `v1.5.02`) it gave up and a dev cart said `0.0.0-dev.g<commit>`. It now passes over such tags (`git describe --exclude` each one) and counts from the nearest `v<semver>` tag, as wasmcart v0.39.1 does. `tests/app-version.mjs` puts two such tags nearer than `v1.2.3` (the old code fails it).

## v0.38.3 (2026-11-07)

### Fixed
- **Terrain3D obeys `visible`**: the terrain and its scatter are drawn, and cast shadows, only while the node and every Node3D above it are visible. Its meshes now sit in one group the pass shows or hides; before, they were added straight to the scene root and a hidden node drew anyway. A hidden terrain is not selected or posed either (no CPU time per frame), and it still collides. `terrain-coverage` checks a hidden terrain, a hidden parent and a terrain shown again (v0.38.0 fails the first two).
- **A Terrain3D owns its terrain's origin**, and now says so: the node set the origin every tick, so `cw_terrain_set_origin` on a node's terrain was silently undone. It is still undone (move the node instead), with a warning naming the node, and the header and the terrain guide state the rule. `unit-terrain` checks both directions.
- **The terrain's second layer reads less**: where two layers blend, the second layer's top-down normal is read only where the height blend gives it a share (none where one layer's height stands out by more than the blend's sharpness). WebGPU frames are the same to the bit; GL frames move by one to eleven levels at a few scattered pixels: 15 to 220 of a 1080p frame in the terrain samples and example games checked (the GL compiler rounds the reordered code differently).
- **Cascaded sun shadows read only what they use**: the shadow lookup and filtering are skipped where no cascade weighs in (past the last cascade's far edge), and the fade partner's filter runs only across the band where it fades in. Before, every lit pixel ran both cascades' filters (ten compare samples) and threw away what the weights zeroed. Frames are the same to the bit on GL and WebGPU (every sample and example game with sun cascades checked). In a meadow view at 1080p High the scene pass went from 3.68 to 3.61 ms on GL and from 3.67 to 3.59 ms on WebGPU (the whole sun shadow, maps and lookups, is about 0.4 ms of that pass there).

### Measured (Radeon 890M, native, 1080p High; a ground that blends two layers across the view; scene pass GPU time)

| Case | GL | WebGPU |
|---|---|---|
| whole scene pass, v0.38.0 | 3.96 ms | 4.86 ms |
| whole scene pass, v0.38.3 | 3.79 ms | 4.52 ms |
| the second layer alone, v0.38.0 | 1.0 ms | 2.1 ms |
| the second layer alone, anisotropic filtering forced off in the driver | 0.2 ms | 0.3 ms |

The WebGPU excess is the driver's: the generated shaders have the same structure on both backends and compile to code of the same size, and the cost is in 8x anisotropic fetches from a different layer of the layer array (the terrain guide's Performance section has the details).

## v0.38.2 (2026-11-07)

### Fixed
- **The grounding gate judges each caster where it stands.** `tests/quality-grounding.mjs` took every caster's base as the lowest point under the middle of its box, and judged whatever shadow fell there. For two kinds of caster nothing of it stands there: a caster whose box stands around the camera (a ring of walls, a floor; the camera is above its footprint), and a long one (a wall, a fence, a row of stands: a footprint over 6 times as long as it is wide and as it is tall). A night arena's walls failed frames because a floodlight High casts shaded the middle of the floor. The `shadow_contacts` request now marks the first `around` (the gate leaves it out: it has no single base) and the second `along` (its base is the point of its base line nearest the camera), and keeps the old point as `cx`, `cy`. `--centre` judges every caster the old way.

### Added
- `tools/lib/grounding.mjs`: the gate's judge, on its own so it can be checked without a GPU. Gate `quality-grounding-judge`: the judge on made-up frames; controls: the old rule fails a frame wrongly over a ring of walls around the camera and a long wall, no shadows fails, and a wide caster that is neither is still judged at its middle.

### Moving a game to v0.38.2
- Nothing to change. A games repository that worked around wide casters in its own grounding check (a size cap on the bases it counts) can drop the workaround.

## v0.38.1 (2026-11-07)

### Fixed
- **A cart's manifest carries its game's real version.** Every cart was packed with `"version": "0.1.0"` whatever its release. `cw_add_app` now works the version out on every build, from the game's git checkout: at a clean `v1.4.2` tag it is `1.4.2`, so a tag build is the release by construction; anywhere else it is the last tag plus `-dev.g<commit>` (`1.4.2-dev.g1a2b3c4`, `0.0.0-dev.g<commit>` before the first tag), so a dev cart is never taken for a release; outside git it is `project.cwproj`'s `version`, else `0.1.0`. When the version moves (a new tag, a commit) the next build packs the cart again with it, without relinking (`tools/cart-version.mjs`). The engine's own sample and test carts follow the same rule. See docs/shipping.md, "The version".

### Added
- `cw_add_app(... VERSION <semver>)` for one app, and `-DCW_APP_VERSION=<semver>` for every app of a build, to set the version by hand. Both must be semver; anything else stops the configure.
- `tests/app-version.mjs`: the semver rule (cmake and JavaScript agree), the git rule in a scratch repository, the repack when the version moves, and a built cart's manifest, with controls that must fail.

### Changed
- `cwexport` takes the same version as the cart build (`--version`, else the preset's `version`, else the git checkout's, else the project's `version`, else `0.1.0`), so an export's package names and its `.wasc` always agree; an explicit version reaches the cart as `CW_APP_VERSION` for that build only. The version must be semver (`1` and `1.2` in `project.cwproj` still mean `1.0.0` and `1.2.0`).

### Moving a game to v0.38.1
- Nothing to change. A `version` key in `project.cwproj` no longer sets a game's version inside a git checkout (the tags do); remove it, or keep it for builds outside git. A CI that builds carts from a shallow clone of a branch has no tags, so those carts say `0.0.0-dev.g<commit>`; a tag push checks its tag out and gets the release version.

## v0.38.0 (2026-11-07)

### Added
- **Sprite instances** (`cw_r2d_instances`, `cw_sprite_instance`, `cw_r2d_instance_buffer_*`): many sprites of one texture in one instanced draw, 24 bytes an instance (where, turn, scale, colour, a frame of up to 32 source rects, flips) built into quads on the GPU; per frame (copied at the call, one upload a pass) or kept in a buffer written once and drawn every frame for nothing on the CPU. They blend, take intensity, emission, flash and wind sway and are lit in lit layers. 200,000 moving sprites: 3.3 / 5.7 ms of CPU a frame as sprites, 0.36 / 0.99 ms as per-frame instances, under 0.01 / 0.07 ms from a kept buffer (GL / WebGPU, 890M, 1080p); the GPU time is the pixels', as before.
- **Lookup tables in the 2D post chain** (`cw_r2d_post.lut`, `lut_amount`, `cw_r2d_lut_load`, `cw_r2d_lut_from_strip`, `cw_r2d_lut_pixels`): a 3D colour table applied to the finished display colour, authored as a strip image (grade a screenshot holding the identity strip in any image editor, cut the strip back out); presets `cw_r2d_lut_preset`: warm, cool, teal and orange, black and white, sepia, bleach bypass, moonlight.
- **Light textures** (`cw_light2d.texture`, `texture_src`; Light2D `texture`): a point or spot light's colour times an image laid over its radius square and turned by its angle (window panes, a lantern's cage, stained glass).
- **Sharp scaling of a virtual canvas** (`cw_r2d_pass_desc.scale_filter`): `SHARP` draws each canvas pixel as a crisp block of its true width and blends only the window pixel on a block's edge, so pixel art at a scale that is not whole neither shimmers nor blurs; `LINEAR` smooths.
- `CW_R2D_FEATURES` 3. `CW_R2D_TIMERS` parts `cull` and `gi_source`. Stat `instances`. `r2d_stress` cases `inst200k`, `instkept200k` and `sidescroll` (a side-scrolling HDR screen: sky, four fogged parallax layers, a lit back wall, about 800 normal-mapped tiles, 7 soft-shadowed lights, the chain, global illumination). `r2d_hdr` modes `lut`, `lutid`, `cookie`, `front`, `frontnodistort`; `r2d_camera` mode `sharp`.

### Changed
- **Covered pixels are not shaded** in the offscreen canvases (the HDR canvas, a lit run's buffers): each submission gets a depth from its place in the sorted order, a group of layers first draws its fully opaque texels nearest first into a depth buffer (colour off), then draws as always testing it, so a pixel behind an opaque one is never shaded. The picture is the same to the bit. Groups end where something reads what was drawn (screen reads, distortion, the end of a lit run, the HUD layer). `CW_R2D_CULL=0` (native) turns it off.
- **A distortion layer bends only what is under it**: a layer above it (under the HUD) draws over the bent picture straight (one extra full-screen pass, only then). Before, every layer under the HUD bent.
- **Global illumination takes fewer passes**: last frame's bounce is part of the source pass, the flood starts from the source (no seed pass), and the composite adds the irradiance (no full-size apply pass).
- **Low and Very Low scale the 2D lights and HDR**: the lights shade a light buffer half (Low) or a third (Very Low) of the canvas each way, upsampled by an edge-aware composite (normals and occluder edges keep their place); the HDR canvas is R11G11B10 float where the host renders it, the pass clears opaque and no layer reads the screen; GPU particles draw the effects detail's share of each emitter (0.25 Very Low, 0.4 Low, 0.7 Medium); global illumination's field never exceeds the pass's own `resolution` below High (half of it on Low). High and Ultra draw as before.

### Measured (Radeon 890M, native, GL / WebGPU; the before and after runs interleaved in one suite)

| Case | v0.37.0 | v0.38.0 |
|---|---|---|
| explorer_bench, 1080p High, whole 2D frame to GPU completion | 4.5 / 5.1 ms | 3.1 / 3.3 ms |
| the same, its global illumination (GPU) | 1.6 / 2.1 ms | 0.29 / 0.36 ms |
| explorer_bench, 4K High | 15.0 / 17.6 ms | 11.1 / 12.1 ms |
| explorer_bench, 1080p Low | 4.3 / 4.9 ms | 2.2 / 2.4 ms |
| explorer_bench, 1080p Very Low | 2.1 / 2.2 ms | 1.6 / 1.7 ms |
| sidescroll (r2d_stress), 1080p High, layer drawing with culling | 0.62 / 0.60 ms | 0.48 / 0.46 ms (pre-pass included) |
| 64 shadowed lights (shadow64), the lights pass on Low | 2.0 / 1.6 ms | 0.58 / 0.47 ms |
| the same on Very Low | 2.0 / 1.5 ms | 0.34 / 0.26 ms |
| 200k sprites, CPU | 3.3 / 5.7 ms | instances 0.36 / 0.99 ms, kept 0.003 / 0.07 ms |

The global illumination source pass was the cost at an eighth of the canvas: each field pixel read 16 filtered taps of four full-size buffers in a row, with too few pixels in flight to hide the waits. It now runs in two steps (half the canvas, then the field). Depth culling helps scenes with opaque art over other layers (sidescroll's wall over its parallax layers); explorer's walls fade and its parallax is translucent, so it has no opaque art to cull with and takes no pre-pass. Every other 2D sample draws the same pixels as v0.37.0 (r2d_hdr within 2 levels on 3 % of its pixels, from global illumination's arithmetic now in 32-bit floats).

### Moving a game to v0.38.0
- Nothing to change in code. A layer drawn above a distortion layer (and under the HUD) now stays straight; a game that relied on it bending moves that layer under the distortion layer. `CW_R2D_FEATURES >= 3` gates the new calls for code that builds against older SDKs too.

### Tests
- `r2d-round2` (GPU, GL and WebGPU): culling changes no pixel (r2d_hdr, sidescroll), control: culling without its half step must lose opaque texels; the identity table keeps the picture within 2 levels, control: the teal and orange table must change it; light textures change the lit picture; a lattice above a distortion layer stays straight while the picture under it bends, control: the old bend-everything-at-the-end must bend the lattice; 200k instances draw the sprites' picture, control: the still kept buffer must differ; Very Low and Low near High but not equal; GL against WebGPU for every frame.
- `r2d-hdr`: a lookup table control. `r2d-perf`: the instance and sidescroll cases, instances one draw.

## v0.37.0 (2026-11-07)

### Added
- **HDR 2D and a post chain** (`cw_r2d_pass_desc.post`, `cw_r2d_post`): the layers under `hud_layer` draw into an RGBA16F canvas, then bloom (a soft-knee threshold, up to 8 halvings, a 13-tap filtered downsample and a tent upsample), exposure, a tone curve (`SOFT` by default: everything under its knee exactly as drawn), a grade (contrast, saturation, lift, gamma, gain, tint), a vignette and a dither below one 8-bit level. Layers from `hud_layer` up draw after it. A NULL `post` draws exactly as before.
- **Intensity and emission**: `cw_r2d_set_intensity` (colour x 1..16, so glowing things pass 1 and bloom) and `cw_r2d_set_emissive` (the share of the colour lit layers leave unlit).
- **Material maps** (`cw_texture_set_material_map`, `cw_texture_load_material_map`): emission, specular and gloss per texel; the lights add a glint on specular texels.
- **Lights**: spot cones (`cone`, `angle`, `cone_soft`), directional lights (a sun or moon over the whole layer), volumetric scatter (shafts between occluders, a glow round a lamp), soft shadows (`softness`), and `cw_r2d_layer_desc.occluder_depth` (light reaches only that deep into occluders).
- **Global illumination** (`cw_r2d_pass_desc.gi`, `cw_r2d_gi`): radiance cascades over a distance field of the occluders and the glowing pixels; glowing things light the world round them, light bounces off occluders frame after frame, occluders shadow all of it. Off on Very Low; the field at 1/8, 1/4, 1/4, 1/2 of the canvas from Low to Ultra.
- **Screen reads and distortion**: `cw_r2d_layer_desc.screen_read` with `cw_r2d_screen_texture()`; distortion layers (`cw_r2d_layer_desc.distortion`) bend the picture by their draws' red and green, with `cw_r2d_noise_texture()` for heat haze and ripples and `cw_r2d_shockwave`.
- **2D water**: `cw_water2d` (a spring surface, splashes, deterministic on the CPU) and `cw_r2d_water` (the world refracted through the body and mirrored on it from the screen texture, a depth tint, caustics from `cw_r2d_caustics_texture()`, a lit surface line).
- **Wind**: `cw_r2d_pass_desc.wind` and `cw_r2d_set_sway`: sprites' top edges sway in the vertex stage (grass, reeds, banners) at no CPU cost.
- **GPU particles** (`cw_r2d_particles`): stateless emitters, one instanced draw each, every particle a closed-form function of the time (spawn box, spread, speed, gravity, drag, life, size, colour, spin, wobble); lit in lit layers, additive with intensity for blooming sparks. A million particles cost their pixels and nothing on the CPU.
- **Trails**: `cw_r2d_trail`, a ribbon along a path tapering and fading from head to tail, textured along its length (`uv_repeat`).
- **Occluders from art**: `cw_r2d_set_occluder(true)` makes later sprites, shapes, text and tilemaps in lit layers cast shadows (and block global illumination) wherever their alpha is over half: ground tiles and props need no polygons.
- `CW_R2D_FEATURES` (2) in render2d.h, for code that builds against older SDKs too.
- `CW_R2D_TIMERS=1` (native): the GPU time of each part of `cw_r2d_end` (draw, field, lights, gi, composite, post) as debug fields `r2d_gpu.*`, on GL and WebGPU; `tools/r2d-perf.mjs --timers`.
- **Fog and flash**: `cw_r2d_layer_desc.fog` turns a layer toward a colour (atmospheric depth for parallax layers); `cw_r2d_set_flash` turns sprites and shapes toward their colour (hit flashes, silhouettes).
- **Mipmapped atlas pages** (`cw_texture_desc.mipmaps`): three box-filtered halvings made at load, 8-pixel gutters, so HD art drawn small stays smooth.
- Scene nodes: Light2D `softness`, `cone`, `cone_soft`, `directional`, `volumetric`; Sprite2D `sway`, `emissive`, `intensity`; CanvasSettings `occluder_depth`, `post` (with `exposure`, `bloom`, `bloom_threshold`, `bloom_intensity`, `tone`, `contrast`, `saturation`, `vignette`; screen-space CanvasLayers draw after the chain), `gi` (`gi_intensity`, `gi_bounce`), `wind_strength`, `wind_speed`, `wind_wavelength`, `wind_gusts`.
- Stats: `gpu_calls` (the WebGPU calls the 2D renderer made this frame: in a cart each is an import call) and `gi_cascades`.
- Samples `r2d_hdr`, `r2d_motion`, `r2d_stress`; `tools/r2d-perf.mjs` (the stress cases on GL and WebGPU at any size and preset: CPU ms, frame ms, draws, WebGPU calls).

### Changed
- **The lights of a lit run are tiled**: sorted into 32-pixel screen tiles on the CPU, then one full-screen pass shades each pixel with its tile's lights, reading its normal and material once and writing once (no blending of overlapping light quads). 500 lights: 3 draws and 70 WebGPU calls a frame, from 502 draws and 3058 calls.
- **Shadows come from a distance field of the occluders** instead of stencil volumes rebuilt on the CPU per light per occluder edge: the outlines are filled into a mask, a jump flood makes the field once per lit run, and each lit pixel marches toward its light. 500 shadowed lights over 120 occluders: 1.2 ms of CPU and 231 WebGPU calls, from 4.8 to 8.1 ms and 6061 calls. Shadows can be soft now. The field's size and the march's steps follow the effects detail. An occluder's own pixels stay lit, as before; the other 2D samples draw the same pixels as before, and the r2d_lights sample's shadow edges move by a pixel or two (0.04% of its pixels).
- The lit layers' normal and material buffers are made only when a normal map, a material map or emission is drawn into them.

### Fixed
- Batches merged across canvas layers that draw to different targets, and a lit run merged across layers lost the lights of its later layers.

### Measured (Radeon 890M, 1080p, native, GL / WebGPU; GPU parts from CW_R2D_TIMERS)

| Case | Before | After |
|---|---|---|
| 20k normal-mapped sprites, 500 lights | 502 draws, 3058 WebGPU calls | 3 draws, 70 calls; lights 1.6 / 1.4 ms |
| the same, 500 shadowed lights, 120 occluders | CPU 4.8 / 8.1 ms, 1002 draws, 6061 calls, hard shadows | CPU 1.2 / 1.3 ms, 13 draws, 231 calls; field 0.5 ms, lights 3.6 / 3.0 ms, soft shadows |
| 16 shadowed lights + global illumination | (no GI) | field 0.5, lights 1.0 / 0.8, GI 0.8 / 1.0 ms, 34 draws |
| post chain, 6 bloom levels | (none) | 0.6 ms (0.16 without bloom, as on Very Low) |
| a million GPU particles | (none) | 9.3 / 10.9 ms, 1 draw, 0.05 ms CPU; 100k: 0.7 / 0.8 ms |

Shadowed lights cost more GPU time than the old hard stencil shadows (the field and a march per light per pixel: 64 large shadowed lights 3.3 / 2.6 ms) and far less CPU and far fewer calls; the field halves on Very Low and Low (0.16 ms).

### Tests
- `r2d-hdr` and `r2d-motion`: GL against WebGPU, with controls that leave out the chain, bloom, global illumination, shadows, distortion, the pool, the penumbra, the wind, the trails and the mipmaps, each of which must change the frame.
- `r2d-perf` (local): every stress case within its WebGPU call budget, the lit cases within their few draws; control: budgets of one call fail.

## v0.36.1 (2026-11-07)

### Fixed
- WebGPU pass timing sees the GPU work three.c submits on command buffers of its own: a ReflectionProbe3D's or a ProceduralSky's capture (PMREM) and mip chains. three.c v0.187.5 tells the engine of each such command encoder, and the engine writes the pass's timestamps inside it. Before, that work was untimed on WebGPU, so a scene that captures a probe every few frames read far cheaper on WebGPU than on GL (the chromite game's High read 3.7 ms on WebGPU against 7.1 ms on GL).
- WebGPU pass timing reads back through two buffers, so every frame is timed. With one, the frame after each read back went untimed, and work that runs every other frame could fall only on untimed frames.
- A pass that ran in a frame and wrote no timestamp (a probe or sky pass between its captures) reads 0 for that frame on WebGPU, instead of holding the last frame it drew.

### Changed
- three.c v0.187.5 (its encoder hook, `t3_renderer_set_encoder_hook`).
- `tools/quality-range.mjs` takes a frame's cost as the passes' 16-frame averages summed. The pipeline's own total is the last frame's, which a capture or a changing sky every few frames made jump by whether the last frame had one.

### Moving a game to v0.36.1
- Nothing to change in code. On WebGPU a game with a reflection probe or a changing sky now reads its real GPU time: re-measure its quality range.

## v0.36.0 (2026-11-06)

### Changed
- **Very Low and Low draw the HDR scene in R11G11B10 float** where the device renders and blends it (GL: an R11F_G11F_B10F framebuffer that is complete; WebGPU: the rg11b10ufloat-renderable feature, which the native backend now asks for and a cart gets when its host's device has it). Half the bytes of RGBA16F a pixel, no alpha (nothing those presets draw after the scene reads it), less precision in the darks. On the 890M at 1080p it took the jetski game's scene pass from 1.46 to 1.05 ms and the arena shooter's from 3.73 to 2.84 ms at Very Low (GL). `CW_TEST_SMALL_HDR=0` (native) keeps RGBA16F.
- **Low runs the height fog and FXAA inside the tone mapping pass**, as Very Low did, with bloom's composite in the same pass (the glow is gathered from the scene before the fog, the composite adds it after). Bloom's first level on Low is made from four bilinear reads (a 4 x 4 texel footprint) instead of thirteen.
- The fused FXAA (Very Low and Low) averages its edge reads in a compressed space (each over one plus its exposed brightness, undone after the average) and tone maps once, instead of tone mapping four reads at an edge.
- **Low draws no volumetric fog and no distortion buffer.** A HeightFog with `volumetric` on draws as plain height fog on Low and Very Low (before, Very Low drew no fog at all for it), so the scene keeps its fog without the froxel grid's cost.
- ProceduralSky re-renders its cube and environment at most every 8 frames on Very Low, 4 on Low and 2 on Medium while its parameters keep changing (a sun or haze easing a little each frame); High and Ultra as before. A changing sky cost a racing game about 3 ms of GL time a frame at every preset.
- Docs: quality.h and the rendering guide's quality tables (HDR scene, sky re-render, fog, distortion, bloom), the Very Low and Low post section.

### Measured (GPU time on the Radeon 890M at 1080p, as a share of High)

| Game | Very Low / High, before -> after | Low / High, before -> after |
|---|---|---|
| Pirates | 0.22 / 0.27 -> 0.22 / 0.21 | 0.39 / 0.47 -> 0.38 / 0.34 |
| Jetski | 0.26 / 0.21 -> 0.18 / 0.24 | 0.42 / 0.51 -> 0.35 / 0.42 |
| Arena FPS (with its own depth prepass on Low and Very Low) | 0.48 / 0.42 -> 0.23 / 0.24 | 0.62 / 0.59 -> 0.31 / 0.32 |

(GL / WebGPU, before on v0.34.3. The example repositories carry every game. The engine samples in tests/quality-range-engine.json measured Very Low 0.20 to 0.64 and Low 0.31 to 0.89 of High, bound by terrain and open-ground pixels a preset does not cut; their shares are tightened to that.)

### Tests
- `quality-verylow-post` runs Low as well as Very Low (the fused fog and FXAA against the separate passes, FXAA at work), with a third control on Low: `CW_TEST_VERYLOW_POST=bloom` leaves the glow out of the fused pass and must break the bound.
- `quality-range`: the engine list's shares of High tightened to the new measurements. `tools/quality-range.mjs` takes a scene's `gpus` (the backends to measure it on), for a scene whose cost a backend's timers cannot see (a 2D game's app pass is untimed on WebGPU).

### Moving a game to v0.36.0
- Nothing to change in code. A game's Low looks closer to Very Low's post (no volumetric fog grid, no distortion), and a game whose fog is volumetric now has height fog on Very Low. A game that keeps its own ProceduralSky parameters easing every frame re-renders the sky less often below High. Re-measure the game's quality range: Low and Very Low cost less, so its shares of High can be tightened.

## v0.35.0 (2026-11-06)

### Added
- `cw_shell_hud_shown()` (cartwheel/shell.h): true only in play, false under the pause menu, the settings opened from it, the join screen opened during play, and on the title and loading screens. The shell draws its menus over the game's frame and dims it only lightly, so a HUD left on screen sits under the menu's lines, and at 150% and 200% text they overlap. A game draws its HUD only when this is true; the world and the views stay. Debug field `shell.hud`. The game shell guide shows it in the calls a game makes, and the shell_join sample draws a HUD line with it.

### Fixed
- The text scale cap (`CW_TEXT_SCALE_CAP`, 8% of the canvas height) is measured in canvas pixels, after the 2D transform and the layer camera's zoom. A game that draws on its own stage (a fixed-size virtual canvas scaled to the window with cw_r2d_set_transform) had the cap applied in the stage's units: on a 1080-unit stage in a 720-line window, text that asked for 6% of the canvas did not grow at all at 200%. It now grows to the cap like text drawn unscaled.
- The editor's text is at least 13 px on its 720-line window (the 18 px at 1080 lines floor): the inspector, scene tree, file system, import, preview and viewport panels drew captions, badges and buttons at 10 to 12 px, and the 2D view's ruler numbers and the curve editor's range label 10 px (the 2D rulers are 20 px deep for them, were 18). The interface font size setting goes down to 13 (was 10).

### Changed
- The gate option `textFloor` is on again by default for a game's editor builds (`<game>_editor_*`), as for the game: v0.34.1 turned it off while the inspector's text was under the floor.

### Tests
- `text-floor`: the text_size sample draws a banner on a 1920 x 1080 stage scaled to its 720-line window; at 200% it reaches the cap (sample.stage_share 0.08), where the cap in stage units left it at 0.06.
- New gate `shell-hud-pause`: the shell_join sample's HUD line is gone under the pause menu (`shell.hud` 0). Its control never pauses and keeps it.
- The editor gates (editor-view3d, editor-view2d, editor-gizmo, editor-pointer, editor-pointer-keys) run with the text floor on.

### Moving a game to v0.35.0
- A game on the shell: draw the HUD only when `cw_shell_hud_shown()` is true (where it drew it on `CW_SHELL_PLAY || CW_SHELL_PAUSE`). Nothing else changes.

## v0.34.3 (2026-11-06)

### Fixed
- WebGPU at Very Low under the water: the tonemap pass with the vignette, the grade or FXAA fused in and bloom off left its bloom texture binding holding whatever an earlier pass had bound there, the underwater view's depth, which does not fit that binding: each such frame logged a WebGPU error (a racing game's flooded track logged hundreds). The pass now binds an empty texture there when bloom is off.
- The graphics page (cartwheel/quality_menu.h) lays its two columns out from their measured widths: at 200% text in a wide font the Resolution row (`100%  1920x1080`) ran off the right edge. The pair is centred on the canvas and its text is drawn smaller only when even that does not fit. The debug field `quality_menu.overflow` says how far a row runs past an edge (0 when it fits).

### Tests
- New gate `post-verylow-underwater`: water_shore on WebGPU at Very Low, the camera under the water and the vignette on, with 0 WebGPU errors. Its control, `CW_TEST_BLOOM_UNIT=stale`, leaves the unit as it was and must log errors.
- New gate `quality-menu-wide`: the shell_join sample's graphics page at 200% in the monospaced font (`SHELL_JOIN_FONT=mono`) keeps every row on the canvas. Its control, `CW_TEST_QUALITY_MENU=centre`, splits the columns at the centre as before and runs past the edge.

## v0.34.2 (2026-11-06)

### Changed
- **Movement prompts draw the d-pad when the d-pad and a stick are both bound.** A pair or four directions whose actions are all on the d-pad draw the d-pad glyph with those arms solid, whether or not a stick is bound too and whatever the player last steered with: `{move_left|move_right}` lights left and right, `{move_up|move_down}` up and down, the four directions every arm. One movement action alone on both, such as `{lean_fwd}`, lights its one arm. Actions on a stick and not the d-pad (right-stick aiming, looking round) draw the stick as before. The old choice (the stick, unless the pad had only ever been used through its d-pad) is gone, and with it the input module's per-pad record of which was used.
- The prompts sample adds a line with a d-pad pair and a right-stick pair, and its stick ladder uses actions on the stick alone. docs/images/prompts.png is from the new sample.
- Docs: the input guide's Movement rule and prompt.h.

### Tests
- `unit-prompt` checks the d-pad for pairs, four directions and one direction bound to both (after the stick, the d-pad and the stick again were used), the stick for stick-only actions, and a face button first staying the face button.
- New gate `prompts-dpad`: the sample's glyph codes for a pad player, d-pad for the pairs bound to both, the right stick for look. Its control, `CW_TEST_PROMPT_BREAK=stick_first`, draws the stick when both are bound and fails. `prompts-glyphs` has the new codes.

### Moving a game to v0.34.2
- Nothing to change in code. A game that hand-draws a stick beside movement bound to the d-pad too should use the prompt markup instead, and a game whose prompt checks compare glyph codes or pictures gets the d-pad codes (5<<16 | arms) where it had the stick (4<<16 | arms<<4).

## v0.34.1 (2026-11-05)

### Fixed
- The gate option `textFloor` is no longer on by default for a game's editor builds (`<game>_editor_*`: the editor's inspector is a tool, not the game's text). CI's editor-pointer, editor-pointer-keys and editor-view2d gates failed on it in v0.34.0.

## v0.34.0 (2026-11-05)

### Added
- **One text scale for all 2D text.** `cw_text_scale_set` multiplies every cw_r2d_text, cw_font_measure, cw_font_advance and cw_font_caret_x call, the button prompts (cw_prompt_text, its size call and the glyphs) and every UI theme font: `cw_ui_set_text_scale` is now the same scale. Measuring scales as drawing does, so a layout made from cw_font_measure grows with no code. The steps a player picks are 100%, 125%, 150% and 200% (`cw_text_scale_cycle`, `_cycle_back`, `_label`). Text the scale would take past 8% of the canvas height (`CW_TEXT_SCALE_CAP`) stops there, a bitmap font scales by whole multiples of its pixel size, and text drawn into a render target is drawn as asked. `cw_text_drawn_size` answers the size a given size draws at now.
- **The size floor.** `CW_TEXT_FLOOR_BODY` (26 px at 1080 lines, 2.4% of the canvas height: HUD values, menus, hints, captions) and `CW_TEXT_FLOOR_MIN` (18 px at 1080 lines, 1.7%: the smallest text anywhere). A new `cw_text_style` field `fixed_size` marks art text (a title logo drawn as text, a word on a sign): drawn as asked, never scaled, not checked.
- **The check.** Every frame the engine records the smallest text drawn to the window, as a share of the canvas height (debug fields `text.min_share`, `text.min_where`, `text.run_min_share`, `text.run_min_where`, `text.scale`, `text.backdrop`). The debug request `text` lists every size the frame drew (with the caller's file and line in a debug build of the game); `text scale <k>` and `text backdrop <off|outline|panel>` set the settings. Render-only.
- **Gate option `textFloor`**, on by default for games (a games repository's apps, this repository's `games/`): a run that drew text under the smallest floor fails, naming the string. **`tools/check-text.mjs`** walks a game's screens at any text scale and host frame size and holds body text to the body floor, with a `secondary` list for the few smaller labels; `--shots` keeps frames for a contact sheet. `run-cart.mjs --end-cmds` runs debug requests after the last frame. check-text.mjs ships in the desktop SDKs (beside run-cart.mjs).
- **Text background**, the player's contrast setting for text over a 3D scene: Off, Outline or Panel (`cw_text_backdrop_set`, `_cycle`, `_label`). The engine adds a dark outline or a dark rounded panel to text whose style has neither an outline nor a shadow; `cw_text_style.own_backdrop` marks text on the game's own panel.
- **The game shell's Settings has Text size and Text background rows**, saved in the shell's save (a byte that was reserved: older saves load with the defaults). Games with their own menus keep `cw_text_scale_saved()` in their save and call `cw_text_scale_restore` at init.
- Sample `text_size` (a HUD laid out from measured text, an art title, prompts and a wrapped message), gate `text-floor` (check-text and the gate check pass the sample at 100% and 200% and fail its control, a line 12 px tall at 1080 lines; the HUD block measures twice as tall at 200%) and gate `text-size-scale` (200% with the panel background on every host). `unit-shell` checks the settings' steps and their save.

### Changed
- The game shell lays its screens out from the size its text draws at: at 200% nothing overlaps, a menu that no longer fits scrolls with its selection (a mark above or below says there is more), a hint line too wide for the canvas breaks into lines, and the controls list takes the menu's place when there is no room beside it. The settings rows are a little smaller (4.25% of the canvas height) to make room for the two new rows. The graphics page (cartwheel/quality_menu.h) scrolls the same way.
- Captions (CaptionPanel) are never smaller than the body floor (a little over it) times the player's text scale.
- The button prompts' minimum size is now given as a share of the canvas height (1.5%, 16 px at 1080 lines); a prompt line should have the body text size.
- The editor templates and the first 2D and first 3D tutorials size their hint line at 3% of the canvas height and place it by its measured height.
- Docs: localization and accessibility "Text size" (the floor, the scale, the background, the check), 2D rendering, the game shell, testing (the gate option and check-text), CONVENTIONS (the two numbers).

### Moving a game to v0.34.0
- **Gates now check text size.** Every gate of a games repository runs with `textFloor` on: a run that drew any text under 18 px at 1080 lines (1.7% of the canvas height) fails, naming the string. Make the text larger (body text 26 px at 1080 lines, 2.4%, or more), mark text that is art and not reading `.fixed_size = true` in its style, or (rarely) set `"textFloor": false` on one gate with a note why.
- **Write `tests/<game>-text.json`** (the screens: title, every menu, play, pause, results) and run `CW_PROJECT_ROOT=$PWD node <host sdk>/tools/check-text.mjs tests/<game>-text.json --scales 1,2` (with the GPU suite lock, or `CW_TEST_GPU=software`): body text must reach the body floor; the few smaller labels go in its `secondary` list.
- **Lay out for 200%.** Text, measuring and prompts now grow with the player's Text size. Place lines by measured height (`cw_font_measure`, the metrics `cw_r2d_text` returns, `cw_prompt_text_size`), size panels from measured widths, and where code spaces lines by a size, use `cw_text_drawn_size(font, size)`. Check every HUD, menu and results screen at 200% (`--start-cmds "text scale 2"`).
- **Games with their own settings** add Text size (`cw_text_scale_cycle` / `_cycle_back` / `_label`) and Text background (`cw_text_backdrop_cycle` / `_label`) rows, keep `cw_text_scale_saved()` (one byte) in their save and call `cw_text_scale_restore` at init. Shell games get both rows with no code.
- Text drawn on the game's own panel sets `.own_backdrop = true`, so the Text background setting does not add a second one.
- A game that called `cw_ui_set_text_scale` for its own reasons now scales its HUD text too (it is the one scale).
- The shell's settings page has two more rows (Text size, Text background before Controls): an input script that walked to Controls or Back by count needs two more downs.

## v0.33.1 (2026-11-05)

### Fixed
- Very Low in split screen: the shared post chain no longer takes the height fog into its tonemap pass (it has no one camera or depth for it, and crashed). The fog stays its own pass in each view there. Gate `quality-verylow-post` now runs fog_shafts_split too.

## v0.33.0 (2026-11-05)

### Added
- **The plain lighting model on Very Low.** On effects detail Very Low every MeshStandardMaterial and MeshPhysicalMaterial the lighting system marks carries a new shading part, `plain` (`cw_quality_scale("shading")` is 0 there, 1 on every other preset): Lambert diffuse and a Blinn-Phong highlight sized by the roughness; the environment and the probes light it with their diffuse light only (no reflection is sampled; the specular colour takes that light through an analytic estimate, so metals keep their tint); no clearcoat, sheen, iridescence or anisotropy. Colour, maps, normal map, emission, ambient occlusion and light maps are unchanged, and a material with transmission keeps the full model. Under it the light probe grid reads its first two SH bands (6 reads, not 14).
- **Very Low post in one pass.** On Very Low the height fog and FXAA run inside the tone mapping pass: the fog blended over the HDR value as it is read, and FXAA 3.11's console variant (four diagonal reads and the centre, two or four along an edge, no search), its edge test on a brightness estimated from the HDR value and its blend of tone-mapped reads. Two full-frame passes fewer.
- Gate `quality-verylow-post`: on Very Low the fog and FXAA are reported fused, the frame is the separate passes' but for FXAA's edges, and FXAA visibly smooths. Controls: `CW_TEST_VERYLOW_POST=fog` and `=aa` leave each step out and must fail.

### Changed
- Terrain on Very Low reads each layer once (no stochastic tiling, a third of the reads) and switches to its coarser levels from 0.6 of their distance (they still morph in, no cracks).
- Every sample, game and test program table regenerated with its Very Low states (`x<parts>_plain`, `xterrainmin`): cw_samples 103 to 158 states, terrain_demo 53 to 72, character_skin 18 to 26, gi_room 13 to 17, the editor 7 to 13. **A game's own tables need a survey at Very Low** (`node tools/programs.mjs <app> --presets verylow`, or every preset); a state missing there is a skipped draw on Very Low.
- `tests/quality-range-engine.json`: Very Low's share of High tightened to this release's measurements (fx_explosion 0.55 to 0.35, night_street 0.85 to 0.75, open_world 0.95 to 0.6, terrain_demo 0.97 to 0.6).

### Measured
GPU ms a frame on the Radeon 890M at 1920 x 1080 (GL / WebGPU, the pipeline's timers), Very Low in v0.32.0 against now, High unchanged:

| | Very Low before | Very Low now | High |
|---|---|---|---|
| Pirates (more-cartwheel-examples) | 1.51 / 1.51 | 1.37 / 1.44 | 5.52 / 6.16 |
| Jetski | 2.40 / 2.42 | 1.56 / 1.64 | 7.52 / 6.42 |
| Arena FPS | 4.81 / 4.92 | 3.81 / 3.80 | 9.81 / 9.50 |
| Sky Fighter | 3.26 / 3.01 | 2.21 / 2.03 | 6.84 / 5.31 |
| the co-op platformer | 1.99 / 2.03 | 1.56 / 1.51 | 4.20 / 4.39 |
| the fighter | 3.82 / 3.47 | 3.08 / 2.82 | 5.91 / 5.24 |
| open_world | 2.35 / 3.08 | 1.35 / 1.44 | 2.71 / 3.28 |
| terrain_demo | 2.42 / 3.41 | 1.50 / 1.64 | 2.95 / 4.06 |

Pirates and Jetski draw Very Low at a quarter of High or less. Scenes whose High is already cheap (the brawler, the horde shooter, platformer3d, around 2 ms or less) keep a floor of about a millisecond at 1080p: the scene's pixels at full resolution and one tone mapping pass.

## v0.32.0 (2026-11-05)

### Added
- Cart art: a cart can carry pictures of the game for launchers (an icon, a cover, a background, a logo, the title screen and labelled screenshots) with a description and the author, as wasmcart 0.38.0 defines them. `cw_add_app` takes `ART <dir>`, `DESCRIPTION` and `AUTHOR`, or reads a project's `art`, `description` and `author` keys from `project.cwproj` for the app the project names, and the cart build packs the pictures under `art/` in the `.wasc` with the manifest's `art`, `description` and `author`. A file that is not a PNG or JPEG stops the build; a picture of the wrong shape or size is a warning. docs/shipping.md, "Give the cart its art", says what to make; the templates carry the keys commented out.
- `tests/cart-art.mjs`: the project keys parse as `cw_add_app` reads them, the pack tool packs a fixture with every picture and its label, `samples/hello` carries the fixture's art and a sample without `ART` carries none, with a control (a cover of the wrong shape) that must be reported. CI runs it on the cart shares, and the cart SDK's check packs a cart with art using the SDK's own files.

### Changed
- wasmcart 0.38.0 (from 0.36.2). 0.37.0 gives WebGPU carts a core device where the GPU has one, on every host; 0.38.0 adds the cart art fields and `wasmcart-pack --art`, and hosts ignore manifest fields they do not know.

### Fixed
- The dual-cart check's control (`pipeline_test gl_msaa_on_webgpu`) passed on a core WebGPU device, which multisamples the scene, so the GL call it puts back never ran; it now takes the GL path on any device. `tests/programs-carts.mjs` expected two survey runs, where the survey now runs every graphics preset on both hosts.
- The cart SDK now ships `src/cartArt.js` beside `bin/wasmcart-pack.js`, which imports it from wasmcart 0.38.0 on: without it every cart build from the SDK would stop at the pack.

## v0.31.0 (2026-11-05)

### Added
- A **Very Low** graphics preset below Low (`CW_QUALITY_VERY_LOW`, first in `cw_quality`, so `q <= CW_QUALITY_LOW` reads "Low or cheaper"). It keeps Resolution at 100% and cuts what a lower resolution would not: no ambient occlusion, bloom, volumetric fog, distortion, SSR, depth of field or skin shading; FXAA in place of the scene's MSAA; the new **Minimal** shadows (the sun only, one cascade of 512 to 1024, hard edges, half the shadow distance, small casters culled), so the sun's shadow still grounds characters and props; a quarter of the GPU particles drawn, a fifth of the instancing density, three times the LOD error budget, a quarter of the clustered lights; and water on a grid half of Low's with a cheaper surface program (`waterlow`: one normal map octave, the floor's depth colour and the shore, crest and wake foam, without refraction, the ripples' slope, caustics, the foam's lace or shadows on the water).
- **Shadow caster culling**: Minimal, Low and Medium skip a mesh whose bounding sphere is small on screen (radius over its distance to the camera under 0.02, 0.012 and 0.006) in every shadow map of the frame. The debug fields `shadow.culled` and `shadow.casters` count them.
- `cw_quality_keep_msaa(true)`: a game whose look needs the scene's MSAA keeps it on every anti-aliasing value but Off.
- Named values for every setting (`CW_GFX_SHADOWS_MINIMAL` .. `CW_GFX_SHADOWS_ULTRA`, `CW_GFX_AA_OFF` .. `CW_GFX_AA_FULL`, `CW_GFX_SSAO_*`, `CW_GFX_RES_*`); the debug request `quality set` and scope caps take a value by name (`quality set shadows minimal`, `quality scope push ssao=off`).
- `tools/quality-range.mjs`: each app's GPU time per pass at every preset, natively on GL and WebGPU at 1080p on the test GPU, failing presets that do not order by cost.
- Gates `quality-unit`, `unit-quality`, `quality-sim-same`, `quality-grounding`, `quality-range`, each with a control that must fail.

### Changed
- **Resolution is independent of the presets.** Picking a preset or Auto never changes the player's Resolution, a preset with any resolution shows its own name (High at 75% is "High", not Custom), and the save keeps both. Auto lowers Resolution only in a game that calls `cw_quality_full_resolution(false)`, and then only below Very Low.
- Low and Medium no longer draw the scene's MSAA: the Anti-aliasing setting's values are Off, FXAA, SMAA and Full, and only Full (High, Ultra) keeps the PostProcess's MSAA. Medium now draws SMAA (it drew FXAA). Off and FXAA also draw without TAA and its velocity pass.
- Ultra holds finer levels of detail further (x0.75 the LOD error budget) and stretches the cascades' distance 1.5 times.
- **Auto** walks a ladder of single changes (High; High with shadows Medium; Medium; Medium without ambient occlusion; Low; Low with effects Very Low; Very Low) instead of whole presets, and on a big overshoot (the first second and a half at a rung over 1.6 times the budget) jumps to the rung expected to fit, so a weak device is at its level within a few seconds of play.
- **Source change for games with their own settings**: `cw_quality_lever_add` takes the value each preset picks as a `cw_quality_lever_presets` struct passed by value (`(cw_quality_lever_presets){ .very_low = 0, .low = 0, .medium = 1, .high = 1, .ultra = 1 }`), so a call written for four presets does not build instead of reading a shifted array. A game's own tables indexed by `cw_quality` need a Very Low entry, and an equality test against `CW_QUALITY_LOW` now misses Very Low.
- The save word (`cw_quality_saved`) takes 30 bits: keep four bytes. Every word an older engine wrote still restores the choice it held.
- `CW_GFX_SHADOWS` has a new lowest value, so every numeric shadow value moved up by one: use the names.
- `tools/programs.mjs` surveys every native app at all five presets (`--presets` narrows it), and every cart at each preset; gates (`--gates`) also run at Very Low.
- Every sample's program table regenerated for the new presets.

### Measured
GPU ms a frame on the Radeon 890M at 1920 x 1080 (GL / WebGPU, the pipeline's timers):

| | Low before | High | Very Low now |
|---|---|---|---|
| Pirates (more-cartwheel-examples) | 2.43 / 2.69 | 5.52 / 5.96 | 1.51 / 1.51 |
| Jetski | 3.62 / 4.52 | 7.54 / 6.36 | 2.40 / 2.42 |
| Arena FPS | 6.64 / 6.95 | 9.73 / 9.37 | 4.81 / 4.92 |
| Tux Racer | 2.82 / 2.69 | 4.71 / 3.74 | 2.32 / 2.29 |
| open_world | 2.82 / 3.41 | 2.80 / 3.54 | 2.35 / 3.08 |

Scenes bound by the pixels of their ground (open_world, terrain_demo) gain least: their cost is the terrain's shading, which no preset cuts at full resolution. Pirates' cart, through romdev on GL, takes 1.2 ms of CPU a frame on Very Low against 3.1 ms on High (its simulation 0.26 against 0.69 ms: the water's ripple grid has half the cells from Low down).

## v0.30.1 (2026-11-03)

### Fixed
- The stick prompt read as an eye or a target, not as a stick: a ring round a dot, with the arrows of a left and right pair closing it into an almond, and a press drawn as rings inside rings. A stick is now a thumbstick seen from the front and a little above (a flat socket, a short post, a wide cap whose top shows), with the other stick smaller and dimmer beside it (the left stick has its twin on its right). Arrows sit beside the cap for left and right, above it for up and under the socket for down; a press lowers the cap under an arrow pointing down onto it. The glyph's width is unchanged (1.42 times the size), so layouts do not move. Glyph codes are unchanged.
- The touch stick prompt had the same ring-and-dot look: it is now a knob on a filled base with the arrows inside the base. Its width is unchanged.
- A single-character keycap legend (J, W, 7) was the size of a word's and thin, so at the 16 pixel minimum a J in HUD text read as an I. One character is now drawn larger and heavier, centred on its capital height with a hanging letter (J, Q) lifted so its tail stays inside the keycap. Words ("Space", "Esc", "Left Shift") are unchanged, and so are keycap widths.
- `tools/check-prompts.mjs` missed a face-button letter in lower case in round brackets ("(a) resume", "(b): back"). It now reports one at the start of the text or after a space, a colon or a comma, and passes code and ordinary text such as `f(x)`, `bitcast<i32>(a)`, `item(s)`, `(x)%d`, `[x]` checkboxes and `[b]` tags; the fixtures test both.

### Added
- Sample `prompts`: the debug request `ladder` shows every stick glyph (a pair, four directions, one direction, each stick, a press, the touch stick) and the keycaps at 16, 20, 24, 32 and 48 canvas pixels on dark and light, then in lines of HUD text at 16 and 22.

## v0.30.0 (2026-11-03)

### Added
- Button prompts (`cartwheel/prompt.h`): a game names the action and the engine draws how to do it on the device each player is using right now. `cw_prompt_text(font, "{jump}: hop a gap", player, pos, size, color, align)` draws a diamond of four dots with the south one solid for a pad player, a keycap reading "Space" for a keyboard player and the on-screen button's disc for a touch player; rebind jump to north and the north dot lights on that frame. The markup: `{action}`, a pair `{move_left|move_right}` or four directions drawn as one stick or d-pad glyph, `{pad:west}` for one physical button (designs laid out by position), `{{` and `}}` for braces. It sits inside translated strings, so a translator moves the glyph, and right-to-left lines lay glyphs out in reading order. `cw_prompt_action`, `cw_prompt_actions`, `cw_prompt_binding` draw single glyphs, each with a `_size` call; `cw_prompt_device_of(player)` says which device a player used last (pad, keyboard, mouse or touch), from the frame snapshot, so no game tracks it itself. Player -1 is a screen every seat reads. Glyphs are vector shapes on a dark backing, readable on light and dark backgrounds, with no maker's letters or colours; Start is the word "Start". Minimum size 16 canvas pixels at a pixel scale of 1.
- Rules, each with a test: the first binding on the player's device in binding order (the keyboard and mouse stand in for each other); an action with nothing on the device draws nothing and logs once; a `{name}` that is no action logs once and counts in the debug field `prompt.errors`, which `tests/gate.mjs` now fails any gate on; a pad used only through its d-pad shows the d-pad where the stick would be shown (a pad with no stick); prompts never reach the simulation, recordings or `input.hash`, and a call from `fixed_update` asserts in a debug build and counts in `prompt.errors` in a release build. Tests force the device with `CW_TEST_PROMPT_DEVICE` natively or the debug request `prompt device <player|all> <device|auto>` in a cart.
- `tools/check-prompts.mjs` fails on text that names a controller button by one maker's label or colour (`A:`, `(A)`, `press A`, `A or`, `A B X Y`, `LB`, `RT`, `L1`, `ZR`, `Cross`, `green button`) in C strings, scene files and catalogs; `prompt-ok: <reason>` marks a line that means it. It runs in `tests/all.mjs` over the engine and ships in the SDK, so a game runs `node <sdk>/tools/check-prompts.mjs src`.
- `cwpack --cwtr-check` checks every `{...}` in a catalog is well formed and, with `--po-source`, that a translation keeps the source's placeholders and prompt markup.
- Sample `prompts`; gates `unit-prompt`, `prompts-glyphs`, `prompts-device`, `prompts-device-switch`, `prompts-device-keys`, `prompts-device-env`, `prompts-rebind`, `prompts-text`, each with a control that must fail.
- The docs can show a picture rendered by a sample (`@image`); the input guide's "Showing controls" has one of every glyph.

### Changed
- The game shell draws its hints as button prompts: the join screen's slots and footer, and a line of controls under the title, pause, settings and graphics pages. It has a new action, `ui_start` (Start on a pad, Enter on the keyboard), which the join screen's prompt names; it adds a slot to every shell game's input snapshot, so `input.hash` values change. On a touch screen the hint lines are left out.
- `RemapButton` shows the binding as a glyph beside its name.
- `games/platformer3d`, `games/action2d`, the editor templates, the first 2D and 3D tutorials and the touch example in `input.h` name actions or positions, never a maker's buttons.

## v0.29.1 (2026-11-01)

### Fixed
- Terrain3D: a height-map normal pointing away from the eye caught specular light at a grazing angle and drew bright shards on steep walls. Such normals are now brought back to face the eye. Games pick this up when they regenerate their program tables.
- A paused scene moved: its tick was frozen, but the host's interpolation alpha kept cycling, so Water3D's waves and every node that interpolates between its last two ticks shifted from frame to frame for as long as the pause lasted. A paused tree now draws at its frozen tick. Pausing or resuming moves the picture by no more than one tick. New gate `water-pause`.
- Water3D's refraction bend could cross a shoreline into terrain clipped out of the refraction target and show dark slivers of its clear colour. Over a baked floor map the bend fades out from 3 m of water to 0.5 m, using the smaller of the depth at the rest plane and at the moving surface; a floor probe that hit nothing counts as open water. Water with no floor map is unchanged. The water's program changed, so a project's program tables need regenerating (`node tools/programs.mjs`).
- WebGPU: rendering twice on one embedder's encoder, detaching it between the renders, could overwrite instance and vertex data the first render's commands still had to read, since detaching an encoder does not submit it. three.c now keeps those buffers until the embedder says it submitted. On a game whose refraction draws instanced props this was visible: instance data from a later pass drew them through the water on WebGPU only.

## v0.29.0 (2026-10-31)

### Added
- Terrain3D `side_detile` (0 to 1, off by default): cliffs stop showing the rock layer's repeat as a grid. Each cell of a side projection reads the layer at a larger scale and its own offset, with the original scale as fine grain, joined along the rock's own cracks, and the rock takes colour over tens of metres and strata that follow height. Seven texture reads for two on a steep face where it is on. Terrain programs changed, so a project's program tables need regenerating (`node tools/programs.mjs`).

## v0.28.6 (2026-10-31)

### Fixed
- Water3D: the view from under the water and its caustics ignored the wave mask: in a bay a camera above the held-down swell could be judged under the waves of the open sea, and a teal sheet covered the screen. They now read the mask as the surface does.

## v0.28.5 (2026-10-31)

### Fixed
- Water3D draws as it did before v0.26.0 with none of the new options set. v0.26.0 said every new water option was off by default, but its open-sea change also moved where all water reads its floor map and ripple grid: at the rest position of a surface point, where the waves have not yet carried it, in place of where it is drawn. So a wake's foam, a splash and a shore line sat up to half a metre off, wobbled with the waves and read softer and creamier (1.7% to 2.1% of the pixels in Party Pack's raft game moved by more than 16 levels; none of the new options caused it). They are back where they were made, and the depth in the picture agrees with `cw_water_floor_depth_at` again. Water that used to turn black on a crest stays fixed: that is the only remaining difference from v0.25.4. A game tuned since v0.26.0 will see its wakes and foam change. New gate `water-default-look`: against a frame stored from v0.25.4, it fails before this release at 3.8%, with a control. The water's program changed, so a project's program tables need regenerating (`node tools/programs.mjs`).

## v0.28.4 (2026-10-31)

### Fixed
- `tools/programs.mjs --gates` could report "no missing states" when every run had failed. A failed run now stops the survey with exit 1, naming the reason, and no table changes; a failed control variant is only reported, since a control may be meant to break its run.
- `tools/programs.mjs --gates` from an SDK now works on a project: the SDK ships `tools/run-cart.mjs`, `--build <project>/build/native` names the project, gates, inputs and assets come from it, `--gates-file` (or `CW_GATES_FILE`) reads a project's gate files as `tests/gate.mjs` does, and WebGPU runs where the SDK has it.
- `tools/lib/wcnative.mjs` looks for wasmcart-native beside `CW_PROJECT_ROOT` too, so an engine in a project's `deps/` finds `wasmcart-run`.
- Image gates of a game on Auto could fail on Chromium and wasmcart-native. Those test hosts keep a real clock and are not deterministic runs, so Auto stepped by how fast the machine happened to run and the picture stopped matching the other hosts. `tools/run-cart.mjs` now sends a debug request on them, `test_run` (`quality lock` with `--warmup-spread`), that locks quality as a deterministic run does: Auto off, High, no saved choice, explicit preset requests still apply. A game no longer needs to pin its gates. A cart built on an older engine does not know the request, so a harness from this release runs it with a spread warm-up: rebuild carts with the SDK they are tested with.

## v0.28.3 (2026-10-31)

### Fixed
- The warm-up built only the programs of what the camera could see, and a scene whose camera later turned compiled the rest in play, a stall in the player's hands. The shader phase now draws with culling off. A game's loading bar may run a little longer, and its first turn no longer stalls.

## v0.28.2 (2026-10-31)

### Fixed
- Auto keeps the level it reached when Auto is the game's own default, as `quality.h` said. `cw_quality_saved` returned 0 unless the player had picked Auto, so a game starting on Auto began every run at High and a weak machine sat through a slow opening each time. It now returns the level once Auto has stepped down. The shell's save writes it by itself; a game that keeps the word itself writes it from `cw_quality_on_change`.
- A saved Auto level no longer lasts for good. A run timed by frame time (every cart) cannot tell how far under budget it is, so it never stepped back up, and one bad session stayed in the save. A run now starts one level above its saved level; if that holds a couple of seconds of play on budget it is the new saved level, and if not Auto steps back down and the save is unchanged. A machine that really cannot hold it pays those couple of seconds at the start of each run's play.
- `cw_quality_auto(false, ...)` turns Auto off. It set High and left Auto running.

## v0.28.1 (2026-10-30)

### Fixed
- Water3D `crest_light` hardly showed on clear water: it only tinted the body, which the refraction covers. It now lights the top of a wave over whatever the refraction shows and foams its very top, so a swell seen from a low camera reads as a crest. Only water that sets `crest_light` changes. The water's program changed, so a project's program tables need regenerating (`node tools/programs.mjs`).

## v0.28.0 (2026-10-30)

### Fixed
- The join screen answers Start. A pad with no seat pressing Start got no response at all; it now takes the first free seat and the match starts, or with every seat taken the seated players play, and on a join screen opened during a match it takes a free seat and play resumes. Start on the title accepts the highlighted item, so Start, Start from the title is playing. Enter plays from a keyboard in any seat, not only the first.
- A press on the join screen could be lost or seen twice. It read a frame's raw presses from the fixed tick, so on a display faster than the tick rate a press on a frame with no tick did nothing (58% of presses at 144 Hz, 50% at 120 Hz, 20% at 75 Hz), and a frame of two ticks saw one press twice (at 30 Hz, Enter on the title also seated the keyboard, and the next Enter started the match). Each press now counts once, on one tick. Taps on menu items had the same fault.
- A press to play is answered on its own frame: the select sound and "Starting..." on screen. The game's `start` hook runs after that picture is drawn, one frame later than before, so a long `start` no longer leaves a dead join screen. `cw_shell_start` still calls it at once. New debug field `shell.starting`. Sample `shell_join_slow`, gates `shell-start-title`, `shell-start-answer`, `shell-start-unseated`, `shell-start-second`, `shell-start-full`, `shell-start-midjoin`, `shell-start-enter`, `shell-start-144hz`, `shell-start-30hz`: all fail before this release, each with a control.
- Water3D: wave crests standing above the eye line could turn pure black: where the refraction depth lay in front of the surface, the caustic fade overflowed to infinity and multiplied by zero, and the resulting NaN reached the picture. The water's program changed, so a project's program tables need regenerating (`node tools/programs.mjs`).

### Added
- Water3D `crest_light`: how much lighter and greener the thin water at a wave's top is than the body under it (0, the default: as before).

## v0.27.0 (2026-10-30)

### Changed
- Full resolution is the default: the Low and Medium presets and Auto no longer lower render resolution. Auto's ladder
  is High, Medium, Low, each at 100%, so it gives up detail, never pixels. Low and Medium cost more than before on a weak
  GPU. The player can still pick a resolution on the graphics page, and a game that wants the old behaviour calls
  `cw_quality_full_resolution(false)` in init. A saved Auto level is read on the shorter ladder (a level past its end
  becomes Low).

## v0.26.1 (2026-10-30)

### Fixed
- Auto graphics never stepped down on a host that delivers a fixed time step and has no GPU timestamps (a cart on the
  libretro core, which passes 1000/60 ms every frame for its audio): it measured frame time on that clock, so every frame
  looked on time. It now reads the real clock.

## v0.26.0 (2026-10-30)

Water for an open sea: as large as you like, sheltered where you say, sharp under a chase camera. Every option is off by default. The water's program changed, so a project's program tables need regenerating (`node tools/programs.mjs --projects <dir>`).

Correction: the options are off by default, but this release also changed how water without them looks, by accident: see v0.28.5.

### Added
- Water3D `grid = VIEW`: the surface is drawn as rings around the camera out to `horizon_distance` (`subdivisions` quads around each ring, at least `grid_min_cell` metres between rings), so quads are about 30 cm under a chase camera on any size of sea, for about 130,000 triangles. Each wave fades out between five and eight of its wavelengths from the camera, on the drawn surface only.
- `cw_water_set_wave_mask` and Water3D `wave_shelter`: a map of how free the waves are, and how much it holds each wave down, so a swell is full height at sea and gone in a harbour while the chop carries on. The drawn surface, `cw_water_height_at`, `cw_water_sample_at` and Buoyancy3D read the same map. Sample `water_open_sea`, gate `water-open-sea-surface`: within 0.44 cm of the CPU on a software GL run over the transition, 48 to 97 cm for the height with the mask ignored (the control).
- `cw_water_ripple_follow`: a small, sharp ripple grid that follows a point by whole cells, the same bits on every host. Gate `water-open-sea`.
- `cw_water_refraction_hide`: leave a floating hull out of the refraction, where it drew a ghost on the water behind it.
- Water3D for water seen from close by: `foam_scale`, `foam_lace`, `foam_soft` (foam by thickness, thinning through soft patches with no cut-out edge), `foam_spread` (a wake's foam widens as it fades) and `swell_reflect` (a swell's tilt moves the reflection). See [Water](docs/guides/water.md).

## v0.25.4 (2026-10-30)

### Fixed
- The SDK workflow's link check stopped on Android, whose SDK ships no cwpack, so the v0.25.3 tag published no SDKs.
  This is the release with the macOS fix below: use v0.25.4, not v0.25.3.

## v0.25.3 (2026-10-30)

three.c v0.187.3.

### Fixed
- macOS: no game could link against the macOS SDK of v0.25.0, v0.25.1 or v0.25.2 (`Undefined symbols:
  _eglGetProcAddress`, from three.c). Move a macOS game to this release. Reversed depth on macOS is WebGPU's, as
  on Windows.

### Added
- The SDK workflow links a game against each native SDK (`scripts/sdk-link-check.sh`) before it publishes it.

## v0.25.2 (2026-10-30)

### Fixed
- `tools/run-cart.mjs --host chromium` from a games repository's copy of the engine source, which has no `deps/`,
  stopped on a missing-module error. It now names the command that fetches its host:
  `scripts/fetch-deps.sh --only wasmcart-wgpu` in that copy. `docs/guides/testing.md` says so too.

## v0.25.1 (2026-10-30)

### Fixed
- Baked meshes skipped their draws in a project on a table made before v0.25.0: lightmaps read through the
  second UV set, a different program. `tools/programs.mjs` now adds each `lmap` state's `c1lmap` state when it
  regenerates a table, so `--table` is enough. Regenerate every table with lightmaps.
- `tools/programs.mjs` in a prebuilt SDK no longer exits 1 after writing a table: the SDK carries no webgpu-node
  for the WGSL check, so it warns and goes on (`WEBGPU_NODE=<dir>` still runs the check). An engine checkout still
  requires it.
- `tools/programs.mjs --gates` reads the project's `tests/gates.json` (`CW_PROJECT_ROOT`), not the tool's.
- `tests/export.mjs` shuts down an Android emulator it started (it left one running, 3 GB, after every run).

### Added
- A program manifest can name a project's own extension modules: a line `# extend: <path>`, relative to the manifest.
  `ninja programs` and a bare `--table` then need no `--extend`.

## v0.25.0 (2026-10-30)

three.c v0.187.2: three.js r187. Every program table regenerated: a project's tables need
regenerating too (`node tools/programs.mjs --table <manifest>` for each, or `--projects <dir>` for a project
built with cwproject; an engine older than v0.25.1 misses lightmap states).

### Added
- A reversed depth buffer on WebGPU: `cw_app_desc.reversed_depth`. Depth runs from 1 at the near plane
  to 0 at the far plane in a 32-bit float buffer, so precision is spread evenly out to the horizon: far
  surfaces stop flickering through each other and the far plane can sit much farther away. The same
  picture otherwise (gate `reversed-depth`: 41 samples against forward depth; `samples/far_view` shows what
  it is for). Shadow maps stay forward and no program table grows. A GL run stays on forward depth: a
  cart's GL has no way to change it. A pass with its own WGSL against the scene's depth checks
  `cw_gpu_reversed_depth()`; its WGSL has `CW_REV` and `CW_FAR` declared.
- Mesh3D `diffuse_roughness`: rough diffuse (EON). Dusty, chalky surfaces that look soft and flat at grazing
  angles instead of plastic.
- glTF materials honour `texCoord` and KHR_texture_transform's: a map on the second UV set reads it.

### Changed
- Reflection probes and the sky's environment are r187's PMREM, a cube render target with a mip chain. They
  look better, and they cost more: r187's smallest is 256 pixels a face, with GGX filtering at 256 samples a
  texel on the sharp levels. On a Radeon 890M at 1280 x 720, `gi_room` at High goes from 1.4 to 2.0 ms for the
  gi pass and from 4.2 to 5.0 ms for the whole frame.
- r187's own shading changes arrive with the tables: specular intensity in direct light, the ACES Filmic fit,
  point light shadow seams, PCF radius on non-square shadow maps.
- PostProcess `depth_prepass` runs on WebGPU, and the viewmodel pass resolves the scene colour once
  (`cw.msaa_resolves`): the engine was ready and three.c now has both.

### Fixed
- Lightmaps read through their own second UV set: a bake's generated charts line up on any mesh, on GL and
  WebGPU. They lined up only where they happened to match uv0, which drew blocky patches on most meshes.
  `lightmap_uv = UV0` still lays a lightmap on a mesh's uv0.
- `samples/gi_probe_check` sets its walls' Node3D layers (Node3D has written them over since the viewmodel
  layers, and the capture saw no walls); `unit_scene` calls `cw__mesh3d_set_lightmap` with its real signature (a
  wasm trap); `fx_boom_water` reports the peak of the particles alive (it read them after the burst burnt out).
- Tests: `unit_terrain`'s scatter check waits for the job pool, `ik-determinism` runs on the hosts its gates have.

## v0.24.3 (2026-10-30)

### Fixed
- Terrain on steep ground at Medium and up: each triplanar side projection's detail normal is turned into its own
  plane (it was read as if it lay flat, tipping a cliff toward and away from the light once a repeat: bands down every
  cut bank), and the sides read their layer plainly (their stochastic cells showed as facets). High shades as Medium
  does, the two strongest layers weighted from the four control texels and height-blended once: blending each texel
  apart and mixing the four drew diamonds where layers met. Program tables regenerated (terrain_demo, open_world).

## v0.24.2 (2026-10-30)

### Fixed
- Terrain shading cost on steep ground: triplanar read both side projections and the top-down one for every layer of
  every control texel, 18 texture reads a layer, and a screen of canyon walls at High took 38 ms of scene time on the
  Radeon 890M (46 ms a frame). Now only the side projection a slope faces is read (both only across the band where they
  hand over), the top-down one is skipped where the sides carry it all, and a texel's second layer only where it has a
  share: 8 ms at High, 6 at Medium, for the same transitions. Program tables regenerated (terrain_demo, open_world).

## v0.24.1 (2026-10-29)

### Fixed
- Terrain scatter: a part with fewer LODs than part 0 keeps drawing its coarsest LOD instead of vanishing. A glTF tree's
  leaf cards rarely simplify (cwpack gives them one level where the trunk gets three), so every scattered tree lost its
  crown past the first `lod_distance` and stood as a bare trunk. unit-terrain `scatter_part_lods` (fails without the fix).

## v0.24.0 (2026-10-29)

Smoke that thins at the lens.

### Added
- GPUParticles3D `camera_fade`: particles fade out as they come within this distance of the camera (smoothly, to nothing at
  the lens), on GL and WebGPU and with every blend mode. A blast on the kart or player a chase camera follows puts its smoke
  and flash a few metres from the lens, where one billboard covered the whole view; set it to about the camera's distance
  from its target. Gate `fx-camera-fade`; [Explosions](docs/guides/explosions.md#smoke-near-the-camera).

## v0.23.1 (2026-10-29)

### Fixed
- The touch overlay (`CW_TOUCH_OVERLAY_AUTO`) now hides when a pad's stick or trigger is pushed past half way, not only on a
  pad button: a phone player who touched the screen once and then steered with a controller's stick alone kept the
  on-screen controls. The next touch brings them back, as before.

## v0.23.0 (2026-10-29)

One particle system for a pool of one-shot effects, and light probes for what moves.

### Added
- Particle burst slots: GPUParticles3D `bursts` (1 to 32) splits a system into slots that `cw_particles_burst(node, at, dir)`
  fires one at a time, each a one-shot of the system's settings at its own place and direction: a pool of one-shot
  effects (hits, muzzle flashes, splashes) is one simulation and one draw instead of a system each. A 2000-enemy
  late wave: 96 active particle systems to 50, the particles pass 0.73 to 0.42 ms of GPU time. Sample fx_bursts,
  gate `fx-bursts`.
- `cw_lightmap_probe_irradiance`: the bake's SH probe grid read on the CPU (trilinear, the cosine lobe applied), a
  light probe per moving object; lights in the bake only (realtime off) add their direct light to the probes.
- `cw_splats_set_texture`: a splat set's own falloff texture (a damp sheen that breaks into wet patches).

### Changed
- WebGPU particles: every system's simulation, step and draw uniforms go into one buffer per particles pass, read
  through dynamic offsets and written once (each system made three queue writes a frame, a host call each in a
  cart). The same pixels.
- Splat sets reuse the sines of a yaw equal to the last splat's (a crowd's blob shadows all point away from one sun).

## v0.22.0 (2026-10-29)

Lighting for weak GPUs (baked direct light and reflections, blob shadows and light pools, full-resolution quality), fewer GL calls and faster GL particles, and geometry the engine builds no longer leaks its vertex buffers.

### Added
Lighting for weak GPUs, all opt in (nothing changes for a scene that does not ask):
- Baked direct light: a Light3D with `bake_direct` puts its direct light, shadowed by the static geometry, into
  the lightmaps; LightmapGI `shadow_softness` and `direct_samples` make those shadows soft.
- Bake-only lights: Light3D `realtime` off keeps a light in the bake only; it lights nothing at run time and
  costs nothing a frame.
- LightmapGI `shading` BAKED: lightmapped meshes draw unlit from their lightmap (a Basic copy of their material:
  colour, map, vertex colours, AO map, transparency), so no real-time light reaches them and a pixel costs a few
  texture reads however many lights the scene has. `cw_lightmap_apply_shaded`.
- Instanced scenery in the bake: an instanced mesh under a STATIC MeshInstance3D (`cw_mesh3d_set_procedural`)
  occludes and bounces light (it gets no lightmap of its own).
- MeshInstance3D `lightmap_uv` UV0: the lightmap is laid out on the mesh's own uv0 (a ground plane, a terrain).
- A reflection cube traced through the bake (LightmapGI `reflection_size`, `reflection_origin`;
  `cw_lightmap_reflection`, `cw_lightmap_use_reflection`): wet patches reflect the lit scene for one texture read.
- `cw_lightmap_irradiance_below`: the baked light on the ground under a point, a light probe for what moves
  (a crowd walking through floodlight pools and wall shadows), and `cw_lightmap_gi_data`.
- Splats (cartwheel/splats.h): blob shadows and light pools, one instanced draw of ground discs each, the stand-ins
  for dynamic shadows and lights on baked ground; `cw_splats_add_lights` puts a pool under every lit, unbaked
  OmniLight3D.
- PostProcess `fuse_distortion`: heat haze and shockwaves bend the frame inside the tonemap pass instead of a
  full-frame pass of their own (bloom then glows from the unbent frame).
- `cw_quality_full_resolution`: presets keep Resolution at 100% and Auto's ladder drops its resolution steps.
- `cw_geometry_optimize_vertex_cache`: meshoptimizer's vertex cache order for an indexed geometry.
- Sample lightmap_yard and gate `lightmap-yard`.
### Changed
- GL particles: every system's simulation runs first, then every draw in the systems' order, as on WebGPU (a draw
  right after its own transform feedback waited for it). The same pixels; a 2000-enemy late wave at 1080p on the
  890M: the particles pass 1.07 to 0.75 ms of GPU time.
- Clustered lighting skips the froxel slices no light reaches (the same records, less work a frame).
- The .cwlm format is version 3 (the reflection cube); version 2 files still load.

### Fixed
- The lightmap header said materials read the baked SH probe grid; none does yet (the grid is baked and stored).
  It also says now that three.c reads a lightmap through uv0 (see `lightmap_uv`).

### Fixed
- Water3D, terrain chunks and grass tufts, cloth and rope geometry and
  the colours of a cloth node kept a second reference on every vertex
  attribute and index they made (`t3_geometry_set_attribute` and
  `t3_geometry_set_index` retain what they are given), so each rebuilt
  geometry leaked all its buffers. A game that reloads a scene with water
  every round grew by about 3 MB a round (14 MB in 18000 frames of a
  water mini-game); with the fix its memory stays flat.

### Tests
- `cloth.geometry_refs` (unit_cloth): the attributes and index of the
  geometry `cw_cloth_geometry_new` and `cw_rope_geometry_new` return hold
  one reference each. Before the fix it failed (two each).

### Changed
- GL: the engine's own passes and particle systems no longer send a uniform value the program already holds
  (samplers and constants were set again every frame), and a particle system's draw keeps one vertex array
  per state buffer instead of setting its attributes before every draw. In a cart every GL call crosses into
  the host: on a late wave of a 2000-enemy horde game that is about 4,600 fewer uniform calls and 1,000 fewer
  vertex-attribute calls a frame (Chromium GL: 14,710 calls a frame to 10,137 with the first, about 8,850 with both).
  The pixels are the same: the frames of every fx and post sample, GL and WebGPU, compared byte for byte at
  frames 90, 300 and 600, and the 45 fx and post gates.

## v0.21.1 (2026-10-28)

Water made under the camera draws on WebGPU, and a skipped draw names its real cause.

### Fixed
- A Water3D made while the camera is under its surface (a level that
  builds its pools around the player) draws on WebGPU. Its first frame
  took the under-the-water branch, which makes none of the WebGPU
  refraction colour and depth textures the surface samples, so three.c
  skipped every draw of it (`waterRefractionDepth` unset) until the
  camera came up, and for good with `refraction` off. The surface now
  gets cleared 1 x 1 stand-ins until a refraction is drawn.
- A draw skipped because a material extension left a texture unset no
  longer logs that the app's program table lacks a state: the error
  names the cause three.c gave (a state missing from the table, an
  extension that did not bind a texture, or another reason, which the
  warning before it spells out).
- `cw_touch_set_round_button` and `cw_touch_set_overlay` have their API
  comments (v0.21.0 shipped them without, so its API check failed).

### Tests
- `water-under-new` (GL and WebGPU): a second Water3D spawned under the
  beach scene's surface with refraction off must draw with no draw
  skipped. Before the fix its WebGPU leg failed (147 skipped draws on
  lavapipe). Control: without the second pool the frame must change.

## v0.21.0 (2026-10-28)

Touch controls that stay where they are drawn: a thumbstick with a fixed home
in the bottom left corner, round buttons, sizes that follow the screen, and an
overlay the engine draws; the older floating stick keeps a home too, so every
game that used one stops jumping without a code change.

### Added
- Thumbsticks and round buttons (`cw_touch_add_thumbstick`,
  `cw_touch_add_round_button`, descs `cw_touch_stick_desc` and
  `cw_touch_button_desc`): placed in touch units (1/540 of the shorter side
  of the touch area, `cw_touch_unit`) from a corner (bottom left for a
  stick, bottom right for a button by default), laid out every frame so they
  follow a resize or a rotation. A thumbstick's ring stays at its home: a
  touch landing anywhere in its zone (default 2.4 radii past the home) steers
  relative to the home. `floating` in the desc keeps the older feel. In an
  overlap a button wins over a stick's zone, the nearer centre between
  buttons; a button takes touches out to 1.25 times its radius.
- The touch overlay: the engine draws its thumbsticks (base, ring, knob) and
  round buttons (disc, label) over the frame, translucent, in window
  coordinates (`cw_touch_set_overlay`: AUTO shows it when
  `cw_platform_touch_primary` or once a touch lands and hides it on a pad
  press; ALWAYS; OFF), `hidden` per control for a game that draws its own,
  `cw_touch_overlay_visible`, debug field `cw.touch_overlay`.
- `cw_touch_set_thumbstick`, `cw_touch_set_round_button`,
  `cw_touch_set_radius`, `cw_touch_set_enabled`, `cw_touch_set_area` (a
  safe area or a split screen's rect), `cw_touch_home`, `cw_touch_radius`,
  `cw_touch_set_floating`.

### Changed
- A rect stick added with `floating` (`cw_touch_add_stick(..., true)`) keeps
  its ring at a home two radii in from its rect's bottom left corner (the
  middle of a smaller rect) and steers from it, instead of centring the ring
  where the thumb landed: every example game's stick jumped across half the
  screen. `cw_touch_set_floating(id, true)` brings the older feel back.
  Unit tests `thumbstick` and `rect_stick_home` (controls: a floating stick
  follows the thumb). The input demo uses a thumbstick and a round button.

## v0.20.1 (2026-10-28)

The tag v0.20.0 was made at the wrong commit by mistake and has no release:
do not use it. v0.20.1 is the release described here.

WebGPU antialiases HDR scenes like GL (core WebGPU wherever the GPU has
it), every app builds as the dual GL + WebGPU cart (no app can be GL only
any more), pixel reads and 3D-into-2D targets on both APIs, impostors on
WebGPU, clustered lighting's scene work once a frame instead of once a
split-screen view, and three.c v0.186.22.

### Added
- Core WebGPU where the adapter has it: natively the engine asks for a
  core adapter first and falls back to compatibility mode
  (`CW_WGPU_FEATURE_LEVEL=compat|core` forces one); a cart reads the
  level of the device its host gave it. `cw_gpu_adapter_info()->core`
  (new field `core`) and the debug field `host.gpu.core` say which a run
  got. The test harness's cart hosts (romdev, Chromium, wasmcart-native)
  ask for core too.
- MSAA on HDR scenes on WebGPU: on a core device the scene pass
  multisamples any scene, RGBA16F included, 4x (WebGPU's only count, for
  `msaa` 2 to 8), and the multisampled depth's first sample is resolved
  into the scene depth when the opaque phase ends, as GL's resolving blit
  does, so sky and effects passes, ambient occlusion and decals see the
  same depth. A compatibility-mode device keeps the old rule (RGBA8 scenes
  without depth readers only, a warning otherwise). Sample `msaa_hdr` and
  gate `msaa-hdr` (GL vs WebGPU 0.016% of pixels on the 890M; controls:
  single-sampled WebGPU, the depth resolve left out, a compatibility-mode
  device). Cost on the 890M at 1920 x 1080: 0.3 to 0.6 ms of GPU time on
  WebGPU at Low and Medium, 0.7 to 1.5 ms at High and Ultra (GL 0.25 to
  0.8 ms).
- Pixel reads on GL and WebGPU without GL calls (`cartwheel/gpu.h`):
  `cw_gpu_read_window` (a region of the picture the frame puts in the
  window, taken at the end of the frame), `cw_gpu_read_window_now` (what
  the window holds so far), `cw_gpu_read_rt` (a 3D pipeline target),
  `cw_gpu_read_result` (the top-down RGBA8 pixels once they land: GL from
  the next frame on, WebGPU a frame or more later), `cw_gpu_read_wait`
  (waits natively), `cw_gpu_read_drop`, and `cw_gpu_finish` (waits for the
  GPU natively on both APIs and in a cart on GL; a WebGPU cart cannot
  wait).
- 3D into 2D render targets on GL and WebGPU: `cw_target_rt` (a 2D target
  as a 3D pipeline output, for `cw_pipeline_set_output`) and
  `cw_render3d_texture_target` (an app's own three.c renderer draws into a
  2D target, which the 2D renderer then draws the right way up).
  `cw_target_fbo` stays GL only.
- `cw_anim_from_text`: one `[animation]` section, as a scene file writes
  it, into a stored animation (the inverse of `cw_anim_to_text`).
- Editor animation module: the commands `anim.new`, `anim.track_add`,
  `anim.track_set`, `anim.key_add` (undoable) and `anim.text`, which
  `tools/scene-to-ed.mjs` writes.
- Impostors on WebGPU: the albedo and normal atlases are baked on WebGPU
  (the same texels as GL, mips box filtered on the GPU) and impostors draw
  there as on GL; the mesh fading out under its impostor draws after the
  impostors on both. Measured on the 890M: a bake takes about 1 ms on GL
  and 2 to 3 ms on WebGPU; drawing 49 impostors 0.16 to 0.37 ms on GL and
  0.11 to 0.21 ms on WebGPU, Low to Ultra.

### Changed
- No app can opt out of the dual cart: the `GL_ONLY` option of
  `cw_add_app` is gone (unknown arguments are refused), a cart tree
  configured with `CW_WGPU=OFF` stops at configure,
  `tools/check-imports.mjs` refuses a cart checked without `--wgpu`, and
  `cwproject build <dir> editor` builds the project editor in the dual
  tree (`.cwbuild/cart-wgpu`; the `.cwbuild/editor-cart` GL tree is gone).
  The native test binary `unit_pipeline`, which reads GL framebuffers
  itself, is native only and has no cart target.
- Every sample and the editor draw through the engine and build as dual
  carts: window clears with `cw_gpu_clear_window`, panels with the 2D
  renderer, own three.c renderers through `cw_render3d_window_renderer` /
  `cw_render3d_window_target` / `cw_render3d_texture_target`, GPU waits
  with `cw_gpu_finish`, pixel checks with `cw_gpu_read_window(_now)`. The
  editor's 3D viewport renders into its 2D target through `cw_target_rt`
  and its previews read with `cw_gpu_read_rt` (a cart on WebGPU shows a
  preview when it lands).
- Anti-aliasing Off in the quality settings turns the scene's MSAA off
  too, on GL and WebGPU alike; the presets (FXAA, SMAA) keep the game's
  MSAA on both.
- Split-screen views reuse the WebGPU MSAA attachments and only retarget
  the resolve.
- Profiler: in split screen each view's run of a pass is its own entry in
  the trace's GPU lane (`ssao@v1`; the pass's stats still sum the views),
  and the pipeline's own GPU work between passes is timed as the steps
  `views` and `resolve`. On WebGPU a pass is timed whole, so three.c's
  shadow maps, G-buffers and the volumetric fog's shadow tiles count in the
  pass that drew them. A frame keeps up to 128 GPU entries (was 48).
- The debug field `cw.msaa_resolves` counts the scene colour's resolves a
  frame. The engine is ready for a single resolve when the viewmodel pass
  follows the scene pass on WebGPU, and for PostProcess `depth_prepass` on
  WebGPU; both need three.c support that is not in v0.186.22 (with v0.186.22
  WebGPU resolves as before and draws without the prepass, the same
  picture).
- Clustered lighting does its scene work once a frame, not once a
  split-screen view: the frame's first view builds an index of the scene's
  omni and spot lights and meshes in one walk (lights a game added with
  three.c directly included), takes the clustered lights out of three.c's
  list for the frame, updates world matrices once and marks lit materials
  (a mesh again only when its material slots or their extensions change,
  so a material swapped by a plain write is seen); each view only filters
  the lights by its camera layers and visibility, projects, sorts, bins
  and fills its textures. The cascaded sun's shadow casters key is computed
  once a frame too. Four players on a large racing track, GL: the lights
  pass went from 3.8 to 1.3 ms of CPU a frame, material marking from 1.27
  to 0.13 ms, the picture identical.
- The light and froxel textures only grow (on WebGPU a texture made again
  each view cost every bind group that sampled it).
- three.c v0.186.22: `t3_graph_version` (a counter that moves when a
  scene's structure changes), GLES re-uploads a same-size texture into its
  existing storage, extension uniform and texture lookups are cached by
  name, and GLES shadow maps drawn before a renderer's first render get
  their camera.

### Fixed
- Ambient occlusion and the normals debug view at the frame's left and
  bottom edges: the normal from depth took the difference toward the
  neighbour outside the frame, so it was zero and the normal NaN, and GL
  drew a dark band three pixels high along the bottom of the frame.
- Impostors were about 35% darker than their meshes on GL and WebGPU (the
  normal atlas held sRGB-converted packed normals, so normals decoded bent
  away from the camera); the normal atlas is now sRGB storage and the
  shaders pack what sampling decodes. Against the same trees drawn as
  meshes: 6.71% of pixels over 16 levels became 0.69%.
- Every impostor showed its views mirrored front to back on GL (the bake
  counted viewport rows from the bottom; three.js counts from the top).
- Split screen drew the cascaded sun in the first view only: later views
  drew the sun through three.c's own shadow map. The sun now stays the
  frame's and each view fits its own cascades.
- GL crash in split screen with a cascaded sun: a later view's cascade
  casters drew with another pass's vertex array (no index buffer). The
  lights pass now resets three.c's bindings before its shadow draws.
- A light whose layers a game set while the lights pass had it out kept
  the pass's value; the game's value is kept now.
- Shadow maps drawn on a pipeline's first frame are not kept (the first
  frame had no sun shadows on GL when casters drew before the first scene
  render; three.c v0.186.22 also fixes the cause).
- A frame of a whole number of fixed steps (a 60 Hz frame of a 120 Hz
  game) passes the same `dt` bit for bit on every host; `cw.frame_dt`
  differed in its last bit between native and the cart hosts.

### Tests
- `tests/no-gl-only-carts.mjs` (gate, `tests/all.mjs` and the CI carts
  job): every built cart imports GL and WebGPU, the configure and import
  refusals hold, the project editor cart is dual, no `GL_ONLY` marker is
  left in the build; controls: a GL-only cart made on the spot and a
  planted marker are caught.
- Gates `msaa-hdr` and `msaa-hdr-aa-off` (with Off, GL and WebGPU both
  draw single-sampled and match; control: FXAA keeps the MSAA).
- WebGPU legs on the converted apps' gates (input, audio, motion, physics,
  platformer2d, character3d, r2d, fx, the editor gates, vis-city-house,
  vis-city-check) and again on vis-impostor, vis-impostor-band and
  vis-city (control: no bake on WebGPU, `CW_TEST_IMPOSTOR_WGPU_OFF=1`);
  the cart legs of wcnative-pads, showcase-perf, editor-channel,
  editor-import, editor-play, editor-level and plugins are back.
  `tests/gate.mjs`: a WebGPU control may be told apart by named state
  fields.
- Chromium WebGPU test runs wait for the GPU after each frame, so a
  frame's readbacks land before the next as on native and romdev (after a
  heavy frame, occlusion query counts arrived a frame late in Chromium
  only).
- Gates `lighting-changes` (two views, a cascaded sun, lights added,
  reparented, hidden, moved to another layer and removed, a node freed, a
  material swapped by a plain write; control: the first index kept),
  `lighting-remark` (control: a swapped-in material gets no shading parts)
  and `profile-lighting` (1600 boxes, 64 lights and a cascaded sun traced
  in one view and in four on GL and WebGPU: the scene work runs once a
  frame and four views cost the pass under 2.5 times one; controls: the
  scene work each view, an impossible ratio). It times the CPU against the
  test GPU, so it runs locally, not in CI's software rendering.
- Gate `split-gpu-share`: the viewmodel and prepass checks and per-view
  pass timing (controls: two resolves, a squeezed prepass depth, a run
  without GPU profiling).
- `CW_TEST_WGPU_NO_MSAA`, `CW_TEST_WGPU_NO_DEPTH_RESOLVE`,
  `CW_TEST_IMPOSTOR_WGPU_OFF` and `CW_TEST_IMPOSTOR_BAKE_MS` test switches;
  `CW_TRAP_STACK=1` in `tests/wgpu-carts.mjs` prints a cart's wasm stack
  at a trap.

## v0.19.1 (2026-10-28)

The program-table check builds WebGPU pipelines with the vertex buffers
three.c binds, and three.c v0.186.21 (no GL call when a WebGPU renderer is
destroyed).

### Fixed
- The WebGPU program check (`tools/lib/wgsl-check.mjs`, run by
  `tools/programs.mjs` after each regeneration) gave every vertex attribute
  a buffer of its own, so the instance matrix counted four times: an
  `InstancedMesh` with vertex colours, a normal map and instance colours
  needed 9 vertex buffers there, over WebGPU's 8, and the check refused
  states three.c draws with 6 ("Vertex buffer count (9) exceeds the maximum
  number of vertex buffers (8)"), so their tables could not be regenerated.
  It now uses three.c's own layout (`vertexBufferLayout` in three.c's
  `tools/program-model.mjs`: the matrix in one buffer, the instance colour
  in its own).
- A program that needs more vertex buffers or vertex attributes than a
  WebGPU device allows is reported by the check as over the device limits,
  naming its attributes, before the table ships.

### Changed
- three.c v0.186.21: a WebGPU program over the device's vertex buffer limit
  does not build and is named (it was a pipeline validation error), the
  vertex buffer layout is a tool API, and destroying a WebGPU renderer makes
  no GL call (a dual cart running on WebGPU trapped on `glDeleteTextures(0)`
  when a scene tree was freed).

### Tests
- Sample `instanced_attributes` and gate `instanced-attributes`: one
  InstancedMesh of 20 boxes with every optional vertex attribute (two uv
  sets, vertex colours, tangents, instance colours) and colour, normal,
  ambient occlusion, emissive and roughness maps under a shadow-casting sun,
  on GL and WebGPU, GL vs WebGPU within 0.05% of pixels; the control makes
  every instance white.
- `tests/programs-wgsl.mjs`: an instanced program with every optional
  attribute is accepted (7 buffers); the control, nine geometry attributes,
  is reported as over the device limits.

## v0.19.0 (2026-10-28)

Rim light, navigation links (jumps, drops, jump pads, ladders) and
split-screen fixes for volumetric fog and the frame history.

### Added
- Rim light (`cartwheel/rim_light.h`): a coloured glow along a model's
  silhouette, added to its emissive light and fading in with distance from
  the camera, so team colours read at 30 m without painting over a figure at
  arm's length; set the strength each tick for a pulsing shimmer (spawn
  protection). It composes with clustered lights, the cascaded sun, skin
  shading and global illumination on GL and WebGPU. New API:
  `cw_rim_light_desc`, `cw_rim_light_default`, `cw_rim_light_apply`,
  `cw_rim_light_get`, `cw_rim_light_remove`, `cw_rim_light_apply_tree`,
  `cw_rim_light_remove_tree`, `cw_rim_light_count`. Sample `rim_light`, gate
  `rim-light` (control: no rims). Cost on the 890M: under the noise for one
  view, about 0.45 ms with four split-screen views on WebGPU.
- Navigation links (Detour's off-mesh connections): two points that walking
  cannot join, such as a jump up a ledge, a drop, a jump pad or a ladder.
  `cw_nav_geom_add_link` takes a `cw_nav_link` (start, end, one- or two-way,
  a cost in metres of walking, an area that filters can exclude or weigh, a
  game tag `CW_NAV_LINK_JUMP` / `DROP` / `PAD` / `LADDER` / `TELEPORT` /
  `USER`, a radius, and a crowd crossing's duration and arc height). Links
  are built into static and tile-cache navmeshes and are part of the
  navmesh's bytes, hash and saves. `cw_nav_link_count`, `cw_nav_link_get`,
  `cw_nav_link_connected` (an end that misses the navmesh is named in a
  warning, counted in `cw_navmesh_stats.links_connected` and drawn red by
  `CW_NAV_DRAW_LINKS`). `cw_nav_find_path_links` and `cw_nav_take_path_links`
  give each waypoint's link id, for games that move their own characters.
  Crowd agents cross a link when they reach its start (status
  `CW_CROWD_ON_LINK`, carried along the arc), with
  `cw_crowd_set_link_callback` (`cw_crowd_link_event`, `cw_crowd_link_fn`)
  called in agent order when a crossing begins and lands; with
  `manual_links` an agent waits at the start until `cw_crowd_finish_link`.
  `CW_NAV_LINKS` is defined, so a game can test for links when it also
  builds against an older engine.
- Node `NavigationLink3D`, baked with its `NavigationRegion3D`;
  `NavigationAgent3D` emits `link_entered` and `link_exited` and has
  `manual_links` and `finish_link`.
- Sample `nav_links` (eight agents over a two-way ledge jump, a one-way pad
  and a drop, from the scene file) and gate `nav-links` on GL and WebGPU,
  every host (control: the links removed, nobody reaches the ledge or the
  deck); `unit_nav` covers paths with link ids, cost and area filters,
  one-way links, save and load, an unconnected link, crowd crossings equal at
  pool sizes 0, 1, 3 and 7, manual links and a no-links control.
- Sample `fog_shafts_split` (the fog room from four cameras) and gate
  `fog-split` on GL and WebGPU, every host (control: the old shared grid).
- `unit-nav` and `nav-demo` have WebGPU legs (Chromium and wasmcart-native
  run them again; they had no cart legs off romdev since the GL cart tree
  went away).

### Changed
- Navmesh saves are format 2 (they carry links); format 1 saves still load.
  A navmesh without links hashes as before.

### Fixed
- Split screen: volumetric fog gets a grid per view, sized to the view's
  share of the screen's width, with its own history, uniforms and previous
  view-projection. Before, the views shared one full-size grid: each view's
  temporal blend took the previous view's grid (ghost shafts from another
  camera), and on WebGPU every view's grid was lit with the last view's
  uniforms. Four-way split at 1920 x 1080 on the 890M: the fog grid went from
  2.94 to 1.65 ms (GL High) and 7.64 to 2.79 ms (GL Ultra).
- Split screen: the pipeline keeps each view's previous view-projection, so
  motion vectors and reprojection no longer follow the view drawn before.
- Split screen: the probe grid and reflection probe captures run once per
  frame (on the first view), not once per view.
- The volumetric fog's uniform locations are looked up once (each lookup
  waited for Mesa's GL thread).
- A pad that connects and presses a button on the same frame (a test host
  plugging a pad in, a controller connected with a button held) now reports
  that press: the press event arrived before the pad had a slot, so a
  one-press join (`cw_shell_join_press`) or the join screen missed it on
  wasmcart hosts. Presses are now also taken from each pad's button state
  read up then down. The `shell-join` and `shell-join-press` gates pass on
  romdev again.
- A sub-emitter (`emission_shape` PARENT) on a WebGL host (romdev's GL
  path) spawned every particle at its own node instead of at the parent's
  particles: the simulation sampled the parent's state textures while they
  were still attached to the bound framebuffer, which WebGL refuses as a
  feedback loop. The `fx-sub-emitters` gate passes on romdev's GL leg again.

## v0.18.1 (2026-10-28)

Build and CI cleanup: one cart per app, the dual GL + WebGPU cart. The
GL-only cart tree and its SDK are gone; GL is tested by running the dual
cart on GL hosts. Games built against an SDK recompile when the SDK changes.

### Added
- `samples/decal_walls` and the gate `decal-walls`: about two hundred
  multiply decals on a cliff through HDR, ambient occlusion, bloom and tone
  mapping (the cooperative platformer's settings), drawn the same on GL and
  WebGPU (0.0009 percent of pixels apart on the 890M).

### Fixed
- A game built against a prebuilt SDK now recompiles when another SDK is
  unpacked in its place (an engine bump). The archive keeps each file's time,
  so headers could look older than the game's objects and an incremental
  build linked stale objects against the new libraries (a struct whose layout
  changed crashed at run time). The SDK's CMake package now re-runs the
  configure when its directory changes and puts the SDK's identity in every
  compile line (`tests/sdk-rebuild.mjs`, with the old package as the
  control). Games on an SDK up to v0.18.0: build from a clean tree after an
  SDK bump.
- `scripts/fetch-deps.sh`: a file created by a patch that was later dropped no
  longer stops the fetch (the files each patch creates are recorded and
  removed when it is unapplied), and untracked leftovers that the new pin
  tracks are cleaned on a pin move (`tests/deps-patches.mjs`, with a
  control).
- `tools/programs.mjs --carts` fails, naming the app and host, when a survey
  covered nothing (a failed run, no pipeline reply, no frame rendered, no
  state list) or found no carts, instead of reporting 0 states and success;
  `tests/programs-carts.mjs` surveys noise_demo as the covered control, with
  three cases that must fail.

### Changed
- `scripts/build.sh` builds `build/native` and `build/cart-wgpu` only (the
  dual cart, which renders with WebGPU on WebGPU hosts and with GL on GL
  hosts). `TARGETS=cart` is refused with that reason; `NINJA_JOBS=<n>` caps
  ninja's parallel jobs.
- Releases carry no `cartwheel-sdk-<v>-cart` SDK any more: eleven SDKs, 22
  files (v0.18.0 had 24). Games use `cartwheel-sdk-<v>-cart-wgpu` for carts.
- CI builds the dual carts only (the two GL cart jobs are gone); the SDK
  workflow runs on Node 24 and `actions/download-artifact@v8`.
- `tools/cwproject.mjs build <dir> cart` builds the dual cart into
  `.cwbuild/cart-wgpu/carts/<app>.wasc` (and writes that path as
  `game_cart`); the project editor cart (`... editor`, the editor draws with
  GL itself) is built on request into `.cwbuild/editor-cart/`.
- A cart export (`wasc`, `web`) of an app built `GL_ONLY` is refused with the
  reason instead of shipping a GL-only cart.
- The editor's Play finds a game's cart in `build/cart-wgpu/carts`, and
  romdev plays it on GL (`wgpu: false`), as it played the GL cart.
- Tests and tools run the dual cart on GL where they ran the GL cart:
  romdev with `wgpu: false`, Chromium through the WebGPU tier's host with
  `wgpu: false`, wasmcart-native with `WASMCART_NO_WGPU=1`, the libretro core
  as is (`run-cart.mjs`, `perf-frame.mjs`, `net`, `channel`, `a11y-channel`,
  `profile-cart`, `remote`, `host-resolution`,
  `editor-level`, `plugins`). `cart-audio-ring` uses the dual hello cart and
  `wcnative-pads` checks pad slots on it (input_demo is `GL_ONLY`).
  `editor-channel`, `editor-import` and `editor-play` lost their editor cart
  legs and `showcase-perf` its cart legs (the editor and showcase_perf are
  `GL_ONLY`, so they have no dual cart).

### Known issues
- A 3D scene with an HDR target (sky, effects or a pass that reads depth) is
  multisampled on GL but single-sampled on WebGPU: the engine's WebGPU tier is
  compatibility mode, which cannot multisample `rgba16float` or read a
  multisampled depth texture, so edges differ slightly (about 0.5 to 1 percent
  of pixels in the cooperative platformer). v0.18.0 named Decal3D as the
  cause; it is not: decals draw the same on both (`decal-walls`).

## v0.18.0 (2026-10-27)

Ragdolls, skin shading and morph targets by name for characters, depth of
field, interactive water (ripples, caustics, the view from under the water),
a Screen shake setting, threaded carts that no longer trap when the heap
grows, GL and WebGPU drawing the same textures and decals, and three.c
v0.186.20.

### Added
- Ragdolls (`cartwheel/ragdoll.h`): a skeleton pose turned into Box3D
  capsules with cone, twist and hinge limits measured from the rest pose,
  seeded from the current pose, written back and blended into animation
  (bones no clip writes get their values back); a part can hang from a named
  part (IK feet). `cw_ragdoll_default_def`, `cw_ragdoll_create`,
  `cw_ragdoll_destroy`, `cw_ragdoll_part_count`, `cw_ragdoll_find`,
  `cw_ragdoll_body`, `cw_ragdoll_part_transform`, `cw_ragdoll_axis`,
  `cw_ragdoll_set_velocity`, `cw_ragdoll_apply_impulse`,
  `cw_ragdoll_apply_force`, `cw_ragdoll_write_pose`, `cw_ragdoll_blend_pose`,
  `cw_ragdoll_resting`, `cw_ragdoll_max_speed`, `cw_ragdoll_joint_angle`,
  `cw_ragdoll_hash` (types `cw_ragdoll`, `cw_ragdoll_def`, `cw_ragdoll_part`,
  `cw_ragdoll_joint_kind`). See `samples/ragdoll` and the animation guide.
- Characters:
  - Skin shading (`cartwheel/skin_shading.h`): wrap lighting and
    translucency on a skin material, from glTF extras,
    `KHR_materials_diffuse_transmission` or code (`cw_skin_shading_default`,
    `cw_skin_shading_apply`, `cw_skin_shading_apply_named`,
    `cw_skin_shading_remove`, `cw_skin_shading_get`,
    `cw_skin_shading_count`, `cw_skin_shading_enabled`,
    `cw_skin_shading_desc`); on from the Medium quality preset (`skin` in
    `cw_quality_scale`), plain PBR on Low.
  - Morph targets by name (`cartwheel/morph.h`: `cw_morph_set`,
    `cw_morph_get`, `cw_morph_reset`, `cw_morph_names`), and a glTF
    animation's morph weight channels play as AnimationPlayer tracks.
  - glTF materials keep their names and the factors of
    `KHR_materials_sheen`, `clearcoat`, `specular`, `ior`, `transmission`,
    `volume`, `emissive_strength`, `iridescence` and `anisotropy`.
  - `samples/character_skin` (six instances of one skinned glTF: skin,
    sheen, clear coat, a talking jaw clip, smiles and blinks by name). Costs
    on the 890M are in the rendering guide (twenty characters 0.75 ms GL,
    0.63 ms WebGPU).
- Depth of field on PostProcess (`dof`, `dof_focus_distance`,
  `dof_focus_range`, `dof_blur`, `dof_near_blur`): a full resolution gather
  in the HDR image before bloom, the blur from each pixel's view depth, a
  sharp subject keeping its edge against a blurred background. GL and WebGPU;
  off on the Low effects preset.
- Water:
  - Interactive ripples: a height field of small ripples drawn as a bent
    normal and foam, written by splashes, wakes and rain, bit-identical on
    every host (`cw_water_splash`, `cw_water_wake`, `cw_water_ripple_at`,
    `cw_water_ripple_hash`; Water3D `ripple_cells`, `ripple_size`,
    `ripple_center`, `ripple_speed`, `ripple_damping`, `ripple_strength`,
    `foam_fade`, `rain`, `rain_size`, `rain_strength`, `splash_fx`,
    `splash_fx_min`, and a body's `splash` and `wake`).
  - Caustics on the floor (softer with depth, only on surfaces facing up
    under the water), the view from under the water (fog that loses red first,
    light shafts, the surface seen from below; the `underwater` stat), lace
    foam, crest scatter and sun glint (`scatter`, `glint`).
  - The refraction depth on WebGPU through an R32Float copy (compatibility
    mode).
- Shell: a Screen shake setting (Full, Reduced, Off) saved with the other
  settings: `cw_shell_screen_shake` is the factor a game multiplies its
  camera shake by, `cw_shell_shake_level` / `cw_shell_set_shake_level` read
  and set it (`cw_shell_shake`, `CW_SHELL_SHAKE_FULL`,
  `CW_SHELL_SHAKE_REDUCED`, `CW_SHELL_SHAKE_OFF`,
  `CW_SHELL_SHAKE_REDUCED_SCALE`); reduced motion caps it at Reduced.
- GL on a WebGPU run stops at the call site: in a build that can render with
  WebGPU every GL entry point is checked, and the engine names the GL
  function, file, line and the feature that was running
  (`cartwheel/gl.h`). `tests/wgpu-carts.mjs` runs every dual cart on WebGPU
  and on GL for its longest gate's frames, with a control.
- `samples/texture_filtering` (a grazing tiled floor and a wall of textures at
  anisotropy 8) and its gate, GL against WebGPU.
- `tools/programs.mjs`: every regenerated WebGPU program is compiled with
  Tint and a rejection fails the run naming the state; `--carts
  --webgpu-only` skips the GL host leg; `CW_PROJECT_ROOT` surveys a games
  repository's carts and gates.
- `tools/run-cart.mjs`: Chromium runs the dual cart told to render with GL
  (`--dual-gl`), as romdev does, when there is no GL-only cart.
- Tests: `tests/mem-grow.mjs`, `tests/jobs-stall.mjs`,
  `tests/romdev-load-args.mjs` (every romdev load passes `gpuPower`) and
  `tests/deps-patches.mjs`, each with a control that must fail.

### Changed
- three.c v0.186.20 (was v0.186.19 with three local patches): glTF material
  names and KHR_materials factors, morph target names, WebGPU blend modes,
  anisotropic WebGPU samplers, flip_y screen derivatives and one-morph-target
  meshes on WebGPU compatibility mode. The engine carries no three.c patches.
- The wasmcart and wasmcart-wgpu test hosts are v0.36.2 (0.34.3 read integer
  textures four times over); `scripts/fetch-deps.sh` reinstalls wasmcart's
  packages when its lockfile changes.
- The unit test suites are dual carts (they clear the window with
  `cw_gpu_clear_window`), so their romdev legs run the dual cart on GL;
  `unit_pipeline` reads GL framebuffers itself and stays `GL_ONLY` (native
  only when there is no GL-only cart tree, said by the gate runner).
- `tests/vfs.mjs` and `tests/water-surface.mjs` use the dual cart on a GL
  host instead of the GL-only cart.

### Fixed
- Threaded carts: a thread whose view of the shared memory lagged another
  thread's growth trapped in memset or memcpy although the address was in
  bounds (V8 checks `memory.fill` and `memory.copy` against a per-thread copy
  of the size, updated only at an interrupt check; a navmesh tile job trapped
  in `cw_calloc`). Every allocation refreshes the calling thread when the
  block lies past what it sees, and the job pool catches a lagging thread up
  before each chunk and after a wait.
- A cart's page thread stops with an error when a job makes no progress for
  30 seconds (a worker thread died mid-chunk) instead of spinning forever and
  wedging its host.
- GL and WebGPU draw the same: Decal3D and every material with a custom,
  subtractive or multiply blend (WebGPU drew them as normal blending),
  anisotropic textures (WebGPU sampled them at anisotropy 1), sampler settings
  changed after a texture's upload (GL kept the old ones), and normal maps on
  geometry without tangents in the pipeline's offscreen targets (WebGPU lit
  them from the wrong side). A mesh with one morph target draws on WebGPU.
- Volumetric fog heals from a broken cell (a NaN becomes 0, each cell holds
  at most 60000 of light and an extinction of 64, and the temporal blend
  takes in at most 8 times what a cell holds) instead of a black cloud that
  never cleared.
- `scripts/fetch-deps.sh` moves a checkout with patches applied to a new pin
  (the patched files restored and the files the patches made removed first),
  and skips a patch already applied.
- Gate runner: `--gpu webgpu` names each requested gate that has no WebGPU
  entry (SKIP) and fails when none has one, instead of passing a run that
  tested nothing; a host `--hosts` asked for that a gate leaves out is a
  WARN; test timeouts kill their whole process tree.
- The ragdoll body accessor `cw_ragdoll_body` is documented (the API check
  failed on it).

## v0.17.0 (2026-10-27)

Fighting, shooting and platforming controls (motion inputs, a framing camera,
aim curves, first person viewmodels, a 2D platformer controller), an
explosion kit with heat haze and shockwaves, shore and depth shading on water, animation events, joining a
match while it runs, quality scopes, and three.c v0.186.19.

### Added
- Motion inputs (`cartwheel/motion.h`): a per-player command buffer
  (`cw_cmd_push`, `cw_cmd_dir`, `cw_cmd_pressed_ago`, `cw_cmd_released_ago`,
  `cw_cmd_held_now`, `cw_cmd_consume`, `cw_cmd_clear`) and a motion matcher
  (`cw_motion_parse`, `cw_motion_match`, `cw_special_match`) with leniency,
  charges, exact steps (`!6!6`, so a dash is not read out of a quarter circle,
  and the last step of a motion is always exact) and buffered buttons. See the
  motion input guide and `samples/motion_demo`.
- A framing camera (`cartwheel/framing.h`): `cw_frame_fit` finds where a
  camera has to be to keep a set of points in view with a margin, and
  `cw_frame_follow` eases it there.
- Aim curves (`cartwheel/aim.h`): `cw_aim_step` turns a look stick into yaw
  and pitch with a dead zone, a response curve, turn rates, a boost that
  builds while the stick is held at its edge, and aim assist;
  `cw_aim_curve_default` gives the tuned defaults.
- Render layers and first person viewmodels: Node3D `layers`, and Camera3D
  `cull_layers`, `viewmodel_layers` and `viewmodel_fov`. The viewmodel layers
  are drawn after the scene with their own field of view, over the kept depth,
  so a weapon never clips into a wall (GL and WebGPU). See
  `samples/viewmodel_demo` and the render layers section of the 3D rendering
  guide. The viewmodel pass never draws the scene's background again (a
  ProceduralSky would cover the picture), and the gate reads the scene behind
  the gun to prove it.
- A 2D platformer controller (`cartwheel/platformer2d.h`:
  `cw_platformer2d_init`, `cw_platformer2d_step`, `cw_platformer2d_teleport`,
  `cw_platformer2d_default_def`): coyote time, jump buffer, variable jump,
  wall jump, dash and swimming, with corner correction and one-way platforms
  (`one_way_mask`) in the 2D mover. See `samples/platformer2d`.
- A distortion buffer and a copy of the scene colour: heat haze (GPUParticles3D
  blend `DISTORT`), refracting particles (`REFRACT`) and `Shockwave3D`
  (`cw_shockwave_fire`, `cw_shockwave_active`, `cw_shockwave_radius`), on GL
  and WebGPU, wired into the effects quality presets (`distortion`,
  `dispersion` and `flash_lights` in `cw_quality_scale`).
- An explosion kit (`cartwheel/explosions.h`, the explosions guide):
  - `Explosion3D` conducts a blast: a flash from a pooled light
    (`flash_energy`, `flash_color`, `flash_range`, `flash_duration`), a
    cooling scorch mark (`scorch_size`, `scorch_time`, `scorch_life`,
    `scorch_glow`, `scorch_depth`, `scorch_offset`; four baked shapes,
    each turned and stretched at random) and camera shake (`shake_amount`,
    `shake_radius`, `shake_time`), with `autostart`, `loop` and
    `duration`. `cw_explosion_fire`, `cw_explosion_playing`,
    `cw_explosion_shake`, `cw_shake_offset` and `cw_explosion_get_stats`
    (`cw_explosion_stats`). The engine pools the lights and decals under the
    tree's root (`ExplosionFlash`, up to 8 by quality; `ScorchDark` and
    `ScorchGlow`, 12 pairs).
  - GPUParticles3D sub-emitters (`emission_shape = PARENT`, `parent`,
    `inherit_velocity`) and motion-vector flipbooks (`motion_texture`,
    `motion_strength`); `tools/fx/bake-flipbooks.mjs` bakes fire and smoke
    sheets with their motion sheets.
  - Samples `fx_explosion` and `fx_sub_emitters`.
- Water: a baked floor depth map from the static physics shapes, a shallow to
  deep colour, shore foam and crest foam (`floor_map_size`, `floor_range`,
  `shallow_color`, `clarity`, `shore_foam`, `crest_foam`;
  `cw_water_floor_depth_at`, `cw_water_rebake_floor`).
- Animation events for gameplay (`cw_anim_add_event`,
  `cw_anim_player_on_event`, `cw_animtree_on_event`, `cw_anim_event_count`,
  `cw_anim_event_time`) and an update divisor for far characters
  (`cw_anim_player_set_update_divisor`, `cw_anim_player_update_divisor`,
  `cw_anim_lod_divisor`).
- Game shell: a `draw_background` hook draws the game's own picture behind
  the title, join, settings and pause screens, and `shared_screen` gives every
  player one shared view instead of a split screen. Players join and leave
  during play: `cw_shell_join_open` shows the join screen over the running
  match (seats keep their numbers), `cw_shell_join_press` seats a pad the
  moment it presses A, `cw_shell_join_requested` cues the game, an unplugged
  pad frees its seat, and `players_changed` tells the game. See
  `samples/shell_join`.
- Quality scopes: `cw_quality_scope_push` / `cw_quality_scope_pop` cap
  settings for one scene or mode (never raising what the player chose), and
  `cw_gfx_effective` gives the value in force.
- `cw_frame_dt()`: seconds since the previous rendered frame, the same value
  on every host (debug field `cw.frame_dt`).
- Handles cut to 32 bits are caught: every pool lookup and `cw_crowd_*` call
  reports one with an error naming the fix (`cw_handle_report_truncated`,
  `cw_handle_truncated_count`, `CW_HANDLE_IS_TRUNCATED`), and
  `cw_crowd_agent_slot` gives a small, safe index for a game's own arrays.
- Program tables: a draw whose program state no table has is loud: an error
  naming the command that adds it (`node tools/programs.mjs <app>`), a banner
  on the picture while any state is missing, and `CW_PROGRAMS_STRICT=1` stops
  a native run with status 4. `tools/programs.mjs --carts` surveys the dual
  cart on a GL host and a WebGPU host and merges the states it is asked for.
- `scripts/fetch-deps.sh --verify` lists dependencies that are off their pin,
  and the build stops before compiling when `deps/` is off its pins, naming
  the fix.
- `tools/programs.mjs --scenes NAME=a,b,c` surveys an app once per scene, so
  a game that switches scenes gets the union of their program states.
- Gate runner: `--cpu` runs state-only gates on the software renderer with
  no GPU suite lock, and a gate's `env` applies to every native run of it.
- The GPU suite lock has a hold limit (`CW_SUITE_MAX_MINUTES`, default 15).

### Changed
- three.c v0.186.19 (was c5f840b with a local patch): rgba vertex colours
  (a glTF exporter's default `COLOR_0`) draw with their alpha ignored instead
  of the draw being skipped, the WebGPU viewport depth range the viewmodel
  pass uses, and three.c's own GL program tables with every flipY flag of a
  texture sampled at several places filled.

### Fixed
- Romdev loads always pass the test GPU's power preference (`gpuPower`), GL
  loads included, so tests never land on a machine's display GPU.
- Navigation: a full obstacle queue is applied by the next add or remove
  instead of refusing it.
- Skipped draws say once, at error level, how to regenerate the program table.
- CI: open_world's long runs get the time llvmpipe needs, and the lit decals
  test runs only its native legs there.
- A WebGPU host that cannot multisample the scene (HDR, sky, effects) renders
  it single-sampled instead of asking GL for a multisampled framebuffer, which
  stopped every dual cart whose PostProcess set `msaa`.
- `alpha_to_coverage`, listed in v0.15.0 as softening alpha-tested foliage
  edges, does not do that yet: an opaque alpha-tested material draws the same
  picture with it on or off (see three.c v0.186.19's notes).

## v0.16.0 (2026-10-27)

Pad rumble: a game drives each player's pad motors, natively and in carts.

### Added
- `cw_input_rumble(player, low, high, ms)` runs a player's pad motors: `low`
  the heavy motor, `high` the light one (0 to 1), for up to 5000 ms, replacing
  the rumble running; `cw_input_rumble_stop` ends it and
  `cw_input_has_rumble` says whether the player's pad has motors. A player with
  no pad, a pad without motors or a host without hardware make these silent
  no-ops, so a game calls them unconditionally. Natively it is SDL's
  controller rumble; in a cart it is wasmcart's existing `wc_pad_rumble` (no
  new imports). `CW_HAVE_INPUT_RUMBLE` lets a game also build against an older
  SDK, and the debug field `cw.rumble` shows the last request to tests. See the
  input guide.
- The gate runner takes `--gates-file` / `CW_GATES_FILE` to name another gate
  file in the project's `tests/`, so a repository of several games keeps one
  file per game.

## v0.15.1 (2026-10-23)

Loading without the stalls: the loading screen's shader phase compiles one
program per material rather than one per mesh, terrain layer uploads cost
milliseconds instead of tens, and a game's own shading parts keep the engine's
shadows and lights.

### Changed
- The scene app's warm-up (the shader phase under the loading screen)
  reveals one object per distinct material, object kind and attribute set,
  then the rest at once: a game that builds every character and vehicle once
  at init so their programs compile under the loading screen (BikeMesa's
  seven bikes and three riders: a thousand meshes over thirty materials)
  costs thirty slices, not a thousand.
- `cw_terrain_set_layer_pixels` resamples a layer to the tile size on the job
  pool in fixed point and takes the layer's mean colour through a 256-entry
  table instead of a `powf` per texel: a 256-square layer cost 55 ms on one
  thread in a cart (BikeMesa's backdrop set three per track load), now about
  3 ms. The resampled pixels are the same.
- Shading parts a game registers (`cw_shading_part`) now compose with the
  engine's lighting: the sun's cascaded shadows and clustered lights are added
  to a material carrying a project part (before, only the engine's own part
  names composed and such a material lost its shadows). `cw_shading_part_known`
  asks whether a part is registered.
- Terrain3D keeps the shading parts a game added to its material across a
  quality switch (it swaps only its own surface part), so a project part on
  the terrain survives the graphics menu.

## v0.15.0 (2026-10-22)

Screens of other aspects: a game declares how it meets a 4:3 handheld or a
16:10 Steam Deck and gets the whole screen instead of bars. Retina windows
draw the whole window again.

### Added
- `cw_app_desc.fit` with `aspect_min` / `aspect_max` (cartwheel/platform.h):
  how a game meets a screen of another aspect. `CW_FIT_HEIGHT` keeps the
  design height and lets the logical width follow the host's preferred size
  (Hor+: a 960x540 game is 720x540 on a 640x480 handheld, 864x540 on a
  1280x800 Steam Deck, 1280x540 on 21:9) between the aspect bounds (default
  4:3 to 2.4), so the frame is the whole screen instead of bars; the 3D
  camera and a plain 2D canvas follow, as they already did natively on a
  window resize. `CW_FIT_WIDTH` is Vert+. `CW_FIT_LETTERBOX` (0) is the
  behaviour so far. `cw_design_size` is the declared size, for HUD scale
  factors; `cw_fit_logical_size` is the pure fit, for tests and tools.
  Natively `CW_HOST_PREF=WxH` behaves as a cart on a host preferring that
  frame, so a handheld's picture is one environment variable away.
- `CW_SCALE_EXPAND` (cartwheel/render2d.h; `EXPAND` in CanvasSettings):
  a virtual canvas at `FIT`'s scale over the whole window, so a pixel-art
  game shows more world on a wider screen instead of bars; Camera2D limits
  still hold.
- three.c c5f840b: `t3_material.alpha_to_coverage` (material.alphaToCoverage):
  on a multisampled scene pass (PostProcess `msaa`) an alpha-tested card's
  alpha becomes its sample coverage, so foliage edges are soft. GL only.

### Fixed
- three.c ba2dadc (after v0.186.18): an extension texture may be sampled up
  to 128 times in one program (was 64). The terrain extension's Medium and
  High tiers sample the layer-normals array 72 times, so every near terrain
  draw was skipped with "sampled too many times" in a game whose program
  table was generated from a fresh project (the samples' table hid it).
- HiDPI windows (macOS retina): the output is now sized in the window's
  drawable pixels, not its points. The render resolution setting below 100%,
  `pixel_scale` and the crisp-UI floor drew the scene into the lower-left
  quarter of the window (the viewport was in points on a 2x drawable) and the
  2D passes over it clipped. Mouse events (points) map through the ratio.
- ProceduralSky with `background` off no longer sets the scene's background
  texture to NULL every frame: it only lets go of its own cube, so an app can
  keep a background cube of its own (a skybox) while the physical sky lights
  the scene.

## v0.14.1 (2026-10-21)

Cart audio and controller fixes, safer visibility and terrain updates, and
smoother terrain, ocean and volumetric cloud rendering on GL and WebGPU.

### Added
- Water3D `horizon_distance` stretches the outer grid into one connected
  ocean surface while preserving dense sampling in the central `size`.
- `tools/programs.mjs --extend <module>` includes project shader extensions
  alongside engine extensions, including composed sun-shadow variants.

### Fixed
- Cart audio now reserves 16,384 stereo frames instead of 4,096. A valid
  100 ms host frame generates 4,800 samples at 48 kHz and previously overwrote
  unread samples in its own block. The maximum generated block remains
  8,192 frames; compile-time capacity and power-of-two checks protect it.
- Cart L3, R3 and Guide buttons reach the SDL input snapshot.
- Render-scale changes apply at the next frame boundary instead of resizing
  a render target while a frame is using it.
- Terrain height edits invalidate both height and normal textures, so edited
  slopes receive updated lighting on the GPU.
- VisibilityManager respects hidden raw three.c ancestors of procedural
  meshes and uses fixed InstancedMesh spheres for conservative cached bounds,
  independent of the active LOD subset.
- Volumetric noise wind uses world units per second regardless of noise scale.
- Project foliage shader extensions compose with directional and cascaded
  sun shadows while retaining their material/decal entry points.

### Changed
- Terrain uses smooth macro noise and stochastic triangular layer blending
  to reduce repeating surface patterns. Layer textures grow from 512 to
  1,024 pixels per side: four times the texel storage; anisotropic filtering
  is enabled for terrain layers.
- Medium volumetric fog grows from 96 × 48 to 128 columns and 64 depth slices;
  screen-height resolution also follows column count. At the same aspect
  ratio, storage and volume work grow by about 137%. Bicubic XY reconstruction on both
  backends smooths cloud silhouettes; the larger preset costs more memory
  and rendering time.
- Distant ocean wave normals fade toward a flat surface between 2 and 12 km
  to reduce subpixel glints toward the horizon.

## v0.14.0 (2026-10-21)

Box2D v3.2 under the 2D physics, with rollback that restores 2D worlds whole.

### Added
- **Large 2D worlds**: Box2D keeps world positions as doubles (about 1000 km
  of world instead of 16 km). `cw_pos2` and `cw_wxform2` carry body
  positions, ray and hit points, query origins, joint anchors and the
  character mover's feet; `cw_body2d_world_transform`,
  `cw_body2d_set_world_transform`, `cw_body2d_set_world_target` and
  `cw_body2d_position` read and write them, and `cw_qshape2d_at` gives a
  query shape an origin. The float calls still work. Scene nodes stay floats
  in pixels.
- **2D snapshots**: `cw_world2d_snapshot` / `cw_world2d_restore` save and put
  back a whole world, warm-start caches and sleeping islands included, and
  `cw_world2d_state_hash` is Box2D's host-stable hash of it.
- **2D recording and replay**: `cw_world2d_record_start` / `_stop`,
  `cw_recording2d_*` (save, load, validate) and `cw_replay2d_*` (open, step,
  seek, diverged, debug draw), plus the queries the game made each step
  (`cw_replay2d_query_count`, `_get_query`, `_get_hit`,
  `_debug_draw_queries`) and keyframe control and statistics for seeking
  backwards.
- **Mover and pogo joints** (2D only): `CW_JOINT_MOVER` commands a velocity
  on a dynamic body, limited by a force; `CW_JOINT_POGO` is a spring that
  holds a dynamic character off the ground. The `Joint2D` node has them as
  `MOVER` and `POGO` (`mover_velocity`, `mover_max_force`, `pogo_normal`,
  `pogo_tension`, `pogo_compression`).
- 2D world, body, shape, chain and joint controls from Box2D v3.2: maximum
  linear speed, contact recycle distance, contact tuning, restitution
  iterations and propagation, workers, capacity, bounds, a 4-wide solver
  switch; body sleep, contact recycling, safety factor, extents, bounds,
  centre and mass update; shape bounds, point test, closest point, ray cast,
  rounded boxes, loose chain segments and debug colour; per-segment chain
  materials; joint rest length, speed, motor and spring force, separations.
- 2D stats: `cw_world2d_profile_get` (the last step's milliseconds by phase),
  `cw_world2d_counters_get` (Box2D's counters, constraints per solver
  colour) and `cw_physics2d_memory_bytes`.
- 2D debug draw modes: `CW_PHYS_DRAW_JOINT_EXTRAS`, `_ANCHOR_A`,
  `_CHAIN_NORMALS`, `_CONTACT_FORCES`, `_FRICTION_FORCES`, `_ISLANDS` and
  `_GRAPH_COLORS`.
- `cw_font_preload_range` and `cw_font_preload_text` bake glyphs during
  loading instead of on first draw; the text call also prepares fallback and
  contextually shaped glyphs for localized strings.
- `cw_mesh3d_set_procedural` puts a `t3_mesh` built in code on a
  MeshInstance3D, so the VisibilityManager culls it like any other mesh.
- Profiler zones `r2d.flush`, `r2d.sort_batch`, `r2d.upload` (the end of a
  2D pass) and `text.glyph_bake`.

### Changed
- The 2D wrapper API takes and returns `cw_pos2` (double) wherever a world
  position goes: ray cast and overlap origins and bounds, hit and event
  points, force and impulse points, joint anchors and targets, and the
  `cw_mover2d` feet and positions; a query shape's points are now floats
  relative to its origin (`cw_qshape2d_at`). Code that held these in
  `cw_vec2` changes type; `cw_body2d_transform` and
  `cw_body2d_set_transform` still take floats.
- three.c v0.186.18 replaces v0.186.17: the prepass and bounds sorting below
  come from it, an InstancedMesh's changed instance data goes into new
  buffer storage on GLES (the upload no longer waits on the previous frame),
  and a new light never inherits a destroyed light's shadow map.
- Box2D v3.2.0 replaces the earlier v3.1 pin. The engine's patches are ported (WebAssembly
  SIMD in the shared 4-wide solver, relaxed spin polls, wasi threads); the
  8-wide AVX2 solver is picked at run time, so the `BOX2D_AVX2` compile flag
  is gone. Box2D task workers are capped at 32 (was 64). The
  `physics-determinism` gate checks upstream's v3.2 double-precision hashes.
- Rollback restores 2D physics worlds through Box2D snapshots: a tree
  snapshot carries each 2D world, so dynamic `RigidBody2D` piles and
  `Area2D` overlap signals re-simulate bit for bit, and the desync hash folds
  in Box2D's state hash. 3D is unchanged: Box3D has no snapshot, so 3D
  dynamic rigid bodies and Area3D signals still do not re-simulate exactly.
- Box2D's log lines go to the engine log as warnings, and a failed Box2D
  assertion is logged as an engine error.
- The depth prepass draws through three.c's `t3_renderer_render_prepass`:
  transparent and transmissive surfaces no longer write depth there (they
  hid what was behind them), while alpha-tested cutouts still discard.
- Opaque meshes sort front to back by the centre of their world bounds
  instead of their origin, which orders merged and instanced scenery
  correctly. `CW_SORT_ORIGINS=1` restores origin sorting for comparison.
- Clustered lighting's spot light cone test rejects cells along the axis
  before the square root, so binning is cheaper.
- `tools/run-cart.mjs` asks romdev for the requested backend explicitly, so a
  GL gate on a cart that can run both never lands on WebGPU.

### Fixed
- Split-screen views restore the VisibilityManager's culled meshes after
  each view, so one view's culling no longer hides meshes from the next
  view's shadows, GI or culling. The restore also runs on the first frame.
- Screen-space decals in split screen: each viewport uploads its decals into
  fresh buffer storage instead of overwriting what the previous view was
  still drawing from.
- A mesh decal created after the frame's 3D poses were updated stamps at
  the decal's real position instead of the origin.
- A GPUParticles3D whose `preprocess` (prewarm) reaches back before the
  scene started reports its live particles and keeps simulating once
  emission stops; its negative emission start read as never emitted.
- Clustered lighting no longer copies from an empty cell's storage.

## v0.13.0 (2026-10-17)

### Added
- Mesh decals are lit like the surface they sit on: a decal takes the same
  lights, sun cascades and shadows, GI and fog, in FORWARD and CLUSTERED
  lighting, on rigid and skinned meshes and on glass. Multiply and add decals,
  and decals on unlit surfaces, are drawn as before. A `CW_DECAL_UNLIT=1`
  debug variable forces the old unlit decal.

### Changed
- Changing a decal's `blend` rebuilds it, because a normal decal on a lit
  surface now carries a different material.
- An app whose lit decals use a program state missing from its table skips
  those draws, like any lit material: regenerate its table with
  `tools/programs.mjs`.

## v0.12.2 (2026-10-17)

### Fixed
- Mesh decals land on surfaces lit by clustered lights, cascades and GI.
  Decal3D mesh mode used to skip any mesh whose material carried a shading
  part, which is every lit material in CLUSTERED lighting. The decal itself is
  still drawn unlit, as in FORWARD lighting.
- Light3D energy runs to 100000 (it was clamped to 1000 with no sign), and a
  ranged property set outside its range now logs one warning per node and
  property.

## v0.12.1 (2026-10-17)

### Fixed
- The SDKs ship every module's generator extensions (`src/*/extensions`). The
  v0.12.0 SDK left out the water module's, so `tools/programs.mjs` could not
  capture Water3D program states against a prebuilt SDK.

### Changed
- CI runs the WebGPU tests in six shares, balanced by freshly measured test
  times (`tests/ci-times.json`); a run had grown to about an hour.

## v0.12.0 (2026-10-17)

### Added
- `cw_cloth_step_many`: steps a scene's cloths as one job each, and a cloth
  cost table in the physics guide.

### Changed
- GI is cheaper for the same picture: the G-buffer draws use a program of
  their own instead of the full lit one, and SSGI traces a half-resolution
  colour copy and a two-channel normal copy. On the 890M at 1280x720 (GL), a
  frame at Medium goes from 2.6 to 2.3 ms, at High from 4.5 to 4.2 ms, and at
  Ultra from 18.2 to 14.2 ms.
- The GPU guard test scales to the GPU it is pinned to, and the network test
  runs wasmcart-native players paced instead of uncapped.

### Fixed
- A reflection probe capture no longer leaves the sun's shadow maps drawn
  without the objects it hides from its capture: the GI pass redraws them
  after its renders, so a capture frame keeps every shadow.

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
