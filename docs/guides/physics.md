# Physics

{{name}} simulates 2D physics with Box2D v3 and 3D physics with Box3D, through thin wrappers whose 2D and 3D APIs mirror each other name for name, and a layer of physics node types for the scene tree. Both libraries step on the engine's job pool and are deterministic across worker counts and hosts.

Headers: [cartwheel/physics.h](header:physics.h) (what both dimensions share: layers, events, filters, stats, debug lines, joint types), [cartwheel/physics2d.h](header:physics2d.h), [cartwheel/physics3d.h](header:physics3d.h), [cartwheel/physics_nodes.h](header:physics_nodes.h) (the node types) and [cartwheel/physics_tasks.h](header:physics_tasks.h) (the job-pool glue, for code that creates raw library worlds). Samples: `physics3d_stack` and `physics2d_joints` (the wrapper API), `character3d` (the 3D mover), `physics3d_nodes`, `joints_nodes` and `games/action2d` (the node types), `physics_determinism` (determinism).

## Two ways in

- **Physics nodes.** Put a [RigidBody3D](node:RigidBody3D) with a [CollisionShape3D](node:CollisionShape3D) child in a scene and it falls. The tree owns one 2D and one 3D world, created when the first physics node enters, and steps them in the right place of the tick. This is the normal way to build a game.
- **The wrapper API.** Create a world, bodies, shapes and joints in C and step it yourself. Use it for apps that do not run a scene tree, for physics that is not part of the scene (a minigame, a simulation tool), or for full control. Every handle has a `_raw()` accessor returning the Box2D or Box3D id for anything the wrapper does not cover.

The node layer is built on the wrapper, so everything below about layers, events and determinism applies to both.

## Worlds, bodies, shapes

A world is created from a def ([cw_world3d_default_def](api:cw_world3d_default_def): gravity (0, -10, 0), 4 substeps, sleeping and continuous collision on) that can also name its 32 collision layers:

@include samples/physics3d_stack/main.c#world_def

Bodies are `STATIC`, `KINEMATIC` or `DYNAMIC` ([cw_body3d_create](api:cw_body3d_create)); shapes attach to bodies with a material (friction, restitution, rolling resistance, conveyor speed, and a `user_material` id reported in hits). 3D shapes: box, sphere, capsule, convex hull and static triangle mesh; 2D: box, circle, capsule, polygon, segment and chains (one-sided terrain). Units are meters, kilograms, seconds and radians. Handles are the libraries' own generational ids, so a destroyed body's handle reads as invalid, never as another body. The world's and bodies' user-data slots belong to the wrapper; use `cw_body3d_set_user` and the shape def's `user`.

## The typical frame

In `fixed_update`: set velocities, forces, kinematic targets and movers, step, then read the events:

@include samples/physics3d_stack/main.c#step

In `render`: write every bound body's transform into its three.c object (or a plain transform), interpolated between the last two steps by the render callback's `alpha`:

@include samples/physics3d_stack/main.c#render

[cw_world3d_sync](api:cw_world3d_sync) skips bodies that did not move, so a sleeping pile costs nothing per frame. Interpolation is presentation only: it may differ in the last bit between hosts, so never feed it back into game state. `cw_body3d_transform` is the simulation value.

## Layers

A shape sits on one layer (0..31) and collides with the layers in its mask; two shapes collide only when each one's mask holds the other's layer. A shared nonzero `group` overrides: negative never collides, positive always does. Layer names resolve to bits with [cw_world3d_layer_bits](api:cw_world3d_layer_bits) (`"world|enemy"`). Detection is two-sided for areas too: an area sees a body only when both masks agree, so give a player a mask such as `world|pickups|hazards`.

## Events

After each step the world holds one event list ([cw_world3d_events](api:cw_world3d_events)), valid until the next step, in a fixed order: contact begin, contact end, hit, sensor enter, sensor exit, joint break, and within each kind the library's deterministic order. No allocation per event. Contact events are on by default; hit events need `hit_events` on a shape and an approach speed above the world's `hit_speed`; a joint with a break force or torque breaks itself and reports it. For end and exit events the shape may already be gone, and then the body handles are null.

## Queries

Ray casts, shape casts (sphere, capsule, box or a convex point set swept along a translation) and overlap tests take a [cw_phys_filter](api:cw_phys_filter) (layers, sensors or not, a body to ignore, an optional callback) and write into caller arrays: no allocation. Results are sorted by fraction, then shape id, so they are identical on every host. [cw_world3d_raycast_batch](api:cw_world3d_raycast_batch) runs many rays on the job pool with the same results as one at a time; `physics3d_stack` profiles its pile with 64 of them:

@include samples/physics3d_stack/main.c#rays

Queries may run concurrently with each other, never with a step or a world change.

What a body touches after a step, with how hard, is [cw_body3d_contacts](api:cw_body3d_contacts): one entry per touching shape pair with the other body, the impulse-weighted point, the normal, Box3D's normal impulse for the step, the closing speed before the solve (a hit, against a push or a rest) and the sliding speed across. Entries are sorted by shape ids, the same on every host. Damage from collisions can be computed from it.

## Joints

One enum for both dimensions ([cw_joint_type](api:cw_joint_type)): distance, filter (no collision between two bodies), motor, prismatic, revolute, weld, wheel and mouse (drag a body toward a point) everywhere; spherical (ball and socket with cone and twist limits) and parallel (keep a body upright) in 3D. Motors, limits and springs change in place (`cw_joint3d_set_motor`, `_set_limits`, `_set_spring`). `physics2d_joints` and `joints_nodes` exercise every type.

## Characters

A character is not a rigid body. [cw_mover3d](api:cw_mover3d) is a kinematic capsule outside the simulation (Box3D's mover API) moved with move-and-slide: you set its velocity from input and gravity, [cw_mover3d_move](api:cw_mover3d_move) moves as far as the world allows, slides along walls, walks up slopes up to `max_floor_angle`, steps up ledges up to `step_height`, snaps down within `snap_length`, rides moving platforms and can push dynamic bodies as a given mass. Outputs (`on_floor`, `on_wall`, `floor_normal`, `platform_velocity`) are fields of the struct. Call it once per fixed step, always on the same side of the world step. `character3d` runs a course of stairs, ramps, a low bar, a moving platform and a too-steep slope with it. [cw_mover2d](api:cw_mover2d) is the 2D twin (`physics2d_joints` rides a kinematic platform with one). The upstream 3D mover API is marked experimental.

## Vehicles

[cartwheel/vehicle3d.h](header:vehicle3d.h) puts wheels under a 3D body. You make the chassis (a dynamic body and its shapes: they give it mass, inertia and its crashes); [cw_vehicle3d](api:cw_vehicle3d) adds up to eight raycast wheels, each a spring and damper (`hertz`, `damping_ratio`: the same feel at any scale) cast along the chassis's own down, so a car on a wall or a ceiling stands on its wheels there. Each tyre's drive, brake and sideways grip share one friction circle of `grip` times its load: inside it the tyre holds, past it the tyre slides on `slide_grip` until it slows back inside, so a car carves at low speed and drifts when pushed; on loose ground (`plough_grip`) a tyre shoved sideways past `plough_speed` digs in and grips harder at its contact, which is what trips a car over when it is T-boned on dirt; the handbrake drops the grip of its wheels and (`handbrake_locks`) locks them. Steering, drive and brake are set per axle with [cw_vehicle3d_set_axle](api:cw_vehicle3d_set_axle); [cw_vehicle3d_update](api:cw_vehicle3d_update) runs once per fixed tick before the world step and applies everything as forces and impulses on the chassis (and the reaction on a dynamic body it stands on). Contact, normal, compression, load, slip and spin are plain fields per wheel; [cw_vehicle3d_wheel_world](api:cw_vehicle3d_wheel_world) is the interpolated transform a renderer draws a wheel at. [cw_vehicle3d_teleport](api:cw_vehicle3d_teleport) places it for a respawn with no history, and [cw_vehicle3d_set_active](api:cw_vehicle3d_set_active) switches the wheels off while it tumbles. It uses only deterministic maths and sorted queries: the same bits on every host and with any number of step workers (`unit_vehicle3d` checks both).

## Physics nodes

[cartwheel/physics_nodes.h](header:physics_nodes.h) maps the wrappers to node types, in matching 2D and 3D sets:

| Node | What it is |
|---|---|
| [StaticBody3D](node:StaticBody3D) | never moves by itself; moving the node teleports it |
| [KinematicBody3D](node:KinematicBody3D) | follows its node: each tick the body gets the velocity that reaches it, so it pushes and carries |
| [RigidBody3D](node:RigidBody3D) | simulated; its transform is written into the node after every step |
| [CharacterBody3D](node:CharacterBody3D) | the mover as a node: moves only through `cw_character3d_move_and_slide` |
| [Area3D](node:Area3D) | sensor shapes; signals `body_entered`, `body_exited`, `area_entered`, `area_exited` |
| [CollisionShape3D](node:CollisionShape3D) | a shape for its parent body or area (`TRIMESH` and `CONVEX` can take a mesh node's geometry) |
| [Joint3D](node:Joint3D), [RayCast3D](node:RayCast3D), [ShapeCast3D](node:ShapeCast3D) | joints between two body nodes; casts refreshed after every step |

Layers are named in a scene by `collision_layer_name` and `collision_mask_names`, resolved against `layer_names_3d` (or `_2d`) in `project.cwproj`, where `gravity_2d`, `gravity_3d`, `pixels_per_meter` and `substeps` live too. 2D nodes work in canvas pixels with y down; the Box2D world runs in meters with y up (`pixels_per_meter`, default 32). From `physics3d_nodes`, a crate:

@include samples/physics3d_nodes/assets/crate.cwscene

Gameplay goes in node types. The walker is a `CharacterBody3D` that sets `velocity` and moves:

@include samples/physics3d_nodes/main.c#walker

A moving platform is a `KinematicBody3D` that only moves its node; the body follows and carries what stands on it:

@include samples/physics3d_nodes/main.c#oscillator

### The tick, in order

Each fixed tick of a tree with physics nodes:

1. Body nodes snapshot their transforms (the render interpolation start).
2. Nodes' `fixed_update` runs in tree order: read last step's results, set velocities, call move-and-slide (characters move now, against the current world), move kinematic nodes.
3. Deferred structural changes and signals from step 2 apply.
4. Node changes go to the bodies (new shapes, teleports, kinematic targets, joints), the 2D world steps, then the 3D world, rigid bodies write back to their nodes, casts refresh, and contact, sensor, hit and break events become signals, delivered in the library's event order. Frees and adds made by signal handlers are deferred until all are sent.
5. Camera2D smoothing.

So a script sees the signals from the step that followed its last `fixed_update`. One consequence: a character on a kinematic platform follows it with one tick of lag; it is consistent and deterministic. The worlds do not step while the tree is paused.

## Determinism and checking it

Everything that feeds the simulation (movers, queries, events, the state hash) uses only `+ - * / sqrt` and `cartwheel/math.h`, and the build sets `-ffp-contract=off`. Box2D v3 and Box3D are deterministic across worker counts: `workers` in the world def only splits the work. A step with fewer than `CW_PHYSICS_POOL_MIN_AWAKE` (128) awake bodies runs on the calling thread: fanning a small world's step out to the pool costs many times the work. Bigger worlds step on the pool, and the result is the same either way. Each world keeps a running hash of every step's moved transforms and events ([cw_world3d_stats](api:cw_world3d_stats)), which games publish as a debug field so the gates can compare hosts. `physics3d_stack` after 240 ticks:

```sh
node tools/run-cart.mjs --host native --app physics3d_stack --frames 240 --state out/p3.json
```

```text
"phys.hash":63552413,"phys.steps":240,"phys.bodies":95,"phys.awake":88,"phys.contacts":361,"ev.begin":608,"ev.end":342,"ev.hit":82,...,"ray.hits":38
```

The same command with `--host romdev` must print the same hash. `physics_determinism` goes further: it runs upstream's own determinism scenes with 1 to 5 workers and checks upstream's published hashes on every host (`physics-determinism` gate), and `physics-scaling` holds both halves of the sizing: the large pyramids get faster with workers, the small world does not get slower.

## Debug draw and stats

[cw_world3d_debug_draw](api:cw_world3d_debug_draw) appends shapes, joints, bounds, contacts or mass as a flat line list ([cw_phys_lines](api:cw_phys_lines)) any renderer can draw as `GL_LINES`; clearing keeps the capacity, so it does not allocate once warm. The `phys` debug command reports the tree's worlds over the debug channel.
