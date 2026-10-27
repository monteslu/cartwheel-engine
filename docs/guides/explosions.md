# Explosions and distortion

{{name}} bends the picture the way a blast does. Heat haze above a fire, a shockwave ring racing across a wall, a glass shard or a water drop that shows the scene through it: all of them draw "move the picture this much here" instead of colour, and one pass after the 3D scene reads the frame through those moves. It looks the same on GL and WebGPU.

Header: [cartwheel/explosions.h](header:explosions.h). Node types: [Shockwave3D](node:Shockwave3D), and [GPUParticles3D](node:GPUParticles3D) with the new `DISTORT` and `REFRACT` blends. Sample: `fx_distort`. Gates: `fx-distort` and `fx-distort-regions`.

## Heat haze

Set a particle system's `blend` to `DISTORT`. Its particles stop drawing colour; each one shifts whatever is behind it by `distort_strength` (in world units, at the particle's distance, so a haze looks the same near and far) times the alpha of its `color_ramp` over life. Without a texture each particle wobbles by its age; a texture's red and green channels (0.5 = no push, like a normal map) give the direction instead, and its alpha the weight.

@include samples/fx_distort/assets/main.cwscene#Haze

A tall column of slow, big, overlapping particles over a fire is heat haze. Keep `distort_strength` small (0.05 to 0.15): the picture should shimmer, not melt.

Two rules keep the picture honest. A haze particle is hidden wherever scene geometry is nearer than it, so a pole standing in front of the haze stays sharp. And the pass never pulls in colour from geometry nearer than the haze, so a foreground object does not smear into it.

## A shockwave

A [Shockwave3D](node:Shockwave3D) is an expanding ring that bends the picture: no texture needed. `max_radius` is where it ends, `duration` how long it takes, `thickness` the width of the bent band, `strength` how far it pushes at the start (the ring slows as it grows and fades out). It fires when it enters the tree (`autostart`); `cw_shockwave_fire` fires it again, so one node can serve every blast: move it to the blast, then fire it. `ground` lays the ring along the node's XZ plane for a blast spreading over the floor; without it the ring faces the camera.

@include samples/fx_distort/assets/main.cwscene#Wave

The ring advances with the fixed step only, so the radius at tick N is the same on every host: `cw_shockwave_radius` reads it.

## Seeing through particles

Set `blend` to `REFRACT` and each particle shows the scene through a lens: the engine copies the scene colour once before the particles draw, and the particle samples that copy bent radially (or by its texture's red and green) by `distort_strength`. Use it for glass shards, water drops, a heat bubble. A sample that would land on geometry nearer than the particle is not taken, so nothing in front smears into the lens. Refracting particles show the scene as it was before any particle drew, so list them first in the tree if other particles should appear on top of them.

@include samples/fx_distort/assets/main.cwscene#Lens

## Cost and presets

The distortion buffer is drawn at a share of the frame that follows the effects preset (half on Low and Medium, full on High and Ultra) and filtered up; colour fringing along bent edges is High and Ultra only. The passes run only while something distorts: a scene without haze pays nothing. Everything here is render only and reads the frame's own depth; nothing reaches the simulation.

## Testing

`tests/distort.mjs` renders `fx_distort` on both backends with the effect on and off and checks that the haze, ring and lens change the picture inside their footprints, that nothing changes outside them, that a pole in front of the haze is identical with and without it (and bends when the pole stands behind it: the control), and that GL and WebGPU agree.
