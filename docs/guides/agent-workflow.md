# The agent workflow

An agent builds a {{name}} game in C against the public headers, runs it, and proves it works by asking the running game questions. This page is the loop: read the API as data, write C, run on a host, inspect and drive the game over the debug channel, hot reload, and turn what you checked into a gate.

## 1. Read the API as data

Two generated files describe everything an agent can call or build with. Both come from the source (`node tools/gen-api.mjs`); nobody edits them by hand.

| File | What it holds |
|---|---|
| `docs/api.json` | `headers[]`: each public header's `header` path and `summary`, then `functions` (`name`, `signature`, `doc`), `types` and `macros` |
| `docs/nodes.json` | `types[]`: each node type's `name`, `parent`, `doc`, `signals` and `properties` (`name`, `type`, `default`, range, enum names, `stored`, `readonly`, `doc`) |

A function entry looks like this:

```text
{"name":"cw_node_set","signature":"bool cw_node_set(cw_node n, const char *prop, cw_value v)",
 "doc":"properties by name: \"position\", \"material/color\" style paths reach into ...","inline":false}
```

The comments in `include/cartwheel/*.h` are the documentation: what a function does, what it costs and who owns what. The [C API reference](header:scene.h) and the [node reference](node:Node) are these files rendered.

## 2. Write the game in C

A game returns a `cw_app_desc` from `cw_main` and is called back; it never owns a loop. Most games run a scene tree through `cw_scene_app`, register their own node types and expose the state a test needs by name. A minimal node type and its scene app:

@include samples/hot_demo/main.c#node_type

@include samples/hot_demo/main.c#main

A method makes game code callable by name, with typed arguments, from the debug channel (and from triggers and scene connections):

@include samples/remote_target/main.c#boost

@include samples/remote_target/main.c#register_types

Exporting `cw_game_register_types` (every node type and method the game defines, nothing else) is what lets `cwhot` re-register types after a reload.

Rules that keep a game verifiable: randomness from `cw_rng` (or `cw_tree_rng`), time from the fixed `dt` only, trig from `cartwheel/math.h`, state that matters exposed with `cw_debug_i32` / `cw_debug_f32` (`cartwheel/debug.h`), and `.debug = true` in the app desc. The full list is in [Working on {{name}}](guide:conventions).

## 3. Run it and read its state

`tools/run-cart.mjs` runs an app for an exact frame count on any host and writes its named state ([Getting started](guide:getting-started)). Comparing the native state with the romdev or Chromium state is the first check that the game is deterministic.

Through the romdev MCP server, the same cart is driven step by step:

```text
loadMedia({ platform: 'wasmcart', path: '<abs path>/build/cart/carts/scene3d.wasc', deterministicSeed: 1 })
frame({ op: 'step', frames: 29 })
wasm({ op: 'debugState' })
wasm({ op: 'command', text: 'tree /root 2' })
host({ op: 'shutdown' })
```

romdev's load runs frame 1, so stepping N-1 frames lands on the same frame as a native `CW_FRAMES=N` run. `wasm({op:'debugState'})` returns every `cw_debug_*` field by name, plus the channel's own `dbg.*` mailbox fields. `tools/run-cart.mjs --host romdev` shows how input scripts map onto romdev's input tool (pads, pointer and wheel; it has no keyboard op, so key scripts run on the other hosts).

## 4. Ask the game: the debug channel

A running app with `.debug = true` answers text requests and replies with JSON. A node is addressed as `#uid`, an absolute path (`/root/Level/Player`) or a path from the root (`Level/Player`). Each request costs one frame of the running game, so freeze it first with `pause` when that matters.

| Where | How to send a request |
|---|---|
| romdev | `wasm({op:'command', text:'tree /root 2'})`; give `maxFrames` larger than N for `step N` |
| native, in a test | `CW_FRAMES=30 CW_DEBUG_CMDS='cmd1;cmd2' <exe>`: runs after the last frame, one reply per line |
| native, live | `CW_REMOTE=tcp:HOST:PORT` or `fd:N`: length-prefixed frames, any number in flight (`cartwheel/remote.h` launches a game this way) |

Real requests and replies, natively, from `samples/remote_target` after 30 frames:

```sh
CW_FRAMES=30 CW_DEBUG_CMDS='methods #4;call Level/Mover boost 10;get Level/Mover speed position;set Level/Mover speed 5;pause' build/native/samples/remote_target
```

```text
{"methods":[{"name":"boost","type":"Mover","args":"float","doc":"add to speed; returns the new speed"},{"name":"teleport","type":"Mover","args":"vec2","doc":"jump to a position"}]}
{"called":"boost","result":"43.7","result_type":"float","tick":30}
{"uid":4,"tick":30,"values":{"speed":{"value":"43.7","type":"float"},"position":{"value":"vec2(36.85001, 100)","type":"vec2"}}}
{"value":"5","type":"float","uid":4,"revision":2}
{"frame":30,"tick":30,"time":0.483333,"paused":true,"steps_left":0,"tree_paused":false,"tree_tick":30,"nodes":6,"revision":2,"scene":"main.cwscene"}
```

And through romdev on the `scene3d` cart, after `pause`:

```text
wasm({op:'command', text:'step 10', maxFrames: 20})
{"framesRun":11,"reply":{"frame":41,"tick":40,"time":0.666667,"paused":true,"steps_left":0,...}}
```

A refusal is exactly `{"error":"..."}` and names the fix:

```text
{"error":"no node at '/root/Nope' (address nodes as #uid, /root/Level/Player or Level/Player; `tree` lists them)"}
```

The commands every debug app has, and what scene apps and modules add (`help` lists exactly what a given game answers, `help <command>` one usage line):

| Group | Commands |
|---|---|
| always | `help`, `status`, `stats`, `fields [name...]`, `pause [1\|0]`, `resume`, `step [ticks]`, `screenshot <world\|ui\|debug\|all> [scale]`, `screenshot_get <id> [offset]`, `record start\|stop\|status\|fetch\|replay`, `vfs ...`, `quit`, `debug_crash` |
| scene apps | `tree [node] [depth]`, `node`, `changes <revision>`, `get`, `set`, `props`, `methods`, `call`, `emit`, `spawn <scene\|Type> <parent> [name]`, `free`, `save [node]`, `types`, `group <name>`, `tree_pause`, `change_scene`, `reload_scene` |
| modules | `phys`, `impulse`, `res`, `reload`, `anim`, `anim_play`, `anim_stop`, `anim_seek`, `anim_text`, `animtree`, `animtree_param`, `animtree_travel`, `pipeline`, `lighting`, `environment`, `plugins`, `l10n`, `a11y` |
| cwhot | `hot_reload [path]`, `hot_status` |

Values use the scene-file text form: `2.5`, `true`, `vec3(1, 2, 3)`, `color(1, 0, 0, 1)`, `"text"`, an enum name, `#uid`. An argument with spaces is quoted as a whole (`"a b"`); a value like `vec3(1, 2, 3)` keeps its spaces until the closing parenthesis. `pause` / `step` freeze the whole simulation (the debugger's pause), unlike `tree_pause`, which is the game's own pause menu flag. `screenshot` and `step` reply a frame or more later. A game adds its own commands with `cw_debug_command`. [The debug channel](guide:debug-channel) is the full reference.

## 5. Change code without restarting: cwhot

Natively, `cw_add_app` can also build a game as a loadable module (`<app>_hot.so`, not built by default). `cwhot` runs it and swaps in new code when the file changes, keeping everything in engine memory: the tree, node data, resources, physics worlds.

```sh
ninja -C build/native cwhot hot_demo_hot.so
build/native/cwhot build/native/samples/hot_demo_hot.so
```

Edit the C, rebuild the module, and the running game picks it up within half a second. Over the channel:

```text
{"generation":0,"module":"build/native/samples/hot_demo_hot.so","last_error":""}
```

is `hot_status`; `hot_reload [path]` swaps now. A module that fails to load is reported and the old code keeps running. A module's own static variables do not survive a swap: keep game state in nodes or behind the app's `user` pointer. Node types re-register on reload (`cw_node_type_register` keeps the data layout and takes the new callbacks; a changed `data_size` or parent is refused):

@include samples/hot_demo/main.c#register_types

The module carries its app's three.c program tables (`cw_add_app` links them into `<app>_hot.so`, see [Rendering 3D](guide:rendering-3d)): `cwhot` links only the engine's own, and registers the module's from its `cw__app_program_tables()` when it loads, and each swap puts the new module's tables in their place. A module built by a project's own CMake (a `game_module` in `project.cwproj`) exports that function itself; `editor/tests/agent_project` shows how.

`tests/hot.mjs` is the gate: it swaps code at an exact frame (`CW_HOT_SWAP_AT`, `CW_HOT_SWAP_PATH`) and checks that the state carried on.

## 6. Make the check permanent

What an agent checked by hand becomes a gate in `tests/gates.json`: an app, a frame count, an optional input script, the named fields that must agree across hosts, expected values, and a control that must fail. See [Testing](guide:testing). A feature is done when its gate passes on every host.

## Rules of thumb

- Prefer a named value over a screenshot for any assertion about game state.
- A frame hash cannot prove a game runs: feed input and check that state moved.
- Pause before reading values that change every tick; each request runs a frame.
- When a gate's control passes, the gate cannot see anything: fix the gate first.
