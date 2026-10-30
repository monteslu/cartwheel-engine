/* Lighting internals (cartwheel/lighting.h) shared by src/render/lighting*.c
 * and the light node types (src/scene/render3d.c). */
#ifndef CW_LIGHTING_INTERNAL_H
#define CW_LIGHTING_INTERNAL_H

#include "cartwheel/lighting.h"
#include "cartwheel/pipeline.h"

/* a DirectionalLight3D's cascades, as its node asks (render3d.c) */
typedef struct cw__csm_cfg {
  int cascades;        /* 1..4 */
  float distance;      /* r187 maxFar: where the last cascade ends (0 = the camera's far) */
  bool fade;           /* r187 CSMShadowNode.fade */
  int size;            /* one cascade's map, texels (quality-adjusted) */
} cw__csm_cfg;

/* render3d.c, each frame from the light's pre_render: the tree's cascaded sun
 * (the first one wins; a frame without a call has none) */
void cw__lighting_csm_light(cw_tree *t, t3_light *sun, const cw__csm_cfg *cfg);
/* render3d.c's light cap check: the kinds the tree clusters (1 omni, 2 spot)
 * and whether a cascaded sun leaves three.c's list */
int cw__lighting_cluster_kinds(cw_tree *t);
bool cw__lighting_takes(cw_tree *t, const t3_object *light);

/* ── cascades (lighting_csm.c) ─────────────────────────────────────── */

typedef struct cw__csm cw__csm;
cw__csm *cw__csm_new(void);
void cw__csm_free(cw__csm *c);
/* fit the cascades to this frame's camera and render the ones whose light
 * matrix or casters changed; false (nothing to sample) when it cannot */
bool cw__csm_update(cw__csm *c, cw_pass_ctx *ctx, t3_light *sun, const cw__csm_cfg *cfg);
/* the "csm" part's values for a draw (csm_part.mjs names them) */
void cw__csm_fill(cw__csm *c, t3_extension_ctx *x);
typedef struct cw__csm_info { int cascades, rendered, size; } cw__csm_info;
void cw__csm_stats(const cw__csm *c, cw__csm_info *out);

/* WebGPU: clear a rect of a Depth32Float view to the far depth with a draw on
 * the frame's encoder (a render pass would clear the whole view) */
void cw__shadow_clear_rect_wgpu(void *view, int x, int y, int w, int h);

/* a shadow map's depth pass: shadow maps stay forward under a reversed depth
 * buffer (a light's short range needs no extra precision, and the shading parts'
 * compares are written for it) */
void cw__render_shadow_depth(t3_renderer *r, t3_scene *scene, t3_camera *cam, const t3_external_target *et);

/* ── the spot / omni shadow atlas (lighting_atlas.c) ───────────────── */

typedef struct cw__shadow_req {
  const t3_light *light;
  bool omni;                /* a 3 x 2 block of cube faces, else one spot tile */
  int size;                 /* wanted texels (a face's for omni), a power of two */
  float priority;           /* bigger first: layout and the update budget */
  float range;              /* the shadow camera's far */
} cw__shadow_req;
typedef struct cw__shadow_out {
  int type;                 /* 0 no shadow (yet), 1 spot, 2 omni */
  float mat[16];            /* spot: world -> atlas uv (before the y flip) and depth */
  t3_vec3 pos;              /* omni: the light where the faces were drawn */
  float near, far;
  float rect[4];            /* texels, rows top-down: x, y, face size */
} cw__shadow_out;
typedef struct cw__atlas_info { int shadowed, faces_rendered, faces_cached; } cw__atlas_info;
typedef struct cw__atlas cw__atlas;
cw__atlas *cw__atlas_new(void);
void cw__atlas_free(cw__atlas *a);
/* lay the tiles out (when the lights or sizes changed), redraw what changed
 * (at most budget faces, most important first) and say what each request's
 * shader reads (out[i]) */
void cw__atlas_update(cw__atlas *a, cw_pass_ctx *ctx, const cw__shadow_req *req, int n, int atlas_size, int budget,
                      bool cache, cw__shadow_out *out);
t3_texture *cw__atlas_texture(cw__atlas *a);
void cw__atlas_get_info(const cw__atlas *a, cw__atlas_info *out);

#endif
