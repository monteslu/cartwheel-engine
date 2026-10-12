/* Visibility module internals: the BVH (bvh.c), the per-tree culling state
 * (visibility.c) and impostors (impostor.c). */
#ifndef CW_VIS_INTERNAL_H
#define CW_VIS_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/gl.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/visibility.h"
#include "scene/render3d_internal.h"

/* ── frustum ──────────────────────────────────────────────────────── */

#define VIS_MAX_PLANES 12
typedef struct vis_frustum {
  float nx[VIS_MAX_PLANES], ny[VIS_MAX_PLANES], nz[VIS_MAX_PLANES], d[VIS_MAX_PLANES];
  int n;
} vis_frustum;
/* the six planes of a view-projection matrix (inside: n.p + d >= 0) */
void cw__vis_frustum_from(vis_frustum *f, const t3_mat4 *vp);
/* box against planes: 0 outside, 1 intersecting, 2 inside every plane;
 * sweep (may be NULL): the box is also swept by that vector (shadow casters) */
int cw__vis_box_test(const vis_frustum *f, const float mn[3], const float mx[3], const float *sweep);

/* ── BVH4 over static boxes ─────────────────────────────────────────── */

typedef struct vis_bvh_node {
  float minx[4], miny[4], minz[4], maxx[4], maxy[4], maxz[4];
  int32_t child[4]; /* >= 0: node; < 0: leaf with first prim -child - 1 */
  int32_t count[4]; /* leaf prim count; 0 = empty slot */
  int32_t lo, hi;   /* the prims of this subtree: prims[lo, hi) */
} vis_bvh_node;

typedef struct vis_bvh {
  CW_ARRAY(vis_bvh_node) nodes;
  CW_ARRAY(int32_t) prims; /* record indices, leaves are ranges of this */
  int root;                /* -1 when empty */
  float rmin[3], rmax[3];  /* root bounds */
} vis_bvh;

/* Build over n boxes (mins / maxs as xyz triples) with record ids ids[i]. */
void cw__vis_bvh_build(vis_bvh *b, int n, const float *mins, const float *maxs, const int32_t *ids);
/* Refit every node from the boxes (indexed by record id), after static
 * meshes moved. */
void cw__vis_bvh_refit(vis_bvh *b, const float *rec_min, const float *rec_max, int stride);
void cw__vis_bvh_free(vis_bvh *b);
/* Mark out[id] = 1 (intersecting) or 2 (inside) for every prim whose box
 * passes the frustum (swept by sweep when not NULL), walking the top of the
 * tree on this thread and the subtrees on the job pool. Returns nodes
 * visited. */
int cw__vis_bvh_cull(const vis_bvh *b, const vis_frustum *f, const float *sweep, uint8_t *out);

/* ── impostors (impostor.c) ─────────────────────────────────────────── */

typedef struct vis_impostor_inst {
  float center[3], radius;
  float quat[4]; /* the object's world rotation */
  float alpha;   /* 1 = opaque (the mesh is hidden), < 1 = fading in over the mesh */
} vis_impostor_inst;

typedef struct vis_impostors vis_impostors;
vis_impostors *cw__vis_impostors_new(void);
void cw__vis_impostors_free(vis_impostors *im);
/* The atlas for a look (baked on first request: call from a pass with the
 * renderer, before the scene draws). src: the node whose look it is. Returns
 * an atlas index, -1 when it cannot be baked. */
int cw__vis_impostor_atlas(vis_impostors *im, cw_pass_ctx *ctx, cw_node src, const cw__mesh_info *info,
                           float *radius, float center_local[3]);
/* queue an instance for this frame */
void cw__vis_impostor_add(vis_impostors *im, int atlas, const vis_impostor_inst *inst);
void cw__vis_impostors_begin_frame(vis_impostors *im);
/* the OPAQUE-phase draw; returns draw calls */
int cw__vis_impostors_draw(vis_impostors *im, cw_pass_ctx *ctx);
int cw__vis_impostor_atlas_count(const vis_impostors *im);

#endif
