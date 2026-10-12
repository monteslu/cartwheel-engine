# Scenes and nodes

A {{name}} game is a tree of nodes. Each node has a registered type that says what data it carries, which properties it exposes by name, which signals it emits and which lifecycle callbacks it runs; scenes are subtrees saved as text that diffs and merges in git.

Headers: [cartwheel/scene.h](header:scene.h) (nodes, tree, groups, signals, methods, reflection), [cartwheel/scene_file.h](header:scene_file.h) (`.cwscene` and the scene runner), [cartwheel/scene2d.h](header:scene2d.h) (the 2D canvas types and pass). The node types themselves are listed in the node reference, starting at [Node](node:Node). Samples: `scene3d` (a level from scene files, instancing and an override), `platformer2d` (a whole game from scene files plus one custom node type), and the `unit-scene` suite.

## Nodes and the tree

A node is created with [cw_node_new](api:cw_node_new) from a type name and lives outside any tree until it is added under a node that is in one ([cw_node_add_child](api:cw_node_add_child)). You hold it by a `cw_node` generational handle: once the node is freed the handle resolves to nothing instead of to another node, so keeping handles in game state is safe. Freeing a node frees its whole subtree.

Paths find nodes: `"Arm/Hand"`, `"../Sibling"`, `"/root/Level/Player"` (absolute), `"%Name"` (a scene-unique name). [cw_node_get](api:cw_node_get) walks the path each call, so look a node up once in `init` or `ready` when it is used every tick. Every node also has a `uid`, stable across saves, that the editor and the debug channel use (`#12` addresses node 12).

Types inherit: `MeshInstance3D` extends `Node3D`, which extends `Node`. A node carries one data block per type in its chain; [cw_node_data](api:cw_node_data) returns the block for one type (or `NULL` when the node is not of that type). [Node3D](node:Node3D) owns a three.c object and a transform; [Node2D](node:Node2D) has a 2D transform, z order and modulate. Both can render between the last two fixed ticks (`interpolate`); code that moves such a node in `fixed_update` calls `cw_node3d_snapshot` / `cw_node2d_snapshot` first, as `scene3d` does for its orbiting ball.

## Lifecycle and processing order

A type's callbacks, all optional:

| Callback | When |
|---|---|
| `init` | after allocation, before entering a tree |
| `enter_tree` / `exit_tree` | when the node joins or leaves a tree |
| `ready` | once, after all its children are ready |
| `fixed_update(dt)` | every simulation tick |
| `update(dt)` | every rendered frame |
| `pre_render(alpha)` | before drawing, with the interpolation factor |
| `prop_changed`, `children_changed` | after a property write, after the child list changed |
| `destroy` | when freed |

Parent types' callbacks run first for init, enter and ready, and last for exit and destroy. Each phase visits nodes in tree order (depth first, children in order), so a run is deterministic. Adding, removing or freeing nodes while a phase runs is queued and applied when the phase ends; deferred signal connections are flushed at the same point. Modules hook phases too ([cw_tree_add_hook](api:cw_tree_add_hook)): physics steps before nodes see the results.

A tree can be paused ([cw_tree_set_paused](api:cw_tree_set_paused)): each node's `pause_mode` (`INHERIT`, `PAUSABLE`, `WHEN_PAUSED`, `ALWAYS`, `DISABLED`) decides whether it still processes, which is how a pause menu keeps running over a frozen game. This is the game's own pause; the debugger's pause (see [Debug channel](guide:debug-channel)) freezes everything.

Each tree also has a seeded RNG ([cw_tree_rng](api:cw_tree_rng)) and per-module slots where physics worlds, the render scene and the audio listener keep their per-tree state.

## Properties and values

Every property of every node is reachable by name with a typed [cw_value](api:cw_value): [cw_node_set](api:cw_node_set) and [cw_node_get_prop](api:cw_node_get_prop). That one path serves the inspector, the scene format, animation tracks, triggers and the debug channel, so anything a scene file can say, code and agents can say too. Values have a text form: `3.5`, `true`, `vec3(1, 2, 3)`, `color(1, 0.5, 0, 1)`, `"text"`, `node("Path/To")`, `res(0x1234)`. Enums can be set by name with `cw_val_enum("SPHERE")`.

`cw_node_set` checks the name and type, converts where it can, logs and returns false where it cannot, and notifies the owning type. It costs a lookup per call: fine for gameplay, but per-frame bulk updates of thousands of nodes should use the type's own setter (`cw_node3d_set_position`) or its data.

Over the debug channel the same calls look like this (`scene3d`, after 10 frames):

```sh
SDL_VIDEODRIVER=offscreen CW_FRAMES=10 CW_DEBUG_CMDS="get Level/Ball position;set Level/CrateA/Box color color(0,1,0,1)" build/native/samples/scene3d
```

```text
{"value":"vec3(2.4501665, 1.3168255, 0.4966733)","type":"vec3"}
{"value":"color(0, 1, 0, 1)","type":"color","uid":8,"revision":1}
```

## Groups

A group is a name a node belongs to (`groups="coins"` in a scene file, or [cw_node_add_group](api:cw_node_add_group)). [cw_tree_group](api:cw_tree_group) returns the members in tree order as a frame-scratch array; the list is cached per tree and rebuilt only when membership changes, so calling it every tick is cheap. `scene3d` counts its crates this way.

## Signals and methods

A signal is a named event a node emits (`body_entered`, `pressed`, `animation_finished`); the node reference lists each type's signals. [cw_connect](api:cw_connect) attaches a C function. The connection belongs to the target node and disappears when either node is freed; `CW_CONNECT_DEFERRED` queues the call to the end of the phase, `CW_CONNECT_ONESHOT` disconnects after the first call. From `platformer2d`:

@include samples/platformer2d/main.c#connect

Connections saved in scene files name a *method* instead of a function pointer. The game resolves names with a method resolver ([cw_set_method_resolver](api:cw_set_method_resolver)); the coin scene below connects `body_entered` to `collect`:

@include samples/platformer2d/main.c#collect

Types can also expose named methods ([cw_node_type_add_method](api:cw_node_type_add_method)) that the editor and agents invoke with `call <node> <method> [args]`; [Triggers](guide:triggers) uses them as actions.

## Custom node types

Gameplay code is a node type: a data struct, a static [cw_node_type](api:cw_node_type) with callbacks, and properties declared with the `CW_PROP_FIELD` / `CW_PROP_ACCESSOR` macros so they show in the inspector and save in scenes. `platformer2d`'s player extends [CharacterBody2D](node:CharacterBody2D) and only adds a tick:

@include samples/platformer2d/main.c#player_data

@include samples/platformer2d/main.c#player_type

Register types in a function named `cw_game_register_types` and call it from `cw_main`. The editor calls the same function to open the game's scenes, and the hot-reload host calls it after new code loads; registering a name again takes the new callbacks and keeps the data layout (a changed `data_size` or parent is refused).

@include samples/platformer2d/main.c#main

Reflection is data: `docs/nodes.json` is generated from the registry, the `types` debug command lists every type with its properties, and `props <node>` returns each property's type, owner type, value and doc.

## The .cwscene format

A scene file is one node per `[node]` section and one property per line, in tree order. Only properties that differ from the type's default are written, and saving a loaded scene reproduces the file byte for byte, so scene diffs stay small and merge cleanly. `scene3d`'s crate:

@include samples/scene3d/assets/crate.cwscene

and the level that uses it:

@include samples/scene3d/assets/main.cwscene

The parts that matter:

- `[ext_scene id=1 path="crate.cwscene"]` declares a sub-scene; `instance=1` on a node instances it there. Properties on that line override the instanced root (`CrateB`'s position and rotation).
- A `[node]` with neither `type` nor `instance` is an override of a node inside an instance: `[node name="Box" parent="CrateB"]` recolours only the second crate's box.
- `parent` paths are relative to the scene root (`.` is the root).
- `[connection signal=... from=... to=... method=...]` lines restore signal connections by method name.
- Modules add sections of their own (animations write `[animation]` with `[track]` sub-headers) through [cw_scene_register_section](api:cw_scene_register_section).

From code: [cw_scene_instance_path](api:cw_scene_instance_path) makes a plain copy of a scene (to spawn rooms or enemies, say), [cw_scene_instance_linked](api:cw_scene_instance_linked) keeps the link so saving writes `instance=` plus the differences (the editor's "instance a scene"), and [cw_scene_save](api:cw_scene_save) writes any subtree back as text. Parsed scenes are cached by path; [cw_scene_cache_forget](api:cw_scene_cache_forget) drops one after rewriting it. Loads read the project layer first ([App and platform](guide:app-platform)), so a scene an agent or the editor just wrote is what loads.

## The scene runner

[cw_scene_app](api:cw_scene_app) turns a scene path and a base description into a complete app: it makes the tree, seeds its RNG from the host seed, instances the main scene under the root and drives every phase. Each frame:

1. `fixed_update`: the tree's tick (module hooks, then nodes in tree order), then your `base.fixed_update` (skipped while the tree is paused);
2. `update`: the tree, then `base.update`;
3. `render`: the tree's `pre_render`, then the render pipeline: 3D only when a [Camera3D](node:Camera3D) exists, the post chain, the 2D canvas and UI, then your `base.render` as the last overlay, so a HUD drawn there with render2d lands on top.

[cw_scene_app_tree](api:cw_scene_app_tree) returns the tree; [cw_scene_app_change_scene](api:cw_scene_app_change_scene) swaps the main scene (the debug channel's `change_scene` and `reload_scene` use it). With a `NULL` scene path you build the tree yourself in `base.init`.

## 2D in the tree

The 2D node types ([Sprite2D](node:Sprite2D), [TileMap2D](node:TileMap2D), [Camera2D](node:Camera2D), [CanvasLayer](node:CanvasLayer), [Light2D](node:Light2D) and the rest) draw in one pass that walks the tree in order and submits to the batched 2D renderer by canvas layer, z and tree order. Custom drawing types register a draw callback with [cw_canvas_register_type](api:cw_canvas_register_type). The pass, cameras and canvases are covered in [2D rendering](guide:rendering-2d).
