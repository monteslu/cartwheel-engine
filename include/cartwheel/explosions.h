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

#include "particles.h"
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


/* ── the explosion kit ───────────────────────────────────────────────────
 *   Explosion3D (Node3D)   fires a whole explosion from its children and a
 *                          few pooled helpers. Author the children like any
 *                          scene: GPUParticles3D parts (fire, smoke, sparks,
 *                          debris; emitting off, one_shot on), a Shockwave3D
 *                          (autostart off). The node's properties:
 *     autostart, loop, duration    fires when it enters the tree; loop fires
 *                                  again every `duration` seconds
 *     flash_energy, flash_color,   a light that flashes at the blast and dies
 *     flash_range, flash_duration  away fast. The lights come from a pool the
 *                                  effects preset sizes (cw_quality_scale
 *                                  "flash_lights": 2, 4, 8, 8 lights); a
 *                                  busy scene takes the oldest flash back, so
 *                                  blasts in a row or in four split-screen
 *                                  views never grow the light count
 *     scorch_size, scorch_time,    a scorch mark on the ground under the node
 *     scorch_life, scorch_glow,    (it stands at ground level): a dark layer
 *     scorch_depth, scorch_offset  that fades after scorch_life and a glow
 *                                  that cools from orange to nothing over
 *                                  scorch_time (12 marks pooled, the oldest
 *                                  recycled)
 *     shake_amount, shake_radius,  camera shake: cw_explosion_shake gives the
 *     shake_time                   shake at a place; cw_shake_offset turns it
 *                                  into a camera offset
 * Sub-emitters: a GPUParticles3D with emission_shape PARENT spawns at the
 * living particles of another GPUParticles3D (particles.h).
 * Flipbooks: GPUParticles3D motion_texture blends flipbook frames along
 * baked motion vectors (tools/fx/bake-flipbooks.mjs bakes fire and smoke).
 * Everything runs on the fixed step: the same ticks give the same flash,
 * scorch and shake on every host. */

/* Fire an Explosion3D now (restarts its particles and shockwave, takes a flash
 * light and a scorch mark, records the shake). */
void cw_explosion_fire(cw_node n);
/* The explosion is playing (fired and not past its duration). */
bool cw_explosion_playing(cw_node n);
/* Camera shake at a world position from every explosion playing in the tree:
 * each adds shake_amount x (1 - distance / shake_radius)^2 x (1 - age /
 * shake_time)^2; 0 when none is near, at most 4. */
float cw_explosion_shake(cw_tree *t, t3_vec3 at);
/* A deterministic camera offset for a shake value: three numbers hashed from
 * the tick, each in [-max_offset, max_offset] x min(shake, 1). Add it to the
 * camera's position (and scale it to its rotation if you like); the same
 * tick gives the same offset on every host. */
static inline t3_vec3 cw_shake_offset(float shake, uint32_t tick, float max_offset) {
  float k = (shake < 1 ? shake : 1) * max_offset;
  return (t3_vec3){ ((float)(cw_particle_hash(tick * 3u + 1u) >> 8) / 8388608.0f - 1.0f) * k,
                    ((float)(cw_particle_hash(tick * 3u + 2u) >> 8) / 8388608.0f - 1.0f) * k,
                    ((float)(cw_particle_hash(tick * 3u + 3u) >> 8) / 8388608.0f - 1.0f) * k };
}

/* What the pools hold now (tests, profilers). */
typedef struct cw_explosion_stats {
  int flash_pool;       /* flash lights made */
  int flashes_active;   /* of them borrowed by an explosion now */
  int lights_lit;       /* of them with energy above 0 now */
  int scorch_pool;      /* scorch marks (dark + glow decal pairs) made */
  int scorches_active;  /* of them in use */
} cw_explosion_stats;
/* The pools' counts now; zeroed when the tree has no Explosion3D. */
void cw_explosion_get_stats(cw_tree *t, cw_explosion_stats *out);

#ifdef __cplusplus
}
#endif
#endif
