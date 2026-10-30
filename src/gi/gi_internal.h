/* src/gi: reflection probes (cartwheel/gi.h), shared between its files */
#ifndef CW_GI_INTERNAL_H
#define CW_GI_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/gi.h"
#include "cartwheel/pipeline.h"

typedef struct refl_probe {
  t3_vec3 size, origin_offset;
  bool box_projection, ambient;
  float blend_distance, intensity;
  int resolution;
  int update_mode;
  int update_every;
  float near, far;
  int cull_mask;
  /* runtime */
  t3_render_target *rt[2]; /* front: sampled; the other: the next capture */
  int front;
  int size_now;            /* face size of the front target (0 = none) */
  int age;                 /* frames since the last capture */
  bool requested;
  int captures;
} refl_probe;

typedef struct probe_grid {
  t3_vec3 size, probes;
  int cube_size, bounces, update_mode, probes_per_frame, bake_per_frame;
  float intensity, normal_offset, near, far;
  /* runtime */
  t3_render_target *atlas; /* nx*nz x ny*7 texels, half float */
  int atlas_w, atlas_h, res_now[3];
  int next, sweeps, updates;
  bool baked, requested, clear_atlas;
} probe_grid;

typedef struct gi_tree {
  CW_ARRAY(cw_node) probes;
  CW_ARRAY(cw_node) grids;
  /* the grid's capture: six faces, the projection quad */
  t3_render_target *face[6];
  int face_size, proj_group;
  t3_camera *face_cam, *proj_cam;
  t3_scene *proj_scene;
  t3_material *proj_mat;
  t3_renderer *proj_hook;
  CW_ARRAY(t3_material *) mats; /* retained: those given the gi part */
  t3_pmrem_generator *pmrem;
  t3_texture *dummy, *dummy_cube;
  const t3_texture *env_seen; /* scene.environment at the last frame */
  t3_renderer *hook_renderer, *gbuf_hook;
  /* the screen-space passes' G-buffer (ssr.c) and the mode the gi part draws in: 0 lit, 1 G-buffer */
  cw_rt gbuf, albedo; /* the G-buffer: normal + roughness + metalness; diffuse colour (SSGI) */
  bool gbuf_ok, albedo_ok; /* drawn this frame */
  int mode;
  int bound_max;
  cw_gi_stats stats;
} gi_tree;

gi_tree *cw__gi_tree(cw_tree *t);
void cw__gi_fill(t3_extension_ctx *c, void *user);
void cw__gi_fill_gbuf(t3_extension_ctx *c, void *user);
void cw__register_reflection_probe(void);
t3_texture *cw__gi_dummy(gi_tree *g);
/* the light probe grid's values for a draw (probe_grid.c) */
void cw__gi_fill_grid(t3_extension_ctx *c, gi_tree *g);
void cw__gi_grids_exec(gi_tree *g, cw_pass_ctx *ctx);
void cw__gi_grid_tree_free(gi_tree *g);
void cw__gi_grids_sky_changed(gi_tree *g);
void cw__register_probe_grid(void);
/* give every Standard / Physical material under o the gi part */
void cw__gi_tag(gi_tree *g, t3_object *o);
void cw__gi_add_post_passes(cw_pipeline *p);
void cw__gi_add_shadow_pass(cw_pipeline *p);
bool cw__gi_gbuffer(cw_pass_ctx *ctx, gi_tree *g, int w, int h, int mode);
void cw__gi_add_ssgi_pass(cw_pipeline *p);

#endif
