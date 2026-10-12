# Triggers

A trigger is a "when X happens, do Y" rule stored in a scene: when a node emits a signal, set a property, call a method, emit another signal, spawn a scene, free a node or change the scene. Triggers are how a level designer wires a door to a switch without writing code; everything else is C.

Header: [cartwheel/triggers.h](header:triggers.h). Node type: [Trigger](node:Trigger). Sample: `triggers_demo`, whose every reaction is a trigger in its scene file. Gates: `triggers` and `triggers-end`; their control disables every trigger and must fail.

## Where triggers fit: no scripting VM

{{name}} has no script language and no VM. Game code comes in three layers:

1. **Hot loops are threaded SIMD C**: anything per entity, per particle or per tile at scale runs as C functions over flat arrays on the job pool ([cw_parallel_for](api:cw_parallel_for), see [Core](guide:core)).
2. **Gameplay is C** written against the node API: node types with lifecycle callbacks, signals, properties and methods ([Scenes](guide:scenes)). It has the same language, debugger and speed as the engine, and no binding layer.
3. **Triggers** cover the handful of rules a human tweaks by hand: which door a switch opens, what a pickup does, where the exit leads. They are data, not code: nodes in a `.cwscene`, readable and writable as text, editable in the inspector, undoable like any edit, and visible through the debug channel.

So a trigger never replaces C. When a rule needs a loop, state, or anything per frame, it belongs in a node type; a trigger can then `CALL` a method that type exposes.

## A rule is a node

A `Trigger` is a child of the node whose signal fires it. From `triggers_demo`, the rules on its `Goal` area:

@include samples/triggers_demo/assets/main.cwscene#Goal/LightLamp..Goal/Never

The first one reads: when `Goal` emits `body_entered` with a node of group `player`, set `Lamp.color` to green. Its properties:

| Property | Meaning |
|---|---|
| `source` | the node whose signal fires the rule; default `..`, the trigger's parent. Paths are relative to the trigger (`../Lamp`, `/root/Level/Door`, `%Unique`) |
| `signal` | the source's signal (`body_entered`, `pressed`, `animation_finished`, ...) |
| `filter_group` | fire only when the signal's first argument is a node in this group (the player, not a crate); empty fires always |
| `action` | `SET`, `CALL`, `EMIT`, `SPAWN`, `FREE` or `CHANGE_SCENE` |
| `target` | the node acted on; default `..` |
| `member`, `value` | the property, method or signal name, and the value as text |
| `once` | fire at most once per trigger life (a pickup, a cutscene) |
| `enabled` | false never fires (another trigger may `SET` it true) |
| `fired` | read-only count of how many times the rule ran (not saved) |

The actions:

- `SET`: `target.member = value`, parsed for the property's type (enums by name, `color(1, 0, 0, 1)`, `vec2(...)`, numbers, booleans, text).
- `CALL`: call method `member` on the target (a method registered with [cw_node_type_add_method](api:cw_node_type_add_method), or one the game's method resolver knows). With `value` empty the signal's arguments are passed; otherwise `value` is parsed as the method's first argument type.
- `EMIT`: the target emits signal `member` with the signal's arguments.
- `SPAWN`: instance the scene file `value` as a child of the target (`member`, when set, names the new node).
- `FREE`: free the target and its subtree.
- `CHANGE_SCENE`: the scene app's main scene becomes `value`.

## Semantics

Deliberately small:

- A rule connects when the trigger becomes ready and disconnects when it leaves the tree. A missing source, or a signal the source's type does not declare, is a warning that names the fix. Changing `source`, `signal` or `enabled` at run time reconnects.
- Firing is **deferred**: actions run at the end of the phase that emitted the signal, in emission order. A rule may therefore free, spawn or change the scene safely from inside a physics callback.
- Several triggers on one signal fire in tree order. One trigger does one action; chain more by adding triggers, or `EMIT` a signal another trigger listens to.

`triggers_demo`'s `Counter` node chains that way: the goal's `Bump` rule emits `bump` on the counter, and the counter's own trigger turns that into a call:

@include samples/triggers_demo/assets/main.cwscene#Counter

The only C in the sample is that `Counter` type, a node with a `bump` signal and an `add(int)` method for rules to use:

@include samples/triggers_demo/main.c#counter

The exit rule ends the level:

@include samples/triggers_demo/assets/main.cwscene#Exit/Win

## Checking what fired

Every trigger's `fired` count is a property, so the debug channel reads it:

```sh
SDL_VIDEODRIVER=offscreen CW_FRAMES=28 CW_DEBUG_CMDS='get Level/Goal/LightLamp fired' build/native/samples/triggers_demo
```

```text
{"value":"1","type":"int"}
```

`triggers_demo` publishes all seven counts as one debug field. After 200 frames the ball has lit the lamp (tick 22), the crate has been counted and freed, and the ball has reached the exit (tick 33); the disabled rule never ran:

```sh
node tools/run-cart.mjs --host native --app triggers_demo --frames 200 --state out/tr.json
```

```text
"hits":12,"lamp_g":100,"lamp_visible":1,"spark":1,"crate":0,"won":1,"tick_lamp":22,"tick_won":33,"disabled":0,"fired":[1,2,1,1,1,0,1]
```

`hits` is 12: `Count` fired for the ball and the crate (1 + 1) and the chained `bump` added 10.

From C, [cw_trigger_fire](api:cw_trigger_fire) runs a trigger's action as if its signal fired (filter, `once` and `enabled` still apply); the editor's "test fire" uses it.

## Authoring

Write triggers by hand in the scene text, set them with `set` over the debug channel, or use the editor's Triggers panel, which lists the active scene's rules as sentences and builds new ones for the selected node from the signals its type declares. All three produce the same nodes, so a rule written as text opens in the editor and a rule a designer built reads as plain text in a diff.
