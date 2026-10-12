/* render2d tilemaps: cells in CW_TILE_CHUNK^2 chunks of static geometry,
 * rebuilt when edited, culled to the view, animated in the vertex shader. */
#include <string.h>

#include "internal.h"

typedef struct r2d_tvert {
  float x, y;
  uint16_t id, corner, frames, fps16;
} r2d_tvert;

typedef struct r2d_chunk {
  GLuint vbo, vao;
  void *wvbo; /* WebGPU vertex buffer */
  int quads;
  bool dirty;
} r2d_chunk;

typedef struct r2d_tilemap {
  cw_tilemap_desc d;
  int columns;
  uint32_t *cells;    /* layers * w * h */
  int ccx, ccy;       /* chunks across, down */
  r2d_chunk *chunks;  /* layers * ccx * ccy */
  cw_map anim;        /* id -> frames | fps16 << 16 */
} r2d_tilemap;

void r2d_tilemaps_init(void) { cw_pool_init(&R.tilemaps, sizeof(r2d_tilemap), CW_MEM_RENDER2D); }

static r2d_tilemap *map_get(cw_tilemap m) { return R.inited ? cw_pool_get(&R.tilemaps, m.id) : NULL; }

cw_tilemap cw_tilemap_create(const cw_tilemap_desc *d) {
  if (!r2d_lazy_init()) return (cw_tilemap){ 0 };
  const r2d_tex *t = r2d_tex_get(d->tileset);
  if (!t || t->target || d->width <= 0 || d->height <= 0 || d->tile_w <= 0 || d->tile_h <= 0 || d->layers < 1 ||
      d->layers > 16) {
    cw_error("cw_tilemap_create: needs a loaded image tileset (not a render target), positive width/height and tile "
             "size, and 1..16 layers");
    return (cw_tilemap){ 0 };
  }
  uint64_t id;
  r2d_tilemap *m = cw_pool_alloc(&R.tilemaps, &id);
  m->d = *d;
  if (m->d.cell_w <= 0) m->d.cell_w = (float)d->tile_w;
  if (m->d.cell_h <= 0) m->d.cell_h = (float)d->tile_h;
  m->columns = d->columns > 0 ? d->columns : (t->w - 2 * d->margin + d->spacing) / (d->tile_w + d->spacing);
  if (m->columns < 1) m->columns = 1;
  m->cells = cw_calloc((size_t)d->layers * (size_t)d->width * (size_t)d->height, sizeof(uint32_t), CW_MEM_RENDER2D);
  m->ccx = (d->width + CW_TILE_CHUNK - 1) / CW_TILE_CHUNK;
  m->ccy = (d->height + CW_TILE_CHUNK - 1) / CW_TILE_CHUNK;
  m->chunks = cw_calloc((size_t)d->layers * (size_t)m->ccx * (size_t)m->ccy, sizeof(r2d_chunk), CW_MEM_RENDER2D);
  cw_map_init(&m->anim, CW_MEM_RENDER2D);
  return (cw_tilemap){ id };
}

static void map_free(r2d_tilemap *m) {
  int n = m->d.layers * m->ccx * m->ccy;
  for (int i = 0; i < n; i++) {
    if (m->chunks[i].vbo) glDeleteBuffers(1, &m->chunks[i].vbo);
    if (m->chunks[i].vao) glDeleteVertexArrays(1, &m->chunks[i].vao);
    if (m->chunks[i].wvbo) r2d_wg_buffer_release(m->chunks[i].wvbo);
  }
  cw_free(m->chunks);
  cw_free(m->cells);
  cw_map_free(&m->anim);
}

void cw_tilemap_destroy(cw_tilemap h) {
  r2d_tilemap *m = map_get(h);
  if (!m) return;
  map_free(m);
  cw_pool_release(&R.tilemaps, h.id);
}

void r2d_tilemaps_shutdown(void) {
  for (int i = 0; i < R.tilemaps.slot_count; i++) {
    r2d_tilemap *m = cw_pool_at(&R.tilemaps, i);
    if (m) map_free(m);
  }
  cw_pool_free_all(&R.tilemaps);
}

static r2d_chunk *chunk_at(r2d_tilemap *m, int layer, int cx, int cy) {
  return &m->chunks[((size_t)layer * (size_t)m->ccy + (size_t)cy) * (size_t)m->ccx + (size_t)cx];
}

void cw_tilemap_set(cw_tilemap h, int layer, int x, int y, uint32_t cell) {
  r2d_tilemap *m = map_get(h);
  if (!m || layer < 0 || layer >= m->d.layers || x < 0 || y < 0 || x >= m->d.width || y >= m->d.height) return;
  uint32_t *c = &m->cells[((size_t)layer * (size_t)m->d.height + (size_t)y) * (size_t)m->d.width + (size_t)x];
  if (*c == cell) return;
  *c = cell;
  chunk_at(m, layer, x / CW_TILE_CHUNK, y / CW_TILE_CHUNK)->dirty = true;
}

uint32_t cw_tilemap_get(cw_tilemap h, int layer, int x, int y) {
  r2d_tilemap *m = map_get(h);
  if (!m || layer < 0 || layer >= m->d.layers || x < 0 || y < 0 || x >= m->d.width || y >= m->d.height) return 0;
  return m->cells[((size_t)layer * (size_t)m->d.height + (size_t)y) * (size_t)m->d.width + (size_t)x];
}

void cw_tilemap_set_layer(cw_tilemap h, int layer, const uint32_t *cells) {
  r2d_tilemap *m = map_get(h);
  if (!m || layer < 0 || layer >= m->d.layers) return;
  memcpy(&m->cells[(size_t)layer * (size_t)m->d.width * (size_t)m->d.height], cells,
         sizeof(uint32_t) * (size_t)m->d.width * (size_t)m->d.height);
  for (int cy = 0; cy < m->ccy; cy++)
    for (int cx = 0; cx < m->ccx; cx++) chunk_at(m, layer, cx, cy)->dirty = true;
}

void cw_tilemap_set_animation(cw_tilemap h, int id, int frames, float fps) {
  r2d_tilemap *m = map_get(h);
  if (!m) return;
  int f16 = (int)(fps * 16.0f + 0.5f);
  cw_map_set(&m->anim, (uint64_t)id, (uint64_t)(uint32_t)CW_CLAMP(frames, 1, 65535) | ((uint64_t)CW_CLAMP(f16, 0, 65535) << 16));
  /* chunks holding the tile rebuild */
  for (int l = 0; l < m->d.layers; l++)
    for (int y = 0; y < m->d.height; y++)
      for (int x = 0; x < m->d.width; x++) {
        uint32_t c = m->cells[((size_t)l * (size_t)m->d.height + (size_t)y) * (size_t)m->d.width + (size_t)x];
        if (c && (int)((c & CW_TILE_ID_MASK) - 1) == id) chunk_at(m, l, x / CW_TILE_CHUNK, y / CW_TILE_CHUNK)->dirty = true;
      }
}

void cw_tilemap_world_to_cell(cw_tilemap h, cw_vec2 origin, cw_vec2 w, int *x, int *y) {
  r2d_tilemap *m = map_get(h);
  if (!m) {
    *x = *y = 0;
    return;
  }
  *x = (int)floorf((w.x - origin.x) / m->d.cell_w);
  *y = (int)floorf((w.y - origin.y) / m->d.cell_h);
}

static void ensure_tile_ibo(void) {
  int quads = CW_TILE_CHUNK * CW_TILE_CHUNK;
  if (R.tile_ibo) return;
  uint16_t *ix = cw_alloc(sizeof(uint16_t) * 6 * (size_t)quads, CW_MEM_RENDER2D);
  for (int q = 0; q < quads; q++) {
    uint16_t b = (uint16_t)(q * 4);
    uint16_t s[6] = { b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3) };
    memcpy(ix + 6 * q, s, sizeof s);
  }
  glGenBuffers(1, &R.tile_ibo);
  glBindVertexArray(0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R.tile_ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint16_t) * 6 * (size_t)quads), ix, GL_STATIC_DRAW);
  cw_free(ix);
  R.tile_ibo_quads = quads;
}

static void rebuild(r2d_tilemap *m, int layer, int cx, int cy, r2d_chunk *ch) {
  if (!r2d_wg()) ensure_tile_ibo();
  r2d_tvert *v = cw_frame_alloc(sizeof(r2d_tvert) * 4 * CW_TILE_CHUNK * CW_TILE_CHUNK);
  int q = 0;
  for (int ty = 0; ty < CW_TILE_CHUNK; ty++) {
    int y = cy * CW_TILE_CHUNK + ty;
    if (y >= m->d.height) break;
    for (int tx = 0; tx < CW_TILE_CHUNK; tx++) {
      int x = cx * CW_TILE_CHUNK + tx;
      if (x >= m->d.width) break;
      uint32_t c = m->cells[((size_t)layer * (size_t)m->d.height + (size_t)y) * (size_t)m->d.width + (size_t)x];
      if (!(c & CW_TILE_ID_MASK)) continue;
      uint32_t id = (c & CW_TILE_ID_MASK) - 1;
      uint64_t an = 0;
      cw_map_get(&m->anim, id, &an);
      uint16_t frames = an ? (uint16_t)(an & 0xffff) : 1, fps16 = (uint16_t)(an >> 16);
      static const int px[4] = { 0, 1, 1, 0 }, py[4] = { 0, 0, 1, 1 };
      for (int k = 0; k < 4; k++) {
        /* Tiled's order on the image is diagonal, then x, then y; on the
         * sampling coordinate that is y, then x, then the swap */
        int cu = px[k], cv = py[k];
        if (c & CW_TILE_FLIP_Y) cv = 1 - cv;
        if (c & CW_TILE_FLIP_X) cu = 1 - cu;
        if (c & CW_TILE_FLIP_D) {
          int t = cu;
          cu = cv;
          cv = t;
        }
        v[4 * q + k] = (r2d_tvert){ ((float)x + (float)px[k]) * m->d.cell_w, ((float)y + (float)py[k]) * m->d.cell_h,
                                    (uint16_t)id, (uint16_t)(cu | (cv << 1)), frames, fps16 };
      }
      q++;
    }
  }
  if (r2d_wg()) { /* WebGPU: one vertex buffer per chunk; the shared index buffer lives in wgpu.c */
    ch->wvbo = r2d_wg_buffer(ch->wvbo, v, sizeof(r2d_tvert) * 4 * (size_t)q, false); /* also when empty: built once, as GL */
    ch->quads = q;
    ch->dirty = false;
    R.stats.chunk_rebuilds++;
    return;
  }
  if (!ch->vbo) {
    glGenBuffers(1, &ch->vbo);
    glGenVertexArrays(1, &ch->vao);
    glBindVertexArray(ch->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ch->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R.tile_ibo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(r2d_tvert), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_SHORT, GL_FALSE, sizeof(r2d_tvert), (void *)8);
    glBindVertexArray(0);
  }
  glBindBuffer(GL_ARRAY_BUFFER, ch->vbo);
  if (q) glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(r2d_tvert) * 4 * (size_t)q), v, GL_STATIC_DRAW);
  ch->quads = q;
  ch->dirty = false;
  R.stats.chunk_rebuilds++;
}

void cw_r2d_tilemap(cw_tilemap h, int map_layer, cw_vec2 origin, cw_color tint) {
  r2d_tilemap *m = map_get(h);
  if (!m || map_layer < 0 || map_layer >= m->d.layers) return;
  const r2d_tex *t = r2d_tex_get(m->d.tileset);
  if (!t) return;
  if (t->has_normal && R.layers[R.cur_layer].lit) R.normals_used = true;
  cw_array_push(&R.tdraws, ((r2d_tilemap_draw){ h.id, map_layer, origin, r2d_default_white(tint),
                                                R.has_xform ? R.xform : cw_xform2d_identity(), R.cur_layer }));
  r2d_push_special(t->array, origin.y, (uint32_t)(R.tdraws.count - 1));
}

void r2d_tilemap_render(const r2d_tilemap_draw *d, bool lit, int target_h) {
  (void)target_h;
  r2d_tilemap *m = map_get((cw_tilemap){ d->map_id });
  if (!m) return;
  const r2d_tex *t = r2d_tex_get(m->d.tileset);
  if (!t) return;
  const r2d_array *arr = &R.arrays.data[t->array];
  /* map-local -> canvas */
  cw_xform2d o = { 1, 0, 0, 1, d->origin.x, d->origin.y };
  cw_xform2d lm = cw_xform2d_mul(&d->local, &o);
  cw_xform2d full = cw_xform2d_mul(&R.layer_view[d->canvas_layer], &lm);
  /* visible chunk range: the canvas (clip of the layer) back into the map */
  cw_xform2d inv = cw_xform2d_invert(&full);
  float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
  float cw = (float)R.canvas_w, chh = (float)R.canvas_h;
  cw_vec2 corners[4] = { { 0, 0 }, { cw, 0 }, { cw, chh }, { 0, chh } };
  for (int i = 0; i < 4; i++) {
    cw_vec2 p = cw_xform2d_apply(&inv, corners[i]);
    minx = CW_MIN(minx, p.x);
    miny = CW_MIN(miny, p.y);
    maxx = CW_MAX(maxx, p.x);
    maxy = CW_MAX(maxy, p.y);
  }
  float chw = m->d.cell_w * CW_TILE_CHUNK, chh2 = m->d.cell_h * CW_TILE_CHUNK;
  int x0 = CW_MAX(0, (int)floorf(minx / chw)), y0 = CW_MAX(0, (int)floorf(miny / chh2));
  int x1 = CW_MIN(m->ccx - 1, (int)floorf(maxx / chw)), y1 = CW_MIN(m->ccy - 1, (int)floorf(maxy / chh2));
  if (x1 < x0 || y1 < y0) return;
  if (r2d_wg()) {
    float u[24] = { full.a, full.c, full.tx, R.layers[d->canvas_layer].pixel_snap ? 1.0f : 0.0f,
                    full.b, full.d, full.ty, 0,
                    (float)(t->x + m->d.margin), (float)(t->y + m->d.margin), (float)m->d.tile_w, (float)m->d.tile_h,
                    (float)(m->d.tile_w + m->d.spacing), (float)(m->d.tile_h + m->d.spacing), (float)m->columns, (float)t->page,
                    1.0f / (float)arr->w, 1.0f / (float)arr->h, R.pass.time, 0,
                    d->tint.r, d->tint.g, d->tint.b, d->tint.a };
    r2d_wg_tilemap_begin(u, t->array, lit);
    for (int cy = y0; cy <= y1; cy++)
      for (int cx = x0; cx <= x1; cx++) {
        r2d_chunk *ch = chunk_at(m, d->map_layer, cx, cy);
        if (ch->dirty || (!ch->wvbo && !ch->quads)) rebuild(m, d->map_layer, cx, cy, ch);
        if (!ch->quads) continue;
        r2d_wg_tilemap_chunk(ch->wvbo, ch->quads);
        R.stats.draw_calls++;
        R.stats.tile_chunks++;
      }
    return;
  }
  const r2d_program *p = lit ? &R.tile_lit : &R.tile;
  glUseProgram(p->prog);
  glUniform4f(p->u[U_M0], full.a, full.c, full.tx, R.layers[d->canvas_layer].pixel_snap ? 1.0f : 0.0f);
  glUniform4f(p->u[U_M1], full.b, full.d, full.ty, 0);
  glUniform2f(p->u_target, (float)R.canvas_w, (float)R.canvas_h);
  glUniform4f(p->u[U_SET0], (float)(t->x + m->d.margin), (float)(t->y + m->d.margin), (float)m->d.tile_w,
              (float)m->d.tile_h);
  glUniform4f(p->u[U_SET1], (float)(m->d.tile_w + m->d.spacing), (float)(m->d.tile_h + m->d.spacing),
              (float)m->columns, (float)t->page);
  glUniform2f(p->u[U_INV_PAGE], 1.0f / (float)arr->w, 1.0f / (float)arr->h);
  glUniform1f(p->u_time, R.pass.time);
  glUniform4f(p->u[U_TINT], d->tint.r, d->tint.g, d->tint.b, d->tint.a);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, arr->tex);
  if (lit) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, r2d_array_normal_tex(t->array));
    glActiveTexture(GL_TEXTURE0);
  }
  for (int cy = y0; cy <= y1; cy++)
    for (int cx = x0; cx <= x1; cx++) {
      r2d_chunk *ch = chunk_at(m, d->map_layer, cx, cy);
      if (ch->dirty || (!ch->vbo && !ch->quads)) rebuild(m, d->map_layer, cx, cy, ch);
      if (!ch->quads) continue;
      glBindVertexArray(ch->vao);
      glDrawElements(GL_TRIANGLES, ch->quads * 6, GL_UNSIGNED_SHORT, (void *)0);
      R.stats.draw_calls++;
      R.stats.tile_chunks++;
    }
  glBindVertexArray(0);
}
