/* 2D navigation (nav2d.h): grids, A* and jump point search, flow fields and the 2D crowd.
 *
 * Costs are integers: 10 for a straight step and 14 for a diagonal one, times the cost of the cell entered.
 * The open list is a binary heap ordered by (total, distance still to go, cell index), a total order, so a
 * search expands the same cells in the same order on every host.
 *
 * Jump point search follows Harabor and Grastien (2011) with the rules for grids where a diagonal step needs
 * both cells beside it free, as in PathFinding.js's JPFMoveDiagonallyIfNoObstacles (MIT; NOTICE). */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "cartwheel/nav2d.h"
#include "nav_internal.h"
#include "third_party/rvo2/orca.h"

#define STRAIGHT 10u
#define DIAGONAL 14u
#define UNREACHED 0xffffffffu
#define MAX_PATH 48 /* waypoints an agent keeps */

typedef struct heap_item {
  uint32_t f, h;
  int32_t idx;
} heap_item;

/* one pool thread's search memory for a grid */
typedef struct search {
  uint32_t *g;
  int32_t *parent;
  uint32_t *stamp; /* == epoch: g and parent are this search's; bit 31 of g... closed is its own array */
  uint8_t *closed;
  uint32_t epoch;
  CW_ARRAY(heap_item) heap;
  CW_ARRAY(int32_t) chain;
  CW_ARRAY(cw_vec2) pts;
  int expanded;
} search;

typedef struct obstacle {
  uint32_t id;
  int kind; /* 0 circle, 1 rect */
  cw_vec2 center, half;
  float radius, rotation;
} obstacle;

typedef struct grid {
  int w, h;
  cw_vec2 cell, origin;
  bool diag;
  int layout; /* cw_tile_layout */
  int nn;     /* neighbours of a cell: 4, 8 or 6 (hexagons) */
  uint8_t *cost;   /* what the game or the bake set */
  uint16_t *cover; /* obstacles over each cell */
  uint8_t *walk;   /* the cost searches use: 0 where blocked or covered */
  int walkable, costly; /* walkable cells; walkable cells whose cost is not 1 */
  uint32_t version;
  search *searches;
  int nsearch;
  CW_ARRAY(obstacle) obstacles;
  uint32_t next_obstacle;
  CW_ARRAY(uint64_t) fields, crowds;
} grid;

static cw_pool g_grids, g_fields, g_crowds;
static bool g_init;

static void init_pools(void) {
  if (g_init) return;
  cw_pool_init(&g_grids, sizeof(grid), CW_MEM_NAV);
  g_init = true;
}

static grid *get(cw_navgrid h) { return g_init ? cw_pool_get(&g_grids, h.id) : NULL; }

static inline bool W(const grid *g, int x, int y) { return x >= 0 && y >= 0 && x < g->w && y < g->h && g->walk[y * g->w + x]; }
/* plain ground: what a straightened leg may cross on a grid with costs (a short cut through mud is no short cut) */
static inline bool P(const grid *g, int x, int y, bool plain) { return W(g, x, y) && (!plain || g->walk[y * g->w + x] == 1); }

/* ── grids ────────────────────────────────────────────────────────────── */

cw_navgrid cw_navgrid_new(const cw_navgrid_desc *d) {
  if (!d || d->width < 1 || d->height < 1 || d->width > 4096 || d->height > 4096) {
    cw_error("cw_navgrid_new: a grid is 1..4096 cells each way (got %d x %d)", d ? d->width : 0, d ? d->height : 0);
    return (cw_navgrid){ 0 };
  }
  init_pools();
  uint64_t id;
  grid *g = cw_pool_alloc(&g_grids, &id);
  memset(g, 0, sizeof *g);
  g->w = d->width, g->h = d->height;
  g->cell = cw_v2(d->cell.x > 0 ? d->cell.x : 1, d->cell.y > 0 ? d->cell.y : 1);
  g->origin = d->origin;
  g->diag = !d->no_diagonals;
  g->layout = (int)d->layout;
  bool hex = g->layout == CW_TILE_LAYOUT_HEX_ROWS || g->layout == CW_TILE_LAYOUT_HEX_COLUMNS;
  if (hex) g->diag = false; /* a hexagon has no corner neighbours */
  g->nn = hex ? 6 : g->diag ? 8 : 4;
  size_t n = (size_t)g->w * (size_t)g->h;
  g->cost = cw_alloc(n, CW_MEM_NAV);
  g->walk = cw_alloc(n, CW_MEM_NAV);
  g->cover = cw_calloc(n, sizeof(uint16_t), CW_MEM_NAV);
  memset(g->cost, 1, n);
  memset(g->walk, 1, n);
  g->walkable = (int)n;
  g->version = 1;
  g->nsearch = cw_jobs_thread_count() > 0 ? cw_jobs_thread_count() : 1;
  g->searches = cw_calloc((size_t)g->nsearch, sizeof(search), CW_MEM_NAV);
  return (cw_navgrid){ id };
}

static void field_destroy(uint64_t id);
static void crowd_destroy(uint64_t id);

void cw_navgrid_free(cw_navgrid h) {
  grid *g = get(h);
  if (!g) return;
  while (g->fields.count) field_destroy(g->fields.data[g->fields.count - 1]);
  while (g->crowds.count) crowd_destroy(g->crowds.data[g->crowds.count - 1]);
  for (int i = 0; i < g->nsearch; i++) {
    search *s = &g->searches[i];
    cw_free(s->g);
    cw_free(s->parent);
    cw_free(s->stamp);
    cw_free(s->closed);
    cw_array_free(&s->heap);
    cw_array_free(&s->chain);
    cw_array_free(&s->pts);
  }
  cw_free(g->searches);
  cw_free(g->cost);
  cw_free(g->walk);
  cw_free(g->cover);
  cw_array_free(&g->obstacles);
  cw_array_free(&g->fields);
  cw_array_free(&g->crowds);
  cw_pool_release(&g_grids, h.id);
}

bool cw_navgrid_valid(cw_navgrid h) { return get(h) != NULL; }

void cw_navgrid_size(cw_navgrid h, int *width, int *height) {
  grid *g = get(h);
  *width = g ? g->w : 0;
  *height = g ? g->h : 0;
}

cw_vec2 cw_navgrid_cell_size(cw_navgrid h) {
  grid *g = get(h);
  return g ? g->cell : cw_v2(0, 0);
}

/* a cell's effective cost changed: the counters and the version follow */
static void refresh(grid *g, int i) {
  uint8_t now = g->cover[i] ? 0 : g->cost[i], was = g->walk[i];
  if (now == was) return;
  g->walkable += (now != 0) - (was != 0);
  g->costly += (now > 1) - (was > 1);
  g->walk[i] = now;
  g->version++;
}

void cw_navgrid_set_cost(cw_navgrid h, int x, int y, uint8_t cost) {
  grid *g = get(h);
  if (!g || x < 0 || y < 0 || x >= g->w || y >= g->h) return;
  int i = y * g->w + x;
  g->cost[i] = cost;
  refresh(g, i);
}

uint8_t cw_navgrid_cost(cw_navgrid h, int x, int y) {
  grid *g = get(h);
  return g && x >= 0 && y >= 0 && x < g->w && y < g->h ? g->cost[y * g->w + x] : 0;
}

void cw_navgrid_set_costs(cw_navgrid h, const uint8_t *costs) {
  grid *g = get(h);
  if (!g || !costs) return;
  for (int i = 0; i < g->w * g->h; i++) {
    g->cost[i] = costs[i];
    refresh(g, i);
  }
}

bool cw_navgrid_walkable(cw_navgrid h, int x, int y) {
  grid *g = get(h);
  return g && W(g, x, y);
}

static inline bool is_hex(const grid *g) { return g->layout == CW_TILE_LAYOUT_HEX_ROWS || g->layout == CW_TILE_LAYOUT_HEX_COLUMNS; }
static inline void cell_of(const grid *g, cw_vec2 p, int *x, int *y) {
  if (g->layout != CW_TILE_LAYOUT_SQUARE) {
    cw_tile_layout_cell_at((cw_tile_layout)g->layout, g->cell.x, g->cell.y, cw_v2(p.x - g->origin.x, p.y - g->origin.y), x, y);
    return;
  }
  *x = (int)floorf((p.x - g->origin.x) / g->cell.x);
  *y = (int)floorf((p.y - g->origin.y) / g->cell.y);
}
static inline cw_vec2 center_of(const grid *g, int x, int y) {
  if (g->layout != CW_TILE_LAYOUT_SQUARE) {
    cw_vec2 o = cw_tile_layout_cell_origin((cw_tile_layout)g->layout, g->cell.x, g->cell.y, x, y);
    return cw_v2(g->origin.x + o.x + 0.5f * g->cell.x, g->origin.y + o.y + 0.5f * g->cell.y);
  }
  return cw_v2(g->origin.x + ((float)x + 0.5f) * g->cell.x, g->origin.y + ((float)y + 0.5f) * g->cell.y);
}
/* a position in cell space, where cell (x, y) is the unit square at (x, y): squares and diamonds only */
static inline void cell_space(const grid *g, cw_vec2 p, float *fx, float *fy) {
  float px = p.x - g->origin.x, py = p.y - g->origin.y;
  if (g->layout == CW_TILE_LAYOUT_ISOMETRIC) {
    float a = px / (g->cell.x * 0.5f), b = py / (g->cell.y * 0.5f);
    *fx = (a + b) * 0.5f, *fy = (b - a) * 0.5f;
  } else {
    *fx = px / g->cell.x, *fy = py / g->cell.y;
  }
}
static inline cw_vec2 from_cell_space(const grid *g, float fx, float fy) {
  if (g->layout == CW_TILE_LAYOUT_ISOMETRIC)
    return cw_v2(g->origin.x + (fx - fy) * g->cell.x * 0.5f, g->origin.y + (fx + fy) * g->cell.y * 0.5f);
  return cw_v2(g->origin.x + fx * g->cell.x, g->origin.y + fy * g->cell.y);
}
/* Neighbour k (1..g->nn) of a cell and what a step to it costs before the cell's own cost; false when the
 * step is not allowed (off the grid, blocked, or a diagonal past a blocked cell). Squares and diamonds: N, E,
 * S, W, then NE, SE, SW, NW. Hexagons: the two of its row (or column), then the four of the rows beside it. */
static inline bool neighbour(const grid *g, int x, int y, int k, int *nx, int *ny, uint32_t *step) {
  static const int sq[9][2] = { { 0, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 }, { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 } };
  if (g->layout == CW_TILE_LAYOUT_HEX_ROWS) {
    static const int even[7][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { -1, 1 }, { 0, 1 } };
    static const int odd[7][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, -1 }, { 1, -1 }, { 0, 1 }, { 1, 1 } };
    const int (*t)[2] = (y & 1) ? odd : even;
    *nx = x + t[k][0], *ny = y + t[k][1];
  } else if (g->layout == CW_TILE_LAYOUT_HEX_COLUMNS) {
    static const int even[7][2] = { { 0, 0 }, { 0, 1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { 1, -1 }, { 1, 0 } };
    static const int odd[7][2] = { { 0, 0 }, { 0, 1 }, { 0, -1 }, { -1, 0 }, { -1, 1 }, { 1, 0 }, { 1, 1 } };
    const int (*t)[2] = (x & 1) ? odd : even;
    *nx = x + t[k][0], *ny = y + t[k][1];
  } else {
    *nx = x + sq[k][0], *ny = y + sq[k][1];
    if (k > 4) {
      *step = 14u;
      return W(g, *nx, *ny) && W(g, *nx, y) && W(g, x, *ny);
    }
  }
  *step = 10u;
  return W(g, *nx, *ny);
}

void cw_navgrid_cell_at(cw_navgrid h, cw_vec2 p, int *x, int *y) {
  grid *g = get(h);
  *x = *y = 0;
  if (g) cell_of(g, p, x, y);
}

cw_vec2 cw_navgrid_cell_center(cw_navgrid h, int x, int y) {
  grid *g = get(h);
  return g ? center_of(g, x, y) : cw_v2(0, 0);
}

uint32_t cw_navgrid_version(cw_navgrid h) {
  grid *g = get(h);
  return g ? g->version : 0;
}

uint64_t cw_navgrid_hash(cw_navgrid h) {
  grid *g = get(h);
  if (!g) return 0;
  uint64_t hh = 1469598103934665603ull;
  int32_t dims[2] = { g->w, g->h };
  const uint8_t *d = (const uint8_t *)dims;
  for (size_t i = 0; i < sizeof dims; i++) hh = (hh ^ d[i]) * 1099511628211ull;
  for (int i = 0; i < g->w * g->h; i++) hh = (hh ^ g->walk[i]) * 1099511628211ull;
  return hh;
}

cw_navgrid_stats cw_navgrid_get_stats(cw_navgrid h) {
  cw_navgrid_stats s = { 0 };
  grid *g = get(h);
  if (!g) return s;
  s.cells = g->w * g->h;
  s.walkable = g->walkable;
  s.jump_points = g->diag && g->costly == 0 && !is_hex(g);
  int wi = cw_jobs_worker_index();
  s.last_expanded = wi >= 0 && wi < g->nsearch ? g->searches[wi].expanded : 0;
  return s;
}

/* ── obstacles ────────────────────────────────────────────────────────── */

static bool obstacle_has(const obstacle *o, cw_vec2 p) {
  float dx = p.x - o->center.x, dy = p.y - o->center.y;
  if (o->kind == 0) return dx * dx + dy * dy <= o->radius * o->radius;
  float s, c;
  cw_sincos(-o->rotation, &s, &c);
  float lx = dx * c - dy * s, ly = dx * s + dy * c;
  return fabsf(lx) <= o->half.x && fabsf(ly) <= o->half.y;
}

static void obstacle_stamp(grid *g, const obstacle *o, int delta) {
  float r = o->kind == 0 ? o->radius : sqrtf(o->half.x * o->half.x + o->half.y * o->half.y);
  /* the cells of the shape's box: of its four corners (a box is not a box of cells on diamonds), and one more */
  int x0 = g->w, y0 = g->h, x1 = -1, y1 = -1;
  for (int k = 0; k < 4; k++) {
    int cx, cy;
    cell_of(g, cw_v2(o->center.x + ((k & 1) ? r : -r), o->center.y + ((k & 2) ? r : -r)), &cx, &cy);
    x0 = CW_MIN(x0, cx), y0 = CW_MIN(y0, cy), x1 = CW_MAX(x1, cx), y1 = CW_MAX(y1, cy);
  }
  if (g->layout != CW_TILE_LAYOUT_SQUARE) x0--, y0--, x1++, y1++;
  x0 = CW_MAX(x0, 0), y0 = CW_MAX(y0, 0), x1 = CW_MIN(x1, g->w - 1), y1 = CW_MIN(y1, g->h - 1);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      if (!obstacle_has(o, center_of(g, x, y))) continue;
      int i = y * g->w + x;
      g->cover[i] = (uint16_t)((int)g->cover[i] + delta);
      refresh(g, i);
    }
}

static uint32_t obstacle_add(cw_navgrid h, obstacle o) {
  grid *g = get(h);
  if (!g) return 0;
  o.id = ++g->next_obstacle;
  cw_array_push(&g->obstacles, o);
  obstacle_stamp(g, &o, 1);
  return o.id;
}

uint32_t cw_navgrid_obstacle_add_circle(cw_navgrid h, cw_vec2 center, float radius) {
  return obstacle_add(h, (obstacle){ .kind = 0, .center = center, .radius = radius });
}

uint32_t cw_navgrid_obstacle_add_rect(cw_navgrid h, cw_vec2 center, cw_vec2 half, float rotation) {
  return obstacle_add(h, (obstacle){ .kind = 1, .center = center, .half = half, .rotation = rotation });
}

bool cw_navgrid_obstacle_remove(cw_navgrid h, uint32_t id) {
  grid *g = get(h);
  if (!g || !id) return false;
  for (int i = 0; i < g->obstacles.count; i++)
    if (g->obstacles.data[i].id == id) {
      obstacle_stamp(g, &g->obstacles.data[i], -1);
      memmove(&g->obstacles.data[i], &g->obstacles.data[i + 1], sizeof(obstacle) * (size_t)(g->obstacles.count - i - 1));
      g->obstacles.count--;
      return true;
    }
  return false;
}

/* ── the open list ────────────────────────────────────────────────────── */

static inline bool item_less(heap_item a, heap_item b) {
  if (a.f != b.f) return a.f < b.f;
  if (a.h != b.h) return a.h < b.h;
  return a.idx < b.idx;
}

static void heap_push(search *s, heap_item it) {
  cw_array_push(&s->heap, it);
  heap_item *d = s->heap.data;
  int i = s->heap.count - 1;
  while (i > 0) {
    int p = (i - 1) / 2;
    if (!item_less(d[i], d[p])) break;
    heap_item t = d[i];
    d[i] = d[p], d[p] = t;
    i = p;
  }
}

static heap_item heap_pop(search *s) {
  heap_item *d = s->heap.data, top = d[0];
  int n = --s->heap.count;
  d[0] = d[n];
  for (int i = 0;;) {
    int l = 2 * i + 1, r = l + 1, m = i;
    if (l < n && item_less(d[l], d[m])) m = l;
    if (r < n && item_less(d[r], d[m])) m = r;
    if (m == i) break;
    heap_item t = d[i];
    d[i] = d[m], d[m] = t;
    i = m;
  }
  return top;
}

static search *search_for(grid *g) {
  int wi = cw_jobs_worker_index();
  if (wi < 0 || wi >= g->nsearch) wi = 0;
  search *s = &g->searches[wi];
  size_t n = (size_t)g->w * (size_t)g->h;
  if (!s->g) {
    s->g = cw_alloc(n * sizeof(uint32_t), CW_MEM_NAV);
    s->parent = cw_alloc(n * sizeof(int32_t), CW_MEM_NAV);
    s->stamp = cw_calloc(n, sizeof(uint32_t), CW_MEM_NAV);
    s->closed = cw_alloc(n, CW_MEM_NAV);
  }
  if (++s->epoch == 0) { /* wrapped: every stamp is stale */
    memset(s->stamp, 0, n * sizeof(uint32_t));
    s->epoch = 1;
  }
  cw_array_clear(&s->heap);
  s->expanded = 0;
  return s;
}

static inline uint32_t octile(int dx, int dy) {
  dx = dx < 0 ? -dx : dx, dy = dy < 0 ? -dy : dy;
  int lo = CW_MIN(dx, dy), hi = CW_MAX(dx, dy);
  return DIAGONAL * (uint32_t)lo + STRAIGHT * (uint32_t)(hi - lo);
}
static inline uint32_t heuristic(const grid *g, int x, int y, int gx, int gy) {
  if (is_hex(g)) {
    /* offset to axial coordinates, then the hexagon distance */
    bool rows = g->layout == CW_TILE_LAYOUT_HEX_ROWS;
    int q0 = rows ? x - (y - (y & 1)) / 2 : x, r0 = rows ? y : y - (x - (x & 1)) / 2;
    int q1 = rows ? gx - (gy - (gy & 1)) / 2 : gx, r1 = rows ? gy : gy - (gx - (gx & 1)) / 2;
    int dq = q0 - q1, dr = r0 - r1, ds = dq + dr;
    dq = dq < 0 ? -dq : dq, dr = dr < 0 ? -dr : dr, ds = ds < 0 ? -ds : ds;
    return STRAIGHT * (uint32_t)((dq + dr + ds) / 2);
  }
  int dx = x - gx, dy = y - gy;
  if (g->diag) return octile(dx, dy);
  return STRAIGHT * (uint32_t)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
}

static inline void relax(const grid *g, search *s, int from, uint32_t gfrom, int nx, int ny, uint32_t step, int gx, int gy) {
  int ni = ny * g->w + nx;
  if (s->stamp[ni] == s->epoch && s->closed[ni]) return;
  uint32_t ng = gfrom + step;
  if (s->stamp[ni] == s->epoch && s->g[ni] <= ng) return;
  s->stamp[ni] = s->epoch;
  s->closed[ni] = 0;
  s->g[ni] = ng;
  s->parent[ni] = from;
  uint32_t hh = heuristic(g, nx, ny, gx, gy);
  heap_push(s, (heap_item){ ng + hh, hh, ni });
}

/* ── jump point search ────────────────────────────────────────────────── */

/* the next jump point from (x, y), reached from (px, py), or -1 */
static int jump(const grid *g, int x, int y, int px, int py, int gx, int gy) {
  for (;;) {
    int dx = x - px, dy = y - py;
    if (!W(g, x, y)) return -1;
    if (x == gx && y == gy) return y * g->w + x;
    if (dx && dy) {
      if (jump(g, x + dx, y, x, y, gx, gy) >= 0 || jump(g, x, y + dy, x, y, gx, gy) >= 0) return y * g->w + x;
      if (!(W(g, x + dx, y) && W(g, x, y + dy))) return -1;
    } else if (dx) {
      if ((W(g, x, y - 1) && !W(g, x - dx, y - 1)) || (W(g, x, y + 1) && !W(g, x - dx, y + 1))) return y * g->w + x;
    } else {
      if ((W(g, x - 1, y) && !W(g, x - 1, y - dy)) || (W(g, x + 1, y) && !W(g, x + 1, y - dy))) return y * g->w + x;
    }
    px = x, py = y;
    x += dx, y += dy;
  }
}

static inline int sgn(int v) { return (v > 0) - (v < 0); }

static void jps_expand(const grid *g, search *s, int idx, int gx, int gy) {
  int x = idx % g->w, y = idx / g->w;
  int nb[8][2], n = 0;
  int p = s->parent[idx];
#define PUSH(X, Y) (nb[n][0] = (X), nb[n][1] = (Y), n++)
  if (p >= 0) {
    int dx = sgn(x - p % g->w), dy = sgn(y - p / g->w);
    if (dx && dy) {
      bool a = W(g, x, y + dy), b = W(g, x + dx, y);
      if (a) PUSH(x, y + dy);
      if (b) PUSH(x + dx, y);
      if (a && b) PUSH(x + dx, y + dy);
    } else if (dx) {
      bool next = W(g, x + dx, y), top = W(g, x, y + 1), bot = W(g, x, y - 1);
      if (next) {
        PUSH(x + dx, y);
        if (top) PUSH(x + dx, y + 1);
        if (bot) PUSH(x + dx, y - 1);
      }
      if (top) PUSH(x, y + 1);
      if (bot) PUSH(x, y - 1);
    } else {
      bool next = W(g, x, y + dy), right = W(g, x + 1, y), left = W(g, x - 1, y);
      if (next) {
        PUSH(x, y + dy);
        if (right) PUSH(x + 1, y + dy);
        if (left) PUSH(x - 1, y + dy);
      }
      if (right) PUSH(x + 1, y);
      if (left) PUSH(x - 1, y);
    }
  } else {
    bool up = W(g, x, y - 1), rt = W(g, x + 1, y), dn = W(g, x, y + 1), lf = W(g, x - 1, y);
    if (up) PUSH(x, y - 1);
    if (rt) PUSH(x + 1, y);
    if (dn) PUSH(x, y + 1);
    if (lf) PUSH(x - 1, y);
    if (up && rt) PUSH(x + 1, y - 1);
    if (dn && rt) PUSH(x + 1, y + 1);
    if (dn && lf) PUSH(x - 1, y + 1);
    if (up && lf) PUSH(x - 1, y - 1);
  }
#undef PUSH
  for (int k = 0; k < n; k++) {
    int j = jump(g, nb[k][0], nb[k][1], x, y, gx, gy);
    if (j < 0) continue;
    int jx = j % g->w, jy = j / g->w;
    relax(g, s, idx, s->g[idx], jx, jy, octile(jx - x, jy - y), gx, gy);
  }
}

static void astar_expand(const grid *g, search *s, int idx, int gx, int gy) {
  int x = idx % g->w, y = idx / g->w;
  uint32_t gi = s->g[idx];
  for (int k = 1; k <= g->nn; k++) {
    int nx, ny;
    uint32_t step;
    if (neighbour(g, x, y, k, &nx, &ny, &step)) relax(g, s, idx, gi, nx, ny, step * g->walk[ny * g->w + nx], gx, gy);
  }
}

/* The search from cell s to cell e. Fills search->chain with the cells of the path, goal first (jump points,
 * or every cell of a plain A* path), and returns its cost; UNREACHED with an empty chain when the start is
 * blocked. *partial: the chain ends at the cell nearest the goal that was reached. */
static uint32_t find_cells(grid *g, search *s, int sx, int sy, int ex, int ey, int max_iters, bool *partial, bool force_astar) {
  cw_array_clear(&s->chain);
  *partial = false;
  if (!W(g, sx, sy)) return UNREACHED;
  bool jps = g->diag && g->costly == 0 && !force_astar && !is_hex(g);
  int si = sy * g->w + sx, ei = ey * g->w + ex;
  s->stamp[si] = s->epoch;
  s->closed[si] = 0;
  s->g[si] = 0;
  s->parent[si] = -1;
  uint32_t h0 = heuristic(g, sx, sy, ex, ey);
  heap_push(s, (heap_item){ h0, h0, si });
  int best = si;
  uint32_t best_h = h0;
  bool found = false;
  while (s->heap.count) {
    heap_item it = heap_pop(s);
    if (s->closed[it.idx] || it.f != s->g[it.idx] + it.h) continue; /* a stale entry */
    s->closed[it.idx] = 1;
    if (it.idx == ei) {
      found = true;
      break;
    }
    if (it.h < best_h || (it.h == best_h && it.idx < best)) best = it.idx, best_h = it.h;
    if (max_iters > 0 && s->expanded >= max_iters) break;
    s->expanded++;
    if (jps) jps_expand(g, s, it.idx, ex, ey);
    else astar_expand(g, s, it.idx, ex, ey);
  }
  int end = found ? ei : best;
  *partial = !found;
  for (int i = end; i >= 0; i = s->parent[i]) cw_array_push(&s->chain, i);
  return s->g[end];
}

/* ── sight ────────────────────────────────────────────────────────────── */

/* walks the cells from a to b; false at the first blocked one (*t: how far along it got, 0..1). plain: cells
 * with a cost of their own stop it too */
static bool sight(const grid *g, cw_vec2 a, cw_vec2 b, bool plain, float *t_hit) {
  if (t_hit) *t_hit = 0;
  if (is_hex(g)) {
    /* hexagons: only within one cell (paths keep their cell middles) */
    int hx0, hy0, hx1, hy1;
    cell_of(g, a, &hx0, &hy0);
    cell_of(g, b, &hx1, &hy1);
    bool same = hx0 == hx1 && hy0 == hy1 && P(g, hx0, hy0, plain);
    if (same && t_hit) *t_hit = 1;
    return same;
  }
  float ax, ay, bx, by;
  cell_space(g, a, &ax, &ay);
  cell_space(g, b, &bx, &by);
  int x = (int)floorf(ax), y = (int)floorf(ay), ex = (int)floorf(bx), ey = (int)floorf(by);
  if (!P(g, x, y, plain)) return false;
  float dx = bx - ax, dy = by - ay;
  int stepx = dx > 0 ? 1 : dx < 0 ? -1 : 0, stepy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
  float tdx = stepx ? fabsf(1.0f / dx) : FLT_MAX, tdy = stepy ? fabsf(1.0f / dy) : FLT_MAX;
  float tmx = stepx > 0 ? ((float)(x + 1) - ax) / dx : stepx < 0 ? ((float)x - ax) / dx : FLT_MAX;
  float tmy = stepy > 0 ? ((float)(y + 1) - ay) / dy : stepy < 0 ? ((float)y - ay) / dy : FLT_MAX;
  for (int guard = g->w + g->h + 4; guard > 0 && (x != ex || y != ey); guard--) {
    float t;
    if (tmx < tmy) {
      t = tmx, x += stepx, tmx += tdx;
    } else if (tmy < tmx) {
      t = tmy, y += stepy, tmy += tdy;
    } else {
      /* through a corner: both cells beside it must be free */
      t = tmx;
      if (!P(g, x + stepx, y, plain) || !P(g, x, y + stepy, plain)) {
        if (t_hit) *t_hit = t;
        return false;
      }
      x += stepx, y += stepy, tmx += tdx, tmy += tdy;
    }
    if (t > 1.0f) break;
    if (!P(g, x, y, plain)) {
      if (t_hit) *t_hit = t;
      return false;
    }
  }
  if (t_hit) *t_hit = 1;
  return true;
}

static bool sight_wide(const grid *g, cw_vec2 a, cw_vec2 b, float radius, bool plain, float *t_hit) {
  float t = 1, tt;
  bool ok = sight(g, a, b, plain, &tt);
  t = tt;
  if (radius > 0) {
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (len > 0) {
      cw_vec2 n = cw_v2(-dy / len * radius, dx / len * radius);
      bool l = sight(g, cw_v2(a.x + n.x, a.y + n.y), cw_v2(b.x + n.x, b.y + n.y), plain, &tt);
      t = CW_MIN(t, tt);
      bool r = sight(g, cw_v2(a.x - n.x, a.y - n.y), cw_v2(b.x - n.x, b.y - n.y), plain, &tt);
      t = CW_MIN(t, tt);
      ok = ok && l && r;
    }
  }
  if (t_hit) *t_hit = t;
  return ok;
}

bool cw_navgrid_raycast(cw_navgrid h, cw_vec2 from, cw_vec2 to, float radius, cw_vec2 *hit) {
  grid *g = get(h);
  if (!g) return false;
  float t;
  bool ok = sight_wide(g, from, to, radius, false, &t);
  if (hit) *hit = ok ? to : cw_v2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
  return ok;
}

/* the walkable cell nearest p within max_distance: rings outward, the nearest middle, then the lower index */
static bool nearest_cell(const grid *g, cw_vec2 p, float max_distance, int *ox, int *oy) {
  int cx, cy;
  cell_of(g, p, &cx, &cy);
  if (W(g, cx, cy)) {
    *ox = cx, *oy = cy;
    return true;
  }
  float cmin = CW_MIN(g->cell.x, g->cell.y);
  if (max_distance <= 0) max_distance = 4 * cmin;
  int rings = (int)(max_distance / cmin) + 1;
  bool found = false;
  float best = FLT_MAX;
  for (int r = 1; r <= rings; r++) {
    for (int y = cy - r; y <= cy + r; y++)
      for (int x = cx - r; x <= cx + r; x++) {
        if (CW_MAX(abs(x - cx), abs(y - cy)) != r || !W(g, x, y)) continue;
        cw_vec2 c = center_of(g, x, y);
        float d = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y);
        if (d < best) best = d, *ox = x, *oy = y, found = true;
      }
    /* a ring further out cannot hold a nearer middle than r - 1 cells away */
    if (found && best <= (float)r * cmin * (float)r * cmin) break;
  }
  return found && best <= max_distance * max_distance;
}

bool cw_navgrid_nearest(cw_navgrid h, cw_vec2 p, float max_distance, cw_vec2 *out) {
  grid *g = get(h);
  int x, y;
  if (!g || !nearest_cell(g, p, max_distance, &x, &y)) return false;
  int cx, cy;
  cell_of(g, p, &cx, &cy);
  *out = cx == x && cy == y ? p : center_of(g, x, y);
  return true;
}

/* ── paths ────────────────────────────────────────────────────────────── */

static int path_in(grid *g, search *s, cw_vec2 from, cw_vec2 to, const cw_navgrid_query *q, cw_vec2 *pts, int max, bool *partial) {
  static const cw_navgrid_query defaults = { 0 };
  if (!q) q = &defaults;
  if (partial) *partial = false;
  if (max < 1) return 0;
  int sx, sy, ex, ey, fx, fy, tx, ty;
  if (!nearest_cell(g, from, q->max_snap, &sx, &sy) || !nearest_cell(g, to, q->max_snap, &ex, &ey)) return 0;
  cell_of(g, from, &fx, &fy);
  cell_of(g, to, &tx, &ty);
  if (fx != sx || fy != sy) from = center_of(g, sx, sy);
  if (tx != ex || ty != ey) to = center_of(g, ex, ey);
  bool part;
  find_cells(g, s, sx, sy, ex, ey, q->max_iters, &part, false);
  /* corners: where it starts, the middle of each cell of the chain, where it ends */
  cw_array_clear(&s->pts);
  cw_array_push(&s->pts, from);
  for (int i = s->chain.count - 1; i >= 0; i--) cw_array_push(&s->pts, center_of(g, s->chain.data[i] % g->w, s->chain.data[i] / g->w));
  if (!part) cw_array_push(&s->pts, to);
  cw_vec2 *p = s->pts.data;
  int n = s->pts.count, out = 0;
  pts[out++] = p[0];
  if (q->keep_cells) {
    for (int i = 1; i < n && out < max; i++) {
      if (p[i].x == pts[out - 1].x && p[i].y == pts[out - 1].y) continue;
      pts[out++] = p[i];
    }
    if (partial) *partial = part || (out == max && (pts[out - 1].x != p[n - 1].x || pts[out - 1].y != p[n - 1].y));
    return out;
  }
  /* pull it straight: from each corner kept, on past every later corner while the next is still in sight
   * (one sight test a corner: linear in the path) */
  int i = 0;
  bool cut = false;
  while (i < n - 1) {
    int j = i + 1;
    while (j < n - 1 && sight_wide(g, p[i], p[j + 1], q->agent_radius, g->costly > 0, NULL)) j++;
    if (p[j].x != pts[out - 1].x || p[j].y != pts[out - 1].y) {
      if (out == max) {
        cut = true;
        break;
      }
      pts[out++] = p[j];
    }
    i = j;
  }
  if (partial) *partial = part || cut;
  return out;
}

int cw_navgrid_find_path(cw_navgrid h, cw_vec2 from, cw_vec2 to, const cw_navgrid_query *q, cw_vec2 *pts, int max, bool *partial) {
  grid *g = get(h);
  if (partial) *partial = false;
  if (!g || !pts) return 0;
  return path_in(g, search_for(g), from, to, q, pts, max, partial);
}

float cw_navgrid_path_length(const cw_vec2 *pts, int count) {
  float len = 0;
  for (int i = 1; i < count; i++) {
    float dx = pts[i].x - pts[i - 1].x, dy = pts[i].y - pts[i - 1].y;
    len += sqrtf(dx * dx + dy * dy);
  }
  return len;
}

typedef struct paths_job {
  grid *g;
  const cw_navgrid_path_req *reqs;
  const cw_navgrid_query *q;
  cw_vec2 *pts;
  int max_points;
  int *counts;
} paths_job;

static void paths_range(int b, int e, int w, void *ctx) {
  (void)w;
  paths_job *j = ctx;
  for (int i = b; i < e; i++)
    j->counts[i] = path_in(j->g, search_for(j->g), j->reqs[i].from, j->reqs[i].to, j->q, j->pts + (size_t)i * (size_t)j->max_points,
                           j->max_points, NULL);
}

void cw_navgrid_find_paths(cw_navgrid h, const cw_navgrid_path_req *reqs, int count, const cw_navgrid_query *q, cw_vec2 *pts,
                           int max_points, int *counts) {
  grid *g = get(h);
  if (!g || count <= 0) return;
  paths_job j = { g, reqs, q, pts, max_points, counts };
  cw_parallel_for(count, 1, paths_range, &j);
}

/* test hooks (tests/unit/test_nav2d.c): the cost of the cell path, by jump points or by plain A* */
uint32_t cw__navgrid_path_cost(cw_navgrid h, int sx, int sy, int ex, int ey, bool plain, int *expanded) {
  grid *g = get(h);
  if (!g) return UNREACHED;
  search *s = search_for(g);
  bool partial;
  uint32_t c = find_cells(g, s, sx, sy, ex, ey, 0, &partial, plain);
  if (expanded) *expanded = s->expanded;
  return partial ? UNREACHED : c;
}

/* ── flow fields ──────────────────────────────────────────────────────── */

typedef struct field {
  uint64_t grid;
  uint32_t *dist;
  uint8_t *dir; /* 0 none, 1..8: N, E, S, W, NE, SE, SW, NW */
  uint32_t version; /* the grid's at the last build (0: never built) */
  CW_ARRAY(heap_item) heap;
} field;



static field *fget(cw_flowfield h) { return g_fields.elem_size ? cw_pool_get(&g_fields, h.id) : NULL; }

cw_flowfield cw_flowfield_new(cw_navgrid gh) {
  grid *g = get(gh);
  if (!g) return (cw_flowfield){ 0 };
  if (!g_fields.elem_size) cw_pool_init(&g_fields, sizeof(field), CW_MEM_NAV);
  uint64_t id;
  field *f = cw_pool_alloc(&g_fields, &id);
  memset(f, 0, sizeof *f);
  f->grid = gh.id;
  size_t n = (size_t)g->w * (size_t)g->h;
  f->dist = cw_alloc(n * sizeof(uint32_t), CW_MEM_NAV);
  f->dir = cw_calloc(n, 1, CW_MEM_NAV);
  for (size_t i = 0; i < n; i++) f->dist[i] = UNREACHED;
  cw_array_push(&g->fields, id);
  return (cw_flowfield){ id };
}

static void field_destroy(uint64_t id) {
  field *f = cw_pool_get(&g_fields, id);
  if (!f) return;
  grid *g = cw_pool_get(&g_grids, f->grid);
  if (g)
    for (int i = 0; i < g->fields.count; i++)
      if (g->fields.data[i] == id) {
        g->fields.data[i] = g->fields.data[--g->fields.count];
        break;
      }
  cw_free(f->dist);
  cw_free(f->dir);
  cw_array_free(&f->heap);
  cw_pool_release(&g_fields, id);
}

void cw_flowfield_free(cw_flowfield h) {
  if (fget(h)) field_destroy(h.id);
}
bool cw_flowfield_valid(cw_flowfield h) { return fget(h) != NULL; }

typedef struct dir_job {
  const grid *g;
  field *f;
} dir_job;

static void dir_range(int b, int e, int w, void *ctx) {
  (void)w;
  dir_job *j = ctx;
  const grid *g = j->g;
  for (int y = b; y < e; y++)
    for (int x = 0; x < g->w; x++) {
      int i = y * g->w + x;
      uint8_t best = 0;
      uint32_t bd = j->f->dist[i];
      if (bd != UNREACHED && bd != 0)
        for (int k = 1; k <= g->nn; k++) {
          int nx, ny;
          uint32_t step;
          if (!neighbour(g, x, y, k, &nx, &ny, &step)) continue;
          uint32_t d = j->f->dist[ny * g->w + nx];
          if (d < bd) bd = d, best = (uint8_t)k; /* the first of equals wins: straight steps before diagonal ones */
        }
      j->f->dir[i] = best;
    }
}

void cw_flowfield_build(cw_flowfield h, const cw_vec2 *goals, int count) {
  field *f = fget(h);
  grid *g = f ? cw_pool_get(&g_grids, f->grid) : NULL;
  if (!g) return;
  int n = g->w * g->h;
  for (int i = 0; i < n; i++) f->dist[i] = UNREACHED;
  cw_array_clear(&f->heap);
  search tmp = { 0 }; /* the heap helpers take a search: lend them this field's heap */
  tmp.heap.data = f->heap.data, tmp.heap.cap = f->heap.cap;
  for (int i = 0; i < count; i++) {
    int x, y;
    cell_of(g, goals[i], &x, &y);
    if (!W(g, x, y) || f->dist[y * g->w + x] == 0) continue;
    f->dist[y * g->w + x] = 0;
    heap_push(&tmp, (heap_item){ 0, 0, y * g->w + x });
  }
  while (tmp.heap.count) {
    heap_item it = heap_pop(&tmp);
    if (it.f != f->dist[it.idx]) continue;
    int x = it.idx % g->w, y = it.idx / g->w;
    /* outward from the goals: stepping from a neighbour INTO this cell costs this cell's cost */
    uint32_t step_cost = g->walk[it.idx];
    for (int k = 1; k <= g->nn; k++) {
      int nx, ny;
      uint32_t step;
      if (!neighbour(g, x, y, k, &nx, &ny, &step)) continue;
      uint32_t nd = it.f + step * step_cost;
      int ni = ny * g->w + nx;
      if (nd < f->dist[ni]) {
        f->dist[ni] = nd;
        heap_push(&tmp, (heap_item){ nd, 0, ni });
      }
    }
  }
  f->heap.data = tmp.heap.data, f->heap.cap = tmp.heap.cap, f->heap.count = 0;
  dir_job j = { g, f };
  cw_parallel_for(g->h, 8, dir_range, &j);
  f->version = g->version;
}

bool cw_flowfield_stale(cw_flowfield h) {
  field *f = fget(h);
  grid *g = f ? cw_pool_get(&g_grids, f->grid) : NULL;
  return !g || f->version != g->version;
}

static const float DIAG = 0.70710678f;
static const float UNIT[9][2] = { { 0, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0.70710678f, -0.70710678f },
                                  { 0.70710678f, 0.70710678f }, { -0.70710678f, 0.70710678f }, { -0.70710678f, -0.70710678f } };

cw_vec2 cw_flowfield_direction(cw_flowfield h, cw_vec2 p) {
  field *f = fget(h);
  grid *g = f ? cw_pool_get(&g_grids, f->grid) : NULL;
  if (!g) return cw_v2(0, 0);
  int x, y;
  cell_of(g, p, &x, &y);
  if (x < 0 || y < 0 || x >= g->w || y >= g->h) return cw_v2(0, 0);
  int k = f->dir[y * g->w + x];
  (void)DIAG;
  if (g->layout == CW_TILE_LAYOUT_SQUARE || !k) return cw_v2(UNIT[k][0], UNIT[k][1]);
  /* other layouts: toward the middle of the neighbour it names */
  int nx, ny;
  uint32_t step;
  neighbour(g, x, y, k, &nx, &ny, &step);
  cw_vec2 a = center_of(g, x, y), b = center_of(g, nx, ny);
  float dx = b.x - a.x, dy = b.y - a.y, d = sqrtf(dx * dx + dy * dy);
  return d > 0 ? cw_v2(dx / d, dy / d) : cw_v2(0, 0);
}

float cw_flowfield_distance(cw_flowfield h, cw_vec2 p) {
  field *f = fget(h);
  grid *g = f ? cw_pool_get(&g_grids, f->grid) : NULL;
  if (!g) return -1;
  int x, y;
  cell_of(g, p, &x, &y);
  if (x < 0 || y < 0 || x >= g->w || y >= g->h || f->dist[y * g->w + x] == UNREACHED) return -1;
  return (float)f->dist[y * g->w + x] * 0.1f * 0.5f * (g->cell.x + g->cell.y);
}

uint64_t cw_flowfield_hash(cw_flowfield h) {
  field *f = fget(h);
  grid *g = f ? cw_pool_get(&g_grids, f->grid) : NULL;
  if (!g) return 0;
  uint64_t hh = 1469598103934665603ull;
  for (int i = 0; i < g->w * g->h; i++) {
    uint32_t d = f->dist[i];
    for (int b = 0; b < 4; b++) hh = (hh ^ ((d >> (8 * b)) & 255)) * 1099511628211ull;
    hh = (hh ^ f->dir[i]) * 1099511628211ull;
  }
  return hh;
}

/* ── crowds ───────────────────────────────────────────────────────────── */

#define MAX_NB CW_CROWD2D_MAX_NEIGHBORS

typedef struct agent {
  uint32_t gen; /* odd: live */
  cw_crowd2d_agent_params p;
  cw_vec2 pos, vel, dvel, nvel, target, vreq;
  cw_crowd2d_agent_status status;
  bool partial, need_plan, planned_ok;
  uint64_t plan_seq;
  uint32_t path_version;
  cw_vec2 path[MAX_PATH];
  cw_vec2 new_path[MAX_PATH];
  int npath, ipath, new_n;
  bool new_partial;
  uint64_t flow; /* a flow field's id while it follows one */
  int held;      /* ticks in a row it wanted to move and barely did */
  int lean;      /* ticks left of leaning aside to get round what holds it */
  bool lean_left; /* the side of the next lean: right first, then the other each time it is held again */
  bool lost;     /* pushed out of sight of its next corner this step: a new path is asked for after the phase */
  int nnb;
  int nb[MAX_NB];
} agent;

typedef struct crowd {
  uint64_t grid;
  int max, live;
  agent *agents;
  CW_ARRAY(int) free_slots;
  int plan_per_step, plan_iters;
  uint64_t seq;
  uint32_t grid_version;
  /* step scratch */
  float *px, *py, *vx, *vy, *rad, *pri;
  uint8_t *avoids, *alive;
  int *bin_of, *bin_start, *bin_items, *plan_list;
  int bins_cap, bw, bh;
  float bin, bx0, by0, dt;
} crowd;

static crowd *cget(cw_crowd2d h) { return g_crowds.elem_size ? cw_pool_get(&g_crowds, h.id) : NULL; }
static agent *agent_of(crowd *c, cw_crowd2d_agent a) {
  if (!c) return NULL;
  uint32_t slot = (uint32_t)(a.id & 0xffffffffu), gen = (uint32_t)(a.id >> 32);
  if (!a.id || slot >= (uint32_t)c->max) return NULL;
  agent *ag = &c->agents[slot];
  return ag->gen == gen && (gen & 1) ? ag : NULL;
}

cw_crowd2d_agent_params cw_crowd2d_agent_params_default(void) {
  return (cw_crowd2d_agent_params){ .radius = 6, .max_speed = 60, .max_accel = 480, .neighbor_dist = 48, .max_neighbors = 8,
                                    .time_horizon = 1, .arrive_dist = 3, .slowdown_dist = 12, .avoidance = true,
                                    .avoidance_priority = 0.5f };
}

static cw_crowd2d_agent_params clean_params(const cw_crowd2d_agent_params *p) {
  cw_crowd2d_agent_params d = p ? *p : cw_crowd2d_agent_params_default();
  if (d.radius <= 0) d.radius = 0.01f;
  if (d.time_horizon <= 0) d.time_horizon = 0.01f;
  d.max_neighbors = CW_CLAMP(d.max_neighbors, 0, MAX_NB);
  d.avoidance_priority = CW_CLAMP(d.avoidance_priority, 0.0f, 1.0f);
  return d;
}

cw_crowd2d cw_crowd2d_new(cw_navgrid gh, int max_agents) {
  grid *g = get(gh);
  if (!g || max_agents < 1) return (cw_crowd2d){ 0 };
  if (!g_crowds.elem_size) cw_pool_init(&g_crowds, sizeof(crowd), CW_MEM_NAV);
  uint64_t id;
  crowd *c = cw_pool_alloc(&g_crowds, &id);
  memset(c, 0, sizeof *c);
  c->grid = gh.id;
  c->max = max_agents;
  c->agents = cw_calloc((size_t)max_agents, sizeof(agent), CW_MEM_NAV);
  size_t n = (size_t)max_agents;
  c->px = cw_calloc(n * 6, sizeof(float), CW_MEM_NAV);
  c->py = c->px + n, c->vx = c->py + n, c->vy = c->vx + n, c->rad = c->vy + n, c->pri = c->rad + n;
  c->avoids = cw_calloc(n * 2, 1, CW_MEM_NAV);
  c->alive = c->avoids + n;
  c->bin_of = cw_calloc(n * 3, sizeof(int), CW_MEM_NAV);
  c->bin_items = c->bin_of + n, c->plan_list = c->bin_items + n;
  for (int i = max_agents - 1; i >= 0; i--) cw_array_push(&c->free_slots, i);
  c->plan_per_step = 8;
  c->grid_version = g->version;
  cw_array_push(&g->crowds, id);
  return (cw_crowd2d){ id };
}

static void crowd_destroy(uint64_t id) {
  crowd *c = cw_pool_get(&g_crowds, id);
  if (!c) return;
  grid *g = cw_pool_get(&g_grids, c->grid);
  if (g)
    for (int i = 0; i < g->crowds.count; i++)
      if (g->crowds.data[i] == id) {
        g->crowds.data[i] = g->crowds.data[--g->crowds.count];
        break;
      }
  cw_free(c->agents);
  cw_free(c->px);
  cw_free(c->avoids);
  cw_free(c->bin_of);
  cw_free(c->bin_start);
  cw_array_free(&c->free_slots);
  cw_pool_release(&g_crowds, id);
}

void cw_crowd2d_free(cw_crowd2d h) {
  if (cget(h)) crowd_destroy(h.id);
}
bool cw_crowd2d_valid(cw_crowd2d h) { return cget(h) != NULL; }

void cw_crowd2d_set_plan_budget(cw_crowd2d h, int paths_per_step, int max_iters) {
  crowd *c = cget(h);
  if (!c) return;
  c->plan_per_step = CW_MAX(paths_per_step, 1);
  c->plan_iters = CW_MAX(max_iters, 0);
}

cw_crowd2d_agent cw_crowd2d_add(cw_crowd2d h, cw_vec2 p, const cw_crowd2d_agent_params *params) {
  crowd *c = cget(h);
  grid *g = c ? cw_pool_get(&g_grids, c->grid) : NULL;
  if (!g || !c->free_slots.count) return (cw_crowd2d_agent){ 0 };
  int x, y, cx, cy;
  if (!nearest_cell(g, p, 0, &x, &y)) return (cw_crowd2d_agent){ 0 };
  cell_of(g, p, &cx, &cy);
  if (cx != x || cy != y) p = center_of(g, x, y);
  int slot = cw_array_pop(&c->free_slots);
  agent *a = &c->agents[slot];
  uint32_t gen = a->gen + 1;
  memset(a, 0, sizeof *a);
  a->gen = gen;
  a->p = clean_params(params);
  a->pos = p;
  c->live++;
  return (cw_crowd2d_agent){ ((uint64_t)gen << 32) | (uint32_t)slot };
}

void cw_crowd2d_remove(cw_crowd2d h, cw_crowd2d_agent ah) {
  crowd *c = cget(h);
  agent *a = agent_of(c, ah);
  if (!a) return;
  a->gen++;
  c->live--;
  /* lowest slot first: keep the free list sorted descending */
  int slot = (int)(a - c->agents), i = c->free_slots.count;
  cw_array_push(&c->free_slots, slot);
  while (i > 0 && c->free_slots.data[i - 1] < slot) {
    c->free_slots.data[i] = c->free_slots.data[i - 1];
    i--;
  }
  c->free_slots.data[i] = slot;
}

bool cw_crowd2d_agent_valid(cw_crowd2d h, cw_crowd2d_agent a) { return agent_of(cget(h), a) != NULL; }
int cw_crowd2d_agent_slot(cw_crowd2d h, cw_crowd2d_agent ah) {
  crowd *c = cget(h);
  agent *a = agent_of(c, ah);
  return a ? (int)(a - c->agents) : -1;
}

void cw_crowd2d_set_params(cw_crowd2d h, cw_crowd2d_agent ah, const cw_crowd2d_agent_params *p) {
  agent *a = agent_of(cget(h), ah);
  if (a) a->p = clean_params(p);
}

void cw_crowd2d_set_target(cw_crowd2d h, cw_crowd2d_agent ah, cw_vec2 target) {
  crowd *c = cget(h);
  agent *a = agent_of(c, ah);
  if (!a) return;
  a->target = target;
  a->flow = 0;
  a->status = CW_CROWD2D_WAITING;
  a->npath = a->ipath = 0;
  a->need_plan = true;
  a->plan_seq = ++c->seq;
}

void cw_crowd2d_set_flow(cw_crowd2d h, cw_crowd2d_agent ah, cw_flowfield f) {
  crowd *c = cget(h);
  agent *a = agent_of(c, ah);
  field *ff = fget(f);
  if (!a || !ff || ff->grid != c->grid) return;
  a->flow = f.id;
  a->need_plan = false;
  a->npath = a->ipath = 0;
  a->status = CW_CROWD2D_MOVING;
}

void cw_crowd2d_clear_target(cw_crowd2d h, cw_crowd2d_agent ah) {
  agent *a = agent_of(cget(h), ah);
  if (!a) return;
  a->status = CW_CROWD2D_IDLE;
  a->need_plan = false;
  a->flow = 0;
  a->npath = a->ipath = 0;
}

void cw_crowd2d_set_velocity(cw_crowd2d h, cw_crowd2d_agent ah, cw_vec2 v) {
  agent *a = agent_of(cget(h), ah);
  if (!a) return;
  a->vreq = v;
  a->status = CW_CROWD2D_VELOCITY;
  a->need_plan = false;
  a->flow = 0;
  a->npath = a->ipath = 0;
}

void cw_crowd2d_teleport(cw_crowd2d h, cw_crowd2d_agent ah, cw_vec2 p) {
  crowd *c = cget(h);
  agent *a = agent_of(c, ah);
  if (!a) return;
  a->pos = p;
  a->vel = cw_v2(0, 0);
  if (a->status == CW_CROWD2D_MOVING || a->status == CW_CROWD2D_WAITING || a->status == CW_CROWD2D_ARRIVED || a->status == CW_CROWD2D_NO_PATH) {
    if (!a->flow) {
      a->status = CW_CROWD2D_WAITING;
      a->npath = a->ipath = 0;
      a->need_plan = true;
      a->plan_seq = ++c->seq;
    }
  }
}

void cw_crowd2d_set_position(cw_crowd2d h, cw_crowd2d_agent ah, cw_vec2 p) {
  agent *a = agent_of(cget(h), ah);
  if (a) a->pos = p;
}

bool cw_crowd2d_get_state(cw_crowd2d h, cw_crowd2d_agent ah, cw_crowd2d_agent_state *out) {
  agent *a = agent_of(cget(h), ah);
  if (!a) return false;
  out->position = a->pos, out->velocity = a->vel, out->desired_velocity = a->dvel, out->target = a->target;
  out->next_corner = a->ipath < a->npath ? a->path[a->ipath] : a->pos;
  out->status = a->status;
  out->partial = a->partial;
  out->path_points = a->npath - a->ipath;
  out->path_version = a->path_version;
  return true;
}

cw_vec2 cw_crowd2d_position(cw_crowd2d h, cw_crowd2d_agent ah) {
  agent *a = agent_of(cget(h), ah);
  return a ? a->pos : cw_v2(0, 0);
}

int cw_crowd2d_count(cw_crowd2d h) {
  crowd *c = cget(h);
  return c ? c->live : 0;
}

uint64_t cw_crowd2d_hash(cw_crowd2d h) {
  crowd *c = cget(h);
  if (!c) return 0;
  uint64_t hh = 1469598103934665603ull;
  for (int i = 0; i < c->max; i++) {
    const agent *a = &c->agents[i];
    if (!(a->gen & 1)) continue;
    float v[4] = { a->pos.x, a->pos.y, a->vel.x, a->vel.y };
    const uint8_t *b = (const uint8_t *)v;
    for (size_t k = 0; k < sizeof v; k++) hh = (hh ^ b[k]) * 1099511628211ull;
    hh = (hh ^ (uint64_t)a->status) * 1099511628211ull;
  }
  return hh;
}

/* phase 1: paths */
static void plan_range(int b, int e, int w, void *ctx) {
  (void)w;
  crowd *c = ctx;
  grid *g = cw_pool_get(&g_grids, c->grid);
  for (int k = b; k < e; k++) {
    agent *a = &c->agents[c->plan_list[k]];
    cw_navgrid_query q = { .max_iters = c->plan_iters, .agent_radius = a->p.radius };
    a->new_n = path_in(g, search_for(g), a->pos, a->target, &q, a->new_path, MAX_PATH, &a->new_partial);
  }
}

/* phase 2: neighbours. Agents are counting-sorted into square bins as wide as the widest neighbour distance. */
static void build_bins(crowd *c) {
  float lo_x = FLT_MAX, lo_y = FLT_MAX, hi_x = -FLT_MAX, hi_y = -FLT_MAX, bin = 1;
  for (int i = 0; i < c->max; i++) {
    const agent *a = &c->agents[i];
    c->alive[i] = (a->gen & 1) != 0;
    if (!c->alive[i]) continue;
    c->px[i] = a->pos.x, c->py[i] = a->pos.y, c->vx[i] = a->vel.x, c->vy[i] = a->vel.y;
    /* one that stands (arrived, idle, waiting) gives no way: whoever passes it does all the avoiding */
    bool walks = a->status == CW_CROWD2D_MOVING || a->status == CW_CROWD2D_VELOCITY;
    c->rad[i] = a->p.radius, c->pri[i] = a->p.avoidance_priority, c->avoids[i] = a->p.avoidance && walks;
    lo_x = CW_MIN(lo_x, a->pos.x), lo_y = CW_MIN(lo_y, a->pos.y), hi_x = CW_MAX(hi_x, a->pos.x), hi_y = CW_MAX(hi_y, a->pos.y);
    bin = CW_MAX(bin, a->p.neighbor_dist);
  }
  if (lo_x > hi_x) {
    c->bw = c->bh = 0;
    return;
  }
  c->bin = bin, c->bx0 = lo_x, c->by0 = lo_y;
  c->bw = (int)((hi_x - lo_x) / bin) + 1, c->bh = (int)((hi_y - lo_y) / bin) + 1;
  /* a crowd spread thin over a huge area: wider bins, never more bins than a few per agent */
  while ((int64_t)c->bw * c->bh > (int64_t)c->max * 4 + 64) {
    c->bin *= 2;
    c->bw = (int)((hi_x - lo_x) / c->bin) + 1, c->bh = (int)((hi_y - lo_y) / c->bin) + 1;
  }
  int nb = c->bw * c->bh;
  if (nb + 1 > c->bins_cap) {
    cw_free(c->bin_start);
    c->bins_cap = nb + 1;
    c->bin_start = cw_alloc(sizeof(int) * (size_t)c->bins_cap, CW_MEM_NAV);
  }
  memset(c->bin_start, 0, sizeof(int) * (size_t)(nb + 1));
  for (int i = 0; i < c->max; i++) {
    if (!c->alive[i]) continue;
    int bx = (int)((c->px[i] - c->bx0) / c->bin), by = (int)((c->py[i] - c->by0) / c->bin);
    c->bin_of[i] = by * c->bw + bx;
    c->bin_start[c->bin_of[i] + 1]++;
  }
  for (int b = 0; b < nb; b++) c->bin_start[b + 1] += c->bin_start[b];
  /* fill in index order: each bin's agents ascend */
  int *fill = cw_frame_alloc(sizeof(int) * (size_t)(nb + 1));
  memcpy(fill, c->bin_start, sizeof(int) * (size_t)(nb + 1));
  for (int i = 0; i < c->max; i++)
    if (c->alive[i]) c->bin_items[fill[c->bin_of[i]]++] = i;
}

static void neighbours_range(int b, int e, int w, void *ctx) {
  (void)w;
  crowd *c = ctx;
  for (int i = b; i < e; i++) {
    if (!c->alive[i]) continue;
    agent *a = &c->agents[i];
    a->nnb = 0;
    if (!a->p.avoidance || a->p.max_neighbors <= 0) continue;
    float nd[MAX_NB], r2 = a->p.neighbor_dist * a->p.neighbor_dist;
    int bx = c->bin_of[i] % c->bw, by = c->bin_of[i] / c->bw;
    for (int yy = CW_MAX(by - 1, 0); yy <= CW_MIN(by + 1, c->bh - 1); yy++)
      for (int xx = CW_MAX(bx - 1, 0); xx <= CW_MIN(bx + 1, c->bw - 1); xx++) {
        int bi = yy * c->bw + xx;
        for (int k = c->bin_start[bi]; k < c->bin_start[bi + 1]; k++) {
          int j = c->bin_items[k];
          if (j == i) continue;
          float dx = c->px[j] - c->px[i], dy = c->py[j] - c->py[i], d2 = dx * dx + dy * dy;
          if (d2 > r2) continue;
          /* insert by (distance, index) */
          int n = a->nnb, at = n;
          while (at > 0 && (nd[at - 1] > d2 || (nd[at - 1] == d2 && a->nb[at - 1] > j))) at--;
          if (at >= a->p.max_neighbors) continue;
          int last = n < a->p.max_neighbors ? n : a->p.max_neighbors - 1;
          for (int m = last; m > at; m--) nd[m] = nd[m - 1], a->nb[m] = a->nb[m - 1];
          nd[at] = d2, a->nb[at] = j;
          if (n < a->p.max_neighbors) a->nnb++;
        }
      }
  }
}

/* phase 3: steering and avoidance */
static void steer(crowd *c, const grid *g, int i) {
  agent *a = &c->agents[i];
  a->dvel = cw_v2(0, 0);
  if (a->status == CW_CROWD2D_VELOCITY) {
    a->dvel = a->vreq;
  } else if (a->status == CW_CROWD2D_MOVING && a->flow) {
    field *f = cw_pool_get(&g_fields, a->flow);
    int x, y;
    cell_of(g, a->pos, &x, &y);
    if (!f) {
      a->status = CW_CROWD2D_IDLE;
      a->flow = 0;
    } else if (x >= 0 && y >= 0 && x < g->w && y < g->h && f->dist[y * g->w + x] == 0) {
      a->status = CW_CROWD2D_ARRIVED;
    } else if (x >= 0 && y >= 0 && x < g->w && y < g->h) {
      int k = f->dir[y * g->w + x];
      /* toward the middle of the next cell, so agents round corners instead of scraping them */
      if (k) {
        int nx, ny;
        uint32_t step;
        neighbour(g, x, y, k, &nx, &ny, &step);
        cw_vec2 next = center_of(g, nx, ny);
        float dx = next.x - a->pos.x, dy = next.y - a->pos.y, d = sqrtf(dx * dx + dy * dy);
        if (d > 0) a->dvel = cw_v2(dx / d * a->p.max_speed, dy / d * a->p.max_speed);
      }
    }
  } else if (a->status == CW_CROWD2D_MOVING && a->ipath < a->npath) {
    /* corners reached are dropped; the last one is the goal */
    float reach = CW_MAX(a->p.max_speed * c->dt, a->p.radius * 0.5f);
    while (a->ipath < a->npath - 1) {
      float dx = a->path[a->ipath].x - a->pos.x, dy = a->path[a->ipath].y - a->pos.y;
      if (dx * dx + dy * dy > reach * reach) break;
      a->ipath++;
    }
    /* avoidance may have pushed it round a corner of the walls: with the next corner out of sight, walking at
     * it would only press it into the wall */
    if (!a->need_plan && !is_hex(g) && !sight(g, a->pos, a->path[a->ipath], false, NULL)) a->lost = true;
    float dx = a->path[a->ipath].x - a->pos.x, dy = a->path[a->ipath].y - a->pos.y, d = sqrtf(dx * dx + dy * dy);
    bool last = a->ipath == a->npath - 1;
    if (last && d <= a->p.arrive_dist) {
      a->status = CW_CROWD2D_ARRIVED;
      a->ipath = a->npath;
    } else if (d > 0) {
      float speed = a->p.max_speed;
      if (last && a->p.slowdown_dist > 0 && d < a->p.slowdown_dist) speed = speed * (d / a->p.slowdown_dist);
      /* never past the corner in one step */
      if (speed * c->dt > d) speed = d / c->dt;
      a->dvel = cw_v2(dx / d * speed, dy / d * speed);
    }
  }
  /* Two streams that meet head on in a doorway hold each other still: reciprocal avoidance has no reason to
   * pick a side, and one that stands in the way is not walked round by a path that runs through it. An agent
   * held for a third of a second leans to its right for the next three quarters of one, as people do; held
   * again (a wall was on that side), to its left, and so on in turn. */
  float want2 = a->dvel.x * a->dvel.x + a->dvel.y * a->dvel.y, have2 = c->vx[i] * c->vx[i] + c->vy[i] * c->vy[i];
  if (want2 > 0 && have2 < want2 * 0.0625f && a->nnb > 0) a->held++;
  else if (have2 >= want2 * 0.25f) a->held = 0;
  if (a->held > 20) {
    a->held = 0;
    a->lean_left = a->lean > 0 ? !a->lean_left : a->lean_left;
    a->lean = 45;
  }
  float nv[2] = { a->dvel.x, a->dvel.y };
  if (a->lean > 0) {
    /* turned 60 degrees (clockwise on screen, y down, for a lean to the right): cos 0.5, sin 0.8660254 */
    float sn = a->lean_left ? -0.8660254f : 0.8660254f;
    nv[0] = a->dvel.x * 0.5f - a->dvel.y * sn;
    nv[1] = a->dvel.x * sn + a->dvel.y * 0.5f;
    if (--a->lean == 0 && have2 < want2 * 0.0625f) a->lean_left = !a->lean_left; /* it did not help: the other side next */
  }
  /* the acceleration limit shapes what the agent wants, before avoidance: the velocity avoidance answers with
   * is taken whole, or a limited agent could not get out of another's way in time */
  {
    float ax = nv[0] - c->vx[i], ay = nv[1] - c->vy[i];
    float al = sqrtf(ax * ax + ay * ay), amax = a->p.max_accel * c->dt;
    if (a->p.max_accel > 0 && al > amax) nv[0] = c->vx[i] + ax / al * amax, nv[1] = c->vy[i] + ay / al * amax;
  }
  if (a->p.avoidance && a->nnb > 0) {
    cw_orca_input in;
    float vxi = c->vx[i], vyi = c->vy[i], pri = c->pri[i];
    for (int k = 0; k < a->nnb; k++) {
      int j = a->nb[k];
      in.rpx[k] = c->px[j] - c->px[i];
      in.rpy[k] = c->py[j] - c->py[i];
      in.rvx[k] = vxi - c->vx[j];
      in.rvy[k] = vyi - c->vy[j];
      in.cr[k] = c->rad[i] + c->rad[j];
      float sum = pri + c->pri[j];
      in.weight[k] = !c->avoids[j] ? 1.0f : sum > 0 ? c->pri[j] / sum : 0.5f;
      in.side[k] = i < j ? -1.0f : 1.0f;
    }
    for (int k = a->nnb; k < ((a->nnb + 3) & ~3); k++) {
      in.rpx[k] = in.rpy[k] = in.rvx[k] = in.rvy[k] = 0;
      in.cr[k] = 1, in.weight[k] = 0, in.side[k] = 1;
    }
    cw_orca_line lines[MAX_NB];
    cw_orca_lines(&in, a->nnb, vxi, vyi, 1.0f / a->p.time_horizon, 1.0f / c->dt, lines);
    float out[2];
    cw_orca_solve(lines, a->nnb, a->p.max_speed, nv, out);
    nv[0] = out[0], nv[1] = out[1];
  }
  a->nvel = cw_v2(nv[0], nv[1]);
}

static void steer_range(int b, int e, int w, void *ctx) {
  (void)w;
  crowd *c = ctx;
  const grid *g = cw_pool_get(&g_grids, c->grid);
  for (int i = b; i < e; i++)
    if (c->alive[i]) steer(c, g, i);
}

/* phase 4: movement. A circle against the blocked cells round it: pushed out of each along the shortest way,
 * twice over so a corner between two cells settles. */
static cw_vec2 slide_square(const grid *g, cw_vec2 p, float radius);
static cw_vec2 slide(const grid *g, cw_vec2 p, float radius) {
  if (g->layout == CW_TILE_LAYOUT_SQUARE) return slide_square(g, p, radius);
  if (g->layout == CW_TILE_LAYOUT_ISOMETRIC) {
    /* diamonds are squares in cell space: slide there, with the radius in cells (measured along a diamond's
     * side), and come back */
    float fx, fy, side = 0.5f * sqrtf(g->cell.x * g->cell.x + g->cell.y * g->cell.y);
    cell_space(g, p, &fx, &fy);
    grid unit = *g;
    unit.layout = CW_TILE_LAYOUT_SQUARE, unit.cell = cw_v2(1, 1), unit.origin = cw_v2(0, 0);
    cw_vec2 q = slide_square(&unit, cw_v2(fx, fy), radius / side);
    return q.x == fx && q.y == fy ? p : from_cell_space(g, q.x, q.y);
  }
  /* hexagons: a blocked cell pushes out as the circle inside it */
  float in = 0.5f * CW_MIN(g->cell.x, g->cell.y) * (g->cell.x < g->cell.y ? 1.0f : 1.0f);
  float r = CW_MIN(radius, 0.45f * CW_MIN(g->cell.x, g->cell.y));
  for (int pass = 0; pass < 2; pass++) {
    int cx, cy;
    cell_of(g, p, &cx, &cy);
    for (int k = 0; k <= 6; k++) {
      int nx = cx, ny = cy;
      if (k) {
        uint32_t step;
        neighbour(g, cx, cy, k, &nx, &ny, &step);
      }
      if (W(g, nx, ny)) continue;
      cw_vec2 c = center_of(g, nx, ny);
      float dx = p.x - c.x, dy = p.y - c.y, d2 = dx * dx + dy * dy, keep = in + r;
      if (d2 >= keep * keep || d2 <= 0) continue;
      float d = sqrtf(d2);
      p = cw_v2(c.x + dx / d * keep, c.y + dy / d * keep);
    }
  }
  return p;
}
static cw_vec2 slide_square(const grid *g, cw_vec2 p, float radius) {
  float r = CW_MIN(radius, 0.49f * CW_MIN(g->cell.x, g->cell.y)); /* wider agents keep off walls through the bake's agent_radius */
  for (int pass = 0; pass < 2; pass++) {
    int x0, y0, x1, y1;
    cell_of(g, cw_v2(p.x - r, p.y - r), &x0, &y0);
    cell_of(g, cw_v2(p.x + r, p.y + r), &x1, &y1);
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        if (W(g, x, y)) continue;
        float cx0 = g->origin.x + (float)x * g->cell.x, cy0 = g->origin.y + (float)y * g->cell.y;
        float qx = CW_CLAMP(p.x, cx0, cx0 + g->cell.x), qy = CW_CLAMP(p.y, cy0, cy0 + g->cell.y);
        float dx = p.x - qx, dy = p.y - qy, d2 = dx * dx + dy * dy;
        if (d2 >= r * r) continue;
        if (d2 > 0) {
          float d = sqrtf(d2);
          p.x = qx + dx / d * r, p.y = qy + dy / d * r;
        } else {
          /* inside the cell: out through the nearest side that leads to a walkable cell */
          float l = p.x - cx0, rr = cx0 + g->cell.x - p.x, u = p.y - cy0, dn = cy0 + g->cell.y - p.y;
          float best = FLT_MAX;
          cw_vec2 q = p;
          if (W(g, x - 1, y) && l < best) best = l, q = cw_v2(cx0 - r, p.y);
          if (W(g, x + 1, y) && rr < best) best = rr, q = cw_v2(cx0 + g->cell.x + r, p.y);
          if (W(g, x, y - 1) && u < best) best = u, q = cw_v2(p.x, cy0 - r);
          if (W(g, x, y + 1) && dn < best) best = dn, q = cw_v2(p.x, cy0 + g->cell.y + r);
          p = q;
        }
      }
  }
  return p;
}

static void move_range(int b, int e, int w, void *ctx) {
  (void)w;
  crowd *c = ctx;
  const grid *g = cw_pool_get(&g_grids, c->grid);
  for (int i = b; i < e; i++) {
    if (!c->alive[i]) continue;
    agent *a = &c->agents[i];
    if (a->status != CW_CROWD2D_MOVING && a->status != CW_CROWD2D_VELOCITY) {
      a->vel = cw_v2(0, 0);
      continue;
    }
    cw_vec2 p = cw_v2(a->pos.x + a->nvel.x * c->dt, a->pos.y + a->nvel.y * c->dt);
    cw_vec2 q = slide(g, p, a->p.radius);
    a->vel = q.x == p.x && q.y == p.y ? a->nvel : cw_v2((q.x - a->pos.x) / c->dt, (q.y - a->pos.y) / c->dt);
    a->pos = q;
  }
}

void cw_crowd2d_step(cw_crowd2d h, float dt) {
  crowd *c = cget(h);
  grid *g = c ? cw_pool_get(&g_grids, c->grid) : NULL;
  if (!g || dt <= 0) return;
  c->dt = dt;
  /* the grid changed: every path is planned again, in agent order */
  if (g->version != c->grid_version) {
    c->grid_version = g->version;
    for (int i = 0; i < c->max; i++) {
      agent *a = &c->agents[i];
      if (!(a->gen & 1) || a->flow) continue;
      /* those on their way, those with no way, and those who stopped short of a target they could not reach */
      bool short_of = a->status == CW_CROWD2D_ARRIVED && a->partial;
      if ((a->status == CW_CROWD2D_MOVING || a->status == CW_CROWD2D_NO_PATH || a->status == CW_CROWD2D_WAITING || short_of) && !a->need_plan) {
        a->need_plan = true;
        a->plan_seq = ++c->seq;
      }
    }
  }
  /* 1. the oldest requests, up to the budget */
  int np = 0;
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if (!(a->gen & 1) || !a->need_plan) continue;
    /* keep plan_list the plan_per_step lowest sequence numbers, ascending */
    int at = np;
    while (at > 0 && c->agents[c->plan_list[at - 1]].plan_seq > a->plan_seq) at--;
    if (at >= c->plan_per_step) continue;
    int last = np < c->plan_per_step ? np : c->plan_per_step - 1;
    for (int m = last; m > at; m--) c->plan_list[m] = c->plan_list[m - 1];
    c->plan_list[at] = i;
    if (np < c->plan_per_step) np++;
  }
  if (np) {
    cw_parallel_for(np, 1, plan_range, c);
    for (int k = 0; k < np; k++) {
      agent *a = &c->agents[c->plan_list[k]];
      a->need_plan = false;
      a->path_version++;
      if (a->new_n >= 1) {
        memcpy(a->path, a->new_path, sizeof(cw_vec2) * (size_t)a->new_n);
        a->npath = a->new_n;
        a->ipath = a->new_n > 1 ? 1 : 0; /* the first waypoint is where it stands */
        a->partial = a->new_partial;
        a->status = CW_CROWD2D_MOVING;
      } else {
        a->npath = a->ipath = 0;
        a->partial = false;
        a->status = CW_CROWD2D_NO_PATH;
      }
    }
  }
  /* 2..4 */
  build_bins(c);
  if (!c->bw) return;
  cw_parallel_for(c->max, 64, neighbours_range, c);
  cw_parallel_for(c->max, 64, steer_range, c);
  for (int i = 0; i < c->max; i++) {
    agent *a = &c->agents[i];
    if ((a->gen & 1) && a->lost) {
      a->lost = false;
      a->need_plan = true;
      a->plan_seq = ++c->seq;
    }
  }
  cw_parallel_for(c->max, 64, move_range, c);
}

/* ── debug draw ───────────────────────────────────────────────────────── */

static void line(cw_phys_lines *out, cw_vec2 a, cw_vec2 b, uint32_t color) {
  float pa[3] = { a.x, a.y, 0 }, pb[3] = { b.x, b.y, 0 };
  cw_phys_lines_add(out, pa, pb, color);
}

void cw_navgrid_debug_draw(cw_navgrid h, cw_phys_lines *out, uint32_t flags) {
  grid *g = get(h);
  if (!g || !(flags & CW_NAV2D_DRAW_BLOCKED)) return;
  for (int y = 0; y < g->h; y++)
    for (int x = 0; x < g->w; x++) {
      if (g->walk[y * g->w + x]) continue;
      cw_vec2 c = center_of(g, x, y);
      float hx = g->cell.x * 0.3f, hy = g->cell.y * 0.3f;
      uint32_t col = g->cover[y * g->w + x] ? 0xff9040 : 0xe04040;
      line(out, cw_v2(c.x - hx, c.y - hy), cw_v2(c.x + hx, c.y + hy), col);
      line(out, cw_v2(c.x - hx, c.y + hy), cw_v2(c.x + hx, c.y - hy), col);
    }
}

void cw_flowfield_debug_draw(cw_flowfield h, cw_phys_lines *out) {
  field *f = fget(h);
  grid *g = f ? cw_pool_get(&g_grids, f->grid) : NULL;
  if (!g) return;
  for (int y = 0; y < g->h; y++)
    for (int x = 0; x < g->w; x++) {
      int k = f->dir[y * g->w + x];
      if (!k) continue;
      cw_vec2 c = center_of(g, x, y);
      cw_vec2 dir = cw_flowfield_direction(h, c);
      line(out, c, cw_v2(c.x + dir.x * g->cell.x * 0.4f, c.y + dir.y * g->cell.y * 0.4f), 0x40c0ff);
    }
}

void cw_crowd2d_debug_draw(cw_crowd2d h, cw_phys_lines *out, uint32_t flags) {
  crowd *c = cget(h);
  if (!c) return;
  for (int i = 0; i < c->max; i++) {
    const agent *a = &c->agents[i];
    if (!(a->gen & 1)) continue;
    if (flags & CW_NAV2D_DRAW_AGENTS) {
      for (int k = 0; k < 8; k++) {
        float r = a->p.radius;
        line(out, cw_v2(a->pos.x + UNIT[(k % 8) + 1][0] * r, a->pos.y + UNIT[(k % 8) + 1][1] * r),
             cw_v2(a->pos.x + UNIT[((k + 4) % 8) + 1][0] * r, a->pos.y + UNIT[((k + 4) % 8) + 1][1] * r), 0xffe060);
      }
      line(out, a->pos, cw_v2(a->pos.x + a->vel.x * 0.25f, a->pos.y + a->vel.y * 0.25f), 0xffffff);
    }
    if ((flags & CW_NAV2D_DRAW_PATHS) && a->ipath < a->npath) {
      line(out, a->pos, a->path[a->ipath], 0x60ff80);
      for (int k = a->ipath; k + 1 < a->npath; k++) line(out, a->path[k], a->path[k + 1], 0x60ff80);
    }
  }
}
