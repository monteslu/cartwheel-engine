# Input

Games ask about named actions ("jump", "move_left"), never about devices. Keys, mouse buttons, the wheel, pad buttons, pad axes and on-screen touch controls are bindings of those actions, rebindable at run time, and the stream of action states can be recorded and replayed bit for bit.

Header: [cartwheel/input.h](header:input.h); showing a player which button does what: [cartwheel/prompt.h](header:prompt.h) (below). Sample: `input_demo` (every feature below, at a 120 Hz tick so each 60 fps frame runs two ticks). Gates: `input-pad`, `input-keys`, `input-pointer`, `unit-input`; `games/action2d` plays the same run on a pad and on a keyboard and must end in the same state (`action2d`, `action2d-keys`).

## Actions and bindings

[cw_action_add](api:cw_action_add) defines an action with a deadzone and returns its index (a plain `int`; -1 is "no action" and every query on it answers false or 0). Look actions up once and keep the index: a query is an array read.

Bindings have a text form, used everywhere (binding files, `project.cwproj`, saved settings, the debug channel):

| Text | Binding |
|---|---|
| `key:space`, `key:a`, `key:44` | a key by SDL scancode name or number |
| `mouse:left`, `wheel:up` | a mouse button; a wheel direction (pressed on frames the wheel moves) |
| `pad:south`, `pad:start`, `pad:up` | a pad button, by position: `south` is the bottom face button on every pad |
| `axis:lx-`, `axis:rt+@0.5` | a pad axis direction, with an optional threshold (triggers are axes) |
| `touch:1`, `touch:0.left` | an on-screen touch button, or one direction of a touch stick |

A binding set is one action per line; a line replaces that action's bindings, and `deadzone:0.3` may appear in the list. `input_demo` ships these defaults:

@include samples/input_demo/assets/bindings.txt

Load defaults, commit them, then apply the player's saved overrides on top:

@include samples/input_demo/main.c#bindings

## Players and devices

Up to four players. By default player 0 owns pad 0, the keyboard, the mouse and touch, and players 1 to 3 own pads 1 to 3. [cw_input_assign](api:cw_input_assign) changes that; a device may be shared. Every query takes the player index.

### More players than pads

`CW_INPUT_MAX_PLAYERS` is eight while a host has four pads: players past the pads are CPU seats ([cw_input_set_actions](api:cw_input_set_actions)) or people who take turns. [cw_input_assign_pads](api:cw_input_assign_pads) gives a player several pads at once (a bit per pad; each binding reads the strongest), which is how the shell's hot seats hand every free pad to the seat whose turn it is ([The game shell](guide:game-shell)). When a player's devices change, what is already held on them is held, not a new press, so the button that ended one turn is not the next player's first press. The change takes effect from the next frame. A recording made with four players replays on eight.

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

The usual way is a **thumbstick** with a home in the bottom left corner and **round buttons** in the bottom right: [cw_touch_add_thumbstick](api:cw_touch_add_thumbstick) and [cw_touch_add_round_button](api:cw_touch_add_round_button) place them in touch units (1/540 of the screen's shorter side, [cw_touch_unit](api:cw_touch_unit)) from a corner, lay them out again every frame so they follow a resize or a turn of the phone, and the engine draws them (base, ring, knob; disc and label) over the frame, translucent ([cw_touch_set_overlay](api:cw_touch_set_overlay): shown when [cw_platform_touch_primary](api:cw_platform_touch_primary) or once a touch lands, hidden while a pad is in use: a button, or a stick or trigger past half way, so a phone with a controller plugged in shows them until the controller is used and brings them back on the next touch). A thumbstick's ring stays at its home: a thumb landing anywhere in its zone steers relative to the home, so the drawn ring never jumps to where the thumb landed. A touch that starts inside a control belongs to it until lifted, so sliding off a stick keeps steering. Controls are bound like any device (`touch:0.left`), so a game written for a pad plays on a phone by adding bindings.

The older rect controls, [cw_touch_add_button](api:cw_touch_add_button) and [cw_touch_add_stick](api:cw_touch_add_stick), are regions in logical window coordinates that the game lays out and draws itself (the engine does not draw them). A rect stick added with `floating` keeps its ring at a home two radii in from the rect's bottom left corner and steers from there, like a thumbstick; [cw_touch_set_floating](api:cw_touch_set_floating) brings back the older feel, the ring centred where the touch started. [cw_touch_home](api:cw_touch_home), [cw_touch_stick_center](api:cw_touch_stick_center), `cw_touch_stick_value` and `cw_touch_control_active` give a HUD what it needs to draw one.

### In a cart the host draws the pad

In a cart (a `.wasc`) the wasmcart host draws every on-screen pad control, so one place draws the pad and a phone never shows two sets. There the touch controls above claim no touch, so every touch reaches the game as gameplay (the pointer with `touch_pointer`, and `touches[]` of [cw_input_raw](api:cw_input_raw)); the engine draws no overlay, and [cw_touch_overlay_visible](api:cw_touch_overlay_visible) is false. [cw_touch_controls_on](api:cw_touch_controls_on) says which case a build is in: true natively (desktop, Android, iOS), where everything above holds, false in a cart. A game that draws its own controls (rect controls, or anchored ones marked `hidden`) draws them only while it is true. The gate switch `CW_TEST_TOUCH_CONTROLS=1` turns them back on in a cart (a test's control, not for shipping). A cart host delivers it from frame 1, so a test reads it in a frame, not in init. `CW_TEST_CART_TOUCH=1` gives a native run the cart's rule (the controls off unless the switch is set), which is how CI, with no cart host, checks a test against it (gate `unit-input-cart-touch`).

The host draws what the cart's manifest asks for: its `controls` field (wasmcart SPEC), the parts of the standard pad the game reads. `cw_add_app` (cmake/app.cmake) works it out from the project's input map in `project.cwproj`:

| binding | manifest token |
|---|---|
| `pad:south`, `pad:east`, `pad:west`, `pad:north` | `a`, `b`, `x`, `y` |
| `pad:up`, `pad:down`, `pad:left`, `pad:right` | `dpad` |
| `axis:lx`, `axis:ly` / `axis:rx`, `axis:ry` | `left_stick` / `right_stick` |
| `axis:lt` / `axis:rt` | `left_trigger` / `right_trigger` |
| `pad:lshoulder` / `pad:rshoulder` | `l` / `r` |
| `pad:start` / `pad:back` | `start` / `select` |
| `pad:lstick` / `pad:rstick` | `l3` / `r3` |

Every `pad:` and `axis:` binding of every `input.<action> = ...` line counts, and so do the shell's menus and pause (the d-pad, `a`, `b` and `start`). A project key `controls = a, b, start` replaces the list; `controls = none` writes an empty list, for a game whose touch is all its own (a drawing game, a puzzle played by tapping), so the host draws no pad. Both projects count: the packed one the game runs with (beside the `PACK` list, usually where the input map is) and the one beside `CMakeLists.txt`, whose `controls` key wins, and `input.cfg` beside the `PACK` list, the bindings file a game loads itself with [cw_input_load_bindings_asset](api:cw_input_load_bindings_asset) (its `action = bindings` lines count as the input map; a bindings file by another name needs a `controls = ...` key). A project without an input map leaves the field out and the host picks its default. An app without a project passes the list to `cw_add_app(... CONTROLS dpad a b start)` (or `CONTROLS none`). `node tools/cart-controls.mjs --bindings assets/input.cfg assets/project.cwproj project.cwproj` prints what a game gets.

### Touch as the pointer

A game built around the mouse (drag, click, release) can take touch without adding controls: set `touch_pointer` in [cw_app_desc](api:cw_app_desc). The first finger down becomes the mouse's left button at that finger's position, drags with it and releases when it lifts; it stays the pointer until then, whatever other fingers do. A tap that lands and lifts within one frame still clicks (held for that frame, released on the next). Other fingers stay in `touches[]` of [cw_input_raw](api:cw_input_raw), so a second finger can pinch, and `mouse_from_touch` says the pointer came from a finger this frame (show touch prompts). The mapping happens in the platform layer after the devices are read, so it is the same on every host: SDL touch natively, a cart host's pointer slots 1 to 9 (slot 0 stays the mouse), and the `t<id>=` fields of the CW_INPUT test script (`"touches"` in run-cart scripts).

### Touch-first devices

[cw_platform_touch_primary](api:cw_platform_touch_primary) says the device is a phone or tablet and no gamepad has been used, so a game can show touch prompts or controls from the first frame instead of waiting for a tap. Android and iOS builds report it; a desktop does not (SDL2 cannot tell whether a mouse is attached; `CW_TOUCH_PRIMARY=1` forces it natively). A cart cannot ask its host, so the host tells it through the debug channel: the web export's player sends `touch_primary 1` before the first frame when the browser's pointer is coarse and no fine pointer exists, and tests pass it with `run-cart --start-cmds "touch_primary 1"` (a gate's `startCmds`). Other cart hosts (romdev, wasmcart-run, wasmcart-android) leave it false. Any pad button or stick turns it false for the rest of the run; a pad merely reported present does not.


## Rebinding and persistence

All rebinding calls ([cw_action_bind](api:cw_action_bind), `cw_action_set_binding`, `cw_action_unbind`, `cw_input_reset_to_defaults`) fire one bindings-changed callback. The game owns persistence: the engine never writes files (a cart cannot). The usual pattern is to save only what differs from the committed defaults:

@include samples/input_demo/main.c#persist

A rebinding screen listens for the next press with capture. `input_demo` binds whatever is pressed next to its `special` action:

@include samples/input_demo/main.c#rebind

For a ready-made control, the UI module has a remap button node ([RemapButton](node:RemapButton)): it shows the binding as a button prompt beside its name.

## Showing controls

A prompt tells the player which button does something. Never write a button's name or colour into a prompt: "A: jump" is the right button on one maker's pad, the wrong one on another's (the bottom button carries a different letter there, and the letter A sits on the east button), meaningless on a keyboard or a phone, and unknown on hosts that cannot say which pad is plugged in. Name the action instead and let the engine draw it for the device the player holds: [cartwheel/prompt.h](header:prompt.h).

@include samples/prompts/main.c#hint

[cw_prompt_text](api:cw_prompt_text) draws text with glyphs in it. `{jump}` is an action's glyph; `{move_left|move_right}` a pair, and `{left|right|up|down}` four directions, drawn as one stick or d-pad glyph; `{pad:west}` one physical input, for a design about position; `{{` and `}}` are braces. [cw_prompt_action](api:cw_prompt_action), [cw_prompt_actions](api:cw_prompt_actions) and [cw_prompt_binding](api:cw_prompt_binding) draw a single glyph, and each has a `_size` call that measures without drawing.

@image docs/images/prompts.png Every glyph on a dark and a light strip, the face-button diamond from 10 to 48 canvas pixels, and a player's prompts on a pad, a keyboard, a mouse and a touch screen (the prompts sample, `samples/prompts`).

| Input | Glyph |
|---|---|
| south, east, west, north | a diamond of four dots, the one to press solid, the other three rings |
| shoulders | two flat bars on top of a pad's outline, the left or right one solid |
| triggers | two tall tabs, the left or right one solid |
| a stick | a thumbstick seen from the front and a little above (a flat socket, a short post, a wide cap), with the other stick smaller and dimmer beside it: the left stick has its twin on its right, the right stick on its left. Arrows show the direction: beside the cap for left and right, above it for up, under the socket for down |
| a stick press | the stick with its cap pushed down and an arrow pointing down onto it |
| the d-pad | a plus, the arms to press solid |
| Start | the word "Start" in a pill |
| Back | two small pills, the left one solid (the makers name this button differently, so no word is used) |
| a key | a keycap with [cw_binding_display_name](api:cw_binding_display_name)'s text (a single character larger and heavier than a word, so it reads at the minimum size); the arrow keys draw an arrow |
| the mouse | a mouse with the bound button solid, or the wheel and its direction |
| touch | the on-screen control as the touch overlay draws it: a button's disc and label; a stick's knob on its filled base, the arrows inside the base. In a cart, where the host draws the on-screen pad ([cw_touch_controls_on](api:cw_touch_controls_on) false), the pad glyph of the action's pad binding instead: the host's button is what the player presses |

Every glyph is vector shapes from render2d, so it is sharp at any size and the same on GL and WebGPU. Solid means "this one" and an outline "not this one", so nothing depends on colour. Each glyph sits on its own dark backing, so it reads on light and dark backgrounds. The minimum size is 1.5% of the canvas height (16 px at 1080 lines): below it keycap text, the word "Start" and stick arrows close up (the diamond alone still reads at 14 px). Prompts sit in text, so give a prompt line the body text size, 2.4% of the canvas height or more. The player's text scale grows prompts with the text they sit in.

The rules, each with a test in `unit-prompt` and the `prompts-*` gates:

- **Which device.** The one the player used last (a pad button, a stick or trigger past half way, a key, a mouse button or the wheel, a touch landing), from the frame snapshot. Before anything is used: the pad when it is connected, else touch on a touch-first device, else the keyboard. [cw_prompt_device_of](api:cw_prompt_device_of) answers it; a game never tracks the last device itself.
- **Which binding.** The first one, in binding order, on that device: `key:space, pad:south` draws the diamond for a pad player and "Space" for a keyboard player. The keyboard and the mouse stand in for each other.
- **Rebinding.** A prompt reads the bindings when it draws, so a player who moves jump to the north button sees north lit, everywhere, on that frame.
- **Nothing on the device.** An action with no binding there draws nothing and logs a warning once, naming the action and the device. It never guesses.
- **A typo.** `{jmup}` draws nothing, logs once and counts in the debug field `prompt.errors`; `tests/gate.mjs` fails every gate whose run has one.
- **Movement.** A pair or four directions draw one glyph when their pad bindings agree. When every action is on the d-pad the glyph is the d-pad with those arms solid, even when a stick is bound too: `{move_left|move_right}` lights left and right, `{move_up|move_down}` up and down, all four light every arm. Every pad has a d-pad, so this is the glyph any player can follow, whichever of the two they last steered with. Only actions on a stick and not the d-pad (right-stick aiming, looking round) draw the stick. One direction alone on both, such as `{lean_fwd}`, lights its one arm.
- **Shared prompts.** Player `-1` is every present seat: a title or instructions screen shows the pad glyph if any seat is on a pad, then the keycap if any is on the keyboard, then the touch control.
- **Determinism.** Prompts are presentation. Nothing here enters the simulation, the recording or `input.hash`. Calling them from `fixed_update` asserts in a debug build and counts in `prompt.errors` in a release build.
- **Two colours.** A game drawn in two colours only (ink and paper: a one-bit look) calls [cw_prompt_set_two_colour](api:cw_prompt_set_two_colour)(true, ink, paper): every glyph's fill, legend and words are ink, its backing and the words' outline paper, both opaque, with one-bit edges (a pixel half covered or more is ink, else nothing; [cw_r2d_set_one_bit](api:cw_r2d_set_one_bit)), so every pixel a prompt draws is one of the two. The colour passed to a draw call is not used then, and the in-between tones (a dimmer stick, a touch tint) go. It stays until changed; [cw_prompt_two_colour](api:cw_prompt_two_colour) reads it. The game shell's two-colour menus draw their prompts in their own ink and paper and give the game's setting back when they close. Sample `r2d_onebit`, gate `r2d-one-bit` ([2D rendering](guide:rendering-2d), "One-bit edges").
- **Tests.** `CW_TEST_PROMPT_DEVICE=pad|keyboard|mouse|touch` forces every player's device in a native run; the debug request `prompt device <player|all> <device|auto>` does it in a cart (a gate's `startCmds`); an input script's first event on another device switches it as a player would.

@include samples/prompts/main.c#shared

When the design is about position (lanes, answer slots, a dance mat), lay it out by position and draw each slot's glyph with `{pad:west}`, `{pad:south}`, `{pad:east}`, so the slot, the glyph and the button all point the same way:

@include samples/prompts/main.c#lanes

### What a game may assume

Every pad the engine meets has four face buttons in a diamond, a d-pad, two shoulders, Start and Back: wasmcart's own default set (its SPEC, the manifest's `controls`). Confirm is south and back is east on every pad; the engine does not swap them for any maker.

Players use the left stick and the d-pad interchangeably, so every movement action binds to both, on the same action: `move_left = key:a, key:left, pad:left, axis:lx-`. A player then steers with either, and the prompt draws the d-pad (see Movement above). The right stick is its own control: aiming and the camera live there with no d-pad binding.

### The rules for games

1. Never write a button's name or colour in text or art. `{jump}: hop a gap`, not `A: hop a gap`. "Start" is the one exception: every pad has that button, and on a keyboard the prompt shows its key.
2. Colours belong to the game, not the controller. A green lane is fine; "press green" is not, and neither is a green dot meaning the south button.
3. When a design is about position, lay it out by position.
4. Text says what the button does ("checkpoint"); the glyph says which button.
5. Do not track the last device yourself. Ask [cw_prompt_device_of](api:cw_prompt_device_of).
6. One string serves every device: the markup draws keys on a keyboard and the touch control on a phone with no extra work.

`node tools/check-prompts.mjs src` (it ships in the SDK) fails on the usual slips in string literals, scene files and catalogs: `A:`, `(A)`, `press A`, `A or`, `A B X Y`, `LB`, `RT`, `L1`, `ZR`, `Cross`, `green button`. A line that really means it carries `prompt-ok: <reason>` on it or on the line above. It cannot see letters drawn in art or in code (a function that draws a letter in a circle), so review those by hand, and move keyboard hints such as "W / S: gas" to the markup too.

## The project input map

Projects from editor templates keep their actions in `project.cwproj`, one `input.<action> = ...` line each (the binding-set format). [cw_project_load_input](api:cw_project_load_input) loads them all and commits them as defaults, and the editor's project settings edit the same lines. The 2D platformer template:

@include editor/templates/platformer_2d/project.cwproj

## Aiming with a stick

A raw stick is a poor aim control: a dead zone you notice, a response too touchy near the middle and too slow at the edge. [cw_aim_step](api:cw_aim_step) ([cartwheel/aim.h](header:aim.h)) turns the stick vector into the radians to turn this tick:

- a radial dead zone, rescaled so the response starts at zero (no jump at the edge of the zone),
- a response curve, `power`: above 1 gives fine control near the middle and full speed at the edge, below 1 is twitchy,
- a top turn rate for each axis (pitch is usually slower than yaw),
- acceleration: holding the stick at its edge ramps the turn rate up over `ramp_seconds` to `1 + ramp_gain` times, so a flick turns fast and small corrections stay precise,
- an assist: while `on_target` is set (the crosshair sits on something to aim at) the turn is slowed by up to `assist`.

```c
static cw_aim_state aim[4];                          // one per player
cw_aim_curve curve = cw_aim_curve_default();
cw_vec2 stick = cw_input_vector(p, look_l, look_r, look_u, look_d, CW_DEADZONE_RADIAL, 0);
cw_vec2 turn = cw_aim_step(&aim[p], &curve, stick, 1.0f / 60, crosshair_on_enemy ? 1 : 0);
yaw -= turn.x;
pitch -= turn.y;
```

It uses only + - * / and `cw_pow`, so a replay turns the same on every host. The `unit-input` gate checks the dead zone, the curve, the ramp and the assist.

## Cursors between targets

A board, a market of cards, a map of build pads or a network of cities is a set of spots, not a grid of UI controls. [cartwheel/cursor.h](header:cursor.h) gives each player a cursor that hops between such spots by stick direction, all players at once, and that a mouse or finger moves by pointing.

- The targets are points ([cw_cursors_set_points](api:cw_cursors_set_points)) or nodes ([cw_cursors_set_nodes](api:cw_cursors_set_nodes): their positions are read at each step, so targets on moving pieces move with them).
- A hop goes to the target that lies most nearly the way held: ahead of the cursor, at most `cone` times as far to the side as ahead (1.75: about 60 degrees either way), lowest `ahead + side_weight x sideways` winning (2.2). A near target a little off the line beats a far one dead ahead. Nothing that way: the cursor stays.
- The stick snaps to eight ways past `deadzone`. A hop happens on the tick a way is first held, then every `repeat_every` ticks once it has been held `repeat_delay` ticks (18 and 9 by default).
- Directions are screen directions. Give each player their view ([cw_cursors_set_view](api:cw_cursors_set_view): the camera and the rectangle its picture fills) and 3D targets are projected through that camera, so up on the stick is up on that player's picture, also in split screen with the cameras on opposite sides of the table. Without a view, 2D targets read x right and y down, and 3D ones the board plane seen from above.
- [cw_cursors_set_allow](api:cw_cursors_set_allow) says which targets a player may stand on (their own pieces, empty squares); a cursor left on one it may no longer use goes to the nearest it may.
- [cw_cursors_point](api:cw_cursors_point) moves a player's cursor where the pointer is: with node targets, onto the node a 3D pick ([Picking and projection](guide:rendering-3d)) lands on or under, else onto the target nearest the point within `pick_radius` pixels.

`samples/board_pick` moves two players' cursors over the 36 squares of a board, each player's view set when the split screen is laid out:

@include samples/board_pick/main.c#views

and each tick:

@include samples/board_pick/main.c#cursors

[cw_cursor_hop_select](api:cw_cursor_hop_select) is the hop alone, for a simulation that keeps its own cursor state (a replay, a CPU player choosing the way to press). Everything is + - * / and square roots on the targets' places, so cursors hop the same on every host. The `board-cursor` gate hops two players' cursors at once; its control reads the sticks mirrored and lands on other squares. `unit-pick3d` checks the scores, the cone, the repeat timing, the allow rule and the view turning the stick round.

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

### The pointer channel: clicks and taps

A game played with the mouse or a finger (a board game picking squares and cards, a cursor following the pointer) reads the pointer in `fixed_update` with [cw_input_pointer](api:cw_input_pointer): one snapshot a tick, like the actions. It says whether the pointer is there (`present`, the mouse over the window or a finger down), `down`, `pressed` (a click or a finger landing since the tick before, a press and release inside one tick included), `released`, `touch`, `secondary` (the right button) and `pos` in logical window coordinates (where a press landed, on a tick with one). Outside a tick it answers for the frame.

Turn the channel on with [cw_input_pointer_channel](api:cw_input_pointer_channel)`(true)` in `init`, before a recording begins: the pointer is then recorded (format version 2), replayed and hashed with the actions, so a match won with clicks replays exactly, and the shell's menus answer taps from the same snapshot. `board_kit` turns cards over with clicks it reads only this way:

@include samples/board_kit/main.c#pointer

Off (the default) nothing changes: the stream hash, the record hash and the recordings are what they were (version 1, byte for byte), so no game's golden input hash moves until it turns the channel on. Every recording replays: version 1 has no pointer (a replay gives one that is not present), version 2 does, also into a build with the channel off. An idle pointer records nothing; a moving one costs a few bytes a tick. Positions are kept in steps of 1/16 of a logical pixel, so a replay gives the same position on every host.

The `board-kit-pointer` gate clicks two cards and the table, replays the match from its own recording and requires the same cards turned and the same input hash; its control (`CW_TEST_POINTER_UNRECORDED=1`) leaves the pointer out of the recording and the replay loses every click. `unit-input-pointer` checks random clicks, drags, the right button and finger taps over frames with no tick and with several, the channel off, and an old recording.

## Actions set by the game: CPU seats

A computer player decides inside `fixed_update` and presses its seat's actions for that tick, as a pad would: [cw_input_set_actions](api:cw_input_set_actions) sets the whole seat (everything it does not list is up), [cw_input_set_action](api:cw_input_set_action) one action, and [cw_input_set_vector](api:cw_input_set_vector) a stick onto four actions. The same queries read them back (`cw_action_pressed`, `cw_action_just_pressed` against the tick before, strength, axis, vector), so the game's rules read every seat one way and never ask who is a CPU. A board game with four CPUs plays through exactly the code people use, and a test can drive any seat the same way.

`hex_board` presses one action at a time, down for one tick and up again, so every press is a fresh `just_pressed`:

@include samples/hex_board/main.c#cpu_presses

The CPUs press first in the tick, then the rules read every seat:

@include samples/hex_board/main.c#tick

What a set does:

- `cw_input_set_actions` releases every action of the seat first, so on a seat that also has a pad, keyboard or touch (a test bot or an autopilot helping a person) it swallows every button the person holds that the call does not list, each tick it is called. There, set only the actions the bot drives with `cw_input_set_action` (or `cw_input_set_vector`); the others keep what the devices gave. `cw_input_set_actions` is for a seat with no devices: a CPU seat, or a test that owns the seat whole.
- It replaces what that player's devices gave the action for this tick only. The next tick starts again from the devices, so an action held for many ticks is set every tick. A CPU seat has no devices: the shell assigns it `cw_input_assign(seat, -1, 0)` ([The game shell](guide:game-shell)).
- Strength is 0 to 1, quantized like a pad's; it counts as pressed at or above the action's dead zone.
- It works only inside `fixed_update` (the tick context); anywhere else it returns false and logs once.
- A tick is hashed and recorded when it ends (at the next tick, or at the end of the frame's ticks), so everything set during `fixed_update` is in `cw_input_stream_hash`, the recording and its hash. A run with no sets hashes as before.
- During a replay the recording already holds what was set: the calls change nothing and return false. The game's CPU code can keep running (its decisions come out the same) or not run at all.

[cw_set_raw_input_filter](api:cw_set_raw_input_filter) is still the tool for an autopilot that stands in for the devices once a frame; a CPU that decides per tick, possibly several ticks in one frame, sets actions instead.

The `hex-board-replay` gate plays a person on a pad against three CPUs, records the match, replays it and requires the same board, CPU state and input hash. Its control (`CW_TEST_SET_UNRECORDED=1`) leaves what the game set out of the recording, as a CPU that bypassed the input path would, and the replay loses the CPUs' moves. `unit-input-set-actions` checks the rules above and the same replay with and without the CPU code running.

## Rumble

A game drives a player's pad motors with [cw_input_rumble](api:cw_input_rumble): `low` is the heavy motor, `high` the light one (both 0 to 1, out of range clamps), `ms` how long (at most 5000). A new call replaces the one running and [cw_input_rumble_stop](api:cw_input_rumble_stop) ends it. The player's pad is the one [cw_input_assign](api:cw_input_assign) gave them, so a split-screen game buzzes only the pad of the player who was hit.

```c
if (hard_landing) cw_input_rumble(player, 0.6f, 0.2f, 140);
if (boosting)     cw_input_rumble(player, 0.15f, 0.35f, 100); /* re-issued each few ticks while it lasts */
```

Every call is safe on any setup: no pad, a pad without motors, keyboard only, or a host without hardware all do nothing. Ask [cw_input_has_rumble](api:cw_input_has_rumble) only to offer the player an on/off setting. Natively it is SDL's controller rumble; in a cart it reaches the host through wasmcart's `wc_pad_rumble`, which a browser plays as a dual-rumble effect. Rumble is presentation only: it never feeds into input or the simulation, so it is fine to call from `fixed_update` (and a replay or a rollback re-simulation should not call it again for ticks it re-runs). The debug field `cw.rumble` (request count, pad, low and high in thousandths, ms) shows the last request to tests; the `input-pad` gate checks it on every host.

## Raw input

[cw_input_raw](api:cw_input_raw) (in [cartwheel/platform.h](header:platform.h)) is the per-frame device snapshot the actions are built from: keys by scancode, mouse, wheel, ten touches, four pads. [cw_set_raw_input_filter](api:cw_set_raw_input_filter) rewrites it before actions are built, at the same point on every host, which is how an autopilot or demo player feeds an input-free host. Text entry (typed, composed text for text fields) arrives as events, not actions; see [UI](guide:ui).
