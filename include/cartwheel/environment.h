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
 *                     volumetric = true: the fog within volumetric_distance
 *                     of the camera is lit per cell of a 3D grid that
 *                     follows the view (froxels): each light lights it
 *                     through its shadows, so light shafts show where the
 *                     sun falls through a window or between columns and a
 *                     spot light draws its cone. Light from everywhere is
 *                     the fog colour x volumetric_ambient; each light adds
 *                     its colour x energy x its fog_energy property through
 *                     a Henyey-Greenstein lobe of anisotropy sun_g (sun_scatter
 *                     is the analytic fog's alone). The analytic fog
 *                     continues past the grid, lit by the grid's ambient and
 *                     its first directional light, so the two meet without a
 *                     seam; start_distance does not apply. The grid's cells
 *                     are sampled at a jittered point each frame and blended
 *                     with the previous frames (volumetric_temporal), which
 *                     hides the jitter's noise; a light that moves fast
 *                     leaves a short trail.
 *   FogVolume3D       a region of volumetric fog: a BOX or ELLIPSOID of
 *                     `size` placed by the node's transform, or WORLD
 *                     (everywhere), with its own density, albedo (the
 *                     share of light it scatters), emission (light of its
 *                     own), height falloff, soft edges and value noise that
 *                     drifts with noise_wind. Volumes add to each other and
 *                     to a volumetric HeightFog; they work without one too
 *                     (ambient light then comes only from a HeightFog's
 *                     colour, so with no HeightFog a volume shows only where
 *                     lights reach it or it emits). At most 16 volumes and 8
 *                     lights (fog_energy > 0) light the fog.
 *
 * Volumetric fog cost follows the effects detail quality setting
 * (cartwheel/quality.h): the grid (cells across x down x slices; down
 * follows the aspect) and the shadow maps it draws for itself (one
 * orthographic tile for the first shadowed directional light, fitted to the
 * grid's reach; one perspective tile each for up to three shadowed spot
 * lights; omni lights light it unshadowed):
 *
 *                     Low          Medium       High         Ultra
 *   grid              64 x 32      96 x 48      160 x 64     240 x 96
 *   shadow tile       256 (sun)    512          1024         1024
 *
 * On GL (GLES 3.0, WebGL 2) the grid is built by fragment passes into the
 * layers of RGBA16F 3D textures; it needs float render targets (every host
 * cartwheel runs on has them; without, the analytic fog draws instead). On
 * WebGPU it is built by compute shaders. Only perspective cameras.
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

/* What the volumetric fog did last frame (zeroed when the tree has none). */
typedef struct cw_volumetric_fog_state {
  bool active;              /* the grid was built and applied */
  int grid[3];              /* cells across, down and deep */
  int lights;               /* lights that lit it */
  int shadowed_lights;      /* of those, lights with a shadow tile */
  int volumes;              /* FogVolume3D nodes in it */
  float distance;           /* how far the grid reaches (world units) */
  int shadow_map;           /* pixels of a shadow tile (0 = none drawn) */
} cw_volumetric_fog_state;
/* The tree's volumetric fog last frame (zeroed when it has none). */
void cw_volumetric_fog_get_state(cw_tree *t, cw_volumetric_fog_state *out);

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
