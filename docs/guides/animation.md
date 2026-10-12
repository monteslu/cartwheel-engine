# Animation

{{name}} animates any reflected node property, glTF skeletons and method calls with keyframed clips, blends clips through graphs and state machines, and runs one-off tweens from code. Everything runs in the fixed step by default and produces the same values on every host.

The module is `cartwheel/anim.h` ([reference](header:anim.h)). It has three layers, and you can use any one without the others:

| Layer | What it is | Owned by |
|---|---|---|
| `cw_anim` | a clip resource: length, loop mode, tracks of keys | reference counted; players hold their own reference |
| [AnimationPlayer](node:AnimationPlayer) | a node that plays clips from its library, cross-fades and queues them | the scene tree |
| [AnimationTree](node:AnimationTree) | a node that evaluates a blend graph over a player's library | the scene tree; graph nodes are owned by the tree |
| `cw_tween` | a sequence of property steps built in code, no clip | the tree it runs on, optionally bound to a node |

Frame-based sprite animation (`AnimatedSprite2D`) and animated tiles belong to the 2D renderer; see [2D rendering](guide:rendering-2d).

## Clips and tracks

A clip is a name, a length in seconds, a loop mode (`NONE`, `LOOP`/`LINEAR`, `PING_PONG`) and a list of tracks. A track has a path and sorted keys:

- **Value tracks** write a node property by path, relative to the player's `root_node`: `"Arm:rotation"`, `":position"` (the root itself), `"Arm:position.y"` (one component of a vector or colour). Any reflected property works. Floats, ints, vectors, quaternions and colours interpolate; bools, strings, enums, node and resource references step.
- **Bone tracks** are value tracks on a glTF bone: `"Model@bone_name:quaternion"` finds the three.c object named `bone_name` under the Node3D `Model`. Only position, quaternion and scale animate.
- **Method tracks** call a method on a node at each key: a C callback registered with [cw_anim_register_method](api:cw_anim_register_method), else the node's signal of that name, with up to four arguments.
- **Event tracks** carry names; the player emits `animation_event(name, animation)`.

Interpolation is per track: `NEAREST` (a step), `LINEAR`, `CUBIC` (Catmull-Rom through the neighbour keys) and `HERMITE` (explicit in and out tangents per key, the glTF CUBICSPLINE convention). Quaternions slerp by default or nlerp per track.

Inserting keys in time order is O(1); inserting in the middle is O(keys). Sampling a track is a binary search and one interpolation, and the player caches the binding from each track to its node property, so a playing clip costs a few property writes per tick, not a path lookup.

### Key firing

Method and event keys fire when playback sweeps over them, in the direction of motion, so every key fires exactly once per pass even at large time steps. The rules at the edges are exact and documented in the header: a looping clip fires the keys at its end and then the keys at 0 on every wrap, ping-pong fires the keys at each turning point once per turn, and `seek` never fires keys. Within one tick the order is fixed: advance times, write the blended values, then fire keys and signals.

## Clips in scene files

Animations live in the `.cwscene` file of the scene that owns the player, after its nodes. This is the spinning blade of the 3D platformer's saw hazard:

@include games/platformer3d/assets/scenes/saw.cwscene#Anim..spin

One key per line, `time = value`, with values written the way the scene format writes properties. Defaults are omitted (`loop=NONE`, `interp=LINEAR`, `rot=SLERP`), so a clip saved from a loaded scene reproduces the file byte for byte; the `anim-props` gate checks exactly that. Clips imported from a glTF are not stored ([cw_anim_set_stored](api:cw_anim_set_stored)): they come from the model every load. The [scenes guide](guide:scenes) covers the file format as a whole.

`samples/anim_props` drives three players from its scene file: a looping "show" that spins a box, cycles its colour with a cubic track, dims the sun, steps a lamp colour, calls a C callback from a method track and fires events; a ping-pong bounce with a cubic `position.y` component track and a hermite squash; and a player that cross-fades between two clips at fixed ticks.

## AnimationPlayer

A player holds a library of named clips and plays one at a time, with cross-fades between them. Its main properties are `autoplay` (played when the node becomes ready), `speed_scale` (may be negative), `default_blend` and `process`:

- `FIXED` (default): advanced in the fixed step. Deterministic; use it for anything gameplay reads.
- `UPDATE`: advanced once per rendered frame. Smoother for purely cosmetic animation at high refresh rates, but not part of the deterministic state.
- `MANUAL`: only [cw_anim_player_advance](api:cw_anim_player_advance) moves it.

[cw_anim_player_play](api:cw_anim_player_play) starts a clip with a blend time (a negative blend uses the pair's time from [cw_anim_player_set_blend](api:cw_anim_player_set_blend), else `default_blend`). [cw_anim_player_queue](api:cw_anim_player_queue) plays the next clip when a non-looping one finishes. A clip that is fading out keeps advancing but fires no keys. Signals: `animation_started`, `animation_finished`, `animation_changed`, `animation_looped`, `animation_event`.

A value that only part of the active weight touches is mixed toward its *rest value*: the value the property had when the player first animated it (for a glTF bone, its bind pose). That is what makes a half-faded clip settle toward the original pose instead of toward zero.

## AnimationTree

An AnimationTree points at a player (its `player` property; stop the player's own playback) and evaluates a graph of blend nodes into the same mixer. Graph nodes are created in code and addressed by `cw_blend_id`; each has at most one parent, and the first one created is the root unless [cw_animtree_set_root](api:cw_animtree_set_root) says otherwise.

| Node | Built with | Does |
|---|---|---|
| clip | [cw_animtree_clip](api:cw_animtree_clip) | plays one library clip at its own speed |
| 1D blend space | [cw_animtree_blend1d](api:cw_animtree_blend1d) | children at positions on one parameter; the two around the value blend linearly |
| 2D blend space | [cw_animtree_blend2d](api:cw_animtree_blend2d) | children at points on two parameters, gradient-band weights (no triangulation) |
| blend2 | [cw_animtree_blend2](api:cw_animtree_blend2) | `lerp(a, b, param)` |
| add | [cw_animtree_add](api:cw_animtree_add) | base plus an additive branch scaled by a parameter, relative to rest values |
| state machine | [cw_animtree_state_machine](api:cw_animtree_state_machine) | states playing graph nodes, transitions with fades, conditions and switch modes |

Any subtree can be restricted to a set of bones or node paths with [cw_animtree_set_filter](api:cw_animtree_set_filter) (a bone mask). Parameters are named floats set with [cw_animtree_set_param](api:cw_animtree_set_param); transition conditions read them (`"speed > 0.5"`, `"!grounded"`). Every node evaluated in a tick advances its time even at weight 0, so the children of a blend space stay in phase.

Transitions are `AUTO` (taken when the condition holds), `ENABLED` (only by travel) or `DISABLED`, and switch `IMMEDIATE` or `AT_END` (when the current state finishes or wraps). [cw_animtree_sm_travel](api:cw_animtree_sm_travel) walks to a state along the shortest chain of transitions, taking each with its fade. While a cross-fade runs no other transition is evaluated, so keep fades short where transitions chain.

`samples/anim_skinned` drives a skinned glTF character from a gamepad. Its graph is a state machine (a 1D speed blend space of idle, walk and a faster walk, plus a one-shot wave reached by travel) with an additive, bone-masked 2D look blend space on top:

@include samples/anim_skinned/main.c#graph

Each fixed tick the game only sets parameters and asks for travel; the tree does the rest:

@include samples/anim_skinned/main.c#params

Note the comment in the sample: the app's `fixed_update` runs after the tree, so input applied there takes effect on the next tick. The sample also snaps stick values to 1/64, because hosts quantize analog axes slightly differently and the gate compares poses exactly.

> The blend graph is built in code. AnimationTree's properties (player, process, root motion) are stored in scene files, its graph is not, and the editor has no animation timeline or graph editor yet.

## Root motion

Set `root_motion_track` to a bone or Node3D position track and `root_motion_target` to a Node3D. Each tick the track's motion along the axes in `root_motion_lock` (bit 0 x, 1 y, 2 z, in the track's parent space) is taken off the bone and added to the target's position, so a walk cycle moves the character instead of sliding the hips away from it. [cw_animtree_root_motion](api:cw_animtree_root_motion) and [cw_anim_player_root_motion](api:cw_anim_player_root_motion) return the last tick's motion if you want to feed a character controller instead. In `anim_skinned` the tree's root motion moves `Mover` and the left stick turns it; locking x and y (`root_motion_lock = 3`) keeps the torso's bob on the bone.

## Tweens

A tween is a handle, not a node: a sequence of steps that runs on a tree. Steps are property changes with an easing curve from `cw_ease` (`cartwheel/math.h`, deterministic), waits and callbacks. [cw_tween_parallel](api:cw_tween_parallel) makes the next step join the previous one's group; groups run one after another. Modifiers apply to the last step added: `cw_tween_from`, `cw_tween_relative`, `cw_tween_delay`.

@include samples/anim_tween/main.c#sequence

For the common case, [cw_tween_property](api:cw_tween_property) is one call that also binds the tween to the node:

@include samples/anim_tween/main.c#oneshot

Ownership is simple: a finished tween frees itself and its handle reads as stale; a tween bound to a node dies with the node (no finished callback); tweens pause with the tree unless bound to a node that processes while paused, which is how a pause menu animates. Leftover time carries into the next step, so a sequence lands on the same tick on every host and at every frame rate. `samples/anim_tween` shows all of it, including an `UPDATE`-phase tween and a tween that dies when its node is freed; the 2D action game in `games/action2d` uses tweens for its UI banners.

## Determinism and testing

Animation that runs in the fixed step is bit-identical across hosts: time is a double advanced by the fixed `dt`, interpolation uses only arithmetic, `sqrt` and the `cartwheel/math.h` functions (three.c's own mixer calls libm, so {{name}} samples glTF clips itself and lets three.c only render the skin), blends accumulate in insertion order, and values are written through the reflection path. Players and trees keep a running hash of every value they write ([cw_anim_player_hash](api:cw_anim_player_hash), [cw_animtree_hash](api:cw_animtree_hash)); the samples publish one hash per 60-tick window as debug state, and the gates compare them across hosts:

```sh
node tests/gate.mjs anim-props anim-tween anim-skinned unit-anim
```

A running app answers animation questions over the [debug channel](guide:debug-channel): `anim <player>` (current clip, position, library, hash), `anim_play`, `anim_stop`, `anim_seek`, `anim_text <player> <clip>` (the clip as scene text), and `animtree <tree>` (parameters, state machines, clip weights), `animtree_param`, `animtree_travel`.
