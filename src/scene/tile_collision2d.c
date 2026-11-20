/* TileCollision2D: Box2D static geometry from a TileMap2D's cells and its tile shapes
 * (cartwheel/scene2d.h "tile collision" says what is built and why).
 *
 * Per map layer and chunk: every cell's polygon gives directed edges; an edge on a cell's side loses the parts
 * the neighbour's polygon covers on the same side, what is left joins end to start into the outline of the
 * union, and runs of a chunk's own edges become chains whose ghost vertices are the neighbouring edges (which
 * may belong to the next chunk). All of it in integers (CW__TILE_SUB units to a tile pixel), so the same cells
 * give the same chains on every host. */
#include <stdlib.h>

#include "phys_nodes_internal.h"
#include "scene2d_internal.h"

typedef struct tc_chunk {
  CW_ARRAY(uint64_t) chains;
  int32_t segments;
} tc_chunk;

typedef struct tilecol {
  const char *tilemap;
  int32_t layers;
  int32_t one_way_layer;
  /* runtime */
  cw_node map;
  int cw, ch, nl;   /* chunks across and down, map layers */
  tc_chunk *chunks; /* nl * ch * cw */
  uint32_t serial;  /* the map's edit serial the chunks are built for */
  int32_t builds;
} tilecol;

const cw_node_type *cw__tilecol_t;

typedef struct tc_edge {
  int32_t ax, ay, bx, by;
  int32_t next, prev;
  uint8_t owned, done;
} tc_edge;
typedef CW_ARRAY(tc_edge) tc_edges;

static void tc_init(cw_node n, void *d) {
  (void)n;
  tilecol *t = d;
  t->tilemap = cw_intern("..");
  t->layers = -1;
  t->one_way_layer = -1;
}

static void free_chunks(tilecol *t) {
  for (int i = 0; i < t->nl * t->cw * t->ch; i++) cw_array_free(&t->chunks[i].chains);
  cw_free(t->chunks);
  t->chunks = NULL;
  t->cw = t->ch = t->nl = 0;
}

static void tc_destroy(cw_node n, void *d) {
  (void)n;
  free_chunks(d);
}

static cw_node find_map(cw_node n, tilecol *t) {
  cw_node m = cw_node_get(n, t->tilemap ? t->tilemap : "");
  return cw_node_is(m, "TileMap2D") ? m : (cw_node){ 0 };
}

/* ── transforms: map-local pixels -> the body's frame, pixels ─────────── */

typedef struct aff {
  float a, b, c, d, tx, ty;
} aff;
static aff aff_of(const float *g) { return (aff){ g[0], g[1], g[2], g[3], g[4], g[5] }; }
static aff aff_mul(aff p, aff q) { /* p after q */
  return (aff){ p.a * q.a + p.c * q.b, p.b * q.a + p.d * q.b, p.a * q.c + p.c * q.d, p.b * q.c + p.d * q.d,
                p.a * q.tx + p.c * q.ty + p.tx, p.b * q.tx + p.d * q.ty + p.ty };
}
static aff aff_inv(aff m) {
  float det = m.a * m.d - m.b * m.c;
  if (det == 0) return (aff){ 1, 0, 0, 1, 0, 0 };
  float i = 1.0f / det;
  aff r = { m.d * i, -m.b * i, -m.c * i, m.a * i, 0, 0 };
  r.tx = -(r.a * m.tx + r.c * m.ty);
  r.ty = -(r.b * m.tx + r.d * m.ty);
  return r;
}
static cw_vec2 aff_apply(aff m, cw_vec2 p) { return cw_v2(m.a * p.x + m.c * p.y + m.tx, m.b * p.x + m.d * p.y + m.ty); }

/* The body sits at the node's global position and rotation with the scale dropped (as every 2D body does), so
 * map-local pixels reach its frame through the node's own frame and then the node's global scale. With the map
 * as the node's parent (the usual layout) this depends on the node's local transform alone, so a map that moves
 * carries its colliders along without rebuilding them. */
static aff map_to_body(cw_node n, cw_node map) {
  aff rel;
  if (cw_node_parent(n).id == map.id) {
    const cw_node2d *nd = cw_node_data_t(n, cw__node2d_t);
    cw_vec2 p = nd->position, s = nd->scale;
    float sn, cs;
    cw_sincos(nd->rotation, &sn, &cs);
    rel = aff_inv((aff){ cs * s.x, sn * s.x, -sn * s.y, cs * s.y, p.x, p.y });
  } else {
    rel = aff_mul(aff_inv(aff_of(cw_node2d_global_transform(n))), aff_of(cw_node2d_global_transform(map)));
  }
  const float *g = cw_node2d_global_transform(n);
  float sx = sqrtf(g[0] * g[0] + g[1] * g[1]), sy = sqrtf(g[2] * g[2] + g[3] * g[3]);
  return aff_mul((aff){ sx, 0, 0, sy, 0, 0 }, rel);
}

uint32_t cw__tilecol_hash(cw_node n, uint32_t h) {
  tilecol *t = cw_node_data_t(n, cw__tilecol_t);
  cw_node map = find_map(n, t);
  h = cw_hash32(&map.id, sizeof map.id, h);
  h = cw_hash32(&t->layers, sizeof t->layers, h);
  h = cw_hash32(&t->one_way_layer, sizeof t->one_way_layer, h);
  cw__tilemap_info in;
  if (!cw__tilemap2d_info(map, &in)) return h;
  aff m = map_to_body(n, map);
  h = cw_hash32(&m, sizeof m, h);
  h = cw_hash32(&in.epoch, sizeof in.epoch, h);
  h = cw_hash32(&in.tile_shapes, sizeof in.tile_shapes, h);
  h = cw_hash32(&in.tile_size, sizeof in.tile_size, h);
  h = cw_hash32(&in.cell_size, sizeof in.cell_size, h);
  h = cw_hash32(&in.layers, sizeof in.layers, h);
  return h;
}

/* ── a chunk's edges ──────────────────────────────────────────────────── */

enum { SIDE_NONE, SIDE_LEFT, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM };

static int side_of(const int32_t *a, const int32_t *b, int32_t ex, int32_t ey) {
  if (a[0] == b[0]) {
    if (a[0] == 0) return SIDE_LEFT;
    if (a[0] == ex) return SIDE_RIGHT;
  }
  if (a[1] == b[1]) {
    if (a[1] == 0) return SIDE_TOP;
    if (a[1] == ey) return SIDE_BOTTOM;
  }
  return SIDE_NONE;
}

/* [lo, hi] minus the cuts: writes the pieces left, returns how many */
static int subtract(int32_t lo, int32_t hi, int32_t (*cut)[2], int ncut, int32_t (*out)[2]) {
  int n = 1;
  out[0][0] = lo, out[0][1] = hi;
  for (int c = 0; c < ncut; c++) {
    int32_t tmp[CW__TILE_SHAPE_PTS + 2][2];
    int k = 0;
    for (int i = 0; i < n; i++) {
      int32_t l = out[i][0], h = out[i][1];
      if (cut[c][1] <= l || cut[c][0] >= h) { tmp[k][0] = l, tmp[k][1] = h, k++; continue; }
      if (cut[c][0] > l) tmp[k][0] = l, tmp[k][1] = cut[c][0], k++;
      if (cut[c][1] < h) tmp[k][0] = cut[c][1], tmp[k][1] = h, k++;
    }
    n = k;
    memcpy(out, tmp, sizeof(int32_t[2]) * (size_t)n);
  }
  return n;
}

static void push_edge(tc_edges *e, int32_t ax, int32_t ay, int32_t bx, int32_t by, bool owned) {
  if (ax == bx && ay == by) return;
  tc_edge ed = { ax, ay, bx, by, -1, -1, owned, 0 };
  cw_array_push(e, ed);
}

/* the solid and the one-way edges of the cells of chunk (cx, cy) and a ring of one cell round it */
static void gather(cw_node map, const cw__tilemap_info *in, int layer, int cx, int cy, tc_edges *solid, tc_edges *oneway) {
  int x0 = cx * CW_TILE_CHUNK, y0 = cy * CW_TILE_CHUNK;
  int x1 = CW_MIN(x0 + CW_TILE_CHUNK, in->w), y1 = CW_MIN(y0 + CW_TILE_CHUNK, in->h);
  int32_t ex = in->ext_x, ey = in->ext_y;
  static const int nb[5][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
  static const int opposite[5] = { SIDE_NONE, SIDE_RIGHT, SIDE_LEFT, SIDE_BOTTOM, SIDE_TOP };
  for (int y = y0 - 1; y < y1 + 1; y++)
    for (int x = x0 - 1; x < x1 + 1; x++) {
      cw__tile_shape s;
      cw__tilemap2d_cell_shape(map, layer, x, y, &s);
      if (!s.kind) continue;
      bool owned = x >= x0 && x < x1 && y >= y0 && y < y1;
      int32_t ox = x * ex, oy = y * ey;
      if (s.kind == 2) {
        push_edge(oneway, ox + s.pt[0][0], oy + s.pt[0][1], ox + s.pt[1][0], oy + s.pt[1][1], owned);
        continue;
      }
      cw__tile_shape ns[5];
      bool have[5] = { false, false, false, false, false };
      for (int i = 0; i < s.n; i++) {
        const int32_t *a = s.pt[i], *b = s.pt[(i + 1) % s.n];
        int side = side_of(a, b, ex, ey);
        if (side == SIDE_NONE) {
          push_edge(solid, ox + a[0], oy + a[1], ox + b[0], oy + b[1], owned);
          continue;
        }
        if (!have[side]) {
          cw__tilemap2d_cell_shape(map, layer, x + nb[side][0], y + nb[side][1], &ns[side]);
          have[side] = true;
        }
        int axis = side == SIDE_LEFT || side == SIDE_RIGHT ? 1 : 0;
        int32_t cut[CW__TILE_SHAPE_PTS][2];
        int ncut = 0;
        if (ns[side].kind == 1)
          for (int j = 0; j < ns[side].n; j++) {
            const int32_t *p = ns[side].pt[j], *q = ns[side].pt[(j + 1) % ns[side].n];
            if (side_of(p, q, ex, ey) != opposite[side]) continue;
            cut[ncut][0] = CW_MIN(p[axis], q[axis]), cut[ncut][1] = CW_MAX(p[axis], q[axis]), ncut++;
          }
        int32_t pieces[CW__TILE_SHAPE_PTS + 2][2];
        int np = subtract(CW_MIN(a[axis], b[axis]), CW_MAX(a[axis], b[axis]), cut, ncut, pieces);
        bool up = a[axis] < b[axis];
        for (int k = 0; k < np; k++) {
          int32_t from = up ? pieces[k][0] : pieces[k][1], to = up ? pieces[k][1] : pieces[k][0];
          if (axis) push_edge(solid, ox + a[0], oy + from, ox + a[0], oy + to, owned);
          else push_edge(solid, ox + from, oy + a[1], ox + to, oy + a[1], owned);
        }
      }
    }
}

/* ── joining edges end to start ───────────────────────────────────────── */

static const tc_edge *sort_edges;
static int by_start(const void *pa, const void *pb) {
  int32_t ia = *(const int32_t *)pa, ib = *(const int32_t *)pb;
  const tc_edge *a = &sort_edges[ia], *b = &sort_edges[ib];
  if (a->ax != b->ax) return a->ax < b->ax ? -1 : 1;
  if (a->ay != b->ay) return a->ay < b->ay ? -1 : 1;
  return ia < ib ? -1 : ia > ib ? 1 : 0; /* a total order: any sort gives the same array */
}

/* the turn from direction d1 to d2 as a number that grows with the signed angle in (-180, 180] degrees */
static double turn(int64_t d1x, int64_t d1y, int64_t d2x, int64_t d2y) {
  double cr = (double)(d1x * d2y - d1y * d2x), dt = (double)(d1x * d2x + d1y * d2y);
  double len = sqrt((double)(d1x * d1x + d1y * d1y) * (double)(d2x * d2x + d2y * d2y));
  double c = len > 0 ? dt / len : 1;
  return cr >= 0 ? 1.0 - c : c - 1.0;
}

static void stitch(tc_edges *e) {
  int n = e->count;
  if (!n) return;
  int32_t *order = cw_frame_alloc(sizeof(int32_t) * (size_t)n);
  for (int i = 0; i < n; i++) order[i] = i;
  sort_edges = e->data;
  qsort(order, (size_t)n, sizeof(int32_t), by_start);
  for (int i = 0; i < n; i++) {
    tc_edge *ed = &e->data[i];
    /* the first edge starting at this edge's end */
    int lo = 0, hi = n;
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      const tc_edge *m = &e->data[order[mid]];
      if (m->ax < ed->bx || (m->ax == ed->bx && m->ay < ed->by)) lo = mid + 1;
      else hi = mid;
    }
    int best = -1;
    double best_turn = 0;
    for (int k = lo; k < n; k++) {
      tc_edge *c = &e->data[order[k]];
      if (c->ax != ed->bx || c->ay != ed->by) break;
      if (c->prev >= 0) continue;
      /* where two outlines touch at a point, keep to this one: the sharpest turn toward the solid side
       * (the most negative angle with y down, the shoelace-negative winding) */
      double t = turn(ed->bx - ed->ax, ed->by - ed->ay, c->bx - c->ax, c->by - c->ay);
      if (best < 0 || t < best_turn) best = order[k], best_turn = t;
    }
    if (best >= 0) {
      ed->next = best;
      e->data[best].prev = i;
    }
  }
}

/* ── chains ───────────────────────────────────────────────────────────── */

typedef struct emit {
  cw__body_ctx *x;
  tilecol *t;
  tc_chunk *chunk;
  aff to_body;
  float ux, uy; /* shape units -> map-local pixels */
  cw_shape2d_def def;
  bool reverse; /* one-way runs: Box2D collides on the right of a segment, so a floor runs right to left (y up) */
} emit;

static cw_vec2 to_m(const emit *em, int32_t gx, int32_t gy) {
  cw_vec2 p = aff_apply(em->to_body, cw_v2((float)gx * em->ux, (float)gy * em->uy));
  return cw_v2(p.x / em->x->ppm, -p.y / em->x->ppm);
}

static void emit_chain(emit *em, const int32_t (*pts)[2], int n, bool loop) {
  if (n < (loop ? 3 : 4)) return;
  cw_vec2 *m = cw_frame_alloc(sizeof(cw_vec2) * (size_t)n);
  for (int i = 0; i < n; i++) {
    const int32_t *p = pts[em->reverse ? n - 1 - i : i];
    m[i] = to_m(em, p[0], p[1]);
  }
  int segs = 0;
  uint64_t id = cw__phys2d_ctx_chain(em->x, &em->def, m, n, loop, &segs);
  if (!id) return;
  cw_array_push(&em->chunk->chains, id);
  em->chunk->segments += segs;
}

static bool collinear(const int32_t *a, const int32_t *b, const int32_t *c) {
  int64_t ux = b[0] - a[0], uy = b[1] - a[1], vx = c[0] - b[0], vy = c[1] - b[1];
  return ux * vy - uy * vx == 0 && ux * vx + uy * vy > 0;
}

/* every run of owned edges becomes a chain: a loop when the run closes on itself, else an open chain with
 * the edges before and after it (or a straight continuation) as ghost vertices */
static void emit_runs(emit *em, tc_edges *e) {
  int n = e->count;
  for (int i = 0; i < n; i++) {
    if (!e->data[i].owned || e->data[i].done) continue;
    /* back to the run's first edge */
    int head = i;
    bool loop = false;
    for (;;) {
      int p = e->data[head].prev;
      if (p < 0 || !e->data[p].owned) break;
      if (p == i) { loop = true; break; }
      head = p;
    }
    if (loop) head = i;
    int count = 0;
    for (int k = head;;) {
      count++;
      e->data[k].done = 1;
      int nx = e->data[k].next;
      if (nx < 0 || !e->data[nx].owned || nx == head) break;
      k = nx;
    }
    /* the run's corners: a straight run is one segment */
    int32_t (*pts)[2] = cw_frame_alloc(sizeof(int32_t[2]) * (size_t)(count + 3));
    int np = loop ? 0 : 1, last = head;
    for (int k = head, c = 0; c < count; c++, k = e->data[k].next) {
      pts[np][0] = e->data[k].ax, pts[np][1] = e->data[k].ay, np++;
      last = k;
    }
    const tc_edge *h = &e->data[head], *l = &e->data[last];
    if (!loop) pts[np][0] = l->bx, pts[np][1] = l->by, np++;
    int first = loop ? 0 : 1, k = first;
    for (int j = first; j < np; j++) {
      const int32_t *prev = k > first ? pts[k - 1] : loop ? pts[np - 1] : NULL;
      const int32_t *next = j + 1 < np ? pts[j + 1] : loop ? pts[first] : NULL;
      if (prev && next && collinear(prev, pts[j], next)) continue;
      pts[k][0] = pts[j][0], pts[k][1] = pts[j][1], k++;
    }
    np = k;
    if (loop) {
      emit_chain(em, (const int32_t (*)[2])pts, np, true);
    } else {
      /* ghost vertices: the edges before and after the run (the next chunk's, usually), or straight on */
      if (h->prev >= 0) pts[0][0] = e->data[h->prev].ax, pts[0][1] = e->data[h->prev].ay;
      else pts[0][0] = 2 * h->ax - h->bx, pts[0][1] = 2 * h->ay - h->by;
      if (l->next >= 0) pts[np][0] = e->data[l->next].bx, pts[np][1] = e->data[l->next].by;
      else pts[np][0] = 2 * l->bx - l->ax, pts[np][1] = 2 * l->by - l->ay;
      np++;
      emit_chain(em, (const int32_t (*)[2])pts, np, false);
    }
  }
}

static tc_chunk *chunk_at(tilecol *t, int layer, int cx, int cy) { return &t->chunks[((size_t)layer * (size_t)t->ch + (size_t)cy) * (size_t)t->cw + (size_t)cx]; }

static void clear_chunk(cw__body_ctx *x, tc_chunk *c) {
  for (int i = 0; i < c->chains.count; i++) cw__phys2d_ctx_chain_destroy(x, c->chains.data[i]);
  cw_array_clear(&c->chains);
  c->segments = 0;
}

static void build_chunk(cw__body_ctx *x, tilecol *t, const cw__tilemap_info *in, aff to_body, int layer, int cx, int cy) {
  tc_chunk *c = chunk_at(t, layer, cx, cy);
  clear_chunk(x, c);
  t->builds++;
  tc_edges solid = { 0 }, oneway = { 0 };
  gather(t->map, in, layer, cx, cy, &solid, &oneway);
  emit em = { .x = x, .t = t, .chunk = c, .to_body = to_body, .ux = in->cell_size.x / (float)in->ext_x,
              .uy = in->cell_size.y / (float)in->ext_y, .def = x->def };
  if (solid.count) {
    stitch(&solid);
    emit_runs(&em, &solid);
  }
  if (oneway.count) {
    stitch(&oneway);
    if (t->one_way_layer >= 0) em.def.layer = t->one_way_layer & 31;
    em.reverse = true;
    emit_runs(&em, &oneway);
  }
  cw_array_free(&solid);
  cw_array_free(&oneway);
}

static bool layer_on(const tilecol *t, int l) { return (t->layers >> l) & 1; }

void cw__tilecol_build(cw__body_ctx *x) {
  tilecol *t = cw_node_data_t(x->node, cw__tilecol_t);
  free_chunks(t); /* the body's chains are gone already */
  t->map = find_map(x->node, t);
  cw__tilemap_info in;
  if (!cw__tilemap2d_info(t->map, &in)) {
    cw_error("TileCollision2D '%s': tilemap '%s' is not a TileMap2D (a path from this node; make it the map's child "
             "and leave it \"..\")", cw_node_name(x->node), t->tilemap ? t->tilemap : "");
    return;
  }
  t->serial = in.serial;
  if (!in.w || !in.h || in.ext_x <= 0 || in.ext_y <= 0) return;
  t->cw = (in.w + CW_TILE_CHUNK - 1) / CW_TILE_CHUNK;
  t->ch = (in.h + CW_TILE_CHUNK - 1) / CW_TILE_CHUNK;
  t->nl = in.layers;
  t->chunks = cw_calloc((size_t)t->nl * (size_t)t->cw * (size_t)t->ch, sizeof(tc_chunk), CW_MEM_PHYSICS);
  aff m = map_to_body(x->node, t->map);
  for (int l = 0; l < t->nl; l++) {
    if (!layer_on(t, l)) continue;
    for (int cy = 0; cy < t->ch; cy++)
      for (int cx = 0; cx < t->cw; cx++) build_chunk(x, t, &in, m, l, cx, cy);
  }
}

void cw__tilecol_update(cw__body_ctx *x) {
  tilecol *t = cw_node_data_t(x->node, cw__tilecol_t);
  cw__tilemap_info in;
  if (!t->chunks || !cw__tilemap2d_info(t->map, &in) || in.serial == t->serial) return;
  cw__tile_edit ed[256];
  int n = cw__tilemap2d_edits_since(t->map, t->serial, ed, 256);
  t->serial = in.serial;
  aff m = map_to_body(x->node, t->map);
  size_t total = (size_t)t->nl * (size_t)t->cw * (size_t)t->ch;
  uint8_t *dirty = cw_frame_alloc(total);
  memset(dirty, n < 0 ? 1 : 0, total);
  for (int i = 0; i < n; i++) {
    if (ed[i].layer >= t->nl) continue;
    /* the cell's chunk, and the neighbours whose ring of one cell it is in */
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        int nx = ed[i].x + dx, ny = ed[i].y + dy;
        if (nx < 0 || ny < 0) continue;
        int cx = nx / CW_TILE_CHUNK, cy = ny / CW_TILE_CHUNK;
        if (cx < t->cw && cy < t->ch) dirty[((size_t)ed[i].layer * (size_t)t->ch + (size_t)cy) * (size_t)t->cw + (size_t)cx] = 1;
      }
  }
  for (int l = 0; l < t->nl; l++) {
    if (!layer_on(t, l)) continue;
    for (int cy = 0; cy < t->ch; cy++)
      for (int cx = 0; cx < t->cw; cx++)
        if (dirty[((size_t)l * (size_t)t->ch + (size_t)cy) * (size_t)t->cw + (size_t)cx]) build_chunk(x, t, &in, m, l, cx, cy);
  }
}

bool cw__tilecol_restored(cw_node n, int body_chains) {
  tilecol *t = cw_node_data_t(n, cw__tilecol_t);
  int mine = 0;
  for (int i = 0; i < t->nl * t->cw * t->ch; i++)
    for (int k = 0; k < t->chunks[i].chains.count; k++, mine++)
      if (!b2Chain_IsValid(b2LoadChainId(t->chunks[i].chains.data[k]))) return false;
  return mine == body_chains;
}

/* ── the node ─────────────────────────────────────────────────────────── */

cw_tile_collision2d_stats cw_tile_collision2d_stats_get(cw_node n) {
  cw_tile_collision2d_stats s = { 0 };
  tilecol *t = cw__tilecol_t ? cw_node_data_t(n, cw__tilecol_t) : NULL;
  if (!t) return s;
  for (int i = 0; i < t->nl * t->cw * t->ch; i++) {
    s.chunks += t->chunks[i].chains.count > 0;
    s.chains += t->chunks[i].chains.count;
    s.segments += t->chunks[i].segments;
  }
  s.builds = t->builds;
  return s;
}

cw_node cw_tile_collision2d_map(cw_node n) {
  tilecol *t = cw__tilecol_t ? cw_node_data_t(n, cw__tilecol_t) : NULL;
  return t ? t->map : (cw_node){ 0 };
}

void cw_tile_collision2d_update(cw_node n) {
  if (cw__tilecol_t && cw_node_data_t(n, cw__tilecol_t)) cw__phys2d_sync_shapes(n);
}

void cw_tile_collision2d_cell_at(cw_node n, cw_vec2 global, cw_vec2 normal, int *x, int *y) {
  *x = *y = -1;
  tilecol *t = cw__tilecol_t ? cw_node_data_t(n, cw__tilecol_t) : NULL;
  if (!t || !cw_node_valid(t->map)) return;
  cw_vec2 cs = cw_tilemap2d_cell_size(t->map);
  float len = sqrtf(normal.x * normal.x + normal.y * normal.y), nudge = CW_MIN(cs.x, cs.y) * 0.125f;
  if (len > 0) global = cw_v2(global.x - normal.x / len * nudge, global.y - normal.y / len * nudge);
  cw_vec2 local = aff_apply(aff_inv(aff_of(cw_node2d_global_transform(t->map))), global);
  cw_tilemap2d_local_to_cell(t->map, local, x, y);
}

static const cw_prop tc_props[] = {
  CW_PROP_FIELD(tilecol, tilemap, CW_T_STRING, CW_PROP_DEFAULT, "Path to the TileMap2D, from this node (\"..\": the parent)."),
  CW_PROP_FIELD(tilecol, layers, CW_T_INT, CW_PROP_DEFAULT, "Bit k: map layer k collides (-1 = every layer)."),
  CW_PROP_FIELD_RANGE(tilecol, one_way_layer, CW_T_INT, -1, 31, "Physics layer of one-way platforms (-1 = collision_layer)."),
  CW_PROP_END,
};
static cw_node_type tc_type = { .name = "TileCollision2D", .parent = "StaticBody2D", .data_size = sizeof(tilecol), .props = tc_props,
                                .init = tc_init, .destroy = tc_destroy,
                                .doc = "Static colliders built from a TileMap2D's cells and tile_shapes: merged outlines per chunk, rebuilt where cells change." };

void cw__register_tile_collision_type(void) {
  cw_node_type_register(&tc_type);
  cw__tilecol_t = &tc_type;
}
