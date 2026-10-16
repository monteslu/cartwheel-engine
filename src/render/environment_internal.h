/* Sky and fog internals (src/render/sky.c, fog.c). */
#ifndef CW_ENVIRONMENT_INTERNAL_H
#define CW_ENVIRONMENT_INTERNAL_H

#include "cartwheel/environment.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"

/* The tree's sky this frame: its prefiltered environment (NULL when off),
 * the direction toward the sun, the sunlight that reaches the ground (colour
 * x energy) and the horizon's radiance. False when the tree has no sky. */
bool cw__sky_env(cw_tree *t, t3_texture **env, t3_vec3 *sun, t3_color *sun_color, t3_color *horizon);

/* The sky model's inputs this frame (for the lightmapper's sky): false when
 * the tree has no ProceduralSky. */
bool cw__sky_params(cw_tree *t, t3_vec3 *sun, float *rayleigh, float *mie, float *g, float *e0, t3_color *night);

void cw__register_sky_types(void);
void cw__register_fog_types(void);
void cw__register_fog_volume_types(void);

/* A HeightFog node's properties (fog.c). */
typedef struct cw__height_fog {
  float density, height_falloff, base_height;
  t3_color color;
  bool use_sky_color;
  float sun_scatter, sun_g, max_opacity, start_distance, sky_distance;
  /* volumetric mode (volumetric_fog.c) */
  bool volumetric;
  float volumetric_distance, volumetric_detail_spread, volumetric_temporal, volumetric_ambient;
  t3_color volumetric_albedo;
} cw__height_fog;

/* The tree's HeightFog this frame (NULL when it has none). */
const cw__height_fog *cw__height_fog_of(cw_tree *t);

/* Volumetric fog (volumetric_fog.c): whether this run draws it (a host that
 * cannot render float targets on GL falls back to the analytic fog). */
bool cw__vfog_supported(void);
/* A light of the tree for the froxel lighting, from its pre_render (only
 * kept when the tree has volumetric fog): kind 0 directional, 1 omni, 2
 * spot; shadow: the light casts shadows (the fog gets its own map). */
void cw__vfog_push_light(cw_tree *t, const t3_light *l, int kind, bool shadow, float fog_energy);
/* The tree has volumetric fog this frame: its passes are registered. */
void cw__vfog_touch(cw_tree *t);

#endif
