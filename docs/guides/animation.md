# Animation

{{name}} animates any reflected node property, glTF skeletons and method calls with keyframed clips, blends clips through graphs and state machines, and runs one-off tweens from code. Everything runs in the fixed step by default and produces the same values on every host.

The module is `cartwheel/anim.h` ([reference](header:anim.h)); skeleton modifiers are in `cartwheel/ik.h` and retargeting in `cartwheel/retarget.h`. It has three layers, and you can use any one without the others:

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

One key per line, `time = value`, with values written the way the scene format writes properties. Defaults are omitted (`loop=NONE`, `interp=LINEAR`, `rot=SLERP`), so a clip saved from a loaded scene reproduces the file byte for byte; the `anim-props` gate checks exactly that. Clips imported from a glTF are not stored ([cw_anim_set_stored](api:cw_anim_set_stored)): they come from the model every load. [cw_anim_to_text](api:cw_anim_to_text) writes one clip as its `[animation]` section and [cw_anim_from_text](api:cw_anim_from_text) reads such a section back into a stored clip (null, logged, on a parse error), so a tool can build or edit a clip as text and hand it to [cw_anim_player_add](api:cw_anim_player_add). The [scenes guide](guide:scenes) covers the file format as a whole.

`samples/anim_props` drives three players from its scene file: a looping "show" that spins a box, cycles its colour with a cubic track, dims the sun, steps a lamp colour, calls a C callback from a method track and fires events; a ping-pong bounce with a cubic `position.y` component track and a hermite squash; and a player that cross-fades between two clips at fixed ticks.

## AnimationPlayer

A player holds a library of named clips and plays one at a time, with cross-fades between them. Its main properties are `autoplay` (played when the node becomes ready), `speed_scale` (may be negative), `default_blend` and `process`:

- `FIXED` (default): advanced in the fixed step. Deterministic; use it for anything gameplay reads.
- `UPDATE`: advanced once per rendered frame. Smoother for purely cosmetic animation at high refresh rates, but not part of the deterministic state.
- `MANUAL`: only [cw_anim_player_advance](api:cw_anim_player_advance) moves it. Advance it in your app's `fixed_update` (a game that steps its animation with its own simulation, or holds it through a hit stop); skeleton modifiers on the model still adjust that pose the same tick (see [Inverse kinematics](#inverse-kinematics)).

[cw_anim_player_play](api:cw_anim_player_play) starts a clip with a blend time (a negative blend uses the pair's time from [cw_anim_player_set_blend](api:cw_anim_player_set_blend), else `default_blend`). [cw_anim_player_queue](api:cw_anim_player_queue) plays the next clip when a non-looping one finishes. A clip that is fading out keeps advancing but fires no keys. Signals: `animation_started`, `animation_finished`, `animation_changed`, `animation_looped`, `animation_event`.

A value that only part of the active weight touches is mixed toward its *rest value*: the value the property had when the player first animated it (for a glTF bone, its bind pose). That is what makes a half-faded clip settle toward the original pose instead of toward zero.

### Gameplay events: hit windows, footsteps, sound cues

A clip can carry named events that the game reads, so a hit window or a footstep sits on the clip's own timeline instead of in a table of tick counts kept in step by hand. [cw_anim_add_event](api:cw_anim_add_event) puts a named key on any clip, a glTF one included (it makes the clip's event track on first use). [cw_anim_player_on_event](api:cw_anim_player_on_event) (or [cw_animtree_on_event](api:cw_animtree_on_event) for a tree) registers one C callback that gets the event name, the clip's name and the key's time, in time order, on the fixed tick that crosses the key, just before the `animation_event` signal. A hit window is two events, `hit_on` and `hit_off`; [cw_anim_event_time](api:cw_anim_event_time) reads a key's time when the game wants it up front (for example to line up an attack's telegraph). `samples/anim_skinned` puts a footstep on each foot's contact of the walk and counts them in the debug field `footsteps`; `unit_anim` checks the order, the key times and the tick on which each fires, with a run without a callback as the control.

### Faces: morph targets

Morph targets (blend shapes) load with a glTF mesh, skinned or not, and draw on GL and WebGPU. Their names come from the mesh's `extras.targetNames`, which Blender and most exporters write. Set a weight by name over a whole model ([morph.h](header:morph.h)) with [cw_morph_set](api:cw_morph_set): every mesh under the root that has a target of that name moves, so a face split into several meshes (skin, mouth, eyes) smiles as one. [cw_morph_get](api:cw_morph_get), [cw_morph_reset](api:cw_morph_reset) and [cw_morph_names](api:cw_morph_names) read them back.

Clips drive the same weights. A glTF animation's `weights` channel becomes one float track per target in [cw_anim_from_t3_clip](api:cw_anim_from_t3_clip), with the path `Model@Mesh:morphTargetInfluences[i]`, so an AnimationPlayer or an AnimationTree blends a mouth shape with the body. A track made in code may name the target instead of its index: `Model@Face:morphTargetInfluences[smile]`. Code and clips write the same weights; whatever runs last in the tick wins (the app's fixed update runs after the tree, so a smile set there sits on top of the clip).

Each instance from [cw_gltf_instantiate](api:cw_gltf_instantiate) has its own weights over the shared geometry. A morphed mesh is drawn on its own (it is never batched with others), and its vertex shader reads every target's offset for every vertex, so keep face targets on the head mesh, not the whole body. `samples/character_skin` drives a talk clip's jaw through the glTF weights channel and smiles, brows and blinks from code on six characters.

### Many characters: cheap clones and animation detail

A crowd of skinned characters does not need one glTF parse each. Load the model once as a resource and make each character with [cw_gltf_instantiate](api:cw_gltf_instantiate): every instance shares the geometry and materials and gets its own skeleton, and [cw_gltf_instance_auto_lod](api:cw_gltf_instance_auto_lod) picks a coarser mesh for a far one (see Resources). Animation has a matching knob: [cw_anim_player_set_update_divisor](api:cw_anim_player_set_update_divisor) makes a far character sample its clip and write its bones on every 2nd, 4th or 8th tick only, with all the time since the last sample, so the clip's time, loops and event count are exactly those of full rate while the pose steps; characters are spread over the ticks by their slot so they do not all sample together. [cw_anim_lod_divisor](api:cw_anim_lod_divisor) turns a distance into the divisor (full rate within a distance, then half, a quarter, an eighth as the distance doubles). An AnimationTree has the same knob, [cw_animtree_set_update_divisor](api:cw_animtree_set_update_divisor): the whole graph is evaluated on every Nth tick with the time since. Clips are shared too: every character of a crowd can hold the same `cw_anim` in its player, since bone tracks name bones by a path relative to the character.

## AnimationTree

An AnimationTree points at a player (its `player` property; stop the player's own playback) and evaluates a graph of blend nodes into the same mixer. The graph is written as text in the scene file (below) or built in code; either way its nodes are addressed by `cw_blend_id`, each has at most one parent, and the first one created is the root unless [cw_animtree_set_root](api:cw_animtree_set_root) (or a `root` line) says otherwise.

| Node | Built with | Does |
|---|---|---|
| clip | [cw_animtree_clip](api:cw_animtree_clip) | plays one library clip at its own speed |
| 1D blend space | [cw_animtree_blend1d](api:cw_animtree_blend1d) | children at positions on one parameter; the two around the value blend linearly |
| 2D blend space | [cw_animtree_blend2d](api:cw_animtree_blend2d) | children at points on two parameters, gradient-band weights (no triangulation) |
| blend2 | [cw_animtree_blend2](api:cw_animtree_blend2) | `lerp(a, b, param)` |
| add | [cw_animtree_add](api:cw_animtree_add) | base plus an additive branch scaled by a parameter, relative to rest values |
| layer | [cw_animtree_layer](api:cw_animtree_layer) | the base in full, with an overlay replacing it on the bones the overlay writes (an upper body that aims while the legs run) |
| one-shot | [cw_animtree_oneshot](api:cw_animtree_oneshot) | a layer that plays its overlay once each time a parameter is set, with a fade in and a fade out (an attack, a hit reaction, a wave) |
| time scale | [cw_animtree_time_scale](api:cw_animtree_time_scale) | runs its child at a parameter times the tree's speed |
| state machine | [cw_animtree_state_machine](api:cw_animtree_state_machine) | states playing graph nodes, transitions with fades, conditions and switch modes |

Any subtree can be restricted to a set of bones or node paths with [cw_animtree_set_filter](api:cw_animtree_set_filter) (a bone mask). A name ending in `+` is that bone and every bone below it, so `Spine+` is the whole upper body of most rigs. Parameters are named floats set with [cw_animtree_set_param](api:cw_animtree_set_param); transition conditions read them (`"speed > 0.5"`, `"!grounded"`). Every node evaluated in a tick advances its time even at weight 0, so the children of a blend space stay in phase.

Transitions are `AUTO` (taken when the condition holds), `ENABLED` (only by travel) or `DISABLED`, and switch `IMMEDIATE` or `AT_END` (when the current state finishes or wraps). [cw_animtree_sm_travel](api:cw_animtree_sm_travel) walks to a state along the shortest chain of transitions, taking each with its fade. While a cross-fade runs no other transition is evaluated, so keep fades short where transitions chain.

`samples/anim_skinned` drives a skinned glTF character from a gamepad. Its graph is a state machine (a 1D speed blend space of idle, walk and a faster walk, plus a one-shot wave reached by travel) with an additive, bone-masked 2D look blend space on top:

@include samples/anim_skinned/main.c#graph

Each fixed tick the game only sets parameters and asks for travel; the tree does the rest:

@include samples/anim_skinned/main.c#params

Note the comment in the sample: the app's `fixed_update` runs after the tree, so input applied there takes effect on the next tick. A game that wants the tree stepped by its own simulation instead sets the tree's `process` to `MANUAL` and calls [cw_animtree_advance](api:cw_animtree_advance) in its `fixed_update`; the model's skeleton modifiers still adjust that pose in the same tick ([Inverse kinematics](#inverse-kinematics)). The sample also snaps stick values to 1/64, because hosts quantize analog axes slightly differently and the gate compares poses exactly.

### Blend graphs in scene files

A tree's graph is text in the `.cwscene` that owns the tree, after the animations: an `[animtree tree="..."]` section with one statement per line. No tool is needed to write one. This is the whole animation logic of the 3D platformer's hero, from `games/platformer3d/assets/scenes/player.cwscene`:

@include games/platformer3d/assets/scenes/player.cwscene#animtree:Tree

The game's code builds nothing. It finds the state machine by name once and sets parameters every tick:

@include games/platformer3d/nodes.c#tree-params

A statement is a keyword and `key=value` attributes (quote a value that holds spaces). Every graph node has a `name`, and other lines refer to it by that name; a node may be used before the line that defines it.

| Statement | Attributes | Meaning |
|---|---|---|
| `param` | `name`, `value` | a parameter and the value it starts with |
| `clip` | `name`, `anim`, `speed` (1) | plays the player's clip `anim` |
| `blend1d` | `name`, `param`, `sync` (false) | a 1D blend space; its children are `point` lines |
| `blend2d` | `name`, `param`, `param_y`, `sync` | a 2D blend space |
| `point` | `of`, `node`, `x`, `y` (2D only) | puts the node `node` at a position in the blend space `of` |
| `blend2` | `name`, `a`, `b`, `param` | `lerp(a, b, param)` on every bone |
| `add` | `name`, `base`, `add`, `param` | the base plus `param` times the additive input; a `param` below 0 adds nothing (it does not subtract), so a look or lean that goes both ways is one clip per direction, each on its own `add` |
| `layer` | `name`, `base`, `over`, `param` (always 1), `bones` | the overlay replaces the base, by `param`, on the bones in `bones` that it writes |
| `oneshot` | `name`, `base`, `shot`, `param`, `fadein`, `fadeout`, `bones` | setting `param` above 0 plays `shot` once over the base |
| `timescale` | `name`, `node`, `param` | `node` at `param` times the speed |
| `machine` | `name` | a state machine; `state` and `transition` lines fill it |
| `state` | `of`, `name`, `node` | a state of the machine `of` that plays `node`; the first is the start state |
| `transition` | `of`, `from`, `to`, `xfade` (0), `if`, `advance` (AUTO), `switch` (IMMEDIATE), `sync` (false) | see below |
| `filter` | `node`, `bones` | a bone mask on any node: `bones="Head\|Neck"`, `bones="Spine+"` |
| `root` | `node` | the node the tree evaluates (default: the one nothing else uses first) |

A transition's `if` is a condition on one parameter: `"speed > 0.5"`, `"grounded"`, `"!grounded"`, `"dir == 2"`. `xfade` is the cross-fade in seconds. `advance=AUTO` takes the transition by itself when the condition holds (with no `if` and `switch=AT_END` that is "go on when this clip ends"), `ENABLED` only when code travels through it, `DISABLED` never. `sync=true` starts the new state at the old state's place in its cycle, so a walk becomes a run without a skipped step.

An attribute a statement does not know, a name that matches no node, a node used twice or a machine without states is an error that names the file and the line; nothing is silently ignored. Saving a loaded scene writes the section back byte for byte (defaults left out), which `unit-anim` checks.

**Layers and one-shots.** This graph runs the legs from a speed blend, lets the upper body aim by a parameter, and plays an attack once on top whenever the game sets `attack`:

```text
[animtree tree="Tree"]
param name="speed" value=0
clip name="idle" anim="idle"
clip name="walk" anim="walk"
clip name="run" anim="run"
blend1d name="move" param="speed" sync=true
point of="move" node="idle" x=0
point of="move" node="walk" x=1.6
point of="move" node="run" x=5
clip name="aim" anim="aim"
layer name="upper" base="move" over="aim" param="aiming" bones="Spine+"
clip name="swing" anim="attack"
oneshot name="top" base="upper" shot="swing" param="attack" fadein=0.08 fadeout=0.15 bones="Spine+"
root node="top"
```

A layer is not a `blend2`: bones outside the mask, and bones the overlay has no track for, keep the base's pose exactly, where `blend2` would fade every bone. `sync=true` on the blend space makes the walk and the run step together although their clips have different lengths. `cw_animtree_set_param(tree, "attack", 1)` fires the one-shot; the node sets the parameter back to 0 as it takes the request, a new request restarts the shot, a negative value fades it out early, and [cw_animtree_oneshot_active](api:cw_animtree_oneshot_active) says whether it is still playing. A layer is exact where it has the whole weight of its place in the graph (at the root, or in a state that is not cross-fading); under a blend with other nodes its overlay takes its share of what is there.

**Instances.** A scene that instances another may carry an `[animtree]` section for a tree inside the instance (`tree="Enemy3/Tree"`). It replaces that instance's graph and leaves the other instances alone; it is saved only while it differs from the instanced scene's own graph.

**From C and from the debug channel.** [cw_animtree_find](api:cw_animtree_find) gives a node's id by name, [cw_animtree_set_param](api:cw_animtree_set_param) and [cw_animtree_get_param](api:cw_animtree_get_param) use parameter names, and [cw_animtree_load_text](api:cw_animtree_load_text) and [cw_animtree_to_text](api:cw_animtree_to_text) read and write the body of a section, so a tool or a test can swap a graph at run time. A graph built with the C calls is not stored unless [cw_animtree_set_stored](api:cw_animtree_set_stored) asks for it. On a running game, `animtree_text <tree>` prints the graph, `animtree <tree>` its parameters, machines, one-shots and clip weights, `animtree_param <tree> <name> <value>` sets a parameter and `animtree_travel <tree> <machine> <state>` travels.

## Retargeting

A clip is made for one skeleton. Retargeting plays it on another with different proportions, a different rest pose and different bone names, so one set of walk, run and attack clips serves every humanoid in a game. The module is `cartwheel/retarget.h` ([reference](header:retarget.h)). It needs no tool: the settings are a few lines of text.

Both skeletons are described by a **bone map**: which bone fills each slot of a standard humanoid (`hips`, `spine`, `chest`, `upper_chest`, `neck`, `head`, and for each side `shoulder`, `upper_arm`, `lower_arm`, `hand`, `upper_leg`, `lower_leg`, `foot`, `toes` and three joints per finger). [cw_bone_map_auto](api:cw_bone_map_auto) fills a map from the bone names of the common rigs:

| Rig | Names it reads |
|---|---|
| Mixamo | `mixamorig:Hips`, `mixamorig:LeftUpLeg`, `mixamorig:LeftForeArm`, `mixamorig:Spine1` |
| VRM and glTF humanoid | `hips`, `leftUpperLeg`, `leftLowerArm`, `upperChest`, `J_Bip_L_UpperLeg` |
| Unreal style | `pelvis`, `thigh_l`, `calf_l`, `upperarm_r`, `spine_01`, `ball_l` (twist bones are left out) |
| Blender Rigify | `thigh.L`, `shin.L`, `upper_arm.L`, `forearm.L`, `spine.001` |
| 3ds Max Biped | `Bip001 L Thigh`, `Bip001 L Calf` |
| plain names with a side | `UpperLegL`, `Upper_Leg.L`, `LeftFoot`, `RightForeArm` |

Only the hips and both legs down to the feet are required; everything else is carried over where both skeletons have it. What the names do not say, a line of text does: `left_hand = GripL`, or `left_toes =` to clear a slot ([cw_bone_map_apply_text](api:cw_bone_map_apply_text); [cw_bone_map_to_text](api:cw_bone_map_to_text) prints a map).

For every frame the transfer does three things:

1. **Rotations, relative to the rest poses.** A target bone turns from its rest orientation by the turn the source bone made from its own, in model space, so it does not matter how either rig orients its bones or how many twist and helper bones sit between the mapped ones. Where the rest poses differ (a T-pose and an A-pose), the spine, arms and fingers are first lined up by direction: an arm that points down in the source points down in the target. Legs turn from their own rest pose (`align_legs` lines them up too), so a source that stands with its legs apart does not bow the target's knees.
2. **Hips travel scaled by leg length.** The hips move by the source's motion from rest times the ratio of the leg lengths, so a short character takes short steps and crouches a short way.
3. **Feet by IK.** Each foot is put where the source's foot is at the target's size, keeping the target's own stance width, with the leg solved by the two-bone solver above (the knee keeps its plane) and the foot keeping its transferred rotation. A leg that cannot reach lowers the hips until it can. Without this pass a different thigh-to-shin ratio moves the feet wherever the rotations happen to put them. `hand_ik` does the same for hands, measured from the chest and scaled by arm length, for two hands on a prop.

**Planted feet.** A walk only looks right when a foot on the ground stays where it landed. Many clips do not quite manage that themselves (their planted feet travel at an uneven speed), and no walking speed fixes it. With `plant_feet`, [cw_retarget_clip](api:cw_retarget_clip) finds each stance (the foot within the lowest tenth of its travel), moves every planted frame onto a straight line at the stance's mean speed, and eases the frames around it in and out. [cw_anim_ground_velocity](api:cw_anim_ground_velocity) returns the velocity of a clip's planted feet: move the character at the opposite of it. [cw_foot_plant](api:cw_foot_plant) is the same pass on bare arrays.

### At import: clips baked into the model

The usual place for retargeting is the pack step. A model's `.cwimport` sidecar names the model that has the clips, and `cwpack` bakes them onto this model's skeleton and writes them into it as ordinary glTF animations. The game loads them like the model's own clips and never sees the source skeleton. This is `samples/anim_retarget/project/models/runner.glb.cwimport`, the whole of the setup for one character:

@include samples/anim_retarget/project/models/runner.glb.cwimport

| Setting | Meaning |
|---|---|
| `retarget.source` | the model with the clips, a path from the project root |
| `retarget.clips` | which clips: `new name=source name`, or just a name; left out, every clip |
| `retarget.fps` | keys per second in the baked clips (30) |
| `retarget.plant` | the clips, by their new names, whose planted feet get their slide taken out (walk and run cycles) |
| `retarget.foot_ik`, `retarget.hand_ik`, `retarget.align_rest`, `retarget.align_legs`, `retarget.hips_scale` | as the options above (true, false, true, false, the leg ratio) |
| `retarget.map.<slot>` | this model's bone for a slot the names do not give (`none` clears one) |
| `retarget.source_map.<slot>` | the same for the source model |

A slot or a bone that does not exist, or a model whose legs cannot be found, stops the pack with a message that names the setting to add.

### At load, and per frame

[cw_retarget_clip](api:cw_retarget_clip) does the same from C: it takes a clip, the two skeletons as [cw_pose](api:cw_pose_bind) (bound to a glTF, a Skeleton3D's pose, or free-standing) and their maps, and returns a new clip with a rotation track per mapped bone and a position track for the hips:

@include samples/anim_retarget/main.c#load

For a puppet that follows another skeleton live, [cw_retarget_new](api:cw_retarget_new) prepares a pairing once and [cw_retarget_apply](api:cw_retarget_apply) writes one pose onto another each tick; [cw_retarget_apply_feet](api:cw_retarget_apply_feet) takes the two foot positions from the game instead (a foot on a ledge).

### The sample

`samples/anim_retarget` plays one walk cycle and one punch, keyed for a 1.74 m coach with plain bone names, on three other people (all four are generated by the sample's `make-characters.mjs`): a 1.86 m runner with long legs, Mixamo bone names and a T-pose; a 1.58 m gymnast with short shins, VRM names and an A-pose; a 1.78 m lifter with a broad build, short legs, Unreal names and bones that point along +X. The first two get their clips from `cwpack`, the third at load. Each is an instance of one scene whose AnimationTree walks and layers the punch over the walk from the spine up, so the legs keep walking while the arms punch; the two instances with other bone names carry their own `[animtree]` override.

Everyone walks across the floor at the speed their own clip gives ([cw_anim_ground_velocity](api:cw_anim_ground_velocity)):

@include samples/anim_retarget/main.c#speed

The sample measures what matters by eye: while a foot is planted, how far does it travel over the ground? The figure is the furthest any planted foot moved during one stance, over seven seconds of walking:

| | Runner (1.86 m) | Gymnast (1.58 m) | Lifter (1.78 m) | Coach (the source clip) |
|---|---|---|---|---|
| retargeted with `plant_feet` | 0.002 mm | 0.002 mm | 0.003 mm | 617 mm |
| control: foot IK, no planting | 688 mm | 542 mm | 577 mm | |
| control: rotations only, no foot IK | 697 mm | 567 mm | 411 mm | |

The gate holds the people under 5 mm. The coach's own walk is keyed from joint angles and slides a great deal (its planted feet travel unevenly, and no walking speed takes that out); foot IK alone reproduces the source's feet exactly, slide included, at each person's size, which is the middle row. The clips `cwpack` baked and the same clips made at load agree to 0.0004 mm at the feet.

`RETARGET_SHEET=walk`, `walk_side`, `attack` or `attack_side` stands the coach and the three people in a row, facing the camera or side on, with that clip on the whole body: the view to judge a retarget against its source by eye.

**Cost.** Retargeting a clip is a load-time or pack-time cost: one pose transfer and two two-bone solves per key. At run time a retargeted clip is an ordinary clip. For the trees themselves, `RETARGET_CROWD=100` adds a hundred more lifters, cheap clones that share the model and the two clips, each with its own tree (a walk with the punch layered over it by a one-shot, 22 bones). On the 890M laptop (Ryzen AI 9 HX 370, native, load average about 49 from other work) a tree costs 9.1 us per character per tick on average and 4.4 us on the quietest tick: 0.9 ms and 0.44 ms for the hundred. The same cart on a WebGPU host measured 10.3 us and 5.9 us. On a Vega 8 laptop (about half the per-core speed) expect about twice that. There is no GPU cost, and the quality presets do not change it; for a far crowd, [cw_animtree_set_update_divisor](api:cw_animtree_set_update_divisor) evaluates a tree on every Nth tick.

```sh
node tests/gate.mjs anim-retarget unit-anim
```

Everything here uses only arithmetic, `sqrt` and the `cartwheel/math.h` functions, so the same clip retargets to the same bits on every host; the gate compares the people's pose hashes across hosts.

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

Modifiers are nodes, children of the Node3D that holds a skinned model (or of its [Skeleton3D](node:Skeleton3D), below), and they run in child order every fixed tick:

1. The AnimationPlayer or AnimationTree writes the animated pose onto the bones, as usual.
2. After every node's `fixed_update` and the physics step, the model's pose is captured into a `cw_pose`: local translation, rotation and scale per bone, parent indices and model-space transforms.
3. Each active modifier adjusts the pose the previous one left.
4. The pose is written back to the bones once, and the skin draws from it.

Every tick starts again from the animated pose, never from the last tick's solution. A bone the animation does not write (a static model, a bone no track drives) gets its last animated value back first, so results never pile up. A bone that game code or an `UPDATE`-mode player writes keeps that value as its animated value.

Your app's `fixed_update` runs after the modifiers. When it writes a model's bones there, most often by advancing a `MANUAL` AnimationTree or AnimationPlayer ([cw_animtree_advance](api:cw_animtree_advance), [cw_anim_player_advance](api:cw_anim_player_advance)) or by posing bones from code, the scene runner solves that model's modifiers again right after your `fixed_update`, the same tick ([cw_skeleton_modifiers_refresh](api:cw_skeleton_modifiers_refresh)). They start from the pose you left and from their targets where you moved them, so a manual advance gets its look-at, reaches and planted feet in the frame it is drawn, with no tick of lag and no hook of your own. Foot placement's easing and SpringBone3D's tails advance once a tick however often a model solves in it. A model whose bones you do not write in `fixed_update` is not solved again: a target you move there applies on the next tick. An app that runs [cw_tree_fixed_update](api:cw_tree_fixed_update) itself calls [cw_skeleton_modifiers_refresh](api:cw_skeleton_modifiers_refresh) after its own tick code.

```c
static void fixed_update(void *u, double dt) {
  cw_animtree_set_param(hero_tree, "speed", speed);
  cw_animtree_advance(hero_tree, hitstop ? 0 : dt);   /* a MANUAL tree, stepped by the game */
  /* the look-at and the feet on this model adjust the advanced pose this tick, before it is drawn */
}
```

| Node | What it does |
|---|---|
| [TwoBoneIK3D](node:TwoBoneIK3D) | an arm or a leg (`root_bone`, `middle_bone`, `end_bone`) reaches a `target`. The middle joint bends toward a `pole` (`use_pole`), or to the side the animation bends it (its offset from the line from the root to the end bone), so a target moving across the upper bone never flips the joint round. `soften` eases the last stretch so a straight limb does not snap. `use_target_rotation` gives the hand or foot the target's rotation (grip a handle, hold a wheel). `reached` reports whether the target was in reach |
| [LookAtModifier3D](node:LookAtModifier3D) | turns a `bone` toward a target by the shortest rotation, at most `limit_angle` from where the animation points it. `forward_axis` is the way the bone faces at rest, in model space (`(0, 0, 1)` for a glTF character's head). Chain two, a neck at `influence = 0.4` and then the head, to spread the turn |
| [CCDIK3D](node:CCDIK3D) | cyclic coordinate descent from `end_bone` up to `root_bone`, the same algorithm and limits as three.js r187's `CCDIKSolver`. Per-joint limits come from [cw_ccdik3d_set_joint](api:cw_ccdik3d_set_joint) |
| [FABRIK3D](node:FABRIK3D) | FABRIK over a chain (tails, spines, cables), with cone and hinge limits per joint ([cw_fabrik3d_set_joint](api:cw_fabrik3d_set_joint)) |
| [FootPlacement3D](node:FootPlacement3D) | plants feet on the physics ground. Each leg casts a ray down through its animated ankle, the ankle is lifted or lowered by the ground's height (keeping the animation's own swing), the pelvis drops when a foot has to reach down, each leg is solved as two-bone IK (the knee bends to the side the animation bends it; a leg the animation holds straight bends to the side it last bent to, so a high or straight leg never flips round), and the foot turns onto the ground normal while it is near the ground. A raised ankle stays well below its hip, and no leg bone turns more than 8 degrees in a tick past what the animation turns it, at any pace (below) |
| [SpringBone3D](node:SpringBone3D) | secondary motion: hair, braids, ponytails, beards, tails, coat tails and robe panels lag behind the body, sway and settle back to the animated pose, kept out of the body by spheres and capsules on its bones (below) |

"Rest" (a look-at's `forward_axis` and `offset`, a spring collision shape's offsets) is the skin's bind pose, read from its inverse bind matrices, not whatever the bones hold when the modifiers first run: a game can build a model, its AnimationTree and its modifiers and advance the tree in the same tick.

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

#### Feet at a run

A run is where foot placement is hardest: the feet cross stone edges between one tick and the next, a swinging foot is already high when the ground under it rises, and a leg near straight turns tens of degrees for a few centimetres of reach. FootPlacement3D holds three things at every pace, turn and stop:

- **No leg turns round in a tick.** What the solve does to each thigh, shin and foot (its turn off the incoming, animated rotation) changes by at most 8 degrees a tick (480 a second), from the hip down. A target that jumps (a ray landing on another surface under a planted foot, a swinging foot's ground snapped as it touches down) is followed over a few ticks; a target that moves smoothly is met exactly. A planted foot crossing a step's edge reaches its new ground a few ticks later instead of in one.
- **No foot is pulled up over its hip.** A raised ankle keeps at least half the height the animation gives it below the hip (along up), or a quarter of the leg's length when that is less; a foot the animation holds at or above the hip (a kick, a high knee) moves with the pelvis alone. Without it, a swinging foot already 0.35 m up over a 0.45 m stone beside a run was pulled above the hip and the thigh turned from down and back to up and forward in one tick (162 degrees).
- **The hips move at most three leg lengths a second** when they drop for a low foot or come back up, since they carry both legs.

These ease only with `blend_speed` above 0: at 0 (at once) the feet, legs and pelvis take their targets in the tick. A model that moves more than a metre in one tick (a respawn, a teleport) takes its new ground and solve at once too.

`samples/anim_feet` runs a person (the coat figure of `anim_robe`) at 5.4 m/s over stones 5 to 50 cm high with its feet placed at full influence, through a turn at speed, a sharp turn, a stop, a turn on the spot, a platform and another stop; a twin runs the same clips with rays that hit nothing, so its legs are the animation's. The probe measures every leg bone's turn in a tick against the twin's, how far each ankle stays under its hip, and whether planted feet sit on raised ground:

@include samples/anim_feet/main.c#probe

```sh
node tests/foot-run-turn.mjs     # each check with its control, native and romdev, GL and WebGPU
```

The worst leg bone turns 8 degrees past the animation in a tick; with the turn limit off (`CW_TEST_FOOT_NO_RATE=1`) one turns 67. With the reach guard off (`CW_TEST_FOOT_NO_REACH=1`) an ankle keeps only 0.71 of the height under its hip the guard leaves it.

### Hair, tails and coats: SpringBone3D

Skinned parts that should swing on their own (a ponytail, braids, a beard, a tail, coat tails) are bone chains in the model, and a [SpringBone3D](node:SpringBone3D) moves them. A garment all round the legs (a robe, a long coat) is the same chains in skirt mode: see [Robes, skirts and long coats](#robes-skirts-and-long-coats-skirt-mode). It is a modifier like the others, a child of the model, run in child order after the animation. Under it go one [SpringBoneChain3D](node:SpringBoneChain3D) per chain and any number of [SpringBoneCollision3D](node:SpringBoneCollision3D) shapes; every chain of the node stays out of every shape. (Cloth3D is for capes and flags: a simulated sheet, not skinned bones.)

`samples/anim_spring` is a person in a coat with a ponytail and two coat tails, generated by `samples/anim_spring/make-character.mjs` (each chain ends in an `_end` bone, as glTF exporters write them):

@include samples/anim_spring/assets/main.cwscene#Springs..Head

| SpringBoneChain3D | |
|---|---|
| `root_bone`, `end_bone` | the chain, from the first bone that swings down to the last (below it). The end bone's tip is its child bone (an `_end` marker, the next bone), or else its own length again along the incoming direction |
| `stiffness` | 1/s², default 120: the pull of each joint back toward where the animation puts it (below its already simulated parent). One joint alone sways at about √stiffness / 2π times a second; a chain of several is looser, so a four-bone ponytail wants about 250 |
| `damping` | 1/s, default 8: drag on the swing. Only the motion apart from the animation is damped, so motion at a steady velocity (a glide, a slide) leaves no lag; starts, stops, turns and the bob of a walk or a run do: `anim_spring`'s tips trail 9 to 12 cm at the walk's pace and 12 to 20 cm as it starts. Damping well under critical (about `2 * sqrt(stiffness)`: 22 at the default stiffness, 35 at 300) lets a short stiff chain build up when a run's bob comes near its own sway rate, until it swings over its bone; give such chains damping near critical (in `unit_ik`, a chain of stiffness 300 driven at its own rate swings 3.14 rad at damping 8, with no limit, and 0.69 rad at 35) |
| `gravity`, `gravity_strength` | a world direction (down) and m/s² (default 0). Each joint sags about `gravity_strength / stiffness` metres from its animated place |
| `radius` | m, default 0.02: each joint's and the tip's hit radius against the shapes |
| `max_angle` | radians, default 2 (115 degrees): the most a joint swings off the way the animation points it, so a chain never flips over its bone (a run's bob at the chain's sway rate, a step-up, a hard turn); 0 is no limit |
| `tip_offset`, `tip_distance` | read-only: the tip from where the animated pose puts it, last tick (world, metres) |

| SpringBoneCollision3D | |
|---|---|
| `bone` | the bone the shape rides |
| `shape` | `SPHERE` (at `offset`) or `CAPSULE` (from `offset` to `tail`) |
| `radius` | m |
| `offset`, `tail` | metres from the bone's origin, along the model's own axes in its rest pose (y up, z forward for a glTF character), whatever the bone's local axes are |

The SpringBone3D itself has `active`, `influence`, `collisions` (default true) and:

- `teleport_distance` (m, default 0.25) and `teleport_angle` (radians, default 1.5): a model that moves or turns further than this in one tick has teleported, and the chains move with it just as they hang. A respawn, a cut or a warp needs no call, and neither does a character controller stepping up a ledge in one tick (a step-up of 0.41 m with the old default of 1 m flung every chain over the head). Keep it under the tallest step your controller climbs in a tick and over the farthest a character moves in one at its fastest (0.25 m is 15 m/s; raise it for a character riding something faster). 0 turns a check off.
- `reset`, or [cw_spring_bone3d_reset](api:cw_spring_bone3d_reset): the chains start again from the animated pose on the next tick (a new character in a pool, a cut to a pose that has nothing to do with the last one). A SpringBone3D that misses a tick (inactive, just added) starts again the same way. `resets` counts both.

How it works: each joint's tail is a Verlet point kept in world space from one tick to the next, so moving or turning the model leaves it behind. Each tick, from the root down, the tail keeps its velocity less the drag, the spring pulls it toward where the animation puts it below the simulated parent, gravity sags it, the bone turns at most `max_angle` off its animated direction, the shapes turn it out of them along the sphere of the bone's length (so the length and the shapes hold together: no last length pass pulls a joint back inside a shape), and the bone turns by the shortest rotation onto it. Every tick starts from the animated pose, as for every modifier; only the tails carry over.

[cw_spring_chain3d_joint_world](api:cw_spring_chain3d_joint_world) gives each simulated joint and [cw_spring_collision3d_world](api:cw_spring_collision3d_world) each shape, in the world, for a debug drawing or a test; the `ik <model path>` debug request lists each chain's tip offset.

The sample walks, stops, spins three turns in under two seconds, stands, teleports 6 m and walks back. Its probe measures the deepest any simulated joint of any chain goes into any shape:

@include samples/anim_spring/main.c#probe

```sh
node tests/gate.mjs unit-ik anim-spring
node tests/spring-bone.mjs        # each check with its control, native and romdev, GL and WebGPU
```

- Motion: the tips trail their animated places by 12 to 20 cm as the walk starts (9 to 12 cm at its pace, from its bob: `sb.lag_steady`) and are back within 1 cm, for good, 18 to 62 ticks after the stop. Control: the modifier off, no lag.

`unit_ik` adds three cases (gate `spring-collide`, on GL and WebGPU as a cart): a short hair chain (6 mm joints) against a neck capsule with the head bobbing at a run stays outside it (6 mm clear; control `CW_TEST_SPRING_OLD_COLLIDE=1`, the old order of length and push, 5.3 mm inside); a stiff chain driven at its own sway rate swings at most `max_angle` (with no limit it flips, 3.14 rad); a 0.41 m step-up in one tick leaves the chain hanging (the old defaults, 1 m and no limit, fling it 3.06 rad).
- Collision: through the spin the tips fly out 40 to 43 cm and every joint stays at least 1.9 cm outside every shape. Control: collisions off, joints go 8.5 cm inside.
- Teleport: after the 6 m jump every tip's offset matches an identical twin's that did not jump, to the bit, and the reset call puts every tip on its animated place. Control: the jump check off, the tips whip 55 to 58 cm.
- Determinism: the pose hash per 60-tick window is the same bits natively and in the cart, on GL and WebGPU.

Cost, measured on the 890M laptop through romdev with a release cart (the modifier phase's own clock, which includes capturing and writing back each 35-bone pose): seven people with three chains each (21 chains, 3 to 4 joints, five shapes) take 63 us a tick on average, 50 us on the quietest tick: about 3 us a chain, 9 us a character. Eight people (24 chains) run one model per job, 74 us on average and 37 us on the quietest tick. A chain costs a few multiplies per joint and shape, so cost grows with joints times shapes. There is no GPU cost.

### Robes, skirts and long coats: skirt mode

A garment that hangs all round the legs (a robe, a long skirt, a long coat) needs more than chains on their own. Legs pass between two chains, and a thigh swinging into a chain pushes it out by its whole depth in one tick, which throws it. Set `skirt` on the SpringBone3D and its chains become one garment:

- `skirt = RING` (a robe, a skirt) links each chain to the next, in child order, at every joint, and the last to the first. `skirt = OPEN` (a coat open at the front) leaves the last and the first apart: list the chains from one front edge round the back to the other.
- Every link stays between `(1 - skirt_squeeze)` and `(1 + skirt_stretch)` times its animated length (0.5 and 0.05: panels bunch up to half and barely stretch). A garment the legs must spread when running wants `skirt_stretch` about 0.3.
- Every bone, every link and the line across the middle of every panel between two chains is kept out of every shape, capsule against capsule. A shin folded up behind fits between two links; it does not fit past that middle line.
- The shapes are inside the garment. A segment found on a shape's inner side (toward the garment's axis, from the chains' roots toward their tips) is pushed out through the shape's outer side, so a leg that got through in a fast tick ends up inside the cloth again, never outside it.
- A pushed tail keeps none of its velocity away from the shape (apart from the animation's), and each tail is pulled toward where the animation puts it in the model, not below its simulated parent (hung from the parent, the lags of the levels add up).
- `skirt_iterations` (4) passes of lengths, links and shapes run each tick. Each chain's `max_angle` holds as in chain mode: no bone turns further than that from its animated direction.

Build the garment from bone chains in the model: one chain per panel edge, all hanging from the same bone (the hips) and all with the same number of bones, each ending in an `_end` bone. Skin the garment's surface across, between the two nearest chains by angle, and down, along the bone each vertex lies on. Put capsules on the thighs, the shins and the feet (a heel flicked up behind a running figure goes through a long robe's back otherwise), a little fatter than the mesh. Feet below a robe's hem still need theirs: the hem lifts onto the feet.

`samples/anim_robe` has two figures generated by `samples/anim_robe/make-figures.mjs`: a long robe on eight chains of four bones from the waist to the ankles, and a long coat open at the front on five chains of three bones to below the knees. The robe:

@include samples/anim_robe/assets/main.cwscene#Robed/Model/Robe..Robed/Model/Robe/RobeF

and the shapes on one leg, on both figures:

@include samples/anim_robe/assets/main.cwscene#Robed/Model/Robe/ThighL..Robed/Model/Robe/FootL

| Property | Robe (8 x 4 bones, waist to ankle) | Coat (5 x 3 bones, waist to below the knee) |
|---|---|---|
| `skirt` | `RING` | `OPEN` |
| `skirt_stretch` | 0.3 | 0.3 |
| chain `stiffness`, `damping` | 220, 6 | 240, 6 |
| chain `gravity_strength`, `radius` | 2, 0.025 | 2, 0.025 |
| shapes | thighs 0.085, shins 0.07, feet 0.05 | the same |

Both run at 5.4 m/s, turn half round at speed, stop, turn half round on the spot, run and stop again. The sample measures the legs (capsules placed from the bones: hip to knee, knee to ankle, ankle to toes) against the garment's surface, sampled five by five in every panel:

@include samples/anim_robe/main.c#probe

```sh
node tests/gate.mjs unit-ik anim-robe
node tests/robe-skirt.mjs        # each check with its control, native and romdev, GL and WebGPU
```

The same bits natively and in the cart, on GL and WebGPU (the pose hash per 60-tick window):

- Legs: no thigh, shin or foot reaches either garment's surface. In the runs the nearest stays 8.7 mm outside the robe and 6.5 mm outside the coat, at the stands and in the turn 2 cm. Control: skirt `OFF` (the chains on their own), legs 8.3 cm through.
- Settling: after every stop each tip is back within 1 cm of where it then hangs, for good, 28 to 34 ticks later. At rest a tip's offset strays from its 5-tick running mean by 0.018 mm at most. Control: chains on their own take 84 ticks or never settle, and shake by up to 7.6 mm.
- No flip: in the runs no tip strays more than 0.39 m (robe) or 0.30 m (coat) from its animated place. Control: chains on their own throw a tip 1.04 m.
- Cost, with the modifier phase's own clock, including the capture and write-back of each 62-bone pose. On the 890M laptop with a release cart through romdev: two people 23 us a character on average, 19 us on the quietest tick; eight people (one model per job) 18.5 us and 12 us. The chains on their own cost 13.7 us and 11.1 us. Natively, eight people take 13.6 us a character on average and 9.5 us on the quietest tick. There is no GPU cost.

Two measured choices are behind this. Each tail is pulled toward its place in the model, not below its simulated parent: with the parent-relative pull of chain mode the same run settled 60 to 98 ticks after each stop, with up to 1.7 mm of rest jitter. A pushed tail loses only its velocity away from the shape. Crediting the whole of each push to the tail's previous place instead (the passes undo much of each push, so that credit is far larger than the move) threw the robe up over the thighs within a few strides.

The sample can also run each garment as an XPBD cloth (cartwheel/cloth.h, `ROBE_CLOTH=1`, natively): particles on the chains' joints, the waist pinned, kinematic Box3D capsules on the legs. That is how skirt mode was chosen; on the same run:

- Legs 8.5 cm through both garments in the runs. Cloth collides at its particles only, so a leg passes between two of them, as it does between chains.
- No settling: the robe never settled after a stop, and rest jitter reached 52 to 65 mm. With damping 8 the coat settled in 31 to 49 ticks, but the robe still did not (jitter 18 to 26 mm, the closed ring hanging on the legs).
- Cost natively: 22 us a garment on a quiet machine, 57 us on a loaded one, for the cloth and its share of the world step that moves the capsules. The whole modifier phase with skirt mode took 31 and 58 us a character in the same conditions. The cloth's figure is before any work to drive the garment's bones from the particles.

### The solvers from C

The nodes call plain functions that work on any `cw_pose`. You can use them directly, for example to pose a procedural rig or to bake a pose:

- [cw_ik_two_bone](api:cw_ik_two_bone): no trigonometry at all. The law of cosines gives the middle joint, and each bone turns by the shortest rotation.
- [cw_ik_aim](api:cw_ik_aim): an aim with an angle limit.
- [cw_ccdik_solve](api:cw_ccdik_solve): r187's `CCDIKSolver.update`. It keeps its link order, the 1e-5 skip, min and max angles, the `limitation` axis projection, the Euler clamp, the early stop and the final slerp.
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

The gates compare solver results against three.js r187. They plant the robot's feet on the stairs: the ankle's height above the ground under it must match its animated height above flat ground, within 1 cm through every stance (worst 3.8 mm). They keep the hand within 1 mm of the handle (0.1 mm), and they compare pose hashes across hosts and worker counts:

```sh
node tests/gate.mjs unit-ik anim-ik ik-manual-refresh ik-manual-advance foot-high-sweep
node tests/ik-determinism.mjs
```

`unit-ik` checks the solvers (reach accuracy, the pole plane, limits, FABRIK constraints) and compares CCD against three.js r187 run in Node (`tools/gen-ccdik-fixture.mjs` writes the reference). It also runs the nodes in a tree (no accumulation across ticks, influence, foot placement on Box3D ground, a 12-rig crowd through the job pool).

`ik-manual-refresh` and `ik-manual-advance` hold the same-tick solve of a manual advance: in `unit_ik` a look-at 70 degrees to the side turns the head drawn each tick by 1.2217 rad off the animated head (worst error 0.0005 rad), and foot placement refreshed every tick ends bit for bit as without a refresh; in `anim_ik` with `IK_MANUAL=1` the front robot's player is `MANUAL`, advanced in the app's `fixed_update`, and the head drawn every frame turns toward a firefly held 70 degrees to its side, on GL and WebGPU. Their control, `CW_TEST_IK_NO_REFRESH=1`, draws the advanced pose without the modifiers: 0 rad. `foot-high-sweep` swings a leg through a high kick with the leg near straight, a heel drawn up to the hip and back, over ground off the model's origin: the upper leg never turns more than 12 degrees a tick past the animation (3.5 at most); its control, `CW_TEST_IK2_OLD_BEND=1` (the bend side from the thigh's direction off the new reach line, as before 0.72.0) with `CW_TEST_FOOT_NO_RATE=1` (no limit on how fast the solve turns a leg, which would spread the flip over several ticks), flips the knee 69 degrees in a tick.

The debug command `ik <model path>` lists a model's modifiers with their last results (`reached`, `in_limit`, foot offsets, pelvis offset), the pose hash and the microseconds the last tick took.

## Skeleton3D and BoneAttachment3D

A [Skeleton3D](node:Skeleton3D) is a skinned model's bones as a node: count, names, parents, and each bone's rest and pose transform to read and set, from C and over the debug channel. A [BoneAttachment3D](node:BoneAttachment3D) is a Node3D that rides one bone, so whatever you put under it (a sword, a hat, a muzzle flash, a hit shape) follows the hand or the head. The module is `cartwheel/skeleton.h` ([reference](header:skeleton.h)).

A Skeleton3D is a view of the model's bones, not a copy. The bones stay the three.c objects glTF loaded, and bone tracks, AnimationTree, skeleton modifiers and ragdolls write them exactly as before; a scene without a Skeleton3D behaves as it always did. Put the node in either place:

```text
[node name="Hero" type="MeshInstance3D"]          ; a skinned glTF in `mesh`, or cw_node3d_set_object from code
[node name="Skeleton" parent="Hero" type="Skeleton3D"]
[node name="Sword" parent="Hero/Skeleton" type="BoneAttachment3D"]
bone_name = "Palm2L"
[node name="Blade" parent="Hero/Skeleton/Sword" type="MeshInstance3D"]
[node name="Look" parent="Hero/Skeleton" type="LookAtModifier3D"]
bone = "Head"
```

Under the model's node, as above, the skeleton is its parent's. A Skeleton3D can also hold the model itself (its own `mesh`-carrying child, or `cw_node3d_set_object` on the Skeleton3D), a common layout of imported scenes; `model` names another node by path when neither fits. The skeleton binds as soon as the model's bones exist (a mesh resource may arrive a few frames after the scene loads), and binds again if the model is replaced. Until then `bone_count` is 0. The signal `skeleton_bound` says when.

| Call | What it does |
|---|---|
| [cw_skeleton3d_bone_count](api:cw_skeleton3d_bone_count), [cw_skeleton3d_find_bone](api:cw_skeleton3d_find_bone), [cw_skeleton3d_bone_name](api:cw_skeleton3d_bone_name), [cw_skeleton3d_bone_parent](api:cw_skeleton3d_bone_parent) | the bones, parents before children |
| [cw_skeleton3d_bone_pose](api:cw_skeleton3d_bone_pose), [cw_skeleton3d_set_bone_pose](api:cw_skeleton3d_set_bone_pose) | a bone's local translation, rotation and scale now; setting one takes effect at once |
| [cw_skeleton3d_bone_rest](api:cw_skeleton3d_bone_rest), [cw_skeleton3d_set_bone_rest](api:cw_skeleton3d_set_bone_rest), [cw_skeleton3d_reset_bone_pose](api:cw_skeleton3d_reset_bone_pose) | the rest transform (the bone as the file has it), and back to it |
| [cw_skeleton3d_bone_model_transform](api:cw_skeleton3d_bone_model_transform), [cw_skeleton3d_bone_global_transform](api:cw_skeleton3d_bone_global_transform) | the bone in the model's space and in the world |
| [cw_skeleton3d_pose](api:cw_skeleton3d_pose) | the whole skeleton as a `cw_pose`, for the solvers below and for ragdolls |
| [cw_skeleton3d_hash](api:cw_skeleton3d_hash) | a hash of every bone's pose, for tests that compare hosts |

Who wins when several things write a bone: an animation with a track on that bone writes it every tick, so pose a bone from code only where no playing clip has a track for it, or after the animation in your own `fixed_update`. `show_rest_only` holds the whole skeleton at rest each tick whatever played. Bone tracks may name the Skeleton3D (`Hero/Skeleton@Head:quaternion`) or, as before, the model's node.

A BoneAttachment3D takes its bone's transform after animation, modifiers and physics each tick, and again just before each frame is drawn, from the bones as they are then. It is drawn exactly where the bone is drawn, the same frame: there is no tick of lag behind a fast swing, an `UPDATE`-mode player or a ragdoll your own code writes. It is placed relative to the model, so a character that is drawn interpolated between ticks carries its attachments with it. The bone's scale comes along: a rig authored in centimetres under a x100 armature (common from Blender) gives the attachment a scale of 100, so size what you hang on it in the bone's units, or put a scaled Node3D between. The attachment's own transform is overwritten every tick; offset the sword with a child node.

`samples/skeleton_attach` has three robots. One walks with a sword on its palm and a hat on its head, a look-at modifier under its skeleton and its walk clip addressed to the skeleton node. One holds its rest pose under a playing clip (`show_rest_only`). One has no animation and is posed from code each tick:

@include samples/skeleton_attach/main.c#puppet

The gate `skeleton-attach` compares the sword's world matrix with the palm bone's after every frame's 3D draw, to a tenth of a millimetre, on GL and WebGPU; its control places the sword from the bone transform read one tick earlier and fails. `unit-skeleton` covers binding in both layouts, late binding and rebinding, rest and pose, attachments through a moving model, modifiers and tracks through a skeleton, the scene-file round trip and that a freed skeleton lets go of its bones.

Cost: there is no GPU cost and no second pose. A bound skeleton costs one short check per tick and per frame, an attachment one bone transform (the chain of bones above it) per tick and per frame. 200 skeletons with 400 attachments on a 5-bone rig cost 0.23 ms per tick and 0.28 ms per frame natively on the 890M laptop (Ryzen AI 9 HX 370) at a load average of 20, about 0.6 microseconds per attachment each time; a Zen+ laptop is about twice that. The quality presets do not change it: attachments carry game state.

Over the debug channel: `skeleton <node>` (bound, bone count, hash), `skeleton <node> bones`, `skeleton <node> bone <name>` (pose, rest, world position), `skeleton <node> set <name> position|quaternion|scale <value>`, `skeleton <node> rest [<name>]`.

## Ragdolls

[cartwheel/ragdoll.h](api:cw_ragdoll_create) turns a skeleton into physics for a fall: a character knocked down, thrown or killed goes limp, lands on whatever is there, and then either stays down or blends back into animation and gets up. It works on the same [cw_pose](api:cw_pose_bind) the IK layer uses, so any skinned glTF character works.

You describe the body as a list of parts. Each part is one bone with a capsule from the bone's origin to a tip (a child bone, or an offset), and the joint that hangs it from the nearest ancestor part:

- `CW_RAGDOLL_BALL`: a cone around the bone's rest direction plus a twist range about the bone (shoulders, hips, spine, neck).
- `CW_RAGDOLL_HINGE`: one axis given in model space at rest, with an angle range (knees, elbows). 0 is the rest pose.

Limits are measured from the rest pose, so a ragdoll made from any animated pose has the same anatomy. Bones that are not parts (fingers, the face, twist bones) keep whatever the animation gives them and ride along with their part, so a knocked out face or a clenched fist stays on.

```c
static const cw_ragdoll_part PARTS[] = {
  { .bone = "Hips", .tip = "Spine", .radius = 0.14f },
  { .bone = "Spine", .tip = "Chest", .radius = 0.13f, .joint = CW_RAGDOLL_BALL, .swing = 0.45f, .lower = -0.35f, .upper = 0.35f },
  { .bone = "UpperLegL", .tip = "LowerLegL", .radius = 0.085f, .joint = CW_RAGDOLL_BALL, .swing = 1.25f, .lower = -0.45f, .upper = 0.45f },
  { .bone = "LowerLegL", .tip = "FootL", .radius = 0.065f, .joint = CW_RAGDOLL_HINGE, .axis = { 1, 0, 0 }, .lower = -0.05f, .upper = 2.4f },
  /* ... */
};
```

The usual flow:

1. On the fall: `cw_pose_capture`, then [cw_ragdoll_create](api:cw_ragdoll_create) with the model's world transform. The bodies start where the bones are. Give them the blow with [cw_ragdoll_set_velocity](api:cw_ragdoll_set_velocity) (the whole body, plus a spin) and [cw_ragdoll_apply_impulse](api:cw_ragdoll_apply_impulse) (one part, at a point).
2. Each tick, after the world step: `cw_pose_capture` (the animated pose), [cw_ragdoll_blend_pose](api:cw_ragdoll_blend_pose) with weight 1, `cw_pose_flush`. The skin follows the bodies.
3. To get up: when [cw_ragdoll_resting](api:cw_ragdoll_resting) says it has settled (or your rules say it is time), read [cw_ragdoll_axis](api:cw_ragdoll_axis) on the chest (its rest forward against world up) to choose a face-up or face-down get-up clip, turn the model so the clip's lying body points the way the ragdoll lies, play the clip and ramp the weight from 1 to 0 over a few tenths of a second. Then [cw_ragdoll_destroy](api:cw_ragdoll_destroy).
4. The dead: keep the ragdoll until it rests, then copy the pose and destroy it. A still body costs nothing.

A ragdoll lives in the [cw_world3d](guide:physics) it was made in and collides with everything there; its own parts never collide with each other. [cw_ragdoll_apply_force](api:cw_ragdoll_apply_force) on the root part is enough for a tether: a spring that keeps a limp body near a point your rules choose.

### Ragdolls and game rules

Box3D steps are bit-identical across hosts for the same inputs, and the ragdoll code uses only arithmetic and `sqrt`, so the same fall lands the same way everywhere ([cw_ragdoll_hash](api:cw_ragdoll_hash) folds every body; the sample's gate compares it across hosts). Even so, a game whose rules run on their own fixed tick is easier to keep correct when ragdolls are presentation: the rules decide who falls, where the body is and when it gets up, and the ragdoll only shows it. The brawler example works that way: its fights are identical with and without ragdolls.

### Cost

A ragdoll costs CPU only: the skin draws the same triangles whether bones come from a clip or from bodies. The figures below are the whole per-tick ragdoll work (pose captures, the Box3D step of the ragdoll world, writes and blends), measured on the 890M laptop (Ryzen AI 9 HX 370, native, load average about 8):

| Scene | Live ragdolls | Bodies | Per tick |
|---|---|---|---|
| a street brawl, everyone knocked down every 4 s (4 fighters, 14 enemies, a cap of 12) | 12 | 168 | 0.34 ms mean |

Resting ragdolls fall asleep and cost almost nothing; a dead body that has settled can be copied into a still pose and destroyed. On a Vega 8 laptop (about half the per-core speed) expect about twice that: 0.7 ms per tick for twelve live ragdolls. Cap the number of live ragdolls in game code (fall back to a keyed clip past the cap); the quality presets do not change ragdolls because they are a CPU cost the GPU presets do not touch.

The sample:

```sh
node tests/gate.mjs ragdoll
```

Four robots are knocked over one after another onto a floor, a kerb and a crate. The gate checks that every part bone in the written pose sits on its body (within 2 cm), that hinges stay inside their limits, that one robot lands on the crate, that all four get up, and compares the ragdoll hash per 60-tick window across hosts. Its control stops writing the ragdoll into the pose: the bones stay standing while the bodies lie, and the follow check fails.

## Determinism and testing

Animation that runs in the fixed step is bit-identical across hosts: time is a double advanced by the fixed `dt`, interpolation uses only arithmetic, `sqrt` and the `cartwheel/math.h` functions (three.c's own mixer calls libm, so {{name}} samples glTF clips itself and lets three.c only render the skin), blends accumulate in insertion order, and values are written through the reflection path. Players and trees keep a running hash of every value they write ([cw_anim_player_hash](api:cw_anim_player_hash), [cw_animtree_hash](api:cw_animtree_hash)); the samples publish one hash per 60-tick window as debug state, and the gates compare them across hosts:

```sh
node tests/gate.mjs anim-props anim-tween anim-skinned unit-anim
```

A running app answers animation questions over the [debug channel](guide:debug-channel): `anim <player>` (current clip, position, library, hash), `anim_play`, `anim_stop`, `anim_seek`, `anim_text <player> <clip>` (the clip as scene text), and `animtree <tree>` (parameters, state machines, one-shots, clip weights), `animtree_text <tree>` (the graph as scene text), `animtree_param`, `animtree_travel`.
