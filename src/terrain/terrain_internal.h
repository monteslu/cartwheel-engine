/* Terrain internals (cartwheel/terrain.h): the data and physics
 * (terrain.c), the CDLOD quadtree and selection (cdlod.c), the GPU side
 * (terrain_render.c), scatter (scatter.c) and the nodes (terrain_node.c). */
#ifndef CW_TERRAIN_INTERNAL_H
#define CW_TERRAIN_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/terrain.h"

#define TR_TILE 512                /* layer texture size in the atlases */
#define TR_GUTTER (TR_TILE / 8)    /* wrapped border around each tile */
#define TR_CELL (TR_TILE + 2 * TR_GUTTER)
#define TR_ATLAS_COLS 3            /* 3 x 3 layer cells (8 used), albedo cells then normal cells */
#define TR_ATLAS_W (TR_CELL * TR_ATLAS_COLS * 2)
#define TR_ATLAS_H (TR_CELL * TR_ATLAS_COLS)
#define TR_MAX_SCATTER 16
#define TR_SCATTER_CELL 32.0f      /* metres */

typedef struct tr_region {
  cw_heightfield3d field;
  cw_body3d body;
  cw_shape3d shape;
  bool dirty;
} tr_region;

/* one selected patch: a quadtree node (or a quarter of one) drawn at a level */
typedef struct tr_patch {
  int32_t x, z;      /* first sample */
  int32_t size;      /* samples covered (quads) */
  int16_t lod;       /* level whose spacing and morph it draws with */
  uint8_t half;      /* a quarter of a node: drawn with the half grid */
  uint8_t far;       /* wholly past far_distance */
  float dist;        /* nearest distance from the camera to its box (sorting) */
} tr_patch;

typedef CW_ARRAY(tr_patch) tr_patch_array;

typedef struct tr_scatter tr_scatter; /* scatter.c */

struct cw_terrain {
  cw_terrain_def def;
  int nx, nz;              /* samples: size + 1 */
  uint16_t *h;             /* nx * nz */
  uint32_t *ctl;           /* nx * nz */
  float sy;                /* metres per height step */

  /* physics */
  int rnx, rnz;
  tr_region *regions;
  cw_world3d world;
  cw_shape3d_def shape_def;
  bool physics;

  /* what edits made stale: GPU rectangles (samples, inclusive) and flags */
  int dirty_h[4], dirty_c[4]; /* x0, z0, x1, z1; x0 > x1 = clean */
  bool tree_dirty, normals_dirty, macro_dirty, layers_dirty;

  /* quadtree: per level, the min / max height step of each node */
  int lods;
  int lnx[CW_TERRAIN_MAX_LODS], lnz[CW_TERRAIN_MAX_LODS];
  uint16_t *lmin[CW_TERRAIN_MAX_LODS], *lmax[CW_TERRAIN_MAX_LODS];
  float range[CW_TERRAIN_MAX_LODS]; /* metres each level reaches */
  float range_scale;                /* quality's patch range factor */

  /* selection: per root chunk outputs, merged in order */
  tr_patch_array *chunk_out;
  int *chunk_counts; /* visited, culled per chunk */
  int chunk_cap;
  tr_patch_array patches;

  /* layers */
  struct {
    uint8_t *albedo, *normal; /* TR_TILE^2 RGBA8, NULL = default */
    float uv_scale;
    float mean[3];            /* linear mean albedo, for the macro bake */
    bool set;
  } layer[CW_TERRAIN_LAYERS];

  /* GPU */
  void *gpu;                  /* terrain_render.c */
  int quality;                /* -1 = cw_quality_get() */

  /* scatter */
  tr_scatter *scatter[TR_MAX_SCATTER];

  t3_vec3 camera;
  cw_terrain_stats stats;
};

/* terrain.c */
static inline int tr_idx(const cw_terrain *t, int x, int z) { return z * t->nx + x; }
static inline float tr_height_m(const cw_terrain *t, uint16_t q) { return t->def.origin.y + t->def.min_height + (float)q * t->sy; }
void cw__terrain_mark(cw_terrain *t, int x0, int z0, int x1, int z1, bool heights, bool control);
/* the height of the drawn / collided triangles at sample-space (fx, fz), metres (render use: scatter) */
float cw__terrain_height_fast(const cw_terrain *t, float fx, float fz);
t3_vec3 cw__terrain_normal_fast(const cw_terrain *t, float fx, float fz);

/* cdlod.c */
void cw__terrain_tree_init(cw_terrain *t);
void cw__terrain_tree_free(cw_terrain *t);
void cw__terrain_tree_rebuild(cw_terrain *t); /* min / max pyramid of the dirty part (all, for now) */
void cw__terrain_ranges(cw_terrain *t);
void cw__terrain_select(cw_terrain *t, t3_vec3 cam, const t3_mat4 *vp);

/* terrain_render.c */
void cw__terrain_gpu_free(cw_terrain *t);
void cw__terrain_gpu_update(cw_terrain *t);
void cw__terrain_gpu_draw_setup(cw_terrain *t);
void cw__terrain_gpu_set_origin(cw_terrain *t);
int cw__terrain_quality(const cw_terrain *t);
float cw__terrain_far(const cw_terrain *t);

/* scatter.c */
void cw__scatter_free(tr_scatter *s);
void cw__scatter_update(cw_terrain *t, t3_vec3 cam, const t3_mat4 *vp);
void cw__scatter_attach(cw_terrain *t, tr_scatter *s);

/* ── SIMD (GCC / clang vector extensions: SSE natively, simd128 in carts) ── */
typedef float tr_v4f __attribute__((vector_size(16)));
typedef int32_t tr_v4i __attribute__((vector_size(16)));
typedef uint16_t tr_u16x8 __attribute__((vector_size(16)));
/* blends by mask (compares give all-ones lanes): C has no vector ?: */
static inline tr_u16x8 tr_u16_min(tr_u16x8 a, tr_u16x8 b) { tr_u16x8 m = (tr_u16x8)(a < b); return (a & m) | (b & ~m); }
static inline tr_u16x8 tr_u16_max(tr_u16x8 a, tr_u16x8 b) { tr_u16x8 m = (tr_u16x8)(a > b); return (a & m) | (b & ~m); }
static inline tr_v4f tr_v4_max(tr_v4f a, tr_v4f b) {
  tr_v4i m = (tr_v4i)(a > b);
  return (tr_v4f)(((tr_v4i)a & m) | ((tr_v4i)b & ~m));
}

#include <time.h>
static inline double tr_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* the scene node (terrain_node.c) */
t3_scene *cw__terrain_scene(cw_terrain *t);

#endif
