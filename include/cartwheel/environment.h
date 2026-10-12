/* Sky and fog. Node types, all reflected
 * (docs/nodes.json) and .cwscene-authorable:
 *
 *   ProceduralSky     a physically based sky: single scattering of sunlight
 *                     by air (Rayleigh) and haze (Mie) in a 60 km atmosphere
 *                     over a 6360 km planet, ray-marched on the GPU into a
 *                     cube map. The sun's position comes from time_of_day
 *                     (hours), latitude and declination (the season); a
 *                     non-zero day_speed runs the day / night cycle on the
 *                     fixed tick (deterministic). It drives a
 *                     DirectionalLight3D (`sun`): its direction, its colour
 *                     (the light that reaches the ground through the
 *                     atmosphere: reddening at sunset) and its energy
 *                     (sun_energy x that transmittance; zero below the
 *                     horizon). The cube is the scene's background (with an
 *                     analytic sun disk drawn after the opaque scene) and,
 *                     prefiltered by three.c's PMREMGenerator, the scene's
 *                     environment (image-based light for Standard materials
 *                     without an envMap of their own). The cube and the environment are
 *                     re-rendered only when the sun moved more than
 *                     env_update_angle or a property changed (at most once a
 *                     frame). Units: sky radiance is sunlight scattered from
 *                     an irradiance of sun_energy x sky_energy, the same
 *                     lux-like scale as light energies.
 *   HeightFog         exponential height fog in the post chain (an EFFECTS
 *                     pass that reads depth): density falls off with height
 *                     above base_height, integrated in closed form along
 *                     each view ray (no froxels); in-scattered colour from
 *                     `color`, or from the sky's horizon when
 *                     use_sky_color; a Henyey-Greenstein lobe toward the sun
 *                     (sun_scatter, sun_g) for the glow around it. Sky
 *                     pixels are fogged up to
 *                     sky_distance.
 *
 * The debug channel's `environment` command reports the sun and the sky
 * updates. */
#ifndef CW_ENVIRONMENT_H
#define CW_ENVIRONMENT_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_sky_state {
  bool active;              /* a ProceduralSky is in the tree */
  float time_of_day;        /* hours */
  t3_vec3 sun_direction;    /* world, toward the sun */
  float sun_elevation;      /* degrees above the horizon */
  t3_color sun_color;       /* transmittance x sun colour (what the driven light gets) */
  float sun_energy;         /* the driven light's energy */
  t3_color horizon;         /* sky radiance at the horizon toward the sun's side (fog colour) */
  int cube_updates;         /* sky cubes rendered so far */
} cw_sky_state;

/* The tree's sky this frame (zeroed when it has none). */
void cw_sky_get_state(cw_tree *t, cw_sky_state *out);

/* The single-scattering model on the CPU, for tools and tests: the sky
 * radiance in direction `dir` (world, unit) with the sun toward `sun_dir`,
 * for the given Rayleigh / Mie scales and Mie anisotropy, from a sun of
 * irradiance 1 outside the atmosphere. Deterministic (cw_ math). */
t3_color cw_sky_radiance(t3_vec3 dir, t3_vec3 sun_dir, float rayleigh, float mie, float mie_g);
/* The share of sunlight that reaches the ground along `sun_dir` (per channel). */
t3_color cw_sky_transmittance(t3_vec3 sun_dir, float rayleigh, float mie);

#ifdef __cplusplus
}
#endif
#endif
