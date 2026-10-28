# Changelog

What each release added, changed and fixed. Releases are git tags; each one's
prebuilt SDKs (one per target, see [docs/sdk.md](docs/sdk.md)) are on its
GitHub release. The guides in `docs/guides/` cover each feature in depth.

## v0.18.1 (2026-10-27)

Build and CI cleanup: one cart per app, the dual GL + WebGPU cart. The
GL-only cart tree and its SDK are gone; GL is tested by running the dual
cart on GL hosts.

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
  as is (`run-cart.mjs`, `perf-frame.mjs`, `net`, `showcase-perf`,
  `channel`, `a11y-channel`, `profile-cart`, `remote`, `host-resolution`,
  `editor-level`, `plugins`). `cart-audio-ring` uses the dual hello cart and
  `wcnative-pads` checks pad slots on it (input_demo is `GL_ONLY`).
  `editor-channel`, `editor-import` and `editor-play` lost their editor cart
  legs (the editor is `GL_ONLY`, so it has no dual cart).

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

### Known issues
- Decal3D in the cooperative platformer example draws slightly differently on
  GL and WebGPU; the cause is not found yet.

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
