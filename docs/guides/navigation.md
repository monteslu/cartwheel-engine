# Navigation

{{name}} finds paths over navigation meshes and moves crowds of agents along them. Navmeshes are built by Recast and queried with Detour (both zlib, vendored with small patches); obstacles carve them at run time through DetourTileCache. The crowd is the engine's own C, with ORCA avoidance ported from the RVO2 library. Builds run on the job pool, crowds step on it too, and the results are bit-identical on every host and at every worker count.

Header: [cartwheel/nav.h](header:nav.h). Samples: `nav_demo` (the node types: 500 agents and a gate that closes a doorway), `nav_links` (jumps up a ledge, a jump pad and a drop) and `nav_perf` (the C API, timed). Tests: `unit_nav`, and the gates described under Determinism below.

## Two ways in

- **Navigation nodes.** Put a [NavigationRegion3D](node:NavigationRegion3D) over the level's meshes and static colliders, and give each character a [NavigationAgent3D](node:NavigationAgent3D) child. The region bakes on its first tick, the agents walk their parents to their `target_position`, and [NavigationObstacle3D](node:NavigationObstacle3D) nodes carve a dynamic region while they are in the tree.
- **The C API.** Fill a [cw_nav_geom](api:cw_nav_geom), build a navmesh with [cw_navmesh_build](api:cw_navmesh_build), query it, and step a crowd ([cw_crowd_new](api:cw_crowd_new)) from `fixed_update`. The nodes are built on it.

## Building a navmesh

The input is a triangle soup in world space with an area id per triangle: area 0 is not walkable, 1 to 63 are kinds of ground, and [CW_NAV_AREA_WALK](api:CW_NAV_AREA_WALK) is the default. Fill it from arrays ([cw_nav_geom_add_tris](api:cw_nav_geom_add_tris), with a transform), boxes, three.c object trees ([cw_nav_geom_add_object](api:cw_nav_geom_add_object)), or a physics world's static shapes ([cw_nav_geom_add_world3d](api:cw_nav_geom_add_world3d): hulls and boxes by their faces, meshes and height fields by their triangles, spheres and capsules as prisms). Sources are appended in a fixed order (array order, tree order, body creation order), so a scene gives the same soup every time.

@include samples/nav_perf/main.c#build

[cw_nav_config_default](api:cw_nav_config_default) describes an agent 2 m tall with a 0.6 m radius that climbs 0.9 m steps and 45 degree slopes, on 0.3 m voxels. The navmesh keeps `agent_radius` away from walls, so an agent's center can go anywhere on it. The world is cut into square tiles of `tile_size` voxels (48 by default; 0 makes one tile of the whole bounds). [cw_navmesh_build](api:cw_navmesh_build) bins each triangle to the tiles it touches, builds every tile on the job pool (each with its own heightfields), then adds them to the navmesh in tile order on the calling thread. Polygon refs therefore depend only on the input, never on which thread built which tile.

- [cw_navmesh_rebuild_area](api:cw_navmesh_rebuild_area) rebuilds the tiles over a box from new geometry: for a static navmesh whose level changed in one place.
- [cw_navmesh_save](api:cw_navmesh_save) writes the navmesh as bytes and [cw_navmesh_load](api:cw_navmesh_load) reads them back. A dynamic navmesh saves its tile cache and its obstacles. Equal navmeshes save equal bytes on every host, so a game can bake offline and ship the file.
- [cw_navmesh_hash](api:cw_navmesh_hash) hashes every tile's bytes: the determinism check.

The partition (`CW_NAV_PARTITION_WATERSHED`, `MONOTONE` or `LAYERS`) trades build time against polygon quality, as in Recast. A dynamic navmesh (`dynamic = true`) is built through the tile cache in layers of at most 255 voxels a side, so its tiles stay small, and it has no height detail mesh.

## Queries

Points are snapped to the navmesh within the query extents (half sizes 2, 4, 2 by default; [cw_navmesh_set_query_extents](api:cw_navmesh_set_query_extents)). A filter excludes areas and weighs their cost; NULL allows everything at cost 1.

- [cw_nav_find_path](api:cw_nav_find_path): straight-line waypoints from a to b (b's closest reachable point, flagged partial, when b is cut off).
- [cw_nav_raycast](api:cw_nav_raycast): walks the surface toward a point and reports the wall that stops it.
- [cw_nav_nearest](api:cw_nav_nearest) and [cw_nav_height](api:cw_nav_height).
- [cw_nav_random_point](api:cw_nav_random_point) and [cw_nav_random_point_near](api:cw_nav_random_point_near) draw from a [cw_rng](api:cw_rng), so the same seed gives the same points.

Every pool thread has its own Detour query object, so queries may run from range jobs while nothing changes the navmesh. [cw_nav_find_paths](api:cw_nav_find_paths) runs a batch on the pool, with the same answers as one at a time:

@include samples/nav_perf/main.c#paths

### The per-tick budget

Games that ask for many paths at once spread them over ticks with [cw_nav_request_path](api:cw_nav_request_path) and [cw_nav_tick](api:cw_nav_tick). A tick answers at most `max_requests` of the oldest requests, each searching at most `max_iters` A* iterations (a search that runs out returns the best partial path). The requests a tick answers depend only on the order they were made and the budget, never on time or thread count, and the answers run in parallel. [cw_nav_take_path](api:cw_nav_take_path) collects one.

## Obstacles

On a dynamic navmesh, [cw_nav_obstacle_add_cylinder](api:cw_nav_obstacle_add_cylinder) and [cw_nav_obstacle_add_box](api:cw_nav_obstacle_add_box) (an axis box, or a box turned about y) queue a change, and so does [cw_nav_obstacle_remove](api:cw_nav_obstacle_remove). [cw_nav_update](api:cw_nav_update) applies the queue. The queue holds 64 changes; when it is full, the next add or remove applies it first, so a level can add hundreds of obstacles (or a game destroy dozens of crates in one tick) without calling update in between. The touched tiles rebuild in parallel and are committed in ascending tile order, which is why Detour's tile cache is patched (below). Crowds notice the change and replan agents whose paths crossed a polygon that is gone.

## Links: jumps, drops and pads

Walking only reaches what the navmesh joins: a ledge higher than `agent_max_climb`, a roof reached by a jump pad, or the floor below a balcony are separate pieces. A link ([cw_nav_link](api:cw_nav_link), Detour's off-mesh connection) joins two points that walking cannot. Paths cross it, and crowd agents traverse it. Add links to the build input with [cw_nav_geom_add_link](api:cw_nav_geom_add_link):

- `start` and `end` lie on the walkable surface (within `radius` across, default the navmesh's `agent_radius`, and `agent_max_climb` up or down). The navmesh keeps `agent_radius` back from walls, so put a ledge jump's ends a little out from the wall and a little in from the top's edge.
- `bidirectional`: a ledge jump is two-way; a jump pad or a drop is one-way (start to end).
- `tag`: what kind of link it is, for the game (`CW_NAV_LINK_JUMP`, `DROP`, `PAD`, `LADDER`, `TELEPORT`, or any value from `CW_NAV_LINK_USER` up).
- `cost`: what crossing costs, in metres of walking (0: its length). A pad that saves a long walk can cost less than its length; a risky drop can cost more. Paths multiply it by the filter's cost for the link's `area`, and a filter that excludes that area keeps paths off the link (a heavy unit that cannot jump, say).
- `duration` and `arc`: how a crowd agent crosses (seconds, default its length over the agent's speed; the arc's height at the middle).

A zero field takes its default, so a designated initializer is enough. A link's id is its order in the geometry (1, 2, 3...). Links are part of the build input, so they are in the navmesh's bytes, its hash and its saves. An end that misses the navmesh makes the link useless: the build names it in a warning, [cw_nav_link_connected](api:cw_nav_link_connected) says false, and the debug draw paints it red.

[cw_nav_find_path_links](api:cw_nav_find_path_links) and [cw_nav_take_path_links](api:cw_nav_take_path_links) return the waypoints and, for each, the id of the link it starts (0 for walking). A game that moves its own characters (a player-like bot with a character controller) follows the waypoints and, at a waypoint with a link id, does what the link's tag says: jumps toward the next waypoint, waits for the pad to throw it, or steps off.

A crowd agent whose next corner starts a link begins crossing it within 2.25 radii of the corner: its status becomes `CW_CROWD_ON_LINK` and it moves from where it stands to the link's end along the arc, then walks on. [cw_crowd_set_link_callback](api:cw_crowd_set_link_callback) is called at the end of the step, in agent order, for each crossing that began or landed, with the link's id, tag, the start, the end and the duration: the place to start the jump animation or the pad's effect. With `manual_links` set in the agent's params, the agent stops at the link's start and waits until the game has moved its body across and calls [cw_crowd_finish_link](api:cw_crowd_finish_link). A target set during a crossing is planned after landing, and a teleport ends the crossing.

In a scene, a [NavigationLink3D](node:NavigationLink3D) under a [NavigationRegion3D](node:NavigationRegion3D) is baked with it: `start_position` and `end_position` are local to the node, and the other properties are the link's fields. Agents emit `link_entered` (the link's id, its tag and where it lands) and `link_exited`, and their parent is carried along the arc. `nav_links` declares its three links in its scene file:

@include samples/nav_links/assets/main.cwscene#Pad

@include samples/nav_links/main.c#signals

Links are cheap: each is one extra polygon in its tile and a few bytes in the save. A path search treats a link like any other polygon edge. A link cheaper than its length lets the search's distance estimate overestimate, so the path it picks is still valid but may not be the shortest one.

## Crowds

A crowd ([cw_crowd_new](api:cw_crowd_new)) holds up to `max_agents` agents on one navmesh. An agent has a radius, height, speed, acceleration, a neighbour radius and count, a time horizon and an arrival distance ([cw_crowd_agent_params](api:cw_crowd_agent_params)). Give it a target and step the crowd every fixed tick:

@include samples/nav_perf/main.c#crowd

An agent's handle (`cw_crowd_agent`) is 64 bits: the slot in the low half and a generation in the high half, so a removed agent's handle stops being valid when its slot is reused. Keep the handle itself in the game's own data. A handle cut to 32 bits (kept in a `uint32_t` or an `int`) can never be valid; every `cw_crowd_*` call that receives one logs an error naming the cause and the fix, instead of quietly answering with a zero position, and [cw_handle_truncated_count](api:cw_handle_truncated_count) counts them for a test. For a small number to index the game's own per-agent array, use [cw_crowd_agent_slot](api:cw_crowd_agent_slot) (0 to `max_agents - 1`; -1 for an invalid handle). The same check runs in every pool lookup, so a cut node or resource handle is reported too.

[cw_crowd_step](api:cw_crowd_step) runs four phases, each a range job over the agents, and each agent's work reads only the state the step started with and writes only its own slot:

1. **Plan.** The oldest agents waiting for a path (in the order their targets were set; 8 per step by default, [cw_crowd_set_plan_budget](api:cw_crowd_set_plan_budget)) get a corridor, the polygon list to their target.
2. **Neighbours.** Positions and velocities are copied into flat arrays and the agents counting-sorted into a grid. Each agent then keeps its nearest neighbours by distance, then index. The distance tests run four agents at a time with SIMD.
3. **Steering and avoidance.** The corridor gives the next corners of the path (shortcut when the corner after next is in sight) and so the desired velocity, slowing near the target. ORCA turns it into the velocity closest to it that avoids every neighbour, assuming they avoid back (each takes a share set by `avoidance_priority`). Its constraint lines are built four neighbours at a time with SIMD. Acceleration is limited last.
4. **Movement.** Each agent slides along the navmesh surface, and its corridor follows.

[cw_crowd_set_velocity](api:cw_crowd_set_velocity) drives an agent by velocity instead of by path (it still avoids others). [cw_crowd_set_position](api:cw_crowd_set_position) moves an agent whose body other code moves, such as a character controller following the computed velocity. [cw_crowd_get_state](api:cw_crowd_get_state) reports position, velocity, the desired velocity, the next corner, status and a path version that changes with every new path.

ORCA keeps agents apart and lets lanes form in open space and in moderate flows. Like every velocity-obstacle method, it can still jam when two dense opposing streams meet in a passage a few agents wide: each agent avoids its neighbours, but nothing plans who goes first. Give busy routes room. `nav_demo` uses 8 m doorways for 500 agents.

## Navigation nodes

A [NavigationRegion3D](node:NavigationRegion3D) bakes from what is under it: [MeshInstance3D](node:MeshInstance3D) meshes (primitives, and loaded mesh resources) and, when the tree has a physics world, the static colliders of bodies under it (`source` picks one or both, `collision_mask` the layers). Subtrees that move are skipped: rigid, character and kinematic bodies, obstacles, and any node with a NavigationAgent3D child. It bakes on its first fixed tick (after the physics world has built that tick's bodies), or loads `navmesh_path` instead (bytes from [cw_navmesh_save](api:cw_navmesh_save)). The `bake` method, or [cw_nav_region_bake](api:cw_nav_region_bake), bakes again. `debug_draw` overlays the navmesh, agents and paths.

@include samples/nav_demo/assets/main.cwscene#Nav

A [NavigationAgent3D](node:NavigationAgent3D) joins its region's crowd at its parent's position. Setting `target_position` plans a path, and every tick it moves the parent Node3D (the parent's transform is snapshotted first, so rendering interpolates). It emits `path_changed`, `target_reached` once per target, and `velocity_computed` while it moves. With `drive_parent` off it moves nothing itself: the game moves the parent (for example a [CharacterBody3D](node:CharacterBody3D) from `velocity_computed`) and the agent follows it. `nav_demo` spawns its agents from C and sends each one on when it arrives:

@include samples/nav_demo/main.c#agent

@include samples/nav_demo/main.c#reached

A [NavigationObstacle3D](node:NavigationObstacle3D) is a cylinder (standing on its origin) or a box (centered, turned with the node about y) carved out of a dynamic region while it is in the tree. Moving the node moves the hole on the next tick.

One fixed-tick hook per tree, at order -50, runs all of this. It bakes, applies obstacle changes, adds new agents and targets, steps every region's crowd and moves the agents' parents, in the order the nodes entered the tree.

## Debug draw

[cw_nav_debug_draw](api:cw_nav_debug_draw), [cw_crowd_debug_draw](api:cw_crowd_debug_draw) and [cw_nav_debug_draw_path](api:cw_nav_debug_draw_path) append line segments to a [cw_phys_lines](api:cw_phys_lines), the same list physics debug drawing uses. Polygon edges are dim and walls bright, obstacles are outlined, links are arcs from start to end (green when paths can use them, red when an end missed the navmesh, with barbs at the end of a one-way link), agents are circles with their velocity, and paths are polylines through their corners. Regions draw it themselves with `debug_draw`.

## Determinism

Navigation results are part of the simulation, so they follow the engine's rule: the same on every host and at every worker count.

- **Recast's sorting.** `qsort` is not stable, and glibc, wasi-libc, MSVC and Bionic order equal keys differently. The comparators for contour holes, hole diagonals and BV-tree items break ties by index (`patches/recastnavigation/0001`).
- **Trigonometry.** The walkable-slope cosine and the box-obstacle rotation go through math hooks that the engine fills with `cw_cos` and `cw_sin` (`0002`).
- **Memory.** Recast and Detour allocate through `cw_alloc` (tag `CW_MEM_NAV`, thread-safe), compiled in through a hook rather than a function pointer set at run time. Memory comes zeroed, so no uninitialized byte reaches tile data that is hashed or saved (`0003`).
- **Tile-cache rebuilds.** `dtTileCache::update` rebuilt one tile per call. It is split into a pure build that any thread may run and a commit that runs in tile order (`0004`).
- **The crowd.** Each phase reads the step's starting state and writes only its own agent. Link crossings start and end in the movement phase, and their callbacks run afterwards on the calling thread in agent order. Neighbours are chosen by a total order. Planning order is request order. The SIMD paths use only add, subtract, multiply, divide, square root, compares and bitwise selects, so SSE2, wasm SIMD128, NEON and plain C give the same bits.

The `unit-nav`, `nav-demo` and `nav-links` gates pin their results (the suite's checksum, and the samples' crowd hashes folded every tick). `nav-links` also runs on the WebGPU axis, so its crowd is compared in Chromium, romdev and wcnative too. `node tests/nav-determinism.mjs` runs all three on native, Chromium and wcnative at `CW_WORKERS` 0, 1, 3 and the default. A cart has no environment: Chromium's carts get the pool size as `navigator.hardwareConcurrency`, and the GPU sandbox shows wcnative the matching number of online CPUs. Natively, `unit_nav` also rebuilds and reruns at four pool sizes in one process.

## Performance

`node tests/nav-perf.mjs` runs `nav_perf` natively and in Chromium, on one thread and on the default pool. It requires equal result hashes everywhere, budgets, and a parallel build at least twice as fast as one thread. On a Ryzen AI 9 HX 370 (24 threads, Linux):

| | native, 1 thread | native, 24 threads | Chromium, 1 thread | Chromium, 24 threads |
|---|---|---|---|---|
| build, 256 x 256 m world (324 tiles, 2666 polygons) | 193 ms | 26 ms | 257 ms | 81 ms |
| the same, dynamic (tile cache) | 140 ms | 19 ms | 163 ms | 24 ms |
| 64 obstacles applied | 3.7 ms | 1.0 ms | 5.0 ms | 1.4 ms |
| 1000 batched paths | 47 ms | 4.0 ms | 50 ms | 12 ms |
| crowd step, 500 agents (median) | 0.93 ms | 0.24 ms | 0.95 ms | 0.32 ms |
| crowd step, 2000 agents (median) | 3.8 ms | 0.58 ms | 5.5 ms | 0.89 ms |

Tile parallelism is what makes builds fast. In the crowd, the expensive work per agent is the Detour corner and corridor queries, and they spread over the pool.
