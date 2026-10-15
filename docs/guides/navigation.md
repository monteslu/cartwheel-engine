# Navigation

{{name}} finds paths over navigation meshes and moves crowds of agents along them. Navmeshes are built by Recast and queried with Detour (both zlib, vendored with small patches); obstacles carve them at run time through DetourTileCache. The crowd is the engine's own C, with ORCA avoidance ported from the RVO2 library. Builds run on the job pool, crowds step on it too, and the results are bit-identical on every host and at every worker count.

Header: [cartwheel/nav.h](header:nav.h). Samples: `nav_demo` (the node types: 500 agents and a gate that closes a doorway) and `nav_perf` (the C API, timed). Tests: `unit_nav`, and the gates described under Determinism below.

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

On a dynamic navmesh, [cw_nav_obstacle_add_cylinder](api:cw_nav_obstacle_add_cylinder) and [cw_nav_obstacle_add_box](api:cw_nav_obstacle_add_box) (an axis box, or a box turned about y) queue a change, and so does [cw_nav_obstacle_remove](api:cw_nav_obstacle_remove). [cw_nav_update](api:cw_nav_update) applies the queue. The touched tiles rebuild in parallel and are committed in ascending tile order, which is why Detour's tile cache is patched (below). Crowds notice the change and replan agents whose paths crossed a polygon that is gone.

## Crowds

A crowd ([cw_crowd_new](api:cw_crowd_new)) holds up to `max_agents` agents on one navmesh. An agent has a radius, height, speed, acceleration, a neighbour radius and count, a time horizon and an arrival distance ([cw_crowd_agent_params](api:cw_crowd_agent_params)). Give it a target and step the crowd every fixed tick:

@include samples/nav_perf/main.c#crowd

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

[cw_nav_debug_draw](api:cw_nav_debug_draw), [cw_crowd_debug_draw](api:cw_crowd_debug_draw) and [cw_nav_debug_draw_path](api:cw_nav_debug_draw_path) append line segments to a [cw_phys_lines](api:cw_phys_lines), the same list physics debug drawing uses. Polygon edges are dim and walls bright, obstacles are outlined, agents are circles with their velocity, and paths are polylines through their corners. Regions draw it themselves with `debug_draw`.

## Determinism

Navigation results are part of the simulation, so they follow the engine's rule: the same on every host and at every worker count.

- **Recast's sorting.** `qsort` is not stable, and glibc, wasi-libc, MSVC and Bionic order equal keys differently. The comparators for contour holes, hole diagonals and BV-tree items break ties by index (`patches/recastnavigation/0001`).
- **Trigonometry.** The walkable-slope cosine and the box-obstacle rotation go through math hooks that the engine fills with `cw_cos` and `cw_sin` (`0002`).
- **Memory.** Recast and Detour allocate through `cw_alloc` (tag `CW_MEM_NAV`, thread-safe), compiled in through a hook rather than a function pointer set at run time. Memory comes zeroed, so no uninitialized byte reaches tile data that is hashed or saved (`0003`).
- **Tile-cache rebuilds.** `dtTileCache::update` rebuilt one tile per call. It is split into a pure build that any thread may run and a commit that runs in tile order (`0004`).
- **The crowd.** Each phase reads the step's starting state and writes only its own agent. Neighbours are chosen by a total order. Planning order is request order. The SIMD paths use only add, subtract, multiply, divide, square root, compares and bitwise selects, so SSE2, wasm SIMD128, NEON and plain C give the same bits.

The `unit-nav` and `nav-demo` gates pin their results (the suite's checksum, and the demo's crowd hash folded every tick). `node tests/nav-determinism.mjs` runs both on native, Chromium and wcnative at `CW_WORKERS` 0, 1, 3 and the default. A cart has no environment: Chromium's carts get the pool size as `navigator.hardwareConcurrency`, and the GPU sandbox shows wcnative the matching number of online CPUs. Natively, `unit_nav` also rebuilds and reruns at four pool sizes in one process.

## Performance

`node tests/nav-perf.mjs` runs `nav_perf` natively and in Chromium, on one thread and on the default pool. It requires equal result hashes everywhere, budgets, and a parallel build at least twice as fast as one thread. On a Ryzen AI 9 HX 370 (24 threads, Linux):

| | native, 1 thread | native, 24 threads | Chromium, 1 thread | Chromium, 24 threads |
|---|---|---|---|---|
| build, 256 x 256 m world (324 tiles, 2666 polygons) | 245 ms | 32 ms | 231 ms | 103 ms |
| the same, dynamic (tile cache) | 181 ms | 24 ms | 173 ms | 36 ms |
| 64 obstacles applied | 5.0 ms | 1.5 ms | 6.5 ms | 2.1 ms |
| 1000 batched paths | 68 ms | 6.5 ms | 83 ms | 19 ms |
| crowd step, 500 agents | 1.17 ms | 0.24 ms | 1.21 ms | 0.31 ms |
| crowd step, 2000 agents | 4.8 ms | 0.69 ms | 6.1 ms | 1.06 ms |

Tile parallelism is what makes builds fast. In the crowd, the expensive work per agent is the Detour corner and corridor queries, and they spread over the pool.
