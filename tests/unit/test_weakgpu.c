/* unit_weakgpu: the opt-in pieces for weak GPUs that need no picture to check.
 *   vertex_cache     cw_geometry_optimize_vertex_cache on a grid whose triangles were shuffled: fewer vertices
 *                    shaded per triangle, the same set of triangles (control: the shuffled order is worse)
 *   full_resolution  cw_quality_full_resolution: Low keeps 100%; off again, Low is back to 67% (the control)
 *   splats           cw_splats: a set holds its capacity and no more, begin clears it
 * Every number folds into unit.checksum, compared across hosts. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/quality.h"
#include "cartwheel/render3d.h"
#include "cartwheel/splats.h"
#include "test.h"

static int tri_cmp(const void *a, const void *b) {
  const uint32_t *x = a, *y = b;
  for (int i = 0; i < 3; i++)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}
/* each triangle's corners rotated so the smallest comes first, then the list sorted: the set, whatever the order */
static uint32_t *tri_set(const t3_geometry *g, int *n) {
  *n = g->index->count / 3;
  uint32_t *t = malloc(sizeof(uint32_t) * 3 * (size_t)*n);
  for (int i = 0; i < *n; i++) {
    uint32_t v[3];
    for (int k = 0; k < 3; k++) v[k] = ((const uint16_t *)g->index->array)[i * 3 + k];
    int m = v[0] <= v[1] && v[0] <= v[2] ? 0 : v[1] <= v[2] ? 1 : 2;
    for (int k = 0; k < 3; k++) t[i * 3 + k] = v[(m + k) % 3];
  }
  qsort(t, (size_t)*n, sizeof(uint32_t) * 3, tri_cmp);
  return t;
}

static void test_vertex_cache(void) {
  t3_geometry *g = t3_plane_geometry_new(10, 10, 40, 40);
  CHECK(g->index && g->index->type == T3_UINT16);
  /* shuffle the triangles (a fixed sequence): a modelling-order mesh at its worst */
  uint16_t *ix = g->index->array;
  int nt = g->index->count / 3;
  uint32_t r = 12345;
  for (int i = nt - 1; i > 0; i--) {
    r = r * 1664525u + 1013904223u;
    int j = (int)(r % (uint32_t)(i + 1));
    for (int k = 0; k < 3; k++) {
      uint16_t s = ix[i * 3 + k];
      ix[i * 3 + k] = ix[j * 3 + k];
      ix[j * 3 + k] = s;
    }
  }
  int n0, n1;
  uint32_t *before = tri_set(g, &n0);
  float a0 = 0, a1 = 0;
  CHECK(cw_geometry_optimize_vertex_cache(g, &a0, &a1));
  uint32_t *after = tri_set(g, &n1);
  CHECK_EQ_INT(n0, n1);
  CHECK(memcmp(before, after, sizeof(uint32_t) * 3 * (size_t)n0) == 0); /* the same triangles */
  CHECK(a0 > 1.5f);                                                     /* control: shuffled, far from the 0.5 a grid allows */
  CHECK(a1 < 0.9f && a1 < a0 * 0.5f);                                   /* reordered: each vertex shaded about once */
  int q0 = (int)(a0 * 1000), q1 = (int)(a1 * 1000);
  FOLD(q0), FOLD(q1);
  CHECK(!cw_geometry_optimize_vertex_cache(NULL, NULL, NULL));
  free(before), free(after);
  t3_release(g);
}

static void test_full_resolution(void) {
  cw_quality_full_resolution(true);
  CHECK(cw_quality_is_full_resolution());
  cw_quality_set(CW_QUALITY_LOW, false);
  int full = cw_gfx_get(CW_GFX_RESOLUTION);
  CHECK_EQ_INT(full, 5); /* 100% */
  cw_quality_full_resolution(false);
  cw_quality_set(CW_QUALITY_LOW, false);
  int low = cw_gfx_get(CW_GFX_RESOLUTION);
  CHECK_EQ_INT(low, 2); /* the control: 67% without it */
  cw_quality_set(CW_QUALITY_HIGH, false);
  FOLD(full), FOLD(low);
}

static void test_splats(void) {
  cw_node root = cw_node_new("Node3D", "Root");
  cw_splats *s = cw_splats_new(root, "Shadows", CW_SPLATS_SHADOW, 3);
  CHECK(s != NULL);
  CHECK(cw_splats_new(root, "None", CW_SPLATS_LIGHT, 0) == NULL);
  cw_splats_begin(s);
  int ok = 0;
  for (int i = 0; i < 5; i++) ok += cw_splats_add(s, (t3_vec3){ (float)i, 0, 0 }, 1, 1, 0, 0, (t3_color){ 1, 1, 1 }, 0.5f);
  cw_splats_end(s);
  CHECK_EQ_INT(ok, 3); /* full at its capacity */
  CHECK_EQ_INT(cw_splats_count(s), 3);
  CHECK_EQ_INT(cw_splats_mesh(s)->count, 3);
  cw_splats_begin(s);
  cw_splats_end(s);
  CHECK_EQ_INT(cw_splats_mesh(s)->count, 0);
  FOLD(ok);
  cw_splats_free(s);
  cw_node_free(root);
}

CW_TEST_SUITE("weakgpu", { "vertex_cache", test_vertex_cache }, { "full_resolution", test_full_resolution }, { "splats", test_splats })
