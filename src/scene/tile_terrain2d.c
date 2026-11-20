/* TileMap2D terrains: autotiling (cartwheel/scene2d.h "terrains" has the text form and the tile orders).
 *
 * A cell belongs to a terrain when its tile id is one of the terrain's tiles, so nothing is stored beside the
 * cells: a scene's cells text is the whole state, and fixing a region again gives the same tiles. Variants
 * come from a cw_rng seeded by the terrain's seed and the cell, so a cell gets the same variant on every host
 * and whatever order the map was painted in. */
#include <stdlib.h>

#include "scene2d_internal.h"

enum { MODE_SINGLE, MODE_EDGE16, MODE_CORNER16, MODE_BLOB47 };
static const int mode_count[] = { 1, 16, 16, 47 };

#define MAX_VARIANTS 8

typedef struct terrain_def {
  const char *name; /* interned */
  int mode;
  int variants;
  bool outside;     /* cells outside the map count as this terrain */
  uint32_t seed;
  int32_t ids[MAX_VARIANTS][47]; /* tile id per variant and template index */
} terrain_def;

struct cw__terrains {
  CW_ARRAY(terrain_def) list;
  /* tile id -> terrain index + 1, template index, for ids up to max_id */
  int max_id;
  uint8_t *of_tile;
  uint8_t *index_of_tile;
};

/* ── the blob set: the 47 neighbour masks that differ once a corner only counts between two set sides ──── */

static uint8_t blob_masks[47];
static int8_t blob_index[256];
static bool blob_ready;

static int blob_reduce(int m) {
  /* bits clockwise from north: N 1, NE 2, E 4, SE 8, S 16, SW 32, W 64, NW 128 */
  if (!((m & 1) && (m & 4))) m &= ~2;
  if (!((m & 4) && (m & 16))) m &= ~8;
  if (!((m & 16) && (m & 64))) m &= ~32;
  if (!((m & 64) && (m & 1))) m &= ~128;
  return m;
}

static void blob_init(void) {
  if (blob_ready) return;
  int n = 0;
  for (int m = 0; m < 256; m++) blob_index[m] = -1;
  for (int m = 0; m < 256; m++)
    if (blob_reduce(m) == m) blob_masks[n] = (uint8_t)m, blob_index[m] = (int8_t)n, n++;
  CW_ASSERT(n == 47, "the blob set has 47 tiles");
  blob_ready = true;
}

int cw_tile_terrain_tiles(cw_tile_terrain_mode mode) { return mode >= 0 && mode <= CW_TILE_TERRAIN_BLOB47 ? mode_count[mode] : 0; }

int cw_tile_terrain_mask(cw_tile_terrain_mode mode, int index) {
  if (index < 0 || index >= cw_tile_terrain_tiles(mode)) return -1;
  if (mode == CW_TILE_TERRAIN_BLOB47) {
    blob_init();
    return blob_masks[index];
  }
  return mode == CW_TILE_TERRAIN_SINGLE ? 0 : index;
}

int cw_tile_terrain_index(cw_tile_terrain_mode mode, int mask) {
  switch (mode) {
  case CW_TILE_TERRAIN_SINGLE: return 0;
  case CW_TILE_TERRAIN_EDGE16: case CW_TILE_TERRAIN_CORNER16: return mask & 15;
  case CW_TILE_TERRAIN_BLOB47:
    blob_init();
    return blob_index[blob_reduce(mask & 255)];
  default: return -1;
  }
}

/* ── the text form ────────────────────────────────────────────────────── */

void cw__terrains_free(cw__terrains *t) {
  if (!t) return;
  cw_array_free(&t->list);
  cw_free(t->of_tile);
  cw_free(t->index_of_tile);
  cw_free(t);
}

static const char *ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
  return p;
}

static bool word(const char **pp, const char *w) {
  size_t n = strlen(w);
  const char *p = *pp;
  if (strncmp(p, w, n) != 0) return false;
  char c = p[n];
  if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') return false;
  *pp = p + n;
  return true;
}

/* "grass: blob47 16 x3; wall: edge16 64 outside; path: corner16 80-95; floor: single 1 x4 seed 7" */
cw__terrains *cw__terrains_parse(const char *text) {
  blob_init();
  cw__terrains *T = cw_calloc(1, sizeof *T, CW_MEM_SCENE);
  const char *p = text ? text : "";
  while (*(p = ws(p))) {
    if (*p == ';') { p++; continue; }
    const char *start = p, *ns = p;
    while (*p && *p != ':' && *p != ';' && *p != ' ') p++;
    terrain_def tr = { .name = cw_intern_n(ns, (size_t)(p - ns)), .variants = 1 };
    p = ws(p);
    bool ok = *p == ':';
    int ids[47 * MAX_VARIANTS], nid = 0, xn = 0;
    if (ok) {
      p = ws(p + 1);
      if (word(&p, "blob47")) tr.mode = MODE_BLOB47;
      else if (word(&p, "edge16")) tr.mode = MODE_EDGE16;
      else if (word(&p, "corner16")) tr.mode = MODE_CORNER16;
      else if (word(&p, "single")) tr.mode = MODE_SINGLE;
      else ok = false;
    }
    while (ok && *(p = ws(p)) && *p != ';') {
      char *e;
      if (word(&p, "outside")) tr.outside = true;
      else if (word(&p, "seed")) { tr.seed = (uint32_t)strtoul(p, &e, 10); ok = e != p; p = e; }
      else if (*p == 'x' && p[1] >= '0' && p[1] <= '9') { xn = (int)strtol(p + 1, &e, 10); p = e; }
      else {
        long a = strtol(p, &e, 10), b = a;
        if (e == p) { ok = false; break; }
        p = e;
        if (*p == '-') { b = strtol(p + 1, &e, 10); p = e; }
        for (long v = a; v <= b && nid < (int)CW_ARRAY_LEN(ids); v++) ids[nid++] = (int)v;
      }
    }
    int count = mode_count[tr.mode];
    if (ok) {
      /* one id: the first of a run in template order; else whole sets of ids, a set per variant */
      if (nid == 1) {
        tr.variants = CW_CLAMP(xn ? xn : 1, 1, MAX_VARIANTS);
        for (int v = 0; v < tr.variants; v++)
          for (int i = 0; i < count; i++) tr.ids[v][i] = ids[0] + v * count + i;
      } else if (nid >= count && nid % count == 0 && nid / count <= MAX_VARIANTS && !xn) {
        tr.variants = nid / count;
        for (int v = 0; v < tr.variants; v++)
          for (int i = 0; i < count; i++) tr.ids[v][i] = ids[v * count + i];
      } else {
        ok = false;
      }
    }
    if (!ok || T->list.count >= 254) {
      int len = 0;
      while (start[len] && start[len] != ';') len++;
      cw_warn("TileMap2D terrains: cannot read '%.*s' (write \"grass: blob47 16 x3; wall: edge16 64 outside; path: corner16 "
              "80; floor: single 1 x4\": a name, blob47 | edge16 | corner16 | single, then the first tile id of the set "
              "or every id of it, xN for N variant sets in a row, outside, seed N)", len, start);
    } else {
      cw_array_push(&T->list, tr);
    }
    while (*p && *p != ';') p++;
  }
  for (int t = 0; t < T->list.count; t++)
    for (int v = 0; v < T->list.data[t].variants; v++)
      for (int i = 0; i < mode_count[T->list.data[t].mode]; i++) T->max_id = CW_MAX(T->max_id, T->list.data[t].ids[v][i]);
  if (T->list.count) {
    T->of_tile = cw_calloc((size_t)T->max_id + 1, 1, CW_MEM_SCENE);
    T->index_of_tile = cw_calloc((size_t)T->max_id + 1, 1, CW_MEM_SCENE);
    /* the first terrain to name a tile owns it */
    for (int t = T->list.count - 1; t >= 0; t--)
      for (int v = 0; v < T->list.data[t].variants; v++)
        for (int i = 0; i < mode_count[T->list.data[t].mode]; i++) {
          int id = T->list.data[t].ids[v][i];
          if (id < 0) continue;
          T->of_tile[id] = (uint8_t)(t + 1);
          T->index_of_tile[id] = (uint8_t)i;
        }
  }
  return T;
}

/* ── picking tiles ────────────────────────────────────────────────────── */

typedef struct ctx {
  cw_node n;
  cw__terrains *T;
  int layer, w, h;
} ctx;

/* the terrain (1..) of the tile in a cell and its template index; 0 for none. Outside the map: `outside` of
 * the terrain asked about decides (see same()). */
static int cell_terrain(const ctx *c, int x, int y, int *index) {
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return 0;
  int id = cw_tilemap2d_get_cell(c->n, c->layer, x, y);
  if (id < 0 || id > c->T->max_id || !c->T->of_tile) return 0;
  if (index) *index = c->T->index_of_tile[id];
  return c->T->of_tile[id];
}

static bool same(const ctx *c, int t, int x, int y) {
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return c->T->list.data[t - 1].outside;
  return cell_terrain(c, x, y, NULL) == t;
}

/* the corner bits (NE 1, SE 2, SW 4, NW 8) the tile in a cell gives terrain t's corner set */
static int corner_bits(const ctx *c, int t, int x, int y) {
  int idx = 0;
  return cell_terrain(c, x, y, &idx) == t ? idx : 0;
}

/* is the map vertex (vx, vy) (the top-left corner of cell (vx, vy)) part of corner terrain t: any of the four
 * cells round it says so */
static bool vertex_in(const ctx *c, int t, int vx, int vy) {
  return (corner_bits(c, t, vx, vy) & 8) || (corner_bits(c, t, vx - 1, vy) & 1) || (corner_bits(c, t, vx - 1, vy - 1) & 2) ||
         (corner_bits(c, t, vx, vy - 1) & 4);
}

static int pick_variant(const terrain_def *tr, int layer, int x, int y) {
  if (tr->variants <= 1) return 0;
  cw_rng r;
  cw_rng_seed(&r, ((uint64_t)tr->seed << 48) ^ ((uint64_t)(uint32_t)layer << 40) ^ ((uint64_t)(uint32_t)y << 20) ^ (uint64_t)(uint32_t)x);
  return (int)(cw_rng_u32(&r) % (uint32_t)tr->variants);
}

static void put(const ctx *c, int t, int x, int y, int index) {
  const terrain_def *tr = &c->T->list.data[t - 1];
  int id = tr->ids[pick_variant(tr, c->layer, x, y)][index];
  if (cw_tilemap2d_get_cell(c->n, c->layer, x, y) != id) cw_tilemap2d_set_cell(c->n, c->layer, x, y, id, 0);
}

/* the tile a cell of a cell-based terrain should show, from its neighbours */
static void fix_cell(const ctx *c, int x, int y) {
  int t = cell_terrain(c, x, y, NULL);
  if (!t) return;
  const terrain_def *tr = &c->T->list.data[t - 1];
  int mask = 0;
  switch (tr->mode) {
  case MODE_EDGE16:
    mask = (same(c, t, x, y - 1) ? 1 : 0) | (same(c, t, x + 1, y) ? 2 : 0) | (same(c, t, x, y + 1) ? 4 : 0) | (same(c, t, x - 1, y) ? 8 : 0);
    break;
  case MODE_BLOB47:
    mask = (same(c, t, x, y - 1) ? 1 : 0) | (same(c, t, x + 1, y - 1) ? 2 : 0) | (same(c, t, x + 1, y) ? 4 : 0) |
           (same(c, t, x + 1, y + 1) ? 8 : 0) | (same(c, t, x, y + 1) ? 16 : 0) | (same(c, t, x - 1, y + 1) ? 32 : 0) |
           (same(c, t, x - 1, y) ? 64 : 0) | (same(c, t, x - 1, y - 1) ? 128 : 0);
    break;
  case MODE_CORNER16: return; /* corner sets are fixed by vertex (fix_corners) */
  default: break;
  }
  put(c, t, x, y, cw_tile_terrain_index((cw_tile_terrain_mode)tr->mode, mask));
}

/* cells [x0, x1] x [y0, y1] of corner terrain t from the vertices' state `in` (a (x1 - x0 + 2) x (y1 - y0 + 2)
 * grid of 0 / 1, vertex (x0, y0) first) */
static void write_corners(const ctx *c, int t, int x0, int y0, int x1, int y1, const uint8_t *in) {
  int vw = x1 - x0 + 2;
  for (int y = CW_MAX(y0, 0); y <= y1; y++)
    for (int x = CW_MAX(x0, 0); x <= x1; x++) {
      const uint8_t *v = in + (size_t)(y - y0) * (size_t)vw + (size_t)(x - x0);
      int mask = (v[1] ? 1 : 0) | (v[vw + 1] ? 2 : 0) | (v[vw] ? 4 : 0) | (v[0] ? 8 : 0);
      int cur_t = cell_terrain(c, x, y, NULL);
      if (mask) {
        if (x < c->w && y < c->h) put(c, t, x, y, mask);
      } else if (cur_t == t) {
        cw_tilemap2d_set_cell(c->n, c->layer, x, y, -1, 0);
      }
    }
}

static bool make_ctx(ctx *c, cw_node n, int layer) {
  c->n = n;
  c->T = cw__tilemap2d_terrains(n);
  c->layer = layer;
  cw_tilemap2d_size(n, &c->w, &c->h);
  return c->T && c->T->list.count > 0 && layer >= 0 && layer < 8;
}

int cw_tilemap2d_terrain(cw_node n, const char *name) {
  cw__terrains *T = cw__tilemap2d_terrains(n);
  const char *in = name ? cw_intern_find(name) : NULL;
  if (!T || !in) return 0;
  for (int i = 0; i < T->list.count; i++)
    if (T->list.data[i].name == in) return i + 1;
  return 0;
}

int cw_tilemap2d_terrain_count(cw_node n) {
  cw__terrains *T = cw__tilemap2d_terrains(n);
  return T ? T->list.count : 0;
}

const char *cw_tilemap2d_terrain_name(cw_node n, int terrain) {
  cw__terrains *T = cw__tilemap2d_terrains(n);
  return T && terrain >= 1 && terrain <= T->list.count ? T->list.data[terrain - 1].name : "";
}

int cw_tilemap2d_get_terrain(cw_node n, int layer, int x, int y) {
  ctx c;
  if (!make_ctx(&c, n, layer)) return 0;
  return cell_terrain(&c, x, y, NULL);
}

void cw_tilemap2d_fix_terrain(cw_node n, int layer, int x0, int y0, int x1, int y1) {
  ctx c;
  if (!make_ctx(&c, n, layer)) return;
  x0 = CW_MAX(x0, 0), y0 = CW_MAX(y0, 0), x1 = CW_MIN(x1, c.w - 1), y1 = CW_MIN(y1, c.h - 1);
  if (x1 < x0 || y1 < y0) return;
  /* corner terrains: every vertex of the region from the cells round it first (as the cells are now), then the
   * cells from the vertices, a ring wider: a block of whole tiles grows its transitions */
  for (int t = 1; t <= c.T->list.count; t++) {
    if (c.T->list.data[t - 1].mode != MODE_CORNER16) continue;
    int cx0 = x0 - 1, cy0 = y0 - 1, cx1 = x1 + 1, cy1 = y1 + 1;
    int vw = cx1 - cx0 + 2, vh = cy1 - cy0 + 2;
    uint8_t *v = cw_alloc((size_t)vw * (size_t)vh, CW_MEM_SCENE);
    bool any = false;
    for (int y = 0; y < vh; y++)
      for (int x = 0; x < vw; x++) any |= (v[(size_t)y * (size_t)vw + (size_t)x] = vertex_in(&c, t, cx0 + x, cy0 + y)) != 0;
    if (any) write_corners(&c, t, cx0, cy0, cx1, cy1, v);
    cw_free(v);
  }
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) fix_cell(&c, x, y);
}

void cw_tilemap2d_set_terrain_rect(cw_node n, int layer, int x0, int y0, int x1, int y1, int terrain) {
  ctx c;
  if (!make_ctx(&c, n, layer) || terrain < 0 || terrain > c.T->list.count || x1 < x0 || y1 < y0) return;
  x0 = CW_MAX(x0, 0), y0 = CW_MAX(y0, 0);
  if (x1 < x0 || y1 < y0) return;
  if (terrain && (x1 >= c.w || y1 >= c.h)) {
    /* painting past the map grows it */
    cw_tilemap2d_set_cell(n, layer, CW_MAX(x1, c.w - 1), CW_MAX(y1, c.h - 1),
                          cw_tilemap2d_get_cell(n, layer, CW_MAX(x1, c.w - 1), CW_MAX(y1, c.h - 1)), 0);
    cw_tilemap2d_size(n, &c.w, &c.h);
  }
  x1 = CW_MIN(x1, c.w - 1), y1 = CW_MIN(y1, c.h - 1);
  if (x1 < x0 || y1 < y0) return;
  /* what the cells hold now: a corner terrain is erased by vertex, a cell terrain by cell */
  for (int t = 1; t <= c.T->list.count; t++) {
    const terrain_def *tr = &c.T->list.data[t - 1];
    if (tr->mode != MODE_CORNER16) continue;
    /* vertices x0..x1+1, y0..y1+1 become `terrain == t`; the cells that touch them are x0-1..x1+1 */
    int cx0 = x0 - 1, cy0 = y0 - 1, cx1 = x1 + 1, cy1 = y1 + 1;
    int vw = cx1 - cx0 + 2, vh = cy1 - cy0 + 2;
    uint8_t *v = cw_alloc((size_t)vw * (size_t)vh, CW_MEM_SCENE);
    bool any = false, touched = false;
    for (int y = 0; y < vh; y++)
      for (int x = 0; x < vw; x++) {
        bool in = vertex_in(&c, t, cx0 + x, cy0 + y);
        if (x >= 1 && x <= vw - 2 && y >= 1 && y <= vh - 2) {
          touched |= in != (terrain == t);
          in = terrain == t;
        }
        v[(size_t)y * (size_t)vw + (size_t)x] = in;
        any |= in;
      }
    if (touched || any) write_corners(&c, t, cx0, cy0, cx1, cy1, v);
    cw_free(v);
  }
  const terrain_def *want = terrain ? &c.T->list.data[terrain - 1] : NULL;
  if (!want || want->mode != MODE_CORNER16)
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        int cur = cell_terrain(&c, x, y, NULL);
        if (cur == terrain && terrain) continue;
        if (cur && c.T->list.data[cur - 1].mode == MODE_CORNER16) continue; /* handled by vertex above */
        if (!terrain) {
          if (cur) cw_tilemap2d_set_cell(n, layer, x, y, -1, 0);
        } else {
          put(&c, terrain, x, y, 0); /* any tile of the terrain marks the cell; the pass below picks the right one */
        }
      }
  /* the cells and what is round them: a ring for the cells' own neighbours, one more for the neighbours of a
   * corner terrain's transition cells */
  for (int y = y0 - 2; y <= y1 + 2; y++)
    for (int x = x0 - 2; x <= x1 + 2; x++) fix_cell(&c, x, y);
}

void cw_tilemap2d_set_terrain(cw_node n, int layer, int x, int y, int terrain) {
  cw_tilemap2d_set_terrain_rect(n, layer, x, y, x, y, terrain);
}
