# Changelog

What each release added, changed and fixed. Releases are git tags; each one's
prebuilt SDKs (one per target, see [docs/sdk.md](docs/sdk.md)) are on its
GitHub release. The guides in `docs/guides/` cover each feature in depth.

## v0.71.0 (2026-11-23)

### Added
- Foliage (`cartwheel/foliage.h`): wind, smooth cut-out edges, two-sided lighting and dithered fades for grass, leaf
  cards and bushes, as shading parts on a material (`foliage`, and `folalpha` for smooth edges), each switched by its
  values. `cw_foliage_apply`, `cw_foliage_apply_mesh`, `cw_foliage_apply_tree`, `cw_foliage_remove`,
  `cw_foliage_get`, `cw_foliage_count`, `cw_foliage_default`, `cw_wind_default`, `cw_wind_lean` (the wind on the
  CPU, for gameplay), `cw_foliage_time`.
  - Wind (`cw_wind`: `strength`, `frequency`, `direction`, `gust`, `gust_scale`): each vertex leans downwind by the
    square of its height on the mesh, with a phase from its world position (a smooth noise about a metre across) so
    neighbouring plants never sway together, and gusts travelling over the field. The engine's frame clock drives
    it: the same leaves on every host and in every view of a split screen; the shadows move with them.
  - Smooth cut-out edges (`smooth_alpha`, alpha-tested materials such as a glTF MASK): alpha to coverage under MSAA
    (three.js r187's alphaToCoverage program; on WebGPU through three.c v0.187.21), hashed alpha on noise moved every
    frame under TAA without it. The engine picks the mode each frame from the scene pass's sample count.
  - Two-sided lighting (`two_sided_lighting`): a double-sided material's back faces lit with the front face's normal
    and normal map instead of the flipped one, so leaves with up-bent normals no longer go dark on the back.
  - Dithered fades (`fade_in_*`, `fade_out_*`): a screen-door that TAA resolves; two LODs given the same band
    crossfade.
  - Root colour (`root_color`, `root_height`, `tip_jitter`): the lower part of an instanced mesh takes its instance
    colour.
- TerrainScatter3D and `cw_scatter_def`: `wind_strength`, `wind_frequency`, `wind_direction`, `wind_gust`,
  `wind_gust_scale` (`wind`), `smooth_alpha`, `two_sided_lighting`, `fade_mode` (SHRINK, the default, or DITHER:
  instances dissolve over the fade and each LOD crossfades with the next over `lod_blend` metres round its switch,
  an instance in the band drawn by both), `tint_jitter` (a per-instance colour: brightness and a dry yellow or deep
  green), `root_from_terrain` and `root_height` (each instance's root in the terrain's colour under it). Any of them
  on gives the layer its own copies of its materials with the foliage parts; all off, a layer draws as before.
- `cw_terrain_albedo_at`: the terrain's colour at a point (its layers' mean albedo weighted by the control map, times
  the terrain's colour); `cw_terrain_scatter_cell_colors`: a cell's instance colours.
- Sample `foliage_field`: the GRASS tuft over two terrain layers from a play camera, and the scenes the gate
  measures from the picture. Gates `foliage` (native and romdev, GL and WebGPU, each check with a control that must
  fail) and `foliage-field`; `tests/foliage-perf.mjs` reports what each option costs. Measured from the picture, the same on every host and backend: wind 0.1 moves 1 m stems' tips 0.07 m (0.2: 0.14 m), neighbours uncorrelated (-0.12; control in one phase 0.99); `smooth_alpha` cuts card shimmer under a pan by 46% (MSAA 4x) and 31% (TAA); a card's back lit within 0.001 of its front (control: 0.38 darker); DITHER cuts the largest single-frame change at a LOD switch from 22/255 to 8/255; strength 0 with the part on draws the no-wind picture byte for byte, and scenes with none of it (terrain_demo's rocks and trees) byte for byte as v0.70.0. GPU on the 890M at 1920 x 1080 over a dense field: wind about 0.5 ms, `smooth_alpha` 0.4 to 0.7 ms, DITHER 0.5 to 0.9 ms.

- Terrain3D near-ground options (cartwheel/terrain.h, [Terrain](docs/guides/terrain.md#the-near-ground)). A terrain
  that uses none of them draws exactly as before, pixel for pixel, with the program it always had (`terrain_demo` and
  `open_world` at every preset, GL and WebGPU, byte for byte against v0.70.0; GPU time the same within the noise):
  - `blend_sharpness` on `cw_terrain_def` and the Terrain3D node (0.15 by default, the value the height blend always
    used): smaller is a crisper edge along the higher layer's shapes, larger a softer one; per layer with
    `cw_terrain_set_layer_blend_sharpness` and `layer<i>_blend_sharpness` (a pair uses the mean of its two).
  - Macro variation against a single layer reading as wallpaper from a play camera, both off by default:
    `macro_tint` (brightness and a little hue varied by world-space noise over `macro_tint_scale` metres, 40 by
    default) and `macro_layer` (a second read of each layer with a tile `macro_layer_scale` times larger, 4 by
    default, blended in by height from `macro_layer_near` to `macro_layer_far` metres, 0 and 60 by default).
  - Parallax occlusion on a layer's top-down projection: `cw_terrain_set_layer_parallax` and `layer<i>_parallax` (the
    depth in metres from the top of the layer's height to the bottom), within `parallax_distance` metres (30 by
    default). Steps per pixel follow the quality: none at Very Low and Low, 4 at Medium, 8 at High, 16 at Ultra.
  - `cw_terrain_set_shading`: the def's shading fields, live; the node applies a change of any of these properties at
    once.

  Costs on a Radeon 890M at 1920 x 1080 (`samples/terrain_detail` from its play camera, flagstones over two thirds of
  the frame, release build, the least of 3 interleaved rounds, the frame without the option 1.7 to 1.9 ms): rotation
  blend 0.62 ms GL / 0.80 WebGPU (against the same field drawn with the nearest texel's rotation), macro tint 0.63 /
  0.72, macro layer 1.00 / 0.89, parallax 1.03 / 0.90 at Medium, 1.20 / 1.13 at High, 1.77 / 1.66 at Ultra. About 0.6
  ms of any of them is the larger program that carries the options (the `terrainfx`, `terrainfxlow` and
  `terrainfxmin` surface parts), which a terrain draws with only while it uses one.
- Sample `terrain_detail` (a flat field of flagstones and sand, test switches `TD_*`). Gate `terrain-detail` (native
  and romdev, GL and WebGPU: each option measured from the picture, with controls, among them
  `CW_TEST_TERRAIN_NEAREST_UV=1`, each point's rotation from its nearest texel). Timing report
  `terrain-detail-perf`.
- `cw_post_prewarm(pipeline, &settings)` and `cw_pipeline_post_prewarms` (`cartwheel/pipeline.h`): the post passes a
  game turns on later (SSR, `ssr_backdrop`, SSGI, ...) prewarmed. Turning SSR on puts the gi part on every lit
  material of the scene, so the frame a style first turned it on built every material's programs and WebGPU pipelines
  for the scene and the SSR G-buffer, and the SSR passes' own: a night sea world fading in with SSR cost 213 to 304 ms
  of CPU on WebGPU and 138 to 172 ms on GL on the 890M (80 ms on the Steam Machine in a rhythm game). Given in
  `init`, the settings are kept; at the scene app's prewarm (the end of its warm-up, and of each loading phase) each
  pass they turn on and the current settings do not runs once with them in the shadow and post phases, its picture
  drawn into a pooled target of its own (kept while the settings are, so the chain has it when the pass comes on), and
  the scene is drawn once more with the gi part on, every object shown, into small targets of the scene target's
  format and samples; the part then comes off before the frame's own scene pass. Nothing of it reaches the frame.
- The scene prewarm, for an app that gives `cw_post_prewarm` settings: the same prewarm draws the scene once more with
  every object shown (hidden ones, instanced meshes of no instances), so what a style shows later builds nothing then
  (an app that does not keeps its warm-up as it was: an object hidden for good would be drawn in a state its program
  table was never surveyed with). The `pipeline` debug command reports
  `post_prewarms` and `scene_prewarms`.
- A Backdrop with `environment` on is captured whole by its prewarm, its faces blended and filtered as the tree's
  environment (into the tree's own targets, the scene's environment untouched), and the scene drawn lit by it (every object
  shown, the gi part and the SSR G-buffers too, when the post prewarm turns SSR or SSGI on). Its first shown frame then draws
  only its share of the next capture and filters nothing, lighting the scene from the prewarm's capture at a share of
  0.001 until the next one finishes (`env_every` frames). The `backdrop` debug command reports `env_prewarms`.
- samples/style_switch: a third world, a night sea (Water3D, environment on) that fades in with the highway's SSR and
  `ssr_backdrop` turned on (given to `cw_post_prewarm` in init); debug field `sw.sea_renders`.

### Changed
- The built-in GRASS tuft (TerrainScatter3D `shape = GRASS`, `cw_scatter_def_builtin`) reads as grass from a play
  camera: 12 curved blades of three segments tapering from 3 cm to a point (6 wider straight ones in the far LOD)
  instead of five flat 10 cm triangles, a dark root to a lighter tip, a few dry blades, normals bent up, every
  triangle drawn both ways round with one normal so a blade is lit the same from either side, and a per-instance
  colour (`tint_jitter` 0.25 for GRASS, also set by `cw_scatter_def_builtin`). A GRASS layer draws new program
  states (`+icol`, and no `+ds`): a game with GRASS scatter regenerates its program table (`./programs.sh`).
- three.c v0.187.21: `alpha_to_coverage` reaches WebGPU pipelines (it was GL only).
- The tree's environment targets of its backdrops (cube, blend faces, filter and filtered target) are kept when no
  backdrop lights the scene any more, so the next one's first frame makes none (`CW_TEST_ENV_FREE_ON_RESTORE=1` frees
  them as before).

### Fixed
- A control map whose uv rotation or scale changes from one texel to the next drew a straight seam on the line where
  the nearest texel changes: a point took its rotation and scale from its nearest texel alone. Where the texels round
  a point disagree, each layer is now read once for each rotation and scale among them and the reads are blended by
  the bilinear weights, by height (the stones of one rotation stand over the joints of the other), across one texel,
  with the gradients rotated and scaled per read (no mip seam). Measured on a quarter turn every other 4 m block: the
  brightness step across a block's edge went from 3.5 times the step inside the blocks (2.4 from a play camera) to
  0.74 (0.77), and at most 1.06 anywhere across the blend.
- A style change that first shows a world lighting the scene and turns SSR on no longer hitches: gate
  `backdrop-look-prewarm` holds the sea's first showing with SSR to 3 ms of CPU over the usual frame (890M through
  romdev: GL +1.3 ms, WebGPU +1.6 ms; control `CW_TEST_NO_PREWARM=1`: +138 to +172 ms GL, +213 to +304 ms WebGPU) and its
  first two frames' GPU time to its second showing's plus 0.25 ms (GL 4.61 vs 6.88 ms, WebGPU 4.55 vs 6.18; control
  `CW_TEST_NO_ENV_PREWARM=1`, the first capture on the first shown frame: +0.91 ms GL, +1.57 ms WebGPU over it). A
  test's first frame, where every prewarm runs, is still the same picture pixel for pixel as without them. Test
  switches `CW_TEST_NO_ENV_PREWARM`, `CW_TEST_NO_ENV_SCENE_WARM`, `CW_TEST_NO_SCENE_PREWARM`.

## v0.70.1 (2026-11-23)

### Added
- SpringBone3D, a skeleton modifier for secondary motion (`cartwheel/ik.h`): hair, braids, ponytails, beards,
  tails, coat tails and robe panels on bone chains lag behind the body, sway and settle back to the animated pose.
  A child of the model like the other modifiers, run in child order after the animation. Under it,
  SpringBoneChain3D nodes (`root_bone`, `end_bone`, `stiffness`, `damping`, `gravity`, `gravity_strength`,
  `radius`; read-only `tip_offset` and `tip_distance`) and SpringBoneCollision3D spheres and capsules on bones
  (`bone`, `shape`, `radius`, `offset`, `tail`) that keep every chain out of the body. Each joint's tail is a
  Verlet point kept in world space from tick to tick; drag acts only on the motion apart from the animation, so a
  steady walk leaves no lag. `active`, `influence`, `collisions`; `teleport_distance` and `teleport_angle`: a
  model that jumps further in one tick carries its chains along as they hang, with no whip; `reset` and
  `cw_spring_bone3d_reset` start them again from the animated pose; read-only `resets`.
  `cw_spring_chain3d_joint_world` and `cw_spring_collision3d_world` for drawing and tests; the `ik` debug
  request lists each chain's tip offset. Arithmetic, sqrt and the `cartwheel/math.h` functions only, so the
  pose hash is the same bits on every host. About 3 us a chain in a release cart on the 890M laptop.
  Guide: Animation, "Hair, tails and coats: SpringBone3D".
- Sample `anim_spring`: a generated person in a coat with a ponytail and two coat tails
  (`samples/anim_spring/make-character.mjs`), body, shoulder, thigh and head shapes, walking, stopping, spinning
  three turns, teleporting and walking back. Gates `anim-spring` (every host and both backends) and
  `spring-bone`, natively and on romdev, GL and WebGPU, each check with a control that must fail: the tips lag
  12 to 20 cm while walking and settle within 1 cm 18 to 62 ticks after the stop (control: the modifier off, no
  lag); no joint enters a shape during the spin (control: collisions off, 8.5 cm inside); after a 6 m teleport
  every tip matches an identical twin's to the bit (control: the jump check off, 55 cm of whip); pose hashes per
  60-tick window equal across hosts and backends. `unit-ik` covers the node's settle, sag, teleport, reset and
  collision. Scenes without SpringBone3D (anim_ik, skeleton_attach, ragdoll) draw and hash byte for byte as before.
- Prewarm of backdrops and looks: a style change (a Backdrop world fading in from opacity 0, a look pair set) no
  longer builds what it draws on the frame it first appears. A scene app (`cw_scene_app`) prewarms every Backdrop of
  its tree and every look made so far at the end of its warm-up, in its last render under the loading screen (a run
  with no warm-up render, a test, in its first frame), and again at the end of each loading phase
  (`cw_loading_begin`): each world drawn once with its hidden root shown for that render and its picture blended at
  opacity 0, each look drawn alone and in a blend into the pass's own target at its own stage (with the dither tone
  options' small targets and programs, the half blocks' history). Nothing of it reaches the screen,
  and nothing costs anything a frame after it: opacity 0 and an unset look are still free. Measured on the 890M with
  a release cart through romdev, the worst frame of each first showing over the usual frame (4.3 ms), GL / WebGPU:
  a fire canyon fading in +40 / +61 ms of CPU before, +1.0 / +0.5 ms now; ASCII +9 / +35 before, +0.6 / +0.7 now;
  half blocks with a palette +3 / +7 before, +0.9 / +1.1 now; a one-bit Bayer dither at the DISPLAY stage +46 / +134
  before, +1.5 / +0.3 now; a stage in volumetric haze +16 / +44 before, +0.7 / +1.0 now.
- `cw_backdrop_prewarm(node)` and `cw_backdrop_prewarms` (Backdrop `prewarms`, read-only; the `backdrop` debug
  command reports it): a backdrop added, or a world built in code, after the warm-up is drawn once at the next frame
  whatever its opacity. An `environment` capture is still made on its first shown frame.
- `cw_look_prewarm(pipeline, look)`, `cw_look_prewarm_all(pipeline)` and `cw_pipeline_look_prewarms`
  (`cartwheel/look.h`): a look made later, or drawn by another pipeline (a SubViewport's), is drawn once at the next
  frame without being set.
- samples/style_switch: a note highway over two backdrop worlds (a fire canyon with flames, embers and haze; the
  backdrop sample's stage of volumetric spots) hidden at opacity 0, and three looks (ASCII, half blocks with a
  palette, a one-bit Bayer dither with the tone options at the DISPLAY stage), each shown on a schedule the first time
  and again; debug fields `sw.tick`, `sw.event`, `sw.fire_renders`, `sw.stage_renders`.
- Profiler zones for first-use work (`cartwheel/profile.h`): `pass.program_gl` (a pass program compiled and
  linked), `pass.pipeline_wgpu` (a pass's WebGPU pipeline made for a new target, blend or sample count),
  `look.program`, `look.ascii_prepare`, `look.ascii_atlas`, `look.prewarm`, `backdrop.world` and
  `backdrop.prewarm`.
- Gates `backdrop-look-prewarm` (style_switch's cart through romdev on GL and WebGPU, every frame timed: each change's
  frames at most 3 ms of CPU over the usual frame, a first showing's first two frames (on a quiet GPU) at most 1.5x
  the usual frame, or its second showing's worst or the three frames before it + 1.5 ms, of wall time; control
  `CW_TEST_NO_PREWARM=1`, the warm-up without the prewarm, must cost a first showing at least 8 ms of CPU; nothing of
  a prewarm is seen: a test's first frame, where it runs, is the same picture without it (control
  `CW_TEST_PREWARM_SHOW=1`: the worlds at opacity 1, the looks drawn after the pass's real run), and unseeded the
  frame whose render ran it shows the loading screen alone (control: a frame of play)) and `unit-looks` (the
  blue noise table, below).

- `CW_TEST_WARM_SLICES=N` (test switch, `src/scene/runner.c`): the warm-up's shader phase renders at least N slices a
  frame whatever the clock says; gate `warm-states` runs with 4, so a loaded machine (one slice spending the frame's
  12 ms budget) no longer leaves it with no later slice to check.
- Sample `fog_transparents` (a lit alpha-blended panel, an additive panel, additive flames and alpha-blended smoke on
  a plain in height fog, crossing the horizon) and gates `fog-transparents`, `fog-transparents-vfog`,
  `fog-transparents-split`, `fog-transparents-backdrop`, `fog-transparents-nofog` and `fog-transparents-checks`: every
  preset, volumetric fog, a split screen and a Backdrop, natively and as the dual cart on romdev, GL and WebGPU; an
  additive surface's light is its unfogged light times the fog's transmittance at its own distance, above and below
  the horizon alike, and an alpha-blended one is the background blended with its colour fogged at its distance.
  Controls: `CW_TEST_FOG_OVER_TRANSPARENTS=1` (the fog over everything by the depth behind it, as before) and
  `CW_TEST_FOG_NO_HFOG=1` (three.c materials without their own fog). The `hidden_vis` and `runtime_nodes` tables
  gained their transparent materials' fog states.
- Debug field `look.tone_small` (1 when the tone options ran in small targets). Sample `looks_onebit` takes
  `ONEBIT_KIND=diffuse` and `ONEBIT_XFADE=N` (a fade from the one-bit look into half blocks at SCENE and back with
  `ONEBIT_XREV=1`, `ONEBIT_XT=t` to hold it). Gate `looks-display` checks the tone once a dither pixel against the
  tone in every screen pixel (`CW_TEST_LOOK_TONE_EACH_PIXEL=1`; control `CW_TEST_LOOK_TONE_NO_MARGIN=1`), a fade
  across stages both ways with no step at either end (within 2/255 in every 32x32 tile; control
  `CW_TEST_LOOK_ONE_STAGE=1`, the old rule, steps by 5/255) and through the flash check, and the pair rule. Gate `looks` checks half blocks on a palette at DISPLAY under TAA over 17 frames (control
  `CW_TEST_HALFBLOCK_NO_HOLD=1`); sample `looks` takes `LOOK_STAGE=display`. Timing
  report `looks-tone-perf` (GPU ms before and after, GL and WebGPU, with the load and the GPU clock).- A start angle for particles: `angle` (radians) and `angle_random` (0..1, a share of a full turn centred on
  `angle`) on GPUParticles3D and GPUParticles2D. Billboards used to start at a random turn every time, so an upright
  flipbook (flame tongues, a rising wisp) came out tilted every way; with `angle_random 0` every particle starts at
  `angle`, and a camera-facing billboard at angle 0 stays upright on screen however the camera circles or tilts.
  GPUParticles3D turns counter-clockwise as the camera sees it (MESH: about each particle's own random axis;
  STRETCHED ignores it, its velocity sets the turn); GPUParticles2D turns clockwise with y down, as Node2D rotation.
  `spin` still adds on top. The defaults (`angle 0`, `angle_random 1`) draw exactly what they drew before, pixel
  for pixel (fx_particles, fx_particles2d, fx_explosion and fx_bursts, natively and on romdev, GL and WebGPU).
  Guide: Explosions and distortion, "Upright flames: the start angle".
- Sample `fx_particle_angle` (upright arrows from both node types while the camera circles) with gates
  `fx-particle-angle` (every host and both backends) and `particle-angle`: each arrow's turn measured from the
  picture, natively and on romdev, GL and WebGPU: `angle_random 0` keeps every arrow upright at three points of the
  camera's circle (control: `angle_random 1` fails it, 3D and 2D), `angle 0.7` turns every arrow by 0.7, `spin`
  turns every arrow by the same amount from its start angle (control: no spin turns none), and STRETCHED sparks at
  a set angle draw the default picture (control: a billboard system at that angle changes it).
### Fixed
- The skeleton modifiers' rest pose (LookAtModifier3D's `forward_axis` and `offset`, SpringBoneCollision3D's
  offsets) is the skin's bind pose, read from its inverse bind matrices, and no longer whatever the bones held when
  the modifiers first ran. A game that built a model, its AnimationTree and its modifiers and advanced the tree before
  the first modifier phase got its first animated frame as rest, and a look-at aimed 13 degrees of yaw and 15 of
  pitch off. Gate `ik-bind-rest`: a skinned head turned and pitched before the modifiers bind still aims at its
  target (0.000 rad; control `CW_TEST_IK_REST_CURRENT=1`, the old rest: 0.345 rad).
- runtime-nodes' SubViewport leg waits for each frame on Mesa's lavapipe alone (`CW_TEST_WGPU_SYNC_LAVAPIPE`, matched
  by the adapter's name): lavapipe faults in its own rasterizer there with frames in flight (from v0.67.0 on, never
  on SwiftShader or a GPU, never with the wait), and failed CI. An unsynced lavapipe run is still reported, not
  gated, so a Mesa that fixes it shows.
- The first frame any dither look drew (the Bayer pattern too) cost 46 ms of CPU in a cart: the looks' 64 x 64 blue
  noise tile was computed there (void and cluster). It is now a table the engine ships
  (`src/render/look_blue_noise.h`, made by `tools/gen-look-blue-noise.c`): the same bytes, so every dither and
  `cw_look_blue_noise()` draws what it drew; `unit_looks` runs the void and cluster again on every host (native, the
  cart on GL and WebGPU) and holds the table to it.
- Height fog fogged transparent and additive surfaces by the depth of the opaque scene behind them, not their own
  distance: the fog is a pass over the finished scene that reads the depth buffer, and a transparent surface writes no
  depth, so each of its pixels took the fog of whatever lay behind it. Flames standing on a plain 38 m away were crisp
  over the sky and washed into blurred blobs below the horizon, with a hard line along it, because each pixel there
  took the fog of ground 80 m and more away; lowering the density, moving the ground or turning soft particles off
  only moved the line. Now, while a fog draws (height fog or volumetric, in the main scene, a split screen's views or
  a Backdrop's world), the scene pass draws the opaque queue, the fog draws over the opaque picture, and the
  transparent queue (every transparent and transmissive three.c material, as the new EFFECTS pass `transparents`),
  the GPU particles and the trails draw after it, each fogged in its own shader at its own distance: an
  alpha-blended surface turns into the fog's light, an additive one fades toward nothing (light added through thick
  fog adds nothing), a multiplied one toward white. three.c materials take it through the new `hfog` shading part,
  which the engine puts on every transparent material of a scene with fog: new program states (`x...hfog`), so run
  `tools/programs.mjs` on a game with transparent materials in fog (a missing state is reported as always). In
  volumetric fog the particles and trails look the froxel grid up at their own distance; three.c's transparent
  materials take the fog's analytic medium lit by the grid's ambient light and its sun. A scene with fog and nothing
  transparent draws as before, byte for byte (fog_valley, fog_shafts and open_world at every preset, GL and WebGPU),
  and so does a scene with transparent surfaces and no fog. On Very Low and Low the fog runs as a pass of its own,
  rather than inside the tone mapping pass, while anything draws after it. Transparent surfaces drawn after the fog
  go into the resolved scene, so with MSAA their edges are smoothed by the antialiasing pass. GPU time on the
  Radeon 890M at 1280 x 720 (`samples/fog_transparents`, every pass, against the old order): GL +0.01 to +0.03 ms a
  frame, WebGPU within its noise.
- `tools/run-cart.mjs --host romdev` stepped a whole run in one call, so a cart loaded unseeded (`--warmup-spread`, as
  the cart survey's player runs load it) never got past its loading screen on GL: the shader phase waits on the
  driver's parallel shader compile, which finishes in real time, and one fast call never let those completions land
  (destructo and detexto sat at "Compiling shaders", `cw.loading` 21 of 200, for good). It now steps in calls of at
  most 150 frames (frame steps and pad-only input sequences alike, a held pad carried across calls), as a player's host
  and 150-frame steps reach the title; a seeded run's frames are byte for byte the same. Control
  `CW_TEST_RUNCART_ONE_STEP=1` (the one call, as before) stays at the loading screen.
- A dither look's tone options cost many times what they should when a dither pixel is larger than a screen pixel:
  each screen pixel worked its dither pixel's tone out again, so a dither pixel of n x n screen pixels took its mean
  colour n^2 times and the unsharp mask (`sharpen`) nine times that. The tone is now worked out once a dither pixel,
  in small targets (the brightness of every dither pixel the mask reaches, then the mask, gamma, contrast and the
  depth outlines, then the threshold in each screen pixel), for the ordered and the diffused dither; one dither pixel
  a screen pixel keeps the single pass (nothing to share there). GPU ms of the one-bit look at DISPLAY with every
  tone option at 1920 x 1080 on a Radeon 890M (`tests/looks-tone-perf.mjs`, release build, the median of 5
  interleaved rounds; clock 1650 MHz mean, never at the floor; load 3.5 to 4.3):

  | dither pixel (1080 line) | GL before | GL after | WebGPU before | WebGPU after | the mask's own cost, GL / WebGPU before -> after |
  |---|---|---|---|---|---|
  | 1 (output pixels) | 0.31 | 0.32 | 0.55 | 0.53 | unchanged (the same program) |
  | 2, Bayer | 0.48 | 0.19 | 1.22 | 0.25 | 0.22 / 0.93 -> under 0.01 |
  | 6 | 2.99 | 0.13 | 7.45 | 0.47 | 2.25 / 6.42 -> under 0.01 |

  The picture is the one the tone in every screen pixel drew: 0 to 0.007% of the pixels differ (a tone on its
  threshold the compiler rounds differently), and the look still matches the reference pass within 0.051% (bound
  0.2%). The look as a cart on romdev (GL, 890M) went from 0.62 to 0.27 ms over the plain frame.
- A blend between looks of two stages (`cw_pipeline_set_looks`: a dither at DISPLAY and half blocks at SCENE, say)
  was drawn at DISPLAY the whole way, so when it was over the SCENE look alone was drawn at SCENE (bloom and the tone
  curve after it) and the picture stepped in one frame (by up to 5/255 in a 32x32 tile in `looks_onebit`). Each look of a pair is now drawn at its own stage: a blend across stages runs at
  both, each stage blending its own look with the picture it gets by the pair's weight and mask, so it starts on the
  first look's own picture and ends on the second's, and setting the end look alone changes nothing on screen. The
  rule "a pair is drawn at DISPLAY when either look asks for it, whatever the weight" is gone; a pair of looks of one
  stage, and a look with the plain scene, draw as before. Debug field `look.stage` is 2 when both stages ran.
- Half blocks quantised to a palette or to levels flickered at the DISPLAY stage under TAA: the finished picture moves
  a little with the jitter, and a square whose mean sat on a palette boundary flipped colour (in `samples/looks`:
  up to 144 pixels in 4 frames of 17 at 1080p; at SCENE, TAA after the look smoothed it). A pixel now keeps last
  frame's palette colour while it stays within 0.03 display steps of the nearest, and a level while within 0.15 of a
  step past its middle, at either stage (history per view, as the ASCII glyph hold): a still camera holds still.

### Changed
- three.c v0.187.20 (`t3_renderer_render_transparent`: the transparent queue alone, over what the target holds).

## v0.67.0 (2026-11-23)

### Added
- One-bit edges for a game's own 2D drawing (cartwheel/render2d.h), the edges the game shell's two-colour menus use:
  - `cw_r2d_set_one_bit(on)`: later text, sprites and shapes of the pass draw each pixel half covered or more in its
    colour at full alpha and leave the rest untouched (no blended glyph rim, circle edge or scaled sprite edge; a
    colour's own alpha is dropped too; tilemaps unchanged). `cw_r2d_one_bit()` reads it. Off by default, off at
    `cw_r2d_begin`.
  - `cw_r2d_set_one_bit_paper(paper)`: while one-bit is on, every text outline and shadow (the style's own, and the
    player's Text background outline or panel) draws in the paper, opaque. {0,0,0,0} (the default, and at
    `cw_r2d_begin`) leaves their colours.
- `cw_prompt_set_two_colour(on, ink, paper)` (cartwheel/prompt.h): button prompts in two colours, every glyph's fill,
  legend and words in ink, its backing and the words' outline in paper, opaque, with one-bit edges on the prompt's
  own draws; stays until changed. `cw_prompt_two_colour()` reads it. The shell's two-colour menus use their own ink
  and paper and give the game's setting back when they close.
- Sample `r2d_onebit` (a HUD in two colours over paper and a one-bit picture: text with an outline and a shadow, plain
  text, text with prompts, prompt glyphs, shapes and a rotated sprite) and gates `r2d-one-bit` (natively and on romdev,
  GL and WebGPU, at 100% and 200% text: every pixel ink or paper with the Text background off, Outline and Panel;
  each line's ink within 8% of the blended picture's coverage; controls: the setters not called, a cut at 0.95
  coverage; the setters called off is the frame drawn without them byte for byte) and `r2d-onebit` (every host).
  Without the new calls every frame is byte for byte as before (`r2d_text`, `prompts` and the shell's two-colour
  screens checked against v0.65.1 on GL and WebGPU).

## v0.66.1 (2026-11-23)

### Fixed
- Saving a scene whose root is itself an instance of another scene (`[node name="World" instance=1]` at the top, as
  samples/runtime_nodes' world_decl.cwscene) wrote the instanced scene's own children again as plain nodes, so the
  saved file loaded with them twice (the editor's lossless check: "6 children became 11"). They are now compared
  against a fresh instance and written only where they differ, as below any other instance; the editor-scenes gate
  passes again.

## v0.66.0 (2026-11-23)

### Added
- Backdrop reflections (`cartwheel/backdrop.h`): a Backdrop's `environment` (off by default) makes its world the
  scene's image-based light, so glossy and rough surfaces reflect it. The world is captured as a cube from its
  camera's position by its own renderer after its picture (`env_resolution`, 128 texels a face, linear HDR;
  `env_every`, 4: the six faces over the first frames of the cycle, the filter in the last; the lower effects presets
  spread it wider), and the captures of every lighting backdrop are blended by the share of the picture each has (its
  opacity, what the backdrops in front of it leave, its exposure over the scene's) over the scene's own environment by
  the share of the background left, then prefiltered (three.js r187's PMREMGenerator) into the scene's environment when
  a capture finishes. A cross-fade fades the reflections with the pictures; with no backdrop lighting it the scene's
  own environment is back. Read-only `env_captures`; the `backdrop` debug command reports the captures and the tree's
  environment.
- `cw_post_settings.ssr_backdrop` (PostProcess `ssr_backdrop`, off by default): a screen-space reflection ray that
  finds nothing of the scene on screen goes on toward where its direction lands on screen and, when nothing of the
  scene stands in the way, takes the backdrops' pictures there (blended at half size, linear HDR), so a near mirror
  shows a backdrop's lasers and spots sharp. Weighted as a hit is, faded at the screen's edges; a hit takes the scene,
  else the backdrop, else only the environment's reflection in the lit image remains. Its own trace program: with it
  off the trace is the one it was.
- Water3D's planar reflection shows the backdrops where its mirrored view finds nothing of the scene (the backdrops'
  environment at its sharpest level as the reflection's background), so a sea in front of a stage backdrop mirrors the
  stage's lights.
- samples/backdrop: the lane a glossy dark road reflecting the stage's lamps (new emissive lamp heads) and laser and, as
  the sea fades in, the sunset; modes mirror, mirrorsea, mirrormix, mirrorsplit and water; switches BD_ENV, BD_SSR,
  BD_EVERY, BD_ARRAY, BD_HIDELANE.
- Gates backdrop-env, backdrop-env-split (one pass), backdrop-env-water and backdrop-env-checks
  (`tests/backdrop-env.mjs`, GL and WebGPU: the reflection, the cross-fade's 50/50 blend, the screen-space fallback,
  the water, split screen per view and in one pass, each with a control; with `--baseline` the frames with both off
  byte for byte the build before them); backdrop-checks also holds opacity 0 byte-identical to no backdrops with
  environment and ssr_backdrop on.

### Fixed
- GL: a backdrop whose world drew spot shadow tiles (volumetric fog's lights) left the viewport and scissor of the last
  tile behind, and the scene's first render of the frame, the G-buffer of its screen-space reflections, drew into that
  rectangle: the scene showed no screen-space reflections at all. The frame's viewport and scissor are put back after
  the worlds draw.

### Changed
- With environment and ssr_backdrop off (the defaults) frames are byte for byte as before (neither new pass runs), but
  for the GL fix above.

## v0.65.1 (2026-11-23)

### Fixed
- A Backdrop's or a SubViewport's world whose draws had no program in the app's tables went unreported as soon as
  another tree's list changed: the debug fields `cw.three.missing_states` and `cw.three.missing`, and the red banner,
  held only the last tree to change, so the gates and the cart survey (`tools/programs.mjs --carts`) never saw a
  world's skipped draws (water, particles or a mesh added to a world from code while the game runs, say) and the cart
  shipped drawing nothing there. They now hold every tree's states for the run; `cw_pipeline_missing_states` is still
  one tree's. A table that lacked such states gets them on the next survey.

### Added
- Sample `runtime_nodes` and gate `runtime-nodes`: a node of every type that owns render passes or per-frame GPU work
  (GPU particles with an attractor, a trail, screen and mesh decals, omni, spot and cascaded sun lights, height fog with
  a fog volume, a shockwave, an explosion, water, a reflection probe, a light probe grid, cloth) made with
  `cw_node_new` and added while the game runs draws the declared nodes' picture in the main tree, a Backdrop's world
  and a SubViewport's world (natively and as the dual cart on romdev, GL and WebGPU: no pixel more than 16/255 apart,
  every type's counter equal; control: nothing added); a world's skipped states are reported with the main tree's
  (control: `CW_TEST_MISSING_LAST_TREE=1`, the old last-tree list, loses them).

## v0.65.0 (2026-11-22)

### Added
- Two-colour menus for a game whose picture has two colours (a one-bit dither): `cw_shell_desc.two_colour` with `ink`
  and `paper` (cartwheel/shell.h). Every pixel the shell draws on the loading, title, pause, Settings, Graphics and
  join screens, their hint rows' button prompts and "Starting..." is then the ink or the paper, opaque:
  - no see-through dimming layer: the screens are a sheet of paper, or over a picture (`draw_background`, or the pause
    menu over play) the picture stays as it is and each line of text sits on a paper plate;
  - text has one-bit edges (a pixel is ink when the glyph covers half of it or more, which keeps every stroke at 100%
    and 200% text) and a thin paper outline in place of the dark one; marks, frames and button prompts are one-bit too,
    and a prompt's in-between tones (the dimmer second stick, a touch button's tint) go;
  - the selection shows by shape in ink: COLOUR draws as INVERSE (an ink plate, its text in paper), MARKER and
    UNDERLINE draw their mark in ink; the join screen's seats are paper in an ink frame, a taken seat's frame heavier.
- `cw_shell_set_colours(accent, selection_text, ink, paper)` and `cw_shell_set_two_colour(on)` change the menus from the
  next frame they draw (a game whose player picks another look in Settings); `cw_shell_two_colour()` reads the mode.
  Debug fields `shell.two_colour`, `shell.ink` and `shell.paper` (0xRRGGBB).
- `cw_quality_menu_style.two_colour`, `ink` and `paper` (cartwheel/quality_menu.h): the Graphics page in two colours
  for a game that draws it itself.
- Samples `shell_settings` and `shell_join` take `SHELL_TWO_COLOUR=1` (with `SHELL_INK`, `SHELL_PAPER`, `SHELL_TWO_BG=1`
  and `SHELL_SWITCH_AT`, `samples/common/shell_two_colour.h`); gates `shell-two-colour` (natively on GL and WebGPU at
  100% and 200% text: every pixel ink or paper on all five screens, on paper and over a one-bit picture; each line's
  ink within 8% of the blended text's coverage; a colour switch on the frame it is made; controls: the mode off, a cut
  at 0.95 coverage, the switch without the setter), `shell-two-colour-title` and `shell-two-colour-graphics` (every host).
  Without the mode the shell's frames are byte for byte as before.

## v0.64.1 (2026-11-22)

### Fixed
- A node hidden by an ancestor, or by its own `visible`, drew and ran anyway when its type draws outside the mesh path: a
  GPUParticles3D under a hidden Node3D, or with `visible` off itself, simulated and drew its flames. One rule now holds for every type, `cw_node_visible_in_tree`
  (cartwheel/scene.h): a node is shown when it and every Node3D, Node2D and CanvasLayer above it are visible. Hidden,
  it draws nothing and its per-frame work stops; shown again it goes on (the table in the scenes guide, "Visibility"):
  - GPUParticles3D: its clock stops (no step, GPU work or draw, `cw_particles_burst` ignored) and it resumes from the
    particles it had, spawning where the emitter is then; a hidden ParticleAttractor3D or ParticleCollider3D acts on
    nothing. GPUParticles2D: not stepped while hidden (the canvas already did not draw it), resumes as it was.
  - Trail3D keeps no samples while hidden and grows again from the node when shown.
  - Decal3D: a screen decal under a hidden parent drew (only its own `visible` was read); a mesh decal's pieces, which
    are children of the surfaces it was clipped onto, drew whatever the decal's parents said. Both hide now, and a
    hidden mesh decal is not clipped again until shown.
  - Lights: a hidden light already lit nothing (three.c and the clustered lights walk the hierarchy), but a light hidden
    by a parent still made its shadow map or cascades and lit the volumetric fog. It does neither now.
  - FogVolume3D, Shockwave3D (paused; `cw_shockwave_fire` ignored) and Explosion3D (paused; `cw_explosion_fire` and
    autostart wait) under a hidden parent no longer draw or run.
  - Water3D under a hidden parent made its reflection and refraction renders and its underwater pass, and stepped its
    ripples: none of it runs now (its waves still answer `cw_water_height_at`, Buoyancy3D still floats).
  - ReflectionProbe3D and LightProbeGrid3D under a hidden parent captured and shaded: they bind to no draw and capture
    nothing now, and go on where they were when shown.
  - Cloth3D and Rope3D meshes live in the scene's root, so a hidden parent never hid them: they hide now, are not
    stepped, and are rebuilt at rest in their frame when shown (a LINKS rope's bodies are physics and go on).
  - A SubViewport under a hidden parent rendered and ticked its own world: it does neither now, keeping its last picture.
  - A Backdrop under a hidden parent drew its world behind the scene and ticked it: it does neither now.
  - VisibleOnScreenNotifier3D / 2D under a hidden parent reported on screen: it is off screen now.
  - A MeshInstance3D under a hidden parent took part in the VisibilityManager's culling, queries and impostors (an
    impostor could draw it): it is left out now. Terrain3D already followed the rule; it uses the shared function now.
- The stats count what is hidden: `cw_particles_stats.hidden`, `cw_decal_stats.hidden`, `cw_water_stats.hidden`,
  `cw_gi_stats.hidden`. The `visible` property docs of Node3D and Node2D state the rule.
- Sample `hidden_vis` and gate `hidden-vis`: each type under a hidden Node3D (and a Node2D), natively and as the dual
  cart through romdev, GL and WebGPU: hidden draws the picture of the same scene without them, pixel for pixel, and
  moves none of their counters, and so does each node hidden by its own `visible` under shown parents; shown again
  they draw and run, the particles' and the shockwave's clocks resumed where they stopped. Its controls,
  `CW_TEST_HIDDEN_SELF_ONLY=1` (a node's own `visible` only) and `CW_TEST_HIDDEN_PARENTS_ONLY=1` (its ancestors
  only), each fail the picture and every type's counter. A visible tree draws as before, pixel for pixel.

## v0.64.0 (2026-11-22)

### Added
- Look stages (`cartwheel/look.h`): `cw_look_desc.stage`, `CW_LOOK_STAGE_SCENE` (the default, before bloom and the
  tone curve, as before) or `CW_LOOK_STAGE_DISPLAY`: the look drawn last, in the POST pass "looks_display" at order
  9900 (after the tone curve, the grade, anti-aliasing, the lens and a game's own POST passes; the HUD still after),
  on the finished picture in linear light, its colours shown as they are. A dither there puts exactly its two colours
  on screen. A pair is drawn at DISPLAY when either look of it asks for it, whatever the weight. In split screen it
  runs in each view, or once over the laid-out views after a shared post chain (without the views' depth there).
- Dither tone options (`cw_look_dither`), all off by default: `hue_value` (saturated colours by their value, keyed
  by a saturation smoothstep `hue_from`..`hue_to`), `grey_gain` (luminance times a gain, the highlights kept from
  `keep_from` to `keep_to`), `sharpen` and `sharpen_radius` (an unsharp mask), `outline`, `outline_from`,
  `outline_to` (depth outlines: the relative Laplacian of 1 / linear depth, taken as the brighter), `gamma` (before
  the contrast). A setting left at 0 is its default.
- `cw_look_dither.output_pixels`: `pixel` counts screen pixels instead of pixels of a 1080 line picture (1: one
  dither pixel per screen pixel at any resolution).
- CUSTOM looks get `cw_look_stage()`, `cw_look_depth(px)` (as the depth buffer holds it) and
  `cw_look_linear_depth(px)` (view-space distance), the same on GL and WebGPU, the engine's thresholds
  `cw_look_bayer(px)` and `cw_look_blue_noise(px)`, and `cw_look_to_display` / `cw_look_from_display`, in GLSL and WGSL.
- Shell selection styles (`cartwheel/shell.h`): `cw_shell_desc.selection`, `CW_SHELL_SELECT_COLOUR` (the default, as
  before), `_MARKER` (a caret before the row), `_UNDERLINE` (a bar under it), `_INVERSE` (a plate of the accent behind
  it, its text in `selection_text`), on the title, pause, Settings and Graphics lists and the join screen's taken
  seats; `cw_shell_set_selection` and `cw_shell_selection_style` at runtime; debug field `shell.selection`.
  `cw_quality_menu_style` gains `selection` and `selection_text` for the Graphics page drawn on its own.
- Sample `looks_onebit` (and `looks_onebit_720`): a one-bit look of the finished frame (DISPLAY, blue noise, every
  tone option, one dither pixel per screen pixel) beside the game pass it reproduces; on the same frame and
  thresholds the two differ in 0.05% of the pixels (the pass rounds its tone to a half float). It costs about 0.31 ms
  on GL and 0.50 ms on WebGPU at 1920x1080 on a Radeon 890M. `shell_settings` and `shell_join` take
  `SHELL_SELECTION`.
- Gates `looks-display` (the look against the reference pass within 0.2% of the pixels, two colours on screen, each
  tone option alone, output pixels, the stage rule, split screen, still frames, the flash check, each with a control
  that must fail), `shell-selection` (each style on every list, nothing else changed, COLOUR byte for byte the
  default), `look-onebit`, `shell-select-inverse`.

### Unchanged
- Without the new settings every frame is byte for byte the one before them: the `looks` sample (every kind, blends,
  split screen, the tour) and the shell samples on GL and WebGPU.

## v0.63.0 (2026-11-22)

### Added
- Water3D is lit by the scene's own light when the tree has no ProceduralSky: its new `sun` property (the path of a
  DirectionalLight3D), else the tree's first DirectionalLight3D. That light's colour times energy and its direction
  drive the glint, the light through the crests (`scatter`), the body's light, the caustics, the view from under the
  surface and how much shows through it (`see_light`, a scene light counting by its share of a ProceduralSky's full
  sun), so a night sea under a moon has its moon path and moonlit crests. A hidden light lights nothing.
  `cw_water_stats` gains `sun_light` (1: the last surface was lit this way). With a ProceduralSky the sky's sun lights
  the water as before, byte for byte.
- Sample `water_night` (a black background, no ambient light, a low moon, an additive backdrop of disc, halo, horizon
  glow and shore lights on a layer of its own) and gate `water-night`: native and romdev, GL and WebGPU, the moon path
  and the reflected backdrop at reflectivity 1, the background mirrored with reflection off, a split screen of two
  views in one pass, and a ProceduralSky scene byte-identical with the old paths forced. Its controls force each old
  path (`CW_TEST_WATER_SKY_ONLY=1`, `CW_TEST_WATER_REFL_LAYER0=1`, `CW_TEST_VIEWS_BG_CLEAR=1`) and must fail.

### Changed
- A Water3D in a scene with no ProceduralSky and a DirectionalLight3D now takes its look from that light (above):
  the body is lit by it instead of drawn at full light, and its glint and crest light show. Set the light's energy,
  or the water's `scatter` and `glint`, to tune it.

### Fixed
- Water3D's planar reflection and refraction draw what the view's camera draws: its `cull_layers`. They drew layer 0
  only, so a backdrop on a layer of its own (a moon disc, a sky gradient, horizon lights) was missing from the
  reflection and water at a high reflectivity drew black. Transparent and additive meshes were never skipped: they
  reflect, sorted as the main picture sorts them.
- Water3D with no planar reflection drawn (reflection off, or Low) and no environment to stand in for it (no
  ProceduralSky and no environment map) mirrors the scene's background colour (else the clear colour) instead of
  black.
- WebGPU split screen in one pass drawn view by view: a scene with a background colour (a WorldEnvironment, no sky
  texture) cleared the whole target for each view, so every view but the last was black. The first view clears to it
  and the later ones keep the views before them, as on GL.

## v0.62.0 (2026-11-22)

### Added
- Looks (`cartwheel/look.h`): post passes that restyle the 3D picture, two at a time, blended by weight.
  `cw_look_new(&desc)` (start from `cw_look_default(kind)`) makes a look of one of these kinds: `CW_LOOK_ASCII`
  (character cells matched to the shapes in them, glyph set, ramp and font of your own, neon push, a halo, a glyph
  hold), `CW_LOOK_HALFBLOCK` (upper half block cells: a top and a bottom colour per cell, each the scene's mean over its
  square, optionally quantised to levels or a palette), `CW_LOOK_DITHER_ORDERED` (one bit a pixel against 8x8 Bayer
  or blue noise) and `CW_LOOK_DITHER_DIFFUSE` (one bit a pixel by a few parallel direct binary search sweeps from a
  blue noise start: error diffusion's look without its scan-line shuffle), both in two colours of your choice, and
  `CW_LOOK_CUSTOM` (a GLSL and WGSL `look()` of your own with helpers and 16 parameters). `cw_look_set` changes one
  live, `cw_look_free` frees it.
- `cw_pipeline_set_looks(p, a, b, t, &how)`: the pair a pipeline draws and the weight from a to b, blended as a cross
  fade, a dissolve through a noise mask or a wipe along a direction with a soft edge (`cw_look_mix`); a null look is
  the plain scene. Both looks read the same scene colour (one "looks" POST pass at order 295, after automatic exposure,
  before bloom, so the looks glow); weight 0 and 1 draw one look alone. With no look set the pass does not run and the
  frame is byte for byte the one without looks.
- Grids are anchored to each view's pixels and every look is a function of the frame (plus the ASCII hold), so a
  still camera's frames do not change. Looks work per view in split screen and in the one-pass split screen.
- `cw_pipeline_set_look_boxes`: up to 16 HUD rectangles where the looks are dimmed or left out (the plain scene
  shows), so HUD text reads.
- `cw_pipeline_measure_looks` and `cw_pipeline_look_luminance`: each drawn look's mean luminance, reduced on the GPU and
  read back a frame later, to match brightness across a blend.
- Cost at 1920x1080 on a Radeon 890M, any preset: ASCII 0.24 to 0.41 ms on GL and 0.48 to 0.67 ms on WebGPU, half block
  0.12 to 0.21 / 0.40 to 0.52, the ordered dither 0.11 to 0.18 / 0.24 to 0.51, the diffused dither 0.61 to 0.77 / 0.82
  to 0.94, a custom look 0.09 to 0.18 / 0.17 to 0.23, a fade between two looks their sum and about 0.3 ms more.
- Sample `looks` (and `looks_perf` at 1920x1080); gates `looks` (each look against what it promises, both ends and the
  middle of every blend, still frames, split screen one pass against view by view, the flash check over a half-second
  blend, each with a control that must fail), `look-ascii`, `look-halfblock`, `look-dither`, `look-split`.

## v0.61.0 (2026-11-22)

### Added
- Backdrops (`cartwheel/backdrop.h`): a `Backdrop` node draws a whole 3D world of its own (`scene = "stage.cwscene"`:
  its own camera, lights and shadows, sky, height fog and volumetric fog, particles, water) behind the scene, in place
  of the scene's background, with an `opacity` (0 to 1) and an `order` (lower is further back), so two backdrops
  cross-fade while the scene stays on top. `cw_backdrop_set_opacity`, `cw_backdrop_opacity`, `cw_backdrop_set_order`,
  `cw_backdrop_tree`, `cw_backdrop_root`, `cw_backdrop_renders`, `cw_backdrop_last_cpu_ms`; the debug command
  `backdrop`.
- Each backdrop renders its world at the scene's full size into a linear HDR picture through its own pipeline (its
  shadows, sky, scene, effects, fog and its depth-reading post passes). The new "backdrop" pass (OPAQUE, before the
  scene pass) blends the pictures into the scene target over the scene's own background, and the scene pass keeps that
  colour and draws no background, so the scene's opaque, transparent and additive surfaces are exactly what they are
  over their own background, and the main post chain (bloom, exposure, tone curve, grade, antialiasing, lens) runs
  over the composite. A backdrop's exposure is kept (its picture scaled by its exposure over the scene's).
- Opacity 0 costs nothing: no culling, drawing, blending or allocation, and with every backdrop at 0 the frame is byte
  for byte the frame without them. In split screen a backdrop is one picture behind the whole screen, drawn once,
  each view showing its part (one-pass and view-by-view frames alike).
- Sample `backdrop` (a note highway over a stage in volumetric haze with three sweeping shadowed spot lights and
  fires, and a sea, cross-fading); gates `backdrop`, `backdrop-split` and `backdrop-checks` (`tests/backdrop.mjs`:
  the scene's pixels unchanged over a backdrop, the 50/50 blend at opacity 0.5 in linear light, opacity 0 free and
  byte-identical, the backdrop's volumetric fog built; each with a control that must fail). GPU cost at 1080p on a
  Radeon 890M for the stage and the sea cross-fading: 3.8 ms GL, 3.5 ms WebGPU at High for the whole frame (the
  scene alone 0.6), 1.2 / 1.3 at Very Low; the guide's "Backdrops" section has every preset.

## v0.60.0 (2026-11-22)

### Added
- The Settings screen takes rows of the game's own, after the shell's rows (after Controls, before Back), from the
  title and from the pause menu: `cw_shell_desc` gains `settings_items` (up to `CW_SHELL_SETTINGS_MAX`, eight,
  `cw_shell_setting` rows: `label`, `key`, `value` (the game's int, its value at init the default), `choices`,
  `count`), `settings_item_count` and `settings_changed(index, value, user)`. They draw, scroll, size with the
  text and take pads, keys, mouse and touch like the shell's rows: left and right step a choice and stop at the ends,
  accept or a tap steps it forward and goes round. `cw_shell_set_settings_items` replaces the rows at runtime.
- The rows are saved under their keys (slots `shell:<key>`, cartwheel/save.h) and loaded into the game's ints before
  its first frame: in the game's slot store with `game_save_slots`, else in a store of the shell's in the last
  `CW_SHELL_SETTINGS_SAVE_BYTES` (1024) of the save area. A saved choice is found by its label, else its index, else
  the row's default: new rows, removed rows and added, moved or renamed choices keep what they can.
- Sample `shell_settings` and gates `shell-settings-rows` (its control has no rows), `shell-settings-change` (its
  control picks the other row), `shell-settings-pause`, `shell-settings-none` (no rows: the old page), `shell-settings-touch`
  (its control taps an empty corner) and `shell-settings-save` (a restart keeps the choice and an out-of-range save
  falls back; its controls never save the rows, and leave the save unpatched); `unit-save` covers the migrations.
  A game with no rows gets the same Settings page, pixel for pixel.

## v0.59.1 (2026-11-22)

### Fixed
- The native SDK builds (Linux, Windows, macOS, Android) fetch Signalsmith Stretch and Signalsmith Linear, the headers
  the tempo stretcher (`cw_voice_set_tempo`) compiles against; the SDK workflow's dependency list lacked them.

## v0.59.0 (2026-11-22)

### Added
- Tempo with the pitch kept: `cw_voice_set_tempo(v, tempo)` plays a voice faster or slower in the same key (a time
  stretch), from 0.5 to 2; `cw_play_desc.tempo` starts a voice at a tempo. Samples and streams alike, with pitch
  still working on top. A voice at tempo `t` takes exactly `t` source frames per frame it plays (32.32 fixed point)
  and `cw_voice_frames_played` keeps counting source frames, so the song position (what a chart or a cue is timed in)
  stays right at any tempo.
- `cw_voices_set_tempo(voices, count, tempo)` changes up to `CW_AUDIO_TEMPO_GROUP` (16) voices on the same mixer
  block, one command, so the stems of one song started on the same frame stay locked to the sample.
- The stretcher is Signalsmith Stretch 1.4.0 (MIT, header-only C++, on Signalsmith Linear 0.6.4), vendored through
  `scripts/deps.lock` and listed in NOTICE. Its latency is hidden: a voice started with a tempo is pre-fed on its first
  block (it starts on its frame, from its offset), the clock counts what is heard (a change on a stretching voice is
  heard, and moves the clock, about 90 ms after the block that takes it), and a voice playing at tempo 1 that gets
  another tempo warms its stretcher up for 2048 mixer frames before cross-fading into it over one block (and back when
  it returns to exactly 1). About 0.1 ms of one core per 10 ms of sound per stretching stereo voice (five stems: 0.50
  ms native and 0.53 ms in a cart on a Ryzen AI 9 HX 370, 0.57 ms on a Steam Machine), spread evenly over the blocks;
  about 1 MB per voice slot the first time it stretches, allocated on the calling thread.
- Sample `audio_tempo`: five generated stems as streams, the tempo on left and right (or the shoulders) in 5% steps,
  beat lights from the stems' clock. Guide section "Tempo" in docs/guides/audio.md.
- Gates `unit-audio-tempo` (pitch within 2 cents at 0.75 and 1.5 while the length scales; control: the same speeds
  as pitch), `-lock` (five stems mix to exactly one voice's sound with equal clocks; control: changes a block apart),
  `-clock` (a click track heard within 128 source frames of the clock; control: frames played out), `-bypass` (tempo 1
  is bit for bit the old mix; control: the stretcher run at 1), `-clicks` (no step past 0.05 on a 440 Hz tone over nine
  changes; control: no warm up or cross-fade), and `audio-tempo` (the sample with a pad, GL and WebGPU).

### Changed
- A voice at tempo 1 (every voice before this release) mixes bit for bit as before: the existing audio gates'
  hashes are unchanged. Stream rings hold 7200 frames of history behind what plays (so a playing stream can start
  stretching), doubling each ring's memory (`stream_ring_frames` still sets how far ahead it decodes); decoding reads
  the same chunks as before.
- Interactive music (`cw_music`) plays its clips at tempo 1. A stretching voice's clock is exact on every host; its
  stretched samples are the same on every run of one build but not bit for bit across hosts.

## v0.58.0 (2026-11-21)

### Added
- The pause menu takes up to two items of the game's own, after Resume and before Settings: `cw_shell_desc` gains
  `pause_items[2]` (labels, NULL for none) and `pause_item(index, user)`. Choosing one returns to play as Resume does
  and then calls the hook with the item's index; the game switches its own screen (restart a level, pick another song).
  Gamepad, keyboard, mouse and touch all choose them. A game that sets none gets the same three-item menu, pixel for
  pixel. `cw_shell_set_pause_items` changes the items from the next time the menu opens.
- Sample `shell_pause` and gates `shell-pause-items` (order and index; its control chooses the other item),
  `shell-pause-items-wired` (its control leaves the hook unanswered), `shell-pause-leak` (the choosing press does not
  reach the game; its control presses again on the game's first tick), `shell-pause-none` (no items: the old menu; its
  control adds two) and `shell-pause-touch` (a tap on a game item; its control taps an empty corner).

### Fixed
- Resume no longer hands its press to the game. The tick Resume (or Back, or
  Start) left the pause menu on also ran gameplay, which saw the South or Start press as just pressed: a jump on South
  fired on resume. Gameplay now starts on the next tick, as it already did for the press that opened the menu.

## v0.57.7 (2026-11-21)

### Fixed
- The join screen centres its row of seats for any `max_players`. It spaced the seats a quarter of the canvas apart
  from the left edge whatever their number, so a two or three player game showed its seats bunched on the left under
  a centred "Join" heading and hints. Each seat keeps its quarter-canvas width; two or three now sit in the middle and
  four are drawn exactly as before (the same pixels on GL and WebGPU).

### Added
- `cw_shell_join_seat_rect(seat)` returns a seat's box on the join screen in 2D pass units, so a game's
  `draw_background` can put its own art where the seats are.
- The `shell_join` sample takes `SHELL_JOIN_SEATS=1` to `4` from the environment to show another seat count.
- Gate `unit-shell-seats`: the seat boxes are centred under the heading for two, three and four seats, and four are
  where they always were; its control puts them where the old layout did and must fail for two and three seats.

## v0.57.6 (2026-11-21)

Carries v0.57.5, v0.57.4 and v0.57.3, none of which published an SDK (their CI runs stopped on a flaky control or a share over its time limit).

### Fixed
- CI's GL gates are split over nine jobs instead of seven, from gate times measured again on the latest runs
  (`tests/ci-times.json`). Two gates new since the last measurement weighed only the 30 second default
  (`program-tables`, 36 minutes on the runner) or had grown (`views-masks`, 30 minutes with its spots cases), so one
  share ran past the job's 90 minute limit and was cancelled, stopping v0.57.3's and v0.57.5's runs. Each GL share now
  holds about 45 minutes of measured gates (it was up to 84 minutes); the WebGPU shares stay at six, the longest about
  40 minutes. Every gate is in exactly one share, before and after.

## v0.57.5 (2026-11-21)

Carries v0.57.4 and v0.57.3, which published no SDK (their CI runs stopped on this control).

### Fixed
- Gate `cart-version-order`'s control no longer depends on timing. It rewrote an asset under a cart repack and needed
  the rewrite to land mid-read, which CI missed (0 of 5 runs), failing v0.57.3's run and holding its release. The
  control now runs the gate's graph check over a generated cart tree shaped as before the fix (the version target with
  no edge to the asset pack), which it must name, and over the same tree with the edge, which it must pass: the same
  result on every run (ten local runs of each). The check of the real tree and the repack check are unchanged.

## v0.57.4 (2026-11-21)

### Fixed
- A split screen drawn in one pass cast the clustered spot and omni lights' shadows (the shadow atlas) from what view 0
  draws in every view: a view whose level of detail differs from view 0's (cw_object_set_views), and a caster the
  Shadows setting culls as small from view 0's camera but not from that view's, showed view 0's shadows. A
  two-player demolition derby under four shadowed floodlights differed from its per-view picture in up to 0.57% of a
  frame, all of it in the second view (a car's shadow and the dirt round it). Each view's lights pass now draws the
  atlas from that view's casters, as a frame drawn view by view does, and the tiles they change are drawn again for it;
  the sun's and three.c's own shadow maps stay drawn once a frame from view 0, as before. Recorded together
  (`pipeline views_submit 0`, one atlas for every view) a frame whose views cast differently is drawn view by view.
  The derby now differs from its per-view picture in at most 0.008% (arenas 1 and 2, two and four players, GL and
  WebGPU, 240 frames in play). Cost: each view whose casters differ draws the tiles again, as a frame drawn view by
  view does. On a Radeon 890M (GL, release cart, romdev, five interleaved rounds) the four-player derby's one pass
  went from 11.68 to 13.49 ms wall a frame (CPU 8.12 to 10.88, GPU 8.56 to 9.30), the same as its views drawn one by
  one (13.78); two players 11.53 to 11.94. The guide says how a game keeps one tile for every view (one unmasked
  caster standing in for its levels of detail).
- Gate `views-masks` gains spots: clustered spot lights with shadows over statues whose level of detail is chosen per
  view, a depth prepass, a decal and particles beside them, at two and four views; the per-view picture within 0.3%.
  Control: every view casting view 0's casters (CW_TEST_VIEWS_SHARED_CASTERS=1) fails it.

## v0.57.3 (2026-11-21)

### Fixed
- A game whose program table was generated before v0.57.0 drew its water wrong in a split screen drawn in one pass.
  Its water program has no view rectangle, so every view sampled its planar reflection and refraction at the uv of the
  whole picture (6.5% of a two-player frame differed from the per-view picture). A Water3D whose program lacks the view
  rectangle now draws the split screen view by view, and an error names the command that regenerates the table.
  Regenerated, the same frame matches the per-view picture within 0.002% on GL and WebGPU.

### Added
- Program tables are stamped with the extension sources they were generated from. `tools/programs.mjs` writes one hash
  per extension directory the table's states use into the generated C. The configure compares the stamp with the
  sources the build has: the engine tree's files, or an SDK's new `cmake/program-sources.txt`. It stops, with the
  regenerate command, when they differ or when a table has no stamp. Every later shader change to an extension is
  caught for every game that uses it. The configure names every out-of-date table at once, with one command for them
  all, which `tools/programs.mjs` runs a few tables at a time (several `--table`).
- A value an extension hook sets on a program that lacks it is no longer lost without a word (three.c 0.187.19's
  extension miss hook). It is reported once per program state and name: an error, the debug fields
  `cw.three.dropped` and `cw.three.dropped_values`, and `dropped_values` in the `pipeline` reply. Every gate fails on
  it, and so does a `tools/programs.mjs` survey.
- Gate `program-tables`: every table's stamp matches. Natively on GL and WebGPU, water, terrain and clustered lights drop
  no value at any preset, and a stale water program draws split screen view by view within 0.3% of the per-view
  picture. Controls: a changed water helper, a changed SDK hash and an unstamped table each fail the check, and a configure with two stale tables names both
  with one command for both; a value
  the program lacks is reported; the stale program drawn in one pass misses the bound.

### Changed
- Every program table in the engine (samples, tests, the editor's templates and tutorial project, the engine's own)
  is regenerated and stamped. The editor templates, the tutorial project and the agent test project had been
  generated before recent extension changes.
- three.c 0.187.19.

### Upgrading
- A game repository's configure stops on this version until each of its program tables is regenerated against it.
  The configure names every out-of-date or unstamped table in one run and prints one command that regenerates them
  all: `node deps/cartwheel-engine/tools/programs.mjs --table <manifest> --table <manifest> ...`. That command needs
  Node only, no GPU, and runs a few tables at once; each table takes about ten minutes, so a repository with a dozen
  tables takes about half an hour. Commit the regenerated `_gl.c` and `_wgpu.c`.
- `-DCW_PROGRAM_TABLE_CHECK=OFF` turns the check off. It is for an emergency build only, never the fix: a table it
  lets through still draws with programs older than the engine's shaders.

## v0.57.2 (2026-11-21)

v0.57.1 published no SDK either: `views-array` still took 69 minutes of a CI shard with only the heavy cases left
out, and the shard timed out at 90. This release carries v0.57.1 and v0.57.0.

### Fixed
- CI runs three `views-array` cases on its software GPU: two views, four views and four views over 4x MSAA, with their
  controls (each view's passes a view over; the last view's scissor left on). Every case still runs on a real GPU (the
  local gate run). The test now also takes CI's own switch (`CW_TEST_GPU=software`) as the sign it runs there.

## v0.57.1 (2026-11-21, no SDK)

v0.57.0 published no SDK (its release job timed out waiting on a CI shard that ran over an hour): this release carries
all of v0.57.0's changes, below, with these.

### Fixed
- **GL: a split screen in one pass is no longer slower than the views drawn on their own** on a host that waits for
  each frame (a wasmcart host under a libretro frontend, romdev). GL recorded every view before any reached the GPU,
  so the CPU and the GPU ran one after the other: the frame took longer than the per-view frame it replaced even when
  it cost less CPU and the same GPU time. GL now draws each view up to its opaque passes in turn and flushes it, as
  WebGPU submits it, so the GPU draws a view while the CPU records the next. The later views clear and draw inside
  their own rectangle (a scissor, turned off again before the passes after the views, whose MSAA resolve and depth
  copies are blits). The later views also no longer walk the scene's world matrices again (the first view's renders
  bring them up to date), on WebGPU too. `pipeline views_submit 0` records every view first, as before.
- The `views-wall` gate runs at two and four views and at four with a gun a view over 4x MSAA (the viewmodel pass),
  and on GL each view flushed as it ends must beat every view recorded first; its control is now 0.5 ms of CPU a view.
  v0.57.0 fails it (GL at four views: recorded together is the frame it drew; WebGPU with the guns: 3.43 against
  3.15 ms view by view); this release passes every case on GL and WebGPU (a Steam Machine: GL four views with the
  guns 2.65 against 4.25 ms). The `views-array` gate adds the scene multisampled 4x
  at two and four views (`SA_MSAA`), with a control that leaves the last view's scissor on
  (`CW_TEST_GL_VIEW_SCISSOR=1`, fails at 70% of the pixels). The `views-masks` gate compares the views sent to the
  GPU one by one with the views recorded together on GL as well as WebGPU.
- In CI, `views-array` leaves out the Ultra GI cases and the four-view water, distortion and prepass cases (the whole
  set took 71 minutes of one shard on CI's software GPU); not enough: see v0.57.2.

### Added
- `pipeline shared_post 0|1`: the debug request switches a split screen between one post chain over the views and a
  chain of each view's own (for A/B timing).

## v0.57.0 (2026-11-21)

### Added
- More passes take part in a split screen drawn in one pass (`cw_pipeline_set_views_array`), so a frame with them
  no longer goes back to drawing each view on its own:
  - reflection probes and the light probe grid (the `gi` pass): captured and updated once a frame, in the first
    view's run, as a frame drawn view by view does;
  - screen-space reflections and bounce light (`ssr`, `ssgi`): each view's G-buffer and half-resolution
    targets are its own, at half its size, and the passes read the frame's depth and colour through the view's
    rectangle, so one view draws the numbers it drew before;
  - the depth prepass (`depth_prepass`), through the same array camera as the scene;
  - distortion (`distort_sources`, `distort`): one buffer over the whole frame, each view's shockwave rings and
    heat haze particles drawn in its own rectangle, the shifted samples kept inside the view;
  - a lightmap bake (once, no view's).
- The `views-array` gate covers them: `samples/split_array` with a polished slab in a reflection probe's box and a
  light probe grid under SSR and SSGI (Ultra), with a depth prepass, with the pond's planar reflection and
  refraction (alone and under the probes), and with a shockwave and a heat haze, at two and four views on GL and WebGPU, one pass against each view drawn on its own within 0.3% of the pixels over 16 levels.
  Controls: each view's passes a view over (fails at 31% to 41%), and the frame without the `ssr`, `ssgi` or
  `distort` pass must differ from it. Every run must also draw every draw (no state missing from the program table).

### Changed
- Every built-in pass now says how it takes part in a one-pass split screen. Those that still send the frame back to
  each view on its own, and why, are listed in the rendering guide's "Split screen in one pass": `velocity`,
  `taa`, `motion_blur` and `dof` (each view's previous frame and history), `exposure` (each view adapts on its
  own), `water_under`, `terrainset` (a TerrainSet3D chooses its tiles from one camera), and a VisibilityManager's
  `impostors` and `occlusion_queries`.
- Apps whose programs include the SSR, SSGI or distortion programs need no new program states; `samples/split_array`'s
  table (`cw_samples`) gained the states of its new scenes.

## v0.56.2 (2026-11-21)

### Fixed
- CI no longer runs the `views-wall` gate: it times a release cart on romdev against the test GPU, which the CI runners do not have (it stays a local gate, like the other timing gates). v0.56.1 failed CI on it and published no SDK. Same engine as v0.56.0.

## v0.56.1 (2026-11-21)

### Fixed
- `CW_VIEWS_ALL` has a comment of its own, so `cw_object_set_views` keeps the comment above it and the API check (`tools/gen-api.mjs --check`) passes again; v0.56.0 failed that check in CI and published no SDK. Same engine as v0.56.0.

## v0.56.0 (2026-11-20)

### Added
- **What each split screen view draws** (`cw_object_set_views`, `cw_node3d_set_views`, `cw_object_views`,
  `CW_VIEWS_ALL`): an object and everything below it drawn only in the views whose bit is set. It says once a
  frame what a view hook that shows and hides did view by view (a player's own model close up only in its view, a
  level of detail chosen from each view's camera, an item one player collected gone from that player's view), and
  keeps the frame in one pass. Drawn view by view the masks hide and show each object before its view; the shadow
  maps are drawn from what view 0 draws, as before. Sample `split_views`, gate `views-masks` (the masks in one pass
  draw the picture the sample's view hook draws, GL and WebGPU, two and four views, High and Very Low; control: each
  mask the next view's).
- More passes take part in a split screen drawn in one pass: the viewmodel pass (each camera's `viewmodel_layers`
  in its view, 4x MSAA samples kept from view to view), a water surface's planar reflection and refraction (each
  view's own targets at its own size), a VisibilityManager culling to each view's frustum, distance and rooms. A view
  under a water surface that another view sees from above no longer sends a WebGPU frame back to the per-view runs.
- `vignette_per_view` (PostProcess, `cw_post_settings`): with a shared post chain the vignette, chromatic aberration
  and grain in each view's rectangle, as a chain of each view's own draws them, so a game whose look has a vignette
  per view can share its post chain.
- Gate `views-wall`: a split screen in one pass is never slower in wall time than the views drawn on their own on a
  host that waits for each frame (romdev's wasmcart host, GL and WebGPU; control: 0.15 ms of CPU a view more).

### Changed
- WebGPU: a split screen in one pass draws each view's shadow-phase passes, depth prepass and scene in turn and
  submits each view as it is recorded, so the GPU draws a view while the CPU prepares the next (before, nothing
  reached the GPU until every view was prepared). On a host that waits for each frame (a race at High on a Radeon 890M, wall ms a frame), two players 27.4 per view, 25.6 in one pass recorded together, 21.7 submitted view by view; four players 36.4, 34.5, 29.7 (`pipeline views_submit 0|1` switches it; the debug field
  `cw.views_submits` says how many views a frame submitted so).
- The water surface's program reads the view's rectangle (`waterView`): apps with water regenerate their program
  tables (tools/programs.mjs) when they take this release.
- The `pipeline` debug request reports `view_hook` and `view_masks`.

## v0.55.0 (2026-11-20)

### Added
- **Split screen in one pass** (`cw_pipeline_set_views_array`, on by default): with a shared post chain the views
  are drawn into one target, the scene in one three.c render through an array camera (each view culled and sorted
  by its own camera, one render pass, the frame's uploads once) and every per-view pass once per view inside its
  rectangle: clustered lights, terrain and its scatter (each view's own patches), the sun disk, decals, particles
  and trails, height fog, water, volumetric fog and SSAO. The picture is the one each view drawn on its own gives.
  In a four-player race on a Radeon 890M (High, 1920 x 1080) GL went from 12.7 to 10.0 ms of CPU and 7.1 to 6.4 ms
  of GPU a frame, WebGPU from 16.6 to 15.2 and 7.2 to 6.3. A frame with a game's own per-view pass, a view hook,
  the sun's cascades, a planar water reflection or refraction, or the views on both sides of a water surface is
  drawn view by view as before. `cw_pipeline_views_array_used` and the `pipeline` debug request
  (`views_array`, `views_array_why`; `pipeline views_array 0|1` switches it) say what a frame did.
- For passes: `cw_pass_desc.views` (`CW_PASS_VIEWS_NONE` / `EACH` / `ONCE`), `cw_pass_ctx.view_count`,
  `view_index`, `view_rect`, `cw_pass_view_uv` and `cw_pass_view_viewport`.
- Sample `split_array` and gate `views-array` (native GL and WebGPU, two and four views, Very Low, volumetric
  fog, the cascaded sun's fallback; control: each view's passes a view over).

### Changed
- In such a frame the geometry the engine draws for one view only (a terrain's patches and scatter) is on layers 28
  to 31: keep a game's own objects off those layers.
- The guides and headers describe the engine on its own terms; the porting guide is removed.

### Fixed
- Split screen with a shared post chain at Very Low and Low lost its height fog: it was fused into the tonemap pass,
  which runs over the laid-out views and drew no fog. It is drawn per view again.

## v0.54.0 (2026-11-20)

### Added
- **Randomized sounds** (`cw_randomizer`): several sounds, one picked per play,
  weighted with no repeats, weighted with repeats, or in order, with each play's pitch and volume varied within
  `random_pitch` and `random_volume_db`. Seeded, so the same calls pick the same sounds on every host.
- **Interactive music** (`cw_music`): clips with a beat grid (bpm, bar beats,
  length in beats, loop, auto advance to a next clip or back to a held one) and transitions between them (at once,
  on the next beat, the next bar or the clip's end; a cut, a fade in, a fade out or a cross fade over some beats;
  from the start or at the same position; through a filler clip; holding the clip left). Switches are on the mixer's
  frame clock, scheduled `CW_MUSIC_LEAD` (4096) frames ahead, and exact to the sample.
- **A sound's own loop points**: `cw_sound_desc.loop`, `loop_start`, `loop_end` and `cw_sound_set_loop`; every
  voice of the sound loops (an intro, then the loop), samples and streams alike (a stream's decoder seeks back to the
  loop start). A WAV file's sampler chunk (`smpl`) gives a sound its points. `cw_play_desc.once` plays such a sound
  through once.
- **Stream resources**: a `.cwaudio` text file holding a randomizer or an interactive music, named as an
  AudioStreamPlayer's `stream` (any of the three players). A randomizer player picks per play; an interactive
  player has its own music, a `clip` property that switches by the file's transitions, `current_clip`, and
  `cw_audio_player_music` for C.
- `cw_voice_stop_at` (end a voice on a mixer frame, to the sample, or fade it from there) and
  `cw_voice_started_at` (the mixer frame a voice really started on).
- Sample `audio_streams` and gates `audio-streams`, `audio-streams-random`, `audio-streams-loop` (the offline
  mixer, the same samples on every host): loop points as a sample and as a stream, the randomizer, and a calm to
  battle to calm music through a one-beat filler, each switch on its clip's grid. Controls: every switch at once,
  a randomizer that may repeat, the whole file looping instead of its points.

### Changed
- A WAV with a sampler chunk now loops by its chunk's points (none of the engine's or the example games' WAVs have
  one). `docs/guides/audio.md` covers the new parts.

## v0.53.5 (2026-11-20)

### Fixed
- **GL pass timers report passes that start late in a run.** With no display (SDL's offscreen driver, every test and
  tool run) or with vsync off, nothing paced the native loop, and the CPU queued frames 100 and more ahead of the GPU
  on the 890M. A pass's timer queries came back that late, so a pass that ran for fewer frames (`exposure`,
  `motion_blur` and `dof` in post_stack) never reported a GPU time on GL, while WebGPU did. The native loop now
  holds the GPU at most two frames behind the CPU (a fence after each swap, as a swap chain does). Native runs with
  no display now wait for the GPU like a windowed game, so their wall time includes it. Gate `pass-timing` checks
  dof, exposure and motion_blur on GL and WebGPU a few frames after they start; its control
  (`CW_TEST_GL_FRAMES_AHEAD=0`, the queue unbounded) leaves exposure untimed.
- **A version change no longer repacks a cart while its asset pack is being written.** After a version change the
  `<app>_version` step packs the existing cart again from the app's asset pack, and nothing ordered it after
  `<app>_pack`: when the same build rebuilt the packs (an engine pin move does), the packer could read an asset
  while cwpack rewrote it and stop with "file data stream has unexpected number of bytes" (a rebuild then passed).
  The version step now runs after the pack. New gate `cart-version-order` (in CI's carts job): every app with an
  asset pack is ordered so in the cart tree's graph, and a repack with still inputs carries the new version; its
  control, the same repack with an asset rewritten under it, fails with the packer's error.

## v0.53.4 (2026-11-20)

### Fixed
- **CI builds every app as a dev cart, in one job again.** The cart checks (every cart is dual, art, version,
  controls) read imports and manifests, which are the same in a dev cart (no ThinLTO, no wasm-opt), and building
  all 213 apps as release carts took 54 to 60 minutes. Four release carts (hello, plugin_demo, platformer3d,
  tiles2d) are still built as games ship them, with ThinLTO's cache kept between runs, and pass the dual check. This
  also ends v0.53.3's two shares, whose second share failed the art check (it needs hello and vertex_colors
  from one build).

## v0.53.3 (2026-11-20)

### Fixed
- **CI builds the carts in two shares.** One job building every app's cart took 54 minutes on v0.51.0, 57 on v0.52.0
  and passed its 60 minute limit on v0.53.2, so the run never went green and the release waited on it. The carts
  are split over two jobs (every other cart each); the share that builds plugin_demo checks its manifest controls.

## v0.53.2 (2026-11-20)

### Fixed
- **Gate `wgpu-post-taa`: its broken-velocity control runs with TAA's moving-pixel terms off.** Since v0.53.0 TAA
  takes the frame's unjittered picture wherever the motion it reads is wrong, so a broken velocity program alone
  costs only the history (mean 0.04 levels on post_synth's orbiting-camera segment) and passed the control. The
  control is now `CW_TEST_BREAK_PROGRAM=velocity` with `CW_TEST_TAA_STILL_ONLY=1`, where the history follows the
  broken velocity and the picture breaks. Measured on that camera pan (frames 92 to 96, against a 36-sample
  picture of each, GL and WebGPU alike): v0.53 TAA error 0.47 levels and frame-to-frame change of that error
  (shimmer) 0.68; v0.51's TAA 0.87 and 1.26; no TAA 0.59 and 1.06. Keeping the full history for the camera's own
  motion and weighing only motion against the camera was tried and is worse on the pan (0.61 and 0.88): a history
  resampled every frame softens and lags a pan as it does a mover.

## v0.53.1 (2026-11-20)

### Fixed
- **The first frame on GL is drawn again (it came out black since v0.52.0).** The tonemap pass made its neutral
  exposure state (the 1x1 texture it reads when automatic exposure is off) the first time it ran, after it had
  bound the frame to unit 0, and `cw_pass_texture_new` left unit 0 with no texture: the first frame tone mapped
  black, in every app on GL. The state is made before anything is bound, and `cw_pass_texture_new` now puts back
  the texture the active unit had, so a pass that makes a texture in the middle of its draw keeps its inputs. Found
  by `vis-city-check` (its culled frame against a reference drawn the same frame: the whole first frame apart,
  2073600 pixels; now 83 pixels at most, on one frame, as on v0.51.0).

## v0.53.0 (2026-11-20)

### Added

- **Gate `csm-specular`: the sun's highlight under cascades.** gi_room's chrome ball, the ceiling taken away, lit
  by the sun through three cascades and by the same sun with three.c's one shadow map fitted over the room, on GL
  and WebGPU: the highlight is there under cascades (brightest pixel 255 on both) and matches the one map's (mean
  3.3 levels on GL, 2.5 on WebGPU, bound 12). Control: with the ceiling in place the ball's highlight point lies
  in the ceiling's shadow, under cascades and under the fitted map alike (peak 193 and 199), and the presence check
  fails there. The room's default one map starts at the light and leaves the ceiling out of it, which is why that
  map showed a highlight the cascades did not: the cascades were right. gi_room takes `GI_SUN_CASCADES`,
  `GI_SUN_FIT` and `GI_NO_CEILING` for it.

### Fixed

- **TAA on things that move.** With object motion vectors a fast mover under TAA was still about twice as far
  from a 36-sample picture as no TAA at all. Three causes: where little history survives, the frame's sub-pixel
  jitter showed (a turning bar sat up to half a pixel off); a history resampled every frame softened what moves
  and lagged its shading; and the ground a mover uncovered still held the mover in its history. A moving pixel
  now takes the frame unjittered (a 4 x 4 Catmull-Rom at the pixel's centre in a tonemapped space, held inside
  its 2 x 2 neighbours), its history weight falls with its speed (to 0.25 by one pixel a frame), and a pixel whose
  history surface left (the last frame's motion there is not its own; the velocity pass keeps the last frame's
  target) drops its history. Still pixels are unchanged. Error over the mover (display levels, 640 x 360, GL and
  WebGPU within 0.02), before, after and with no TAA: post_stack's bar 3.40, 1.37, 1.70; a turning mesh 6.13,
  1.58, 2.17; a skinned character 6.55, 3.99, 4.02; an instanced swarm 5.29, 2.92, 3.43. `tests/motion-vectors.mjs`
  now also holds TAA at or under no TAA on every mover (control: TAA without the moving-pixel terms,
  `CW_TEST_TAA_STILL_ONLY=1`, fails it) and the still yard as sharp as before (`MV_REF=3`; control: every pixel
  taken as moving fails it). The TAA pass costs 0.04 to 0.07 ms more at post_stack's size on a Radeon 890M
  (GL 0.42 to 0.49 ms, WebGPU 0.41 to 0.45 ms on a still frame; 0.44 to 0.50 and 0.46 to 0.50 on the spinning bar).

### Changed

- **`samples/anim_retarget`: the four generated people are built athletic.** Narrower hips (the hip joints under the
  pelvis instead of outside it, the pelvis narrower, the waist a little wider), leaner thighs, and a heavy build
  (the lifter) carried in the shoulders and arms, its legs only a little thicker. Same generator
  (`samples/anim_retarget/make-characters.mjs`), same bones, names and rest poses; the walk and the punch retarget as
  before: planted feet slide 0.002 to 0.003 mm (gate bound 5 mm), the control (rotations only) 411 to 697 mm.

## v0.52.0 (2026-11-20)

### Added

**Scenes and nodes**

- **SubViewport: render a camera of the scene, or a scene of its own, into a texture, and show it on a mesh, a sprite
  or a UI control.** Security monitors, mirrors, minimaps, a portal's far side (`camera`: a Camera3D of the same
  scene, drawn by the whole pipeline after the frame's own picture, the way a later splitscreen view is); character
  portraits, a 3D model turning in a menu panel, an off-screen 2D canvas (`scene`: a .cwscene instanced into a world
  of its own, with its own cameras, lights, environment and canvas). `update_mode` is ALWAYS, WHEN_VISIBLE (only
  while something shows the texture), ONCE or DISABLED; `update_every` spreads the cost, and the Low and Very Low
  effects presets render every second and third due frame. Scenes name the texture as `"viewport://Name"` in any
  2D node's or UI control's `texture`, and as `viewport_texture` on a MeshInstance3D (emissive by default, or
  the colour map with `viewport_slot = COLOR`); C has `cw_subviewport_texture` and `cw_subviewport_t3_texture`.
  The main picture is what it was without the node. Cost on the 890M at 1080p in `samples/subviewport`: a
  512 x 288 camera view adds about 0.1 ms of GPU a frame, a 1920 x 1080 one about 1.4 ms (a light scene; the table
  per preset is in the guide). Guide: rendering-3d, "Rendering to a texture: SubViewport". Gates `subviewport`
  (five viewports, every mode, GL against WebGPU; its control freezes the monitor and fails) and
  `unit-subviewport`. Games add the new material states with `tools/programs.mjs` as for any new material.
- **MultiMeshInstance3D: many instances of one mesh from a buffer the game fills.** A forest, a field of rocks, a
  swarm: one node, with a MeshInstance3D child as the stamp (a primitive, a mesh resource or a glTF scene of several
  meshes). Fill it with `cw_multimesh_set_instance_count`, `cw_multimesh_set_transform` / `_set_color` /
  `_set_custom`, or write `cw_multimesh_transforms` in a loop and `cw_multimesh_touch`; `fill = GRID` fills a
  grid from the scene file alone. Instances are sorted into `chunk_size` cells, each culled as one draw against
  the camera and every shadow light; `chunk_size = 0` is one draw rewritten in place for a buffer that changes
  every tick. They take the scene's lights (forward and clustered), single-map, atlas and cascaded shadows and
  probes like any mesh. `visible_instance_count`, per-instance colour (`use_colors`), and
  `cw_multimesh_get_stats` (cells and instances in view, draws, rebuild time). 100000 boxes, 56000 in view, with
  sun shadows at 1080p on the 890M: about 0.7 ms (GL) and 1.0 ms (WebGPU) of GPU a frame. Guide: rendering-3d,
  "Many copies of one mesh". Gates `multimesh` (its control draws none of the field and fails) and
  `unit-multimesh`.

**Rendering**

- **Static batches (three.c 0.187.12, three.js r187's BatchedMesh).** `batch = true` on a `MeshInstance3D` draws the
  mesh through its tree's static batches: every batched mesh that shares a material (and its shadow flags) lives
  in one BatchedMesh, whatever its geometry, and the props in view that follow each other in its buffer are one
  draw. Automatic instancing only merges meshes that share a geometry; this is for the thousands of rocks, crates
  and bits of rubble that do not. Each prop is still frustum culled on its own, against the camera and each shadow
  pass's light; hiding, moving or freeing a node updates its prop. Primitives, mesh resources, glTF scenes and
  procedural meshes batch; transparent materials and meshes with a lightmap of their own stay out. A batched mesh
  gives up level of detail, occlusion culling, impostors and `visibility_range`.
  [cartwheel/static_batch.h](include/cartwheel/static_batch.h): `cw_static_batch_get_stats`.
  `samples/static_batch`: 5,000 boulders and crates, each a geometry of its own, in three materials under a
  cascaded sun. Measured on a Radeon 890M, 1920 x 1080, High, the release cart, three interleaved rounds of 110
  frames (machine load 36): the scene pass goes from about 3,200 draws to 4; a frame's CPU time from 57 ms to 14 ms
  on WebGPU and from 37 ms to 14 ms on GL; GPU time from 3.7 ms to 2.1 ms on WebGPU and from 2.8 ms to 2.5 ms on
  GL. The batches form the same way when the material carries the engine's shading parts: `static_batch_lit` is the
  same field under clustered lighting (sixteen omni lights), the cascaded sun and a probe grid (the props' material
  carries cluster, csm and gi): about 3,200 draws to 4, CPU 52 ms to 11 ms on WebGPU and 48 ms to 13 ms on GL, GPU
  time unchanged (2.6 ms WebGPU, 3.1 ms GL: with these lights the pixels cost more than the draws; machine load 12).
  Gates `static-batch` and `static-batch-lit` on GL and WebGPU (native and the cart); their control turns batching
  off and the state differs. Guide: [Thousands of different props: static batches](docs/guides/rendering-3d.md#thousands-of-different-props-static-batches).
  three.c's BatchedMesh feeds the instance id from a vertex attribute where r187 feeds it per draw, so a run of
  visible instances is one draw on both backends (r187 issues one draw an instance); the picture is r187's.
- **Automatic exposure (eye adaptation).** `PostProcess` `auto_exposure` makes the picture follow what it shows:
  step out of a cave and it is bright for a moment, then settles; step back in and the dark comes up more slowly.
  The frame's average log luminance is brought to `auto_exposure_key` (0.18), within `auto_exposure_min_ev` and
  `auto_exposure_max_ev` stops of the exposure you set (`exposure_ev` stays the compensation), at
  `auto_exposure_speed_up` toward a brighter picture and `auto_exposure_speed_down` toward a darker one. It
  always moves by a share of the way, never in a step: over a 30 times jump of the lamps in `samples/post_stack`
  the mean brightness never moves more than 6% a frame and never turns back, and `tools/flash-check.mjs` passes
  on GL and WebGPU. The adapted value stays on the GPU and the tonemap pass reads it there, so nothing is read
  back (no stall) and its clock is the frame clock: a deterministic run adapts the same on every host, and GL
  and WebGPU agree within 0.01% on every frame's mean. `cw_pipeline_exposure_reset` starts over at a cut. It
  runs on every graphics preset (a preset must not change how bright the picture is) and costs 0.09 to 0.15 ms
  of GPU time at 1920 x 1080 on a Radeon 890M. Guide: docs/guides/rendering-3d.md, "Automatic exposure". Gates
  `post-auto-exposure`, `post-auto-exposure-swell`, `post-auto-exposure-return` and the scripted
  `auto-exposure` (control: no adaptation must fail).
- **Motion blur.** `PostProcess` `motion_blur` smears what moved while the shutter was open, from the velocity
  target, so a fast pan, a spinning wheel, a swung arm and each box of an instanced swarm blur alike
  (`motion_blur_shutter` 0.5, `motion_blur_samples` 10, `motion_blur_max` 32 pixels at 1080 lines). A moving
  thing blurs past its outline over what stands still behind it; nothing still is blurred by what moves behind
  it. The UI and a first person viewmodel stay sharp: under a fast camera turn the viewmodel's pixels are within
  1 level of the unblurred frame (`tests/viewmodel-blur.mjs`, with a control that smears it). Off on the Very
  Low and Low presets, 0.6 of the samples on Medium, 1.5 times on Ultra, and off while the player's reduced
  motion setting is on (`cw_ui_set_reduced_motion`). 0.6 to 0.9 ms of GPU time at 1920 x 1080 on a Radeon 890M
  with a bar sweeping the frame. Guide: "Motion blur". Gates `post-motion-blur`, `post-motion-blur-reduced`,
  `viewmodel-blur`.
- **Glints.** `PostProcess` `glints` gives what is brighter than `glint_threshold` thin star rays, as a camera's
  star filter does: lamps, sparks, a gem's highlight, sun glitter. `glint_points` (4, 6 or 8 rays),
  `glint_length`, `glint_rotation`, `glint_intensity`. The rays keep the light's colour and are added in
  scene-linear light before the tone curve. Drawn at a quarter of the frame's size (two passes of 7 reads a ray);
  a one-pixel light does not flicker (the bright part is a 4 x 4 box of the frame), and a whole bright wall
  does not wash the frame out (what is over the threshold counts up to twice it). Off on the Very Low and Low
  presets; 0.52 to 0.60 ms of GPU time at 1920 x 1080 on a Radeon 890M with four rays. A game that drew its own sparkle sprites on highlights can use this instead. Guide:
  docs/guides/rendering-3d.md, "Glints". Gate `post-glints` on GL and WebGPU. Lens dirt is not in this release.

### Fixed

- **Reflection probes and the probe grid capture rooms lit by clustered lights and cascaded sun shadows as they
  are (three.c 0.187.18).** A capture's cube faces were shaded with the view's light data: the clustered lights' view
  space positions and froxel grid, and the sun's view space direction and cascade splits, all of the main camera.
  Every face got a wrong answer, and a different one on GL and WebGPU (the room's floor reflection under the chrome
  ball in `gi_room`: a dark disc on one, a light ring on the other, 3.9% of the frame apart). A capture is now lit
  with its own camera's light data, as r187 lights any render: the clustered lights in the face's view space, all of
  them in one cluster, and the sun's direction in it with every point in the widest cascade (three.c 0.187.18's
  `t3_extension_camera` tells the hooks which camera a draw is for). `gi_room` with clustered lights and three
  cascades (`GI_ONCE`): GL against WebGPU 0.000% captured every other frame and once (forward lighting with the
  sky: 0.000%), and the reflections' light matches FORWARD lighting's (0.000% GL, 0.001% WebGPU around the ball and
  the floor); the control, the view's light data in the captures (`CW_TEST_LIGHTING_NO_CAPTURE_MODE=1`), is 3.9% and
  2.7% apart and 72% off FORWARD's (tests/gi-probe.mjs). Cost: the gi pass on GL 0.62 to 0.63 ms against 0.58 to 0.65
  ms before (1920 x 1080, 890M, two interleaved rounds, load 9-10), the same within noise; with many lamps a face
  shades all of them.
- **three.c 0.187.18** also brings extensions reading an r187 `uniformArray` (a buffer binding no longer drawn
  silently wrong: refused when nothing fills it, filled by the hook when the extension names its elements), and
  auto-instanced meshes casting their shadows as one instanced draw (200 boxes: 4 draws a frame instead of 203).
- **Meshes auto-instance again once the last ReflectionProbe3D and LightProbeGrid3D leave the tree.** The gi shading
  part stayed on every material it had been put on, and a material carrying it is drawn one mesh at a time, so a
  level that unloaded its probes kept paying a draw per mesh. The part now comes off every material when the last
  probe or grid exits (a new one, SSR or SSGI put it back). `gi_probe_check` with `GI_LEAVE=1` adds 24 look-alike
  boxes and frees the probe: 9 draws after, 20 with the part kept (`CW_TEST_GI_KEEP_PART=1`, the control, which
  fails), on GL and WebGPU (tests/gi-probe.mjs).
- **A ReflectionProbe3D captured once (update_mode ONCE) no longer reflects a darker room on WebGPU under clustered
  lighting and cascaded sun shadows.** Its one capture is drawn in the first frame, with command buffers of its own,
  while that frame's cascades and clustered shadow atlas were still waiting in the frame's encoder: the capture went
  to the GPU first and sampled maps not drawn yet. The frame's encoder is now submitted before a probe or probe-grid
  capture. `gi_room` with `GI_ONCE=1` (clustered, three cascades, no sky): GL against WebGPU 2.70% of pixels over 16
  levels apart, within the 3.89% of the same scene with the probe captured every other frame; the old ordering
  (`CW_TEST_GI_NO_FLUSH=1`, the control) 5.19% (tests/gi-probe.mjs).
- **The forward-cascades check of tests/gi-probe.mjs has a control that fails again.** Its control (the cascaded
  sun's part keeping gi off) stopped changing anything once the gi part composed with that part, so it passed
  along with the check. The control is now `CW_TEST_GI_NO_PART=1`, no material gets the gi part: 6 of 6 ball
  checks wrong and gi.materials 0 on GL and WebGPU, while the check itself passes with 7 materials.
- **Play in the editor sees a game crash in romdev again.** romdev now reports a cart trap during a debug command
  after the tool's name (`wasm({op:'command'}): unreachable`), and the editor's romdev client read anything under
  that prefix as a failed request, so a crashed game looked alive (the editor-play gate failed 7 of 90 checks). The
  prefix is now taken off and the rest read: a WebAssembly trap's message is a crash in the old wording and the new.
  tests/remote.mjs runs tests/remote/classify_test.c (11 wordings) and its control, the old rule, which misreads 4;
  editor-play's romdev leg passes 90 of 90.
- **Restoring a Box2D world snapshot with no chain shapes, sensors or islands is no longer undefined behaviour**
  (`patches/box2d/0004`): it zeroed each of those arrays with `memset` even when empty, on a null pointer, which
  UBSan stops on (unit_net, under the sanitizer build). unit_net passes under ASan and UBSan with the patch (66
  checks) and stops at `snapshot.c:713` without it.

### Changed

- **three.c 0.187.18** (was 0.187.11): three.js r187's BatchedMesh (0.187.12), uniform arrays for extensions
  (0.187.13 to 0.187.15), ArrayCamera and a second renderer in one process drawing lit materials right (0.187.16),
  auto-instanced meshes casting their shadows as one instanced draw (0.187.17), and `t3_extension_camera` (0.187.18).
  Every program table is regenerated against it.
- The tonemap programs take their exposure as `u_tm_exposure` times the automatic exposure's state (a texture
  of zero stops when it is off): the same picture as before with automatic exposure off (`tests/post-fusion.mjs`
  and `tests/quality-verylow-post.mjs` pass unchanged).

## v0.51.0 (2026-11-19)

### Added

**Animation, skeletons and retargeting**

- **Skeleton3D: a skinned glTF model's bones as a node; BoneAttachment3D: a node that rides a bone.** A Skeleton3D
  under the model's node (or holding the model) gives bone count, names, parents, rest and pose transforms to read
  and set from C (`cw_skeleton3d_*`, `cartwheel/skeleton.h`) and over the debug channel (`skeleton <node> ...`),
  `show_rest_only`, a `cw_pose` for the solvers and ragdolls and a pose hash. It is a view of the bones glTF loaded,
  so bone tracks, AnimationTree, skeleton modifiers and ragdolls drive it and scenes without one are unchanged;
  modifiers may be its children and bone tracks may name it. A BoneAttachment3D takes its bone's transform each tick
  and again before each frame's draw, so a sword or a hat is drawn where the bone is drawn the same frame. Sample
  `skeleton_attach`; gates `skeleton-attach` (the sword held to the palm bone to 0.1 mm; its control lags one tick
  and fails) and `unit-skeleton`. Guide: [animation](docs/guides/animation.md#skeleton3d-and-boneattachment3d),
  "Skeleton3D and BoneAttachment3D".
- **An AnimationTree's blend graph is text in the scene file: `[animtree tree="..."]` sections.** One statement
  per line (`param`, `clip`, `blend1d`, `blend2d`, `point`, `blend2`, `add`, `layer`, `oneshot`, `timescale`,
  `machine`, `state`, `transition`, `filter`, `root`), nodes named and usable before they are defined, so an agent
  or a person writes a character's whole animation logic with no tool and no C. An attribute a statement does
  not know, a name that matches nothing or a machine without states is an error with the file and the line. A
  loaded scene saves byte for byte; a section that names a tree inside an instanced scene replaces that instance's
  graph and is saved only while it differs. From C: `cw_animtree_to_text`, `cw_animtree_load_text`,
  `cw_animtree_find` (a node's id by name), `cw_animtree_declare_param`, `cw_animtree_set_stored`; on the debug
  channel `animtree_text <tree>`, and `animtree_travel` takes a machine's name.
  [Guide](docs/guides/animation.md): "Blend graphs in scene files".
- **Animation layers with bone masks, one-shots and time scale.** `cw_animtree_layer`: the base plays in full and
  an overlay replaces it only on the bones it writes, so an upper body aims while the legs run (a `blend2` with
  the same mask fades every bone toward its rest pose; the unit test keeps that as the control).
  `cw_animtree_oneshot`: setting a parameter plays a clip once over the base with a fade in and out (an attack, a
  hit reaction). `cw_animtree_time_scale`. A mask name ending in `+` is the bone and every bone below it
  (`Spine+`).
- **Synced blend spaces and transitions.** `sync` on a blend space runs its clips through their cycles together,
  so a walk and a run of different lengths keep their feet in step while they blend; `sync` on a transition starts
  the new state where the old one was in its cycle. Controls: the same graphs without `sync`.
- **Retargeting: one set of clips for every humanoid (`cartwheel/retarget.h`).** A clip made for one skeleton plays
  on another with other proportions, another rest pose and other bone names. Bone maps fill themselves from the
  names of Mixamo, VRM, Unreal, Rigify and Biped rigs and plain names with a side, with a text override
  (`left_hand = Palm2L`). Rotations go across relative to the rest poses in model space (a T-pose and an A-pose
  line up), the hips travel by the ratio of the leg lengths, and each foot is put where the source's is by two-bone
  IK; `plant_feet` takes the slide out of planted feet. At import, a model's `.cwimport` names another model
  (`retarget.source`, `retarget.clips`, `retarget.plant`, `retarget.map.<slot>`) and `cwpack` bakes its clips into
  this one; at load, `cw_retarget_clip`; per frame, `cw_retarget_apply`. `cw_anim_ground_velocity` gives the speed
  to walk a character at. Sample `anim_retarget`: a robot's walk and punch on three generated people (1.86 m, 1.58 m
  and 1.78 m; Mixamo, VRM and Unreal names), the punch layered over the walk by a tree from scene text. Gate
  `anim-retarget`: a planted foot travels at most 2.7 mm over the ground in a stance (bound 5 mm; the robot's own
  clip slides 266 mm; without the planting pass 313 to 397 mm, without foot IK 58 to 67 mm, the gate's control),
  the cwpack bake and the clip made at load agree to 0.0003 mm, and the pose hashes match across hosts.
- **`cw_animtree_set_update_divisor`**: a far character's tree is evaluated on every Nth tick with the time since,
  as an AnimationPlayer already could.

**Gameplay nodes, sound players and saves**

- **Gameplay nodes a game no longer writes by hand** ([gameplay nodes](docs/guides/gameplay-nodes.md)). Each is a
  registered node type with reflected properties, a scene-file round trip and an entry in `docs/nodes.json`, and each
  is deterministic: it counts fixed ticks and reads the tree, never a clock.
  - `Timer`: `wait_time`, `one_shot`, `autostart`, `paused`, `time_left`, signal `timeout`, methods `start` and
    `stop`. It counts whole fixed ticks, and its live count travels in tree snapshots, so a replay and a rollback
    fire on the same tick. `cw_timer_start`, `cw_timer_stop`, `cw_timer_ticks_left`.
  - `Path3D` / `Path2D` with `PathFollow3D` / `PathFollow2D`: a Bezier curve as `points` text and a child that
    sits on it at `progress` (or `progress_ratio`), with `loop`, offsets, a rotation mode (`NONE`, `Y`, `XY`, `XYZ`,
    `ORIENTED`; `rotates` in 2D), a `speed` that moves it every tick and the signal `end_reached`.
  - The curve in C, with no node: `cw_curve_*` (`cartwheel/curve.h`): points with in and out handles and a tilt, a
    baked length, `cw_curve_sample_baked` by distance, `cw_curve_sample_frame` (forward, up and right carried along
    the curve without twisting, upside down through a loop, meeting at the seam of a closed curve),
    `cw_curve_closest_offset` and `cw_curve_closest_point`. Float arithmetic and the engine's own trigonometry in a
    fixed order: the same bits natively and in a cart.
  - `SpringArm3D`: a camera boom along the node's +Z that shortens when the 3D physics world is in the way
    (`spring_length`, `radius`, `margin`, `collision_mask`, `exclude_parent`, `extend_speed`). It never shakes.
  - `VisibleOnScreenNotifier3D` / `VisibleOnScreenNotifier2D`: `screen_entered` and `screen_exited` as a box comes
    into and leaves the current camera's view; `cw_view3d_sees_box` and `cw_view2d_sees_rect` are the tests alone.
- **Sound players as nodes** (`cartwheel/audio_nodes.h`, [audio](docs/guides/audio.md#players-in-a-scene)):
  `AudioStreamPlayer`, `AudioStreamPlayer2D` and `AudioStreamPlayer3D` over the existing mixer, with `stream`,
  `volume_db`, `pitch_scale`, `autoplay`, `loop`, `bus`, `max_polyphony`, `stream_paused`, `playing` and the signal
  `finished`. The 3D player feeds a spatial voice (attenuation, cone, doppler, reverb send, occlusion) from the
  node's world transform every fixed tick; the 2D player pans and fades by where the node is on the view.
  `AudioListener3D` is the ears (several for split screen); with none the current camera listens. `finished` is
  counted in fixed ticks from the sound's length, so it arrives on the same tick on every host. A paused tree holds
  the voices of the players it pauses.
- **What is inside an area.** `cw_area2d_overlapping_bodies`, `cw_area2d_overlapping_areas`, `cw_area2d_overlaps`
  and the `cw_area3d_` set list the bodies and areas inside an `Area2D` / `Area3D`, oldest first, in an order that
  is the same on every host and after a snapshot restore. `cw_physics_body2d_colliding_bodies` and the 3D twin list
  what touches a body with `contact_monitor` on. Methods `get_overlapping_bodies`, `get_overlapping_areas`,
  `overlaps` and `get_colliding_bodies` answer the debug channel.
- **Saves that survive a game update** ([saves](docs/guides/saves.md)).
  - The save store (`cartwheel/save.h`): named slots in a game's save data, each with a version, a size and its own
    checksum. `cw_save_read` always fills the caller's struct: a slot that grew or shrank keeps the bytes both have,
    an older version goes through a migrate hook, a damaged slot is dropped without taking the others with it.
    `cw_save_write`, `cw_save_erase`, `cw_save_info`, `cw_save_count`, `cw_save_name`, `cw_save_room`;
    `cw_save_app_store` over the app's own save data, `cw_shell_save_store` on the game shell
    (`cw_shell_desc.game_save_slots`). The store is plain bytes in the save data every app already has.
  - `cw_shell_desc.game_save_migrate` converts the game's struct from an older `game_save_version`, and
    `game_save_import` turns an existing struct into slots.
  - `cw_save_data` returns the app's save data and its size.
- `cw_canvas_viewport`: the canvas rectangle the last 2D frame drew.
- Sample `gameplay_nodes`: all of the above from one scene file. Gates `gameplay-nodes`, `unit-gameplay`,
  `unit-curve`, `unit-audio-nodes`, `unit-save`, each with a control that must fail.

**2D tile maps: collision, autotile, navigation, isometric and hexagon layouts**

- **Tile maps collide: `tile_shapes` on a TileMap2D and the new TileCollision2D node.** The tile set says what
  each tile id is made of, as text in tile pixels: `"1-4: full; 5: slope 0 16; 6: rect 0 8 16 8; 7: oneway;
  8: poly 0 0, 16 0, 8 16"` (whole cells, slopes by their floor height at each edge, rects, any simple polygon,
  one-way platforms). A cell's flips and transpose turn its shape with its picture. A `TileCollision2D` child of
  the map (a StaticBody2D: its layer, mask, friction and bounce are the body's) builds Box2D static geometry from
  the cells, per map layer and per chunk of 32x32 cells: the outline of the union of the tiles as chain shapes.
  Edges two cells share are removed, also where only part of an edge is shared (a half slope against a block), a
  straight run of any length is one segment, and a chunk's chains carry ghost vertices from the next chunk, so a
  body sliding along a floor never catches on a tile seam or a chunk seam. A 10x2 block of cells is one loop of
  four segments. `layers` picks the map layers that collide, so two TileCollision2D nodes over one map give two
  map layers their own physics layers and masks.
- **One-way platforms from tiles, and for CharacterBody2D.** A `oneway` tile becomes a chain segment that faces
  up on the physics layer `one_way_layer` names: rigid bodies rest on it from above and pass it from below.
  CharacterBody2D has `one_way_mask` (the layers whose shapes are one-way platforms for it: landed on from above,
  passed from below and from the side), `drop_through` (while set, a move falls through the platform it stands
  on) and read-only `on_one_way`; the 2D mover had these, the node did not.
- **Only the chunk of an edited cell is rebuilt.** `cw_tilemap2d_set_cell` rebuilds that cell's chunk (and the
  neighbour's when the cell is on a chunk's edge) before the next physics step, or at once with
  `cw_tile_collision2d_update`; a cells text, new shapes or a resize rebuild every chunk. A tree snapshot restored
  over an edit (rollback) gets the colliders of the snapshot's moment. On a 256x256 map with slopes all 64 chunks
  build in 7.9 ms natively and 13.8 ms in a cart (0.12 and 0.22 ms a chunk); gate `tiles2d-perf-report` holds a
  budget and the same chains on every host.
- **Custom data per tile.** `tile_data = "damage: 5=1, 6-7=2; sound: 1-4=stone, 9=wood"` gives tile ids named
  numbers or words; `cw_tilemap2d_tile_data`, `cw_tilemap2d_tile_data_text` and `cw_tilemap2d_cell_data` read
  them, and `cw_tile_collision2d_cell_at` turns a contact point and normal into the cell that was touched.
- **Queries without physics:** `cw_tilemap2d_cell_solid`, `cw_tilemap2d_point_solid` (exact against the shape,
  flips applied), `cw_tilemap2d_tile_shape`, `cw_tilemap2d_get_raw` / `set_raw` (a cell with its flip bits),
  `cw_tilemap2d_cell_size`, `cw_tilemap2d_cell_center`.
- Sample `tiles2d`: a side-scroller strip from one scene file (gentle and steep slopes, a cliff, half blocks,
  one-way planks, crates, a ledge removed in play). Gates `tiles2d` (GL and WebGPU, every host; the control
  never jumps and so never stands on a plank) and `unit-tiles2d` (outlines by segment count, a frictionless box
  sliding 60 cells, slopes, one-way platforms for characters and rigid bodies, rebuild counts, rollback). Guides:
  2D rendering ("Tile shapes and collision", "Custom tile data"), physics ("Tile maps").
- **2D navigation: grids, paths, flow fields and crowds (`cartwheel/nav2d.h`).** A `cw_navgrid` is a grid of
  cells with a cost each (0 blocks). `cw_navgrid_find_path` is A* with eight neighbours and no corner cutting,
  jump point search on grids where every walkable cell costs the same (the same shortest paths, a fraction of the
  cells visited), with the cell path pulled straight where its corners are in sight; `cw_navgrid_find_paths`
  answers a batch on the job pool; unreachable goals and exhausted budgets return the nearest cell reached and
  say partial. `cw_flowfield_build` gives every cell its distance to the goals and the neighbour to step to, for
  crowds heading to one place. `cw_crowd2d` mirrors the 3D crowd on a grid with the same ORCA solver. Circle
  and rectangle obstacles block and free cells. Costs are integers and ties break in a fixed order, so the same
  grid and request give the same path on every host and at any worker count.
- **NavigationRegion2D, NavigationAgent2D and NavigationObstacle2D**, with the 3D nodes' property and signal
  names. A region bakes its grid from a TileMap2D's `tile_shapes` (the cells with a solid tile on its `layers`;
  `agent_radius` for agents wider than a cell; `cost_data` for cells that are dearer to cross) and bakes map
  edits in per cell at the next tick: painting a wall or cutting a door changes the paths without a whole bake.
  An agent walks its parent Node2D to `target_position` (`path_changed`, `target_reached`,
  `velocity_computed`; `drive_parent` off for a body that moves itself), or follows the region's flow field
  (`follow_flow`, `cw_nav_region2d_set_flow_target`).
- Sample `tiles2d`'s room gains a region, four scouts patrolling between the chambers on paths of their own
  (they take the other doorway when one is painted shut) and ten followers on one flow field toward the hero.
  Gates: `unit-nav2d` (jump point search held to plain A* on 720 seeded pairs, flow distances held to path
  costs, crowds through a doorway, the nodes on a map; checksum pinned), `tiles2d-room` (crowd and field
  hashes on every host), `nav-determinism` now runs both at four pool sizes, `tiles2d-perf-report` times a
  256x256 map and holds every host to the same hashes: the navigation grid bakes in 3.6 ms natively and 4.2 ms
  in a cart, 1000 batched paths take 13.9 and 38 ms, a flow field 5.3 and 9.3 ms, a step of a 500-agent crowd
  0.23 and 0.31 ms (measured on a loaded machine, so upper bounds). Guide: navigation,
  "2D navigation: grids, tile maps and flow fields".
- **Terrains: tile maps that pick their own tiles.** `terrains` on a TileMap2D lists autotile sets as text:
  `"floor: single 12 x4; wall: blob47 16 x2 outside; moss: corner16 112; fence: edge16 128"`. The kinds are the
  47-tile blob set (sides and corners), 16 tiles matched by sides, 16 matched by corners (two-corner Wang tiles,
  the terrain living on the map's vertices) and a single tile that only varies; each set's tiles sit in a fixed
  order the header gives (`cw_tile_terrain_mask` hands it to an art tool). `cw_tilemap2d_set_terrain` paints a
  cell, picks its tile and fixes the cells round it; `cw_tilemap2d_set_terrain_rect` paints a rectangle;
  `cw_tilemap2d_fix_terrain` picks tiles again after raw cell writes (a generated level);
  `cw_tilemap2d_get_terrain` reads a cell's terrain. `xN` gives a set N variants, chosen by a `cw_rng` seeded
  from the terrain's seed, the layer and the cell, so a cell shows the same variant on every host and whatever
  order the map was painted in. `outside` makes cells past the map's edge count as the terrain.
- **`autotile` in a scene.** With it set, a cells text names a terrain by any of its tiles and every cell's tile
  is picked as the text is set, so a hand-written or generated scene never spells out edge tiles. A cell belongs
  to a terrain when its tile is one of the terrain's, so nothing is stored beside the cells and a saved scene
  loads to the same map. Painting goes through the cell edits a TileCollision2D follows: the colliders of the
  chunks touched are rebuilt.
- Sample `tiles2d` gains a room seen from above built only from terrains (Start swaps the scenes), with a
  doorway painted shut in play. Gate `tiles2d-room` (GL and WebGPU, every host; the control leaves the doorway
  open and the hero walks back through); `unit-tiles2d` covers the 47 masks, painting and erasing each kind,
  variants in any paint order, fixing twice, the scene form and collider rebuilds. Guide: 2D rendering,
  "Terrains (autotiling)".
- **Isometric and hexagon tile maps.** `layout` on a TileMap2D (`cw_tilemap_desc.layout` in the immediate API):
  `ISOMETRIC` diamonds, `HEX_ROWS` (pointy-top hexagons, odd rows shifted) and `HEX_COLUMNS` (flat-top, odd
  columns shifted), beside `SQUARE`. The chunk renderer places each cell's quad by the layout and culls chunks
  by the box of their quads; a square map's vertices are what they were, to the bit.
  `cw_tilemap2d_local_to_cell` finds the cell whose diamond or hexagon holds a point and
  `cw_tilemap2d_cell_center` its middle; `cw_tile_layout_cell_at` and `cw_tile_layout_cell_origin` are the same
  arithmetic without a map. These are flat layouts: a tile's picture fills its cell.
- **Collision and navigation follow the layout.** `full` in `tile_shapes` is the cell's diamond or hexagon, and
  a TileCollision2D joins neighbouring cells into one outline (two hexagons side by side: one loop of ten
  segments; six diamonds in a block: four). `cw_navgrid_desc.layout`, and a NavigationRegion2D on such a map:
  diamonds are searched as squares (jump point search, straightened paths, the crowd); hexagons have six
  neighbours, plain A* with the hexagon distance, paths through cell middles, flow fields and the crowd.
- Sample `tiles2d_layouts`: an island of each layout with collision outlines and walkers trading places
  through a wall's gap. Gates `tiles2d-layouts` (GL and WebGPU, every host), and layout cases in `unit-tiles2d`
  and `unit-nav2d`. Guides: 2D rendering ("Isometric and hexagon layouts"), navigation.

**Rendering**

- **Object motion vectors.** The velocity target held the camera's motion only, so under TAA a walking character
  smeared and the edges of anything moving broke up: its history was read from where the background had been. The
  velocity pass now draws every mesh that moved since the last frame over the camera's motion: a node that moved
  or turned, a skinned mesh whose bones moved, and an instanced mesh whose instance matrices were written, each
  placed where it was a frame ago (previous world matrix, bone matrices, instance matrices). Nothing is tracked,
  uploaded or drawn for a mesh that stood still, and nothing to turn on: any scene with TAA gets it. TAA also
  takes the motion of the nearest surface in each pixel's 3 x 3 neighbourhood, so a moving object's antialiased
  edge follows the object. Measured against a picture of the same frame rendered with 36 samples a pixel (mean
  error in display levels over the mover, 640 x 360, the same on GL and WebGPU): a fast-turning bar 3.4 with its
  own motion against 6.4 with the camera's alone, a skinned character 6.6 against 10.4, an instanced swarm 5.3
  against 8.4. The whole velocity pass takes 0.17 ms of GPU time at 1920 x 1080 on a Radeon 890M with three movers
  drawn, the same within noise as before. Blended materials, sprites, points and lines keep the camera's motion,
  and a first person viewmodel counts as still on the screen. `PostProcess` `debug_view = VELOCITY` shows it.
  New sample `samples/motion_vectors`; `samples/post_stack` gains three motion segments. Guide:
  docs/guides/rendering-3d.md, "Motion vectors". Gates `post-motion-taa`, `post-motion-plain`,
  `post-motion-velocity`, `motion-vectors-taa`, `motion-vectors-view` on GL and WebGPU, and the scripted
  `motion-vectors` (the measurement above; its control, camera motion only, must fail).
- **Toon shading (three.c 0.187.9, three.js r187's MeshToonMaterial).** A surface lit in flat tones with hard edges
  between them. In a scene it is one property: `MeshInstance3D` `shading = TOON` with `toon_bands` (2 to 16
  tones; below 2, three.js's own two soft-edged tones), on the node's primitive or on every material of its mesh
  resource or glTF scene (nodes showing the same file still share materials). From code,
  [cartwheel/toon.h](include/cartwheel/toon.h): `cw_toon_apply_tree` and `cw_toon_restore_tree` for a loaded
  model, `cw_toon_material` for one material, `cw_toon_ramp` for the shared ramps. A toon material keeps colour
  and colour map, emissive colour, map and intensity, normal map, ambient occlusion, light map, alpha map and
  cutout, vertex colours, transparency and sides; every light lights it with shadows (ambient, hemisphere,
  three.c's lights, the clustered omni and spot lights, the cascaded sun), each light banded on its own.
  `samples/toon_shading` shows banded spheres, an emissive cylinder and two toon trees from one glTF beside their
  physically based twins. Gate `toon-shading` on GL and WebGPU (native and the cart): its control sets every mesh
  back to STANDARD, and the state and the picture differ. Measured on a Radeon 890M, GL, 1920 x 1080, High, three
  interleaved rounds of 180 frames: the sample's frame takes 1.62 ms of GPU time toon shaded against 1.92 ms with
  the same meshes physically based. Guide:
  [Toon shading](docs/guides/rendering-3d.md#toon-shading).
- **IES light profiles (three.c 0.187.11, three.js r187's IESSpotLight and IESLoader).** `ies_profile` on a
  `SpotLight3D` or an `OmniLight3D` names an IES photometric file (`.ies`), and the light takes the fixture's
  measured shape: rings, hot spots, a street lamp's forward throw, a lantern's bars. The profile is read around the
  light's own axes (the tilt off its -Z, the twist from its +Y, so turning a lamp turns its light), replaces a spot
  light's cone and covers the whole sphere on an omni light; shadows, range and decay work as before. `cwpack`
  takes `.ies` files, checks each one when it packs it and stops with the reason on one it cannot read. Profiles
  with a single horizontal angle and with a measured range (mirrored to the circle as the file asks) both work.
  IES lights are drawn by the clustered lights: the scene's profiles sit in one atlas and, while a clustered light
  has one, lit materials draw with the new `clusteries` part in place of `cluster` (a scene without profiles
  keeps its programs and its cost). `cw_lighting_stats.ies` counts them. With FORWARD lighting a spot light's
  profile is three.c's own IESSpotLight; an omni light's needs CLUSTERED (a warning says so).
  `samples/ies_lights`: a ring downlight with a shadow, a street lamp turning about its axis, a lantern on an omni
  light, a plain spot light. Gate `ies-lights` on GL and WebGPU (native and the cart); its control takes the
  profiles away, and the state and the picture differ. Guide:
  [Real fixtures: IES profiles](docs/guides/rendering-3d.md#real-fixtures-ies-profiles).
- **A surface of your own that keeps the engine's lighting** ([surface.h](include/cartwheel/surface.h), guide
  "Writing your own surface" in docs/guides/rendering-3d.md). A game writes how one kind of surface looks (a
  dissolve, triplanar rock, grass leaning in the wind, a force field, lava) as a small three.js r187 TSL module,
  `shaders/<name>.mjs`, that exports `name`, `params` and `surface()` and returns only what it changes: `color`,
  `normal`, `roughness`, `metalness`, `emissive`, `ao`, `opacity`, `mask` (cut-outs, in the shadow too),
  `position` (vertices) and `output` (a last word on the lit colour). Everything that lights a surface stays the
  engine's: three.c's lights and shadows, the clustered lamps, the sun's cascades, reflection probes and the light
  probe grid, screen-space reflections, fog and decals. With its effect at zero a surface draws the pixels the
  plain material draws.
  - C side: `cw_surface_register` (the same parameter list: floats, vectors, colours, a 4x4 matrix, arrays of up
    to 32 vec4, textures), `cw_surface_apply` / `_apply_tree` / `_remove`, `cw_surface_set`, `_set_float`,
    `_set_at` (one array element), `_set_texture`, `cw_surface_set_global` (one value for every material of the
    surface: time, wind), `cw_surface_get`, `_unset`. A value is a memory write sent with each draw; a texture can
    be replaced, or its pixels changed in place (`t->version++`).
  - A surface can be a **node type** whose properties are its parameters (`node_type = "DissolveSurface3D"`): put
    one under a MeshInstance3D in a scene file and an AnimationPlayer keys it like any property.
  - Program tables: name the module in the manifest (`# extend: ../shaders/dissolve.mjs`); `tools/programs.mjs`
    captures the composed states the game draws (`standard+l0001+xcluster_csm_dissolve_gi`) like any others.
  - Meshes that share geometry and a material with a surface are one instanced draw wherever plain ones are.
  - `samples/custom_surface`: a dissolving chrome orb, a triplanar slab with moss and a sticker, forty swaying
    reeds among eight clustered lamps, three cascades, a reflection probe and a probe grid;
    `custom_surface_field`: two hundred reeds as one draw. Gates `custom-surface` (every host, GL and WebGPU) and
    `custom-surface-lighting` (plain materials against surfaces, pixel for pixel).
- `cw_shading_part_traits` / `CW_PART_SURFACE`, `CW_PART_MOVES_VERTICES` ([shading.h](include/cartwheel/shading.h)):
  a part a game wrote by hand can say it is a surface and get the same treatment. `cw_shading_part_everywhere`
  registers a part's hook for every renderer, present and future.

### Fixed

- **A material takes up to 8 shading parts and a 63 character composed name (was 4 and 31; three.c 0.187.10).** A
  game part on the Very Low preset with clustered lamps and cascades is five parts
  (`cluster_csm_plain_<part>_gi`): the fifth was refused without a word, so the game's part, or the probes, went
  missing on that preset (4.0 % of the sample's 1080p pixels were wrong on GL, 3.0 % on WebGPU; 0.000 % now). A material that still cannot take a
  part now says so in the log, once, naming the parts.
- **Reflection probes and the light probe grid light a game's surface on forward lighting.** A material that
  carried only a game's part was left out of the probes unless a lighting part happened to be on it too
  (4.9 % of the sample's pixels on GL, 3.6 % on WebGPU; 0.000 % now).
- **A mesh decal is clipped from a mesh whose material carries a surface** that leaves its vertices alone. It was
  refused for any extension but the engine's lighting parts, so a decal placed after the part was on the material
  never appeared.
- **Screen-space reflections and bounce light see a surface's vertices and cut-outs.** Their G-buffer replaced each
  material's extension with its own, so a dissolving or swaying mesh was reflected whole and still; the G-buffer
  program now keeps the surface (`<surface>_gibuf`).

### Changed

- **three.c 0.187.11** (was 0.187.8): three.js r187's MeshToonMaterial (0.187.9), extension names up to 63
  characters and up to 4096 of them, each with its own hook (0.187.10), and IESSpotLight with IESLoader (0.187.11).
  Every program table is regenerated against it.
- **A game on the shell whose save struct changes size keeps its players' progress.** Before, a save whose
  `game_save_size` differed from the build's was thrown away whole, settings included. Now the bytes both sizes have
  are kept and the rest reads zero, so a struct that grows at its end loses nothing; a different
  `game_save_version` with no migrate hook starts the game's data over and keeps the settings (volumes, graphics,
  text size). Existing saves load unchanged.
- **The 3D platformer's hero keeps its state machine in `scenes/player.cwscene`**, not in C. Its gates carry a new
  field, `anim.hash` (every pose the hero's tree wrote, folded each tick): the same with the graph built in C and
  loaded from the scene, on the input, auto and fall runs.
- **After upgrading, run your program survey** (`./programs.sh` / `node tools/programs.mjs <app>`): a material that
  needed more than four parts used to drop one silently and now asks for the state it should have had (a
  character with feathers and skin shading under the cascades on Very Low, say), and a game part registered as a
  surface asks for its `_gi` and `_gibuf` states.

### Measured

- 890M, 1920 x 1080, `samples/custom_surface`: a frame takes 1.4 to 2.4 ms of GPU time on GL and 1.2 to 2.3 ms on
  WebGPU from Very Low to Ultra; the three surfaces add between nothing and 0.14 ms against the same room with
  plain materials (medians of four interleaved rounds, a round varies by about 0.2 ms; load 23, GPU clock 998 MHz
  mean). `tests/custom-surface-perf.mjs` prints the table. Estimate for a Vega 8: under 0.6 ms.
- Program tables: the sample's table is 99 states with three surfaces across every lighting mode and preset (29
  for the same room with plain materials); its cart is 3.11 MB against 2.85 MB, under 4 KB a state. Raising the
  part limit adds no program by itself: a state exists only when a game draws it.
- AnimationTree: a tree that walks and layers a one-shot over it on a 22-bone character takes 5.7 to 10.9 us per
  character per tick natively on the 890M laptop (a hundred characters: 0.6 to 1.1 ms; the machine was under heavy
  load from other work).

## v0.50.4 (2026-11-18)

### Fixed
- **Distant ground no longer shows the sky through holes on Macs (three.c 0.187.8).** With a depth prepass on, the
  prepass and the colour pass computed the clip position a last bit apart on Metal (Chrome on macOS, native WebGPU
  there), and the colour pass failed its depth test on about half the pixels of far terrain: a racing game's
  mountains looked bright, blotchy and shiny in Chrome on a Mac while every Linux host drew them whole. Every vertex
  program now declares its position invariant (WebGPU `@invariant`, GL `invariant gl_Position`, for WebGL on a Mac
  through ANGLE). Found and proved on a macOS runner with an Apple GPU: the cart draws the mountains whole with
  this, with holes without it; three.c's test checks all 340 WebGPU and 340 GL vertex programs, with a control.
- **An Explosion3D added to the tree before its `scorch_texture` is set no longer logs "texture not found" for
  the default sheet.** It loaded its scorch sheet the moment it entered the tree, so a game that added the node
  and then named its own sheet loaded the default `scorch.png` first, and a game that does not pack that file
  logged `Decal3D: texture 'scorch.png' not found in the assets` (an error: two games' CI smoke tests failed on
  it). The sheet now goes into the decal atlas at the next pre-render, which the warm-up's shader phase runs too,
  so it still loads under the loading screen, and it is the sheet the node names by then. Proved on such a game:
  its cart built against this engine logs nothing; built against v0.50.3 it logs the error. Gate `fx-explosion-kit`
  passes on GL and WebGPU.

## v0.50.3 (2026-11-18)

### Fixed
- **CI runs the cart-controls gate where the carts are built.** It reads plugin_demo's packed cart, which the native
  gates job never builds, so v0.50.1 and v0.50.2 failed CI there and published no SDKs. The gate now runs in the
  carts job (`node tests/cart-controls.mjs --carts build/carts/carts`) and tests/ci.mjs leaves it out of the native
  shares. v0.50.1 and v0.50.2 have no prebuilt SDKs: use v0.50.3, which carries both.

## v0.50.2 (2026-11-18)

### Fixed
- **A game that loads its bindings from `input.cfg` gets its manifest `controls`.** Games that keep their bindings in
  a bindings file they load themselves (`cw_input_load_bindings_asset("input.cfg")`, `action = bindings` lines) and
  have no `input.` lines in a project shipped with no `controls` field. cw_add_app now also reads `input.cfg` beside
  the `PACK` list as the input map; a `controls = ...` key still wins, and a bindings file by another name needs
  one. `tools/cart-controls.mjs` takes `--bindings <file>`. tests/cart-controls.mjs checks a bindings file with
  inline comments and the override winning over it.

## v0.50.1 (2026-11-18)

### Fixed
- **A game whose input map is in its packed `project.cwproj` gets its manifest `controls`.** v0.50.0 read only the
  `project.cwproj` beside `CMakeLists.txt`, so a game with the usual layout (the export project at its root, the
  input map in `assets/project.cwproj` beside the `PACK` list) shipped with no `controls` field and the host fell back
  to its default pad. cw_add_app now reads both, the packed one first, so a `controls = ...` key at the root still
  wins; `tools/cart-controls.mjs` takes several projects. tests/cart-controls.mjs checks the split layout, the root
  override winning, and a control (the root project alone) that must not give the list.

## v0.50.0 (2026-11-18)

### Changed
- **In a cart the wasmcart host draws every on-screen pad control; the engine draws none and claims no touch.** A phone
  playing a cart used to show two sets of controls, the host's pad and the engine's overlay, each catching touches.
  Now one place draws the pad. In a cart (CW_CART) the touch controls, anchored (`cw_touch_add_thumbstick`,
  `cw_touch_add_round_button`) and rect (`cw_touch_add_button`, `cw_touch_add_stick`), never claim a touch, so every
  touch reaches the game as gameplay (the pointer with `touch_pointer`, and the raw touches); the overlay is never
  drawn and `cw_touch_overlay_visible` is false. Native builds (desktop, Android, iOS) keep the controls and the
  overlay as before. New query `bool cw_touch_controls_on(void)` (`cartwheel/input.h`): true natively, false in a
  cart. A game that draws its own touch controls (rect controls, or anchored ones marked `hidden`) draws them only
  while it is true. `CW_TEST_TOUCH_CONTROLS=1` keeps the controls on in a cart, for tests.
- **In a cart a touch player's button prompts show the host's pad buttons.** The engine's touch controls are not drawn
  there, so a prompt pointing at them (`cw_prompt_text`, `cw_prompt_action`) named a control the player could not see.
  For a player on touch in a cart the prompt is now the pad glyph of the action's pad binding (an action on pad:south
  draws the face-button diamond with south solid), the button the host draws. The device is still reported as touch.

### Added
- **Each cart's manifest says which pad parts the host draws: `controls`, worked out from the project's input map.**
  `cw_add_app` reads `project.cwproj` and passes the list to wasmcart-pack as `--controls`: every `pad:` and
  `axis:` binding of every `input.<action> = ...` line (south `a`, east `b`, west `x`, north `y`, the d-pad
  `dpad`, lx/ly `left_stick`, rx/ry `right_stick`, lt `left_trigger`, rt `right_trigger`, lshoulder `l`,
  rshoulder `r`, start `start`, back `select`, lstick `l3`, rstick `r3`), plus the d-pad, `a`, `b` and `start`
  the shell's menus and pause read. A project key `controls = a, b, start` replaces the list and `controls = none`
  writes an empty one (a game played by touch alone: the host draws no pad). A project without an input map leaves
  the field out. `cw_add_app(... CONTROLS ...)` gives the list for an app without a project, and
  `node tools/cart-controls.mjs project.cwproj` prints what a project gets. What games do: declare their pad through
  the input map (or `controls`), and gate any touch controls they draw themselves on `cw_touch_controls_on`.
- Gates `touch-host-pad` (romdev, GL and WebGPU: a finger on input_demo's round button and one in its thumbstick's
  zone reach the game for all 12 frames, the button, the stick and jump stay untouched, `cw.touch_overlay` 0; after a
  touch the prompts sample's jump hint draws the pad face with south solid; control: `CW_TEST_TOUCH_CONTROLS=1` fires
  the button and jump and brings the touch button glyph back) and `cart-controls` (the token rule, an override, `none` and no
  input map; plugin_demo's built cart carries its project's list; a pack with an override and with
  `none`; controls: a pack without `--controls`, and `none` handed to wasmcart-pack directly).

## v0.49.0 (2026-11-18)

### Added
- **A level loaded in play shows the loading screen instead of freezing the window: `cw_loading_begin(step, user)`**
  (`cartwheel/platform.h`). From the next frame the engine calls the step once a frame, before any tick, under the
  same loading screen as the warm-up (progress through `cw_loading_set`); a scene app then compiles the new scene's
  programs a slice a frame as the warm-up's shader phase does. No tick runs while it loads and its frames are not game
  time; a test run does the whole phase in one frame, so its ticks are the same as a level built in one go. A racing
  game measured on the Radeon 890M (WebGPU, 1080p) loaded a race in one 1.2 s frame followed by two 670 ms frames of
  shader compiles. Gate `loading-phase` (the `loading_phase` sample on romdev, GL and WebGPU): the screen shows for
  every step (14 and 15 frames for 12 steps), no tick runs while it loads, the bar never steps back, and a test run
  ends with the same 150 ticks as the level built in its tick. Controls: `LP_SYNC=1` (the level built in one frame)
  shows no loading frame; `CW_TEST_LOADING_NO_SHARE=1` steps the bar back.

### Fixed
- **The loading bar no longer steps back when a scene app's shader phase takes over from the app's warm-up.** The
  shader phase appended 101 parts to the app's total, so an app that reported 12 of 12 steps (a full bar) dropped to
  about half. The app's part is now the first half of the bar and the shader phase the second.
- **The shader phase no longer builds every remaining WebGPU pipeline in its last slice.** It showed the first object of
  each material and object type, then all the rest at once; a WebGPU pipeline is also keyed on the vertex layout, the
  shadow passes and the render state, so the pipelines the first objects did not cover were all built in that one
  frame (300 to 430 ms in a WebGPU cart, nothing on GL). The first-of-each key now includes each attribute's type and
  size and the shadow flags, and the rest come in doubling bites a slice. Gates `warm-states`, `warm-culled` and
  `loading-phase` pass on GL and WebGPU.
- **A slow shader-phase slice makes the next one smaller.** The slices grew while no new program appeared, so one could
  reach several new programs at once (a 226 ms slice in a game's stadium load); a slice over 40 ms now halves the next.
  A loading phase with no step (`cw_loading_begin(NULL, NULL)`, the shader phase alone) has the whole bar instead of
  starting at half.

## v0.48.0 (2026-11-18)

### Added
- **A game sets the 3D scene's share of the output itself, below the Resolution setting's 50% floor:
  `cw_quality_scene_scale(s)`** (`cartwheel/quality.h`, s from 0.1 to 1; 0 gives the scene back to the setting;
  `cw_quality_scene_scale_get` reads it). A look that wants a small scene on purpose (a stylized post effect that
  pixelates, posterizes or redraws the picture, a retro look) renders the scene and its post at that share and has it
  scaled up with the Resolution setting's filter, while the HUD and every overlay pass stay at full size. It replaces
  the setting while it is set; the player's Resolution choice stays in the menu and the save and applies again at 0.
  `cw_render_size` reports the scene's size. Measured on a game with such a look at 1920 x 1080 on the Radeon 890M,
  WebGPU, GPU time a frame: 4.38 and 4.33 ms at 100% (two runs), 1.79 ms at 50%.

### Changed
- **Every overlay pass draws at the output's full size when the scene renders at a share of it** (the Resolution
  setting, `cw_quality_scene_scale`, `pixel_scale` with crisp UI). The scene used to be scaled up only at the first
  2D window pass, so a game's own `CW_PHASE_OVERLAY` pass that ran before the 2D canvas pass (order below -100) drew
  into the reduced scene target: `ctx->output` was 960 x 540 at 50% of 1080p, and what it drew was scaled up with the
  scene. The scene is now scaled up as the overlay phase starts, on GL and WebGPU, so every overlay pass, raw pass
  code included (`cw_pass_bind_target(ctx, NULL)`, `cw_pass_fullscreen`), sees the full-size output in `ctx->output`,
  `ctx->width` and `ctx->height`. A custom output (`cw_pipeline_set_output`) and split-screen views keep their own
  sizes. Gate `overlay-full-res` (the `overlay_full_res` sample on romdev, GL and WebGPU, its scene at 25% of 1080p):
  an overlay pass at order -200 sees 1920 x 1080 and its one-pixel checker alternates on 100% of the neighbour pairs,
  and `cw_render_size` is 480 x 270. Controls: `CW_TEST_OVERLAY_SCALED=1` (the old scaled target) gives the pass
  480 x 270 and 0% of the pairs alternate; `scene_scale 0` gives back 1920 x 1080.

## v0.47.1 (2026-11-12)

### Fixed
- **v0.47.0's CI was red, so it has no release; this is the same water change with CI green.** The water-see-through
  gate drives romdev, which CI does not have, so it is now listed as local only (`tests/ci.mjs`, the testing guide).
  The water-default-look reference was drawn again: a crate standing at the waterline in `water_ripples` now shows
  through the surface, which moved 1.5% of the wake region and nothing else. The CI shares were balanced with water
  gates counted at the default time, which put them all in one job that ran past its 90 minutes; the times in
  `tests/ci-times.json` are measured again from the last runs, so the water gates spread over the jobs.

## v0.47.0 (2026-11-11)

### Added
- **Water shows what is just under its surface, blurred and fading with depth, by day and much less by night.** The
  surface was opaque to a hull that dips below it, a rider's legs or a post: a game that hid them from the refraction
  (the way to keep their ghost off the water) saw them vanish at the surface, and what the refraction did draw read
  as a copy at full strength to a metre or more. A Water3D with the refraction (High and Ultra) now draws a
  submerged object through the surface as cloudy water does: blurred by a radius that grows with its depth (about a
  tenth of it, five taps over a disc, each by its own depth), tinted toward the water's colour, red eaten first, and
  faded by `exp(-depth / see_through)` with the depth straight down from the surface. `see_through` (metres, 0.4 by
  default; 0 is the old look) is the new property: 0.4 m of water leaves a third of the object, 1 m under a tenth,
  1.5 m nothing. `see_night` (0.05) is the share that stays with no light: the sun's light reaching the water
  (smooth through dusk, no pop) and the sky's night light scale the rest (`cw_water_get_stats`: `see_light`).
  What counts as an object is a point much shallower than the ground two metres around it on the screen (the second
  deepest of three taps), so the floor, a slope, a quay or a hull wider than that keep the old look, and an object is
  never read as a shoal: its depth makes no shore colour and no shore foam (a deck seen through the water read as a
  shoal with a hard foam edge). The colour taken from the refraction is capped, since a lit float's very bright
  colour went through the red-eating absorption as lime patches. Same shader on GL and WebGPU (a frame differs
  from the other backend in under 0.0002% of its pixels by more than 16 levels). New sample `water_see_through`
  (five slabs 0.05, 0.2, 0.5, 1 and 2 m under the surface, a half-sunk box, a stern, legs, a lit float) and gate
  `water-see-through` on GL and WebGPU: the slab's difference from the open water beside it is at least 40 at 0.05
  m and 30 at 0.2 m, under a quarter of that at 1 m and an eighth at 2 m, no foam over a slab, a bright float
  not blown out, 0.2 m under a quarter at night, and the light reaching the water with no step over 0.45 across
  dusk. Controls that must fail: refraction off (opaque), `see_through 0` (the old look: 1 m still 33, foam over
  57% of the slab, the float blown out) and `see_night 1` (night as bright as day). Cost on the 890M at High
  (frame({op:'timing'}), GPU mean, 3 interleaved rounds of the before and after carts and the before cart twice as the A/A
  control, whose difference was under 0.03 ms): `water_boat` +0.29 ms on GL (1.83 to 2.12) and +0.23 ms on WebGPU (1.83
  to 2.05), `water_shore` +0.34 ms on GL (1.80 to 2.13) and +0.24 ms on WebGPU (1.80 to 2.04). Open water pays for three
  depth reads and nothing else; the blur and the picture are a branch taken by the pixels of an object only (a first
  form, nine taps and four ground taps for every pixel, cost 1.2 to 1.7 ms). The effect is in the High and Ultra presets
  only, as the refraction is. Other water scenes: `water_ripples` and `water_open_sea` draw the same
  pixels as before; in `water_boat` and `water_shore` only what is under the surface changes (crates, hulls, pier
  legs: under 1.4% of the frame by more than 16 levels).
- **A game that hid its craft from the refraction can stop** (`cw_water_refraction_hide`, still 16 nodes): with
  `see_through` on the ghost of a hull part above the real surface is faint, and the part below it shows.

### Changed
- Games link a new program table: the water's surface program changed. Regenerate it (`node tools/programs.mjs`,
  or `./programs.sh` in a project) after taking this release.

## v0.46.3 (2026-11-10)

### Fixed
- **Gate scripts with keys run through romdev; run-cart sends them as the cart's keyboard.** A gate whose input
  script held keys (`input-keys`, `action2d-keys`) skipped romdev, so a game's keyboard bindings were checked natively
  and in Chromium only, and a game that runs its checks on romdev alone (the starter's `./test.sh`) could not check
  them at all. `tools/run-cart.mjs` now sends a script's keys as SDL scancodes on romdev's pad sets and input sequence
  steps, where a real host's keyboard delivers them (key down and up events with modifiers, the held mask every
  frame); a cart that never asked for a keyboard (`WC_FLAG_KEYBOARD`) fails the run, naming it, and so does an older
  romdev (before 0.155.0) that drops the keys. `input-keys` and `action2d-keys` run through romdev on GL and WebGPU
  in the same requests as the pad script of the same play (`input-keys`: 5 for its 62 frames, the keys on one input
  sequence; `action2d-keys`: 1288, as `action2d`, since its mouse entries take one each); `action2d-keys` reaches the
  pad run's exact state (`g.score` 3530, `g.state` 6) and both controls still differ (32% of pixels, `g.score` 50). Scripts
  with `text` entries still skip romdev: it has no text op.
- **The romdev mode's control ran the gate's own start requests.** `tests/gate.mjs --hosts romdev` gave a gate's
  control the gate's `startCmds` when it had no `controlStartCmds`, so the control of a gate whose control is the same
  frames without its requests (`fx-camera-fade`, `quality-scope-ssao`) was the gate's run itself and CONTROL PASSED
  on romdev GL and WebGPU since v0.40.0. The control gets `controlStartCmds` alone, as the native legs do; both gates
  pass through romdev with their controls differing (1.3% and 3.3% of pixels).

## v0.46.2 (2026-11-10)

### Fixed
- **The first image into the decal atlas cost its frame 13 to 21 ms on WebGPU.** A Decal3D's image went into the
  shared 2048 x 2048 atlas at the decal's first draw, and on WebGPU the whole atlas's mips 1 and 2 were rebuilt on
  the CPU then (4M texels box filtered in the cart) and uploaded; GL did it with glGenerateMipmap (about 4 ms
  natively, 10 ms and more through a cart host). A game whose first scorch mark landed mid-play paid it in that
  frame, and a near-clear decal with the image drawn at load was the workaround. Now every cell of the atlas (an
  image with its 4 pixel border) sits on the mip 2 grid, and its three levels are built from the cell alone when it
  is uploaded: a new image touches nothing else and nothing is rebuilt at draw time, on GL and WebGPU alike (GL no
  longer calls glGenerateMipmap on the atlas; WebGPU keeps no 16 MB CPU copy of it). And a screen-space decal's
  image is loaded when the decal gets it in the tree or enters the tree with it, not at its first draw, so decals
  set up before play load under the loading screen with everything else; the explosion kit puts its scorch sheet in
  as the Explosion3D enters the tree (or gets `scorch_texture` or `scorch_size` there). [cw_decal_preload](api:cw_decal_preload)
  does it ahead of time for any image a game stamps in play; a decal stamped with an image the atlas has not seen
  costs that image's decode in its frame (about 2 ms for a 128 x 128 PNG in a cart) and nothing more.
  `cw_decal_stats.atlas_load_ms` and `atlas_decode_ms` say what the last image cost. Through romdev on the test
  GPU (`samples/decal_late`: a decal with the atlas's first image stamped at tick 60, the stamp's frame against the
  run's usual frame): +0.2 ms of CPU on GL and WebGPU with the image preloaded, +2 to 3 ms when the stamp decodes
  it; the old path, kept as the gate's control under `CW_TEST_DECAL_FULL_MIPS=1`, +8 to 12 ms on GL and +13 to
  17 ms on WebGPU. `fx_decals` draws the same picture as before (WebGPU identical; GL within 2 levels on the
  minified splats, which glGenerateMipmap filtered). Gate `decal-first-image` (romdev, GL and WebGPU, every frame
  timed with `frame({op:'timing'})`, a frame's cost its least over three runs; its second control
  `CW_TEST_DECAL_NO_MIPS=1` leaves a new cell's mips empty and must lose the far mark, which samples mip 2).

## v0.46.1 (2026-11-10)

### Fixed
- **A physics step uses at most 12 threads by default.** A Box3D or Box2D world made with `workers = 0` (the default,
  `cw_world3d_def`, `cw_world2d_def`, `cw_world2d_set_workers`, `cw_box3d_tasks`, `cw_box2d_tasks`) was handed
  every job-pool thread, and past about 12 a step gets no faster and, with a frame's other jobs on the pool, slower:
  6000 awake Box3D bodies in a cart on a 24-thread machine stepped in 5.8 ms on 12 threads and 5.35 on 24, and a blast
  under load took 6.2 to 7.1 ms on 24 against 4.0 to 4.2 on 12. The default is now the pool, at most
  `CW_PHYSICS_DEFAULT_MAX_WORKERS` (12); a count of your own still overrides it, up to the whole pool.
  `cw_physics_workers` says what a count resolves to. The cap is a fixed count because a cart sees only the host's
  hardware threads (logical CPUs) and cannot learn its physical cores. The new `physics_workers` sample steps Box3D's
  and Box2D's large pyramids both ways on the host's own pool (through romdev on a busy 24-thread machine: Box3D 3.6
  to 5.3 ms per step on the default against 4.7 to 7.7 on every thread, Box2D 1.3 to 3.0 against 2.7 to 4.8), and the
  `physics-workers` gate checks the counts and that both ways give the same bits ([physics](docs/guides/physics.md)).

## v0.46.0 (2026-11-10)

### Added
- **The flash check runs through romdev.** `tools/flash-check.mjs --host romdev` (the default for `--app`) checks a
  game's built dual cart on GL or WebGPU (`--gpu`) with no native run: `tools/run-cart.mjs --flash-from K` loads the
  cart and runs the start requests, environment and input script to frame K-1, then one romdev
  `frame({op:'flash'})` call steps frames K to N, filters every frame as a native dump is (a box in linear light, by 8)
  and runs the same photosensitive analysis and its 8 Hz strobe control on the server. A run of 1800 frames is three
  requests, never one a frame. Pad input and `@N` start requests inside the checked frames ride on that call. The
  output adds the worst second's frames and numbers and the romdev request count; `ROMDEV_URL` picks another server.
  `tests/flash-check.mjs` runs fx_explosion through romdev on GL and WebGPU with the frames kept and checks that
  romdev's numbers equal `tools/lib/flash.mjs`'s on those same frames (they agree to the last digit on both).

### Changed
- **A romdev run's `--env` reaches the game in frame 1.** `tools/run-cart.mjs --host romdev` sends a test's
  environment with the load (romdev's `firstCommand`, answered in the frame the load runs), as a native run has it
  in its process environment, instead of with the first start request at frame 2, where a switch the game read in
  frame 1 was missed. Needs romdev 0.153.0 or later; an older one, or a request the cart refuses, fails the run.

## v0.45.1 (2026-11-10)

### Fixed
- **A native run's scripted finger never tapped.** The native input script (`CW_INPUT`, written by
  `tools/run-cart.mjs` from a gate's `touches`) held a finger down and lifted it, but never marked the frame it
  landed as a tap, the edge SDL's finger-down event gives a real device and a cart host's pointer slots give a cart.
  Anything that reads a tap (`cw__input_tap`: the shell's menus and join screen, the graphics page) saw none on a
  native run, so `shell-touch` stayed on the title there while it passed through romdev on GL and WebGPU. Real touch
  screens (Android, a touch laptop) were never affected: their fingers arrive as SDL events. A scripted finger now
  taps on the frame it lands, on GL and WebGPU alike, and `shell-touch`, `shell-touch-pause` and `shell-mouse` run
  natively again (CI on llvmpipe and lavapipe) as well as through romdev; each keeps its control (the taps in an empty
  corner), which the unfixed script could not tell from the real run.

## v0.45.0 (2026-11-10)

### Added
- **GPU timing through romdev.** `tools/quality-range.mjs --host romdev` measures every preset's GPU time from the
  built cart through romdev's `frame({op:'timing'})` on the server's pinned test GPU, GL and WebGPU, with no native
  run: each preset a fresh deterministic load run to the same frame, the last 60 frames timed. On the 890M it read
  night_street within about 10% of the native pass sums at every preset on both backends. A 720p sample is loaded at
  romdev's preferred 1080 lines, as natively it gets `CW_RENDER_SCALE`; the control asks every preset for High; a
  noisy run (another process on the GPU) is run again. `tools/lib/romdev-timing.mjs` is the client a game's perf
  gates use: one load (with an optional first-frame request such as `env NAME=value`), setup requests, a warmup and
  the measured frames, checked against the test GPU.
- **A cart's pass timers on WebGPU.** A cart now times its passes when the host's WebGPU device has timestamp queries
  (romdev with `gpuTiming:true`): the `pipeline` request reports `timer_query` true and each pass's GPU ms, as a
  native run does (their readbacks, and 2D's, complete between frames, as a cart's pixel reads do). A browser host's
  device has none and nothing changes there.

## v0.44.2 (2026-11-10)

v0.44.1 was never released: its tag points at the commit after v0.44.0 (a CI change only) by mistake. Use v0.44.2.

### Fixed
- **Split screen's volumetric fog state said the fog was off.** Since v0.41.0 a run without the fog grid zeroes
  `cw_volumetric_fog_get_state`, so a game sees the fog go off when a preset turns it off in play. Split screen ends
  each frame with its overlay run, which has no camera and so no grid, and it zeroed the state the views had just
  set: every split-screen frame reported no fog (gate `fog-split` failed on every host). The views still drew their
  fog; only the state was wrong. A run with no grid now keeps a grid built earlier in the same frame, and a frame with
  none still reports off (gate `fog-volumetric-off`).
- CI: `warm-states`, `text-floor` (tools/check-text.mjs runs its screens on romdev) and `fx-distort-fringe` are
  local gates (they need romdev), and the WebGPU run leaves out romdev-only gates as the GL run does (`shell-touch`,
  `shell-touch-pause`, `shell-mouse`); `unit-weakgpu-auto` reads the run's `unit.failed` at the end
  (`CW_DEBUG_DUMP`), so its control sees the injected failure, which runs at frame 2, after the suite's summary line
  it read before; a stalled apt mirror times out and retries instead of holding a job until its time limit.

## v0.44.0 (2026-11-09)

### Added
- **The shell's menus take touch.** A finger on a touch screen (any of wasmcart's pointer slots 1 to 9) taps menu
  items on the title, pause, settings and graphics pages as a mouse click does, whether or not the app sets
  `cw_app_desc.touch_pointer`; before, only the mouse reached them, so a tap on Play did nothing on a phone. A tap (or
  click) on the join screen seats the keyboard's player, who holds the mouse and touch, and plays (in a match, goes
  back to it), and with no pad seated the screen says "tap to play". Gates `shell-touch`, `shell-touch-pause` and
  `shell-mouse` (sample `shell_join`, through romdev on GL and WebGPU), each with its taps in an empty corner as the
  control.
- `tools/run-cart.mjs --warmup-spread` runs on romdev too: the cart loads with no seed, as a player's host runs it
  (the warm-up spread under the loading screen, the real clock), quality locked to High unless `--keep-auto`.
- `tools/programs.mjs --carts` also runs each cart, and each gate's run with `--gates`, that way, so a survey sees what
  a player's first frames draw; a deterministic load does the whole warm-up inside frame 1 and never showed it.
- Sample `warm_states` and gate `warm-states` (below).

### Fixed
- **A player's first frames drew program states play never asks for, and the red "no program" banner stayed up.**
  The warm-up's shader phase renders the scene a slice at a time, several slices a frame, and each slice is a pipeline
  render of its own; only a frame's first slice had the tree's pre-render, so the later ones had no cascaded sun and
  drew it as a plain shadowed light (`standard+...+shadow+l1501+s100+xgi` and the like). No survey found those states
  (a deterministic run has no slices), so a cart whose table was complete in every check still showed the banner on a
  player's machine and left those draws out. Every slice now gets its pre-render. Gate `warm-states`: sample
  `warm_states` (a cascaded sun, a probe grid, many meshes over few materials) loaded through romdev as a player's
  host runs it has no missing state on GL and WebGPU; its control, `CW_TEST_WARM_NO_PRERENDER=1`, drops the later
  slices' pre-render and must miss states.
- The probe grid and reflection probes no longer capture during a warm-up slice, which showed only part of the scene:
  a grid baked there kept that partial scene's light. They wait for the warm-up's full render.
- The debug field `cw.three.missing_states` holds the whole list (up to 16 KB); it was cut at 1 KB, which hid states
  from the gates and checks that read it.

## v0.43.2 (2026-11-09)

v0.43.1 was never released: its tag points at the v0.43.0 commit by mistake. Use v0.43.2.

### Fixed
- **Carts get their title.** wasmcart moves to v0.41.0 (both the GL test host pin and the WebGPU tier pin), whose
  wasmcart-pack packs `--title`, so `project.cwproj`'s `title` / `cw_add_app(... TITLE "...")` now reaches the
  manifest's `title` and `tools/check-art.mjs` no longer reports `title: missing` on a cart that sets one. The cart's
  version is unchanged: cw_add_app still works it out (`tools/cart-version.mjs`) and passes it as `--version`, which
  the newer pack's own git rule never overrides.
- Cart SDKs ship wasmcart's `src/cartVersion.js`, which wasmcart-pack imports since 0.39.0; without it the SDK's pack
  tool could not start.

### Changed
- `tests/cart-art.mjs` checks the title on every run: a pack tool without `--title` fails instead of being skipped,
  and a new control packs the fixture without `--title` and must find no title.

## v0.43.0 (2026-11-09)

### Added
- **`tools/check-art.mjs`: a built cart's launcher art, checked.** `node tools/check-art.mjs <cart.wasc | carts dir>`
  reads the manifest and art out of each `.wasc` (nothing else is inflated) and prints one line per problem, exit 1 on
  any: a missing `title`, a missing picture (icon, cover, background, logo, title screen, at least one screenshot), a
  screenshot without a label, a file that is not a whole PNG or JPEG (every PNG chunk's CRC, its image data inflating
  to the full picture, a JPEG's frame, scan and end marker) or whose extension names the other format, an icon not
  square or under 256 x 256, a cover not 2:3 or under 600 x 900, a background not 16:9 or under 1920 x 1080, a logo
  that is not a PNG, not wider than tall, under 1280 wide and 720 tall, or without real transparency (under 5% of its
  pixels at most half opaque), a title screen or screenshot under the cart's size (1920 x 1080 when the manifest
  declares none). It cannot read text in a picture, so each cart prints `cover: name not machine-checked`: look at
  the covers. Shipped in every SDK; `tests/check-art.mjs` runs it with 18 controls that must fail, a 16:9 cover first.
- **A game's title.** `project.cwproj`'s `title` (or `cw_add_app(... TITLE "...")`) goes to the cart's manifest
  `title`: the game's name as people read it, for launchers and frontends; the app's name stays the cart's id. It
  needs a wasmcart-pack that knows `--title` (after wasmcart 0.40.0); the pinned one ignores it until the pin moves.
- `tools/lib/wasc-splice.mjs` `readNamedEntries(wasc, names)`: the named entries of an archive, inflating nothing else.

### Changed
- The `hello` sample's art fixture has a 1920 x 1080 title screen and screenshot, so the sample passes the art check.

## v0.42.0 (2026-11-09)

### Added
- `cw_rollback_set_no_rollback` (cartwheel/net.h): the desc's `no_rollback` test control, set after creation (a gate's
  switch that arrives in play).

### Changed
- `AGENTS.md`: every check goes through romdev. The no-strobe check is a game's own flash meter (debug fields read over
  a ranged romdev run, at most 3 flashes a second, with a control) until romdev can hand over frames in ranges for
  `tools/flash-check.mjs`; the engine checkout's gate command is `--hosts romdev --gpu both`.

### Fixed
- **Every engine gate's switches reach its cart on romdev.** A gate's `env`, `controlEnv` and WebGPU controls' `env`
  go to a cart as the debug channel's env request, answered at frame 2; 93 of 161 such runs failed because the switch
  was read in `init` or the first frame (`fx_distort` read `FXD_OFF` in its init, so its romdev control failed). Each
  is now read with `cw_test_env` where it acts and applied from frame 2, natively the same: 35 samples (`fx_distort`,
  `gi_room`, `gi_probe_check`, `vertex_colors`, `instanced_attributes`, `character_skin`, `texture_filtering`,
  `perf_nodes`, `audio_3d`, `audio_play`, `nav_links` (the links removed and the region baked again), `r2d_hdr`,
  `r2d_motion`, `ui_scene`, `water_open_sea`, `water_ripples`, `night_street`, `fx_pipeline`, `fx_decals`,
  `fx_bursts`, `fx_collide`, `fx_sub_emitters`, `decal_walls`, `fx_mesh_decals`, `vis_impostor`, `vis_city`,
  `viewmodel_demo`, `lightmap_yard`, `rim_light`, `prompts`, `gltf_scene`, `post_synth`, `l10n_demo`,
  `triggers_demo`, `net_arena`), the unit suites (the injected failure and a suite's `cw_test_late()` case run at
  frame 2; `unit.done` waits for them), `fx_gpu`, and the engine's own switches (`CW_TEST_PROMPT_BREAK`,
  `CW_TEST_LIGHTS_STALE`, `CW_TEST_NO_REMARK`). Only control runs change: a gate's main run reads nothing new.
  Gate `env-romdev` runs each such
  environment on its cart through romdev (0 of 161 refused; control: a variable a unit suite reads in its init on
  purpose is refused).
- **LightmapGI `shading` set after the bake applies.** It was read only when the bake landed, so a change in play (the
  lightmap-yard control's `LIT`) did nothing; a lightmapped mesh now keeps its original material and is remade from it.
- `unit_resource`'s GPU-resident check holds on WebGPU (the texels go to three.c, which uploads them at the first draw;
  there is no GL texture).
- The romdev mode's WebGPU leg (`gate.mjs --hosts romdev --gpu both`) runs gates written for GL alone: gate keys
  `glOnlyFields` (state measured on GL only: viewmodel's `vm.wall_ok`), `vsGlTol` (a GPU readback that rounds differently:
  fx-collide's mean height, 1 mm) and a top-level `vsGl` (lightmap-bake's ball covers 0.1% of the frame). All 121 gates of
  the samples changed here pass on romdev GL and WebGPU with their controls failing; 4 are native only.

## v0.41.0 (2026-11-09)

### Added
- **`AGENTS.md`**: the page a coding agent reads first, shipped in every SDK. Where to start (the template and the
  example game closest to the brief, by game type), how a game is laid out, the APIs most games use with examples
  that compile against the headers, the build, run and check loop through romdev (ranged calls), the rules that keep
  a game from failing and the mistakes made most often with their fixes. The SDK also ships `tools/flash-check.mjs`.
- **`DEV=1 scripts/build.sh [targets]`** in an engine checkout: dev carts (no ThinLTO, no wasm-opt) for the
  edit-build-run loop, and the named ninja targets only (`scripts/build.sh platformer3d`). platformer3d after a
  one-line change: 11.2 s (release cart: wasm-opt is 12 s of it) to 1.9 s for the native build and the cart.
- `tools/run-cart.mjs` answers `--start-cmds "@N cmd"` on romdev too (the frames before N in one step, the
  request on frame N).
- **The SDK ships `tests/gate.mjs`** (a game runs its `tests/gates.json` with `CW_PROJECT_ROOT=<game> node
  <sdk>/tests/gate.mjs`), `tools/flash-check.mjs` and `AGENTS.md`; `scripts/sdk-link-check.sh` fails an SDK that lacks
  any file a game's scripts name or a module one of them imports (control: the v0.39.1 SDKs lack `AGENTS.md`).

### Changed
- **A dev cart's pack after a code change splices the new `cart.wasm` into the last archive** when the assets,
  art, version and pack command are what the last pack had (`tools/cart-version.mjs pack --reuse-assets`, which
  `cw_add_app` passes with `CW_CART_DEV`; `tools/lib/wasc-splice.mjs`). Packing deflated every asset again: 4.2 s
  of fighter's 4.5 s cart link (130 MB of models). The archive has the same entries in the same order with the same
  bytes as a fresh pack; a release cart is always packed in full. A games repository's fighter after a one-line
  change, native and dev cart: 6.0 to 7.6 s before, 2.5 to 2.7 s after. Gate `cart-repack` (a stored and a
  deflated wasm against fresh packs, `unzip -t`; an asset changed, added or a new version packs in full; controls:
  a splice past a changed asset and a splice of the old wasm are caught). The cart loaded in romdev draws the same
  frame and debug state as a fully packed one.
- **`tools/programs.mjs` also surveys each preset switched in play**: High to each other preset half way through a
  run, and each back to High, natively and with `--carts` (`--no-switch` leaves them out). Content a game builds
  for the preset it starts on, drawn with another preset's renderer state, asks for states no run started on one
  preset asks for; those draws vanished after a player changed the preset or Auto stepped down ("1 draw state(s) have
  no program" mid-race). Gate `programs-switch`: a scene that adds shadow-casting omni lights on High draws
  everything after a switch to Very Low, natively and as the dual cart through romdev on GL and WebGPU; control: the
  same scene with a table surveyed `--no-switch` misses `standard+instbig+shadow+l1200+s100+xplain`.
- three.c v0.187.7 (`t3_renderer_set_missing_hook`).

### Fixed
- **Reflection probes and the probe grid act on FORWARD lighting with a cascaded sun.** The GI part joined only bare
  materials and those with the clustered lights' part, so with FORWARD lighting and sun cascades (every material
  carrying the sun's `csm` part) no material took it: `cw_gi_get_stats` read 0 materials and the probes did nothing,
  silently. It now composes after `csm` too (`xcsm_gi`, `xcsm_plain_gi` on Very Low). A material the GI part still
  cannot join while the tree has probes is named in an error with the parts it composes with. Gate `gi-probe`: the
  mirror ball in `gi_probe_check` with `GI_CSM=1` shows the six walls and 7 materials carry gi, on GL and WebGPU;
  control `CW_TEST_GI_SKIP_CSM=1` (the old rule) reads 0 materials and fails the ball.
- **A draw with no program says what it is and how to fix it.** Each missing state logs `render: NOT DRAWN: node
  "/root/Level/Crate" (mesh "..."), MeshStandardMaterial "...": its program state ... is in none of the app's
  program tables`, naming the nearest node holding the drawn object, the glTF mesh and the material; the pipeline's
  error then gives the command for this app (and the SDK form, and `--carts`), says a preset changed in play is a
  cause, and names the guide section (rendering-3d.md, "Program tables"); the banner points at the log. render3d's
  error named `<app>` instead of the app; its extension error names the call (`t3_extension_set_texture`). Gate
  `programs-switch` checks the node, material, command and guide in the log (control: a run with nothing missing
  logs no NOT DRAWN).
- **`cw_volumetric_fog_get_state` reads inactive once a preset turns the fog off in play.** It kept the last grid's
  `active` and `volumes` after the effects detail dropped to Very Low or Low, so a game that swaps in a stand-in
  when the fog is off showed neither. Gate `fog-volumetric-off` (High, then `quality verylow` at frame 20, natively
  and through romdev on GL and WebGPU; control `CW_TEST_VFOG_STALE=1`, the old state, fails).
- **The first rebuild after a clean configure no longer recompiles every cart object.** `cmake/cart.cmake` included
  wasi-sdk's toolchain file, which appends `-pthread` and the memory flags to the cached flags; CMake reads a
  toolchain file twice on a first configure, so the flags changed on the second configure and every object (the
  whole engine in an engine checkout) rebuilt once. The flags are now `CMAKE_*_FLAGS_INIT`; the wasm is byte
  for byte the same. Gate `cart-toolchain` (control: wasi-sdk's own file changes the flags).

## v0.40.1 (2026-11-09)

### Fixed
- **Heat haze, shockwaves and blasts no longer paint rainbow fringes.** The distortion pass split the picture's colours on High and Ultra (red sampled further along the bend than green, blue the other way, a third of the shift apart), which drew oil-slick colour round every haze column and blast ring over a high-contrast background. It now moves the picture without splitting it: a colourless shimmer on every preset, the same on GL and WebGPU, with two texture reads a pixel fewer where it bends. `cw_quality_scale("dispersion")` is 0 (it no longer appears in the effects table). New gate `fx-distort-fringe` (`tests/distort-fringe.mjs`, romdev, GL and WebGPU): fx_distort with its scene turned grey, the share of the pixels the distortion changed that come out coloured; 0 % now, 24.8 % on v0.39.1's build, and a frame with red and blue moved 3 pixels apart fails it as its control.

## v0.40.0 (2026-11-09)

### Added
- **Gates run on romdev alone, GL and WebGPU.** `node tests/gate.mjs --hosts romdev [--gpu webgpu|both]` runs no native leg: the game's dual cart on romdev with GL is the reference (a real picture, `expect`, the text floor, missing program states, prompt errors) and its control on romdev GL must differ from it; with `--gpu webgpu` or `both` the same cart runs on WebGPU with the same checks, its named state exact against the GL run, its frame within `webgpu.vsGl` and `maxArea`, and its own control must differ from both. Gates that leave romdev out, tool gates and `webgpu.controls` are listed as skipped, never counted as passing. Before, `--hosts romdev` crashed reading a native reference shot, and `--gpu webgpu` ran only gates with a `webgpu` block. The mode takes no GPU suite lock (it runs no GPU work of its own). `--gates-file` takes an absolute path too.
- **A gate's environment reaches a cart.** The debug channel's new built-in `env NAME=VALUE ... [-- request]` sets variables in the running game on any host; every cart link wraps `getenv`, so a game's own `getenv` calls read them unchanged. `tools/run-cart.mjs --env '{"NAME":"v"}'` (the gate runner passes a gate's `env` and `controlEnv`) sends it riding on a request each host sends anyway, so no host's frames move: before frame 1 on Chromium and wasmcart-native, at frame 2 with the first start request on romdev. A variable the game read before the request arrived fails the run, naming it, rather than running unset. `cw_test_env(name)` (cartwheel/debug.h) reads a test switch alike on every host: NULL in the first frame, the variable from frame 2. derby's `DERBY_NO_MUD` control and partypack's `PP_ROUNDS` boards (read from frame 2) pass on romdev GL and WebGPU against the values measured natively.
- **`tools/check-text.mjs` runs on romdev** (the default; `--host native` for the native build): each screen is a run of the cart, its `env` sent as the env request, the text sizes read from the `text` request after the last frame. `--pref` needs `--host native` (romdev takes no preferred frame size from a client).
- `tools/lib/flash.mjs` `boxDown` (a cart host's RGBA frame filtered down in linear light as a native frame dump is) and `frameDumpHeader`; `tests/flash-check.mjs` checks the filter against sums worked by hand.

### Changed
- **`tools/run-cart.mjs` sends romdev ranges, never a request per frame.** A pad-only input script goes to romdev as one `input` `sequence` call (each entry held until the next, the last to the run's end) instead of a step and a set per entry: a 700-frame derby run with a 10-entry script took 25 requests and now takes 5, with the same frame and state byte for byte. Only mouse, touch or wheel entries take requests per entry. The result reports `romdevCalls`, and every request names its caller (`x-romdev-agent`, `CW_ROMDEV_AGENT`).
- **`tools/flash-check.mjs --app` defaults to romdev and stops there** (exit 2, saying why): romdev cannot hand over every frame of a run in ranged requests yet, and a request per frame floods the shared server. `--host native` runs the engine's own samples natively as before; `--dump` and `--dir` are unchanged.
- `tools/quality-range.mjs` says it is native only: a cart on romdev has no GPU timer (`pipeline` reports `timer_query` false on GL and WebGPU), so a game's gates leave the timing out until romdev can time a cart.

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
