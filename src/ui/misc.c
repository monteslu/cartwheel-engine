/* ColorPicker (HSV), TextureRect, NinePatchRect. */
#include "ui_internal.h"

/* ── ColorPicker ────────────────────────────────────────────────────── */

typedef struct {
  cw_color color;
  float h, s, v;
  int drag; /* 0 none 1 sv 2 hue 3 alpha */
} cp_data;
static const cw_node_type *cp_t;

static cw_color hsv2rgb(float h, float s, float v, float a) {
  h = h - floorf(h);
  float hh = h * 6;
  int i = (int)hh;
  float f = hh - (float)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  switch (i % 6) {
  case 0: return (cw_color){ v, t, p, a };
  case 1: return (cw_color){ q, v, p, a };
  case 2: return (cw_color){ p, v, t, a };
  case 3: return (cw_color){ p, q, v, a };
  case 4: return (cw_color){ t, p, v, a };
  default: return (cw_color){ v, p, q, a };
  }
}
static void rgb2hsv(cw_color c, float *h, float *s, float *v) {
  float mx = CW_MAX(c.r, CW_MAX(c.g, c.b)), mn = CW_MIN(c.r, CW_MIN(c.g, c.b)), d = mx - mn;
  *v = mx;
  *s = mx > 0 ? d / mx : 0;
  if (d <= 0) return; /* keep the hue for greys */
  float hh;
  if (mx == c.r) hh = (c.g - c.b) / d + (c.g < c.b ? 6 : 0);
  else if (mx == c.g) hh = (c.b - c.r) / d + 2;
  else hh = (c.r - c.g) / d + 4;
  *h = hh / 6;
}

typedef struct { cw_rect sv, hue, alpha, swatch, hex; } cp_geo;
static cp_geo cp_layout(cw_node n) {
  cw_control *c = CTL(n);
  float sv = T_CONST(n, "sv_size"), sw = T_CONST(n, "strip_width"), sep = T_CONST(n, "separation");
  float lh = cw_ui_line_height(T_FONT(n), T_FSIZE(n));
  float side = CW_MIN(c->rect.w - 2 * (sw + sep), c->rect.h - lh - sep - 4);
  side = CW_MAX(side, sv * 0.5f);
  cp_geo g;
  g.sv = (cw_rect){ 0, 0, side, side };
  g.hue = (cw_rect){ side + sep, 0, sw, side };
  g.alpha = (cw_rect){ side + sep * 2 + sw, 0, sw, side };
  g.swatch = (cw_rect){ 0, side + sep, lh * 2, lh + 4 };
  g.hex = (cw_rect){ lh * 2 + sep, side + sep, side - lh * 2 - sep, lh + 4 };
  return g;
}

static cw_vec2 cp_min(cw_node n, void *d) {
  (void)d;
  float sv = T_CONST(n, "sv_size"), sw = T_CONST(n, "strip_width"), sep = T_CONST(n, "separation");
  float lh = cw_ui_line_height(T_FONT(n), T_FSIZE(n));
  return (cw_vec2){ sv + 2 * (sw + sep), sv + sep + lh + 4 };
}

static void quad_grad(cw_rect r, cw_color tl, cw_color tr, cw_color br, cw_color bl) {
  cw_vec2 p[4] = { { r.x, r.y }, { r.x + r.w, r.y }, { r.x + r.w, r.y + r.h }, { r.x, r.y + r.h } };
  cw_color c[4] = { tl, tr, br, bl };
  uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
  cw_ui_draw_triangles(p, c, 4, idx, 6);
}

static void checker(cw_rect r) {
  float s = 4;
  cw_ui_draw_rect(r, cw_hex(0xd0d0d0ff));
  for (float y = 0; y < r.h; y += s)
    for (float x = ((int)(y / s) & 1) ? s : 0; x < r.w; x += 2 * s)
      cw_ui_draw_rect((cw_rect){ r.x + x, r.y + y, CW_MIN(s, r.w - x), CW_MIN(s, r.h - y) }, cw_hex(0x9a9a9aff));
}

static void cp_draw(cw_node n, void *d) {
  cp_data *p = d;
  cp_geo g = cp_layout(n);
  cw_color hue = hsv2rgb(p->h, 1, 1, 1);
  quad_grad(g.sv, CW_WHITE, hue, hue, CW_WHITE);
  quad_grad(g.sv, (cw_color){ 0, 0, 0, 0 }, (cw_color){ 0, 0, 0, 0 }, CW_BLACK, CW_BLACK);
  for (int i = 0; i < 6; i++) {
    float y0 = g.hue.y + g.hue.h * (float)i / 6, y1 = g.hue.y + g.hue.h * (float)(i + 1) / 6;
    cw_color a = hsv2rgb((float)i / 6, 1, 1, 1), b = hsv2rgb((float)(i + 1) / 6, 1, 1, 1);
    quad_grad((cw_rect){ g.hue.x, y0, g.hue.w, y1 - y0 }, a, a, b, b);
  }
  checker(g.alpha);
  cw_color opaque = { p->color.r, p->color.g, p->color.b, 1 }, clear = { p->color.r, p->color.g, p->color.b, 0 };
  quad_grad(g.alpha, opaque, opaque, clear, clear);
  cw_color cur = T_COLOR(n, "cursor_color");
  cw_vec2 sv = { g.sv.x + p->s * g.sv.w, g.sv.y + (1 - p->v) * g.sv.h };
  cw_ui_draw_circle(sv, 6, CW_BLACK);
  cw_ui_draw_circle(sv, 5, cur);
  cw_ui_draw_circle(sv, 3.5f, hsv2rgb(p->h, p->s, p->v, 1));
  float hy = g.hue.y + p->h * g.hue.h, ay = g.alpha.y + (1 - p->color.a) * g.alpha.h;
  cw_ui_draw_rect((cw_rect){ g.hue.x - 2, hy - 2, g.hue.w + 4, 4 }, CW_BLACK);
  cw_ui_draw_rect((cw_rect){ g.hue.x - 1, hy - 1, g.hue.w + 2, 2 }, cur);
  cw_ui_draw_rect((cw_rect){ g.alpha.x - 2, ay - 2, g.alpha.w + 4, 4 }, CW_BLACK);
  cw_ui_draw_rect((cw_rect){ g.alpha.x - 1, ay - 1, g.alpha.w + 2, 2 }, cur);
  checker(g.swatch);
  cw_ui_draw_rect(g.swatch, p->color);
  cw_ui_style sw = T_STYLE(n, "swatch");
  cw_ui_draw_style(&sw, g.swatch);
  char hex[16];
  snprintf(hex, sizeof hex, "#%02x%02x%02x%02x", (int)lroundf(p->color.r * 255), (int)lroundf(p->color.g * 255),
           (int)lroundf(p->color.b * 255), (int)lroundf(p->color.a * 255));
  cw_ui_draw_text_in(T_FONT(n), T_FSIZE(n), hex, g.hex, CW_HALIGN_LEFT, T_COLOR(n, "font_color"), true);
}

static void cp_apply(cw_node n, cp_data *p) {
  cw_color c = hsv2rgb(p->h, p->s, p->v, p->color.a);
  if (memcmp(&c, &p->color, sizeof c)) {
    p->color = c;
    cw_value a = cw_val_color(c.r, c.g, c.b, c.a);
    cw_emit(n, "color_changed", &a, 1);
  }
}

static void cp_drag(cw_node n, cp_data *p, cw_vec2 pos) {
  cp_geo g = cp_layout(n);
  if (p->drag == 1) {
    p->s = cw_clampf((pos.x - g.sv.x) / g.sv.w, 0, 1);
    p->v = 1 - cw_clampf((pos.y - g.sv.y) / g.sv.h, 0, 1);
  } else if (p->drag == 2) {
    p->h = cw_clampf((pos.y - g.hue.y) / g.hue.h, 0, 0.9999f);
  } else if (p->drag == 3) {
    p->color.a = 1 - cw_clampf((pos.y - g.alpha.y) / g.alpha.h, 0, 1);
    cw_value a = cw_val_color(p->color.r, p->color.g, p->color.b, p->color.a);
    cw_emit(n, "color_changed", &a, 1);
  }
  cp_apply(n, p);
}

static bool cp_input(cw_node n, void *d, const cw_ui_event *e) {
  cp_data *p = d;
  cp_geo g = cp_layout(n);
  switch (e->type) {
  case CW_UI_MOUSE_DOWN:
    if (e->button != 0) return true;
    p->drag = rect_has(g.sv, e->pos) ? 1 : rect_has(g.hue, e->pos) ? 2 : rect_has(g.alpha, e->pos) ? 3 : 0;
    if (p->drag) cp_drag(n, p, e->pos);
    return true;
  case CW_UI_MOUSE_MOVE:
    if (!p->drag) return false;
    cp_drag(n, p, e->pos);
    return true;
  case CW_UI_MOUSE_UP: p->drag = 0; return true;
  case CW_UI_NAV:
    if (e->nav == CW_NAV_LEFT || e->nav == CW_NAV_RIGHT) {
      p->h += e->nav == CW_NAV_RIGHT ? 1.0f / 72 : -1.0f / 72;
      p->h -= floorf(p->h);
      cp_apply(n, p);
      return true;
    }
    return false;
  default: return false;
  }
}

void cw_color_picker_set_color(cw_node n, cw_color c) {
  cp_data *p = cw_node_data_t(n, cp_t);
  if (!p) return;
  p->color = c;
  rgb2hsv(c, &p->h, &p->s, &p->v);
}
cw_color cw_color_picker_color(cw_node n) {
  cp_data *p = cw_node_data_t(n, cp_t);
  return p ? p->color : (cw_color){ 0 };
}
static void cp_init(cw_node n, void *d) {
  cp_data *p = d;
  p->color = CW_WHITE;
  p->v = 1;
  CTL(n)->focus_mode = CW_FOCUS_ALL;
}
static void cp_changed(cw_node n, void *d, const cw_prop *pr) {
  (void)pr;
  cp_data *p = d;
  rgb2hsv(p->color, &p->h, &p->s, &p->v);
  (void)n;
}
static const cw_prop cp_props[] = {
  CW_PROP_FIELD(cp_data, color, CW_T_COLOR, CW_PROP_DEFAULT, "The picked colour."),
  CW_PROP_END
};
static const char *const cp_signals[] = { "color_changed", NULL };
static cw_node_type cp_type = { .name = "ColorPicker", .parent = "Control", .data_size = sizeof(cp_data), .props = cp_props,
                                .signals = cp_signals, .init = cp_init, .prop_changed = cp_changed,
                                .doc = "HSV square, hue and alpha strips, swatch and hex." };

/* ── TextureRect / NinePatchRect ────────────────────────────────────── */

static const char *const ui_filter_names[] = { "LINEAR", "NEAREST", NULL };
cw_texture cw__tex_get(const char *path, int filter); /* the 2D nodes' texture cache (src/scene) */

typedef struct {
  const char *texture;
  int32_t stretch_mode;
  bool expand;
  float region_rect[4]; /* x y w h in texture pixels; w 0 = whole */
  int32_t filter;       /* 0 LINEAR, 1 NEAREST */
  cw_texture tex;
  const char *loaded;
  int32_t loaded_filter;
} tr_data;
static const cw_node_type *tr_t;

/* Path textures come from the 2D nodes' cache (one load per path + filter,
 * shared with Sprite2D and friends); a texture set from code is kept. */
static cw_texture tex_of(tr_data *t) {
  if (t->texture && (t->texture != t->loaded || t->filter != t->loaded_filter)) {
    t->loaded = t->texture;
    t->loaded_filter = t->filter;
    t->tex = *t->texture ? cw__tex_get(t->texture, t->filter) : (cw_texture){ 0 };
  }
  return t->tex;
}

static cw_vec2 tr_min(cw_node n, void *d) {
  (void)n;
  tr_data *t = d;
  if (t->expand) return (cw_vec2){ 0, 0 };
  int w = 0, h = 0;
  cw_texture tx = tex_of(t);
  if (cw_texture_valid(tx)) cw_texture_size(tx, &w, &h);
  if (t->region_rect[2] > 0) w = (int)t->region_rect[2], h = (int)t->region_rect[3];
  return (cw_vec2){ (float)w, (float)h };
}

static void tr_draw(cw_node n, void *d) {
  tr_data *t = d;
  cw_control *c = CTL(n);
  cw_texture tx = tex_of(t);
  if (!cw_texture_valid(tx)) return;
  int iw, ih;
  cw_texture_size(tx, &iw, &ih);
  cw_rect src = { t->region_rect[0], t->region_rect[1], t->region_rect[2], t->region_rect[3] };
  if (src.w > 0) iw = (int)src.w, ih = (int)src.h;
  float W = c->rect.w, H = c->rect.h, w = (float)iw, h = (float)ih;
  cw_rect dst = { 0, 0, W, H };
  switch (t->stretch_mode) {
  case 1: { /* tile */
    cw_ui_push_clip(dst);
    for (float y = 0; y < H; y += h)
      for (float x = 0; x < W; x += w) cw_ui_draw_texture(tx, (cw_rect){ x, y, w, h }, src, CW_WHITE);
    cw_ui_pop_clip();
    return;
  }
  case 2: dst = (cw_rect){ 0, 0, w, h }; break;
  case 3: dst = (cw_rect){ ui_round((W - w) * 0.5f), ui_round((H - h) * 0.5f), w, h }; break;
  case 4: case 5: case 6: {
    float s = t->stretch_mode == 6 ? CW_MAX(W / w, H / h) : CW_MIN(W / w, H / h);
    float dw = w * s, dh = h * s;
    dst = t->stretch_mode == 4 ? (cw_rect){ 0, 0, dw, dh } : (cw_rect){ (W - dw) * 0.5f, (H - dh) * 0.5f, dw, dh };
    if (t->stretch_mode == 6) {
      cw_ui_push_clip((cw_rect){ 0, 0, W, H });
      cw_ui_draw_texture(tx, dst, src, CW_WHITE);
      cw_ui_pop_clip();
      return;
    }
    break;
  }
  default: break;
  }
  cw_ui_draw_texture(tx, dst, src, CW_WHITE);
}

void cw_texture_rect_set_texture(cw_node n, cw_texture tx) {
  tr_data *t = cw_node_data_t(n, tr_t);
  if (!t) return;
  t->tex = tx;
  t->texture = t->loaded = NULL;
  ui_mark_min_dirty(n);
}
static void tr_init(cw_node n, void *d) {
  (void)d;
  CTL(n)->mouse_filter = CW_MOUSE_PASS;
}
static void tr_changed(cw_node n, void *d, const cw_prop *p) {
  (void)d, (void)p;
  ui_mark_min_dirty(n);
}
static const char *const stretch_names[] = { "SCALE", "TILE", "KEEP", "KEEP_CENTERED", "KEEP_ASPECT", "KEEP_ASPECT_CENTERED",
                                             "KEEP_ASPECT_COVERED", NULL };
static const cw_prop tr_props[] = {
  CW_PROP_FIELD(tr_data, texture, CW_T_STRING, CW_PROP_DEFAULT, "Image asset path."),
  CW_PROP_FIELD(tr_data, region_rect, CW_T_VEC4, CW_PROP_DEFAULT, "Part of the image (x y w h, pixels); w 0 = all."),
  { .name = "stretch_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(tr_data, stretch_mode),
    .size = 4, .enum_names = stretch_names, .doc = "How the image fills the rect." },
  CW_PROP_FIELD(tr_data, expand, CW_T_BOOL, CW_PROP_DEFAULT, "Minimum size 0 instead of the image size."),
  { .name = "filter", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(tr_data, filter), .size = 4,
    .enum_names = ui_filter_names, .doc = "Texture filtering of `texture` (NEAREST for pixel art)." },
  CW_PROP_END
};
static cw_node_type tr_type = { .name = "TextureRect", .parent = "Control", .data_size = sizeof(tr_data), .props = tr_props,
                                .init = tr_init, .prop_changed = tr_changed, .doc = "Draws an image." };

typedef struct {
  const char *texture;
  float region_rect[4];
  float patch_margin_left, patch_margin_top, patch_margin_right, patch_margin_bottom;
  bool draw_center;
  int32_t filter;
  cw_texture tex;
  const char *loaded;
  int32_t loaded_filter;
} np_data;
static const cw_node_type *np_t;

static cw_texture np_tex(np_data *p) {
  if (p->texture && (p->texture != p->loaded || p->filter != p->loaded_filter)) {
    p->loaded = p->texture;
    p->loaded_filter = p->filter;
    p->tex = *p->texture ? cw__tex_get(p->texture, p->filter) : (cw_texture){ 0 };
  }
  return p->tex;
}
static cw_vec2 np_min(cw_node n, void *d) {
  (void)n;
  np_data *p = d;
  return (cw_vec2){ p->patch_margin_left + p->patch_margin_right, p->patch_margin_top + p->patch_margin_bottom };
}
static void np_draw(cw_node n, void *d) {
  np_data *p = d;
  cw_control *c = CTL(n);
  cw_texture tx = np_tex(p);
  if (!cw_texture_valid(tx)) return;
  cw_ui_draw_nine_patch(tx, (cw_rect){ 0, 0, c->rect.w, c->rect.h }, (cw_rect){ p->region_rect[0], p->region_rect[1], p->region_rect[2], p->region_rect[3] }, p->patch_margin_left,
                        p->patch_margin_top, p->patch_margin_right, p->patch_margin_bottom, p->draw_center, CW_WHITE);
}
void cw_nine_patch_set_texture(cw_node n, cw_texture tx) {
  np_data *p = cw_node_data_t(n, np_t);
  if (!p) return;
  p->tex = tx;
  p->texture = p->loaded = NULL;
}
static void np_init(cw_node n, void *d) {
  ((np_data *)d)->draw_center = true;
  CTL(n)->mouse_filter = CW_MOUSE_PASS;
}
static const cw_prop np_props[] = {
  CW_PROP_FIELD(np_data, texture, CW_T_STRING, CW_PROP_DEFAULT, "Image asset path."),
  CW_PROP_FIELD(np_data, region_rect, CW_T_VEC4, CW_PROP_DEFAULT, "Part of the image (x y w h, pixels); w 0 = all."),
  CW_PROP_FIELD(np_data, patch_margin_left, CW_T_FLOAT, CW_PROP_DEFAULT, "Left border, texture pixels."),
  CW_PROP_FIELD(np_data, patch_margin_top, CW_T_FLOAT, CW_PROP_DEFAULT, "Top border."),
  CW_PROP_FIELD(np_data, patch_margin_right, CW_T_FLOAT, CW_PROP_DEFAULT, "Right border."),
  CW_PROP_FIELD(np_data, patch_margin_bottom, CW_T_FLOAT, CW_PROP_DEFAULT, "Bottom border."),
  CW_PROP_FIELD(np_data, draw_center, CW_T_BOOL, CW_PROP_DEFAULT, "Fill the middle."),
  { .name = "filter", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(np_data, filter), .size = 4,
    .enum_names = ui_filter_names, .doc = "Texture filtering of `texture` (NEAREST for pixel art)." },
  CW_PROP_END
};
static cw_node_type np_type = { .name = "NinePatchRect", .parent = "Control", .data_size = sizeof(np_data), .props = np_props,
                                .init = np_init, .prop_changed = tr_changed, .doc = "A nine-slice image." };

void ui_register_misc(void) {
  cw_node_type_register(&cp_type);
  cp_t = &cp_type;
  cw_ui_class_register("ColorPicker", &(cw_ui_class){ .min_size = cp_min, .draw = cp_draw, .input = cp_input });
  cw_node_type_register(&tr_type);
  tr_t = &tr_type;
  cw_ui_class_register("TextureRect", &(cw_ui_class){ .min_size = tr_min, .draw = tr_draw });
  cw_node_type_register(&np_type);
  np_t = &np_type;
  cw_ui_class_register("NinePatchRect", &(cw_ui_class){ .min_size = np_min, .draw = np_draw });
}
