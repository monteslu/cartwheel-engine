# Input

Games ask about named actions ("jump", "move_left"), never about devices. Keys, mouse buttons, the wheel, pad buttons, pad axes and on-screen touch controls are bindings of those actions, rebindable at run time, and the stream of action states can be recorded and replayed bit for bit.

Header: [cartwheel/input.h](header:input.h). Sample: `input_demo` (every feature below, at a 120 Hz tick so each 60 fps frame runs two ticks). Gates: `input-pad`, `input-keys`, `input-pointer`, `unit-input`; `games/action2d` plays the same run on a pad and on a keyboard and must end in the same state (`action2d`, `action2d-keys`).

## Actions and bindings

[cw_action_add](api:cw_action_add) defines an action with a deadzone and returns its index (a plain `int`; -1 is "no action" and every query on it answers false or 0). Look actions up once and keep the index: a query is an array read.

Bindings have a text form, used everywhere (binding files, `project.cwproj`, saved settings, the debug channel):

| Text | Binding |
|---|---|
| `key:space`, `key:a`, `key:44` | a key by SDL scancode name or number |
| `mouse:left`, `wheel:up` | a mouse button; a wheel direction (pressed on frames the wheel moves) |
| `pad:south`, `pad:start`, `pad:up` | a pad button (south is A on an Xbox pad) |
| `axis:lx-`, `axis:rt+@0.5` | a pad axis direction, with an optional threshold (triggers are axes) |
| `touch:1`, `touch:0.left` | an on-screen touch button, or one direction of a touch stick |

A binding set is one action per line; a line replaces that action's bindings, and `deadzone:0.3` may appear in the list. `input_demo` ships these defaults:

@include samples/input_demo/assets/bindings.txt

Load defaults, commit them, then apply the player's saved overrides on top:

@include samples/input_demo/main.c#bindings

## Players and devices

Up to four players. By default player 0 owns pad 0, the keyboard, the mouse and touch, and players 1 to 3 own pads 1 to 3. [cw_input_assign](api:cw_input_assign) changes that; a device may be shared. Every query takes the player index.

## Ticks and frames: two snapshots

The module keeps two snapshots of every action, and a query reads whichever one fits where it is called:

- The **tick snapshot** is taken right before each `fixed_update`. Queries inside `fixed_update` read it. This is what simulation reads, what is recorded and replayed, and what the stream hash covers.
- The **frame snapshot** is taken once per rendered frame after the platform polls devices. Queries in `update`, `render`, `event` and `init` read it. This is what UI reads.

`just_pressed` is right in both. A tap that starts and ends between two ticks is latched, so the next tick sees it pressed (and the tick after sees it released); when several ticks run in one frame only the first sees `just_pressed`. A press that begins and ends inside one frame's event batch is caught by the platform's per-frame "went down" bits. `input_demo` counts the same jump button in both contexts; its simulation runs at 120 Hz:

@include samples/input_demo/main.c#sim_step

Analog values are quantized to int16 steps before they enter a snapshot and every derived value uses IEEE `+ - * / sqrt` only, so the same input gives bit-identical answers on every host.

### Axes and vectors

- [cw_action_strength](api:cw_action_strength): 0..1, rescaled past the action's deadzone.
- [cw_input_axis](api:cw_input_axis): `strength(pos) - strength(neg)`, -1..1.
- [cw_input_vector](api:cw_input_vector): a 2D vector from four actions' raw strengths, with a deadzone mode: `AXIAL` (per component, good for menus), `RADIAL` (cuts small vectors, keeps the rest), `SCALED_RADIAL` (smooth from zero; the usual choice for movement). The vector is screen space, `y` down; for 3D movement on the ground with the camera looking down -Z, use `x` as +X and `y` as +Z.

## Touch controls

[cw_touch_add_button](api:cw_touch_add_button) and [cw_touch_add_stick](api:cw_touch_add_stick) define regions in logical window coordinates. A touch that starts inside a control belongs to it until lifted, so sliding off a stick keeps steering. A stick can be floating (centred where the touch started). Controls are bound like any device (`touch:0.left`), so a game written for a pad plays on a phone by adding bindings. The engine does not draw controls; `cw_touch_stick_value` and `cw_touch_control_active` give what a HUD needs.

### Touch as the pointer

A game built around the mouse (drag, click, release) can take touch without adding controls: set `touch_pointer` in [cw_app_desc](api:cw_app_desc). The first finger down becomes the mouse's left button at that finger's position, drags with it and releases when it lifts; it stays the pointer until then, whatever other fingers do. A tap that lands and lifts within one frame still clicks (held for that frame, released on the next). Other fingers stay in `touches[]` of [cw_input_raw](api:cw_input_raw), so a second finger can pinch, and `mouse_from_touch` says the pointer came from a finger this frame (show touch prompts). The mapping happens in the platform layer after the devices are read, so it is the same on every host: SDL touch natively, a cart host's pointer slots 1 to 9 (slot 0 stays the mouse), and the `t<id>=` fields of the CW_INPUT test script (`"touches"` in run-cart scripts).

### Touch-first devices

[cw_platform_touch_primary](api:cw_platform_touch_primary) says the device is a phone or tablet and no gamepad has been used, so a game can show touch prompts or controls from the first frame instead of waiting for a tap. Android and iOS builds report it; a desktop does not (SDL2 cannot tell whether a mouse is attached; `CW_TOUCH_PRIMARY=1` forces it natively). A cart cannot ask its host, so the host tells it through the debug channel: the web export's player sends `touch_primary 1` before the first frame when the browser's pointer is coarse and no fine pointer exists, and tests pass it with `run-cart --start-cmds "touch_primary 1"` (a gate's `startCmds`). Other cart hosts (romdev, wasmcart-run, wasmcart-android) leave it false. Any pad button or stick turns it false for the rest of the run; a pad merely reported present does not.

wasmcart's own doctrine (its SPEC, the manifest `controls` field) is that the host draws on-screen touch controls from the manifest's hint and carts do not adapt to it; wasmcart-android draws such an overlay. A {{name}} game that draws its own touch controls can double up with one, so keep them behind a single switch the game (or a later host hint) can turn off.

## Rebinding and persistence

All rebinding calls ([cw_action_bind](api:cw_action_bind), `cw_action_set_binding`, `cw_action_unbind`, `cw_input_reset_to_defaults`) fire one bindings-changed callback. The game owns persistence: the engine never writes files (a cart cannot). The usual pattern is to save only what differs from the committed defaults:

@include samples/input_demo/main.c#persist

A rebinding screen listens for the next press with capture. `input_demo` binds whatever is pressed next to its `special` action:

@include samples/input_demo/main.c#rebind

For a ready-made control, the UI module has a remap button node ([RemapButton](node:RemapButton)).

## The project input map

Projects from editor templates keep their actions in `project.cwproj`, one `input.<action> = ...` line each (the binding-set format). [cw_project_load_input](api:cw_project_load_input) loads them all and commits them as defaults, and the editor's project settings edit the same lines. The 2D platformer template:

@include editor/templates/platformer_2d/project.cwproj

## Record and replay

A recording is the tick snapshot of every action of every player, delta-encoded: an idle tick costs well under a byte. It carries the action names, so it replays into any build that defines them. While a replay is active every tick takes its snapshot from the recording instead of devices; [cw_input_replay_step](api:cw_input_replay_step) advances it immediately, so a game can re-simulate a whole run inside one frame. `input_demo` records from its first tick and, on "verify", replays the recording into a fresh state and compares:

@include samples/input_demo/main.c#replay

The `input-pad` gate drives it from a script and requires the replayed state to equal the live one:

```sh
node tools/run-cart.mjs --host native --app input_demo --frames 100 --input tests/input-pad.json --state out/inp.json
```

```text
"sim.jump_jp":3,"sim.jump_ticks":32,"ui.jump_jp":3,"ui.jump_frames":16,"record.ticks":179,"record.bytes":317,"replay.match":1,"replay.hash_match":1
```

179 ticks of input took 317 bytes. Every app with actions also publishes `input.hash` and `input.ticks` as debug fields (the hash of every tick snapshot so far), and the `record start|stop|status|fetch|replay` debug command records and replays from outside the game, which is how a bug report carries its exact input.

## Rumble

A game drives a player's pad motors with [cw_input_rumble](api:cw_input_rumble): `low` is the heavy motor, `high` the light one (both 0 to 1, out of range clamps), `ms` how long (at most 5000). A new call replaces the one running and [cw_input_rumble_stop](api:cw_input_rumble_stop) ends it. The player's pad is the one [cw_input_assign](api:cw_input_assign) gave them, so a split-screen game buzzes only the pad of the player who was hit.

```c
if (hard_landing) cw_input_rumble(player, 0.6f, 0.2f, 140);
if (boosting)     cw_input_rumble(player, 0.15f, 0.35f, 100); /* re-issued each few ticks while it lasts */
```

Every call is safe on any setup: no pad, a pad without motors, keyboard only, or a host without hardware all do nothing. Ask [cw_input_has_rumble](api:cw_input_has_rumble) only to offer the player an on/off setting. Natively it is SDL's controller rumble; in a cart it reaches the host through wasmcart's `wc_pad_rumble`, which a browser plays as a dual-rumble effect. Rumble is presentation only: it never feeds into input or the simulation, so it is fine to call from `fixed_update` (and a replay or a rollback re-simulation should not call it again for ticks it re-runs). The debug field `cw.rumble` (request count, pad, low and high in thousandths, ms) shows the last request to tests; the `input-pad` gate checks it on every host.

## Raw input

[cw_input_raw](api:cw_input_raw) (in [cartwheel/platform.h](header:platform.h)) is the per-frame device snapshot the actions are built from: keys by scancode, mouse, wheel, ten touches, four pads. [cw_set_raw_input_filter](api:cw_set_raw_input_filter) rewrites it before actions are built, at the same point on every host, which is how an autopilot or demo player feeds an input-free host. Text entry (typed, composed text for text fields) arrives as events, not actions; see [UI](guide:ui).
