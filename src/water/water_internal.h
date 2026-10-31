/* water module internals (water.c, buoyancy.c) */
#ifndef CW_WATER_INTERNAL_H
#define CW_WATER_INTERNAL_H

#include "cartwheel/pipeline.h"
#include "cartwheel/resource.h"
#include "cartwheel/water.h"
#include "three.h"

typedef struct wave_eval {
  int n;
  float dx[CW_WATER_MAX_WAVES], dz[CW_WATER_MAX_WAVES], k[CW_WATER_MAX_WAVES], a[CW_WATER_MAX_WAVES];
  float q[CW_WATER_MAX_WAVES], ph[CW_WATER_MAX_WAVES], om[CW_WATER_MAX_WAVES];
  /* the wave mask (cw_water_set_wave_mask); NULL: every wave at full height */
  const float *mask;
  int mask_nx, mask_nz;
  float mask_sx, mask_sz;
  float shelter[CW_WATER_MAX_WAVES];
} wave_eval;

typedef struct water3d {
  /* properties */
  float size[2];
  float horizon_distance;
  int grid_mode; /* 0 RECT, 1 VIEW */
  float grid_min_cell;
  float wave_shelter[4];
  int subdivisions;
  int wave_count;
  float wave0[4], wave1[4], wave2[4], wave3[4];
  float wave_speed, time_offset;
  float color[4], deep_color[4];
  float reflectivity, distortion, swell_reflect, detail, roughness, murk;
  const char *normal_map;
  float normal_scale;
  float flow_direction[2];
  float flow_speed;
  const char *flow_map;
  bool reflection, refraction;
  float resolution_scale;
  int debug_view;
  float debug_range[2];
  /* shore and depth shading, foam */
  int floor_map_size;
  float floor_range, clarity, shore_foam, crest_foam, foam_scale, foam_lace, foam_soft;
  float shallow_color[4];
  /* the look */
  float scatter, glint;
  /* the interactive ripple layer (ripples.c) */
  int ripple_cells;
  float ripple_size[2], ripple_center[2], ripple_speed, ripple_damping, ripple_strength, foam_fade, foam_spread;
  float rain, rain_size, rain_strength;
  const char *splash_fx;
  float splash_fx_min;
  /* caustics and the view from under the water (underwater.c) */
  float caustics, caustic_size;
  bool underwater;
  /* runtime (cleared on destroy from here on) */
  t3_mesh *mesh;
  t3_material *mat;
  t3_render_target *refl, *refr;
  t3_camera *vcam;
  cw_res normal_res, flow_res;
  const char *loaded_normal, *loaded_flow;
  int built_sub, built_mode, triangles;
  float built_min_cell;
  float grid_off[2]; /* VIEW: the grid's centre this view (water-local x, z) */
  float *mask;       /* mask_nx x mask_nz, row z, col x; NULL: none */
  int mask_nx, mask_nz;
  t3_texture *mask_tex, *mask_white;
  float built_size[2], built_amp, built_horizon;
  bool refl_on, refr_on;
  wave_eval ev;      /* this frame's waves, as drawn */
  float flow_off[3]; /* Water2's flow offsets 0, 1 and the half cycle */
  float cam[4];      /* the camera world position this frame */
  t3_texture *floor_tex;
  float *floor_depth; /* floor_n x floor_n metres below the rest level (0: land), row z, col x */
  int floor_n;
  bool floor_dirty, floor_done;
  float foam_time;
  float *rip_h, *rip_v, *rip_f; /* ripple height, velocity, foam: rip_nx x rip_nz, row z, col x */
  int rip_nx, rip_nz, rip_cells_built, rip_quality;
  float rip_ax, rip_az, rain_acc;
  bool rip_awake, rip_upload;
  uint32_t fx_next;
  t3_texture *rip_tex;
  float sun[4], sun_col[4];
  float refr_inv[16]; /* the refraction camera's inverse projection (its depth texture reads back to view space) */
  bool refr_depth_ok;
  bool refr_above;      /* the camera is under the water: the refraction target holds the view up through the surface */
  bool under_drawn;     /* the full-screen pass drew this view as seen from under the water */
  t3_texture *caus_tex; /* the caustic tile for the surface (caustics.c) */
  cw_rt *wg_col, *wg_dep; /* WebGPU: the refraction's colour + depth target and its depth's R32Float copy (refraction_wgpu.c) */
  t3_texture *wg_col_tex, *wg_dep_tex;
  bool wg_fresh;
  bool cam_under; /* this view's camera is under the surface */
} water3d;

/* the waves at time t, phases reduced (water.c) */
void cw__waves_prepare(const cw_waves *w, double t, wave_eval *e);
cw_waves cw__water_waves(const water3d *d);
/* the water's wave mask into a prepared wave_eval */
void cw__waves_use_mask(const water3d *d, wave_eval *e);
const cw_node_type *cw__water_type(void);
water3d *cw__water_data(cw_node n);
void cw__ripples_free(water3d *d);
void cw__ripples_step(cw_node n, water3d *d, uint32_t tick);
void cw__ripples_upload(water3d *d);
int cw__water_quality(void);
/* the tree's i-th Water3D (an invalid node past the last) */
cw_node cw__water_at(cw_tree *t, int i);
/* underwater.c, caustics.c */
#define CW_WATER_CAUSTIC_N 256
const uint8_t *cw__water_caustics(void);      /* CW_WATER_CAUSTIC_N^2 bytes, intensity / 4 */
const uint8_t *cw__water_caustics_soft(void); /* the same, blurred over a few texels */
void cw__water_add_under_pass(cw_tree *t);
/* refraction_wgpu.c: WebGPU's refraction render into d->wg_col and the depth copy into d->wg_dep (cam NULL: cleared) */
bool cw__water_refr_wgpu(cw_pass_ctx *ctx, water3d *d, t3_camera *cam, int w, int h);
void cw__water_refr_wgpu_free(water3d *d);
/* buoyancy.c */
void cw__register_buoyancy_type(void);
int cw__buoyancy_count(cw_tree *t);

#endif
