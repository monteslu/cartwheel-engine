/* The lightmapper's pieces (lightmap*.c): scalar, deterministic. */
#ifndef CW_LIGHTMAP_INTERNAL_H
#define CW_LIGHTMAP_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/lightmap.h"

/* ── BVH over world-space triangles (lightmap_bvh.c) ──────────────── */

typedef struct lm_tri {
  float v0[3], e1[3], e2[3];  /* vertex 0 and the two edges */
  int mesh, tri;              /* owner mesh index, triangle index within it */
} lm_tri;

typedef struct lm_node {
  float lo[3], hi[3];
  int left;                   /* child index (right = left + 1), or -1 for a leaf */
  int first, count;           /* leaf: triangles [first, first + count) */
} lm_node;

typedef struct lm_bvh {
  lm_tri *tris;               /* reordered by the build */
  int tri_count;
  lm_node *nodes;
  int node_count;
} lm_bvh;

/* builds over `tris` (taken over; freed with the BVH) */
void lm_bvh_build(lm_bvh *b, lm_tri *tris, int n);
void lm_bvh_free(lm_bvh *b);

typedef struct lm_hit {
  float t, u, v;              /* distance, barycentrics of v1 and v2 */
  int tri;                    /* index into b->tris, -1 = none */
  bool back;                  /* hit the triangle's back face */
} lm_hit;

/* closest hit in (tmin, tmax) */
bool lm_bvh_closest(const lm_bvh *b, const float o[3], const float d[3], float tmin, float tmax, lm_hit *hit);
/* any hit in (tmin, tmax) */
bool lm_bvh_any(const lm_bvh *b, const float o[3], const float d[3], float tmin, float tmax);

/* ── UV2 charts (lightmap_uv.c) ──────────────────────────────────────── */

typedef struct lm_unwrap {
  int tri_count;
  float *uv2;                 /* 6 floats a triangle (3 corners), in [0, 1] of the lightmap */
  float *tex;                 /* the same in texels (for rasterizing) */
  int width, height;
  int charts;
} lm_unwrap;

/* Charts and packs the triangles (world positions `pos`, 9 floats each;
 * world normals `nrm`, 9 floats each) at `density` texels per unit, with
 * `padding` texels around charts, lightmap side within [min_size, max_size]
 * (the density is lowered, and logged, when the charts would not fit). */
void lm_unwrap_mesh(lm_unwrap *u, const float *pos, const float *nrm, int tri_count, float density, int padding,
                    int min_size, int max_size);
void lm_unwrap_free(lm_unwrap *u);

/* ── half floats ─────────────────────────────────────────────────────── */
uint16_t lm_float_to_half(float f);
float lm_half_to_float(uint16_t h);

#endif
