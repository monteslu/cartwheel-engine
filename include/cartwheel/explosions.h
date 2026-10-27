/* Explosion tools: the distortion buffer (heat haze, shockwaves, refraction),
 * Shockwave3D, and the explosion kit.
 *
 * ── distortion ──────────────────────────────────────────────────────────
 * Things that bend the picture add "move the picture this much here" into a
 * small distortion buffer; one pass after the 3D scene (the "distort" POST
 * pass, order 150: before anti-aliasing, bloom and tone mapping) reads the
 * frame through it. Sources:
 *   GPUParticles3D with blend DISTORT: each particle is a soft patch that
 *       shifts the picture behind it by distort_strength (world units at the
 *       particle's distance) times its colour alpha over life, wobbling with
 *       its age. The colour is not drawn. With a texture, its red and green
 *       channels are the direction (0.5 = none, like a normal map) and its
 *       alpha the weight. A column of such particles over a fire is heat
 *       haze.
 *   Shockwave3D: an expanding ring (below).
 * Guards (the picture stays honest): a bending thing is hidden where scene
 * geometry is nearer than it, so a pole in front of the haze stays sharp;
 * the pass will not pull in colour from geometry nearer than the haze.
 * Render only: nothing here feeds the simulation. Both backends run the same
 * passes, the buffer is drawn at the share of the frame the effects preset
 * sets (cw_quality_scale("distortion"): 0.5, 0.5, 1, 1) and colour fringing
 * (cw_quality_scale("dispersion"): 0, 0, 1, 1) is High and Ultra.
 *
 * ── refraction for particles ────────────────────────────────────────────
 * GPUParticles3D with blend REFRACT: the engine copies the scene colour once
 * before the particles draw and each particle shows that copy through a
 * lens (distort_strength, bent radially, or by the texture's red and green):
 * glass shards, water drops, a heat bubble. A sample that would land on
 * geometry nearer than the particle is not taken (no smear). REFRACT
 * particles do not see each other.
 *
 * ── node types (properties: docs/nodes.json) ────────────────────────────
 *   Shockwave3D (Node3D)   an expanding ring that bends the picture: no
 *                          texture, ground (a ring in the node's XZ plane) or
 *                          facing the camera, max_radius, duration, thickness,
 *                          strength, loop, autostart; read-only age, radius,
 *                          amplitude. The ring slows as it grows
 *                          (radius = max_radius x (1 - (1 - t)^2)) and fades
 *                          out. It advances with the fixed step only. */
#ifndef CW_EXPLOSIONS_H
#define CW_EXPLOSIONS_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start a Shockwave3D now (age 0, playing). A pooled ring is sent to a new
 * place by moving the node, then firing it. */
void cw_shockwave_fire(cw_node n);
/* The ring's radius now (0 before it fires), world units. */
float cw_shockwave_radius(cw_node n);
/* The ring is expanding now. */
bool cw_shockwave_active(cw_node n);

#ifdef __cplusplus
}
#endif
#endif
