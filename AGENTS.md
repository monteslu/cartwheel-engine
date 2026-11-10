# cartwheel for coding agents

Read this first. It is an index: each section points at the header or guide that holds the detail. The headers in
`include/cartwheel/` are the API reference (every comment there is current); the guides are in `docs/guides/`. In
a prebuilt SDK both sit at the same paths under `deps/cartwheel-sdk/<platform>/`.

cartwheel is a 2D and 3D game engine in C. One game source builds as a native program (Linux, Windows, macOS,
Android) and as a wasmcart cart (`.wasc`) that draws with WebGPU where the host has it and GL elsewhere. The 3D
renderer is three.c, the three.js r187 API in C: materials, lights, cameras and glTF behave as in three.js r187.

## Start here

Do not start from a blank file. Start from the template, or from the example game closest to what you were asked
for, and replace its gameplay and art.

- **The template:** <https://github.com/monteslu/cartwheel-starter>. A small four-player game with the title,
  join, pause and settings screens, saves, button prompts, an autopilot build, tests on GL and WebGPU and release
  CI already wired. It builds against the prebuilt SDK, so only the game compiles. Its `AGENTS.md` describes its
  scripts (`./build.sh`, `./run.sh`, `./test.sh`, `./shot.sh`, `./programs.sh`, `./release.sh`).
- **The closest working game:** the table below. Copy systems (a camera rig, an AI, a race course pipeline), never
  art or names.

### Game archetypes

Every game here is complete, runs on GL and WebGPU, and has its own gates. Paths are in each repository's
`games/` directory; each repository's README describes every game and most games have their own README.

| Game type | Game | Repository | Worth copying |
|---|---|---|---|
| kart racer on looping tracks | racer | [cartwheel-examples](https://github.com/monteslu/cartwheel-examples/tree/main/games/racer) | track mesh and collision from one sweep, drift and mini-turbo, items, computer drivers, split screen, a contact shadow under each kart |
| downhill bike racer | bikemesa | [cartwheel-examples](https://github.com/monteslu/cartwheel-examples/tree/main/games/bikemesa) | a 2D physics bike in a 3D world, crash ragdoll, four lanes of split screen, an idle seat that does not hold the start |
| vertical shooter | chromite | [cartwheel-examples](https://github.com/monteslu/cartwheel-examples/tree/main/games/chromite) | wave scripts, a score chain, every blast a light (clustered lighting), a reflection probe for chrome, mouse, keys, pad and touch |
| car combat (demolition derby) | derby | [cartwheel-examples](https://github.com/monteslu/cartwheel-examples/tree/main/games/derby) | Box3D vehicles, damage that changes handling, hit stop with pad rumble instead of shake, per-player HUD in split screen |
| arena team sport | pugball | [cartwheel-examples](https://github.com/monteslu/cartwheel-examples/tree/main/games/pugball) | team AI, ball physics, unlocks, 1v1 to 3v3 with CPU fill |
| bomber arena | bomberdude | [cartwheel-examples](https://github.com/monteslu/cartwheel-examples/tree/main/games/bomberdude) | an integer-only simulation (identical on every host), CPU rivals, a floating touch stick |
| climbing time trial | spout | [cartwheel-examples](https://github.com/monteslu/cartwheel-examples/tree/main/games/spout) | weather that changes the rules, par times and medals, HUD kept off the play area |
| split-screen arena shooter | arenafps | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/arenafps) | navmesh bots with human-like aim error, CSG arenas, first-person weapons, aim assist, mesh decals for bullet holes |
| beat 'em up | brawler | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/brawler) | one shared screen for four, ragdoll knockdowns and get-ups, bosses with tells and phases, decals |
| 3D co-op platformer | coopplatformer | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/coopplatformer) | a shared camera that frames every player, a bubble that carries a lagging player, abilities that open the way |
| dungeon crawler | crawler | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/crawler) | generated mazes, spawners with 100+ monsters, keys and doors, an arrow that shows a lost player the way |
| versus fighter | fighter | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/fighter) | frame data, input buffer, motion inputs, hitboxes, training mode, resident fighters on the select screen |
| twin-stick horde shooter | hordeshooter | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/hordeshooter) | hundreds of enemies, timed power-ups, a yard redesigned for weak GPUs (baked floor, blob shadows) |
| wave racer | jetski | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/jetski) | a craft that rides the drawn water (`cw_water_sample_at`), procedural rider pose, ramps, buoy courses, generated terrain |
| party board game | partypack | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/partypack) | a board with eleven mini-games behind one interface, CPUs and people read through the same input |
| sailing and naval | pirates | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/pirates) | shared-screen ships on the water, baked lightmaps, Auto quality down to Very Low for handhelds |
| flight combat | skyfighter | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/skyfighter) | a flight model, guided missiles and flares, terrain missions, a jet that breaks into Box3D pieces |
| downhill sliding racer | tuxracer | [more-cartwheel-examples](https://github.com/monteslu/more-cartwheel-examples/tree/main/games/tuxracer) | courses on PBR terrain, sliding on Box3D, cascaded shadows, instanced forests |
| tower defense | towers | [even-more-cartwheel-examples](https://github.com/monteslu/even-more-cartwheel-examples/tree/main/games/towers) | thousands of creeps, build pads laid out as the face buttons, maps checked in CI (path, pads, grounding, a bot clear) |
| pinball | pinball | [even-more-cartwheel-examples](https://github.com/monteslu/even-more-cartwheel-examples/tree/main/games/pinball) | a swept, substepped ball solver that never tunnels, flippers, modes, nudge and tilt |
| skateboarding | skate | [even-more-cartwheel-examples](https://github.com/monteslu/even-more-cartwheel-examples/tree/main/games/skate) | tricks that link into combos, grinds and manuals, lightmapped parks, split screen |
| 2D exploration platformer | lanternfall | [even-more-cartwheel-examples](https://github.com/monteslu/even-more-cartwheel-examples/tree/main/games/lanternfall) | the 2D renderer: lit sprites, soft shadows, bounced light; abilities that open a large map |
| physics destruction | destructo | [even-more-cartwheel-examples](https://github.com/monteslu/even-more-cartwheel-examples/tree/main/games/destructo) | thousands of Box3D blocks that break, chain reactions, instant replay |
| third-person action adventure | emberthrone | [even-more-cartwheel-examples](https://github.com/monteslu/even-more-cartwheel-examples/tree/main/games/emberthrone) | lock-on sword fights with stamina, one connected terrain world, a drop-in second player |
| match-three | jewels | [even-more-cartwheel-examples](https://github.com/monteslu/even-more-cartwheel-examples/tree/main/games/jewels) | a calm untimed board, controller and touch, gems ray traced per pixel |

## How a game is laid out

| Path | What |
|---|---|
| `src/main.c` | `cw_main` returns a `cw_app_desc`: `init`, `fixed_update` (60 ticks a second, the simulation), `render` (HUD) |
| `*.cwscene` | the world as scene files: nodes and their properties ([scenes](docs/guides/scenes.md)) |
| `project.cwproj` | name, input map (`input.<action> = key:space, pad:south`), render size, export presets ([app-platform](docs/guides/app-platform.md)) |
| `roots.txt` / `assets/` | what gets packed into the game; scenes reach the rest ([resources](docs/guides/resources.md)) |
| `programs/<app>.txt` | the three.c program table: every material, light and shadow state the game draws ([Program tables](docs/guides/rendering-3d.md#program-tables)) |
| `tests/gates.json` | the gates: runs of the cart on romdev, GL and WebGPU, that must agree ([testing](docs/guides/testing.md)) |
| `<app>_auto` | the same game with an autopilot: the gates, screenshots and soaks run it |

The game shell (`cartwheel/shell.h`, [game-shell](docs/guides/game-shell.md)) gives every game its title, player
join, pause, settings (graphics, audio, controls), saves and split screen. Write gameplay and art, not menus.

## The APIs most games use

Each line names the header; the examples compile against it.

**App and scene** (`scene_file.h`, `scene.h`):

```c
static void init(void *u) { cw_project_load_input(); /* cw_shell_init(...) */ }
static void fixed_update(void *u, double dt) { if (!cw_shell_update()) return; /* gameplay */ }
cw_app_desc cw_main(int argc, char **argv) {
  static cw_app_desc app = { .width = 1920, .height = 1080, .debug = true, .init = init, .fixed_update = fixed_update };
  return cw_scene_app("main.cwscene", &app);
}
cw_node level = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Level");
cw_node box = cw_node_new("MeshInstance3D", "Crate");
cw_node_set(box, "primitive", cw_val_enum("BOX"));
cw_node_add_child(level, box);
cw_node3d_set_position(box, (t3_vec3){ 0, 0.5f, 0 });
```

**Camera** (`scene.h`, node `Camera3D`; split screen `shell.h`): a `Camera3D` node with `fov`, `near`, `far`;
`cw_node3d_look_at(cam, target, (t3_vec3){ 0, 1, 0 })`. One view per player: `cw_shell_split(NULL, cameras)`.
Follow cameras ease (a critically damped spring); they never shake.

**Lights and shadows** (nodes `DirectionalLight3D`, `OmniLight3D`, `SpotLight3D`; `lighting.h`;
[Lights and shadows](docs/guides/rendering-3d.md#lights-and-shadows)): `shadow = true` casts.
A sun covering a level uses cascades: `shadow_cascades = 4`, `shadow_distance = 200`. A single-map sun's
`shadow_far` is the shadow camera's depth from the light, not a view distance.

**Materials and meshes** (node `MeshInstance3D`: `primitive` or `mesh`, `color`, `roughness`, `metalness`,
`emissive`, `cast_shadow`, `receive_shadow`; three.c `t3_mesh_standard_material_new` from code). Characters:
`skin_shading.h`, [Characters](docs/guides/rendering-3d.md#characters-skin-gltf-materials-faces).

**glTF** (`resource.h`, [resources](docs/guides/resources.md)):

```c
cw_res r = cw_res_load("res://models/hero.glb", 0);
t3_object *hero = cw_gltf_instantiate(r);      /* shares geometry and materials; own skeleton */
t3_object_add(cw_node3d_object(level), hero);
t3_release(hero);                               /* the parent holds it */
```

**Physics** (`physics_nodes.h`, [physics](docs/guides/physics.md)): `StaticBody3D`, `RigidBody3D`,
`CharacterBody3D` and `Area3D` nodes with collision shapes in the scene. A player:
`cw_node_set(p, "velocity", cw_val_vec3(vx, vy, vz)); cw_character3d_move_and_slide(p, (float)dt);` then read
`on_floor`. Pickups: `cw_connect(area, "body_entered", area, NULL, on_enter, NULL, 0)`. Vehicles `vehicle3d.h`,
ragdolls `ragdoll.h`, navigation `nav.h`.

**Input and prompts** (`input.h`, `prompt.h`, [input](docs/guides/input.md)):

```c
cw_action jump = cw_action_find("jump");               /* project.cwproj: input.jump = key:space, pad:south */
if (cw_action_just_pressed(player, jump)) { /* ... */ }
float x = cw_input_axis(player, left, right);
cw_prompt_text(font, "{jump}: hop the gap", player, pos, size, color, CW_ALIGN_LEFT);  /* draws the player's button */
cw_input_rumble(player, 0.8f, 0.4f, 120);             /* hits and blasts: the pad, never the camera */
```

**Audio** (`audio.h`, [audio](docs/guides/audio.md)): `cw_sound s = cw_sound_load("res://sfx/hit.wav", NULL);`
`cw_audio_play(s, &(cw_play_desc){ .bus = cw_shell_sfx_bus() });` music on `cw_shell_music_bus()`; 3D sound and
reverb zones in the guide.

**Text and HUD** (`render2d.h`, `ui.h`, [ui](docs/guides/ui.md)): in `render`, `cw_r2d_begin(NULL)`, then
`cw_r2d_text(cw_ui_font_sans(), "3 laps", pos, &(cw_text_style){ .size = h * 0.03f })`, `cw_r2d_end()`, then
`cw_shell_draw()`. Lay each player's HUD inside `cw_shell_player_rect(i)`.

**Quality presets** (`quality.h`, [Graphics settings and presets](docs/guides/rendering-3d.md#graphics-settings-and-presets)):
five presets (Very Low to Ultra) plus Auto. `cw_quality_auto(true, 16.6f)` holds a frame budget;
`cw_quality_on_change(fn, user)` re-applies a game's own levers; `cw_quality_count(n, "density")` and
`cw_quality_density()` thin scenery; `cw_gfx_effective(CW_GFX_EFFECTS)` says what is drawn now.

**Effects** (`explosions.h`, `particles.h`, `decals.h`, [explosions](docs/guides/explosions.md)):
`cw_explosion_fire(node)`; drive rumble with `cw_explosion_shake(tree, at)`.

**Debug channel** (`debug.h`, [debug-channel](docs/guides/debug-channel.md)): publish what a test reads, and
answer requests:

```c
cw_debug_i32("game.score", &score);                  /* a debug field: gates compare it across hosts */
cw_debug_command("level", "level <n>: start level n", cmd_level);
static void cmd_level(int argc, const char **argv, cw_reply *r) { want_level = argc > 1 ? atoi(argv[1]) : 1;
  cw_reply_printf(r, "{\"level\":%d}", want_level); }   /* set state; the work happens over the next ticks */
```

A test switch (a gate's `env`, a control) is read with `cw_test_env("NAME")` where it acts, from frame 2: a cart host
delivers it with its first request, so a variable read in `init` or the first frame runs unset there and the run fails.

## Build, run, check

In the starter (and every games repository built the same way):

| Step | Command |
|---|---|
| build native + cart | `./build.sh` (dev carts by default: no ThinLTO, no wasm-opt, a code change relinks in about a second) |
| play | `./run.sh` |
| the gates | `./test.sh` |
| a screenshot | `./shot.sh` then look at `out/shot.png` |
| program table | `./programs.sh` after a new material, light setup or preset-dependent content |

In an engine checkout: `scripts/fetch-deps.sh`, then `DEV=1 scripts/build.sh <app>` for the edit loop (a release
build is `scripts/build.sh`); `node tests/gate.mjs <gate> --hosts romdev --gpu both`. The SDK carries the same `tools/`
and `tests/gate.mjs`.

**Every check that runs the cart goes through romdev** (the MCP server on 127.0.0.1:7331; send
`x-romdev-session` on every call). Send ranges, never loops: one call steps many frames on an input schedule; never
one request per frame or per press.

```text
loadMedia({platform:'wasmcart', path:'build/cart-wgpu/carts/mygame.wasc', wgpu:true})   // wgpu:false runs it on GL
input({op:'timeline', timeline:[{frame:60, buttons:['start'], until:64},
                                {frame:120, buttons:['right','south'], until:400}], untilFrame:900})
frame({op:'screenshot', path:'out/f900.png'})          // then LOOK at it
wasm({op:'debugState'})                                 // the game's debug fields
wasm({op:'command', text:'quality verylow'})            // a debug request (costs one frame)
frame({op:'step', frames:600})                          // a range with no input
host({op:'shutdown'})                                   // when done
```

`tools/run-cart.mjs --host romdev --app <app> --frames N --input script.json --start-cmds "@300 quality low"`
does the same from a script (`--gpu gl --dual-gl` for the GL leg), and the gates use it.

Checks a game should run (all shipped in the SDK under `tools/`):

- `node tools/check-prompts.mjs src *.cwscene`: no text names a button.
- `node tools/check-art.mjs build/cart-wgpu/carts/<game>.wasc`: the cart's title and launcher art are all there, the
  right shapes and sizes (then look at the cover for the game's name: no tool reads it).
- `node tools/check-text.mjs tests/<game>-text.json`: every screen's text is at or above the size floor (on romdev).
- No strobe: a flash meter of the game's own. `tools/flash-check.mjs` reads every frame and romdev cannot hand frames
  over in ranges yet, so until it can a game measures each effect that can flash (a damage tint, a muzzle light, a
  blast's light) where it draws it: the value it draws each frame, a flash for every pair of opposing changes of 0.1 or
  more, and the most flashes in any one second, published as debug fields (`fl.<effect>_flashes`, `fl.<effect>_peak`)
  and read over a ranged romdev run. A gate holds them to at most 3 flashes a second; its control (the effect switched
  on and off every few ticks) must fail.
- `CW_PROJECT_ROOT=. node <sdk>/tests/gate.mjs --hosts romdev --gpu both`: the game's `tests/gates.json` on the cart,
  GL and WebGPU.
- `node tools/programs.mjs <app>`, then `node tools/programs.mjs --carts <app>`: the program table covers every
  preset, a preset switched in play, and the cart's own states.

## Rules that keep a game from failing

- **Nothing floats.** Every prop, plant, pickup and character stands on what is under it: place by a ray down at
  build time, and draw moving bodies from the pose physics just produced (a wheel cast before the step is drawn a
  tick behind the chassis). Keep sun shadows on every preset (Minimal keeps them) and add a contact shadow
  (a MULTIPLY `Decal3D`) under anything that moves. Particles belong to their emitter, not to a light.
- **No strobe, no screen shake.** Flashes swell and fade over 6 to 10 ticks; nothing jumps to full brightness in
  one frame or flickers. Hold it with the game's flash meter (above; the rules are in
  [Flash check](docs/guides/testing.md#flash-check)).
  No camera ever shakes: hits and blasts rumble the pad (`cw_input_rumble`).
- **A newcomer must finish level 1.** Gate it with a bot that plays only from what the screen shows, with human
  error (reaction delay, aim error that grows with distance, missed presses), and with a second pad that joins and
  then idles: an idle seat must not hold a start screen, a shared camera or a tether. If the way is not obvious,
  show it (lit doors, a light on the goal, an arrow after a while).
- **Readable text, prompts that never name a button.** Body text at least 2.4% of the canvas height (26 px at
  1080 lines), nothing under 1.7%; prompts at least 16 canvas pixels. Write `{jump}: hop the gap`, never "A: jump"
  or "press green" ([Showing controls](docs/guides/input.md#showing-controls)).
- **Quality scales down to a handheld without lowering resolution.** Very Low must run on a handheld-class GPU:
  bake light, cut lights and casters, thin scenery, drop effects; never shrink the render resolution
  (Resolution is the player's own setting). Turn on Auto ([Fully baked lighting for weak GPUs](docs/guides/rendering-3d.md#fully-baked-lighting-for-weak-gpus)).
- **Believable athletic people.** Adults about 7.25 to 7.7 heads tall, real skull, neck, shoulders, waist and legs;
  no balloon heads, no long thin necks, no shapeless round bodies (a heavyweight is muscular).
- **Matte cloth.** Cotton, denim, wool and canvas have high roughness and almost no specular; only leather, vinyl,
  rubber, metal and wet things shine; skin is in between.
- **Determinism.** The simulation runs in `fixed_update` from the input snapshot and `cw_rng`; one `cw_rng_*`
  call per statement; never the wall clock. The gates hold GL, WebGPU and every host to the same state.

## The ten mistakes made most often, and their fixes

1. **Draws vanish after a preset change** (red banner "N draw state(s) have no program"). Content built on High
   and drawn on Very Low asks for states no single-preset run asks for. `tools/programs.mjs` surveys each preset and
   the preset switched in play both ways; run it with `--carts` too so the cart's own states are in. The log names each node
   and material that is not drawn.
2. **Props, wheels and fire floating.** See the grounding rule; check from the game camera and from the side.
3. **The sun's shadow stops short.** `shadow_far` is depth from the light: a sun parked 80 m back with
   `shadow_far = 70` casts nothing. Use cascades with `shadow_distance`, or a `shadow_far` past the far edge of the
   play.
4. **Coplanar faces fighting** (flicker where a sign, decal plate or trim lies flush on a wall). Lay faces a few
   millimetres apart, more with distance (depth resolution falls with the square of the distance over `near`),
   and keep `near` as large as the camera allows.
5. **A one-frame flash.** A damage overlay or muzzle light at full strength for a tick is a strobe; swell and fade.
6. **An idle second seat blocking play** (a select screen waiting for every pad, a shared camera pinned to a player
   who never moves). Test with a pad that joins and does nothing.
7. **A newcomer bot with perfect aim.** A gate that plays with instant reactions and exact aim passes a level a
   person cannot. Give it reaction time, aim error and mistakes, and play only what the screen shows.
8. **A debug command that does all its work inside one request.** A request is answered at the start of one frame;
   a long scan or a whole level load there stalls the cart host. Set state in the command and do the work over the
   next ticks, reporting progress in debug fields.
9. **Reading state the engine changes under you.** Read the engine's state each frame, not once:
   `cw_volumetric_fog_get_state` says the fog is off once a preset turns it off, `cw_gfx_effective` says what each
   setting is now.
10. **Reloading assets on a menu change or a respawn** (a stutter each time). Load once, keep instances resident
    (`cw_gltf_instantiate` shares geometry), swap what is shown; place and pose a spawned body before its first
    visible frame.

## More

[overview](docs/guides/overview.md), [getting-started](docs/guides/getting-started.md),
[architecture](docs/guides/architecture.md), [testing](docs/guides/testing.md),
[profiling](docs/guides/profiling.md), [shipping](docs/shipping.md), [sdk](docs/sdk.md),
[CHANGELOG](CHANGELOG.md).
