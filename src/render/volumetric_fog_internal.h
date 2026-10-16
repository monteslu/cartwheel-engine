/* Volumetric fog internals shared by the GL path (volumetric_fog.c) and the
 * WebGPU path (volumetric_fog_wgpu.c). */
#ifndef CW_VOLUMETRIC_FOG_INTERNAL_H
#define CW_VOLUMETRIC_FOG_INTERNAL_H

#include "cartwheel/pipeline.h"

#define VF_MAX_LIGHTS 8
#define VF_MAX_VOLUMES 16

/* The froxel passes' uniform block: std140 on GL (a uniform buffer) and the
 * same bytes as WGSL's uniform layout (only mat4 and vec4 members). */
typedef struct vf_light_u {
  float pos[4];    /* xyz: position, or the direction toward a directional light; w: 0 dir, 1 omni, 2 spot */
  float dir[4];    /* xyz: spot axis (the way the light travels); w: range (0 = none) */
  float color[4];  /* rgb: colour x energy x fog_energy; w: decay */
  float cone[4];   /* cos(angle), cos(angle x (1 - penumbra)), has shadow, depth bias */
  float tile[4];   /* the shadow tile in the atlas: uv offset (xy), uv scale (zw) */
  float shadow[16]; /* world -> the light's clip space */
} vf_light_u;
typedef struct vf_vol_u {
  float inv[16];      /* world -> the volume's unit space ([-1, 1] inside) */
  float a[4];         /* density, shape (0 box, 1 ellipsoid, 2 world), edge fade, height falloff */
  float albedo[4];    /* rgb, noise amount */
  float emission[4];  /* rgb x energy, noise frequency */
  float wind[4];      /* noise scroll (world units a second), the volume's lowest world height */
} vf_vol_u;
typedef struct vf_uniforms {
  float inv_view[16]; /* the camera's world matrix */
  float prev_vp[16];  /* last frame's view-projection (temporal reprojection) */
  float grid[4];      /* W, H, D, 1 / D */
  float proj[4];      /* P00, P11, P20, P21 of the (unjittered) projection */
  float slices[4];    /* distance, detail spread, 1 / spread, history weight (0 = none this frame) */
  float jitter[4];    /* this frame's sample offset in a cell (-0.5..0.5), time (s) */
  float medium[4];    /* global density, height falloff, base height, anisotropy g */
  float ambient[4];   /* ambient in-scattered radiance (rgb) */
  float albedo[4];    /* the global medium's albedo (rgb) */
  float counts[4];    /* lights, volumes */
  vf_light_u lights[VF_MAX_LIGHTS];
  vf_vol_u vols[VF_MAX_VOLUMES];
} vf_uniforms;

/* the WGSL / GLSL of the block and the shared functions (volumetric_fog.c) */
extern const char CW__VF_WGSL_COMMON[];

/* ── WebGPU (volumetric_fog_wgpu.c; stubs without CW_WGPU) ──────────── */

typedef struct vf_wg vf_wg;
/* (re)make the volumes (W x H x D) and the shadow atlas (atlas x atlas)
 * when their size changed; true when the volumes are new (no history) */
bool cw__vf_wg_resources(vf_wg **g, int w, int h, int d, int atlas);
/* the shadow atlas's depth view and format, for three.c's depth renders */
void *cw__vf_wg_atlas_view(vf_wg *g, int *format);
/* the frame encoder (closes the engine's open render pass) */
void *cw__vf_wg_encoder(void);
/* the passes: inject into volume `cur` (history = the other), integrate */
void cw__vf_wg_run(vf_wg *g, const vf_uniforms *u, int cur, void *encoder);
/* the integrated volume's id in the pipeline's texture registry */
unsigned cw__vf_wg_integrated(vf_wg *g);
void cw__vf_wg_free(vf_wg *g);

#endif
