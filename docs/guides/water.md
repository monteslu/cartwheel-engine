# Water

{{name}} draws water as three.js r187's water add-ons do (planar reflection and refraction, scrolling ripples, flow maps) on a surface that Gerstner waves really move, and floats rigid bodies on it with the same waves, computed the same way on every host.

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

- the reflection: the scene rendered again from a camera mirrored about the rest level, with everything below the water clipped away (an oblique near plane, as r187's Reflector does);
- the refraction: the scene rendered from the view's own camera with everything above the water clipped away (r187's Refractor), tinted by `color`;

both bent by the surface's normal (`distortion`). The water itself is hidden while they render.

## Shore, depth and foam

Water that is the same colour everywhere looks like a sheet of glass. Give it a floor to measure against and it shades by depth: pale turquoise where it is shallow, deep colour further out, foam where it meets the shore and white caps on the crests.

@include samples/water_shore/assets/main.cwscene#Water

The depth comes from a floor map the water bakes once, from the tree's static physics shapes: for each cell of a grid over the rectangle (`floor_map_size` cells along the longer side, 128 by default, 0 turns the whole feature off) it asks the physics world what is there. A static shape holding the point at the rest level is land (depth 0, also where a pier post stands in the water); otherwise the highest static shape under the rest level is the floor, and no shape within `floor_range` metres means deep. Nothing is read back from the GPU, so the map, [cw_water_floor_depth_at](api:cw_water_floor_depth_at) (a wading character, a fishing spot) and the picture all agree on every host. Call [cw_water_rebake_floor](api:cw_water_rebake_floor) after moving static shapes. Floors that are only meshes, with no static shape, have no depth: give them a collision shape.

- `shallow_color` is the water over a shallow floor; it turns into `deep_color` as the depth grows, over `clarity` metres (a Beer-Lambert style falloff, so the first metres change fastest). With the refraction drawn, `murk` only acts as the water deepens, so the floor shows through the shallows.
- `shore_foam` is the width in metres of the foam band where the water meets the floor. It ebbs and flows and is broken up by the same noise as the ripples.
- `crest_foam` is how much of the crest foams: the surface bunches up on a crest, and where it bunches enough the water whitens. The foam is lit by the scene's lights, so it darkens at dusk with everything else.

What the map cannot see: moving bodies. A boat gets its foam and rings from the ripple layer below.

## Splashes, wakes and rain

Water that only waves never reacts. A [Water3D](node:Water3D) also keeps a grid of small ripples: a height field that splashes, wakes and rain write into, that spreads out as rings, fades, and is drawn as a bent surface normal plus foam. It lives on the CPU and reaches the picture as a half-float texture, so GL and WebGPU draw the same thing and nothing needs compute shaders.

@include samples/water_ripples/assets/main.cwscene#Water

- `ripple_cells` is the grid's resolution along its longer side (128 by default; 0 turns the layer off). `ripple_size` and `ripple_center` choose the rectangle it covers (empty: the whole water): a 50 m grid of 200 cells has 25 cm cells, the size of a footstep's ring, so cover the play area, not an ocean. Outside the grid the water is plain. The effects detail setting scales the cell count (Low half, Medium three quarters).
- `ripple_speed`, `ripple_damping`, `ripple_strength` and `foam_fade` set how fast rings travel, how fast they die, how strongly they bend the light, and how long foam stays.
- [cw_water_splash](api:cw_water_splash) makes a round dent (a depth in metres, a radius, an amount of foam) that spreads into rings. [cw_water_wake](api:cw_water_wake) is for a body moving through the water: call it once a tick and it leaves a trough and a foam trail.
- A [Buoyancy3D](node:Buoyancy3D) does both by itself: a splash where its body enters the water (bigger the faster it falls) and a wake while it moves; its `splash` and `wake` properties scale them or switch them off.
- `rain` is raindrops per second over the grid; `rain_size` and `rain_strength` shape each ring.
- `splash_fx` names a node whose [GPUParticles3D](node:GPUParticles3D) children (one-shot, not emitting) are a pool of spray emitters: a splash deeper than `splash_fx_min` moves the next one to the surface and restarts it. Give the pool node an identity transform. The sample's pool is two spray and mist pairs.

The grid is a look, not a gameplay height: bodies float on the Gerstner waves alone, so floating never depends on the grid's resolution or the quality setting. The grid itself is simulated with the engine's own arithmetic and integer hashes in a fixed order, on the fixed clock, so its bits are the same on every host: [cw_water_ripple_hash](api:cw_water_ripple_hash) is pinned by the `water-ripples` gate, and [cw_water_ripple_at](api:cw_water_ripple_at) reads the height for gameplay that wants to know (a fishing float that bobs when something lands near it). A field that has gone flat stops being stepped and uploaded.

## The look

Out of the box the water is tuned to read as water, not as a blue sheet:

- What is seen through the surface loses its light on the way down to it and back up along the view, per colour: red goes first, then green, so sand under a metre of water turns turquoise and under ten metres is gone into blue (`clarity`, 5 m by default, is how far the light carries; `murk` is how much of the water's own colour scatters in over the same path). With the refraction drawn (High and Ultra) that path is measured per pixel from the refraction's depth, so the boat's hull, a pier post and the floor each fade by their own distance.
- The body is lit by the sun: it dims and reddens toward dusk instead of staying one colour.
- The sun sends a turquoise glow through the crests that face it (`scatter`, with a ProceduralSky), and a narrow glitter runs along the fine chop where it reflects (`glint`).
- Three octaves of ripple normals break up the highlights; the swell's own tilt moves the reflection less than the ripples do, so a steep wave face does not reach past the mirrored horizon.
- Foam is lace, not cotton: its bubbles follow the caustic pattern's web of lines (below), thin foam keeping only the brightest knots and thick foam filling the cells; far away, where the lace would shimmer, it evens out.

Set `scatter` and `glint` to 0 for a plainer look.

## Caustics and the view from under the water

Sunlight that passes through a rippled surface is bent into a moving web of bright lines on whatever lies under it. The water draws that web (`caustics`, the strength, 1 by default; `caustic_size`, the metres one tile of the pattern covers) on the floor it shows through its refraction, shifted along the sun's path through the water and fading with depth, so the lines are sharp in the shallows and gone a few metres down. The pattern is traced once when the game starts: a rippled surface bends a grid of light rays and they are gathered where they land, so it has the cusps and bright knots of real caustics, and it is the same image on every host. It needs a sky (ProceduralSky) for the sun's direction and the refraction's depth (High and Ultra) to know where the floor is.

A camera under the surface sees through the water (`underwater`, on by default): light fades with distance, red first, into the water's own colour, which darkens the deeper the camera is; whatever faces up (the floor, a rock's top, not a hull's underside) carries the caustics, and the same pattern met along each view ray draws soft shafts of light slanting down from the sun; and the surface seen from below shows the world above only inside a circle of light (the window that refraction leaves; past it the surface mirrors the water). Where the surface crosses the lens a thin dark line marks the waterline, and each pixel decides for itself whether it is above or below, so a camera bobbing at the surface splits the picture along the real wave. One full-screen pass does it, and only while the camera is under or near the water of its view, so it costs nothing on a dry frame.

The `water-under` gate puts the beach sample's camera under the surface and checks that the frame was drawn from under the water ([cw_water_get_stats](api:cw_water_get_stats), `underwater`), on every host and backend. The `water-under-new` gate spawns a second Water3D while the camera is already under the water (refraction off, as a level that builds its pools around the player does): it must draw from its first frame, with no draw skipped, on GL and WebGPU.

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

| Effects detail | Grid | Reflection | Refraction | Ripple grid | Caustics |
|---|---|---|---|---|---|
| Low | a quarter of `subdivisions` | none: the sky's environment light stands in | none: `deep_color` stands in | half of `ripple_cells` | none |
| Medium | half | at half of `resolution_scale` | none | three quarters | under the water only |
| High | as set | at `resolution_scale` | at `resolution_scale`, with its depth (depth through the refraction, caustics seen from above) | as set | yes |
| Ultra | as set | at 1.5 x `resolution_scale` (at most full size) | likewise | as set | yes |

The view from under the water (its fog and waterline) is the same on every preset: it is one cheap full-screen pass, and it keeps what a player can see the same.

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
