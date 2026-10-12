/* render2d: sprites, nine-slice, shapes, arbitrary geometry. Everything
 * here becomes vertices in the stream at submission time. */
#include <string.h>

#include "internal.h"
#include "cartwheel/jobs.h"

static const uint32_t QUAD_IDX[6] = { 0, 1, 2, 0, 2, 3 };

static inline void put(r2d_vertex *v, float x, float y, float u, float vv, const uint8_t c[4], uint8_t page,
                       uint8_t mode, uint8_t layer) {
  if (R.has_xform) {
    float tx = R.xform.a * x + R.xform.c * y + R.xform.tx;
    y = R.xform.b * x + R.xform.d * y + R.xform.ty;
    x = tx;
  }
  v->x = x;
  v->y = y;
  v->u = u;
  v->v = vv;
  memcpy(v->rgba, c, 4);
  v->info[0] = page;
  v->info[1] = mode;
  v->info[2] = layer;
  v->info[3] = 0;
  memset(v->sdf, 0, 4);
}

static inline float xf_y(float x, float y) { return R.has_xform ? R.xform.b * x + R.xform.d * y + R.xform.ty : y; }

/* ── sprites ───────────────────────────────────────────────────────────── */

typedef struct sprite_ctx {
  const cw_sprite *s;
  r2d_vertex *v;
  uint32_t *idx;
  uint32_t base;
  uint8_t mode_add, layer;
} sprite_ctx;

static void sprite_fill(const cw_sprite *s, r2d_vertex *v, uint32_t *idx, uint32_t base, uint8_t mode_add,
                        uint8_t layer) {
  const r2d_tex *t = CW_HANDLE_IS_NULL(s->texture) ? NULL : r2d_tex_get(s->texture);
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0, w, h;
  uint8_t page = 0, mode = mode_add;
  if (t) {
    cw_rect src = s->src;
    if (src.w == 0 && src.h == 0) src = (cw_rect){ 0, 0, (float)t->w, (float)t->h };
    r2d_tex_uv(t, src, &u0, &v0, &u1, &v1);
    w = s->size.x != 0 ? s->size.x : src.w;
    h = s->size.y != 0 ? s->size.y : src.h;
    page = (uint8_t)t->page;
  } else {
    w = s->size.x != 0 ? s->size.x : 1;
    h = s->size.y != 0 ? s->size.y : 1;
    mode |= R2D_MODE_SOLID;
  }
  if (s->flip_x) {
    float k = u0;
    u0 = u1;
    u1 = k;
  }
  if (s->flip_y) {
    float k = v0;
    v0 = v1;
    v1 = k;
  }
  float sx = s->scale.x, sy = s->scale.y;
  if (sx == 0 && sy == 0) sx = sy = 1;
  w *= sx;
  h *= sy;
  float x0 = -s->pivot.x * w, y0 = -s->pivot.y * h, x1 = x0 + w, y1 = y0 + h;
  float cs = 1, sn = 0;
  if (s->rotation != 0) cw_sincos(s->rotation, &sn, &cs);
  float px = s->pos.x, py = s->pos.y;
  uint8_t c[4];
  r2d_pack_color(c, r2d_default_white(s->color));
  float lx[4] = { x0, x1, x1, x0 }, ly[4] = { y0, y0, y1, y1 };
  float uu[4] = { u0, u1, u1, u0 }, vv[4] = { v0, v0, v1, v1 };
  for (int k = 0; k < 4; k++)
    put(&v[k], px + cs * lx[k] - sn * ly[k], py + sn * lx[k] + cs * ly[k], uu[k], vv[k], c, page, mode, layer);
  for (int k = 0; k < 6; k++) idx[k] = base + QUAD_IDX[k];
}

void cw_r2d_sprite(const cw_sprite *s) {
  const r2d_tex *t = CW_HANDLE_IS_NULL(s->texture) ? NULL : r2d_tex_get(s->texture);
  uint32_t *idx, base;
  if (t && t->has_normal && R.layers[R.cur_layer].lit) R.normals_used = true;
  r2d_vertex *v = r2d_alloc(4, 6, &idx, &base, t ? t->array : R2D_NO_ARRAY, xf_y(s->pos.x, s->pos.y), s->z);
  sprite_fill(s, v, idx, base, r2d_mode(false) & R2D_MODE_ADD, (uint8_t)R.cur_layer);
}

static void sprites_range(int begin, int end, int worker, void *p) {
  (void)worker;
  sprite_ctx *c = p;
  for (int i = begin; i < end; i++)
    sprite_fill(&c->s[i], c->v + 4 * (size_t)i, c->idx + 6 * (size_t)i, c->base + 4u * (uint32_t)i, c->mode_add,
                c->layer);
}

void cw_r2d_sprites(const cw_sprite *s, int n) {
  if (n <= 0) return;
  if (n < 512) {
    for (int i = 0; i < n; i++) cw_r2d_sprite(&s[i]);
    return;
  }
  /* commands first (sequential: keys, states, merging runs), then the
   * vertices of all n sprites in parallel: each sprite owns 4 vertices and 6
   * indices at fixed offsets */
  CW_ASSERT(R.in_pass, "submit between cw_r2d_begin and cw_r2d_end");
  int v0 = R.verts.count, i0 = R.idx.count;
  cw_array_reserve(&R.verts, v0 + 4 * n);
  cw_array_reserve(&R.idx, i0 + 6 * n);
  R.verts.count += 4 * n;
  R.idx.count += 6 * n;
  bool lit = R.layers[R.cur_layer].lit, ysort = R.layers[R.cur_layer].y_sort;
  uint64_t last_tex = ~0ull;
  uint32_t array = R2D_NO_ARRAY, state = 0;
  int last_z = 1 << 30;
  uint64_t key = 0;
  for (int i = 0; i < n; i++) {
    if (s[i].texture.id != last_tex) {
      last_tex = s[i].texture.id;
      const r2d_tex *ti = CW_HANDLE_IS_NULL(s[i].texture) ? NULL : r2d_tex_get(s[i].texture);
      if (ti && ti->has_normal && lit) R.normals_used = true;
      array = ti ? ti->array : R2D_NO_ARRAY;
      state = r2d_current_state(array);
    }
    if (ysort || s[i].z != last_z) {
      key = r2d_make_key(s[i].z, xf_y(s[i].pos.x, s[i].pos.y));
      last_z = s[i].z;
    }
    r2d_push_cmd(key, state, (uint32_t)(i0 + 6 * i), 6);
  }
  sprite_ctx c = { s, R.verts.data + v0, R.idx.data + i0, (uint32_t)v0, (uint8_t)(r2d_mode(false) & R2D_MODE_ADD),
                   (uint8_t)R.cur_layer };
  cw_parallel_for(n, 2048, sprites_range, &c);
}

/* ── nine-slice ────────────────────────────────────────────────────────── */

void cw_r2d_nine_slice(const cw_nine_slice *n) {
  const r2d_tex *t = r2d_tex_get(n->texture);
  if (!t) {
    cw_r2d_rect(n->dst, r2d_default_white(n->color));
    return;
  }
  cw_rect src = n->src;
  if (src.w == 0 && src.h == 0) src = (cw_rect){ 0, 0, (float)t->w, (float)t->h };
  float ms = n->margin_scale != 0 ? n->margin_scale : 1;
  float sl = n->left, sr = n->right, st = n->top, sb = n->bottom;
  float dl = sl * ms, dr = sr * ms, dt = st * ms, db = sb * ms;
  cw_rect d = n->dst;
  if (dl + dr > d.w && dl + dr > 0) {
    float k = d.w / (dl + dr);
    dl *= k;
    dr *= k;
  }
  if (dt + db > d.h && dt + db > 0) {
    float k = d.h / (dt + db);
    dt *= k;
    db *= k;
  }
  float xs[4] = { d.x, d.x + dl, d.x + d.w - dr, d.x + d.w };
  float ys[4] = { d.y, d.y + dt, d.y + d.h - db, d.y + d.h };
  float sxs[4] = { src.x, src.x + sl, src.x + src.w - sr, src.x + src.w };
  float sys[4] = { src.y, src.y + st, src.y + src.h - sb, src.y + src.h };
  float us[4], vs[4];
  for (int i = 0; i < 4; i++) r2d_tex_uv_point(t, sxs[i], sys[i], &us[i], &vs[i]);
  uint8_t c[4];
  r2d_pack_color(c, r2d_default_white(n->color));
  int quads = n->hollow ? 8 : 9;
  uint32_t *idx, base;
  if (t->has_normal && R.layers[R.cur_layer].lit) R.normals_used = true;
  r2d_vertex *v = r2d_alloc(16, quads * 6, &idx, &base, t->array, xf_y(d.x, d.y + d.h), 0);
  uint8_t mode = r2d_mode(false), layer = (uint8_t)R.cur_layer;
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++) put(&v[j * 4 + i], xs[i], ys[j], us[i], vs[j], c, (uint8_t)t->page, mode, layer);
  int k = 0;
  for (int j = 0; j < 3; j++)
    for (int i = 0; i < 3; i++) {
      if (n->hollow && i == 1 && j == 1) continue;
      uint32_t a = base + (uint32_t)(j * 4 + i);
      uint32_t q[6] = { a, a + 1, a + 5, a, a + 5, a + 4 };
      memcpy(idx + k, q, sizeof q);
      k += 6;
    }
}

/* ── solid shapes ──────────────────────────────────────────────────────── */

static void solid_quad(const cw_vec2 p[4], const uint8_t c[4], float sort_y) {
  uint32_t *idx, base;
  r2d_vertex *v = r2d_alloc(4, 6, &idx, &base, R2D_NO_ARRAY, sort_y, 0);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  for (int k = 0; k < 4; k++) put(&v[k], p[k].x, p[k].y, 0, 0, c, 0, mode, layer);
  for (int k = 0; k < 6; k++) idx[k] = base + QUAD_IDX[k];
}

void cw_r2d_rect(cw_rect r, cw_color col) {
  uint8_t c[4];
  r2d_pack_color(c, col);
  cw_vec2 p[4] = { { r.x, r.y }, { r.x + r.w, r.y }, { r.x + r.w, r.y + r.h }, { r.x, r.y + r.h } };
  solid_quad(p, c, xf_y(r.x, r.y + r.h));
}

void cw_r2d_rect_lines(cw_rect r, float width, cw_color col) {
  float w = CW_MIN(width, CW_MIN(r.w, r.h) * 0.5f);
  uint8_t c[4];
  r2d_pack_color(c, col);
  uint32_t *idx, base;
  r2d_vertex *v = r2d_alloc(8, 24, &idx, &base, R2D_NO_ARRAY, xf_y(r.x, r.y + r.h), 0);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  float ox[4] = { r.x, r.x + r.w, r.x + r.w, r.x }, oy[4] = { r.y, r.y, r.y + r.h, r.y + r.h };
  float ix[4] = { r.x + w, r.x + r.w - w, r.x + r.w - w, r.x + w }, iy[4] = { r.y + w, r.y + w, r.y + r.h - w, r.y + r.h - w };
  for (int k = 0; k < 4; k++) {
    put(&v[k], ox[k], oy[k], 0, 0, c, 0, mode, layer);
    put(&v[4 + k], ix[k], iy[k], 0, 0, c, 0, mode, layer);
  }
  for (int k = 0; k < 4; k++) {
    uint32_t a = base + (uint32_t)k, b = base + (uint32_t)((k + 1) & 3);
    uint32_t q[6] = { a, b, b + 4, a, b + 4, a + 4 };
    memcpy(idx + 6 * k, q, sizeof q);
  }
}

void cw_r2d_line(cw_vec2 a, cw_vec2 b, float width, cw_color col) {
  cw_vec2 d = cw_v2_sub(b, a);
  float len = cw_v2_length(d);
  if (len <= 0) return;
  float hw = width * 0.5f;
  cw_vec2 n = { -d.y / len * hw, d.x / len * hw };
  uint8_t c[4];
  r2d_pack_color(c, col);
  cw_vec2 p[4] = { cw_v2_add(a, n), cw_v2_add(b, n), cw_v2_sub(b, n), cw_v2_sub(a, n) };
  solid_quad(p, c, xf_y(a.x, CW_MAX(a.y, b.y)));
}

void cw_r2d_polyline(const cw_vec2 *pts, int n, float width, bool closed, cw_color col) {
  if (n < 2) return;
  int segs = closed ? n : n - 1;
  int joins = closed ? n : n - 2;
  float hw = width * 0.5f;
  /* per segment a quad; per join up to 2 triangles filling the outer gap */
  uint32_t *idx, base;
  float maxy = pts[0].y;
  for (int i = 1; i < n; i++) maxy = CW_MAX(maxy, pts[i].y);
  r2d_vertex *v = r2d_alloc(segs * 4 + joins * 4, segs * 6 + joins * 6, &idx, &base, R2D_NO_ARRAY, xf_y(pts[0].x, maxy), 0);
  uint8_t c[4];
  r2d_pack_color(c, col);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  int vi = 0, ii = 0;
  for (int s = 0; s < segs; s++) {
    cw_vec2 a = pts[s], b = pts[(s + 1) % n];
    cw_vec2 d = cw_v2_sub(b, a);
    float len = cw_v2_length(d);
    cw_vec2 nn = len > 0 ? (cw_vec2){ -d.y / len * hw, d.x / len * hw } : (cw_vec2){ 0, 0 };
    cw_vec2 p[4] = { cw_v2_add(a, nn), cw_v2_add(b, nn), cw_v2_sub(b, nn), cw_v2_sub(a, nn) };
    for (int k = 0; k < 4; k++) put(&v[vi + k], p[k].x, p[k].y, 0, 0, c, 0, mode, layer);
    for (int k = 0; k < 6; k++) idx[ii + k] = base + (uint32_t)vi + QUAD_IDX[k];
    vi += 4;
    ii += 6;
  }
  for (int j = 0; j < joins; j++) {
    int i = closed ? j : j + 1; /* the joint vertex */
    cw_vec2 P = pts[i], A = pts[(i - 1 + n) % n], B = pts[(i + 1) % n];
    cw_vec2 da = cw_v2_normalize(cw_v2_sub(P, A)), db = cw_v2_normalize(cw_v2_sub(B, P));
    cw_vec2 na = { -da.y, da.x }, nb = { -db.y, db.x };
    float cr = cw_v2_cross(da, db);
    float sgn = cr > 0 ? -1.0f : 1.0f; /* the outer side of the turn */
    cw_vec2 oa = cw_v2_add(P, cw_v2_scale(na, sgn * hw)), ob = cw_v2_add(P, cw_v2_scale(nb, sgn * hw));
    cw_vec2 m = cw_v2_normalize(cw_v2_add(na, nb));
    float dd = cw_v2_dot(m, na);
    cw_vec2 tip = ob; /* bevel: the tip collapses onto ob */
    if (dd > 0.25f) tip = cw_v2_add(P, cw_v2_scale(m, sgn * hw / dd)); /* within a 4x miter */
    cw_vec2 q[4] = { P, oa, tip, ob };
    for (int k = 0; k < 4; k++) put(&v[vi + k], q[k].x, q[k].y, 0, 0, c, 0, mode, layer);
    for (int k = 0; k < 6; k++) idx[ii + k] = base + (uint32_t)vi + QUAD_IDX[k];
    vi += 4;
    ii += 6;
  }
}

static int auto_segments(float r, int segments) {
  if (segments > 2) return segments;
  int s = (int)(sqrtf(CW_MAX(r * r2d_px(), 0)) * 4.0f); /* by the pixels it covers */
  return CW_CLAMP(s, 12, 128);
}

void cw_r2d_circle(cw_vec2 ctr, float r, int segments, cw_color col) {
  int n = auto_segments(r, segments);
  uint8_t c[4];
  r2d_pack_color(c, col);
  uint32_t *idx, base;
  r2d_vertex *v = r2d_alloc(n + 1, n * 3, &idx, &base, R2D_NO_ARRAY, xf_y(ctr.x, ctr.y), 0);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  put(&v[0], ctr.x, ctr.y, 0, 0, c, 0, mode, layer);
  for (int i = 0; i < n; i++) {
    float s, co;
    cw_sincos(CW_TAU * (float)i / (float)n, &s, &co);
    put(&v[1 + i], ctr.x + co * r, ctr.y + s * r, 0, 0, c, 0, mode, layer);
    idx[3 * i] = base;
    idx[3 * i + 1] = base + 1 + (uint32_t)i;
    idx[3 * i + 2] = base + 1 + (uint32_t)((i + 1) % n);
  }
}

void cw_r2d_circle_lines(cw_vec2 ctr, float r, float width, int segments, cw_color col) {
  int n = auto_segments(r, segments);
  uint8_t c[4];
  r2d_pack_color(c, col);
  uint32_t *idx, base;
  r2d_vertex *v = r2d_alloc(2 * n, 6 * n, &idx, &base, R2D_NO_ARRAY, xf_y(ctr.x, ctr.y), 0);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  float ro = r + width * 0.5f, ri = CW_MAX(0, r - width * 0.5f);
  for (int i = 0; i < n; i++) {
    float s, co;
    cw_sincos(CW_TAU * (float)i / (float)n, &s, &co);
    put(&v[2 * i], ctr.x + co * ro, ctr.y + s * ro, 0, 0, c, 0, mode, layer);
    put(&v[2 * i + 1], ctr.x + co * ri, ctr.y + s * ri, 0, 0, c, 0, mode, layer);
    uint32_t a = base + 2u * (uint32_t)i, b = base + 2u * (uint32_t)((i + 1) % n);
    uint32_t q[6] = { a, b, b + 1, a, b + 1, a + 1 };
    memcpy(idx + 6 * i, q, sizeof q);
  }
}

/* ── batched shapes: one submission each, however many segments ─────── */

static int corner_segments(float rad, int segments) {
  if (segments > 0) return CW_MIN(segments, 64);
  int s = (int)(sqrtf(CW_MAX(rad * r2d_px(), 0)) * 1.6f) + 2; /* by the pixels it covers */
  return CW_CLAMP(s, 2, 16);
}

/* the corner arcs of a rounded rect inset by `ins`, (seg + 1) points per
 * corner from the bottom-right one clockwise on screen (0 rad = +x); cs/sn:
 * the quarter's unit vectors, shared by every corner and ring */
static void rr_ring(cw_rect r, float rad, float ins, int seg, const float *cs, const float *sn, cw_vec2 *out) {
  float x0 = r.x + ins, y0 = r.y + ins, x1 = r.x + r.w - ins, y1 = r.y + r.h - ins;
  float rr = CW_MAX(rad - ins, 0);
  float cx[4] = { x1 - rr, x0 + rr, x0 + rr, x1 - rr }, cy[4] = { y1 - rr, y1 - rr, y0 + rr, y0 + rr };
  int n = 0;
  for (int k = 0; k < 4; k++)
    for (int i = 0; i <= seg; i++) {
      float c = cs[i], s = sn[i], ux, uy; /* the quarter turned k * 90 degrees */
      if (k == 0) ux = c, uy = s;
      else if (k == 1) ux = -s, uy = c;
      else if (k == 2) ux = -c, uy = -s;
      else ux = s, uy = -c;
      out[n++] = (cw_vec2){ cx[k] + rr * ux, cy[k] + rr * uy };
    }
}

static void quarter(int seg, float *cs, float *sn) {
  for (int i = 0; i <= seg; i++) cw_sincos(CW_PI * 0.5f * (float)i / (float)seg, &sn[i], &cs[i]);
}

void cw_r2d_round_rect(cw_rect r, float radius, int segments, cw_color col) {
  if (r.w <= 0 || r.h <= 0) return;
  float rad = CW_CLAMP(radius, 0, CW_MIN(r.w, r.h) * 0.5f);
  int seg = corner_segments(rad, segments), n = 4 * (seg + 1);
  float cs[65], sn[65];
  quarter(seg, cs, sn);
  cw_vec2 *p = cw_frame_alloc(sizeof(cw_vec2) * (size_t)n);
  rr_ring(r, rad, 0, seg, cs, sn, p);
  cw_r2d_fan(cw_v2(r.x + r.w * 0.5f, r.y + r.h * 0.5f), p, n, true, col);
}

void cw_r2d_round_rect_lines(cw_rect r, float radius, float width, int segments, cw_color col) {
  if (r.w <= 0 || r.h <= 0 || width <= 0) return;
  float rad = CW_CLAMP(radius, 0, CW_MIN(r.w, r.h) * 0.5f);
  float w = CW_MIN(width, CW_MIN(r.w, r.h) * 0.5f);
  int seg = corner_segments(rad, segments), n = 4 * (seg + 1);
  float cs[65], sn[65];
  quarter(seg, cs, sn);
  cw_vec2 *p = cw_frame_alloc(sizeof(cw_vec2) * (size_t)n * 2);
  rr_ring(r, rad, 0, seg, cs, sn, p);
  rr_ring(r, rad, w, seg, cs, sn, p + n);
  uint8_t c[4];
  r2d_pack_color(c, col);
  uint32_t *idx, base;
  r2d_vertex *v = r2d_alloc(2 * n, 6 * n, &idx, &base, R2D_NO_ARRAY, xf_y(r.x, r.y + r.h), 0);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  for (int i = 0; i < 2 * n; i++) put(&v[i], p[i].x, p[i].y, 0, 0, c, 0, mode, layer);
  for (int i = 0; i < n; i++) {
    uint32_t a = base + (uint32_t)i, b = base + (uint32_t)((i + 1) % n);
    uint32_t q[6] = { a, b, b + (uint32_t)n, a, b + (uint32_t)n, a + (uint32_t)n };
    memcpy(idx + 6 * i, q, sizeof q);
  }
}

void cw_r2d_arc(cw_vec2 ctr, float r0, float r1, float a0, float a1, int segments, cw_color col) {
  float sweep = a1 - a0;
  if (r1 <= 0 || sweep == 0) return;
  r0 = CW_CLAMP(r0, 0, r1);
  int n = segments;
  if (n <= 0) n = CW_MAX(2, (int)ceilf((float)auto_segments(r1, 0) * fminf(fabsf(sweep) / CW_TAU, 1.0f)));
  n = CW_MIN(n, 1024);
  /* one sincos for the start and one for the step; the rest rotate */
  float s0, c0, ds, dc;
  cw_sincos(a0, &s0, &c0);
  cw_sincos(sweep / (float)n, &ds, &dc);
  uint8_t c[4];
  r2d_pack_color(c, col);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  uint32_t *idx, base;
  if (r0 <= 0) { /* a pie slice: a fan from the centre */
    r2d_vertex *v = r2d_alloc(n + 2, 3 * n, &idx, &base, R2D_NO_ARRAY, xf_y(ctr.x, ctr.y), 0);
    put(&v[0], ctr.x, ctr.y, 0, 0, c, 0, mode, layer);
    float cx = c0, sx = s0;
    for (int i = 0; i <= n; i++) {
      put(&v[1 + i], ctr.x + cx * r1, ctr.y + sx * r1, 0, 0, c, 0, mode, layer);
      float t = cx * dc - sx * ds;
      sx = sx * dc + cx * ds, cx = t;
    }
    for (int i = 0; i < n; i++) {
      idx[3 * i] = base;
      idx[3 * i + 1] = base + 1 + (uint32_t)i;
      idx[3 * i + 2] = base + 2 + (uint32_t)i;
    }
    return;
  }
  r2d_vertex *v = r2d_alloc(2 * (n + 1), 6 * n, &idx, &base, R2D_NO_ARRAY, xf_y(ctr.x, ctr.y), 0);
  float cx = c0, sx = s0;
  for (int i = 0; i <= n; i++) {
    put(&v[2 * i], ctr.x + cx * r1, ctr.y + sx * r1, 0, 0, c, 0, mode, layer);
    put(&v[2 * i + 1], ctr.x + cx * r0, ctr.y + sx * r0, 0, 0, c, 0, mode, layer);
    float t = cx * dc - sx * ds;
    sx = sx * dc + cx * ds, cx = t;
  }
  for (int i = 0; i < n; i++) {
    uint32_t a = base + 2u * (uint32_t)i, b = a + 2;
    uint32_t q[6] = { a, b, b + 1, a, b + 1, a + 1 };
    memcpy(idx + 6 * i, q, sizeof q);
  }
}

void cw_r2d_fan(cw_vec2 ctr, const cw_vec2 *pts, int n, bool closed, cw_color col) {
  if (n < 2 || (n < 3 && closed)) return;
  int tris = closed ? n : n - 1;
  float maxy = ctr.y;
  for (int i = 0; i < n; i++) maxy = CW_MAX(maxy, pts[i].y);
  uint8_t c[4];
  r2d_pack_color(c, col);
  uint32_t *idx, base;
  r2d_vertex *v = r2d_alloc(n + 1, 3 * tris, &idx, &base, R2D_NO_ARRAY, xf_y(ctr.x, maxy), 0);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  put(&v[0], ctr.x, ctr.y, 0, 0, c, 0, mode, layer);
  for (int i = 0; i < n; i++) put(&v[1 + i], pts[i].x, pts[i].y, 0, 0, c, 0, mode, layer);
  for (int i = 0; i < tris; i++) {
    idx[3 * i] = base;
    idx[3 * i + 1] = base + 1 + (uint32_t)i;
    idx[3 * i + 2] = base + 1 + (uint32_t)((i + 1) % n);
  }
}

/* ── triangulation (ear clipping) ─────────────────────────────────────── */

static float area2(const cw_vec2 *p, int n) {
  float a = 0;
  for (int i = 0, j = n - 1; i < n; j = i++) a += p[j].x * p[i].y - p[i].x * p[j].y;
  return a;
}

static bool in_tri(cw_vec2 p, cw_vec2 a, cw_vec2 b, cw_vec2 c) {
  float d1 = cw_v2_cross(cw_v2_sub(b, a), cw_v2_sub(p, a));
  float d2 = cw_v2_cross(cw_v2_sub(c, b), cw_v2_sub(p, b));
  float d3 = cw_v2_cross(cw_v2_sub(a, c), cw_v2_sub(p, c));
  return d1 >= 0 && d2 >= 0 && d3 >= 0;
}

int cw_triangulate(const cw_vec2 *p, int n, uint32_t *out) {
  if (n < 3) return 0;
  int *v = cw_frame_alloc(sizeof(int) * (size_t)n);
  bool ccw = area2(p, n) > 0; /* make the walk positive-area */
  for (int i = 0; i < n; i++) v[i] = ccw ? i : n - 1 - i;
  int m = n, k = 0, guard = 2 * n;
  while (m > 3 && guard-- > 0) {
    bool clipped = false;
    for (int i = 0; i < m; i++) {
      int ia = v[(i + m - 1) % m], ib = v[i], ic = v[(i + 1) % m];
      cw_vec2 a = p[ia], b = p[ib], c = p[ic];
      if (cw_v2_cross(cw_v2_sub(b, a), cw_v2_sub(c, b)) <= 0) continue; /* reflex */
      bool ear = true;
      for (int j = 0; j < m && ear; j++) {
        int q = v[j];
        if (q == ia || q == ib || q == ic) continue;
        if (in_tri(p[q], a, b, c)) ear = false;
      }
      if (!ear) continue;
      out[k++] = (uint32_t)ia;
      out[k++] = (uint32_t)ib;
      out[k++] = (uint32_t)ic;
      memmove(v + i, v + i + 1, sizeof(int) * (size_t)(m - i - 1));
      m--;
      clipped = true;
      guard = 2 * m;
      break;
    }
    if (!clipped) break; /* degenerate or self-intersecting: fan what is left */
  }
  for (int i = 1; i + 1 < m; i++) {
    out[k++] = (uint32_t)v[0];
    out[k++] = (uint32_t)v[i];
    out[k++] = (uint32_t)v[i + 1];
  }
  return k;
}

void cw_r2d_polygon(const cw_vec2 *pts, int n, cw_color col) {
  if (n < 3) return;
  uint32_t *tri = cw_frame_alloc(sizeof(uint32_t) * 3 * (size_t)n);
  int ni = cw_triangulate(pts, n, tri);
  if (!ni) return;
  float maxy = pts[0].y;
  for (int i = 1; i < n; i++) maxy = CW_MAX(maxy, pts[i].y);
  uint8_t c[4];
  r2d_pack_color(c, col);
  uint32_t *idx, base;
  r2d_vertex *v = r2d_alloc(n, ni, &idx, &base, R2D_NO_ARRAY, xf_y(pts[0].x, maxy), 0);
  uint8_t mode = r2d_mode(true), layer = (uint8_t)R.cur_layer;
  for (int i = 0; i < n; i++) put(&v[i], pts[i].x, pts[i].y, 0, 0, c, 0, mode, layer);
  for (int i = 0; i < ni; i++) idx[i] = base + tri[i];
}

void cw_r2d_polygon_lines(const cw_vec2 *pts, int n, float width, cw_color c) { cw_r2d_polyline(pts, n, width, true, c); }

/* ── arbitrary geometry ────────────────────────────────────────────────── */

void cw_r2d_triangles(cw_texture tex, const cw_vertex2d *src, int nv, const uint32_t *ind, int ni) {
  if (nv <= 0 || ni <= 0) return;
  const r2d_tex *t = r2d_tex_get(tex);
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  if (t) r2d_tex_uv(t, (cw_rect){ 0, 0, (float)t->w, (float)t->h }, &u0, &v0, &u1, &v1);
  if (t && t->has_normal && R.layers[R.cur_layer].lit) R.normals_used = true;
  float maxy = src[0].pos.y;
  for (int i = 1; i < nv; i++) maxy = CW_MAX(maxy, src[i].pos.y);
  uint32_t *idx, base;
  r2d_vertex *v = r2d_alloc(nv, ni, &idx, &base, t ? t->array : R2D_NO_ARRAY, xf_y(src[0].pos.x, maxy), 0);
  uint8_t mode = r2d_mode(!t), layer = (uint8_t)R.cur_layer, page = t ? (uint8_t)t->page : 0;
  for (int i = 0; i < nv; i++) {
    uint8_t c[4];
    r2d_pack_color(c, src[i].color);
    put(&v[i], src[i].pos.x, src[i].pos.y, u0 + src[i].uv.x * (u1 - u0), v0 + src[i].uv.y * (v1 - v0), c, page, mode,
        layer);
  }
  for (int i = 0; i < ni; i++) {
    CW_ASSERT((int)ind[i] < nv, "cw_r2d_triangles: index past the vertex count");
    idx[i] = base + ind[i];
  }
}

void cw_r2d_quad(cw_texture tex, const cw_vec2 pos[4], const cw_vec2 uv[4], cw_color c) {
  cw_vertex2d v[4];
  cw_color col = r2d_default_white(c);
  for (int i = 0; i < 4; i++) v[i] = (cw_vertex2d){ pos[i], uv[i], col };
  cw_r2d_triangles(tex, v, 4, QUAD_IDX, 6);
}
