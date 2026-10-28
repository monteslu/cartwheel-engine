/* Navmesh input geometry: triangle soups from arrays, three.c object trees
 * and physics worlds (nav.h). Everything is appended in a fixed order (array
 * order, tree order, body creation order), so the same scene gives the same
 * soup and so the same navmesh on every host. */
#include <float.h>

#include "nav_internal.h"
#include "../physics/physics3d_internal.h"

static int add_verts(cw_nav_geom *g, int n) {
  int base = g->verts.count / 3;
  cw_array_reserve(&g->verts, g->verts.count + n * 3);
  return base;
}

static void push_vert(cw_nav_geom *g, t3_vec3 p) {
  g->verts.data[g->verts.count++] = p.x;
  g->verts.data[g->verts.count++] = p.y;
  g->verts.data[g->verts.count++] = p.z;
}

static void push_tri(cw_nav_geom *g, int a, int b, int c, uint8_t area) {
  cw_array_push(&g->tris, a);
  cw_array_push(&g->tris, b);
  cw_array_push(&g->tris, c);
  cw_array_push(&g->areas, area);
}

void cw_nav_geom_add_tris(cw_nav_geom *g, const float *verts, int vert_count, const int32_t *idx, int tri_count,
                          const t3_mat4 *xf, uint8_t area) {
  if (!g || !verts || vert_count <= 0 || tri_count <= 0) return;
  int base = add_verts(g, vert_count);
  for (int i = 0; i < vert_count; i++) {
    t3_vec3 p = { verts[i * 3], verts[i * 3 + 1], verts[i * 3 + 2] };
    push_vert(g, xf ? t3_vec3_apply_mat4(p, xf) : p);
  }
  /* a transform that mirrors (negative determinant) flips the winding back, so up stays up */
  bool flip = false;
  if (xf) {
    const float *m = xf->e;
    float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
    flip = det < 0;
  }
  for (int t = 0; t < tri_count; t++) {
    int a = idx ? idx[t * 3] : t * 3, b = idx ? idx[t * 3 + 1] : t * 3 + 1, c = idx ? idx[t * 3 + 2] : t * 3 + 2;
    if (a < 0 || b < 0 || c < 0 || a >= vert_count || b >= vert_count || c >= vert_count) continue;
    if (flip) push_tri(g, base + a, base + c, base + b, area);
    else push_tri(g, base + a, base + b, base + c, area);
  }
}

void cw_nav_geom_add_box(cw_nav_geom *g, t3_vec3 c, t3_vec3 h, uint8_t area) {
  float v[24];
  for (int i = 0; i < 8; i++) {
    v[i * 3] = c.x + ((i & 1) ? h.x : -h.x);
    v[i * 3 + 1] = c.y + ((i & 2) ? h.y : -h.y);
    v[i * 3 + 2] = c.z + ((i & 4) ? h.z : -h.z);
  }
  /* counter-clockwise seen from outside (three.js winding: +y faces up) */
  static const int32_t idx[36] = {
    2, 6, 7, 2, 7, 3, /* +y */
    0, 1, 5, 0, 5, 4, /* -y */
    1, 3, 7, 1, 7, 5, /* +x */
    0, 4, 6, 0, 6, 2, /* -x */
    4, 5, 7, 4, 7, 6, /* +z */
    0, 2, 3, 0, 3, 1, /* -z */
  };
  cw_nav_geom_add_tris(g, v, 8, idx, 12, NULL, area);
}

/* ── three.c objects ─────────────────────────────────────────────────── */

void cw__nav_geom_add_geometry(cw_nav_geom *g, const t3_geometry *geo, const t3_mat4 *xf, uint8_t area) {
  if (!geo) return;
  const t3_attribute *pos = geo->attributes[T3_ATTR_POSITION];
  if (!pos || pos->type != T3_FLOAT32 || pos->item_size < 3 || pos->count <= 0) return;
  const float *src = (const float *)pos->array;
  float *v = cw_alloc(sizeof(float) * 3 * (size_t)pos->count, CW_MEM_NAV);
  for (int i = 0; i < pos->count; i++) {
    v[i * 3] = src[i * pos->item_size];
    v[i * 3 + 1] = src[i * pos->item_size + 1];
    v[i * 3 + 2] = src[i * pos->item_size + 2];
  }
  const t3_attribute *ix = geo->index;
  if (ix && ix->count >= 3) {
    int nt = ix->count / 3;
    int32_t *idx = cw_alloc(sizeof(int32_t) * 3 * (size_t)nt, CW_MEM_NAV);
    for (int i = 0; i < nt * 3; i++)
      idx[i] = ix->type == T3_UINT16 ? ((const uint16_t *)ix->array)[i]
             : ix->type == T3_UINT32 ? (int32_t)((const uint32_t *)ix->array)[i]
                                      : (int32_t)((const float *)ix->array)[i];
    cw_nav_geom_add_tris(g, v, pos->count, idx, nt, xf, area);
    cw_free(idx);
  } else {
    cw_nav_geom_add_tris(g, v, pos->count, NULL, pos->count / 3, xf, area);
  }
  cw_free(v);
}

static void add_object(cw_nav_geom *g, const t3_object *o, const t3_mat4 *xf, uint8_t area) {
  if (o->type == T3_MESH) cw__nav_geom_add_geometry(g, ((const t3_mesh *)o)->geometry, xf, area);
  for (int i = 0; i < o->child_count; i++) {
    const t3_object *c = o->children[i];
    t3_mat4 local, world;
    t3_mat4_compose(&local, c->position, c->quaternion, c->scale);
    t3_mat4_multiply(&world, xf, &local);
    add_object(g, c, &world, area);
  }
}

void cw_nav_geom_add_object(cw_nav_geom *g, const t3_object *root, const t3_mat4 *xf, uint8_t area) {
  if (!g || !root) return;
  add_object(g, root, xf ? xf : &root->matrix_world, area);
}

/* ── physics worlds ──────────────────────────────────────────────────── */

/* A body-local point in world space (floats: the navmesh is single precision). */
static b3Vec3 world_point(b3WorldTransform xf, b3Vec3 p) {
  b3Pos w = b3TransformWorldPoint(xf, p);
  return (b3Vec3){ (float)w.x, (float)w.y, (float)w.z };
}

static void add_hull(cw_nav_geom *g, const b3HullData *hull, b3WorldTransform xf, uint8_t area) {
  const b3Vec3 *pts = b3GetHullPoints(hull);
  const b3HullFace *faces = b3GetHullFaces(hull);
  const b3HullHalfEdge *edges = b3GetHullEdges(hull);
  if (!pts || !faces || !edges) return;
  int base = add_verts(g, hull->vertexCount);
  for (int i = 0; i < hull->vertexCount; i++) {
    b3Vec3 p = world_point(xf, pts[i]);
    push_vert(g, (t3_vec3){ p.x, p.y, p.z });
  }
  /* each face is a convex loop of half edges: a fan */
  for (int f = 0; f < hull->faceCount; f++) {
    int e0 = faces[f].edge, e = edges[e0].next;
    int first = edges[e0].origin;
    while (edges[e].next != e0) {
      push_tri(g, base + first, base + edges[e].origin, base + edges[edges[e].next].origin, area);
      e = edges[e].next;
    }
  }
}

static void add_mesh_shape(cw_nav_geom *g, b3Mesh mesh, b3WorldTransform xf, uint8_t area) {
  const b3MeshData *d = mesh.data;
  if (!d) return;
  const b3Vec3 *v = b3GetMeshVertices(d);
  const b3MeshTriangle *t = b3GetMeshTriangles(d);
  if (!v || !t) return;
  int base = add_verts(g, d->vertexCount);
  for (int i = 0; i < d->vertexCount; i++) {
    b3Vec3 p = { v[i].x * mesh.scale.x, v[i].y * mesh.scale.y, v[i].z * mesh.scale.z };
    p = world_point(xf, p);
    push_vert(g, (t3_vec3){ p.x, p.y, p.z });
  }
  bool flip = mesh.scale.x * mesh.scale.y * mesh.scale.z < 0;
  for (int i = 0; i < d->triangleCount; i++) {
    if (flip) push_tri(g, base + t[i].index1, base + t[i].index3, base + t[i].index2, area);
    else push_tri(g, base + t[i].index1, base + t[i].index2, base + t[i].index3, area);
  }
}

/* A sphere or capsule as an 8-sided prism around its axis: enough for the
 * navmesh to route around it (agents keep their radius off it anyway). */
static void add_round(cw_nav_geom *g, b3Vec3 a, b3Vec3 b, float r, b3WorldTransform xf, uint8_t area) {
  static const float cs[8][2] = { { 1, 0 }, { 0.70710678f, 0.70710678f }, { 0, 1 }, { -0.70710678f, 0.70710678f },
                                  { -1, 0 }, { -0.70710678f, -0.70710678f }, { 0, -1 }, { 0.70710678f, -0.70710678f } };
  b3Vec3 pa = world_point(xf, a), pb = world_point(xf, b);
  float y0 = (pa.y < pb.y ? pa.y : pb.y) - r, y1 = (pa.y > pb.y ? pa.y : pb.y) + r;
  float x0 = pa.x < pb.x ? pa.x : pb.x, x1 = pa.x > pb.x ? pa.x : pb.x;
  float z0 = pa.z < pb.z ? pa.z : pb.z, z1 = pa.z > pb.z ? pa.z : pb.z;
  float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
  float rx = (x1 - x0) * 0.5f + r, rz = (z1 - z0) * 0.5f + r;
  int base = add_verts(g, 16);
  for (int i = 0; i < 8; i++) push_vert(g, (t3_vec3){ cx + cs[i][0] * rx, y0, cz + cs[i][1] * rz });
  for (int i = 0; i < 8; i++) push_vert(g, (t3_vec3){ cx + cs[i][0] * rx, y1, cz + cs[i][1] * rz });
  for (int i = 0; i < 8; i++) {
    int j = (i + 1) & 7;
    push_tri(g, base + i, base + 8 + j, base + j, area);
    push_tri(g, base + i, base + 8 + i, base + 8 + j, area);
  }
  for (int i = 1; i < 7; i++) {
    push_tri(g, base + 8, base + 8 + i + 1, base + 8 + i, area); /* top, facing up */
    push_tri(g, base, base + i, base + i + 1, area);             /* bottom */
  }
}

static void add_height(cw_nav_geom *g, const b3HeightFieldData *hf, b3WorldTransform xf, uint8_t area) {
  if (!hf) return;
  /* Box3D's layout: index = row * columnCount + column, columns along x, rows along z */
  int cx = hf->columnCount, cz = hf->rowCount;
  const uint16_t *h = b3GetHeightFieldCompressedHeights(hf);
  if (!h || cx < 2 || cz < 2) return;
  int base = add_verts(g, cx * cz);
  for (int z = 0; z < cz; z++)
    for (int x = 0; x < cx; x++) {
      float y = hf->minHeight + hf->heightScale * h[z * cx + x];
      b3Vec3 p = { x * hf->scale.x, y * hf->scale.y, z * hf->scale.z };
      p = world_point(xf, p);
      push_vert(g, (t3_vec3){ p.x, p.y, p.z });
    }
  for (int z = 0; z + 1 < cz; z++)
    for (int x = 0; x + 1 < cx; x++) {
      int i = base + z * cx + x;
      push_tri(g, i, i + cx, i + 1, area);
      push_tri(g, i + 1, i + cx, i + cx + 1, area);
    }
}

static int add_shape(cw_nav_geom *g, b3ShapeId s, b3WorldTransform xf, uint8_t area) {
  int before = g->tris.count / 3;
  switch (b3Shape_GetType(s)) {
    case b3_hullShape: add_hull(g, b3Shape_GetHull(s), xf, area); break;
    case b3_meshShape: add_mesh_shape(g, b3Shape_GetMesh(s), xf, area); break;
    case b3_sphereShape: {
      b3Sphere sp = b3Shape_GetSphere(s);
      add_round(g, sp.center, sp.center, sp.radius, xf, area);
      break;
    }
    case b3_capsuleShape: {
      b3Capsule c = b3Shape_GetCapsule(s);
      add_round(g, c.center1, c.center2, c.radius, xf, area);
      break;
    }
    case b3_heightShape: add_height(g, b3Shape_GetHeightField(s), xf, area); break;
    default: break;
  }
  return g->tris.count / 3 - before;
}

int cw_nav_geom_add_world3d(cw_nav_geom *g, cw_world3d w, const cw_phys_filter *filter, uint8_t area) {
  cw__world3 *W = cw__w3(w);
  if (!g || !W) return 0;
  uint32_t layers = filter ? filter->layers : CW_PHYS_ALL_LAYERS;
  bool sensors = filter && filter->sensors;
  int added = 0;
  for (int i = 0; i < W->bodies.slot_count; i++) {
    cw__body3 *b = cw_pool_at(&W->bodies, i);
    if (!b) continue;
    b3BodyId bid = b3LoadBodyId(b->id);
    if (!b3Body_IsValid(bid) || b3Body_GetType(bid) != b3_staticBody) continue;
    if (filter && filter->ignore_body && filter->ignore_body == b->id) continue;
    b3WorldTransform xf = b3Body_GetTransform(bid);
    int n = b3Body_GetShapeCount(bid);
    if (n <= 0) continue;
    b3ShapeId *ids = cw_alloc(sizeof *ids * (size_t)n, CW_MEM_NAV);
    n = b3Body_GetShapes(bid, ids, n);
    for (int k = 0; k < n; k++) {
      b3Filter f = b3Shape_GetFilter(ids[k]);
      if (b3Shape_IsSensor(ids[k]) && !sensors) continue;
      if (!(f.categoryBits & layers)) continue;
      if (filter && filter->fn && !filter->fn(b3StoreShapeId(ids[k]), filter->ctx)) continue;
      added += add_shape(g, ids[k], xf, area);
    }
    cw_free(ids);
  }
  return added;
}

bool cw_nav_geom_bounds(const cw_nav_geom *g, t3_vec3 *bmin, t3_vec3 *bmax) {
  if (!g || g->verts.count < 3) return false;
  t3_vec3 mn = { FLT_MAX, FLT_MAX, FLT_MAX }, mx = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
  for (int i = 0; i < g->verts.count; i += 3) {
    const float *p = &g->verts.data[i];
    mn.x = p[0] < mn.x ? p[0] : mn.x, mx.x = p[0] > mx.x ? p[0] : mx.x;
    mn.y = p[1] < mn.y ? p[1] : mn.y, mx.y = p[1] > mx.y ? p[1] : mx.y;
    mn.z = p[2] < mn.z ? p[2] : mn.z, mx.z = p[2] > mx.z ? p[2] : mx.z;
  }
  if (bmin) *bmin = mn;
  if (bmax) *bmax = mx;
  return true;
}

void cw_nav_geom_clear(cw_nav_geom *g) {
  cw_array_clear(&g->verts);
  cw_array_clear(&g->tris);
  cw_array_clear(&g->areas);
  cw_array_clear(&g->links);
}

void cw_nav_geom_free(cw_nav_geom *g) {
  cw_array_free(&g->verts);
  cw_array_free(&g->tris);
  cw_array_free(&g->areas);
  cw_array_free(&g->links);
}

uint32_t cw_nav_geom_add_link(cw_nav_geom *g, const cw_nav_link *link) {
  if (!g || !link) return 0;
  cw_array_push(&g->links, *link);
  return (uint32_t)g->links.count;
}
