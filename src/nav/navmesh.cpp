/* The navmesh: Recast builds tiles, Detour holds them and answers queries,
 * DetourTileCache rebuilds the tiles obstacles touch. This file is the C++
 * side of cartwheel/nav.h; the crowd, input geometry, nodes and debug draw
 * are C (crowd.c, geom.c, nodes.c, debug.c) over nav_internal.h.
 *
 * Determinism (see nav.h): every tile builds from the triangles binned to it
 * in index order, on any pool thread, with its own rcContext and heightfields;
 * the results join the navmesh in (z, x, layer) order on the calling thread,
 * so tile refs, salts and links are the same however the builds were spread.
 * Recast's sorting, trigonometry and allocator are patched to be the same on
 * every C library (patches/recastnavigation). */
#include <float.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <new>

#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourTileCache.h"
#include "DetourTileCacheBuilder.h"
#include "DetourCommon.h"

#include "cartwheel/jobs.h"
#include "nav_internal.h"

static_assert(sizeof(dtPolyRef) == sizeof(cw__poly), "32-bit Detour polygon refs");

/* ── filters ──────────────────────────────────────────────────────────── */

namespace {

struct Filter : dtQueryFilter {
  uint64_t exclude = 0;
  const cw_nav_link *links = nullptr; /* the navmesh's link table: a link's own cost */
  int nlinks = 0;
  bool passFilter(const dtPolyRef, const dtMeshTile *, const dtPoly *poly) const override {
    return poly->flags != 0 && !((exclude >> poly->getArea()) & 1);
  }
  float getCost(const float *pa, const float *pb, const dtPolyRef, const dtMeshTile *, const dtPoly *, const dtPolyRef,
                const dtMeshTile *curTile, const dtPoly *curPoly, const dtPolyRef, const dtMeshTile *,
                const dtPoly *) const override {
    const float area_cost = getAreaCost(curPoly->getArea());
    if (curPoly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION && nlinks > 0) {
      unsigned idx = (unsigned)(curPoly - curTile->polys) - (unsigned)curTile->header->offMeshBase;
      if (idx < (unsigned)curTile->header->offMeshConCount) {
        unsigned id = curTile->offMeshCons[idx].userId;
        if (id >= 1 && id <= (unsigned)nlinks && links[id - 1].cost > 0) return links[id - 1].cost * area_cost;
      }
    }
    return dtVdist(pa, pb) * area_cost;
  }
};

void make_filter(Filter &f, const cw_nav_filter *src) {
  f.setIncludeFlags(0xffff);
  f.setExcludeFlags(0);
  if (!src) return; /* dtQueryFilter's costs start at 1 */
  f.exclude = src->exclude_areas;
  for (int i = 0; i < CW_NAV_AREAS; i++) f.setAreaCost(i, src->area_cost[i] < 1 ? 1 : src->area_cost[i]);
}

/* ── the tile cache's helpers ─────────────────────────────────────────── */

/* Layers are stored as they are: the memory is small next to the time a
 * compressor would cost every obstacle change. */
struct NoCompressor : dtTileCacheCompressor {
  int maxCompressedSize(const int n) override { return n; }
  dtStatus compress(const unsigned char *buf, const int n, unsigned char *out, const int, int *outn) override {
    memcpy(out, buf, (size_t)n);
    *outn = n;
    return DT_SUCCESS;
  }
  dtStatus decompress(const unsigned char *in, const int n, unsigned char *buf, const int maxn, int *outn) override {
    if (n > maxn) return DT_FAILURE | DT_BUFFER_TOO_SMALL;
    memcpy(buf, in, (size_t)n);
    *outn = n;
    return DT_SUCCESS;
  }
};

struct NavMesh;
void tile_links(const NavMesh *n, const cw_nav_link *links, int nlinks, int tx, int ty, dtNavMeshCreateParams *p);

/* Walkable polygons get flag 1: a polygon with flags 0 is never entered.
 * The links that start in the tile join it. */
struct MeshProcess : dtTileCacheMeshProcess {
  const NavMesh *n = nullptr;
  void process(dtNavMeshCreateParams *params, unsigned char *areas, unsigned short *flags) override;
};

/* Links for one tile's dtNavMeshCreateParams; one per pool thread, kept until that thread's next tile. */
struct LinkScratch {
  CW_ARRAY(float) verts = {}, rad = {};
  CW_ARRAY(unsigned short) flags = {};
  CW_ARRAY(unsigned char) areas = {}, dir = {};
  CW_ARRAY(unsigned int) ids = {};
  void clear() {
    cw_array_clear(&verts), cw_array_clear(&rad), cw_array_clear(&flags);
    cw_array_clear(&areas), cw_array_clear(&dir), cw_array_clear(&ids);
  }
  void free_all() {
    cw_array_free(&verts), cw_array_free(&rad), cw_array_free(&flags);
    cw_array_free(&areas), cw_array_free(&dir), cw_array_free(&ids);
  }
};

/* A bump allocator per pool thread for tile-cache builds (reset per tile). */
struct LinearAlloc : dtTileCacheAlloc {
  unsigned char *buf = nullptr;
  size_t cap = 0, top = 0;
  CW_ARRAY(void *) big = {}; /* requests the buffer could not hold, freed at reset */
  ~LinearAlloc() override {
    reset();
    cw_free(buf);
    cw_array_free(&big);
  }
  void reset() override {
    top = 0;
    for (int i = 0; i < big.count; i++) cw_free(big.data[i]);
    cw_array_clear(&big);
  }
  void *alloc(const size_t size) override {
    size_t n = (size + 15) & ~(size_t)15;
    if (!buf) {
      cap = 256 * 1024;
      buf = (unsigned char *)cw_alloc(cap, CW_MEM_NAV);
    }
    if (top + n <= cap) {
      void *p = buf + top;
      top += n;
      memset(p, 0, n);
      return p;
    }
    void *p = cw_calloc(1, n, CW_MEM_NAV);
    cw_array_push(&big, p);
    return p;
  }
  void free(void *) override {}
};

/* ── the navmesh ──────────────────────────────────────────────────────── */

struct PathReq {
  uint32_t ticket;
  cw_nav_path_status status;
  float from[3], to[3];
  cw_nav_filter filter;
  bool has_filter;
  int count;
  t3_vec3 *pts;    /* answered: count waypoints */
  uint32_t *links; /* answered: each waypoint's link id (0: walking) */
};

struct Obstacle {
  uint32_t id;
  dtObstacleRef ref;
  uint32_t kind; /* 0 cylinder (base, radius, height), 1 box (center, half, y rotation) */
  float v[7];
};

struct CacheTile { /* a tile-cache layer, before it joins the cache */
  unsigned char *data;
  int size;
};

#define MAX_THREADS 64

struct NavMesh {
  cw_nav_config cfg;
  float bmin[3], bmax[3];
  int tw, th;         /* tile grid */
  float tile_world;   /* tile width (world units) */
  int tile_cells;     /* tile width (cells) */
  int border;         /* cells */
  dtNavMesh *mesh;
  dtTileCache *cache; /* dynamic */
  NoCompressor *comp;
  MeshProcess *mproc;
  LinearAlloc *talloc[MAX_THREADS];
  dtNavMeshQuery *query[MAX_THREADS];
  float extents[3];
  uint32_t version;
  CW_ARRAY(PathReq) reqs; /* request order */
  uint32_t next_ticket;
  CW_ARRAY(Obstacle) obstacles;
  uint32_t next_obstacle;
  CW_ARRAY(cw_crowd) crowds;
  float build_ms;
  CW_ARRAY(cw_nav_link) links; /* the build geometry's, defaults filled in; id = index + 1 */
  LinkScratch lscratch[MAX_THREADS];
};

cw_pool g_meshes;
bool g_init;

NavMesh *get(cw_navmesh h) { return g_init ? (NavMesh *)cw_pool_get(&g_meshes, h.id) : nullptr; }

dtNavMeshQuery *query_of(NavMesh *n) {
  int w = cw_jobs_worker_index();
  CW_ASSERT(w >= 0 && w < MAX_THREADS);
  dtNavMeshQuery *q = n->query[w];
  if (!q) {
    q = dtAllocNavMeshQuery();
    if (!q || dtStatusFailed(q->init(n->mesh, n->cfg.max_search_nodes))) CW_FATAL("nav: out of memory for a query");
    n->query[w] = q;
  }
  return q;
}

/* The geometry's links with their defaults filled in become the table. */
void set_links(NavMesh *n, const cw_nav_geom *g) {
  cw_array_clear(&n->links);
  for (int i = 0; g && i < g->links.count; i++) {
    cw_nav_link l = g->links.data[i];
    if (!l.area) l.area = CW_NAV_AREA_WALK;
    if (!(l.radius > 0)) l.radius = n->cfg.agent_radius;
    if (l.cost < 0) l.cost = 0;
    if (l.duration < 0) l.duration = 0;
    cw_array_push(&n->links, l);
  }
}

void filter_links(Filter &f, const NavMesh *n) {
  f.links = n->links.data;
  f.nlinks = n->links.count;
}

/* The links whose start lies in tile (tx, ty) (its own cells, without the
 * border, so each link is in exactly one tile column), into the create
 * params through the calling pool thread's scratch. Detour keeps the ones
 * whose start is at the tile's (or layer's) height. */
void tile_links(const NavMesh *cn, const cw_nav_link *links, int nlinks, int tx, int ty, dtNavMeshCreateParams *p) {
  p->offMeshConCount = 0;
  if (nlinks <= 0) return;
  NavMesh *n = (NavMesh *)cn;
  int w = cw_jobs_worker_index();
  CW_ASSERT(w >= 0 && w < MAX_THREADS);
  LinkScratch &s = n->lscratch[w];
  s.clear();
  const float x0 = n->bmin[0] + tx * n->tile_world, x1 = n->bmin[0] + (tx + 1) * n->tile_world;
  const float z0 = n->bmin[2] + ty * n->tile_world, z1 = n->bmin[2] + (ty + 1) * n->tile_world;
  for (int i = 0; i < nlinks; i++) {
    const cw_nav_link &l = links[i];
    if (!(l.start.x >= x0 && l.start.x < x1 && l.start.z >= z0 && l.start.z < z1)) continue;
    const float v[6] = { l.start.x, l.start.y, l.start.z, l.end.x, l.end.y, l.end.z };
    for (int k = 0; k < 6; k++) cw_array_push(&s.verts, v[k]);
    cw_array_push(&s.rad, l.radius);
    cw_array_push(&s.flags, (unsigned short)1);
    cw_array_push(&s.areas, l.area);
    cw_array_push(&s.dir, (unsigned char)(l.bidirectional ? 1 : 0));
    cw_array_push(&s.ids, (unsigned int)(i + 1));
  }
  p->offMeshConVerts = s.verts.data;
  p->offMeshConRad = s.rad.data;
  p->offMeshConFlags = s.flags.data;
  p->offMeshConAreas = s.areas.data;
  p->offMeshConDir = s.dir.data;
  p->offMeshConUserID = s.ids.data;
  p->offMeshConCount = s.rad.count;
}

void MeshProcess::process(dtNavMeshCreateParams *params, unsigned char *areas, unsigned short *flags) {
  for (int i = 0; i < params->polyCount; i++) flags[i] = areas[i] != RC_NULL_AREA ? 1 : 0;
  if (n) tile_links(n, n->links.data, n->links.count, params->tileX, params->tileY, params);
}

/* ── per-tile build ───────────────────────────────────────────────────── */

struct TileOut {
  unsigned char *nav; /* static: navmesh tile data */
  int nav_size;
  CacheTile layers[16]; /* dynamic: compressed layers */
  int nlayers;
  bool ok;
};

struct BuildJob {
  NavMesh *n;
  const cw_nav_geom *g;
  const int *bin_start; /* per tile: start in bin_tris; [tiles + 1] */
  const int *bin_tris;  /* triangle indices, ascending per tile */
  const int *tiles;     /* (tx, ty) pairs to build */
  TileOut *out;
};

struct RcScratch { /* frees whatever is still held when a tile build returns */
  rcHeightfield *solid = nullptr;
  rcCompactHeightfield *chf = nullptr;
  rcContourSet *cset = nullptr;
  rcPolyMesh *pmesh = nullptr;
  rcPolyMeshDetail *dmesh = nullptr;
  rcHeightfieldLayerSet *lset = nullptr;
  int *tris = nullptr;
  unsigned char *areas = nullptr;
  ~RcScratch() {
    rcFreeHeightField(solid);
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);
    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);
    rcFreeHeightfieldLayerSet(lset);
    cw_free(tris);
    cw_free(areas);
  }
};

struct Cells { /* the config in voxels */
  int walk_h, walk_climb, walk_r, max_edge, min_area, merge_area;
  float slope, max_err, detail_dist, detail_err;
};

Cells cells_of(const cw_nav_config &c) {
  Cells v;
  v.walk_h = (int)ceilf(c.agent_height / c.cell_height);
  v.walk_climb = (int)floorf(c.agent_max_climb / c.cell_height);
  v.walk_r = (int)ceilf(c.agent_radius / c.cell_size);
  v.max_edge = (int)(c.edge_max_len / c.cell_size);
  v.min_area = (int)(c.region_min_size * c.region_min_size);
  v.merge_area = (int)(c.region_merge_size * c.region_merge_size);
  v.slope = c.agent_max_slope;
  v.max_err = c.edge_max_error;
  v.detail_dist = c.detail_sample_dist < 0.9f ? 0 : c.cell_size * c.detail_sample_dist;
  v.detail_err = c.cell_height * c.detail_sample_max_error;
  return v;
}

/* Rasterizes one tile's triangles into a compact heightfield (both modes). */
bool build_chf(BuildJob *j, int ti, int tx, int ty, rcContext *ctx, RcScratch &s, float tbmin[3], float tbmax[3]) {
  NavMesh *n = j->n;
  const cw_nav_config &c = n->cfg;
  Cells v = cells_of(c);
  const int size = n->tile_cells + n->border * 2;
  tbmin[0] = n->bmin[0] + tx * n->tile_world - n->border * c.cell_size;
  tbmin[1] = n->bmin[1];
  tbmin[2] = n->bmin[2] + ty * n->tile_world - n->border * c.cell_size;
  tbmax[0] = n->bmin[0] + (tx + 1) * n->tile_world + n->border * c.cell_size;
  tbmax[1] = n->bmax[1];
  tbmax[2] = n->bmin[2] + (ty + 1) * n->tile_world + n->border * c.cell_size;
  int t0 = j->bin_start[ti], nt = j->bin_start[ti + 1] - t0;
  if (nt == 0) return false;
  s.solid = rcAllocHeightfield();
  if (!s.solid || !rcCreateHeightfield(ctx, *s.solid, size, size, tbmin, tbmax, c.cell_size, c.cell_height)) return false;
  s.tris = (int *)cw_alloc(sizeof(int) * 3 * (size_t)nt, CW_MEM_NAV);
  s.areas = (unsigned char *)cw_alloc((size_t)nt, CW_MEM_NAV);
  const cw_nav_geom *g = j->g;
  for (int i = 0; i < nt; i++) {
    int t = j->bin_tris[t0 + i];
    memcpy(&s.tris[i * 3], &g->tris.data[t * 3], sizeof(int) * 3);
    s.areas[i] = g->areas.data[t];
  }
  rcClearUnwalkableTriangles(ctx, v.slope, g->verts.data, g->verts.count / 3, s.tris, nt, s.areas);
  if (!rcRasterizeTriangles(ctx, g->verts.data, g->verts.count / 3, s.tris, s.areas, nt, *s.solid, v.walk_climb))
    return false;
  rcFilterLowHangingWalkableObstacles(ctx, v.walk_climb, *s.solid);
  rcFilterLedgeSpans(ctx, v.walk_h, v.walk_climb, *s.solid);
  rcFilterWalkableLowHeightSpans(ctx, v.walk_h, *s.solid);
  s.chf = rcAllocCompactHeightfield();
  if (!s.chf || !rcBuildCompactHeightfield(ctx, v.walk_h, v.walk_climb, *s.solid, *s.chf)) return false;
  rcFreeHeightField(s.solid);
  s.solid = nullptr;
  if (!rcErodeWalkableArea(ctx, v.walk_r, *s.chf)) return false;
  return true;
}

void build_static_tile(BuildJob *j, int ti, int tx, int ty, TileOut *o) {
  NavMesh *n = j->n;
  const cw_nav_config &c = n->cfg;
  Cells v = cells_of(c);
  rcContext ctx(false);
  RcScratch s;
  float tbmin[3], tbmax[3];
  o->ok = true;
  if (!build_chf(j, ti, tx, ty, &ctx, s, tbmin, tbmax)) return;
  bool ok;
  switch (c.partition) {
    case CW_NAV_PARTITION_MONOTONE: ok = rcBuildRegionsMonotone(&ctx, *s.chf, n->border, v.min_area, v.merge_area); break;
    case CW_NAV_PARTITION_LAYERS: ok = rcBuildLayerRegions(&ctx, *s.chf, n->border, v.min_area); break;
    default: ok = rcBuildDistanceField(&ctx, *s.chf) && rcBuildRegions(&ctx, *s.chf, n->border, v.min_area, v.merge_area);
  }
  if (!ok) { o->ok = false; return; }
  s.cset = rcAllocContourSet();
  if (!s.cset || !rcBuildContours(&ctx, *s.chf, v.max_err, v.max_edge, *s.cset)) { o->ok = false; return; }
  if (s.cset->nconts == 0) return;
  s.pmesh = rcAllocPolyMesh();
  if (!s.pmesh || !rcBuildPolyMesh(&ctx, *s.cset, c.verts_per_poly, *s.pmesh)) { o->ok = false; return; }
  if (s.pmesh->npolys == 0) return;
  s.dmesh = rcAllocPolyMeshDetail();
  if (!s.dmesh || !rcBuildPolyMeshDetail(&ctx, *s.pmesh, *s.chf, v.detail_dist, v.detail_err, *s.dmesh)) {
    o->ok = false;
    return;
  }
  for (int i = 0; i < s.pmesh->npolys; i++) s.pmesh->flags[i] = s.pmesh->areas[i] != RC_NULL_AREA ? 1 : 0;
  dtNavMeshCreateParams p;
  memset(&p, 0, sizeof p);
  p.verts = s.pmesh->verts;
  p.vertCount = s.pmesh->nverts;
  p.polys = s.pmesh->polys;
  p.polyAreas = s.pmesh->areas;
  p.polyFlags = s.pmesh->flags;
  p.polyCount = s.pmesh->npolys;
  p.nvp = s.pmesh->nvp;
  p.detailMeshes = s.dmesh->meshes;
  p.detailVerts = s.dmesh->verts;
  p.detailVertsCount = s.dmesh->nverts;
  p.detailTris = s.dmesh->tris;
  p.detailTriCount = s.dmesh->ntris;
  p.walkableHeight = c.agent_height;
  p.walkableRadius = c.agent_radius;
  p.walkableClimb = c.agent_max_climb;
  p.tileX = tx;
  p.tileY = ty;
  p.tileLayer = 0;
  rcVcopy(p.bmin, s.pmesh->bmin);
  rcVcopy(p.bmax, s.pmesh->bmax);
  p.cs = c.cell_size;
  p.ch = c.cell_height;
  p.buildBvTree = true;
  tile_links(n, n->links.data, n->links.count, tx, ty, &p);
  if (!dtCreateNavMeshData(&p, &o->nav, &o->nav_size)) o->ok = false;
}

void build_cache_tile(BuildJob *j, int ti, int tx, int ty, TileOut *o) {
  NavMesh *n = j->n;
  const cw_nav_config &c = n->cfg;
  Cells v = cells_of(c);
  rcContext ctx(false);
  RcScratch s;
  float tbmin[3], tbmax[3];
  o->ok = true;
  if (!build_chf(j, ti, tx, ty, &ctx, s, tbmin, tbmax)) return;
  s.lset = rcAllocHeightfieldLayerSet();
  if (!s.lset || !rcBuildHeightfieldLayers(&ctx, *s.chf, n->border, v.walk_h, *s.lset)) { o->ok = false; return; }
  int nl = s.lset->nlayers < c.max_layers ? s.lset->nlayers : c.max_layers;
  if (nl > (int)CW_ARRAY_LEN(o->layers)) nl = (int)CW_ARRAY_LEN(o->layers);
  for (int i = 0; i < nl; i++) {
    const rcHeightfieldLayer *L = &s.lset->layers[i];
    dtTileCacheLayerHeader h;
    memset(&h, 0, sizeof h); /* its padding is copied into the saved bytes */
    h.magic = DT_TILECACHE_MAGIC;
    h.version = DT_TILECACHE_VERSION;
    h.tx = tx;
    h.ty = ty;
    h.tlayer = i;
    dtVcopy(h.bmin, L->bmin);
    dtVcopy(h.bmax, L->bmax);
    h.width = (unsigned char)L->width;
    h.height = (unsigned char)L->height;
    h.minx = (unsigned char)L->minx;
    h.maxx = (unsigned char)L->maxx;
    h.miny = (unsigned char)L->miny;
    h.maxy = (unsigned char)L->maxy;
    h.hmin = (unsigned short)L->hmin;
    h.hmax = (unsigned short)L->hmax;
    if (dtStatusFailed(dtBuildTileCacheLayer(n->comp, &h, L->heights, L->areas, L->cons, &o->layers[i].data,
                                             &o->layers[i].size))) {
      o->ok = false;
      return;
    }
    o->nlayers++;
  }
}

void build_range(int b, int e, int, void *ctx) {
  BuildJob *j = (BuildJob *)ctx;
  for (int i = b; i < e; i++) {
    int tx = j->tiles[i * 2], ty = j->tiles[i * 2 + 1];
    int ti = ty * j->n->tw + tx;
    if (j->n->cfg.dynamic) build_cache_tile(j, ti, tx, ty, &j->out[i]);
    else build_static_tile(j, ti, tx, ty, &j->out[i]);
  }
}

/* Bins every triangle to the tiles its bounds (plus the border) overlap: a
 * counting sort, so each tile's list is in triangle order. */
void bin_triangles(NavMesh *n, const cw_nav_geom *g, int **start_out, int **tris_out) {
  const int ntiles = n->tw * n->th, ntri = g->tris.count / 3;
  int *start = (int *)cw_calloc((size_t)ntiles + 1, sizeof(int), CW_MEM_NAV);
  int *range = (int *)cw_alloc(sizeof(int) * 4 * (size_t)(ntri ? ntri : 1), CW_MEM_NAV);
  const float pad = n->border * n->cfg.cell_size, tw = n->tile_world;
  for (int t = 0; t < ntri; t++) {
    float mn[2] = { FLT_MAX, FLT_MAX }, mx[2] = { -FLT_MAX, -FLT_MAX };
    for (int k = 0; k < 3; k++) {
      const float *p = &g->verts.data[g->tris.data[t * 3 + k] * 3];
      mn[0] = p[0] < mn[0] ? p[0] : mn[0];
      mx[0] = p[0] > mx[0] ? p[0] : mx[0];
      mn[1] = p[2] < mn[1] ? p[2] : mn[1];
      mx[1] = p[2] > mx[1] ? p[2] : mx[1];
    }
    int x0 = (int)floorf((mn[0] - pad - n->bmin[0]) / tw), x1 = (int)floorf((mx[0] + pad - n->bmin[0]) / tw);
    int z0 = (int)floorf((mn[1] - pad - n->bmin[2]) / tw), z1 = (int)floorf((mx[1] + pad - n->bmin[2]) / tw);
    x0 = x0 < 0 ? 0 : x0;
    z0 = z0 < 0 ? 0 : z0;
    x1 = x1 >= n->tw ? n->tw - 1 : x1;
    z1 = z1 >= n->th ? n->th - 1 : z1;
    int *r = &range[t * 4];
    r[0] = x0, r[1] = x1, r[2] = z0, r[3] = z1;
    for (int z = z0; z <= z1; z++)
      for (int x = x0; x <= x1; x++) start[z * n->tw + x + 1]++;
  }
  for (int i = 0; i < ntiles; i++) start[i + 1] += start[i];
  int *fill = (int *)cw_alloc(sizeof(int) * (size_t)ntiles, CW_MEM_NAV);
  memcpy(fill, start, sizeof(int) * (size_t)ntiles);
  int *tris = (int *)cw_alloc(sizeof(int) * (size_t)(start[ntiles] ? start[ntiles] : 1), CW_MEM_NAV);
  for (int t = 0; t < ntri; t++) {
    const int *r = &range[t * 4];
    for (int z = r[2]; z <= r[3]; z++)
      for (int x = r[0]; x <= r[1]; x++) tris[fill[z * n->tw + x]++] = t;
  }
  cw_free(fill);
  cw_free(range);
  *start_out = start;
  *tris_out = tris;
}

/* Builds the listed tiles (pairs of tx, ty) in parallel; the caller commits out[] in list order. */
TileOut *build_tiles(NavMesh *n, const cw_nav_geom *g, const int *tiles, int count) {
  TileOut *out = (TileOut *)cw_calloc((size_t)(count ? count : 1), sizeof(TileOut), CW_MEM_NAV);
  int *start, *tris;
  bin_triangles(n, g, &start, &tris);
  BuildJob j = { n, g, start, tris, tiles, out };
  cw_parallel_for(count, 1, build_range, &j);
  cw_free(start);
  cw_free(tris);
  return out;
}

/* Builds every tile-cache layer's navmesh tile in parallel, committing in list order. */
struct CacheBuildJob {
  NavMesh *n;
  const dtCompressedTileRef *refs;
  unsigned char **data;
  int *size;
  dtStatus *status;
};

void cache_build_range(int b, int e, int, void *ctx) {
  CacheBuildJob *j = (CacheBuildJob *)ctx;
  int w = cw_jobs_worker_index();
  LinearAlloc *a = j->n->talloc[w];
  if (!a) {
    a = new (cw_alloc(sizeof(LinearAlloc), CW_MEM_NAV)) LinearAlloc();
    j->n->talloc[w] = a;
  }
  for (int i = b; i < e; i++) {
    a->reset();
    j->status[i] = j->n->cache->buildNavMeshTileData(j->refs[i], a, &j->data[i], &j->size[i]);
  }
}

int cache_rebuild(NavMesh *n, const dtCompressedTileRef *refs, int count) {
  if (count <= 0) return 0;
  unsigned char **data = (unsigned char **)cw_calloc((size_t)count, sizeof *data, CW_MEM_NAV);
  int *size = (int *)cw_calloc((size_t)count, sizeof(int), CW_MEM_NAV);
  dtStatus *st = (dtStatus *)cw_calloc((size_t)count, sizeof(dtStatus), CW_MEM_NAV);
  CacheBuildJob j = { n, refs, data, size, st };
  cw_parallel_for(count, 1, cache_build_range, &j);
  for (int i = 0; i < count; i++) {
    if (dtStatusFailed(st[i])) cw_warn("nav: a tile-cache tile failed to build (status %08x)", (unsigned)st[i]);
    n->cache->commitNavMeshTile(refs[i], dtStatusFailed(st[i]) ? nullptr : data[i], size[i], n->mesh);
  }
  cw_free(data);
  cw_free(size);
  cw_free(st);
  n->version++;
  return count;
}

int ilog2(unsigned v) {
  int r = 0;
  while (v >>= 1) r++;
  return r;
}

unsigned next_pow2(unsigned v) {
  v--;
  v |= v >> 1, v |= v >> 2, v |= v >> 4, v |= v >> 8, v |= v >> 16;
  return v + 1;
}

/* The tile grid and the Detour objects for a config and bounds; false (logged) on bad values. */
bool setup(NavMesh *n, const cw_nav_config *c, const float bmin[3], const float bmax[3]) {
  n->cfg = *c;
  if (!(c->cell_size > 0) || !(c->cell_height > 0) || c->verts_per_poly < 3 || c->verts_per_poly > DT_VERTS_PER_POLYGON ||
      c->max_search_nodes <= 0 || c->max_search_nodes > 65535 || c->tile_size < 0) {
    cw_error("nav: bad config (cell sizes > 0, verts_per_poly 3..6, max_search_nodes 1..65535, tile_size >= 0)");
    return false;
  }
  memcpy(n->bmin, bmin, sizeof n->bmin);
  memcpy(n->bmax, bmax, sizeof n->bmax);
  int gw, gh;
  rcCalcGridSize(bmin, bmax, c->cell_size, &gw, &gh);
  int ts = c->tile_size ? c->tile_size : (gw > gh ? gw : gh);
  if (ts < 8) ts = 8;
  Cells v = cells_of(*c);
  n->border = v.walk_r + 3;
  if (c->dynamic && ts + n->border * 2 > 255) {
    cw_error("nav: a dynamic navmesh's tiles are at most %d cells wide (tile_size)", 255 - n->border * 2);
    return false;
  }
  n->tile_cells = ts;
  n->tile_world = ts * c->cell_size;
  n->tw = (gw + ts - 1) / ts;
  n->th = (gh + ts - 1) / ts;
  if (n->tw < 1) n->tw = 1;
  if (n->th < 1) n->th = 1;
  int layers = c->dynamic ? (c->max_layers > 0 ? c->max_layers : 1) : 1;
  int tile_bits = ilog2(next_pow2((unsigned)(n->tw * n->th * layers)));
  if (tile_bits > 14) {
    cw_error("nav: %d x %d tiles (x %d layers) is too many: raise tile_size", n->tw, n->th, layers);
    return false;
  }
  int poly_bits = 22 - tile_bits;
  dtNavMeshParams p;
  memset(&p, 0, sizeof p);
  rcVcopy(p.orig, bmin);
  p.tileWidth = n->tile_world;
  p.tileHeight = n->tile_world;
  p.maxTiles = 1 << tile_bits;
  p.maxPolys = 1 << poly_bits;
  n->mesh = dtAllocNavMesh();
  if (!n->mesh || dtStatusFailed(n->mesh->init(&p))) {
    cw_error("nav: navmesh init failed");
    return false;
  }
  if (c->dynamic) {
    n->comp = new (cw_alloc(sizeof(NoCompressor), CW_MEM_NAV)) NoCompressor();
    n->mproc = new (cw_alloc(sizeof(MeshProcess), CW_MEM_NAV)) MeshProcess();
    n->mproc->n = n;
    n->talloc[0] = new (cw_alloc(sizeof(LinearAlloc), CW_MEM_NAV)) LinearAlloc();
    dtTileCacheParams tp;
    memset(&tp, 0, sizeof tp);
    rcVcopy(tp.orig, bmin);
    tp.cs = c->cell_size;
    tp.ch = c->cell_height;
    tp.width = ts;
    tp.height = ts;
    tp.walkableHeight = c->agent_height;
    tp.walkableRadius = c->agent_radius;
    tp.walkableClimb = c->agent_max_climb;
    tp.maxSimplificationError = c->edge_max_error;
    tp.maxTiles = n->tw * n->th * layers;
    tp.maxObstacles = c->max_obstacles > 0 ? c->max_obstacles : 1;
    n->cache = dtAllocTileCache();
    if (!n->cache || dtStatusFailed(n->cache->init(&tp, n->talloc[0], n->comp, n->mproc))) {
      cw_error("nav: tile cache init failed");
      return false;
    }
  }
  n->extents[0] = 2, n->extents[1] = 4, n->extents[2] = 2;
  n->next_ticket = 1;
  n->next_obstacle = 1;
  return true;
}

void destroy(NavMesh *n) {
  while (n->crowds.count) cw__crowd_free_for_navmesh(n->crowds.data[n->crowds.count - 1]);
  cw_array_free(&n->crowds);
  for (int i = 0; i < MAX_THREADS; i++) {
    dtFreeNavMeshQuery(n->query[i]);
    if (n->talloc[i]) {
      n->talloc[i]->~LinearAlloc();
      cw_free(n->talloc[i]);
    }
  }
  dtFreeTileCache(n->cache); /* before the allocator and compressor it uses */
  if (n->comp) {
    n->comp->~NoCompressor();
    cw_free(n->comp);
  }
  if (n->mproc) {
    n->mproc->~MeshProcess();
    cw_free(n->mproc);
  }
  dtFreeNavMesh(n->mesh);
  for (int i = 0; i < n->reqs.count; i++) cw_free(n->reqs.data[i].pts), cw_free(n->reqs.data[i].links);
  cw_array_free(&n->reqs);
  cw_array_free(&n->obstacles);
  cw_array_free(&n->links);
  for (int i = 0; i < MAX_THREADS; i++) n->lscratch[i].free_all();
}

cw_navmesh alloc_handle(NavMesh **out) {
  if (!g_init) {
    cw_pool_init(&g_meshes, sizeof(NavMesh), CW_MEM_NAV);
    g_init = true;
  }
  uint64_t id;
  *out = (NavMesh *)cw_pool_alloc(&g_meshes, &id);
  return cw_navmesh{ id };
}

void release(cw_navmesh h, NavMesh *n) {
  destroy(n);
  cw_pool_release(&g_meshes, h.id);
}

/* Commits built tiles in list order. */
bool commit_tiles(NavMesh *n, TileOut *out, int count, const int *tiles) {
  bool ok = true;
  for (int i = 0; i < count; i++) {
    TileOut *o = &out[i];
    if (!o->ok) {
      cw_error("nav: tile %d,%d failed to build", tiles[i * 2], tiles[i * 2 + 1]);
      ok = false;
    }
    if (!n->cfg.dynamic) {
      n->mesh->removeTile(n->mesh->getTileRefAt(tiles[i * 2], tiles[i * 2 + 1], 0), nullptr, nullptr);
      if (o->nav && dtStatusFailed(n->mesh->addTile(o->nav, o->nav_size, DT_TILE_FREE_DATA, 0, nullptr))) {
        dtFree(o->nav);
        ok = false;
      }
      continue;
    }
    for (int l = 0; l < o->nlayers; l++) {
      if (dtStatusFailed(n->cache->addTile(o->layers[l].data, o->layers[l].size, DT_COMPRESSEDTILE_FREE_DATA, nullptr))) {
        dtFree(o->layers[l].data);
        ok = false;
      }
    }
  }
  return ok;
}

/* Every tile-cache tile's navmesh tile, in cache-tile order (z, x, layer). */
void cache_build_all(NavMesh *n) {
  CW_ARRAY(dtCompressedTileRef) refs = {};
  for (int ty = 0; ty < n->th; ty++)
    for (int tx = 0; tx < n->tw; tx++) {
      dtCompressedTileRef r[32];
      int k = n->cache->getTilesAt(tx, ty, r, 32);
      /* getTilesAt walks a hash chain: order its answer by layer */
      for (int a = 1; a < k; a++)
        for (int b = a; b > 0 && n->cache->getTileByRef(r[b])->header->tlayer <
                                     n->cache->getTileByRef(r[b - 1])->header->tlayer; b--) {
          dtCompressedTileRef t = r[b];
          r[b] = r[b - 1];
          r[b - 1] = t;
        }
      for (int a = 0; a < k; a++) cw_array_push(&refs, r[a]);
    }
  cache_rebuild(n, refs.data, refs.count);
  cw_array_free(&refs);
}

/* Each link's ends that reached the navmesh: bit 0 the start, bit 1 the end
 * (a link a tile-cache navmesh holds in two layers counts once). */
void link_ends(const NavMesh *n, uint8_t *ends) {
  memset(ends, 0, (size_t)n->links.count);
  const dtNavMesh *m = n->mesh;
  for (int i = 0; i < m->getMaxTiles(); i++) {
    const dtMeshTile *t = m->getTile(i);
    if (!t->header) continue;
    for (int k = 0; k < t->header->offMeshConCount; k++) {
      const dtOffMeshConnection *con = &t->offMeshCons[k];
      if (con->userId < 1 || con->userId > (unsigned)n->links.count) continue;
      const dtPoly *poly = &t->polys[con->poly];
      uint8_t e = 0;
      for (unsigned l = poly->firstLink; l != DT_NULL_LINK; l = t->links[l].next)
        e |= t->links[l].edge == 0 ? 1 : t->links[l].edge == 1 ? 2 : 0;
      ends[con->userId - 1] |= e;
    }
  }
}

/* Names every link that cannot be used (an end off the navmesh). */
void warn_links(const NavMesh *n) {
  if (!n->links.count) return;
  uint8_t *ends = (uint8_t *)cw_alloc((size_t)n->links.count, CW_MEM_NAV);
  link_ends(n, ends);
  for (int i = 0; i < n->links.count; i++) {
    if (ends[i] == 3) continue;
    const cw_nav_link &l = n->links.data[i];
    cw_warn("nav: link %d (%g, %g, %g) -> (%g, %g, %g) is not used: its %s not on the navmesh (within radius %g across, "
            "agent_max_climb %g up or down)",
            i + 1, (double)l.start.x, (double)l.start.y, (double)l.start.z, (double)l.end.x, (double)l.end.y,
            (double)l.end.z, ends[i] == 1 ? "end is" : ends[i] == 2 ? "start is" : "start and end are", (double)l.radius,
            (double)n->cfg.agent_max_climb);
  }
  cw_free(ends);
}

} // namespace

double cw__nav_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

/* ── public: build ────────────────────────────────────────────────────── */

cw_nav_filter cw_nav_filter_default(void) {
  cw_nav_filter f;
  f.exclude_areas = 0;
  for (int i = 0; i < CW_NAV_AREAS; i++) f.area_cost[i] = 1;
  return f;
}

cw_nav_config cw_nav_config_default(void) {
  cw_nav_config c;
  memset(&c, 0, sizeof c);
  c.cell_size = 0.3f;
  c.cell_height = 0.2f;
  c.agent_height = 2.0f;
  c.agent_radius = 0.6f;
  c.agent_max_climb = 0.9f;
  c.agent_max_slope = 45;
  c.tile_size = 48;
  c.region_min_size = 8;
  c.region_merge_size = 20;
  c.edge_max_len = 12;
  c.edge_max_error = 1.3f;
  c.verts_per_poly = 6;
  c.detail_sample_dist = 6;
  c.detail_sample_max_error = 1;
  c.partition = CW_NAV_PARTITION_WATERSHED;
  c.max_obstacles = 256;
  c.max_layers = 8;
  c.max_search_nodes = 2048;
  return c;
}

cw_navmesh cw_navmesh_build(const cw_nav_config *config, const cw_nav_geom *geom) {
  cw_navmesh none = { 0 };
  if (!config || !geom || geom->tris.count < 3 || geom->areas.count * 3 != geom->tris.count) {
    cw_error("nav: cw_navmesh_build needs a config and geometry with an area per triangle");
    return none;
  }
  double t0 = cw__nav_now_ms();
  float bmin[3], bmax[3];
  bool zero = config->bmin.x == 0 && config->bmin.y == 0 && config->bmin.z == 0 && config->bmax.x == 0 &&
              config->bmax.y == 0 && config->bmax.z == 0;
  if (zero) {
    rcCalcBounds(geom->verts.data, geom->verts.count / 3, bmin, bmax);
  } else {
    bmin[0] = config->bmin.x, bmin[1] = config->bmin.y, bmin[2] = config->bmin.z;
    bmax[0] = config->bmax.x, bmax[1] = config->bmax.y, bmax[2] = config->bmax.z;
  }
  NavMesh *n;
  cw_navmesh h = alloc_handle(&n);
  if (!setup(n, config, bmin, bmax)) {
    release(h, n);
    return none;
  }
  set_links(n, geom);
  int count = n->tw * n->th;
  int *tiles = (int *)cw_alloc(sizeof(int) * 2 * (size_t)count, CW_MEM_NAV);
  for (int ty = 0, i = 0; ty < n->th; ty++)
    for (int tx = 0; tx < n->tw; tx++, i++) tiles[i * 2] = tx, tiles[i * 2 + 1] = ty;
  TileOut *out = build_tiles(n, geom, tiles, count);
  bool ok = commit_tiles(n, out, count, tiles);
  cw_free(out);
  cw_free(tiles);
  if (ok && n->cfg.dynamic) cache_build_all(n);
  if (!ok) {
    release(h, n);
    return none;
  }
  warn_links(n);
  n->build_ms = (float)(cw__nav_now_ms() - t0);
  return h;
}

bool cw_navmesh_rebuild_area(cw_navmesh h, const cw_nav_geom *geom, t3_vec3 bmin, t3_vec3 bmax) {
  NavMesh *n = get(h);
  if (!n || !geom || n->cfg.dynamic) {
    if (n && n->cfg.dynamic) cw_error("nav: cw_navmesh_rebuild_area is for static navmeshes (dynamic ones use obstacles)");
    return false;
  }
  double t0 = cw__nav_now_ms();
  int x0 = (int)floorf((bmin.x - n->bmin[0]) / n->tile_world), x1 = (int)floorf((bmax.x - n->bmin[0]) / n->tile_world);
  int z0 = (int)floorf((bmin.z - n->bmin[2]) / n->tile_world), z1 = (int)floorf((bmax.z - n->bmin[2]) / n->tile_world);
  x0 = x0 < 0 ? 0 : x0, z0 = z0 < 0 ? 0 : z0;
  x1 = x1 >= n->tw ? n->tw - 1 : x1, z1 = z1 >= n->th ? n->th - 1 : z1;
  set_links(n, geom);
  if (x1 < x0 || z1 < z0) return true;
  int count = (x1 - x0 + 1) * (z1 - z0 + 1);
  int *tiles = (int *)cw_alloc(sizeof(int) * 2 * (size_t)count, CW_MEM_NAV);
  for (int ty = z0, i = 0; ty <= z1; ty++)
    for (int tx = x0; tx <= x1; tx++, i++) tiles[i * 2] = tx, tiles[i * 2 + 1] = ty;
  TileOut *out = build_tiles(n, geom, tiles, count);
  bool ok = commit_tiles(n, out, count, tiles);
  cw_free(out);
  cw_free(tiles);
  n->version++;
  n->build_ms = (float)(cw__nav_now_ms() - t0);
  return ok;
}

void cw_navmesh_free(cw_navmesh h) {
  NavMesh *n = get(h);
  if (n) release(h, n);
}

bool cw_navmesh_valid(cw_navmesh h) { return get(h) != nullptr; }

const cw_nav_config *cw_navmesh_config(cw_navmesh h) {
  NavMesh *n = get(h);
  return n ? &n->cfg : nullptr;
}

uint64_t cw_navmesh_hash(cw_navmesh h) {
  NavMesh *n = get(h);
  if (!n) return 0;
  uint64_t x = 0;
  const dtNavMesh *m = n->mesh;
  for (int i = 0; i < m->getMaxTiles(); i++) {
    const dtMeshTile *t = m->getTile(i);
    if (!t->header) continue;
    uint32_t pre[2] = { (uint32_t)i, t->salt };
    x = cw_hash_bytes(pre, sizeof pre, x);
    x = cw_hash_bytes(t->data, (size_t)t->dataSize, x);
  }
  for (int i = 0; i < n->links.count; i++) { /* none: the hash a navmesh without links always had */
    const cw_nav_link &l = n->links.data[i];
    const float f[10] = { l.start.x, l.start.y, l.start.z, l.end.x, l.end.y, l.end.z, l.cost, l.radius, l.duration, l.arc };
    const uint32_t u[3] = { l.tag, l.bidirectional ? 1u : 0u, l.area };
    x = cw_hash_bytes(f, sizeof f, x);
    x = cw_hash_bytes(u, sizeof u, x);
  }
  return x;
}

cw_navmesh_stats cw_navmesh_get_stats(cw_navmesh h) {
  cw_navmesh_stats s;
  memset(&s, 0, sizeof s);
  NavMesh *n = get(h);
  if (!n) return s;
  const dtNavMesh *m = n->mesh;
  for (int i = 0; i < m->getMaxTiles(); i++) {
    const dtMeshTile *t = m->getTile(i);
    if (!t->header) continue;
    s.tiles++;
    s.polys += t->header->polyCount;
    s.verts += t->header->vertCount;
    s.bytes += t->dataSize;
  }
  s.tiles_x = n->tw;
  s.tiles_z = n->th;
  s.obstacles = n->obstacles.count;
  s.build_ms = n->build_ms;
  s.links = n->links.count;
  if (s.links) {
    uint8_t *ends = (uint8_t *)cw_alloc((size_t)s.links, CW_MEM_NAV);
    link_ends(n, ends);
    for (int i = 0; i < s.links; i++) s.links_connected += ends[i] == 3;
    cw_free(ends);
  }
  return s;
}

int cw_nav_link_count(cw_navmesh h) {
  NavMesh *n = get(h);
  return n ? n->links.count : 0;
}

bool cw_nav_link_get(cw_navmesh h, uint32_t id, cw_nav_link *out) {
  NavMesh *n = get(h);
  if (!n || id < 1 || id > (uint32_t)n->links.count) return false;
  if (out) *out = n->links.data[id - 1];
  return true;
}

bool cw_nav_link_connected(cw_navmesh h, uint32_t id) {
  NavMesh *n = get(h);
  if (!n || id < 1 || id > (uint32_t)n->links.count) return false;
  uint8_t *ends = (uint8_t *)cw_alloc((size_t)n->links.count, CW_MEM_NAV);
  link_ends(n, ends);
  bool ok = ends[id - 1] == 3;
  cw_free(ends);
  return ok;
}

/* ── save / load ──────────────────────────────────────────────────────── */

namespace {
const uint32_t SAVE_MAGIC = 0x4e415643u; /* "CVAN" */
const uint32_t SAVE_VERSION = 2; /* 2: the link table after the bounds */

struct Writer {
  CW_ARRAY(uint8_t) b = {};
  void put(const void *p, size_t n) {
    cw_array_reserve(&b, b.count + (int)n);
    memcpy(b.data + b.count, p, n);
    b.count += (int)n;
  }
  void u32(uint32_t v) { put(&v, 4); }
};

void put_links(Writer &w, const NavMesh *n) {
  w.u32((uint32_t)n->links.count);
  for (int i = 0; i < n->links.count; i++) {
    const cw_nav_link &l = n->links.data[i];
    const float f[10] = { l.start.x, l.start.y, l.start.z, l.end.x, l.end.y, l.end.z, l.cost, l.radius, l.duration, l.arc };
    const uint32_t u[3] = { l.tag, l.bidirectional ? 1u : 0u, l.area };
    w.put(f, sizeof f);
    w.put(u, sizeof u);
  }
}

struct Reader {
  const uint8_t *p, *end;
  bool get(void *out, size_t n) {
    if ((size_t)(end - p) < n) return false;
    memcpy(out, p, n);
    p += n;
    return true;
  }
};

/* The config field by field (no struct padding in the bytes). */
void put_config(Writer &w, const cw_nav_config &c) {
  const float f[] = { c.cell_size, c.cell_height, c.agent_height, c.agent_radius, c.agent_max_climb, c.agent_max_slope,
                      c.region_min_size, c.region_merge_size, c.edge_max_len, c.edge_max_error, c.detail_sample_dist,
                      c.detail_sample_max_error, c.bmin.x, c.bmin.y, c.bmin.z, c.bmax.x, c.bmax.y, c.bmax.z };
  const int32_t i[] = { c.tile_size, c.verts_per_poly, (int32_t)c.partition, c.dynamic, c.max_obstacles, c.max_layers,
                        c.max_search_nodes };
  w.put(f, sizeof f);
  w.put(i, sizeof i);
}

bool get_config(Reader &r, cw_nav_config &c) {
  float f[18];
  int32_t i[7];
  if (!r.get(f, sizeof f) || !r.get(i, sizeof i)) return false;
  c = cw_nav_config_default();
  c.cell_size = f[0], c.cell_height = f[1], c.agent_height = f[2], c.agent_radius = f[3], c.agent_max_climb = f[4];
  c.agent_max_slope = f[5], c.region_min_size = f[6], c.region_merge_size = f[7], c.edge_max_len = f[8];
  c.edge_max_error = f[9], c.detail_sample_dist = f[10], c.detail_sample_max_error = f[11];
  c.bmin = { f[12], f[13], f[14] };
  c.bmax = { f[15], f[16], f[17] };
  c.tile_size = i[0], c.verts_per_poly = i[1], c.partition = (cw_nav_partition)i[2], c.dynamic = i[3] != 0;
  c.max_obstacles = i[4], c.max_layers = i[5], c.max_search_nodes = i[6];
  return true;
}
} // namespace

void *cw_navmesh_save(cw_navmesh h, size_t *len) {
  NavMesh *n = get(h);
  if (!n) return nullptr;
  Writer w;
  w.u32(SAVE_MAGIC);
  w.u32(SAVE_VERSION);
  put_config(w, n->cfg);
  w.put(n->bmin, sizeof n->bmin);
  w.put(n->bmax, sizeof n->bmax);
  put_links(w, n);
  if (!n->cfg.dynamic) {
    const dtNavMesh *m = n->mesh;
    uint32_t count = 0;
    for (int i = 0; i < m->getMaxTiles(); i++) count += m->getTile(i)->header != nullptr;
    w.u32(count);
    for (int i = 0; i < m->getMaxTiles(); i++) {
      const dtMeshTile *t = m->getTile(i);
      if (!t->header) continue;
      w.u32((uint32_t)t->dataSize);
      w.put(t->data, (size_t)t->dataSize);
    }
  } else {
    uint32_t count = 0;
    for (int i = 0; i < n->cache->getTileCount(); i++) count += n->cache->getTile(i)->header != nullptr;
    w.u32(count);
    for (int i = 0; i < n->cache->getTileCount(); i++) {
      const dtCompressedTile *t = n->cache->getTile(i);
      if (!t->header) continue;
      w.u32((uint32_t)t->dataSize);
      w.put(t->data, (size_t)t->dataSize);
    }
    /* live obstacles in the order they were added, as they were added */
    w.u32((uint32_t)n->obstacles.count);
    for (int i = 0; i < n->obstacles.count; i++) {
      w.u32(n->obstacles.data[i].kind);
      w.put(n->obstacles.data[i].v, sizeof n->obstacles.data[i].v);
    }
  }
  *len = (size_t)w.b.count;
  void *out = cw_alloc((size_t)w.b.count, CW_MEM_NAV);
  memcpy(out, w.b.data, (size_t)w.b.count);
  cw_array_free(&w.b);
  return out;
}

cw_navmesh cw_navmesh_load(const void *data, size_t len) {
  cw_navmesh none = { 0 };
  Reader r = { (const uint8_t *)data, (const uint8_t *)data + len };
  uint32_t magic = 0, version = 0;
  cw_nav_config c;
  float bmin[3], bmax[3];
  if (!data || !r.get(&magic, 4) || !r.get(&version, 4) || magic != SAVE_MAGIC || version < 1 ||
      version > SAVE_VERSION || !get_config(r, c) || !r.get(bmin, sizeof bmin) || !r.get(bmax, sizeof bmax)) {
    cw_error("nav: not navmesh data (or another version)");
    return none;
  }
  NavMesh *n;
  cw_navmesh h = alloc_handle(&n);
  if (!setup(n, &c, bmin, bmax)) {
    release(h, n);
    return none;
  }
  bool ok = true;
  if (version >= 2) { /* the link table: before the tiles, which a tile cache builds with its links */
    uint32_t nl = 0;
    ok = r.get(&nl, 4) && (size_t)(r.end - r.p) / 52 >= nl;
    for (uint32_t i = 0; ok && i < nl; i++) {
      float f[10];
      uint32_t u[3];
      ok = r.get(f, sizeof f) && r.get(u, sizeof u);
      if (!ok) break;
      cw_nav_link l;
      memset(&l, 0, sizeof l);
      l.start = { f[0], f[1], f[2] }, l.end = { f[3], f[4], f[5] };
      l.cost = f[6], l.radius = f[7], l.duration = f[8], l.arc = f[9];
      l.tag = u[0], l.bidirectional = u[1] != 0, l.area = (uint8_t)u[2];
      cw_array_push(&n->links, l);
    }
  }
  uint32_t count = 0;
  ok = ok && r.get(&count, 4);
  for (uint32_t i = 0; ok && i < count; i++) {
    uint32_t size;
    ok = r.get(&size, 4) && (size_t)(r.end - r.p) >= size;
    if (!ok) break;
    unsigned char *t = (unsigned char *)dtAlloc(size, DT_ALLOC_PERM);
    r.get(t, size);
    dtStatus st = c.dynamic ? n->cache->addTile(t, (int)size, DT_COMPRESSEDTILE_FREE_DATA, nullptr)
                            : n->mesh->addTile(t, (int)size, DT_TILE_FREE_DATA, 0, nullptr);
    if (dtStatusFailed(st)) {
      dtFree(t);
      ok = false;
    }
  }
  if (ok && c.dynamic) {
    cache_build_all(n);
    uint32_t nob;
    ok = r.get(&nob, 4);
    for (uint32_t i = 0; ok && i < nob; i++) {
      uint32_t kind;
      float v[7];
      ok = r.get(&kind, 4) && r.get(v, sizeof v) && kind <= 1;
      if (!ok) break;
      if (kind == 0) cw_nav_obstacle_add_cylinder(h, { v[0], v[1], v[2] }, v[3], v[4]);
      else cw_nav_obstacle_add_box(h, { v[0], v[1], v[2] }, { v[3], v[4], v[5] }, v[6]);
      if (n->cache && i % 32 == 31) cw_nav_update(h); /* the cache queues at most 64 requests */
    }
    if (ok) cw_nav_update(h);
  }
  if (!ok) {
    cw_error("nav: navmesh data is truncated or corrupt");
    release(h, n);
    return none;
  }
  return h;
}

/* ── queries ──────────────────────────────────────────────────────────── */

void cw_navmesh_set_query_extents(cw_navmesh h, t3_vec3 e) {
  NavMesh *n = get(h);
  if (n) n->extents[0] = e.x, n->extents[1] = e.y, n->extents[2] = e.z;
}

namespace {
bool nearest(NavMesh *n, dtNavMeshQuery *q, const float p[3], const Filter &f, dtPolyRef *ref, float out[3]) {
  *ref = 0;
  if (dtStatusFailed(q->findNearestPoly(p, n->extents, &f, ref, out)) || !*ref) return false;
  return true;
}

/* The straight path; links (may be NULL): the id of the link each waypoint starts. */
int straight(NavMesh *n, dtNavMeshQuery *q, const float s[3], const float e[3], const dtPolyRef *polys, int np,
             t3_vec3 *pts, uint32_t *links, int max) {
  if (max <= 0) return 0;
  float local[3 * 128];
  unsigned char lflags[128];
  dtPolyRef lrefs[128];
  bool big = max > 128;
  float *buf = big ? (float *)cw_alloc(sizeof(float) * 3 * (size_t)max, CW_MEM_NAV) : local;
  unsigned char *flags = big ? (unsigned char *)cw_alloc((size_t)max, CW_MEM_NAV) : lflags;
  dtPolyRef *refs = big ? (dtPolyRef *)cw_alloc(sizeof(dtPolyRef) * (size_t)max, CW_MEM_NAV) : lrefs;
  int ns = 0;
  if (dtStatusFailed(q->findStraightPath(s, e, polys, np, buf, flags, refs, &ns, max, 0))) ns = 0;
  for (int i = 0; i < ns; i++) {
    pts[i] = { buf[i * 3], buf[i * 3 + 1], buf[i * 3 + 2] };
    if (!links) continue;
    links[i] = 0;
    if (flags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) {
      const dtOffMeshConnection *con = n->mesh->getOffMeshConnectionByRef(refs[i]);
      if (con) links[i] = con->userId;
    }
  }
  if (big) cw_free(buf), cw_free(flags), cw_free(refs);
  return ns;
}

const int MAX_CORRIDOR = 512;

/* The path query every public path call shares; max_iters 0 = one findPath. */
int find_path(NavMesh *n, const float a[3], const float b[3], const cw_nav_filter *cf, int max_iters, t3_vec3 *pts,
              uint32_t *links, int max, bool *partial, bool *failed) {
  Filter f;
  make_filter(f, cf);
  filter_links(f, n);
  dtNavMeshQuery *q = query_of(n);
  dtPolyRef sref, eref, polys[MAX_CORRIDOR];
  float s[3], e[3];
  *partial = false;
  *failed = false;
  if (!nearest(n, q, a, f, &sref, s) || !nearest(n, q, b, f, &eref, e)) {
    *failed = true;
    return 0;
  }
  int np = 0;
  dtStatus st;
  if (max_iters <= 0) {
    st = q->findPath(sref, eref, s, e, &f, polys, &np, MAX_CORRIDOR);
  } else {
    st = q->initSlicedFindPath(sref, eref, s, e, &f, 0);
    if (dtStatusInProgress(st)) st = q->updateSlicedFindPath(max_iters, nullptr);
    st = q->finalizeSlicedFindPath(polys, &np, MAX_CORRIDOR);
  }
  if (dtStatusFailed(st) || np == 0) {
    *failed = true;
    return 0;
  }
  if (polys[np - 1] != eref) {
    *partial = true;
    float c[3];
    q->closestPointOnPoly(polys[np - 1], e, c, nullptr);
    dtVcopy(e, c);
  }
  return straight(n, q, s, e, polys, np, pts, links, max);
}

thread_local cw_rng *tl_rng;
float frand() { return cw_rng_float(tl_rng); }
} // namespace

bool cw_nav_nearest(cw_navmesh h, t3_vec3 p, const cw_nav_filter *cf, t3_vec3 *out) {
  NavMesh *n = get(h);
  if (!n) return false;
  Filter f;
  make_filter(f, cf);
  dtPolyRef ref;
  float o[3];
  const float pp[3] = { p.x, p.y, p.z };
  if (!nearest(n, query_of(n), pp, f, &ref, o)) return false;
  if (out) *out = { o[0], o[1], o[2] };
  return true;
}

bool cw_nav_height(cw_navmesh h, t3_vec3 p, float *y) {
  NavMesh *n = get(h);
  if (!n) return false;
  Filter f;
  make_filter(f, nullptr);
  dtNavMeshQuery *q = query_of(n);
  dtPolyRef ref;
  float o[3];
  const float pp[3] = { p.x, p.y, p.z };
  if (!nearest(n, q, pp, f, &ref, o)) return false;
  float hy;
  if (dtStatusFailed(q->getPolyHeight(ref, pp, &hy))) return false;
  if (y) *y = hy;
  return true;
}

int cw_nav_find_path(cw_navmesh h, t3_vec3 a, t3_vec3 b, const cw_nav_filter *f, t3_vec3 *pts, int max, bool *partial) {
  NavMesh *n = get(h);
  bool part = false, failed = false;
  int c = 0;
  if (n && pts && max > 0) {
    const float pa[3] = { a.x, a.y, a.z }, pb[3] = { b.x, b.y, b.z };
    c = find_path(n, pa, pb, f, 0, pts, nullptr, max, &part, &failed);
  }
  if (partial) *partial = part;
  return c;
}

int cw_nav_find_path_links(cw_navmesh h, t3_vec3 a, t3_vec3 b, const cw_nav_filter *f, t3_vec3 *pts, uint32_t *links,
                           int max, bool *partial) {
  NavMesh *n = get(h);
  bool part = false, failed = false;
  int c = 0;
  if (n && pts && max > 0) {
    const float pa[3] = { a.x, a.y, a.z }, pb[3] = { b.x, b.y, b.z };
    c = find_path(n, pa, pb, f, 0, pts, links, max, &part, &failed);
  }
  if (partial) *partial = part;
  return c;
}

bool cw_nav_raycast(cw_navmesh h, t3_vec3 a, t3_vec3 b, const cw_nav_filter *cf, cw_nav_hit *out) {
  NavMesh *n = get(h);
  if (!n) return false;
  Filter f;
  make_filter(f, cf);
  dtNavMeshQuery *q = query_of(n);
  dtPolyRef sref;
  float s[3];
  const float pa[3] = { a.x, a.y, a.z }, pb[3] = { b.x, b.y, b.z };
  if (!nearest(n, q, pa, f, &sref, s)) return false;
  float t = 0, nrm[3] = { 0, 0, 0 };
  dtPolyRef path[64];
  int np = 0;
  if (dtStatusFailed(q->raycast(sref, s, pb, &f, &t, nrm, path, &np, 64))) return false;
  if (out) {
    bool hit = t <= 1;
    float tt = hit ? t : 1;
    out->hit = hit;
    out->t = tt;
    out->normal = { nrm[0], nrm[1], nrm[2] };
    float p[3] = { s[0] + (pb[0] - s[0]) * tt, s[1] + (pb[1] - s[1]) * tt, s[2] + (pb[2] - s[2]) * tt };
    float hy;
    if (np > 0 && dtStatusSucceed(q->getPolyHeight(path[np - 1], p, &hy))) p[1] = hy;
    out->point = { p[0], p[1], p[2] };
  }
  return true;
}

bool cw_nav_random_point(cw_navmesh h, const cw_nav_filter *cf, cw_rng *rng, t3_vec3 *out) {
  NavMesh *n = get(h);
  if (!n || !rng) return false;
  Filter f;
  make_filter(f, cf);
  tl_rng = rng;
  dtPolyRef ref;
  float p[3];
  bool ok = dtStatusSucceed(query_of(n)->findRandomPoint(&f, frand, &ref, p)) && ref;
  tl_rng = nullptr;
  if (ok && out) *out = { p[0], p[1], p[2] };
  return ok;
}

bool cw_nav_random_point_near(cw_navmesh h, t3_vec3 center, float radius, const cw_nav_filter *cf, cw_rng *rng,
                              t3_vec3 *out) {
  NavMesh *n = get(h);
  if (!n || !rng) return false;
  Filter f;
  make_filter(f, cf);
  dtNavMeshQuery *q = query_of(n);
  dtPolyRef sref, ref;
  float s[3], p[3];
  const float c[3] = { center.x, center.y, center.z };
  if (!nearest(n, q, c, f, &sref, s)) return false;
  tl_rng = rng;
  bool ok = dtStatusSucceed(q->findRandomPointAroundCircle(sref, s, radius, &f, frand, &ref, p)) && ref;
  tl_rng = nullptr;
  if (ok && out) *out = { p[0], p[1], p[2] };
  return ok;
}

namespace {
struct BatchJob {
  NavMesh *n;
  const cw_nav_path_req *reqs;
  t3_vec3 *pts;
  int max;
  int *counts;
};
void batch_range(int b, int e, int, void *ctx) {
  BatchJob *j = (BatchJob *)ctx;
  for (int i = b; i < e; i++) {
    const cw_nav_path_req *r = &j->reqs[i];
    const float pa[3] = { r->from.x, r->from.y, r->from.z }, pb[3] = { r->to.x, r->to.y, r->to.z };
    bool part, failed;
    j->counts[i] = find_path(j->n, pa, pb, r->filter, 0, j->pts + (size_t)i * j->max, nullptr, j->max, &part, &failed);
  }
}
} // namespace

void cw_nav_find_paths(cw_navmesh h, const cw_nav_path_req *reqs, int count, t3_vec3 *pts, int max, int *counts) {
  NavMesh *n = get(h);
  if (!n || count <= 0) return;
  BatchJob j = { n, reqs, pts, max, counts };
  cw_parallel_for(count, 4, batch_range, &j);
}

/* ── the per-tick budget ──────────────────────────────────────────────── */

uint32_t cw_nav_request_path(cw_navmesh h, t3_vec3 from, t3_vec3 to, const cw_nav_filter *f) {
  NavMesh *n = get(h);
  if (!n) return 0;
  PathReq r;
  memset(&r, 0, sizeof r);
  r.ticket = n->next_ticket++;
  if (!n->next_ticket) n->next_ticket = 1;
  r.status = CW_NAV_PATH_PENDING;
  r.from[0] = from.x, r.from[1] = from.y, r.from[2] = from.z;
  r.to[0] = to.x, r.to[1] = to.y, r.to[2] = to.z;
  if (f) r.filter = *f, r.has_filter = true;
  cw_array_push(&n->reqs, r);
  return r.ticket;
}

namespace {
struct TickJob {
  NavMesh *n;
  int *idx;
  int iters;
};
void tick_range(int b, int e, int, void *ctx) {
  TickJob *j = (TickJob *)ctx;
  const int MAXP = 256;
  for (int i = b; i < e; i++) {
    PathReq *r = &j->n->reqs.data[j->idx[i]];
    t3_vec3 tmp[MAXP];
    uint32_t ltmp[MAXP];
    bool part, failed;
    int c = find_path(j->n, r->from, r->to, r->has_filter ? &r->filter : nullptr, j->iters, tmp, ltmp, MAXP, &part,
                      &failed);
    r->status = failed ? CW_NAV_PATH_FAILED : part ? CW_NAV_PATH_PARTIAL : CW_NAV_PATH_DONE;
    r->count = c;
    if (c) {
      r->pts = (t3_vec3 *)cw_alloc(sizeof(t3_vec3) * (size_t)c, CW_MEM_NAV);
      memcpy(r->pts, tmp, sizeof(t3_vec3) * (size_t)c);
      r->links = (uint32_t *)cw_alloc(sizeof(uint32_t) * (size_t)c, CW_MEM_NAV);
      memcpy(r->links, ltmp, sizeof(uint32_t) * (size_t)c);
    }
  }
}
} // namespace

void cw_nav_tick(cw_navmesh h, int max_requests, int max_iters) {
  NavMesh *n = get(h);
  if (!n) return;
  int *idx = (int *)cw_alloc(sizeof(int) * (size_t)(n->reqs.count + 1), CW_MEM_NAV);
  int k = 0;
  for (int i = 0; i < n->reqs.count && (max_requests <= 0 || k < max_requests); i++)
    if (n->reqs.data[i].status == CW_NAV_PATH_PENDING) idx[k++] = i;
  TickJob j = { n, idx, max_iters > 0 ? max_iters : 0 };
  cw_parallel_for(k, 1, tick_range, &j);
  cw_free(idx);
}

cw_nav_path_status cw_nav_take_path(cw_navmesh h, uint32_t ticket, t3_vec3 *pts, int max, int *count) {
  return cw_nav_take_path_links(h, ticket, pts, nullptr, max, count);
}

cw_nav_path_status cw_nav_take_path_links(cw_navmesh h, uint32_t ticket, t3_vec3 *pts, uint32_t *links, int max,
                                          int *count) {
  NavMesh *n = get(h);
  if (count) *count = 0;
  if (!n) return CW_NAV_PATH_UNKNOWN;
  for (int i = 0; i < n->reqs.count; i++) {
    PathReq *r = &n->reqs.data[i];
    if (r->ticket != ticket) continue;
    cw_nav_path_status s = r->status;
    if (s == CW_NAV_PATH_PENDING) return s;
    int c = r->count < max ? r->count : max;
    if (pts && c > 0) memcpy(pts, r->pts, sizeof(t3_vec3) * (size_t)c);
    if (links && c > 0) memcpy(links, r->links, sizeof(uint32_t) * (size_t)c);
    if (count) *count = pts ? c : r->count;
    cw_free(r->pts);
    cw_free(r->links);
    memmove(r, r + 1, sizeof *r * (size_t)(n->reqs.count - i - 1));
    n->reqs.count--;
    return s;
  }
  return CW_NAV_PATH_UNKNOWN;
}

void cw_nav_cancel_path(cw_navmesh h, uint32_t ticket) { cw_nav_take_path(h, ticket, nullptr, 0, nullptr); }

int cw_nav_pending_paths(cw_navmesh h) {
  NavMesh *n = get(h);
  int k = 0;
  if (n)
    for (int i = 0; i < n->reqs.count; i++) k += n->reqs.data[i].status == CW_NAV_PATH_PENDING;
  return k;
}

/* ── obstacles ────────────────────────────────────────────────────────── */

namespace {
NavMesh *dyn(cw_navmesh h) {
  NavMesh *n = get(h);
  if (n && !n->cfg.dynamic) {
    cw_error("nav: obstacles need a dynamic navmesh (cw_nav_config.dynamic)");
    return nullptr;
  }
  return n;
}
uint32_t add_obstacle(NavMesh *n, dtStatus st, dtObstacleRef ref, uint32_t kind, const float v[7]) {
  if (dtStatusFailed(st)) {
    cw_warn("nav: obstacle not added (%s)", (st & DT_BUFFER_TOO_SMALL) ? "too many queued changes: call cw_nav_update"
                                                                     : "max_obstacles reached");
    return 0;
  }
  Obstacle o;
  o.id = n->next_obstacle++;
  o.ref = ref;
  o.kind = kind;
  memcpy(o.v, v, sizeof o.v);
  cw_array_push(&n->obstacles, o);
  return o.id;
}
} // namespace

uint32_t cw_nav_obstacle_add_cylinder(cw_navmesh h, t3_vec3 base, float radius, float height) {
  NavMesh *n = dyn(h);
  if (!n) return 0;
  const float p[3] = { base.x, base.y, base.z };
  dtObstacleRef ref = 0;
  const float v[7] = { base.x, base.y, base.z, radius, height, 0, 0 };
  dtStatus st = n->cache->addObstacle(p, radius, height, &ref);
  if (dtStatusDetail(st, DT_BUFFER_TOO_SMALL)) { /* the request queue is full: apply it, then queue this one */
    cw_nav_update(h);
    st = n->cache->addObstacle(p, radius, height, &ref);
  }
  return add_obstacle(n, st, ref, 0, v);
}

uint32_t cw_nav_obstacle_add_box(cw_navmesh h, t3_vec3 center, t3_vec3 half, float y_rotation) {
  NavMesh *n = dyn(h);
  if (!n) return 0;
  dtObstacleRef ref = 0;
  dtStatus st = DT_FAILURE;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (y_rotation == 0) {
      const float mn[3] = { center.x - half.x, center.y - half.y, center.z - half.z };
      const float mx[3] = { center.x + half.x, center.y + half.y, center.z + half.z };
      st = n->cache->addBoxObstacle(mn, mx, &ref);
    } else {
      const float c[3] = { center.x, center.y, center.z }, e[3] = { half.x, half.y, half.z };
      st = n->cache->addBoxObstacle(c, e, y_rotation, &ref);
    }
    if (!dtStatusDetail(st, DT_BUFFER_TOO_SMALL)) break;
    cw_nav_update(h); /* the request queue is full: apply it, then queue this one */
  }
  const float v[7] = { center.x, center.y, center.z, half.x, half.y, half.z, y_rotation };
  return add_obstacle(n, st, ref, 1, v);
}

bool cw_nav_obstacle_remove(cw_navmesh h, uint32_t id) {
  NavMesh *n = dyn(h);
  if (!n) return false;
  for (int i = 0; i < n->obstacles.count; i++) {
    if (n->obstacles.data[i].id != id) continue;
    dtStatus st = n->cache->removeObstacle(n->obstacles.data[i].ref);
    if (dtStatusDetail(st, DT_BUFFER_TOO_SMALL)) { /* the request queue is full: apply it, then queue this one */
      cw_nav_update(h);
      st = n->cache->removeObstacle(n->obstacles.data[i].ref);
    }
    if (dtStatusFailed(st)) {
      cw_warn("nav: obstacle not removed");
      return false;
    }
    memmove(&n->obstacles.data[i], &n->obstacles.data[i + 1], sizeof(Obstacle) * (size_t)(n->obstacles.count - i - 1));
    n->obstacles.count--;
    return true;
  }
  return false;
}

int cw_nav_update(cw_navmesh h) {
  NavMesh *n = get(h);
  if (!n || !n->cfg.dynamic) return 0;
  double t0 = cw__nav_now_ms();
  int total = 0;
  for (;;) { /* the cache holds at most 64 pending tiles at once; more requests drain in rounds */
    dtCompressedTileRef refs[64];
    int count = 0;
    n->cache->processObstacleRequests(refs, &count, 64);
    if (count > 64) count = 64;
    if (!count) break;
    total += cache_rebuild(n, refs, count);
  }
  if (total) n->build_ms = (float)(cw__nav_now_ms() - t0);
  return total;
}

/* ── internal interface (crowd, debug, nodes) ─────────────────────────── */

bool cw__nav_nearest_ref(cw_navmesh h, const float p[3], const cw_nav_filter *cf, cw__poly *ref, float out[3]) {
  NavMesh *n = get(h);
  if (!n) return false;
  Filter f;
  make_filter(f, cf);
  dtPolyRef r;
  bool ok = nearest(n, query_of(n), p, f, &r, out);
  *ref = ok ? r : 0;
  return ok;
}

int cw__nav_path_polys(cw_navmesh h, cw__poly sref, const float s[3], cw__poly eref, const float e[3],
                       const cw_nav_filter *cf, int max_iters, cw__poly *polys, int max, bool *partial) {
  NavMesh *n = get(h);
  *partial = false;
  if (!n) return 0;
  Filter f;
  make_filter(f, cf);
  filter_links(f, n);
  dtNavMeshQuery *q = query_of(n);
  int np = 0;
  dtStatus st;
  if (max_iters <= 0) {
    st = q->findPath(sref, eref, s, e, &f, polys, &np, max);
  } else {
    st = q->initSlicedFindPath(sref, eref, s, e, &f, 0);
    if (dtStatusInProgress(st)) st = q->updateSlicedFindPath(max_iters, nullptr);
    st = q->finalizeSlicedFindPath(polys, &np, max);
  }
  if (dtStatusFailed(st)) return 0;
  *partial = np > 0 && polys[np - 1] != eref;
  return np;
}

int cw__nav_corners(cw_navmesh h, const float s[3], const float e[3], const cw__poly *path, int np, float *corners,
                    uint8_t *flags, cw__poly *refs, int max) {
  NavMesh *n = get(h);
  if (!n || np <= 0) return 0;
  int ns = 0;
  if (dtStatusFailed(query_of(n)->findStraightPath(s, e, path, np, corners, flags, refs, &ns, max, 0))) return 0;
  return ns;
}

uint32_t cw__nav_link_cross(cw_navmesh h, cw__poly prev, cw__poly ref, float start[3], float end[3]) {
  NavMesh *n = get(h);
  if (!n) return 0;
  const dtOffMeshConnection *con = n->mesh->getOffMeshConnectionByRef(ref);
  if (!con || dtStatusFailed(n->mesh->getOffMeshConnectionPolyEndPoints(prev, ref, start, end))) return 0;
  return con->userId;
}

const cw_nav_link *cw__nav_link(cw_navmesh h, uint32_t id) {
  NavMesh *n = get(h);
  return n && id >= 1 && id <= (uint32_t)n->links.count ? &n->links.data[id - 1] : nullptr;
}

void cw__nav_each_link(cw_navmesh h, void (*fn)(const cw_nav_link *l, bool connected, void *ctx), void *ctx) {
  NavMesh *n = get(h);
  if (!n || !n->links.count) return;
  uint8_t *ends = (uint8_t *)cw_alloc((size_t)n->links.count, CW_MEM_NAV);
  link_ends(n, ends);
  for (int i = 0; i < n->links.count; i++) fn(&n->links.data[i], ends[i] == 3, ctx);
  cw_free(ends);
}

int cw__nav_move_along(cw_navmesh h, cw__poly sref, const float s[3], const float e[3], const cw_nav_filter *cf,
                       float out[3], cw__poly *visited, int max) {
  NavMesh *n = get(h);
  if (!n) return 0;
  Filter f;
  make_filter(f, cf);
  int nv = 0;
  if (dtStatusFailed(query_of(n)->moveAlongSurface(sref, s, e, &f, out, visited, &nv, max))) return 0;
  return nv;
}

int cw__nav_raycast_polys(cw_navmesh h, cw__poly sref, const float s[3], const float e[3], const cw_nav_filter *cf,
                          float *t, cw__poly *polys, int max) {
  NavMesh *n = get(h);
  if (!n) return 0;
  Filter f;
  make_filter(f, cf);
  float nrm[3];
  int np = 0;
  if (dtStatusFailed(query_of(n)->raycast(sref, s, e, &f, t, nrm, polys, &np, max))) return 0;
  return np;
}

bool cw__nav_poly_height(cw_navmesh h, cw__poly ref, const float p[3], float *hy) {
  NavMesh *n = get(h);
  return n && dtStatusSucceed(query_of(n)->getPolyHeight(ref, p, hy));
}

bool cw__nav_ref_ok(cw_navmesh h, cw__poly ref, const cw_nav_filter *cf) {
  NavMesh *n = get(h);
  if (!n) return false;
  Filter f;
  make_filter(f, cf);
  return query_of(n)->isValidPolyRef(ref, &f);
}

void cw__nav_closest_on_poly(cw_navmesh h, cw__poly ref, const float p[3], float out[3]) {
  NavMesh *n = get(h);
  dtVcopy(out, p);
  if (n) query_of(n)->closestPointOnPoly(ref, p, out, nullptr);
}

uint32_t cw__nav_version(cw_navmesh h) {
  NavMesh *n = get(h);
  return n ? n->version : 0;
}

bool cw__nav_bounds(cw_navmesh h, float bmin[3], float bmax[3]) {
  NavMesh *n = get(h);
  if (!n) return false;
  dtVcopy(bmin, n->bmin);
  dtVcopy(bmax, n->bmax);
  return true;
}

void cw__nav_attach_crowd(cw_navmesh h, cw_crowd c) {
  NavMesh *n = get(h);
  if (n) cw_array_push(&n->crowds, c);
}

void cw__nav_detach_crowd(cw_navmesh h, cw_crowd c) {
  NavMesh *n = get(h);
  if (!n) return;
  for (int i = 0; i < n->crowds.count; i++)
    if (n->crowds.data[i].id == c.id) {
      memmove(&n->crowds.data[i], &n->crowds.data[i + 1], sizeof(cw_crowd) * (size_t)(n->crowds.count - i - 1));
      n->crowds.count--;
      return;
    }
}

void cw__nav_each_edge(cw_navmesh h, cw__nav_edge_fn fn, void *ctx) {
  NavMesh *n = get(h);
  if (!n) return;
  const dtNavMesh *m = n->mesh;
  for (int i = 0; i < m->getMaxTiles(); i++) {
    const dtMeshTile *t = m->getTile(i);
    if (!t->header) continue;
    for (int p = 0; p < t->header->polyCount; p++) {
      const dtPoly *poly = &t->polys[p];
      if (poly->getType() != DT_POLYTYPE_GROUND) continue;
      for (int k = 0; k < poly->vertCount; k++) {
        /* an edge is a boundary when no link crosses it (portals between tiles have links) */
        bool linked = false;
        if (poly->neis[k] & DT_EXT_LINK) {
          for (unsigned l = poly->firstLink; l != DT_NULL_LINK; l = t->links[l].next)
            if (t->links[l].edge == k) linked = true;
        } else {
          linked = poly->neis[k] != 0;
        }
        const float *a = &t->verts[poly->verts[k] * 3];
        const float *b = &t->verts[poly->verts[(k + 1) % poly->vertCount] * 3];
        fn(a, b, !linked, ctx);
      }
    }
  }
}

void cw__nav_each_tile(cw_navmesh h, void (*fn)(const float bmin[3], const float bmax[3], void *ctx), void *ctx) {
  NavMesh *n = get(h);
  if (!n) return;
  const dtNavMesh *m = n->mesh;
  for (int i = 0; i < m->getMaxTiles(); i++) {
    const dtMeshTile *t = m->getTile(i);
    if (t->header) fn(t->header->bmin, t->header->bmax, ctx);
  }
}

void cw__nav_each_obstacle(cw_navmesh h, void (*fn)(int kind, const float a[3], const float b[3], float r, void *ctx),
                           void *ctx) {
  NavMesh *n = get(h);
  if (!n || !n->cache) return;
  for (int i = 0; i < n->obstacles.count; i++) {
    const dtTileCacheObstacle *ob = n->cache->getObstacleByRef(n->obstacles.data[i].ref);
    if (!ob) continue;
    if (ob->type == DT_OBSTACLE_CYLINDER) {
      const float top[3] = { ob->cylinder.pos[0], ob->cylinder.pos[1] + ob->cylinder.height, ob->cylinder.pos[2] };
      fn(0, ob->cylinder.pos, top, ob->cylinder.radius, ctx);
    } else {
      float mn[3], mx[3];
      n->cache->getObstacleBounds(ob, mn, mx);
      fn(1, mn, mx, 0, ctx);
    }
  }
}
