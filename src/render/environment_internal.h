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
bool cw__vfog_on(void); /* volumetric fog draws (supported, effects detail above Low): else a volumetric fog draws as height fog */
/* A light of the tree for the froxel lighting, from its pre_render (only
 * kept when the tree has volumetric fog): kind 0 directional, 1 omni, 2
 * spot; shadow: the light casts shadows (the fog gets its own map). */
void cw__vfog_push_light(cw_tree *t, const t3_light *l, int kind, bool shadow, float fog_energy);
/* The tree has volumetric fog this frame: its passes are registered. */
void cw__vfog_touch(cw_tree *t);

/* ── fog over transparent surfaces ───────────────────────────────────
 * Transparent and additive surfaces write no depth, so the fog passes (which
 * fog each pixel by the depth of what is behind it) must not fog them: while a
 * fog pass draws, the scene pass draws the opaque queue only, the fog passes
 * run over the opaque picture, and the transparent queue ("transparents",
 * EFFECTS 130) and the particles (200) draw after them, each surface fogged in
 * its own shader at its own distance: alpha-blended ones toward the fog's
 * light, additive ones toward nothing (added light fades in fog). three.c
 * materials take the fog through the "hfog" shading part (extensions/hfog.mjs);
 * the engine's own shaders (particles, trails) through cw_ffog below. */

/* The fog a transparent surface drawn in this run takes at its own distance:
 * mode 0 none, 1 the analytic height fog, 2 the volumetric grid looked up at
 * the surface's own distance, then the analytic fog past the grid (as the
 * volumetric pass does for the opaque picture). u: CW__FFOG_VEC4 vec4s, the
 * layout CW__FFOG_GLSL / CW__FFOG_WGSL read. vol: the integrated grid (mode
 * 2), a GL 3D texture, or on WebGPU a pass texture id (cw__pw_view). */
enum { CW__FFOG_VEC4 = 9 };
typedef struct cw__fog_fwd {
  int mode;
  float u[CW__FFOG_VEC4 * 4];
  unsigned vol;
} cw__fog_fwd;
void cw__fog_forward(cw_pass_ctx *ctx, cw__fog_fwd *out);
/* GLSL: uniform vec4 u_ff[9]; uniform highp sampler3D u_ffvol;
 * vec4 cw_ffog(vec3 world, float view_z): (in-scattered light, transmittance).
 * A premultiplied colour c of coverage a is then c * T + I * a (alpha
 * blended), c * T (additive). */
extern const char CW__FFOG_GLSL[];
/* WGSL: fn cw_ffog(f: array<vec4f, 9>, vol: texture_3d<f32>, s: sampler, frag: vec2f, world: vec3f, view_z: f32) -> vec4f,
 * frag the fragment's position in the target (@builtin(position).xy) */
extern const char CW__FFOG_WGSL[];
/* the volumetric pass's grid for this run's view (volumetric_fog.c): fills mode 2 (false when it draws none) */
bool cw__vfog_forward(cw_pass_ctx *ctx, cw__fog_fwd *out);
/* a fog pass (height or volumetric) draws this run: the transparent queue then waits for it */
bool cw__fog_draws(const cw_pass_ctx *ctx);
/* the "transparents" pass (pipeline_post.c): active when a fog pass draws and the scene has transparent draws; it
 * marks those materials with the hfog part (and takes it off materials that stopped being transparent) */
bool cw__fog_transparents_wanted(const cw_pass_ctx *ctx);
/* the hfog part's values for this run (before the transparent queue draws) */
void cw__fog_transparents_begin(cw_pass_ctx *ctx);
/* CW_TEST_FOG_OVER_TRANSPARENTS=1 (a gate control): the fog over everything, by the depth behind it, as before */
bool cw__fog_over_transparents(void);
/* a renderer was made (src/scene/render3d.c): it takes the hfog part's hook */
void cw__fog_renderer_init(t3_renderer *r);

#endif
