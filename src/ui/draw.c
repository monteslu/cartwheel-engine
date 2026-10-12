/* UI drawing: anti-aliased styles (rounded rects, borders, shadows) as
 * vertex-coloured triangles, text, icons, clipping, and the per-frame
 * traversal that draws a tree's controls, popups and tooltip. */
#include "ui_internal.h"
#include "cartwheel/l10n.h"

static struct {
  cw_vec2 origin;   /* global UI position of the control being drawn */
  cw_color mod;
  float aa;         /* one canvas pixel in UI units */
  float sc;         /* UI units -> canvas pixels */
  cw_rect clip[40]; /* global UI rects */
  int clip_depth;
  bool active;
  bool rtl;         /* the control being drawn lays out right to left */
} D;

bool ui_draw_rtl(void) { return D.rtl; }
static int text_dir(void) { return D.rtl ? CW_TEXT_DIR_RTL : CW_TEXT_DIR_AUTO; }

cw_vec2 ui_draw_origin(void) { return D.origin; }
void ui_draw_set_origin(cw_vec2 o) { D.origin = o; }

static cw_color modc(cw_color c) { return (cw_color){ c.r * D.mod.r, c.g * D.mod.g, c.b * D.mod.b, c.a * D.mod.a }; }

/* ── geometry ───────────────────────────────────────────────────────── */

#define MAX_SEG 12
static int segs_for(float r) {
  if (r <= 0) return 1;
  int n = (int)(sqrtf(r * D.sc) * 1.6f) + 2;
  return n > MAX_SEG ? MAX_SEG : n;
}

/* Rounded-rect outline inset by ins[4] (l t r b; negative = outward), radii
 * shrinking with the inset. segs fixed per corner so rings line up. */
static int rr_path(cw_rect r, const float rad[4], const float ins[4], const int segs[4], cw_vec2 *out) {
  float x0 = r.x + ins[0], y0 = r.y + ins[1], x1 = r.x + r.w - ins[2], y1 = r.y + r.h - ins[3];
  float rr[4];
  rr[0] = CW_MAX(rad[0] - CW_MAX(ins[0], ins[1]), 0);
  rr[1] = CW_MAX(rad[1] - CW_MAX(ins[2], ins[1]), 0);
  rr[2] = CW_MAX(rad[2] - CW_MAX(ins[2], ins[3]), 0);
  rr[3] = CW_MAX(rad[3] - CW_MAX(ins[0], ins[3]), 0);
  float cx[4] = { x0 + rr[0], x1 - rr[1], x1 - rr[2], x0 + rr[3] };
  float cy[4] = { y0 + rr[0], y0 + rr[1], y1 - rr[2], y1 - rr[3] };
  int n = 0;
  for (int c = 0; c < 4; c++) {
    float a0 = CW_PI * (1.0f + 0.5f * (float)c); /* tl 180, tr 270, br 0(360), bl 90(450) */
    int k = segs[c];
    for (int i = 0; i < k; i++) {
      float a = k == 1 ? a0 + CW_PI * 0.25f : a0 + (CW_PI * 0.5f) * (float)i / (float)(k - 1);
      out[n++] = (cw_vec2){ cx[c] + cosf(a) * rr[c], cy[c] + sinf(a) * rr[c] };
    }
  }
  return n;
}

typedef struct {
  cw_vertex2d v[4 * MAX_SEG * 8 + 8];
  uint32_t idx[4 * MAX_SEG * 6 * 6 + 64];
  int nv, ni;
} geo;

static int add_ring(geo *g, const cw_vec2 *p, int n, cw_color c) {
  int base = g->nv;
  for (int i = 0; i < n; i++) g->v[g->nv++] = (cw_vertex2d){ .pos = p[i], .color = c };
  return base;
}
static void strip(geo *g, int a, int b, int n) { /* quads between ring a and ring b */
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    uint32_t q[6] = { (uint32_t)(a + i), (uint32_t)(a + j), (uint32_t)(b + j), (uint32_t)(a + i), (uint32_t)(b + j), (uint32_t)(b + i) };
    memcpy(g->idx + g->ni, q, sizeof q);
    g->ni += 6;
  }
}
static void fan(geo *g, int ring, int n) {
  for (int i = 1; i + 1 < n; i++) {
    g->idx[g->ni++] = (uint32_t)ring;
    g->idx[g->ni++] = (uint32_t)(ring + i);
    g->idx[g->ni++] = (uint32_t)(ring + i + 1);
  }
}
static void flush(geo *g) {
  if (g->ni) cw_r2d_triangles((cw_texture){ 0 }, g->v, g->nv, g->idx, g->ni);
  g->nv = g->ni = 0;
}

static void to_global(cw_rect *r) {
  r->x += D.origin.x;
  r->y += D.origin.y;
}

static void clamp_radii(cw_rect r, const float in[4], float out[4]) {
  float m = CW_MIN(r.w, r.h) * 0.5f;
  for (int i = 0; i < 4; i++) out[i] = CW_CLAMP(in[i], 0, m);
}

/* a filled shape with an anti-aliased edge at inset `ins` */
static void fill_shape(geo *g, cw_rect r, const float rad[4], const float ins4[4], const int segs[4], cw_color c) {
  cw_vec2 a[4 * MAX_SEG], b[4 * MAX_SEG];
  float h = D.aa * 0.5f;
  float in[4] = { ins4[0] + h, ins4[1] + h, ins4[2] + h, ins4[3] + h };
  float out[4] = { ins4[0] - h, ins4[1] - h, ins4[2] - h, ins4[3] - h };
  int n = rr_path(r, rad, in, segs, a);
  rr_path(r, rad, out, segs, b);
  int ra = add_ring(g, a, n, c);
  int rb = add_ring(g, b, n, (cw_color){ c.r, c.g, c.b, 0 });
  fan(g, ra, n);
  strip(g, ra, rb, n);
}

void cw_ui_draw_style(const cw_ui_style *s, cw_rect r) {
  if (!s->draw || !D.active) return;
  to_global(&r);
  float rad[4];
  clamp_radii(r, s->radius, rad);
  geo *g = cw_frame_alloc(sizeof(geo));
  g->nv = g->ni = 0;
  float zero[4] = { 0 };
  /* shadow: a soft ring out to shadow_size */
  if (s->shadow.a > 0 && s->shadow_size > 0) {
    cw_rect sr = { r.x + s->shadow_offset.x, r.y + s->shadow_offset.y, r.w, r.h };
    float sz = s->shadow_size;
    int segs[4];
    for (int i = 0; i < 4; i++) segs[i] = segs_for(rad[i] + sz);
    cw_vec2 a[4 * MAX_SEG], b[4 * MAX_SEG];
    float o4[4] = { -sz, -sz, -sz, -sz };
    float i4[4] = { sz * 0.25f, sz * 0.25f, sz * 0.25f, sz * 0.25f };
    int n = rr_path(sr, rad, i4, segs, a);
    rr_path(sr, rad, o4, segs, b);
    cw_color sc = modc(s->shadow);
    int ra = add_ring(g, a, n, sc);
    int rb = add_ring(g, b, n, (cw_color){ sc.r, sc.g, sc.b, 0 });
    fan(g, ra, n);
    strip(g, ra, rb, n);
    flush(g);
  }
  int segs[4];
  for (int i = 0; i < 4; i++) segs[i] = segs_for(rad[i]);
  bool has_border = s->border_color.a > 0 && (s->border[0] > 0 || s->border[1] > 0 || s->border[2] > 0 || s->border[3] > 0);
  if (s->draw_center && s->bg.a > 0) {
    float fi[4];
    for (int i = 0; i < 4; i++) fi[i] = has_border ? s->border[i] * 0.5f : 0;
    fill_shape(g, r, rad, has_border ? fi : zero, segs, modc(s->bg));
  }
  if (has_border) {
    cw_color bc = modc(s->border_color);
    cw_color b0 = { bc.r, bc.g, bc.b, 0 };
    float h = D.aa * 0.5f;
    cw_vec2 p0[4 * MAX_SEG], p1[4 * MAX_SEG], p2[4 * MAX_SEG], p3[4 * MAX_SEG];
    float i0[4] = { -h, -h, -h, -h }, i1[4] = { h, h, h, h }, i2[4], i3[4];
    for (int i = 0; i < 4; i++) {
      if (s->border[i] <= 0) { /* no border on this side: no anti-aliasing fringe either */
        i0[i] = i1[i] = i2[i] = i3[i] = h;
        continue;
      }
      i2[i] = CW_MAX(s->border[i] - h, h);
      i3[i] = s->border[i] + h;
    }
    int n = rr_path(r, rad, i0, segs, p0);
    rr_path(r, rad, i1, segs, p1);
    rr_path(r, rad, i2, segs, p2);
    rr_path(r, rad, i3, segs, p3);
    int a0 = add_ring(g, p0, n, b0), a1 = add_ring(g, p1, n, bc), a2 = add_ring(g, p2, n, bc), a3 = add_ring(g, p3, n, b0);
    strip(g, a0, a1, n);
    strip(g, a1, a2, n);
    strip(g, a2, a3, n);
  }
  flush(g);
}

void cw_ui_draw_round_rect(cw_rect r, float radius, cw_color c) {
  cw_ui_style s = cw_ui_style_flat(c, radius, 0, 0);
  cw_ui_draw_style(&s, r);
}

void cw_ui_draw_rect(cw_rect r, cw_color c) {
  if (!D.active || c.a <= 0) return;
  to_global(&r);
  cw_r2d_rect(r, modc(c));
}
void cw_ui_draw_rect_lines(cw_rect r, float w, cw_color c) {
  if (!D.active || c.a <= 0) return;
  to_global(&r);
  cw_r2d_rect_lines(r, w, modc(c));
}
void cw_ui_draw_line(cw_vec2 a, cw_vec2 b, float w, cw_color c) {
  if (!D.active || c.a <= 0) return;
  cw_r2d_line(cw_v2_add(a, D.origin), cw_v2_add(b, D.origin), w, modc(c));
}
void cw_ui_draw_circle(cw_vec2 ctr, float radius, cw_color c) {
  if (!D.active || c.a <= 0) return;
  cw_ui_draw_round_rect((cw_rect){ ctr.x - radius, ctr.y - radius, radius * 2, radius * 2 }, radius, c);
}
void cw_ui_draw_polygon(const cw_vec2 *pts, int n, cw_color c) {
  if (!D.active || c.a <= 0 || n < 3) return;
  cw_vec2 *p = cw_frame_alloc(sizeof(cw_vec2) * (size_t)n);
  for (int i = 0; i < n; i++) p[i] = cw_v2_add(pts[i], D.origin);
  cw_r2d_polygon(p, n, modc(c));
}
void cw_ui_draw_triangles(const cw_vec2 *pos, const cw_color *col, int nv, const uint32_t *idx, int ni) {
  if (!D.active) return;
  cw_vertex2d *v = cw_frame_alloc(sizeof(cw_vertex2d) * (size_t)nv);
  for (int i = 0; i < nv; i++) v[i] = (cw_vertex2d){ .pos = cw_v2_add(pos[i], D.origin), .color = modc(col[i]) };
  cw_r2d_triangles((cw_texture){ 0 }, v, nv, idx, ni);
}
void cw_ui_draw_texture(cw_texture t, cw_rect dst, cw_rect src, cw_color tint) {
  if (!D.active) return;
  to_global(&dst);
  cw_color c = modc(tint.a == 0 && tint.r == 0 && tint.g == 0 && tint.b == 0 ? CW_WHITE : tint);
  if (c.a <= 0) return;
  cw_r2d_sprite(&(cw_sprite){ .texture = t, .src = src, .pos = { dst.x, dst.y }, .size = { dst.w, dst.h }, .color = c });
}
void cw_ui_draw_nine_patch(cw_texture t, cw_rect dst, cw_rect src, float l, float tp, float r, float b, bool center,
                           cw_color tint) {
  if (!D.active) return;
  to_global(&dst);
  cw_color c = modc(tint.a == 0 && tint.r == 0 && tint.g == 0 && tint.b == 0 ? CW_WHITE : tint);
  cw_r2d_nine_slice(&(cw_nine_slice){ .texture = t, .src = src, .left = l, .top = tp, .right = r, .bottom = b, .dst = dst,
                                      .color = c, .hollow = !center });
}

/* ── text ───────────────────────────────────────────────────────────── */

float cw_ui_text_width(cw_font f, float size, const char *s) {
  if (!s || !*s) return 0;
  return cw_font_measure(f, s, &(cw_text_style){ .size = size }).width;
}
float cw_ui_line_height(cw_font f, float size) {
  return cw_font_measure(f, "", &(cw_text_style){ .size = size }).line_height;
}

static float snap(float v) { return D.sc > 0 ? floorf(v * D.sc + 0.5f) / D.sc : v; }

float cw_ui_draw_text(cw_font f, float size, const char *s, cw_vec2 pos, cw_color c) {
  if (!s || !*s) return 0;
  cw_color m = modc(c);
  if (!D.active || m.a <= 0) return cw_ui_text_width(f, size, s);
  cw_vec2 p = { snap(pos.x + D.origin.x), snap(pos.y + D.origin.y) };
  return cw_r2d_text(f, s, p, &(cw_text_style){ .size = size, .color = m, .direction = text_dir() }).width;
}

void cw_ui_draw_text_in(cw_font f, float size, const char *s, cw_rect r, cw_halign al, cw_color c, bool ellipsis) {
  if (!s || !*s) return;
  float w = cw_ui_text_width(f, size, s);
  float lh = cw_ui_line_height(f, size);
  const char *draw = s;
  if (w > r.w && ellipsis) {
    int len = (int)strlen(s);
    int lo = 0, hi = len;
    char *buf = cw_frame_alloc((size_t)len + 8);
    float ew = cw_ui_text_width(f, size, "\xE2\x80\xA6");
    while (lo < hi) { /* longest prefix (codepoint boundary) that fits with the ellipsis */
      int mid = (lo + hi + 1) / 2;
      while (mid > 0 && ((unsigned char)s[mid] & 0xc0) == 0x80) mid--;
      if (mid <= lo) { hi = lo; break; }
      memcpy(buf, s, (size_t)mid);
      buf[mid] = 0;
      if (cw_ui_text_width(f, size, buf) + ew <= r.w) lo = mid;
      else hi = mid - 1;
    }
    while (lo > 0 && ((unsigned char)s[lo] & 0xc0) == 0x80) lo--;
    while (lo > 0 && s[lo - 1] == ' ') lo--;
    memcpy(buf, s, (size_t)lo);
    memcpy(buf + lo, "\xE2\x80\xA6", 4);
    draw = buf;
    w = cw_ui_text_width(f, size, buf);
  }
  if (D.rtl) al = al == CW_HALIGN_LEFT ? CW_HALIGN_RIGHT : al == CW_HALIGN_RIGHT ? CW_HALIGN_LEFT : al;
  float x = r.x;
  if (al == CW_HALIGN_CENTER) x = r.x + (r.w - w) * 0.5f;
  else if (al == CW_HALIGN_RIGHT) x = r.x + r.w - w;
  cw_ui_draw_text(f, size, draw, (cw_vec2){ x, r.y + (r.h - lh) * 0.5f }, c);
}

void cw_ui_draw_icon(cw_ui_icon icon, cw_rect r, cw_color c) {
  float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, s = CW_MIN(r.w, r.h) * 0.5f;
  float lw = CW_MAX(1.5f, s * 0.22f);
  switch (icon) {
  case CW_ICON_ARROW_DOWN: case CW_ICON_ARROW_UP: case CW_ICON_ARROW_LEFT: case CW_ICON_ARROW_RIGHT: {
    /* a chevron */
    cw_vec2 p[3];
    float a = s * 0.5f, b = s * 0.28f;
    if (icon == CW_ICON_ARROW_DOWN) p[0] = (cw_vec2){ cx - a, cy - b }, p[1] = (cw_vec2){ cx, cy + b }, p[2] = (cw_vec2){ cx + a, cy - b };
    else if (icon == CW_ICON_ARROW_UP) p[0] = (cw_vec2){ cx - a, cy + b }, p[1] = (cw_vec2){ cx, cy - b }, p[2] = (cw_vec2){ cx + a, cy + b };
    else if (icon == CW_ICON_ARROW_RIGHT) p[0] = (cw_vec2){ cx - b, cy - a }, p[1] = (cw_vec2){ cx + b, cy }, p[2] = (cw_vec2){ cx - b, cy + a };
    else p[0] = (cw_vec2){ cx + b, cy - a }, p[1] = (cw_vec2){ cx - b, cy }, p[2] = (cw_vec2){ cx + b, cy + a };
    if (!D.active) return;
    for (int i = 0; i < 3; i++) p[i] = cw_v2_add(p[i], D.origin);
    cw_r2d_polyline(p, 3, lw, false, modc(c));
    break;
  }
  case CW_ICON_CHECK: {
    cw_vec2 p[3] = { { cx - s * 0.55f, cy + s * 0.02f }, { cx - s * 0.15f, cy + s * 0.42f }, { cx + s * 0.6f, cy - s * 0.4f } };
    if (!D.active) return;
    for (int i = 0; i < 3; i++) p[i] = cw_v2_add(p[i], D.origin);
    cw_r2d_polyline(p, 3, lw * 1.1f, false, modc(c));
    break;
  }
  case CW_ICON_CROSS:
    cw_ui_draw_line((cw_vec2){ cx - s * 0.5f, cy - s * 0.5f }, (cw_vec2){ cx + s * 0.5f, cy + s * 0.5f }, lw, c);
    cw_ui_draw_line((cw_vec2){ cx - s * 0.5f, cy + s * 0.5f }, (cw_vec2){ cx + s * 0.5f, cy - s * 0.5f }, lw, c);
    break;
  case CW_ICON_DOT: cw_ui_draw_circle((cw_vec2){ cx, cy }, s * 0.4f, c); break;
  }
}

/* ── clipping ───────────────────────────────────────────────────────── */

static cw_rect isect(cw_rect a, cw_rect b) {
  float x0 = CW_MAX(a.x, b.x), y0 = CW_MAX(a.y, b.y);
  float x1 = CW_MIN(a.x + a.w, b.x + b.w), y1 = CW_MIN(a.y + a.h, b.y + b.h);
  return (cw_rect){ x0, y0, CW_MAX(x1 - x0, 0), CW_MAX(y1 - y0, 0) };
}

void cw_ui_push_clip(cw_rect r) {
  if (!D.active) return;
  to_global(&r);
  CW_ASSERT(D.clip_depth < 39, "UI clip stack overflow: unbalanced cw_ui_push_clip");
  cw_rect c = isect(D.clip[D.clip_depth], r);
  D.clip[++D.clip_depth] = c;
  cw_r2d_push_clip((cw_rect){ c.x * D.sc, c.y * D.sc, c.w * D.sc, c.h * D.sc });
}
void cw_ui_pop_clip(void) {
  if (!D.active || D.clip_depth <= 0) return;
  D.clip_depth--;
  cw_r2d_pop_clip();
}

/* ── traversal ──────────────────────────────────────────────────────── */

void ui_draw_focus_style(cw_node n) {
  cw_control *c = CTL(n);
  cw_ui_style s = T_STYLE(n, "focus");
  cw_ui_draw_style(&s, (cw_rect){ 0, 0, c->rect.w, c->rect.h });
}

static cw_color mul(cw_color a, cw_color b) { return (cw_color){ a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a }; }

static void draw_ctl(ui_tree_state *s, cw_node n, cw_vec2 porigin, cw_color pmod, bool prtl) {
  cw_control *c = CTL(n);
  if (!c || !c->visible) return;
  bool rtl = c->layout_direction == CW_LAYOUT_RTL ? true : c->layout_direction == CW_LAYOUT_LTR ? false
           : c->layout_direction == CW_LAYOUT_LOCALE ? cw_l10n_is_rtl() : prtl;
  cw_vec2 origin = { porigin.x + c->rect.x, porigin.y + c->rect.y };
  cw_color mod = mul(pmod, c->modulate);
  if (mod.a <= 0) return;
  cw_rect g = { origin.x, origin.y, c->rect.w, c->rect.h };
  int kids = cw_node_child_count(n);
  cw_rect vis = isect(D.clip[D.clip_depth], g);
  if ((vis.w <= 0 || vis.h <= 0) && (c->clip_contents || kids == 0)) return;
  UI.stats.drawn++;
  D.rtl = rtl;
  D.origin = origin;
  D.mod = mul(mod, c->self_modulate);
  if (c->_vt->draw) c->_vt->draw(n, cw_node_data_t(n, c->_vt->draw_t));
  D.origin = origin;
  if ((c->_flags & CF_FOCUSED) && s->focus_visible && !c->_vt->wants_text) ui_draw_focus_style(n);
  if (kids) {
    if (c->clip_contents) cw_ui_push_clip((cw_rect){ 0, 0, c->rect.w, c->rect.h });
    cw_vec2 off = ui_child_offset(n);
    cw_vec2 co = { origin.x + off.x, origin.y + off.y };
    for (int i = 0; i < kids; i++) {
      cw_node ch = cw_node_child(n, i);
      cw_control *cc = CTL(ch);
      if (cc && !(cc->_flags & CF_TOP_LEVEL)) draw_ctl(s, ch, co, mod, rtl);
    }
    D.origin = origin;
    D.rtl = rtl;
    if (c->clip_contents) cw_ui_pop_clip();
  }
  if (c->_vt->draw_over) {
    D.origin = origin;
    D.mod = mul(mod, c->self_modulate);
    c->_vt->draw_over(n, cw_node_data_t(n, c->_vt->draw_over_t));
  }
}

static void draw_tooltip(void) {
  if (!UI.tip_shown || !cw_node_valid(UI.tip_node)) return;
  cw_control *c = CTL(UI.tip_node);
  if (!c || !c->tooltip_text || !*c->tooltip_text) return;
  cw_node n = UI.tip_node;
  const char *tip = ui_tr(n, c->tooltip_text);
  cw_ui_style st = cw_control_theme_style(n, "TooltipPanel", "panel");
  cw_font f = cw_control_theme_font(n, "TooltipLabel", "font");
  float fs = cw_control_theme_font_size(n, "TooltipLabel", "font_size");
  cw_color fc = cw_control_theme_color(n, "TooltipLabel", "font_color");
  cw_text_metrics m = cw_font_measure(f, tip, &(cw_text_style){ .size = fs });
  float w = m.width + SM_W(&st), h = m.height + SM_H(&st);
  cw_vec2 cs = cw_ui_canvas_size();
  float x = UI.tip_pos.x + 4, y = UI.tip_pos.y + 20;
  if (x + w > cs.x) x = cs.x - w;
  if (y + h > cs.y) y = UI.tip_pos.y - h - 4;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  D.origin = (cw_vec2){ 0, 0 };
  D.mod = CW_WHITE;
  cw_ui_draw_style(&st, (cw_rect){ x, y, w, h });
  cw_ui_draw_text(f, fs, tip, (cw_vec2){ x + st.margin[0], y + st.margin[1] }, fc);
}

void ui_draw_tree(ui_tree_state *s) {
  D.sc = ui_canvas_scale();
  D.aa = 1.0f / (D.sc * (UI.draw_virtual ? 1.0f : (UI.dpi > 0 ? UI.dpi : 1.0f))); /* one drawable pixel */
  D.active = true;
  D.clip_depth = 0;
  cw_vec2 cs = cw_ui_canvas_size();
  D.clip[0] = (cw_rect){ -1e6f, -1e6f, 2e6f, 2e6f };
  (void)cs;
  int layer = cw_ui_layer();
  cw_r2d_layer_set(layer, &(cw_r2d_layer_desc){ .has_camera = true, .camera = { .zoom = D.sc, .anchor_top_left = true } });
  cw_r2d_use_layer(layer);
  cw_r2d_set_z(0);
  cw_r2d_set_transform(NULL);
  cw_r2d_set_blend(CW_BLEND_ALPHA);
  UI.stats.drawn = 0;
  ui_roots_refresh(s);
  for (int i = 0; i < s->roots.count; i++) draw_ctl(s, s->roots.data[i], (cw_vec2){ 0, 0 }, CW_WHITE, cw_l10n_is_rtl());
  for (int i = 0; i < s->popups.count; i++) {
    ui_popup *p = &s->popups.data[i];
    cw_control *c = CTL(p->node);
    if (!c || !c->visible) continue;
    if (p->modal) {
      D.origin = (cw_vec2){ 0, 0 };
      D.mod = CW_WHITE;
      cw_ui_draw_rect((cw_rect){ 0, 0, cs.x, cs.y }, cw_control_theme_color(p->node, "AcceptDialog", "dim_color"));
    }
    draw_ctl(s, p->node, (cw_vec2){ 0, 0 }, CW_WHITE, cw_l10n_is_rtl());
  }
  D.origin = (cw_vec2){ 0, 0 };
  D.mod = CW_WHITE;
  D.rtl = cw_l10n_is_rtl();
  ui_dnd_draw(s);
  draw_tooltip();
  D.active = false;
  D.rtl = false;
  cw_r2d_use_layer(0);
}

/* ── entry points ───────────────────────────────────────────────────── */

void cw_ui_draw(cw_tree *t) {
  if (!t || !UI.inited) return;
  UI.draw_virtual = true;
  ui_tree_state *s = ui_state(t);
  ui_layout_tree(s);
  const cw_r2d_stats *st = cw_r2d_get_stats();
  uint32_t c0 = st->commands;
  ui_draw_tree(s);
  UI.stats.commands = st->commands - c0;
}

void cw_ui_render(cw_tree *t) {
  if (!t || !UI.inited) return;
  UI.draw_virtual = false;
  ui_tree_state *s = ui_state(t);
  ui_layout_tree(s);
  ui_roots_refresh(s);
  if (!s->roots.count && !s->popups.count) return;
  const cw_r2d_stats *st = cw_r2d_get_stats();
  /* the r2d layer table outlives passes: the UI's zoom camera on its layer
   * is put back once its own pass ends, so a later pass is not scaled by it */
  cw_r2d_layer_desc saved = *cw_r2d_layer_get(cw_ui_layer());
  cw_r2d_begin(&(cw_r2d_pass_desc){ .clear = false });
  uint32_t d0 = st->draw_calls, c0 = st->commands;
  ui_draw_tree(s);
  uint32_t c1 = st->commands;
  cw_r2d_end();
  cw_r2d_layer_set(cw_ui_layer(), &saved);
  UI.stats.draw_calls = st->draw_calls - d0;
  UI.stats.commands = c1 - c0;
}

/* full-style text at a GLOBAL position (wrap, align, size), modulated */
void ui_draw_text_styled(cw_font f, const char *s, cw_vec2 global, cw_text_style *st) {
  if (!D.active || !s || !*s) return;
  cw_text_style t = *st;
  t.color = modc(t.color);
  if (!t.direction) t.direction = text_dir();
  if (t.color.a <= 0) return;
  cw_r2d_text(f, s, (cw_vec2){ snap(global.x), snap(global.y) }, &t);
}
