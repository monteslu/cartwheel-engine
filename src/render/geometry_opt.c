/* cw_geometry_optimize_vertex_cache (cartwheel/render3d.h): meshoptimizer's vertex cache optimisation on a
 * geometry's index buffer. */
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/render3d.h"
#include "meshoptimizer.h"

bool cw_geometry_optimize_vertex_cache(t3_geometry *g, float *acmr_before, float *acmr_after) {
  if (acmr_before) *acmr_before = 0;
  if (acmr_after) *acmr_after = 0;
  if (!g || !g->index || !g->attributes[T3_ATTR_POSITION]) return false;
  t3_attribute *ia = g->index;
  int n = ia->count, verts = g->attributes[T3_ATTR_POSITION]->count;
  if (n < 3 || n % 3 || verts <= 0 || (ia->type != T3_UINT16 && ia->type != T3_UINT32)) return false;
  unsigned *idx = cw_alloc(sizeof(unsigned) * (size_t)n, CW_MEM_RENDER), *out = cw_alloc(sizeof(unsigned) * (size_t)n, CW_MEM_RENDER);
  for (int i = 0; i < n; i++) idx[i] = ia->type == T3_UINT16 ? ((const uint16_t *)ia->array)[i] : ((const uint32_t *)ia->array)[i];
  for (int i = 0; i < n; i++)
    if (idx[i] >= (unsigned)verts) { /* an index past the vertices: leave the geometry as it is */
      cw_free(idx), cw_free(out);
      return false;
    }
  /* a 16-entry FIFO with no warp grouping: a typical post-transform cache, for the report only */
  if (acmr_before) *acmr_before = meshopt_analyzeVertexCache(idx, (size_t)n, (size_t)verts, 16, 0, 0).acmr;
  meshopt_optimizeVertexCache(out, idx, (size_t)n, (size_t)verts);
  if (acmr_after) *acmr_after = meshopt_analyzeVertexCache(out, (size_t)n, (size_t)verts, 16, 0, 0).acmr;
  for (int i = 0; i < n; i++) {
    if (ia->type == T3_UINT16) ((uint16_t *)ia->array)[i] = (uint16_t)out[i];
    else ((uint32_t *)ia->array)[i] = out[i];
  }
  ia->version++;
  cw_free(idx), cw_free(out);
  return true;
}
