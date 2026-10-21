/* water module internals (water.c, buoyancy.c) */
#ifndef CW_WATER_INTERNAL_H
#define CW_WATER_INTERNAL_H

#include "cartwheel/resource.h"
#include "cartwheel/water.h"
#include "three.h"

typedef struct wave_eval {
  int n;
  float dx[CW_WATER_MAX_WAVES], dz[CW_WATER_MAX_WAVES], k[CW_WATER_MAX_WAVES], a[CW_WATER_MAX_WAVES];
  float q[CW_WATER_MAX_WAVES], ph[CW_WATER_MAX_WAVES], om[CW_WATER_MAX_WAVES];
} wave_eval;

typedef struct water3d {
  /* properties */
  float size[2];
  float horizon_distance;
  int subdivisions;
  int wave_count;
  float wave0[4], wave1[4], wave2[4], wave3[4];
  float wave_speed, time_offset;
  float color[4], deep_color[4];
  float reflectivity, distortion, detail, roughness, murk;
  const char *normal_map;
  float normal_scale;
  float flow_direction[2];
  float flow_speed;
  const char *flow_map;
  bool reflection, refraction;
  float resolution_scale;
  int debug_view;
  float debug_range[2];
  /* runtime (cleared on destroy from here on) */
  t3_mesh *mesh;
  t3_material *mat;
  t3_render_target *refl, *refr;
  t3_camera *vcam;
  cw_res normal_res, flow_res;
  const char *loaded_normal, *loaded_flow;
  int built_sub, triangles;
  float built_size[2], built_amp, built_horizon;
  bool refl_on, refr_on;
  wave_eval ev;      /* this frame's waves, as drawn */
  float flow_off[3]; /* Water2's flow offsets 0, 1 and the half cycle */
  float cam[4];      /* the camera world position this frame */
} water3d;

/* the waves at time t, phases reduced (water.c) */
void cw__waves_prepare(const cw_waves *w, double t, wave_eval *e);
cw_waves cw__water_waves(const water3d *d);
const cw_node_type *cw__water_type(void);
int cw__water_quality(void);
/* buoyancy.c */
void cw__register_buoyancy_type(void);
int cw__buoyancy_count(cw_tree *t);

#endif
