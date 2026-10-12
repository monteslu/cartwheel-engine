# Debug channel

A running {{name}} game answers questions about itself: named debug state that tests compare across hosts, and a text request channel that inspects and edits the live scene, pauses and steps the simulation, takes layer screenshots and records input. The same requests work in a cart under romdev, natively from the command line, and from an editor driving a game in another process.

Headers: [cartwheel/debug.h](header:debug.h) (debug fields, commands, the debugger's pause, render layers) and [cartwheel/remote.h](header:remote.h) (launching and driving a game as a child process or a romdev cart). Samples: `remote_target` (a game made to be driven), `project_edit` (a custom command). Scripts: [tests/channel.mjs](repo:tests/channel.mjs) (the same requests native and cart must answer alike) and [tests/remote.mjs](repo:tests/remote.mjs) (play-in-editor on both transports), both run by [tests/all.mjs](repo:tests/all.mjs).

Turn it on with `.debug = true` in the app description. Every sample and game does.

## Named debug state

A debug field is a named pointer to values the game already keeps: `cw_debug_i32`, `cw_debug_f32`, `cw_debug_u32`, `cw_debug_f32_array`, `cw_debug_bytes`, or [cw_debug_field](api:cw_debug_field) for any type and length. Register in `init`; the name and pointer are kept, not copied, so both must outlive the app. Reading costs the game nothing: hosts read the memory when they ask.

`hello` publishes three fields:

@include samples/hello/main.c#main

and its `init` registers `x`, `angle` and `ticks`. In a cart the registry goes out through the wasmcart debug ABI and romdev reads it with `wasm({op:'debugState'})`; natively `CW_DEBUG_DUMP=file.json` writes every field when a `CW_FRAMES` run ends (`tools/run-cart.mjs --state` does both). The engine adds its own (`cw.frame`, `cw.tick`, `cw.pad0.*`, `tree.nodes`, `input.hash`, ...).

Prefer a named value over a screenshot for any assertion about game state: the gates compare named fields exactly across hosts and pixels only with a tolerance. Publish what a test or an agent needs to know by name (`player.x`, `coins`, `phys.hash`), including hashes that fold many values into one.

## Requests and replies

A request is `name arg arg...` (quote an argument containing spaces with `""`); a reply is JSON. A refusal is exactly `{"error":"..."}` and names what to do instead. `help` lists a game's commands with their usage, so an agent never has to guess.

Built in for every app:

| Command | What it does |
|---|---|
| `help [command]` | the command list with usage |
| `status`, `stats` | frame, tick, pause state; memory per tag and job counts |
| `fields [name...]` | debug field values |
| `pause [1 or 0]`, `resume`, `step [ticks]` | the debugger's pause (below) |
| `screenshot <layers> [scale]`, `screenshot_get <id>` | capture the next frame with only those layers (`world`, `ui`, `debug`, `all`, joined with `+`), fetch it as base64 PNG |
| `record <start, stop, status, fetch or replay>` | the input stream ([Input](guide:input)) |
| `vfs <ls, cat, write, rm, mv, export or import> ...` | the writable project layer ([App and platform](guide:app-platform)) |
| `quit`, `debug_crash` | end the game; crash it on purpose (crash-isolation tests) |

Scene apps add `tree`, `node`, `changes <revision>`, `get`, `set`, `props`, `methods`, `call`, `emit`, `spawn`, `free`, `save`, `types`, `group`, `tree_pause`, `change_scene` and `reload_scene`. Modules add theirs: `res` and `reload` (resources), `phys` and `impulse` (physics), `anim*` and `animtree*` (animation), `pipeline`, `lighting`, `environment` (rendering), `a11y`, `l10n`. Nodes are addressed as `#uid` (stable across edits) or by path.

### Natively

`CW_DEBUG_CMDS="a;b;c"` runs requests after the last frame of a `CW_FRAMES` run and prints one reply per line. On `remote_target` after 30 frames:

```sh
SDL_VIDEODRIVER=offscreen CW_FRAMES=30 CW_DEBUG_CMDS='get #4 speed ticks;call #4 boost 10;set Level/Mover speed 50;methods #4;bogus' build/native/samples/remote_target
```

```text
{"uid":4,"tick":30,"values":{"speed":{"value":"33.7","type":"float"},"ticks":{"value":"30","type":"int"}}}
{"called":"boost","result":"43.7","result_type":"float","tick":30}
{"value":"50","type":"float","uid":4,"revision":2}
{"methods":[{"name":"boost","type":"Mover","args":"float","doc":"add to speed; returns the new speed"},{"name":"teleport","type":"Mover","args":"vec2","doc":"jump to a position"}]}
{"error":"unknown command 'bogus' (help lists this game's commands)"}
```

`set` replies with the tree's change revision; `changes <revision>` returns what changed since, which is how an editor keeps its view in sync. This route answers everything in one frame and cannot wait, so a command that needs frames to pass (`step`, `screenshot`) replies `{"deferred":true}` here.

### In a cart, through romdev

The channel is a mailbox of debug fields (`dbg.cmd`, `dbg.cmd_seq` in, `dbg.reply`, `dbg.reply_seq` out). romdev's `wasm({op:'command', text})` writes a request and steps frames until the reply lands. The app answers at the start of a frame, so **each request costs one frame** of the running game; pause first when that matters. The same game as a cart, loaded with `loadMedia` and stepped to frame 30:

```text
status            => {"framesRun":1,"reply":{"frame":30,"tick":30,"time":0.5,"paused":false,...,"tree_tick":30,"nodes":6,"scene":"main.cwscene"}}
get #4 speed ticks=> {"framesRun":1,"reply":{"uid":4,"tick":31,"values":{"speed":{"value":"33.7","type":"float"},"ticks":{"value":"31","type":"int"}}}}
pause             => {"framesRun":1,"reply":{"frame":32,"tick":32,"time":0.533333,"paused":true,...}}
step 5            => {"framesRun":6,"reply":{"frame":38,"tick":37,"time":0.616667,"paused":true,...}}
get #4 ticks      => {"framesRun":1,"reply":{"value":"37","type":"int"}}
```

The tick moves by one per request until `pause`; after that only `step` advances it, by exactly the count asked.

## The debugger's pause

`pause` freezes the whole simulation: no fixed tick, no update, no input frame runs, but frames still render, window events are read and the channel still answers, so a tool can inspect and edit a frozen game. `step n` runs exactly n ticks, one per frame, each exactly as an unpaused 1/60 s frame runs it, so pausing at tick T and stepping n lands in the same state as running to T + n ([tests/remote.mjs](repo:tests/remote.mjs) checks that against a plain run). Natively `CW_START_PAUSED=1` pauses after frame 1, the frame a cart's host runs on load, so both start at tick 1. This is separate from the game's own pause ([cw_tree_set_paused](api:cw_tree_set_paused)), which a pause menu uses and which `ALWAYS` nodes ignore. From code: [cw_debug_pause](api:cw_debug_pause), [cw_debug_step](api:cw_debug_step).

## Render layers

Screenshots can take any combination of three layers: `world` (3D, effects, post and the 2D canvas), `ui` (Control nodes and the app's render callback) and `debug` (overlay passes with order 500 and up; 1000 is the convention). `remote_target` draws something in each, so a layer capture shows exactly one of them; its debug outline is an overlay pass:

@include samples/remote_target/main.c#debug_pass_add

## Adding commands

[cw_debug_command](api:cw_debug_command) adds a command (or replaces one, built-ins included). The handler gets `argc`/`argv` and builds a JSON reply with `cw_reply_printf`, `cw_reply_str` or `cw_reply_error`. `project_edit` adds `project save`, which writes a subtree into the project layer:

@include samples/project_edit/main.c#command

@include samples/project_edit/main.c#register

```text
{"saved":true,"bytes":1040}
{"files":["out.cwscene"],"root":null}
{"error":"usage: project save <node path> <file.cwscene>"}
```

(`project save /root/Level out.cwscene`, then `vfs ls`, then a malformed request.) A command whose answer needs frames to pass calls [cw_reply_defer](api:cw_reply_defer) with a wait function that runs each frame until it writes the reply. Node methods registered with `cw_node_type_add_method` need no command at all: `call <node> <method> [args]` type-checks the arguments against the method's signature.

## Driving a game from another process: remote.h

[cartwheel/remote.h](header:remote.h) is the controlling side, used by play-in-editor and by tests and agents. [cw_remote_launch](api:cw_remote_launch) starts a game on one of two transports with the same API and the same requests:

- `CW_REMOTE_NATIVE`: the game executable as a child process. It inherits one end of a socketpair as `CW_REMOTE=fd:N`, which turns the channel on even without `.debug`. Frames on the socket carry an id, so any number of requests may be in flight. Exits and crashes are detected and reported with the signal; the child's output comes back as log lines; on Linux the child dies with its parent. (`CW_REMOTE=tcp:HOST:PORT` serves the same protocol on TCP.)
- `CW_REMOTE_ROMDEV`: the game's `.wasc` loaded into romdev over HTTP, each request a `wasm({op:'command'})`; a worker keeps frames flowing at 60 a second while the game is not paused. A wasm trap is reported as a crash.

The game never runs in the controller's address space, so a crashing game cannot take an editor down: the editor sees a state change (`CW_REMOTE_CRASHED`, with a status line such as the signal name) and can [cw_remote_relaunch](api:cw_remote_relaunch). Requests are asynchronous ([cw_remote_send](api:cw_remote_send) plus [cw_remote_pump](api:cw_remote_pump) once per frame, callbacks run inside the pump) or blocking for tools ([cw_remote_call](api:cw_remote_call)). Helpers wrap the common sequences: [cw_remote_step](api:cw_remote_step) steps a paused game and waits, splitting large counts for romdev's per-command frame cap, and [cw_remote_screenshot](api:cw_remote_screenshot) captures layers to a PNG. [tests/remote/remote_test.c](repo:tests/remote/remote_test.c) drives `remote_target` through all of it on both transports: launch paused, edit while paused, step, call methods, follow the change log, layer screenshots, record, reload, change scenes, crash on purpose and relaunch.

Native only: in a cart there are no processes or sockets, and `cw_remote_launch` fails and says so.
