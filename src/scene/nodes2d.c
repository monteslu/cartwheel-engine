/* The 2D canvas node types (cartwheel/scene2d.h): sprites, animated sprites,
 * nine-patches, lines, polygons, labels, cameras, canvas and parallax
 * layers, lights, occluders and the canvas settings. TileMap2D lives in
 * tilemap2d.c, the pass itself in canvas.c. */
#include <stdio.h>
#include <stdlib.h>

#include "scene2d_internal.h"

const cw_node_type *cw__node2d_t, *cw__canvas_layer_t, *cw__parallax_t, *cw__camera2d_t, *cw__settings2d_t,
  *cw__light2d_t;

static const char *const filter_names[] = { "LINEAR", "NEAREST", NULL };

/* ── caches ─────────────────────────────────────────────────────────── */

static cw_map tex_cache, font_cache;

cw_texture cw__tex_get(const char *path, int filter) {
  if (!path || !*path) return (cw_texture){ 0 };
  if (!tex_cache.slot_cap && !tex_cache.count) cw_map_init(&tex_cache, CW_MEM_RENDER2D);
  uint64_t key = ((uint64_t)(uintptr_t)path << 1) | (uint64_t)(filter & 1), v;
  if (cw_map_get(&tex_cache, key, &v)) return (cw_texture){ v };
  cw_texture_desc d = { .filter = filter ? CW_FILTER_NEAREST : CW_FILTER_LINEAR };
  cw_texture t = cw_texture_load(path, &d);
  cw_map_set(&tex_cache, key, t.id); /* a failed load stays null: logged once */
  return t;
}

cw_font cw__font_get(const char *path, int mode, float bake) {
  if (!path || !*path) return (cw_font){ 0 };
  if (!font_cache.slot_cap && !font_cache.count) cw_map_init(&font_cache, CW_MEM_RENDER2D);
  char k[512];
  snprintf(k, sizeof k, "%s|%d|%g", path, mode, (double)bake);
  uint64_t key = (uint64_t)(uintptr_t)cw_intern(k), v;
  if (cw_map_get(&font_cache, key, &v)) return (cw_font){ v };
  cw_font_desc d = { .mode = mode ? CW_FONT_BITMAP : CW_FONT_SDF, .bake_size = bake, .threshold = mode != 0 };
  cw_font f = cw_font_load(path, &d);
  cw_map_set(&font_cache, key, f.id);
  return f;
}

int cw_points_parse(const char *text, cw_vec2 **out) {
  int cap = 0;
  for (const char *p = text ? text : ""; *p; p++) cap += (*p == ',' || *p == ' ' || *p == ';');
  cw_vec2 *pts = cw_frame_alloc(sizeof(cw_vec2) * (size_t)(cap / 2 + 2));
  int n = 0, k = 0;
  float pair[2];
  const char *p = text ? text : "";
  while (*p) {
    char *end;
    double d = strtod(p, &end);
    if (end == p) { p++; continue; }
    pair[k++] = (float)d;
    if (k == 2) {
      if (n < cap / 2 + 2) pts[n++] = cw_v2(pair[0], pair[1]);
      k = 0;
    }
    p = end;
  }
  *out = pts;
  return n;
}

const cw_vec2 *cw__points_get(cw__points *pp, const char *text, int *n) {
  if (pp->src != text || (!pp->pts.count && text && *text)) {
    pp->src = text;
    cw_array_clear(&pp->pts);
    cw_vec2 *tmp;
    int k = cw_points_parse(text, &tmp);
    for (int i = 0; i < k; i++) cw_array_push(&pp->pts, tmp[i]);
  }
  *n = pp->pts.count;
  return pp->pts.data;
}

/* a textured rectangle the way Sprite2D draws one */
static void draw_tex_rect(cw_texture tex, cw_rect src, bool centered, cw_vec2 offset, bool fx, bool fy, cw_color c) {
  cw_sprite s = { 0 };
  s.texture = tex;
  s.src = src;
  s.pos = offset;
  s.pivot = centered ? cw_v2(0.5f, 0.5f) : cw_v2(0, 0);
  s.color = c;
  s.flip_x = fx;
  s.flip_y = fy;
  if (src.w == 0 && cw_texture_valid(tex)) {
    int w, h;
    cw_texture_size(tex, &w, &h);
    s.src = cw_rect_make(0, 0, (float)w, (float)h);
  }
  cw_r2d_sprite(&s);
}

static cw_rect grid_cell(cw_texture tex, int hframes, int vframes, int frame) {
  int w = 0, h = 0;
  cw_texture_size(tex, &w, &h);
  if (hframes < 1) hframes = 1;
  if (vframes < 1) vframes = 1;
  float fw = (float)w / (float)hframes, fh = (float)h / (float)vframes;
  int n = hframes * vframes;
  if (frame < 0) frame = 0;
  frame %= n;
  return cw_rect_make((float)(frame % hframes) * fw, (float)(frame / hframes) * fh, fw, fh);
}

/* ── Sprite2D ───────────────────────────────────────────────────────── */

typedef struct sprite2d {
  const char *texture;
  int filter;
  bool centered;
  cw_vec2 offset;
  bool flip_h, flip_v;
  bool region_enabled;
  float region[4];
  int hframes, vframes, frame;
} sprite2d;

static void sprite_init(cw_node n, void *d) {
  (void)n;
  sprite2d *s = d;
  s->centered = true;
  s->hframes = s->vframes = 1;
}

static void sprite_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  sprite2d *s = d;
  cw_texture tex = cw__tex_get(s->texture, s->filter);
  cw_rect src = { 0 };
  if (s->region_enabled) src = cw_rect_make(s->region[0], s->region[1], s->region[2], s->region[3]);
  else if ((s->hframes > 1 || s->vframes > 1) && cw_texture_valid(tex)) src = grid_cell(tex, s->hframes, s->vframes, s->frame);
  draw_tex_rect(tex, src, s->centered, s->offset, s->flip_h, s->flip_v, it->modulate);
}

static const cw_prop sprite_props[] = {
  CW_PROP_FIELD(sprite2d, texture, CW_T_STRING, CW_PROP_DEFAULT, "Image asset path (PNG/JPEG)."),
  { .name = "filter", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(sprite2d, filter), .size = sizeof(int),
    .enum_names = filter_names, .doc = "Texture filtering (NEAREST for pixel art)." },
  CW_PROP_FIELD(sprite2d, centered, CW_T_BOOL, CW_PROP_DEFAULT, "The node's origin is the sprite's centre (else its top-left)."),
  CW_PROP_FIELD(sprite2d, offset, CW_T_VEC2, CW_PROP_DEFAULT, "Drawing offset, pixels."),
  CW_PROP_FIELD(sprite2d, flip_h, CW_T_BOOL, CW_PROP_DEFAULT, "Mirror horizontally."),
  CW_PROP_FIELD(sprite2d, flip_v, CW_T_BOOL, CW_PROP_DEFAULT, "Mirror vertically."),
  CW_PROP_FIELD(sprite2d, region_enabled, CW_T_BOOL, CW_PROP_DEFAULT, "Draw only `region` of the texture."),
  CW_PROP_FIELD(sprite2d, region, CW_T_VEC4, CW_PROP_DEFAULT, "Source rect in texture pixels: x, y, w, h."),
  CW_PROP_FIELD(sprite2d, hframes, CW_T_INT, CW_PROP_DEFAULT, "Atlas grid columns."),
  CW_PROP_FIELD(sprite2d, vframes, CW_T_INT, CW_PROP_DEFAULT, "Atlas grid rows."),
  CW_PROP_FIELD(sprite2d, frame, CW_T_INT, CW_PROP_DEFAULT, "Atlas grid cell drawn (row-major)."),
  CW_PROP_END,
};
static cw_node_type sprite_type = { .name = "Sprite2D", .parent = "Node2D", .data_size = sizeof(sprite2d), .props = sprite_props,
                                    .doc = "Draws a texture, a region of it or one cell of an atlas grid.", .init = sprite_init };

/* ── AnimatedSprite2D ───────────────────────────────────────────────── */

typedef struct anim_def {
  const char *name;
  float fps;
  bool once;
  int first, count; /* into anim_frames */
} anim_def;

typedef struct anim2d {
  const char *texture;
  int filter;
  int hframes, vframes;
  const char *regions;
  const char *animations;
  const char *animation;
  float fps, speed_scale;
  bool playing;
  int frame; /* index within the current animation */
  int process; /* 0 fixed, 1 frame */
  bool centered;
  cw_vec2 offset;
  bool flip_h, flip_v;
  /* runtime */
  double elapsed;
  const char *parsed;
  CW_ARRAY(anim_def) defs;
  CW_ARRAY(int) frames;
  cw__points regions_pts;
} anim2d;

static const char *const process_names[] = { "FIXED", "FRAME", NULL };
static const char *const anim_signals[] = { "frame_changed", "animation_finished", NULL };

static void anim_parse(anim2d *a) {
  if (a->parsed == a->animations && a->defs.count) return;
  a->parsed = a->animations;
  cw_array_clear(&a->defs);
  cw_array_clear(&a->frames);
  const char *p = a->animations ? a->animations : "";
  while (*p) {
    while (*p == ' ' || *p == ';') p++;
    if (!*p) break;
    const char *end = strchr(p, ';');
    if (!end) end = p + strlen(p);
    char buf[512];
    size_t len = (size_t)(end - p) < sizeof buf - 1 ? (size_t)(end - p) : sizeof buf - 1;
    memcpy(buf, p, len);
    buf[len] = 0;
    p = end;
    char *colon = strchr(buf, ':');
    if (!colon) continue;
    *colon = 0;
    anim_def d = { 0 };
    char *at = strchr(buf, '@');
    if (at) {
      *at = 0;
      d.fps = (float)strtod(at + 1, NULL);
    }
    char *nm = buf;
    while (*nm == ' ') nm++;
    char *ne = nm + strlen(nm);
    while (ne > nm && ne[-1] == ' ') *--ne = 0;
    d.name = cw_intern(nm);
    d.first = a->frames.count;
    char *q = colon + 1;
    while (*q) {
      if (*q == '!') { d.once = true; q++; continue; }
      char *e;
      long v = strtol(q, &e, 10);
      if (e == q) { q++; continue; }
      long hi = v;
      if (*e == '-') {
        char *e2;
        hi = strtol(e + 1, &e2, 10);
        if (e2 != e + 1) e = e2;
        else hi = v;
      }
      for (long k = v; v <= hi ? k <= hi : k >= hi; k += v <= hi ? 1 : -1) cw_array_push(&a->frames, (int)k);
      q = e;
    }
    d.count = a->frames.count - d.first;
    if (d.count) cw_array_push(&a->defs, d);
  }
}

static anim_def *anim_current(anim2d *a) {
  anim_parse(a);
  if (!a->defs.count) return NULL;
  if (a->animation && *a->animation)
    for (int i = 0; i < a->defs.count; i++)
      if (a->defs.data[i].name == a->animation) return &a->defs.data[i];
  return &a->defs.data[0];
}

static int anim_frame_count(anim2d *a, cw_texture tex) {
  anim_def *d = anim_current(a);
  if (d) return d->count;
  int n;
  const cw_vec2 *pts = cw__points_get(&a->regions_pts, a->regions, &n);
  (void)pts;
  if (n >= 2) return n / 2;
  (void)tex;
  return CW_MAX(1, a->hframes) * CW_MAX(1, a->vframes);
}

static void anim_advance(cw_node n, anim2d *a, double dt) {
  if (!a->playing) return;
  anim_def *d = anim_current(a);
  float fps = d && d->fps > 0 ? d->fps : a->fps;
  if (fps <= 0 || a->speed_scale <= 0) return;
  int count = anim_frame_count(a, (cw_texture){ 0 });
  bool once = d && d->once;
  a->elapsed += dt * (double)a->speed_scale;
  double step = 1.0 / (double)fps;
  while (a->elapsed >= step - 1e-9 && a->playing) { /* 1e-9: six 1/60 s ticks are one 1/10 s frame */
    a->elapsed -= step;
    if (a->frame + 1 >= count) {
      if (once) {
        a->playing = false;
        a->elapsed = 0;
        cw_emit(n, "animation_finished", NULL, 0);
        break;
      }
      a->frame = 0;
      cw_emit(n, "animation_finished", NULL, 0);
    } else {
      a->frame++;
    }
    cw_emit(n, "frame_changed", NULL, 0);
  }
}

static void anim_fixed(cw_node n, void *d, double dt) {
  anim2d *a = d;
  if (a->process == 0) anim_advance(n, a, dt);
}
static void anim_update(cw_node n, void *d, double dt) {
  anim2d *a = d;
  if (a->process == 1) anim_advance(n, a, dt);
}

static void anim_init(cw_node n, void *d) {
  (void)n;
  anim2d *a = d;
  a->hframes = a->vframes = 1;
  a->fps = 5;
  a->speed_scale = 1;
  a->playing = true;
  a->centered = true;
}
static void anim_destroy(cw_node n, void *d) {
  (void)n;
  anim2d *a = d;
  cw_array_free(&a->defs);
  cw_array_free(&a->frames);
  cw_array_free(&a->regions_pts.pts);
}

static void anim_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  anim2d *a = d;
  cw_texture tex = cw__tex_get(a->texture, a->filter);
  if (!cw_texture_valid(tex)) {
    draw_tex_rect(tex, cw_rect_make(0, 0, 16, 16), a->centered, a->offset, false, false, it->modulate);
    return;
  }
  anim_def *def = anim_current(a);
  int idx = a->frame;
  if (def) idx = a->frames.data[def->first + (a->frame % def->count + def->count) % def->count];
  int nr;
  const cw_vec2 *r = cw__points_get(&a->regions_pts, a->regions, &nr);
  cw_rect src;
  if (nr >= 2) {
    int k = (idx % (nr / 2) + nr / 2) % (nr / 2);
    src = cw_rect_make(r[2 * k].x, r[2 * k].y, r[2 * k + 1].x, r[2 * k + 1].y);
  } else {
    src = grid_cell(tex, a->hframes, a->vframes, idx);
  }
  draw_tex_rect(tex, src, a->centered, a->offset, a->flip_h, a->flip_v, it->modulate);
}

static void anim_get_animation(cw_node n, void *d, cw_value *o) {
  (void)n;
  anim2d *a = d;
  *o = cw_val_string(a->animation ? a->animation : "");
}
static void anim_set_animation(cw_node n, void *d, const cw_value *v) {
  (void)n;
  anim2d *a = d;
  const char *s = cw_intern(v->s ? v->s : "");
  if (s == a->animation) return;
  a->animation = s;
  a->frame = 0;
  a->elapsed = 0;
}

static const cw_prop anim_props[] = {
  CW_PROP_FIELD(anim2d, texture, CW_T_STRING, CW_PROP_DEFAULT, "Atlas image asset path."),
  { .name = "filter", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(anim2d, filter), .size = sizeof(int),
    .enum_names = filter_names, .doc = "Texture filtering." },
  CW_PROP_FIELD(anim2d, hframes, CW_T_INT, CW_PROP_DEFAULT, "Atlas grid columns."),
  CW_PROP_FIELD(anim2d, vframes, CW_T_INT, CW_PROP_DEFAULT, "Atlas grid rows."),
  CW_PROP_FIELD(anim2d, regions, CW_T_STRING, CW_PROP_DEFAULT, "Explicit frame rects \"x y w h, ...\" (replaces the grid)."),
  CW_PROP_FIELD(anim2d, animations, CW_T_STRING, CW_PROP_DEFAULT, "\"name@fps:frames; ...\" frames as indices/ranges, '!' = once."),
  CW_PROP_ACCESSOR("animation", CW_T_STRING, CW_PROP_DEFAULT, anim_get_animation, anim_set_animation, "Current animation (restarts on change)."),
  CW_PROP_FIELD_RANGE(anim2d, fps, CW_T_FLOAT, 0, 1000, "Frames per second for animations without @fps."),
  CW_PROP_FIELD_RANGE(anim2d, speed_scale, CW_T_FLOAT, 0, 100, "Playback speed multiplier."),
  CW_PROP_FIELD(anim2d, playing, CW_T_BOOL, CW_PROP_DEFAULT, "Advancing."),
  CW_PROP_FIELD(anim2d, frame, CW_T_INT, CW_PROP_DEFAULT, "Frame index within the current animation."),
  { .name = "process", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(anim2d, process), .size = sizeof(int),
    .enum_names = process_names, .doc = "FIXED: advance by fixed ticks (deterministic); FRAME: by frame time." },
  CW_PROP_FIELD(anim2d, centered, CW_T_BOOL, CW_PROP_DEFAULT, "Origin at the frame's centre."),
  CW_PROP_FIELD(anim2d, offset, CW_T_VEC2, CW_PROP_DEFAULT, "Drawing offset."),
  CW_PROP_FIELD(anim2d, flip_h, CW_T_BOOL, CW_PROP_DEFAULT, "Mirror horizontally."),
  CW_PROP_FIELD(anim2d, flip_v, CW_T_BOOL, CW_PROP_DEFAULT, "Mirror vertically."),
  CW_PROP_END,
};
static cw_node_type anim_type = { .name = "AnimatedSprite2D", .parent = "Node2D", .data_size = sizeof(anim2d), .props = anim_props,
                                  .signals = anim_signals, .doc = "Plays frame animations from an atlas grid or a rect list.",
                                  .init = anim_init, .destroy = anim_destroy, .fixed_update = anim_fixed, .update = anim_update };

void cw_animated_sprite2d_play(cw_node n, const char *name, bool restart) {
  anim2d *a = cw_node_data(n, "AnimatedSprite2D");
  if (!a) return;
  const char *s = cw_intern(name ? name : "");
  if (restart || s != a->animation) {
    a->animation = s;
    a->frame = 0;
    a->elapsed = 0;
  }
  a->playing = true;
}
void cw_animated_sprite2d_stop(cw_node n) {
  anim2d *a = cw_node_data(n, "AnimatedSprite2D");
  if (a) a->playing = false;
}

/* ── NinePatch2D ────────────────────────────────────────────────────── */

typedef struct nine2d {
  const char *texture;
  int filter;
  float region[4];
  cw_vec2 size;
  int patch_left, patch_top, patch_right, patch_bottom;
  float margin_scale;
  bool draw_center, centered;
} nine2d;

static void nine_init(cw_node n, void *d) {
  (void)n;
  nine2d *s = d;
  s->size = cw_v2(64, 64);
  s->patch_left = s->patch_top = s->patch_right = s->patch_bottom = 8;
  s->margin_scale = 1;
  s->draw_center = true;
}
static void nine_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  nine2d *s = d;
  cw_nine_slice ns = { 0 };
  ns.texture = cw__tex_get(s->texture, s->filter);
  ns.src = cw_rect_make(s->region[0], s->region[1], s->region[2], s->region[3]);
  ns.left = (float)s->patch_left;
  ns.top = (float)s->patch_top;
  ns.right = (float)s->patch_right;
  ns.bottom = (float)s->patch_bottom;
  ns.dst = cw_rect_make(s->centered ? -s->size.x * 0.5f : 0, s->centered ? -s->size.y * 0.5f : 0, s->size.x, s->size.y);
  ns.color = it->modulate;
  ns.margin_scale = s->margin_scale;
  ns.hollow = !s->draw_center;
  cw_r2d_nine_slice(&ns);
}
static const cw_prop nine_props[] = {
  CW_PROP_FIELD(nine2d, texture, CW_T_STRING, CW_PROP_DEFAULT, "Image asset path."),
  { .name = "filter", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(nine2d, filter), .size = sizeof(int),
    .enum_names = filter_names, .doc = "Texture filtering." },
  CW_PROP_FIELD(nine2d, region, CW_T_VEC4, CW_PROP_DEFAULT, "Source rect x y w h (w 0 = whole texture)."),
  CW_PROP_FIELD(nine2d, size, CW_T_VEC2, CW_PROP_DEFAULT, "Drawn size, pixels."),
  CW_PROP_FIELD(nine2d, patch_left, CW_T_INT, CW_PROP_DEFAULT, "Left margin, texture pixels."),
  CW_PROP_FIELD(nine2d, patch_top, CW_T_INT, CW_PROP_DEFAULT, "Top margin."),
  CW_PROP_FIELD(nine2d, patch_right, CW_T_INT, CW_PROP_DEFAULT, "Right margin."),
  CW_PROP_FIELD(nine2d, patch_bottom, CW_T_INT, CW_PROP_DEFAULT, "Bottom margin."),
  CW_PROP_FIELD_RANGE(nine2d, margin_scale, CW_T_FLOAT, 0, 100, "Margins' on-screen scale."),
  CW_PROP_FIELD(nine2d, draw_center, CW_T_BOOL, CW_PROP_DEFAULT, "Fill the centre."),
  CW_PROP_FIELD(nine2d, centered, CW_T_BOOL, CW_PROP_DEFAULT, "Origin at the centre (else the top-left)."),
  CW_PROP_END,
};
static cw_node_type nine_type = { .name = "NinePatch2D", .parent = "Node2D", .data_size = sizeof(nine2d), .props = nine_props,
                                  .doc = "A nine-slice panel: corners keep their size.", .init = nine_init };

/* ── Line2D, Polygon2D, LightOccluder2D ────────────────────────────── */

typedef struct line2d {
  const char *points;
  float width;
  float color[4];
  bool closed;
  cw__points cache;
} line2d;
static void line_init(cw_node n, void *d) {
  (void)n;
  line2d *l = d;
  l->width = 2;
  l->color[0] = l->color[1] = l->color[2] = l->color[3] = 1;
}
static void line_destroy(cw_node n, void *d) {
  (void)n;
  cw_array_free(&((line2d *)d)->cache.pts);
}
static void line_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  line2d *l = d;
  int k;
  const cw_vec2 *p = cw__points_get(&l->cache, l->points, &k);
  if (k >= 2) cw_r2d_polyline(p, k, l->width, l->closed, cw__col_mul(cw__col4(l->color), it->modulate));
}
static const cw_prop line_props[] = {
  CW_PROP_FIELD(line2d, points, CW_T_STRING, CW_PROP_DEFAULT, "Points \"x y, x y, ...\" (local pixels)."),
  CW_PROP_FIELD_RANGE(line2d, width, CW_T_FLOAT, 0, 10000, "Stroke width, pixels."),
  CW_PROP_FIELD(line2d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Stroke colour."),
  CW_PROP_FIELD(line2d, closed, CW_T_BOOL, CW_PROP_DEFAULT, "Join the last point to the first."),
  CW_PROP_END,
};
static cw_node_type line_type = { .name = "Line2D", .parent = "Node2D", .data_size = sizeof(line2d), .props = line_props,
                                  .doc = "A polyline with mitred joins.", .init = line_init, .destroy = line_destroy };

typedef struct poly2d {
  const char *polygon;
  float color[4];
  const char *texture;
  int filter;
  cw_vec2 offset;
  cw__points cache;
  const char *tri_src;
  CW_ARRAY(uint32_t) tris;
} poly2d;
static void poly_init(cw_node n, void *d) {
  (void)n;
  poly2d *p = d;
  p->color[0] = p->color[1] = p->color[2] = p->color[3] = 1;
}
static void poly_destroy(cw_node n, void *d) {
  (void)n;
  poly2d *p = d;
  cw_array_free(&p->cache.pts);
  cw_array_free(&p->tris);
}
static void poly_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  poly2d *p = d;
  int k;
  const cw_vec2 *pts = cw__points_get(&p->cache, p->polygon, &k);
  if (k < 3) return;
  if (p->tri_src != p->polygon || !p->tris.count) {
    p->tri_src = p->polygon;
    cw_array_reserve(&p->tris, 3 * k);
    p->tris.count = cw_triangulate(pts, k, p->tris.data);
  }
  if (!p->tris.count) return;
  cw_color c = cw__col_mul(cw__col4(p->color), it->modulate);
  cw_texture tex = cw__tex_get(p->texture, p->filter);
  float tw = 1, th = 1;
  if (cw_texture_valid(tex)) {
    int w, h;
    cw_texture_size(tex, &w, &h);
    tw = (float)w;
    th = (float)h;
  }
  cw_vertex2d *v = cw_frame_alloc(sizeof(cw_vertex2d) * (size_t)k);
  for (int i = 0; i < k; i++) {
    v[i].pos = cw_v2_add(pts[i], p->offset);
    v[i].uv = cw_v2(pts[i].x / tw, pts[i].y / th);
    v[i].color = c;
  }
  cw_r2d_triangles(tex, v, k, p->tris.data, p->tris.count);
}
static const cw_prop poly_props[] = {
  CW_PROP_FIELD(poly2d, polygon, CW_T_STRING, CW_PROP_DEFAULT, "Outline \"x y, x y, ...\" (simple polygon, any winding)."),
  CW_PROP_FIELD(poly2d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Fill colour (multiplies the texture)."),
  CW_PROP_FIELD(poly2d, texture, CW_T_STRING, CW_PROP_DEFAULT, "Optional texture; uv = point / texture size."),
  { .name = "filter", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(poly2d, filter), .size = sizeof(int),
    .enum_names = filter_names, .doc = "Texture filtering." },
  CW_PROP_FIELD(poly2d, offset, CW_T_VEC2, CW_PROP_DEFAULT, "Drawing offset."),
  CW_PROP_END,
};
static cw_node_type poly_type = { .name = "Polygon2D", .parent = "Node2D", .data_size = sizeof(poly2d), .props = poly_props,
                                  .doc = "A filled (optionally textured) polygon.", .init = poly_init, .destroy = poly_destroy };

typedef struct occl2d {
  const char *polygon;
  bool closed;
  cw__points cache;
} occl2d;
static void occl_init(cw_node n, void *d) {
  (void)n;
  ((occl2d *)d)->closed = true;
}
static void occl_destroy(cw_node n, void *d) {
  (void)n;
  cw_array_free(&((occl2d *)d)->cache.pts);
}
static void occl_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  occl2d *o = d;
  int k;
  const cw_vec2 *p = cw__points_get(&o->cache, o->polygon, &k);
  if (k >= 2 && cw_r2d_layer_get(it->layer)->lit) cw_r2d_occluder(p, k, o->closed);
}
static const cw_prop occl_props[] = {
  CW_PROP_FIELD(occl2d, polygon, CW_T_STRING, CW_PROP_DEFAULT, "Occluder outline \"x y, ...\"."),
  CW_PROP_FIELD(occl2d, closed, CW_T_BOOL, CW_PROP_DEFAULT, "Join the ends."),
  CW_PROP_END,
};
static cw_node_type occl_type = { .name = "LightOccluder2D", .parent = "Node2D", .data_size = sizeof(occl2d), .props = occl_props,
                                  .doc = "Casts shadows for Light2Ds with shadow on.", .init = occl_init, .destroy = occl_destroy };

/* ── Light2D ────────────────────────────────────────────────────────── */

static void light_init(cw_node n, void *d) {
  (void)n;
  cw__light2d *l = d;
  l->enabled = true;
  l->color[0] = l->color[1] = l->color[2] = l->color[3] = 1;
  l->energy = 1;
  l->range = 128;
  l->falloff = 2;
}
static void light_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  cw__light2d *l = d;
  if (!l->enabled || !cw_r2d_layer_get(it->layer)->lit) return;
  cw_light2d L = { 0 };
  L.radius = l->range;
  L.color = cw__col_mul(cw__col4(l->color), it->modulate);
  L.energy = l->energy;
  L.falloff = l->falloff;
  L.height = l->height;
  L.shadows = l->shadow;
  cw_r2d_light(&L);
}
static const cw_prop light_props[] = {
  CW_PROP_FIELD(cw__light2d, enabled, CW_T_BOOL, CW_PROP_DEFAULT, "On."),
  CW_PROP_FIELD(cw__light2d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Light colour."),
  CW_PROP_FIELD_RANGE(cw__light2d, energy, CW_T_FLOAT, 0, 100, "Intensity."),
  CW_PROP_FIELD_RANGE(cw__light2d, range, CW_T_FLOAT, 0, 1e6, "Radius, pixels (light is 0 there)."),
  CW_PROP_FIELD_RANGE(cw__light2d, falloff, CW_T_FLOAT, 0, 16, "Exponent of (1 - d/range)."),
  CW_PROP_FIELD_RANGE(cw__light2d, height, CW_T_FLOAT, 0, 1e6, "Height above the plane for normal maps (0 = range/4)."),
  CW_PROP_FIELD(cw__light2d, shadow, CW_T_BOOL, CW_PROP_DEFAULT, "Cast shadows from LightOccluder2Ds."),
  CW_PROP_END,
};
static cw_node_type light_type = { .name = "Light2D", .parent = "Node2D", .data_size = sizeof(cw__light2d), .props = light_props,
                                   .doc = "A point light; its canvas is drawn lit.", .init = light_init };

/* ── Label2D ────────────────────────────────────────────────────────── */

typedef struct label2d {
  const char *text, *font;
  int font_mode;
  float bake_size, size;
  float color[4];
  int align;
  float wrap_width, line_spacing, letter_spacing;
} label2d;
static const char *const font_mode_names[] = { "SDF", "BITMAP", NULL };
static const char *const align_names[] = { "LEFT", "CENTER", "RIGHT", NULL };
static void label_init(cw_node n, void *d) {
  (void)n;
  label2d *l = d;
  l->size = 16;
  l->color[0] = l->color[1] = l->color[2] = l->color[3] = 1;
}
static void label_draw(cw_node n, void *d, const cw_canvas_item *it) {
  (void)n;
  label2d *l = d;
  if (!l->text || !*l->text) return;
  cw_font f = cw__font_get(l->font, l->font_mode, l->bake_size);
  if (CW_HANDLE_IS_NULL(f)) return;
  cw_text_style st = { .size = l->size, .color = cw__col_mul(cw__col4(l->color), it->modulate), .wrap_width = l->wrap_width,
                       .align = (cw_text_align)l->align, .line_spacing = l->line_spacing, .letter_spacing = l->letter_spacing };
  cw_r2d_text(f, l->text, cw_v2(0, 0), &st);
}
static const cw_prop label_props[] = {
  CW_PROP_FIELD(label2d, text, CW_T_STRING, CW_PROP_DEFAULT, "UTF-8 text ('\\n' breaks lines)."),
  CW_PROP_FIELD(label2d, font, CW_T_STRING, CW_PROP_DEFAULT, "TTF/OTF asset path."),
  { .name = "font_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(label2d, font_mode), .size = sizeof(int),
    .enum_names = font_mode_names, .doc = "SDF (any size) or BITMAP (pixel fonts at bake_size multiples)." },
  CW_PROP_FIELD_RANGE(label2d, bake_size, CW_T_FLOAT, 0, 512, "Bake size (0 = the mode's default)."),
  CW_PROP_FIELD_RANGE(label2d, size, CW_T_FLOAT, 0, 4096, "Pixel size of the em."),
  CW_PROP_FIELD(label2d, color, CW_T_COLOR, CW_PROP_DEFAULT, "Text colour."),
  { .name = "align", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(label2d, align), .size = sizeof(int),
    .enum_names = align_names, .doc = "Alignment within wrap_width (or the widest line)." },
  CW_PROP_FIELD_RANGE(label2d, wrap_width, CW_T_FLOAT, 0, 1e6, "Wrap at this width (0 = no wrapping)."),
  CW_PROP_FIELD_RANGE(label2d, line_spacing, CW_T_FLOAT, 0, 100, "Line height multiple (0 = 1)."),
  CW_PROP_FIELD_RANGE(label2d, letter_spacing, CW_T_FLOAT, -1000, 1000, "Extra pixels after each glyph."),
  CW_PROP_END,
};
static cw_node_type label_type = { .name = "Label2D", .parent = "Node2D", .data_size = sizeof(label2d), .props = label_props,
                                   .doc = "Text in the canvas.", .init = label_init };

/* ── Camera2D ───────────────────────────────────────────────────────── */

static void cam_init(cw_node n, void *d) {
  (void)n;
  cw__camera2d *c = d;
  c->current = true;
  c->zoom = 1;
  c->smoothing_speed = 5;
  c->limit_left = c->limit_top = -10000000;
  c->limit_right = c->limit_bottom = 10000000;
}
static void cam_enter(cw_node n, void *d) {
  (void)d;
  cw__canvas_camera_enter(n);
}
static void cam_exit(cw_node n, void *d) {
  (void)d;
  cw__canvas_camera_exit(n);
}
static const cw_prop cam_props[] = {
  CW_PROP_FIELD(cw__camera2d, current, CW_T_BOOL, CW_PROP_DEFAULT, "The first current Camera2D in tree order is used."),
  CW_PROP_FIELD_RANGE(cw__camera2d, zoom, CW_T_FLOAT, 0.001, 1000, ">1 magnifies."),
  CW_PROP_FIELD(cw__camera2d, offset, CW_T_VEC2, CW_PROP_DEFAULT, "Screen offset (shake)."),
  CW_PROP_FIELD(cw__camera2d, anchor_top_left, CW_T_BOOL, CW_PROP_DEFAULT, "The camera's position is the view's top-left (else its centre)."),
  CW_PROP_FIELD(cw__camera2d, smoothing, CW_T_BOOL, CW_PROP_DEFAULT, "Follow the node exponentially (fixed ticks)."),
  CW_PROP_FIELD_RANGE(cw__camera2d, smoothing_speed, CW_T_FLOAT, 0, 1000, "Smoothing rate, 1/s."),
  CW_PROP_FIELD(cw__camera2d, limit_left, CW_T_INT, CW_PROP_DEFAULT, "The view never shows world x below this."),
  CW_PROP_FIELD(cw__camera2d, limit_top, CW_T_INT, CW_PROP_DEFAULT, "... y above this."),
  CW_PROP_FIELD(cw__camera2d, limit_right, CW_T_INT, CW_PROP_DEFAULT, "... x past this."),
  CW_PROP_FIELD(cw__camera2d, limit_bottom, CW_T_INT, CW_PROP_DEFAULT, "... y past this."),
  CW_PROP_FIELD(cw__camera2d, rotating, CW_T_BOOL, CW_PROP_DEFAULT, "Follow the node's global rotation."),
  CW_PROP_END,
};
static cw_node_type cam_type = { .name = "Camera2D", .parent = "Node2D", .data_size = sizeof(cw__camera2d), .props = cam_props,
                                 .doc = "The view of the default canvas (and canvases that follow the viewport).",
                                 .init = cam_init, .enter_tree = cam_enter, .exit_tree = cam_exit };

/* ── CanvasLayer, ParallaxLayer, CanvasSettings ─────────────────────── */

static void cl_init(cw_node n, void *d) {
  (void)n;
  cw__canvas_layer *c = d;
  c->layer = 1;
  c->scale = cw_v2(1, 1);
  c->visible = true;
}
static const cw_prop cl_props[] = {
  CW_PROP_FIELD(cw__canvas_layer, layer, CW_T_INT, CW_PROP_DEFAULT, "Draw order; the default canvas is 0."),
  CW_PROP_FIELD(cw__canvas_layer, offset, CW_T_VEC2, CW_PROP_DEFAULT, "Layer offset, pixels."),
  CW_PROP_FIELD(cw__canvas_layer, rotation, CW_T_FLOAT, CW_PROP_DEFAULT, "Layer rotation."),
  CW_PROP_FIELD(cw__canvas_layer, scale, CW_T_VEC2, CW_PROP_DEFAULT, "Layer scale."),
  CW_PROP_FIELD(cw__canvas_layer, follow_viewport, CW_T_BOOL, CW_PROP_DEFAULT, "Use the Camera2D (else screen space: HUDs)."),
  CW_PROP_FIELD(cw__canvas_layer, visible, CW_T_BOOL, CW_PROP_DEFAULT, "Hide the whole layer."),
  CW_PROP_FIELD(cw__canvas_layer, y_sort, CW_T_BOOL, CW_PROP_DEFAULT, "Within equal z, lower y draws first."),
  CW_PROP_END,
};
static cw_node_type cl_type = { .name = "CanvasLayer", .parent = "Node", .data_size = sizeof(cw__canvas_layer), .props = cl_props,
                                .doc = "A separate canvas drawn in `layer` order (HUDs, backgrounds).", .init = cl_init };

static void pl_init(cw_node n, void *d) {
  (void)n;
  ((cw__parallax_layer *)d)->motion_scale = cw_v2(1, 1);
}
static const cw_prop pl_props[] = {
  CW_PROP_FIELD(cw__parallax_layer, motion_scale, CW_T_VEC2, CW_PROP_DEFAULT, "Camera motion factor (0.5 = half speed)."),
  CW_PROP_FIELD(cw__parallax_layer, motion_offset, CW_T_VEC2, CW_PROP_DEFAULT, "Extra offset, pixels."),
  CW_PROP_END,
};
static cw_node_type pl_type = { .name = "ParallaxLayer", .parent = "Node2D", .data_size = sizeof(cw__parallax_layer),
                                .props = pl_props, .doc = "Its children scroll at motion_scale of the camera.", .init = pl_init };

static const char *const scale_names[] = { "NONE", "INTEGER", "FIT", "STRETCH", NULL };
static void cs_init(cw_node n, void *d) {
  (void)n;
  cw__canvas_settings *s = d;
  s->clear_color[3] = 1;
  s->letterbox[3] = 1;
  for (int i = 0; i < 4; i++) s->ambient[i] = 1;
}
static const cw_prop cs_props[] = {
  CW_PROP_FIELD(cw__canvas_settings, virtual_width, CW_T_INT, CW_PROP_DEFAULT, "Virtual canvas width (0 = window pixels)."),
  CW_PROP_FIELD(cw__canvas_settings, virtual_height, CW_T_INT, CW_PROP_DEFAULT, "Virtual canvas height."),
  { .name = "scale_mode", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(cw__canvas_settings, scale_mode),
    .size = sizeof(int), .enum_names = scale_names, .doc = "How the virtual canvas maps to the window." },
  CW_PROP_FIELD(cw__canvas_settings, clear_color, CW_T_COLOR, CW_PROP_DEFAULT, "Background (2D-only trees)."),
  CW_PROP_FIELD(cw__canvas_settings, letterbox, CW_T_COLOR, CW_PROP_DEFAULT, "Bars around a scaled canvas."),
  CW_PROP_FIELD(cw__canvas_settings, ambient, CW_T_COLOR, CW_PROP_DEFAULT, "Light on lit canvases where no Light2D reaches."),
  CW_PROP_FIELD(cw__canvas_settings, pixel_snap, CW_T_BOOL, CW_PROP_DEFAULT, "Snap vertices to whole canvas pixels."),
  CW_PROP_FIELD(cw__canvas_settings, y_sort, CW_T_BOOL, CW_PROP_DEFAULT, "Y-sort the default canvas."),
  CW_PROP_END,
};
static cw_node_type cs_type = { .name = "CanvasSettings", .parent = "Node", .data_size = sizeof(cw__canvas_settings),
                                .props = cs_props, .doc = "Virtual resolution, scaling, clear colour and 2D ambient light.",
                                .init = cs_init };

/* ── registration ───────────────────────────────────────────────────── */

void cw__register_tilemap2d_type(void);

void cw__register_node2d_types(void) {
  cw__node2d_t = cw_node_type_find("Node2D");
  cw_node_type_register(&sprite_type);
  cw_canvas_register_type("Sprite2D", sprite_draw);
  cw_node_type_register(&anim_type);
  cw_canvas_register_type("AnimatedSprite2D", anim_draw);
  cw_node_type_register(&nine_type);
  cw_canvas_register_type("NinePatch2D", nine_draw);
  cw_node_type_register(&line_type);
  cw_canvas_register_type("Line2D", line_draw);
  cw_node_type_register(&poly_type);
  cw_canvas_register_type("Polygon2D", poly_draw);
  cw_node_type_register(&label_type);
  cw_canvas_register_type("Label2D", label_draw);
  cw_node_type_register(&light_type);
  cw__light2d_t = &light_type;
  cw_canvas_register_type("Light2D", light_draw);
  cw_node_type_register(&occl_type);
  cw_canvas_register_type("LightOccluder2D", occl_draw);
  cw_node_type_register(&cam_type);
  cw__camera2d_t = &cam_type;
  cw_node_type_register(&cl_type);
  cw__canvas_layer_t = &cl_type;
  cw_node_type_register(&pl_type);
  cw__parallax_t = &pl_type;
  cw_node_type_register(&cs_type);
  cw__settings2d_t = &cs_type;
  cw__register_tilemap2d_type();
}
