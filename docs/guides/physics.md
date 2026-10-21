# Physics

{{name}} simulates 2D physics with Box2D v3.2 and 3D physics with Box3D, through thin wrappers whose 2D and 3D APIs mirror each other name for name, and a layer of physics node types for the scene tree. Both libraries step on the engine's job pool and are deterministic across worker counts and hosts.

Headers: [cartwheel/physics.h](header:physics.h) (what both dimensions share: layers, events, filters, stats, debug lines, joint types), [cartwheel/physics2d.h](header:physics2d.h), [cartwheel/physics3d.h](header:physics3d.h), [cartwheel/physics_nodes.h](header:physics_nodes.h) (the node types) and [cartwheel/physics_tasks.h](header:physics_tasks.h) (the job-pool glue, for code that creates raw library worlds). Samples: `physics3d_stack` and `physics2d_joints` (the wrapper API), `character3d` (the 3D mover), `physics3d_nodes`, `joints_nodes` and `games/action2d` (the node types), `physics_determinism` (determinism), `cloth_demo` (cloth and ropes).

## Two ways in

- **Physics nodes.** Put a [RigidBody3D](node:RigidBody3D) with a [CollisionShape3D](node:CollisionShape3D) child in a scene and it falls. The tree owns one 2D and one 3D world, created when the first physics node enters, and steps them in the right place of the tick. This is the normal way to build a game.
- **The wrapper API.** Create a world, bodies, shapes and joints in C and step it yourself. Use it for apps that do not run a scene tree, for physics that is not part of the scene (a minigame, a simulation tool), or for full control. Every handle has a `_raw()` accessor returning the Box2D or Box3D id for anything the wrapper does not cover.

The node layer is built on the wrapper, so everything below about layers, events and determinism applies to both.

## Worlds, bodies, shapes

A world is created from a def ([cw_world3d_default_def](api:cw_world3d_default_def): gravity (0, -10, 0), 4 substeps, sleeping and continuous collision on) that can also name its 32 collision layers:

@include samples/physics3d_stack/main.c#world_def

Bodies are `STATIC`, `KINEMATIC` or `DYNAMIC` ([cw_body3d_create](api:cw_body3d_create)); shapes attach to bodies with a material (friction, restitution, rolling resistance, conveyor speed, and a `user_material` id reported in hits). 3D shapes: box, sphere, capsule, convex hull, static triangle mesh and static height field ([cw_shape3d_heightfield](api:cw_shape3d_heightfield): uint16 steps stored exactly, holes, regions that line up; [terrain](guide:terrain) builds on it); 2D: box, circle, capsule, polygon, segment and chains (one-sided terrain). Units are meters, kilograms, seconds and radians. Handles are the libraries' own generational ids, so a destroyed body's handle reads as invalid, never as another body. The world's and bodies' user-data slots belong to the wrapper; use `cw_body3d_set_user` and the shape def's `user`.

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

## Cloth and ropes

[cartwheel/cloth.h](header:cloth.h) adds cloth and ropes beside a 3D world: flags, banners, capes, curtains, hanging ropes, chains and towing lines. They are particles solved with extended position-based dynamics (XPBD): each mesh edge is a distance constraint, each pair of triangles sharing an edge gets a softer bending constraint across it, and long-range tethers keep every particle within its rest distance (along the mesh) of the nearest pinned particle, which is what stops a long cape or rope from stretching under its own weight. A step runs `substeps` small steps (8 for cloth, 16 for ropes), each predicting, solving every constraint once, colliding and updating velocities.

[cw_cloth_create_grid](api:cw_cloth_create_grid) makes a rectangle, [cw_cloth_create](api:cw_cloth_create) any triangle mesh, [cw_rope_create](api:cw_rope_create) a chain of particles (a rope is a cloth without triangles: every `cw_cloth_*` function works on it). Step it right after the world, every fixed tick:

```text
cw_world3d_step(w, dt);
cw_cloth_step(cloth, dt);
```

A scene with several cloths and ropes steps them together with [cw_cloth_step_many](api:cw_cloth_step_many): the same bits as stepping each in turn, but the job pool takes one cloth per job instead of each cloth fanning out on its own. The `Cloth3D` and `Rope3D` nodes of a tree always do this.

- **Pins.** A vertex with inverse mass 0 is pinned ([cw_cloth_pin](api:cw_cloth_pin)); [cw_cloth_set_pin_target](api:cw_cloth_set_pin_target) moves it, blended across the next step's substeps, so a flag on a moving pole or a cape on a running character follows smoothly.
- **Bodies.** [cw_cloth_attach_body](api:cw_cloth_attach_body) ties a vertex to a point on a body. A dynamic body feels the pull: after the step the cloth applies one impulse that brings the body back toward where the rope wants it and, while the rope is taut, takes out the body's speed away from it. A 20 kg crate hangs from a rope with a pull equal to its weight ([cw_cloth_attachment_force](api:cw_cloth_attachment_force)) and swings when shoved; a rope between two bodies tows one behind the other at the rope's length.
- **Skin.** With `max_distance` in the def, [cw_cloth_set_skin_targets](api:cw_cloth_set_skin_targets) gives each vertex a target (the rest shape moved by a bone or node each tick) it may not stray farther from: a cape stays near the back it is worn on.
- **Wind.** [cw_cloth_set_wind](api:cw_cloth_set_wind): drag along the air's flow and lift across it, by each particle's share of the surface and how squarely it meets the flow, with optional turbulence that varies it smoothly over the cloth and over time.
- **Collision.** Particles keep `thickness` from Box3D spheres, capsules, boxes and convex hulls (static, kinematic and dynamic) on the layers in `collide_mask`, with friction. The shapes near the cloth are found once per step, and each body is blended from its pose before the world step to its pose after it across the substeps, so a ball at 12 m/s carries a curtain instead of passing through it. Collision is one-way unless `two_way` is set. Triangle meshes and height fields are not collided with yet, and only particles collide: a thin shape can slip between two particles of a coarse cloth or rope. `self_collision` keeps particles `thickness` apart (it costs about as much again).
- **Rigid chains.** [cw_chain3d_create](api:cw_chain3d_create) builds a chain of Box3D capsule links on spherical joints instead: heavy chains and wrecking balls that are ordinary bodies.

Reading: [cw_cloth_positions](api:cw_cloth_positions), [cw_cloth_stats_get](api:cw_cloth_stats_get) (stretch, energy, constraint and colour counts, step time). Drawing: [cw_cloth_geometry_new](api:cw_cloth_geometry_new) (or [cw_rope_geometry_new](api:cw_rope_geometry_new) for a tube) gives a three.c geometry, and [cw_cloth_update_geometry](api:cw_cloth_update_geometry) writes positions blended by the render alpha and rebuilds the normals each frame; it draws with any material on GL and WebGPU.

### Cloth3D and Rope3D

The node types do all of the above in a scene. [Cloth3D](node:Cloth3D) hangs a grid from its node (down its -y, along its +x), pinned along an edge or at corners, following `attach_to` (a body node is attached to, any other node moves the pins, and `bone` follows a joint of that node's model). [Rope3D](node:Rope3D) runs from its node to `end`, as particles or rigid links, each end hanging from a body node, following the node or free. They step right after the tree's 3D world. From `cloth_demo`, a flag on a pole, a hero whose cape follows and drapes over its capsule, a rope holding a crate and a chain holding a weight:

@include samples/cloth_demo/assets/main.cwscene#Flag

@include samples/cloth_demo/assets/main.cwscene#Cape

@include samples/cloth_demo/assets/main.cwscene#Rope..Crate

### Cost

Microseconds per fixed tick at 8 substeps, wind on, measured natively on the reference machine (Ryzen AI 9 HX 370, the 890M's host, 24 threads) at a load average of about 10, so an idle machine is a little faster. `unit-cloth` prints them every run (`cloth cost`). Each is the median of three runs, each run the best of three blocks of 60 ticks.

| Cloths | Particles each | One thread | Pool, each stepped alone | Pool, `cw_cloth_step_many` |
|---|---|---|---|---|
| 1 | 16 x 16 (256) | 68 | 68 | 68 |
| 8 | 16 x 16 | 540 | 540 | 190 |
| 1 | 32 x 32 (1024) | 290 | 290 | 300 |
| 8 | 32 x 32 | 2400 | 2480 | 760 |
| 1 | 64 x 64 (4096) | 1220 | 540 | 560 |
| 8 | 64 x 64 | 9700 | 4600 | 2700 |

What this says:

- One cloth costs about 280 microseconds per 1000 particles at 8 substeps on one thread, and the cost scales with substeps and particles. Under 1024 particles the pool is not used at all; a single cloth gains from it only from 64 x 64 up (1220 to 540).
- Stepping eight cloths one at a time on the pool is no faster than one thread, because each is too small to split. Giving the pool one cloth per job brings eight 32 x 32 flags from 2400 to 760 microseconds, about 2.5 times the cost of one. That is the pool's wake-up cost, not the solver's.
- **What a game should budget:** a scene with a flag, a cape and a few banners (eight cloths of 16 x 16 to 32 x 32) takes 0.2 to 0.8 ms of a 16.7 ms tick on this machine, spread over the pool. A single 64 x 64 curtain takes about 0.55 ms. Size cosmetic cloth to what it shows: a 16 x 16 cape is plenty on a character seen from a few metres. Halve the substeps (4) for background cloth; mark it `quality_scaled` and the Low preset does it for you.
- On a Vega 8 machine (4 cores, each about 1.5 to 2 times slower per core), expect the one-thread column times 1.5 to 2, divided by about 3 for a batch of eight: roughly 0.4 ms for eight 16 x 16 cloths and 1.5 ms for eight 32 x 32. Budget two or three flags and a cape there, or lower the substeps.
- Self-collision roughly doubles a cloth's per-particle pass. Collision costs per particle for each shape the cloth's bounds reach, and a particle outside a shape's bounding sphere skips it. The nodes' `cloth.us` readings in `cloth_demo` (flag 425 particles, cape 209, rope 21) are the cloths' own work: 0.3 to 0.4 ms per tick in total, natively and in carts.

### Determinism and rollback

Particles are cut into blocks of consecutive indices at creation. Each substep solves the constraints inside the pairs of blocks {0, 1}, {2, 3}, ... as one job each, then those across the other boundaries: the jobs of one phase share no particle, so they run in parallel on the job pool with no atomics, each a fixed-order sweep, and the result does not depend on the number of threads or the order jobs run in. A cloth under 1024 particles runs on the calling thread. Only `+ - * / sqrt` and `cartwheel/math.h` feed the state, so the same inputs give the same bits natively and in a cart ([cw_cloth_hash](api:cw_cloth_hash)); `unit-cloth` pins its checksum and `cloth-determinism` runs it at several pool sizes. [cw_cloth_save](api:cw_cloth_save) and [cw_cloth_load](api:cw_cloth_load) copy the state for rollback.

A cloth marked `quality_scaled` is cosmetic: on the Low graphics preset it runs half the substeps and never self-collides. Leave it off for anything that touches game state; a deterministic run always simulates as High.

## What Box2D v3.2 gives the 2D wrapper

The engine builds Box2D v3.2 and exposes all of it that belongs to a world, a body, a shape or a joint. The plain geometry functions (`b2CollideCircles`, `b2ComputeShapeDistance` and the like) work on data and need no wrapper: call them directly, the headers are on the include path.

**Large worlds.** Box2D keeps world positions as doubles (about 1000 km of world instead of 16 km) and does the solver math in floats. The wrapper follows: [cw_pos2](api:cw_pos2) is a double position, and body positions ([cw_body2d_world_transform](api:cw_body2d_world_transform)), ray and hit points, query origins, joint anchors and the character mover's feet all use it. A query shape carries an origin ([cw_qshape2d_at](api:cw_qshape2d_at)), so its points stay precise relative to it. The float calls (`cw_body2d_transform`, `cw_body2d_set_transform`) still work and round a far position to floats. A mover works in floats relative to where its feet started, so it is as exact 500 km out as at the origin. Scene nodes are still floats in pixels, so a node far from the origin has a float position even though its body does not: use the wrapper API for a world that big. The determinism gate checks upstream's double-precision hash on every host.

**Snapshots and rollback.** [cw_world2d_snapshot](api:cw_world2d_snapshot) and [cw_world2d_restore](api:cw_world2d_restore) save and put back the whole simulation, including the contact warm-start caches and sleeping islands, which is what a deterministic rollback needs. A tree snapshot (`cartwheel/net.h`) carries the 2D world this way, so a pile of `RigidBody2D` and the `Area2D` overlaps re-simulate bit for bit after a rollback; `unit-net` proves it and runs the same game without the image as the control that must fail. A snapshot holds your user pointers, so it is process-local: do not send it to another machine. [cw_world2d_state_hash](api:cw_world2d_state_hash) is Box2D's own host-stable hash of the state.

**Recording and replay.** [cw_world2d_record_start](api:cw_world2d_record_start) writes the world as it is then and every call that changes it, with the state hash of every step. [cw_recording2d_save](api:cw_recording2d_save) writes the file and [cw_replay2d_open](api:cw_replay2d_open) plays it back: step, seek back and forth, wireframes with [cw_replay2d_debug_draw](api:cw_replay2d_debug_draw), and `cw_replay2d_diverged` when this engine simulates it differently. The replay also shows what the game asked each step: the ray casts, shape casts, overlaps and mover casts it made, with their hits ([cw_replay2d_query_count](api:cw_replay2d_query_count), `cw_replay2d_get_query`, `cw_replay2d_get_hit`, and `cw_replay2d_debug_draw_queries` for wireframes). Seeking backwards re-runs from the nearest keyframe; [cw_replay2d_set_keyframes](api:cw_replay2d_set_keyframes) sets their memory budget and minimum spacing, and `cw_replay2d_keyframe_bytes`, `_budget` and `_interval` report them. Attach a recording to a bug report.

**The dynamic character mover and the pogo.** `CW_JOINT_MOVER` commands a velocity on a dynamic body (x and y separately, each limited by a force) and `CW_JOINT_POGO` is a spring that holds a dynamic character off the ground, limited in how hard it pulls and pushes. Both are 2D only. The `Joint2D` node has them as `MOVER` and `POGO` (`mover_velocity`, `mover_max_force`, `pogo_normal`, `pogo_tension`, `pogo_compression`).

**New controls and queries.** Per world: [cw_world2d_set_max_linear_speed](api:cw_world2d_set_max_linear_speed), the contact recycle distance (contact points are reused while the shapes barely move), contact tuning, restitution iterations and propagation, workers, capacity, bounds, and a switch to the 4-wide solver for testing. Per body: sleep, contact recycling and continuous-collision safety factor toggles, extents, bounds, centre of mass and a mass update. Per shape: bounds, a point test, the closest point, a ray cast at one shape, rounded boxes, loose chain segments ([cw_shape2d_chain_segment](api:cw_shape2d_chain_segment)) and a debug colour. Chains take a material per segment or one for all. Joints report their rest length, speed, motor force, spring force and separation.

The 8-wide contact solver (for CPUs with the AVX2 vector instructions) is picked at run time and gives the same bits as the 4-wide one. The cart build runs the 4-wide solver with WebAssembly SIMD, bit for bit the same as native.

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

[cw_world3d_debug_draw](api:cw_world3d_debug_draw) appends shapes, joints, bounds, contacts or mass as a flat line list ([cw_phys_lines](api:cw_phys_lines)) any renderer can draw as `GL_LINES`; clearing keeps the capacity, so it does not allocate once warm. [cw_world2d_debug_draw](api:cw_world2d_debug_draw) does the same for a 2D world and takes the extra 2D-only flags Box2D v3.2 draws (3D ignores them): `CW_PHYS_DRAW_JOINT_EXTRAS` (limits and springs), `CW_PHYS_DRAW_ANCHOR_A`, `CW_PHYS_DRAW_CHAIN_NORMALS` (which side of a chain is solid), `CW_PHYS_DRAW_CONTACT_FORCES`, `CW_PHYS_DRAW_FRICTION_FORCES`, `CW_PHYS_DRAW_ISLANDS` (the groups of touching bodies that sleep together) and `CW_PHYS_DRAW_GRAPH_COLORS` (contacts tinted by the solver colour that holds them). The `phys` debug command reports the tree's worlds over the debug channel.

A 2D world reports more than [cw_world2d_stats](api:cw_world2d_stats): [cw_world2d_profile_get](api:cw_world2d_profile_get) splits the last step's milliseconds by phase (pairs, collide, solve and the solver's stages, sleep, sensors), [cw_world2d_counters_get](api:cw_world2d_counters_get) gives Box2D's counters (bodies, shapes, contacts awake and recycled, islands, tree heights, tasks, constraints per solver colour), and [cw_physics2d_memory_bytes](api:cw_physics2d_memory_bytes) the bytes Box2D holds across all worlds. Box2D's own log lines arrive as engine warnings prefixed `Box2D:`, and a failed Box2D assertion is logged as an engine error before Box2D halts on it.
