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
- **Event tracks** carry names; the player emits `animation_event(name, animation)` and calls its C event callback (see below).

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

### Gameplay events: hit windows, footsteps, sound cues

A clip can carry named events that the game reads, so a hit window or a footstep sits on the clip's own timeline instead of in a table of tick counts kept in step by hand. [cw_anim_add_event](api:cw_anim_add_event) puts a named key on any clip, a glTF one included (it makes the clip's event track on first use). [cw_anim_player_on_event](api:cw_anim_player_on_event) (or [cw_animtree_on_event](api:cw_animtree_on_event) for a tree) registers one C callback that gets the event name, the clip's name and the key's time, in time order, on the fixed tick that crosses the key, just before the `animation_event` signal. A hit window is two events, `hit_on` and `hit_off`; [cw_anim_event_time](api:cw_anim_event_time) reads a key's time when the game wants it up front (for example to line up an attack's telegraph). `samples/anim_skinned` puts a footstep on each foot's contact of the walk and counts them in the debug field `footsteps`; `unit_anim` checks the order, the key times and the tick on which each fires, with a run without a callback as the control.

### Many characters: cheap clones and animation detail

A crowd of skinned characters does not need one glTF parse each. Load the model once as a resource and make each character with [cw_gltf_instantiate](api:cw_gltf_instantiate): every instance shares the geometry and materials and gets its own skeleton, and [cw_gltf_instance_auto_lod](api:cw_gltf_instance_auto_lod) picks a coarser mesh for a far one (see Resources). Animation has a matching knob: [cw_anim_player_set_update_divisor](api:cw_anim_player_set_update_divisor) makes a far character sample its clip and write its bones on every 2nd, 4th or 8th tick only, with all the time since the last sample, so the clip's time, loops and event count are exactly those of full rate while the pose steps; characters are spread over the ticks by their slot so they do not all sample together. [cw_anim_lod_divisor](api:cw_anim_lod_divisor) turns a distance into the divisor (full rate within a distance, then half, a quarter, an eighth as the distance doubles).

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

## Inverse kinematics

Clips play as authored. Skeleton modifiers make them react to the world: feet that land on stairs and slopes, a hand that grabs a handle, a head that follows something moving. The module is `cartwheel/ik.h` ([reference](header:ik.h)).

Modifiers are nodes, children of the Node3D that holds a skinned model, and they run in child order every fixed tick:

1. The AnimationPlayer or AnimationTree writes the animated pose onto the bones, as usual.
2. After every node's `fixed_update` and the physics step, the model's pose is captured into a `cw_pose`: local translation, rotation and scale per bone, parent indices and model-space transforms.
3. Each active modifier adjusts the pose the previous one left.
4. The pose is written back to the bones once, and the skin draws from it.

Every tick starts again from the animated pose, never from the last tick's solution. A bone the animation does not write (a static model, a bone no track drives) gets its last animated value back first, so results never pile up. A bone that game code or an `UPDATE`-mode player writes keeps that value as its animated value. Your app's `fixed_update` runs after the modifiers, so a target it moves applies on the next tick.

| Node | What it does |
|---|---|
| [TwoBoneIK3D](node:TwoBoneIK3D) | an arm or a leg (`root_bone`, `middle_bone`, `end_bone`) reaches a `target`. The middle joint bends toward a `pole` (`use_pole`), or keeps the animation's own bend plane. `soften` eases the last stretch so a straight limb does not snap. `use_target_rotation` gives the hand or foot the target's rotation (grip a handle, hold a wheel). `reached` reports whether the target was in reach |
| [LookAtModifier3D](node:LookAtModifier3D) | turns a `bone` toward a target by the shortest rotation, at most `limit_angle` from where the animation points it. `forward_axis` is the way the bone faces at rest, in model space (`(0, 0, 1)` for a glTF character's head). Chain two, a neck at `influence = 0.4` and then the head, to spread the turn |
| [CCDIK3D](node:CCDIK3D) | cyclic coordinate descent from `end_bone` up to `root_bone`, the same algorithm and limits as three.js r186's `CCDIKSolver`. Per-joint limits come from [cw_ccdik3d_set_joint](api:cw_ccdik3d_set_joint) |
| [FABRIK3D](node:FABRIK3D) | FABRIK over a chain (tails, spines, cables), with cone and hinge limits per joint ([cw_fabrik3d_set_joint](api:cw_fabrik3d_set_joint)) |
| [FootPlacement3D](node:FootPlacement3D) | plants feet on the physics ground. Each leg casts a ray down through its animated ankle, the ankle is lifted or lowered by the ground's height (keeping the animation's own swing), the pelvis drops when a foot has to reach down, each leg is solved as two-bone IK, and the foot turns onto the ground normal while it is near the ground |

Every modifier has `active` and `influence` (0 to 1: the blend between the pose it was given and its result). Animate or tween `influence` to blend a modifier in and out. Bones are named as in the glTF, with the same sanitizing three.js applies (`UpperLeg.L` becomes `UpperLegL`). Targets are Node3D paths relative to the modifier, or a world position in `target_position`.

`samples/anim_ik` walks four skinned robots back and forth over a flight of stairs and a slope, with feet planted, heads (and the front robot's neck) following a firefly, and the front robot's right hand grabbing a handle as it passes. The front robot's modifiers come from its scene file; the other three are built in code the same way:

```text
[node name="Feet" parent="Mover/Model" type="FootPlacement3D"]
legs = "UpperLegL,LowerLegL,LowerLegL_end,FootL;UpperLegR,LowerLegR,LowerLegR_end,FootR"
pelvis = "Body"

[node name="Reach" parent="Mover/Model" type="TwoBoneIK3D"]
root_bone = "UpperArmR"
middle_bone = "LowerArmR"
end_bone = "Palm2R"
target = "../../../Handle/Grip"
influence = 0

[node name="LookHead" parent="Mover/Model" type="LookAtModifier3D"]
bone = "Head"
target = "../../../Firefly"
limit_angle = 1.1
```

A leg is `"upper,lower,end[,foot]"`. The robot's rig keeps its foot bones outside the leg chain (it was rigged for IK in Blender), so FootPlacement3D carries a foot bone that is not below `end` along with the ankle. The app code only moves each robot along the course, sets its height from a ray at its centre, and blends the reach's `influence` by distance to the handle:

@include samples/anim_ik/main.c#reach

### The solvers from C

The nodes call plain functions that work on any `cw_pose`. You can use them directly, for example to pose a procedural rig or to bake a pose:

- [cw_ik_two_bone](api:cw_ik_two_bone): no trigonometry at all. The law of cosines gives the middle joint, and each bone turns by the shortest rotation.
- [cw_ik_aim](api:cw_ik_aim): an aim with an angle limit.
- [cw_ccdik_solve](api:cw_ccdik_solve): r186's `CCDIKSolver.update`. It keeps its link order, the 1e-5 skip, min and max angles, the `limitation` axis projection, the Euler clamp, the early stop and the final slerp.
- [cw_ik_fabrik](api:cw_ik_fabrik): FABRIK, which adds no roll.

[cw_pose_bind](api:cw_pose_bind) binds a pose to a glTF scene's bones, and [cw_pose_init](api:cw_pose_init) makes a free-standing one. [cw_skeleton_pose](api:cw_skeleton_pose) and [cw_skeleton_bone_world](api:cw_skeleton_bone_world) read what the modifiers left this tick.

### Determinism, threads and cost

Solvers use only arithmetic, `sqrt` and the `cartwheel/math.h` functions, with fixed iteration counts, so the same animated pose gives the same bones on every host. Each model keeps a running hash of every pose it flushes ([cw_skeleton_hash](api:cw_skeleton_hash)). Captures, target lookups and FootPlacement3D's rays (one `cw_world3d_raycast_batch` per node) run on the main thread. The solves then run on the main thread, or for 8 models and more one model per job through `cw_parallel_for`; each touches only its own pose, so the result does not depend on the worker count.

Cost on the 890M laptop (Ryzen AI 9 HX 370), anim_ik's front robot: two feet (two rays, two legs, the pelvis), a hand, and a neck and head look-at on a 60-bone rig. The figure is the modifier phase's own wall clock divided by the four robots. It was taken on a shared machine (load average 78 to 103), so it reports both the mean and the quietest tick:

| Host | Mean per character | Quietest tick per character | Four robots per frame |
|---|---|---|---|
| native | 16 to 27 us | 12.6 us | 0.06 to 0.11 ms |
| cart (Chromium) | 40 to 52 us | 22.5 us | 0.16 to 0.21 ms |

There is no GPU cost: modifiers only change bone transforms the skin already reads. The quality presets do not change IK, because it moves simulation state that must agree across machines. Below 8 models the solves run on the main thread, since waking the job pool costs more than a few models' solves; from 8 up, each model is one job. On a Vega 8 laptop (a Zen or Zen+ CPU, about half the per-core speed), expect roughly twice these figures: about 25 to 50 us per character native and 45 to 100 us in a cart, so a crowd of 40 such characters costs about 1 to 2 ms per tick natively before threading.

The gates compare solver results against three.js r186. They plant the robot's feet on the stairs: the ankle's height above the ground under it must match its animated height above flat ground, within 1 cm through every stance (worst 3.8 mm). They keep the hand within 1 mm of the handle (0.1 mm), and they compare pose hashes across hosts and worker counts:

```sh
node tests/gate.mjs unit-ik anim-ik
node tests/ik-determinism.mjs
```

`unit-ik` checks the solvers (reach accuracy, the pole plane, limits, FABRIK constraints) and compares CCD against three.js r186 run in Node (`tools/gen-ccdik-fixture.mjs` writes the reference). It also runs the nodes in a tree (no accumulation across ticks, influence, foot placement on Box3D ground, a 12-rig crowd through the job pool).

The debug command `ik <model path>` lists a model's modifiers with their last results (`reached`, `in_limit`, foot offsets, pelvis offset), the pose hash and the microseconds the last tick took.

## Determinism and testing

Animation that runs in the fixed step is bit-identical across hosts: time is a double advanced by the fixed `dt`, interpolation uses only arithmetic, `sqrt` and the `cartwheel/math.h` functions (three.c's own mixer calls libm, so {{name}} samples glTF clips itself and lets three.c only render the skin), blends accumulate in insertion order, and values are written through the reflection path. Players and trees keep a running hash of every value they write ([cw_anim_player_hash](api:cw_anim_player_hash), [cw_animtree_hash](api:cw_animtree_hash)); the samples publish one hash per 60-tick window as debug state, and the gates compare them across hosts:

```sh
node tests/gate.mjs anim-props anim-tween anim-skinned unit-anim
```

A running app answers animation questions over the [debug channel](guide:debug-channel): `anim <player>` (current clip, position, library, hash), `anim_play`, `anim_stop`, `anim_seek`, `anim_text <player> <clip>` (the clip as scene text), and `animtree <tree>` (parameters, state machines, clip weights), `animtree_param`, `animtree_travel`.
