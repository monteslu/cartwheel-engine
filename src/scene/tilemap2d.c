/* TileMap2D: a render2d tilemap with its cells in a compact text form
 * (cartwheel/scene2d.h describes the format). */
#include <stdio.h>
#include <stdlib.h>

#include "scene2d_internal.h"

#define TM_LAYERS 8

typedef struct tilemap2d {
  const char *tileset;
  int filter;
  cw_vec2 tile_size, cell_size;
  int margin, spacing, columns;
  int layers;
  const char *animations;
  /* cells: w*h per layer, render2d cell values (id+1 | flags) */
  int w, h;
  uint32_t *cells[TM_LAYERS];
  /* runtime */
  cw_tilemap map;
  bool rebuild;              /* recreate the render2d map */
  bool layer_dirty[TM_LAYERS];
  const char *built_tileset;
} tilemap2d;

static const cw_node_type *tm_t;

static void tm_init(cw_node n, void *d) {
  (void)n;
  tilemap2d *m = d;
  m->filter = 1;
  m->tile_size = cw_v2(16, 16);
  m->layers = 1;
}

static void tm_destroy(cw_node n, void *d) {
  (void)n;
  tilemap2d *m = d;
  for (int l = 0; l < TM_LAYERS; l++) cw_free(m->cells[l]);
  if (!CW_HANDLE_IS_NULL(m->map)) cw_tilemap_destroy(m->map);
}

static void resize(tilemap2d *m, int w, int h) {
  if (w <= m->w && h <= m->h) return;
  int nw = CW_MAX(w, m->w), nh = CW_MAX(h, m->h);
  for (int l = 0; l < TM_LAYERS; l++) {
    uint32_t *c = cw_calloc((size_t)nw * (size_t)nh, sizeof(uint32_t), CW_MEM_SCENE);
    if (m->cells[l])
      for (int y = 0; y < m->h; y++) memcpy(c + (size_t)y * (size_t)nw, m->cells[l] + (size_t)y * (size_t)m->w, sizeof(uint32_t) * (size_t)m->w);
    cw_free(m->cells[l]);
    m->cells[l] = c;
  }
  m->w = nw;
  m->h = nh;
  m->rebuild = true;
}

/* text -> cells of one layer; returns false on a syntax error (logged) */
static void parse_cells(tilemap2d *m, int layer, const char *text) {
  /* pass 1: size */
  int rows = 0, maxw = 0, cur = 0;
  bool any = false;
  for (const char *p = text ? text : ""; *p;) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (*p == '/') { maxw = CW_MAX(maxw, cur); cur = 0; rows++; p++; any = false; continue; }
    const char *s = p;
    while (*p && *p != ' ' && *p != '/' && *p != '\t') p++;
    const char *star = memchr(s, '*', (size_t)(p - s));
    cur += star ? atoi(star + 1) : 1;
    any = true;
  }
  if (any || cur) { maxw = CW_MAX(maxw, cur); rows++; }
  resize(m, maxw, rows);
  if (m->cells[layer]) memset(m->cells[layer], 0, sizeof(uint32_t) * (size_t)m->w * (size_t)m->h);
  int x = 0, y = 0;
  for (const char *p = text ? text : ""; *p;) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (*p == '/') { x = 0; y++; p++; continue; }
    const char *s = p;
    while (*p && *p != ' ' && *p != '/' && *p != '\t') p++;
    uint32_t cell = 0;
    const char *q = s;
    if (*q == '.') q++;
    else {
      char *e;
      long id = strtol(q, &e, 10);
      if (e == q) { cw_warn("TileMap2D: bad cell '%.*s'", (int)(p - s), s); id = -1; }
      q = e;
      if (id >= 0) cell = CW_TILE(id);
      for (; q < p && *q != '*'; q++) {
        if (*q == 'x') cell |= CW_TILE_FLIP_X;
        else if (*q == 'y') cell |= CW_TILE_FLIP_Y;
        else if (*q == 'd') cell |= CW_TILE_FLIP_D;
      }
    }
    int rep = 1;
    const char *star = memchr(s, '*', (size_t)(p - s));
    if (star) rep = atoi(star + 1);
    for (int k = 0; k < rep; k++, x++)
      if (x < m->w && y < m->h) m->cells[layer][(size_t)y * (size_t)m->w + (size_t)x] = cell;
  }
  m->layer_dirty[layer] = true;
}

static void cell_text(uint32_t c, char *buf, int cap) {
  if (!(c & CW_TILE_ID_MASK)) { snprintf(buf, (size_t)cap, "."); return; }
  int n = snprintf(buf, (size_t)cap, "%u", (c & CW_TILE_ID_MASK) - 1);
  if (c & CW_TILE_FLIP_X) n += snprintf(buf + n, (size_t)(cap - n), "x");
  if (c & CW_TILE_FLIP_Y) n += snprintf(buf + n, (size_t)(cap - n), "y");
  if (c & CW_TILE_FLIP_D) snprintf(buf + n, (size_t)(cap - n), "d");
}

static const char *format_cells(tilemap2d *m, int layer) {
  if (!m->cells[layer] || !m->w) return cw_intern("");
  /* trailing empty rows / cells are dropped; runs are compressed */
  int rows = m->h;
  while (rows > 0) {
    bool empty = true;
    for (int x = 0; x < m->w && empty; x++) empty = !m->cells[layer][(size_t)(rows - 1) * (size_t)m->w + (size_t)x];
    if (!empty) break;
    rows--;
  }
  size_t cap = (size_t)m->w * (size_t)rows * 16 + 16, len = 0;
  char *out = cw_alloc(cap, CW_MEM_SCENE);
  out[0] = 0;
  for (int y = 0; y < rows; y++) {
    const uint32_t *row = m->cells[layer] + (size_t)y * (size_t)m->w;
    int end = m->w;
    while (end > 0 && !row[end - 1]) end--;
    if (y) len += (size_t)snprintf(out + len, cap - len, len ? " /" : "/");
    for (int x = 0; x < end;) {
      int run = 1;
      while (x + run < end && row[x + run] == row[x]) run++;
      char c[32];
      cell_text(row[x], c, sizeof c);
      if (run > 2) len += (size_t)snprintf(out + len, cap - len, "%s%s*%d", len ? " " : "", c, run);
      else for (int k = 0; k < run; k++) len += (size_t)snprintf(out + len, cap - len, "%s%s", len ? " " : "", c);
      x += run;
    }
  }
  const char *s = cw_intern(out);
  cw_free(out);
  return s;
}

#define CELLS_ACCESSORS(L) \
  static void cells_get##L(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_string(format_cells(d, L)); } \
  static void cells_set##L(cw_node n, void *d, const cw_value *v) { (void)n; parse_cells(d, L, v->s); }
CELLS_ACCESSORS(0)
CELLS_ACCESSORS(1)
CELLS_ACCESSORS(2)
CELLS_ACCESSORS(3)
CELLS_ACCESSORS(4)
CELLS_ACCESSORS(5)
CELLS_ACCESSORS(6)
CELLS_ACCESSORS(7)

static void ensure_map(tilemap2d *m) {
  cw_texture tex = cw__tex_get(m->tileset, m->filter);
  if (!cw_texture_valid(tex) || !m->w || !m->h) return;
  if (m->built_tileset != m->tileset) m->rebuild = true;
  if (m->rebuild || CW_HANDLE_IS_NULL(m->map)) {
    if (!CW_HANDLE_IS_NULL(m->map)) cw_tilemap_destroy(m->map);
    cw_tilemap_desc d = { .width = m->w, .height = m->h, .layers = CW_CLAMP(m->layers, 1, TM_LAYERS), .tileset = tex,
                          .tile_w = (int)m->tile_size.x, .tile_h = (int)m->tile_size.y, .margin = m->margin,
                          .spacing = m->spacing, .columns = m->columns, .cell_w = m->cell_size.x, .cell_h = m->cell_size.y };
    m->map = cw_tilemap_create(&d);
    m->built_tileset = m->tileset;
    m->rebuild = false;
    /* animations before cells: they take effect when chunks build */
    const char *p = m->animations ? m->animations : "";
    while (*p) {
      char *e;
      long id = strtol(p, &e, 10);
      if (e == p) { p++; continue; }
      p = e;
      while (*p == ' ' || *p == ':') p++;
      long frames = strtol(p, &e, 10);
      p = e;
      double fps = 1;
      while (*p == ' ') p++;
      if (*p == '@') fps = strtod(p + 1, &e), p = e;
      if (frames > 1) cw_tilemap_set_animation(m->map, (int)id, (int)frames, (float)fps);
      while (*p && *p != ';') p++;
    }
    for (int l = 0; l < TM_LAYERS; l++) m->layer_dirty[l] = true;
  }
  for (int l = 0; l < CW_CLAMP(m->layers, 1, TM_LAYERS); l++)
    if (m->layer_dirty[l] && m->cells[l]) {
      cw_tilemap_set_layer(m->map, l, m->cells[l]);
      m->layer_dirty[l] = false;
    }
}

static void tm_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  tilemap2d *m = d;
  ensure_map(m);
  if (CW_HANDLE_IS_NULL(m->map)) return;
  for (int l = 0; l < CW_CLAMP(m->layers, 1, TM_LAYERS); l++) {
    cw_r2d_set_z(it->z + l);
    cw_r2d_tilemap(m->map, l, cw_v2(0, 0), it->modulate);
  }
}

/* property setters that change the map's shape */
static void set_rebuild_int(cw_node n, void *d, const cw_value *v, int off) {
  (void)n;
  *(int *)((uint8_t *)d + off) = (int)v->i;
  ((tilemap2d *)d)->rebuild = true;
}
#define INT_REBUILD(field) \
  static void get_##field(cw_node n, void *d, cw_value *o) { (void)n; *o = cw_val_int(((tilemap2d *)d)->field); } \
  static void set_##field(cw_node n, void *d, const cw_value *v) { set_rebuild_int(n, d, v, (int)offsetof(tilemap2d, field)); }
INT_REBUILD(margin)
INT_REBUILD(spacing)
INT_REBUILD(columns)
INT_REBUILD(layers)
static void get_tile_size(cw_node n, void *d, cw_value *o) { (void)n; tilemap2d *m = d; *o = cw_val_vec2(m->tile_size.x, m->tile_size.y); }
static void set_tile_size(cw_node n, void *d, const cw_value *v) { (void)n; tilemap2d *m = d; m->tile_size = cw_v2(v->v[0], v->v[1]); m->rebuild = true; }
static void get_cell_size(cw_node n, void *d, cw_value *o) { (void)n; tilemap2d *m = d; *o = cw_val_vec2(m->cell_size.x, m->cell_size.y); }
static void set_cell_size(cw_node n, void *d, const cw_value *v) { (void)n; tilemap2d *m = d; m->cell_size = cw_v2(v->v[0], v->v[1]); m->rebuild = true; }
static void get_anims(cw_node n, void *d, cw_value *o) { (void)n; tilemap2d *m = d; *o = cw_val_string(m->animations ? m->animations : ""); }
static void set_anims(cw_node n, void *d, const cw_value *v) { (void)n; tilemap2d *m = d; m->animations = cw_intern(v->s ? v->s : ""); m->rebuild = true; }

static const char *const filter_names[] = { "LINEAR", "NEAREST", NULL };
#define CELLS_PROP(L) CW_PROP_ACCESSOR("cells" #L, CW_T_STRING, CW_PROP_STORED | CW_PROP_EDITOR, cells_get##L, cells_set##L, "Layer " #L " cells (text form).")
static const cw_prop tm_props[] = {
  CW_PROP_FIELD(tilemap2d, tileset, CW_T_STRING, CW_PROP_DEFAULT, "Tileset image asset path."),
  { .name = "filter", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(tilemap2d, filter), .size = sizeof(int),
    .enum_names = filter_names, .doc = "Texture filtering (NEAREST default)." },
  CW_PROP_ACCESSOR("tile_size", CW_T_VEC2, CW_PROP_DEFAULT, get_tile_size, set_tile_size, "Tileset cell size, texture pixels."),
  CW_PROP_ACCESSOR("cell_size", CW_T_VEC2, CW_PROP_DEFAULT, get_cell_size, set_cell_size, "World size of a cell (0 = tile_size)."),
  CW_PROP_ACCESSOR("margin", CW_T_INT, CW_PROP_DEFAULT, get_margin, set_margin, "Tileset border, pixels."),
  CW_PROP_ACCESSOR("spacing", CW_T_INT, CW_PROP_DEFAULT, get_spacing, set_spacing, "Gap between tileset cells, pixels."),
  CW_PROP_ACCESSOR("columns", CW_T_INT, CW_PROP_DEFAULT, get_columns, set_columns, "Tileset columns (0 = from the width)."),
  CW_PROP_ACCESSOR("layers", CW_T_INT, CW_PROP_DEFAULT, get_layers, set_layers, "Map layers drawn, 1..8 (layer k at z + k)."),
  CW_PROP_ACCESSOR("animations", CW_T_STRING, CW_PROP_DEFAULT, get_anims, set_anims, "Animated tiles \"id:frames@fps; ...\"."),
  CELLS_PROP(0), CELLS_PROP(1), CELLS_PROP(2), CELLS_PROP(3), CELLS_PROP(4), CELLS_PROP(5), CELLS_PROP(6), CELLS_PROP(7),
  CW_PROP_END,
};

static cw_node_type tm_type = { .name = "TileMap2D", .parent = "Node2D", .data_size = sizeof(tilemap2d), .props = tm_props,
                                .doc = "A chunked tile grid with layers and animated tiles.", .init = tm_init, .destroy = tm_destroy };

void cw__register_tilemap2d_type(void) {
  cw_node_type_register(&tm_type);
  tm_t = &tm_type;
  cw_canvas_register_type("TileMap2D", tm_draw);
}

void cw_tilemap2d_set_cell(cw_node n, int layer, int x, int y, int tile, uint32_t flags) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m || layer < 0 || layer >= TM_LAYERS || x < 0 || y < 0) return;
  resize(m, x + 1, y + 1);
  uint32_t c = tile < 0 ? 0 : (CW_TILE(tile) | (flags & ~CW_TILE_ID_MASK));
  m->cells[layer][(size_t)y * (size_t)m->w + (size_t)x] = c;
  if (!CW_HANDLE_IS_NULL(m->map) && !m->rebuild && layer < m->layers) cw_tilemap_set(m->map, layer, x, y, c);
  else m->layer_dirty[layer] = true;
}

int cw_tilemap2d_get_cell(cw_node n, int layer, int x, int y) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m || layer < 0 || layer >= TM_LAYERS || x < 0 || y < 0 || x >= m->w || y >= m->h || !m->cells[layer]) return -1;
  uint32_t c = m->cells[layer][(size_t)y * (size_t)m->w + (size_t)x] & CW_TILE_ID_MASK;
  return c ? (int)c - 1 : -1;
}

void cw_tilemap2d_size(cw_node n, int *w, int *h) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  *w = m ? m->w : 0;
  *h = m ? m->h : 0;
}

void cw_tilemap2d_local_to_cell(cw_node n, cw_vec2 p, int *x, int *y) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  float cw = 16, ch = 16;
  if (m) {
    cw = m->cell_size.x > 0 ? m->cell_size.x : m->tile_size.x;
    ch = m->cell_size.y > 0 ? m->cell_size.y : m->tile_size.y;
  }
  *x = (int)floorf(p.x / cw);
  *y = (int)floorf(p.y / ch);
}
