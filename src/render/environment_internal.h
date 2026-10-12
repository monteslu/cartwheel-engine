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

#endif
