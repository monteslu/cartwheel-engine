/* TileMap2D: a render2d tilemap with its cells in a compact text form
 * (cartwheel/scene2d.h describes the format). */
#include <stdio.h>
#include <stdlib.h>

#include "scene2d_internal.h"

#define TM_LAYERS 8
#define TM_EDIT_LOG 256
#define TM_SUB CW__TILE_SUB

/* a tile's collision shape, tile pixels */
typedef struct tm_shape {
  int lo, hi;      /* tile ids it covers */
  uint8_t kind;    /* 0 none, 1 polygon, 2 one-way segment */
  uint8_t n;
  float pt[CW__TILE_SHAPE_PTS][2];
} tm_shape;
typedef struct tm_datum {
  const char *name; /* interned */
  int lo, hi;
  float num;
  const char *text; /* interned; "" for a number */
} tm_datum;

typedef struct tilemap2d {
  const char *tileset;
  int filter;
  cw_vec2 tile_size, cell_size;
  int margin, spacing, columns;
  int layers;
  const char *animations;
  int occluder_layers; /* bit k: map layer k casts shadows by its alpha in lit canvases */
  /* per-tile collision shapes and custom data (text forms: cartwheel/scene2d.h) */
  const char *tile_shapes, *tile_data;
  CW_ARRAY(tm_shape) shapes;
  CW_ARRAY(tm_datum) data;
  /* what changed, for whoever builds from the cells (TileCollision2D, the navigation bake): every cell edit
   * gets a serial and sits in a ring; a change too large to list (a cells text, a resize, new shapes) moves epoch */
  uint32_t serial, epoch;
  cw__tile_edit edits[TM_EDIT_LOG];
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
  cw_array_free(&m->shapes);
  cw_array_free(&m->data);
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
  m->epoch++;
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
  m->epoch++;
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
    if (m->occluder_layers & (1 << l)) cw_r2d_set_occluder(true);
    cw_r2d_tilemap(m->map, l, cw_v2(0, 0), it->modulate);
    if (m->occluder_layers & (1 << l)) cw_r2d_set_occluder(false);
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
static void parse_tile_shapes(tilemap2d *m);
static void set_tile_size(cw_node n, void *d, const cw_value *v) {
  (void)n;
  tilemap2d *m = d;
  m->tile_size = cw_v2(v->v[0], v->v[1]);
  m->rebuild = true;
  parse_tile_shapes(m); /* "full" and "slope" are written against the tile size */
}
static void get_cell_size(cw_node n, void *d, cw_value *o) { (void)n; tilemap2d *m = d; *o = cw_val_vec2(m->cell_size.x, m->cell_size.y); }
static void set_cell_size(cw_node n, void *d, const cw_value *v) { (void)n; tilemap2d *m = d; m->cell_size = cw_v2(v->v[0], v->v[1]); m->rebuild = true; m->epoch++; }
static void get_anims(cw_node n, void *d, cw_value *o) { (void)n; tilemap2d *m = d; *o = cw_val_string(m->animations ? m->animations : ""); }
static void set_anims(cw_node n, void *d, const cw_value *v) { (void)n; tilemap2d *m = d; m->animations = cw_intern(v->s ? v->s : ""); m->rebuild = true; }

/* ── per-tile collision shapes and custom data ─────────────────────── */

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

/* "8-12, 24" -> ranges; stops at the first char that is not part of an id list. Returns the count. */
static int parse_ids(const char **pp, int (*out)[2], int cap) {
  const char *p = *pp;
  int n = 0;
  for (;;) {
    p = skip_ws(p);
    char *e;
    long a = strtol(p, &e, 10);
    if (e == p) break;
    long b = a;
    p = skip_ws(e);
    if (*p == '-') {
      b = strtol(p + 1, &e, 10);
      p = skip_ws(e);
    }
    if (n < cap) out[n][0] = (int)CW_MIN(a, b), out[n][1] = (int)CW_MAX(a, b), n++;
    if (*p != ',') break;
    p++;
  }
  *pp = p;
  return n;
}

static int parse_floats(const char **pp, float *out, int cap) {
  const char *p = *pp;
  int n = 0;
  for (;;) {
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    char *e;
    double d = strtod(p, &e);
    if (e == p) break;
    if (n < cap) out[n++] = (float)d;
    p = e;
  }
  *pp = p;
  return n;
}

static bool word_is(const char *p, const char *w, const char **after) {
  size_t n = strlen(w);
  if (strncmp(p, w, n) != 0) return false;
  char c = p[n];
  if ((c >= 'a' && c <= 'z') || c == '_') return false;
  *after = p + n;
  return true;
}

static void parse_tile_shapes(tilemap2d *m) {
  cw_array_clear(&m->shapes);
  float tw = m->tile_size.x, th = m->tile_size.y;
  const char *p = m->tile_shapes ? m->tile_shapes : "";
  while (*(p = skip_ws(p))) {
    if (*p == ';') { p++; continue; }
    int ids[16][2];
    const char *start = p;
    int nid = parse_ids(&p, ids, 16);
    p = skip_ws(p);
    tm_shape sh = { 0 };
    float v[CW__TILE_SHAPE_PTS * 2];
    const char *q = p;
    bool ok = nid > 0 && *p == ':';
    if (ok) {
      p = skip_ws(p + 1);
      if (word_is(p, "full", &q)) {
        sh.kind = 1, sh.n = 4;
        float r[4][2] = { { 0, 0 }, { tw, 0 }, { tw, th }, { 0, th } };
        memcpy(sh.pt, r, sizeof r);
      } else if (word_is(p, "none", &q)) {
        sh.kind = 0;
      } else if (word_is(p, "rect", &q)) {
        ok = parse_floats(&q, v, 4) == 4 && v[2] > 0 && v[3] > 0;
        sh.kind = 1, sh.n = 4;
        float r[4][2] = { { v[0], v[1] }, { v[0] + v[2], v[1] }, { v[0] + v[2], v[1] + v[3] }, { v[0], v[1] + v[3] } };
        if (ok) memcpy(sh.pt, r, sizeof r);
      } else if (word_is(p, "slope", &q)) {
        /* the floor's height over the cell's bottom at its left and right edges */
        ok = parse_floats(&q, v, 2) == 2 && (v[0] > 0 || v[1] > 0);
        sh.kind = 1;
        int k = 0;
        if (ok) {
          if (v[0] > 0) sh.pt[k][0] = 0, sh.pt[k][1] = th - v[0], k++;
          if (v[1] > 0) sh.pt[k][0] = tw, sh.pt[k][1] = th - v[1], k++;
          sh.pt[k][0] = tw, sh.pt[k][1] = th, k++;
          sh.pt[k][0] = 0, sh.pt[k][1] = th, k++;
          sh.n = (uint8_t)k;
        }
      } else if (word_is(p, "poly", &q)) {
        int k = parse_floats(&q, v, CW__TILE_SHAPE_PTS * 2);
        ok = k >= 6 && !(k & 1);
        sh.kind = 1, sh.n = (uint8_t)(k / 2);
        if (ok) memcpy(sh.pt, v, sizeof(float) * (size_t)k);
      } else if (word_is(p, "oneway", &q)) {
        int k = parse_floats(&q, v, 4);
        ok = k == 0 || k == 1 || k == 4;
        sh.kind = 2, sh.n = 2;
        if (k == 0) v[0] = 0;
        if (k <= 1) v[1] = v[0], v[0] = 0, v[2] = tw, v[3] = v[1];
        memcpy(sh.pt, v, sizeof(float) * 4);
      } else {
        ok = false;
      }
    }
    if (!ok) {
      int len = 0;
      while (start[len] && start[len] != ';') len++;
      cw_warn("TileMap2D tile_shapes: cannot read '%.*s' (write \"1-4: full; 5: slope 0 16; 6: rect 0 8 16 8; 7: oneway; "
              "8: poly 0 0, 16 0, 8 16\")", len, start);
    } else {
      /* inside the tile */
      for (int i = 0; i < sh.n; i++) sh.pt[i][0] = CW_CLAMP(sh.pt[i][0], 0, tw), sh.pt[i][1] = CW_CLAMP(sh.pt[i][1], 0, th);
      for (int i = 0; i < nid; i++) {
        sh.lo = ids[i][0], sh.hi = ids[i][1];
        cw_array_push(&m->shapes, sh);
      }
      p = q;
    }
    while (*p && *p != ';') p++;
  }
  m->epoch++;
}

/* "damage: 5=1, 6-7=2; sound: 1-4=stone" */
static void parse_tile_data(tilemap2d *m) {
  cw_array_clear(&m->data);
  const char *p = m->tile_data ? m->tile_data : "";
  while (*(p = skip_ws(p))) {
    if (*p == ';') { p++; continue; }
    const char *ns = p;
    while (*p && *p != ':' && *p != ';' && *p != ' ') p++;
    const char *name = cw_intern_n(ns, (size_t)(p - ns));
    p = skip_ws(p);
    if (*p != ':') {
      cw_warn("TileMap2D tile_data: expected ':' after '%s' (write \"damage: 5=1, 6-7=2; sound: 1-4=stone\")", name);
      while (*p && *p != ';') p++;
      continue;
    }
    p++;
    while (*(p = skip_ws(p)) && *p != ';') {
      if (*p == ',') { p++; continue; }
      int ids[1][2];
      const char *before = p;
      /* one range, then '=' (a list here would read "5=1, 6" as ids 5 and... so ranges are given one by one) */
      char *e;
      long a = strtol(p, &e, 10), b = a;
      if (e == p) { p++; continue; }
      p = skip_ws(e);
      if (*p == '-') { b = strtol(p + 1, &e, 10); p = skip_ws(e); }
      ids[0][0] = (int)CW_MIN(a, b), ids[0][1] = (int)CW_MAX(a, b);
      if (*p != '=') {
        cw_warn("TileMap2D tile_data '%s': expected '=' at '%.12s'", name, before);
        while (*p && *p != ',' && *p != ';') p++;
        continue;
      }
      p = skip_ws(p + 1);
      const char *vs = p;
      while (*p && *p != ',' && *p != ';') p++;
      const char *ve = p;
      while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
      const char *text = cw_intern_n(vs, (size_t)(ve - vs));
      double d = strtod(text, &e);
      tm_datum dt = { name, ids[0][0], ids[0][1], (float)d, (e != text && !*e) ? cw_intern("") : text };
      if (e == text) dt.num = 0;
      cw_array_push(&m->data, dt);
    }
  }
}

static const tm_shape *shape_of(const tilemap2d *m, int tile) {
  /* the last entry naming a tile wins */
  for (int i = m->shapes.count - 1; i >= 0; i--)
    if (tile >= m->shapes.data[i].lo && tile <= m->shapes.data[i].hi) return m->shapes.data[i].kind ? &m->shapes.data[i] : NULL;
  return NULL;
}

static void get_tile_shapes(cw_node n, void *d, cw_value *o) { (void)n; tilemap2d *m = d; *o = cw_val_string(m->tile_shapes ? m->tile_shapes : ""); }
static void set_tile_shapes(cw_node n, void *d, const cw_value *v) { (void)n; tilemap2d *m = d; m->tile_shapes = cw_intern(v->s ? v->s : ""); parse_tile_shapes(m); }
static void get_tile_data(cw_node n, void *d, cw_value *o) { (void)n; tilemap2d *m = d; *o = cw_val_string(m->tile_data ? m->tile_data : ""); }
static void set_tile_data(cw_node n, void *d, const cw_value *v) { (void)n; tilemap2d *m = d; m->tile_data = cw_intern(v->s ? v->s : ""); parse_tile_data(m); }

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
  CW_PROP_FIELD(tilemap2d, occluder_layers, CW_T_INT, CW_PROP_DEFAULT, "Bit k: map layer k casts shadows (and blocks GI) by its alpha on lit canvases."),
  CW_PROP_ACCESSOR("tile_shapes", CW_T_STRING, CW_PROP_DEFAULT, get_tile_shapes, set_tile_shapes,
                   "Collision shape per tile id, tile pixels: \"1-4: full; 5: slope 0 16; 6: rect 0 8 16 8; 7: oneway; 8: poly 0 0, 16 0, 8 16\" (a TileCollision2D child builds the colliders)."),
  CW_PROP_ACCESSOR("tile_data", CW_T_STRING, CW_PROP_DEFAULT, get_tile_data, set_tile_data,
                   "Named values per tile id that the game reads: \"damage: 5=1, 6-7=2; sound: 1-4=stone\" (cw_tilemap2d_tile_data)."),
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
  if (m->cells[layer][(size_t)y * (size_t)m->w + (size_t)x] != c) {
    m->edits[m->serial % TM_EDIT_LOG] = (cw__tile_edit){ x, y, layer };
    m->serial++;
  }
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

/* ── raw cells, shapes, custom data ─────────────────────────────────── */

uint32_t cw_tilemap2d_get_raw(cw_node n, int layer, int x, int y) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m || layer < 0 || layer >= TM_LAYERS || x < 0 || y < 0 || x >= m->w || y >= m->h || !m->cells[layer]) return 0;
  return m->cells[layer][(size_t)y * (size_t)m->w + (size_t)x];
}

void cw_tilemap2d_set_raw(cw_node n, int layer, int x, int y, uint32_t cell) {
  uint32_t id = cell & CW_TILE_ID_MASK;
  cw_tilemap2d_set_cell(n, layer, x, y, id ? (int)id - 1 : -1, cell & ~CW_TILE_ID_MASK);
}

cw_vec2 cw_tilemap2d_cell_size(cw_node n) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m) return cw_v2(16, 16);
  return cw_v2(m->cell_size.x > 0 ? m->cell_size.x : m->tile_size.x, m->cell_size.y > 0 ? m->cell_size.y : m->tile_size.y);
}

cw_vec2 cw_tilemap2d_cell_center(cw_node n, int x, int y) {
  cw_vec2 cs = cw_tilemap2d_cell_size(n);
  return cw_v2(((float)x + 0.5f) * cs.x, ((float)y + 0.5f) * cs.y);
}

cw_tile_shape_kind cw_tilemap2d_tile_shape(cw_node n, int tile, cw_vec2 *points, int cap, int *count) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  const tm_shape *sh = m && tile >= 0 ? shape_of(m, tile) : NULL;
  if (count) *count = sh ? sh->n : 0;
  if (!sh) return CW_TILE_SHAPE_NONE;
  for (int i = 0; i < sh->n && i < cap && points; i++) points[i] = cw_v2(sh->pt[i][0], sh->pt[i][1]);
  return sh->kind == 2 ? CW_TILE_SHAPE_ONE_WAY : CW_TILE_SHAPE_SOLID;
}

bool cw__tilemap2d_info(cw_node n, cw__tilemap_info *o) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m) return false;
  o->w = m->w, o->h = m->h, o->layers = CW_CLAMP(m->layers, 1, TM_LAYERS);
  o->tile_size = m->tile_size;
  o->cell_size = cw_tilemap2d_cell_size(n);
  o->ext_x = (int32_t)lroundf(m->tile_size.x * TM_SUB);
  o->ext_y = (int32_t)lroundf(m->tile_size.y * TM_SUB);
  o->serial = m->serial, o->epoch = m->epoch;
  o->tile_shapes = m->tile_shapes;
  return true;
}

void cw__tilemap2d_cell_shape(cw_node n, int layer, int x, int y, cw__tile_shape *out) {
  out->kind = 0, out->n = 0;
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m || layer < 0 || layer >= TM_LAYERS || x < 0 || y < 0 || x >= m->w || y >= m->h || !m->cells[layer] || !m->shapes.count) return;
  uint32_t c = m->cells[layer][(size_t)y * (size_t)m->w + (size_t)x];
  if (!(c & CW_TILE_ID_MASK)) return;
  const tm_shape *sh = shape_of(m, (int)(c & CW_TILE_ID_MASK) - 1);
  if (!sh) return;
  int32_t ex = (int32_t)lroundf(m->tile_size.x * TM_SUB), ey = (int32_t)lroundf(m->tile_size.y * TM_SUB);
  if (ex <= 0 || ey <= 0) return;
  int k = 0;
  for (int i = 0; i < sh->n; i++) {
    int32_t px = (int32_t)lroundf(sh->pt[i][0] * TM_SUB), py = (int32_t)lroundf(sh->pt[i][1] * TM_SUB);
    /* the renderer's order: transpose, then flip x, then flip y */
    if (c & CW_TILE_FLIP_D) {
      int32_t tx = (int32_t)((int64_t)py * ex / ey), ty = (int32_t)((int64_t)px * ey / ex);
      px = tx, py = ty;
    }
    if (c & CW_TILE_FLIP_X) px = ex - px;
    if (c & CW_TILE_FLIP_Y) py = ey - py;
    if (k && out->pt[k - 1][0] == px && out->pt[k - 1][1] == py) continue;
    out->pt[k][0] = px, out->pt[k][1] = py, k++;
  }
  if (sh->kind == 2) {
    /* a platform is stood on from above whatever the flips: left to right, and never upright */
    if (k < 2 || out->pt[0][0] == out->pt[1][0]) return;
    if (out->pt[0][0] > out->pt[1][0]) {
      int32_t t0 = out->pt[0][0], t1 = out->pt[0][1];
      out->pt[0][0] = out->pt[1][0], out->pt[0][1] = out->pt[1][1];
      out->pt[1][0] = t0, out->pt[1][1] = t1;
    }
    out->kind = 2, out->n = 2;
    return;
  }
  if (k > 1 && out->pt[0][0] == out->pt[k - 1][0] && out->pt[0][1] == out->pt[k - 1][1]) k--;
  if (k < 3) return;
  int64_t area = 0;
  for (int i = 0; i < k; i++) {
    int j = (i + 1) % k;
    area += (int64_t)out->pt[i][0] * out->pt[j][1] - (int64_t)out->pt[j][0] * out->pt[i][1];
  }
  if (area == 0) return;
  if (area > 0)
    for (int i = 0; i < k / 2; i++) {
      int32_t t0 = out->pt[i][0], t1 = out->pt[i][1];
      out->pt[i][0] = out->pt[k - 1 - i][0], out->pt[i][1] = out->pt[k - 1 - i][1];
      out->pt[k - 1 - i][0] = t0, out->pt[k - 1 - i][1] = t1;
    }
  out->kind = 1, out->n = (uint8_t)k;
}

int cw__tilemap2d_edits_since(cw_node n, uint32_t since, cw__tile_edit *out, int cap) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m) return 0;
  uint32_t count = m->serial - since;
  if (count > TM_EDIT_LOG || (int)count > cap) return -1;
  for (uint32_t i = 0; i < count; i++) out[i] = m->edits[(since + i) % TM_EDIT_LOG];
  return (int)count;
}

/* point in polygon (shape units), boundary counts as inside */
static bool shape_has_point(const cw__tile_shape *s, int32_t px, int32_t py) {
  bool in = false;
  for (int i = 0, j = s->n - 1; i < s->n; j = i++) {
    int64_t xi = s->pt[i][0], yi = s->pt[i][1], xj = s->pt[j][0], yj = s->pt[j][1];
    int64_t cr = (xj - xi) * (py - yi) - (yj - yi) * (px - xi);
    if (cr == 0 && px >= CW_MIN(xi, xj) && px <= CW_MAX(xi, xj) && py >= CW_MIN(yi, yj) && py <= CW_MAX(yi, yj)) return true;
    if ((yi > py) != (yj > py)) {
      /* x of the edge at py, compared without dividing */
      int64_t lhs = (px - xi) * (yj - yi), rhs = (xj - xi) * (py - yi);
      if ((yj > yi) ? lhs < rhs : lhs > rhs) in = !in;
    }
  }
  return in;
}

bool cw_tilemap2d_cell_solid(cw_node n, int layer, int x, int y) {
  cw__tile_shape s;
  cw__tilemap2d_cell_shape(n, layer, x, y, &s);
  return s.kind == 1;
}

bool cw_tilemap2d_point_solid(cw_node n, int layer, cw_vec2 local) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m) return false;
  int x, y;
  cw_tilemap2d_local_to_cell(n, local, &x, &y);
  cw__tile_shape s;
  cw__tilemap2d_cell_shape(n, layer, x, y, &s);
  if (s.kind != 1) return false;
  cw_vec2 cs = cw_tilemap2d_cell_size(n);
  /* cell pixels -> tile pixels -> shape units */
  float fx = (local.x - (float)x * cs.x) * m->tile_size.x / cs.x, fy = (local.y - (float)y * cs.y) * m->tile_size.y / cs.y;
  return shape_has_point(&s, (int32_t)lroundf(fx * TM_SUB), (int32_t)lroundf(fy * TM_SUB));
}

static const tm_datum *datum_of(cw_node n, int tile, const char *name) {
  tilemap2d *m = cw_node_data_t(n, tm_t);
  if (!m || tile < 0 || !name) return NULL;
  const char *in = cw_intern_find(name);
  if (!in) return NULL;
  for (int i = m->data.count - 1; i >= 0; i--) {
    const tm_datum *d = &m->data.data[i];
    if (d->name == in && tile >= d->lo && tile <= d->hi) return d;
  }
  return NULL;
}

bool cw_tilemap2d_tile_data(cw_node n, int tile, const char *name, float *value) {
  const tm_datum *d = datum_of(n, tile, name);
  if (d && value) *value = d->num;
  return d != NULL;
}

const char *cw_tilemap2d_tile_data_text(cw_node n, int tile, const char *name) {
  const tm_datum *d = datum_of(n, tile, name);
  return d ? d->text : "";
}

float cw_tilemap2d_cell_data(cw_node n, int layer, int x, int y, const char *name, float fallback) {
  float v = fallback;
  cw_tilemap2d_tile_data(n, cw_tilemap2d_get_cell(n, layer, x, y), name, &v);
  return v;
}
