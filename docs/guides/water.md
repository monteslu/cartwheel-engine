# Water

{{name}} draws water as three.js r186's water add-ons do (planar reflection and refraction, scrolling ripples, flow maps) on a surface that Gerstner waves really move, and floats rigid bodies on it with the same waves, computed the same way on every host.

Header: [cartwheel/water.h](header:water.h). Node types: [Water3D](node:Water3D) and [Buoyancy3D](node:Buoyancy3D). Sample: `water_boat`, a boat and two crates bobbing beside a pier. Gates: `unit-water`, `water-boat` and `water-surface`.

## A body of water

A [Water3D](node:Water3D) is a rectangle of `size` metres in its node's XZ plane, at rest at the node's height, facing up. Up to four waves move it, each a `vec4` of direction (degrees, 0 = +X, 90 = +Z), wavelength (metres), amplitude (metres) and steepness (0 for round swells, 1 for sharp crests). Long waves travel faster, as on the sea: a wave's speed is `sqrt(g / k)` for its wave number `k`, times `wave_speed`.

@include samples/water_boat/assets/main.cwscene#Water

For an open ocean, set `horizon_distance` to the outer half-extent in metres.
The central 80% of grid coordinates covers `size`; the outer fifth stretches
smoothly toward that horizon on each side. This remains one connected surface,
with the same material and reflection treatment. Zero keeps the ordinary
rectangular grid. Increase subdivisions if close waves become too coarse.
Distant ripple normals and broad wave glints fade before they alias at flight
viewing distances.

Small ripples come from a normal map (by default a seamless noise normal map, see [Procedural noise](guide:noise)), sampled twice and scrolled along the flow so it never visibly repeats or resets (Water2's two-phase flow). `flow_direction` and `flow_speed` set the flow; a `flow_map` image (red and green = the direction, 0.5 = still) makes a river bend or an eddy.

The surface is drawn with the scene's lights: the sun's glints and the shadows of what stands over it are the renderer's own. What the water shows is mixed by a Fresnel term (`reflectivity` is the reflectance looking straight down, 0.02 for water):

- the reflection: the scene rendered again from a camera mirrored about the rest level, with everything below the water clipped away (an oblique near plane, as r186's Reflector does);
- the refraction: the scene rendered from the view's own camera with everything above the water clipped away (r186's Refractor), tinted by `color`;

both bent by the surface's normal (`distortion`). The water itself is hidden while they render.

## Things that float

Give a [RigidBody3D](node:RigidBody3D) a [Buoyancy3D](node:Buoyancy3D) child and it floats:

@include samples/water_boat/assets/main.cwscene#Boat

Each fixed tick, before the physics step, every probe point (by default the corners of the body's first box shape) is compared with the water over it. A probe under the surface gets its share of the displaced water's weight, pushing up at that corner, so a boat rolls and pitches with the waves, and a drag against its motion relative to the water's own orbital motion, so it is carried along by the swell and settles instead of bouncing forever. `volume`, `fluid_density`, `probe_depth` and the two drags tune it: a body floats when its density is below the fluid's.

## The same water everywhere

The waves run on the tree's fixed clock (ticks times the fixed time step, plus `time_offset`). Each wave's phase is reduced into one turn in double precision before it is used, so an hour into a game the waves are as exact as in its first second, and the sums use the engine's deterministic `cw_sin` and `cw_cos`. [cw_water_height_at](api:cw_water_height_at) and [cw_water_sample_at](api:cw_water_sample_at) (height, normal, the water's velocity) therefore give the same bits natively and in a cart, and so does every floating body: the `water-boat` gate pins a hash of the bodies' transforms on every host.

The GPU moves the surface's vertices with the same sums from the same reduced phases, so the drawn water is where the CPU says it is, up to float rounding and the flat triangles between vertices. The `water-surface` gate checks it: the sample draws each water pixel's height as colour, and the CPU's height at 48 points must match the frame to 1.5 cm on every host and backend (it is within 6 mm on the 890M).

Gerstner waves move the water sideways as well as up and down, so "the height at (x, z)" is the height of the surface point that has moved over (x, z). [cw_waves_height](api:cw_waves_height) finds it with four fixed-point steps (well under a millimetre off for any steepness below 1); [cw_waves_displace](api:cw_waves_displace) is the forward direction, for effects that follow a point of the surface.

## Quality and costs

Water follows the effects detail setting of the graphics settings ([cartwheel/quality.h](header:quality.h)):

| Effects detail | Grid | Reflection | Refraction |
|---|---|---|---|
| Low | a quarter of `subdivisions` | none: the sky's environment light stands in | none: `deep_color` stands in |
| Medium | half | at half of `resolution_scale` | none |
| High | as set | at `resolution_scale` | at `resolution_scale` |
| Ultra | as set | at 1.5 x `resolution_scale` (at most full size) | likewise |

Each planar view is one more render of the scene at that size, so its cost is the scene's cost times the share of pixels.

Measured on the 890M with `water_boat` (a 120 m water of 400 subdivisions, the 1080p output at each preset's resolution), GPU ms per frame:

| | Low | Medium | High | Ultra |
|---|---|---|---|---|
| GL: the planar views (the `water` pass) | 0.00 | 0.06 | 0.24 | 0.52 |
| GL: the scene pass, water included | 0.87 | 1.01 | 1.32 | 1.32 |
| GL: the whole frame | 1.04 | 1.16 | 1.80 | 2.09 |
| WebGPU: the whole frame | 0.72 | 1.11 | 1.97 | 1.64 |

On WebGPU three.c submits the planar views itself, outside the pipeline's timed passes, so only the frame's total is comparable (and it leaves those submits out). The grid is most of the scene pass's share: 320,000 triangles at High and Ultra, 80,000 at Medium, 20,000 at Low. A Vega 8 has about a third of the 890M's bandwidth and two thirds of its shader throughput: expect 2 to 3 times these numbers, about 3 ms at Low and 5 ms at High for the whole sample frame; lower `subdivisions` (each halving quarters the grid) or `resolution_scale` first.

On the CPU, a height query costs 4 fixed-point steps of the wave sum (about 0.25 microseconds with four waves on one core of the 890M machine); a Buoyancy3D with eight probes costs eight of them a tick.
