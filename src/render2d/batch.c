/* render2d core: module state, passes, canvas layers, the submission stream,
 * the stable sort, batching and drawing. */
#include <string.h>

#include "internal.h"
#include "cartwheel/debug.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"
#include "cartwheel/profile.h"

r2d_state R = { .atlas_array = { -1, -1 } };

/* ── init / shutdown ───────────────────────────────────────────────────── */

static void stream_vao(int i) {
  glBindVertexArray(R.vao[i]);
  glBindBuffer(GL_ARRAY_BUFFER, R.vbo[i]);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R.ibo[i]);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(r2d_vertex), (void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(r2d_vertex), (void *)8);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(r2d_vertex), (void *)16);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(r2d_vertex), (void *)20);
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(r2d_vertex), (void *)24);
  glBindVertexArray(0);
}

bool r2d_lazy_init(void) {
  if (R.inited) return true;
  if (R.failed) return false;
  if (r2d_wg()) { /* WebGPU: pipelines, stream buffers, the dummy normal array */
    if (!r2d_wg_init()) {
      R.failed = true;
      return false;
    }
    cw_pool_init(&R.textures, sizeof(r2d_tex), CW_MEM_RENDER2D);
    R.atlas_array[0] = R.atlas_array[1] = -1;
    R.max_layers = CW_CLAMP(r2d_wg_max_layers(), 1, 256);
    r2d_fonts_init();
    r2d_tilemaps_init();
    cw_array_push(&R.clips, ((cw_rect){ 0, 0, 0, 0 }));
    R.inited = true;
    return true;
  }
  if (!r2d_programs_init()) {
    R.failed = true;
    return false;
  }
  cw_pool_init(&R.textures, sizeof(r2d_tex), CW_MEM_RENDER2D);
  R.atlas_array[0] = R.atlas_array[1] = -1;
  GLint ml = 256;
  glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &ml);
  R.max_layers = CW_CLAMP(ml, 1, 256);
  glGenBuffers(R2D_RING, R.vbo);
  glGenBuffers(R2D_RING, R.ibo);
  glGenVertexArrays(R2D_RING, R.vao);
  for (int i = 0; i < R2D_RING; i++) stream_vao(i);
  /* scratch quads: pos.xy, uv.xy */
  glGenBuffers(1, &R.quad_vbo);
  glGenVertexArrays(1, &R.quad_vao);
  glBindVertexArray(R.quad_vao);
  glBindBuffer(GL_ARRAY_BUFFER, R.quad_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
  glBindVertexArray(0);
  /* the "no normal map" array every lit draw samples when its array has none */
  glGenTextures(1, &R.dummy_normal);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, R.dummy_normal);
  static const uint8_t none[4] = { 128, 128, 0, 255 };
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, none);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  r2d_fonts_init();
  r2d_tilemaps_init();
  cw_array_push(&R.clips, ((cw_rect){ 0, 0, 0, 0 })); /* clip 0 = none */
  R.inited = true;
  return true;
}

bool cw_r2d_init(void) { return r2d_lazy_init(); }

void cw_r2d_shutdown(void) {
  if (!R.inited) return;
  r2d_fonts_shutdown();
  r2d_tilemaps_shutdown();
  r2d_textures_shutdown();
  if (r2d_wg()) r2d_wg_shutdown();
  else {
    r2d_program *ps[] = { &R.sprite, &R.sprite_lit, &R.tile, &R.tile_lit, &R.light, &R.shadow, &R.composite, &R.blit };
  for (size_t i = 0; i < CW_ARRAY_LEN(ps); i++)
    if (ps[i]->prog) glDeleteProgram(ps[i]->prog);
  r2d_yuv_gl_shutdown();
  glDeleteBuffers(R2D_RING, R.vbo);
  glDeleteBuffers(R2D_RING, R.ibo);
  glDeleteVertexArrays(R2D_RING, R.vao);
  glDeleteBuffers(1, &R.quad_vbo);
  glDeleteVertexArrays(1, &R.quad_vao);
  if (R.tile_ibo) glDeleteBuffers(1, &R.tile_ibo);
  }
  cw_array_free(&R.verts);
  cw_array_free(&R.idx);
  cw_array_free(&R.cmds);
  cw_array_free(&R.tdraws);
  cw_array_free(&R.lights);
  cw_array_free(&R.occluders);
  cw_array_free(&R.occ_pts);
  cw_array_free(&R.order);
  cw_array_free(&R.order_tmp);
  cw_array_free(&R.keys);
  cw_array_free(&R.scmds);
  cw_array_free(&R.keys_tmp);
  cw_array_free(&R.out_idx);
  cw_array_free(&R.clips);
  memset(&R, 0, sizeof R);
  R.atlas_array[0] = R.atlas_array[1] = -1;
}

/* ── stats ─────────────────────────────────────────────────────────────── */

static uint32_t atlas_pages_total;

const cw_r2d_stats *cw_r2d_get_stats(void) {
  R.stats.glyphs_missing = r2d_glyphs_missing();
  return &R.stats;
}

void cw_r2d_debug_register(const char *prefix) {
  static const struct { const char *name; size_t off; } f[] = {
#define F(n) { #n, offsetof(cw_r2d_stats, n) }
    F(passes), F(draw_calls), F(batches), F(vertices), F(indices), F(commands), F(sorted), F(tile_chunks),
    F(chunk_rebuilds), F(lights), F(atlas_pages), F(texture_uploads), F(shaped_runs), F(glyphs_missing),
    F(shape_cache_hits),
#undef F
  };
  for (size_t i = 0; i < CW_ARRAY_LEN(f); i++)
    cw_debug_u32(cw_intern(cw_frame_printf("%s.%s", prefix, f[i].name)), (uint8_t *)&R.stats + f[i].off);
}

void r2d_count_draw(void) { R.stats.draw_calls++; }

void r2d_stats_frame_check(void) {
  uint64_t fr = cw_frame_index();
  if (!R.stats_frame_valid || fr != R.stats_frame) {
    uint32_t pages = R.stats.atlas_pages;
    atlas_pages_total = pages;
    memset(&R.stats, 0, sizeof R.stats);
    R.stats.atlas_pages = atlas_pages_total;
    R.stats.glyphs_missing = r2d_glyphs_missing();
    R.stats_frame = fr;
    R.stats_frame_valid = true;
  }
}

/* ── layers ────────────────────────────────────────────────────────────── */

static cw_rect layer_viewport(int l) {
  cw_rect v = R.layers[l].viewport;
  if (v.w <= 0 || v.h <= 0) v = (cw_rect){ 0, 0, (float)R.view_w, (float)R.view_h };
  return v;
}

static cw_xform2d layer_view(int l) {
  const cw_r2d_layer_desc *d = &R.layers[l];
  if (!d->has_camera) return cw_xform2d_identity();
  return cw_camera2d_view(&d->camera, layer_viewport(l), d->parallax);
}

/* what the GPU applies: the layer's view, then logical canvas -> pass pixels */
static cw_xform2d pass_view(int l) {
  cw_xform2d m = layer_view(l);
  if (R.px_x == 1 && R.px_y == 1) return m;
  m.a *= R.px_x, m.c *= R.px_x, m.tx *= R.px_x;
  m.b *= R.px_y, m.d *= R.px_y, m.ty *= R.px_y;
  return m;
}

cw_rect r2d_clip_px(cw_rect c) { return (cw_rect){ c.x * R.px_x, c.y * R.px_y, c.w * R.px_x, c.h * R.px_y }; }
float r2d_px(void) { return CW_MAX(R.px_x, R.px_y); }

void cw_r2d_layer_set(int layer, const cw_r2d_layer_desc *desc) {
  CW_ASSERT(layer >= 0 && layer < CW_R2D_MAX_LAYERS, "layer index 0..CW_R2D_MAX_LAYERS-1");
  R.layers[layer] = desc ? *desc : (cw_r2d_layer_desc){ 0 };
  if (layer == R.cur_layer) R.cur_clip = -1; /* viewport may have changed */
}

const cw_r2d_layer_desc *cw_r2d_layer_get(int layer) {
  CW_ASSERT(layer >= 0 && layer < CW_R2D_MAX_LAYERS, "layer index 0..CW_R2D_MAX_LAYERS-1");
  return &R.layers[layer];
}

void cw_r2d_layer_set_camera(int layer, const cw_camera2d *cam) {
  CW_ASSERT(layer >= 0 && layer < CW_R2D_MAX_LAYERS, "layer index 0..CW_R2D_MAX_LAYERS-1");
  R.layers[layer].has_camera = cam != NULL;
  if (cam) R.layers[layer].camera = *cam;
}

void cw_r2d_use_layer(int layer) {
  CW_ASSERT(layer >= 0 && layer < CW_R2D_MAX_LAYERS, "layer index 0..CW_R2D_MAX_LAYERS-1");
  if (layer != R.cur_layer) R.cur_clip = -1;
  R.cur_layer = layer;
}

cw_vec2 cw_r2d_layer_screen_to_world(int layer, cw_vec2 p) {
  cw_xform2d m = layer_view(layer);
  cw_xform2d inv = cw_xform2d_invert(&m);
  return cw_xform2d_apply(&inv, p);
}

cw_vec2 cw_r2d_layer_world_to_screen(int layer, cw_vec2 w) {
  cw_xform2d m = layer_view(layer);
  return cw_xform2d_apply(&m, w);
}

/* ── draw state ────────────────────────────────────────────────────────── */

void cw_r2d_set_blend(cw_blend b) { R.blend = b; }
void cw_r2d_set_z(int z) { R.cur_z = CW_CLAMP(z, -32768, 32767); }
void cw_r2d_set_transform(const cw_xform2d *m) {
  R.has_xform = m != NULL;
  R.xform = m ? *m : cw_xform2d_identity();
}

static cw_rect rect_intersect(cw_rect a, cw_rect b) {
  float x0 = CW_MAX(a.x, b.x), y0 = CW_MAX(a.y, b.y);
  float x1 = CW_MIN(a.x + a.w, b.x + b.w), y1 = CW_MIN(a.y + a.h, b.y + b.h);
  return (cw_rect){ x0, y0, CW_MAX(0, x1 - x0), CW_MAX(0, y1 - y0) };
}

static cw_rect clip_top(void) {
  return R.clip_depth ? R.clips.data[R.clip_stack[R.clip_depth - 1]] : (cw_rect){ -1e9f, -1e9f, 2e9f, 2e9f };
}

void cw_r2d_push_clip(cw_rect r) {
  CW_ASSERT(R.clip_depth < 32, "clip stack overflow: pop what you push");
  cw_rect c = rect_intersect(clip_top(), r);
  int idx = R.clips.count;
  CW_ASSERT(idx < 255, "more than 254 clip rects in one pass");
  cw_array_push(&R.clips, c);
  R.clip_stack[R.clip_depth++] = idx;
  R.cur_clip = -1;
}

void cw_r2d_pop_clip(void) {
  CW_ASSERT(R.clip_depth > 0, "cw_r2d_pop_clip without a push");
  R.clip_depth--;
  R.cur_clip = -1;
}

/* the clip id for the current layer + clip stack (layer viewports clip too) */
static int current_clip(void) {
  if (R.cur_clip >= 0) return R.cur_clip;
  const cw_r2d_layer_desc *d = &R.layers[R.cur_layer];
  bool vp = d->viewport.w > 0 && d->viewport.h > 0;
  if (!vp && !R.clip_depth) return R.cur_clip = 0;
  if (!vp) return R.cur_clip = R.clip_stack[R.clip_depth - 1];
  cw_rect c = rect_intersect(clip_top(), d->viewport);
  for (int i = 1; i < R.clips.count; i++)
    if (!memcmp(&R.clips.data[i], &c, sizeof c)) return R.cur_clip = i;
  CW_ASSERT(R.clips.count < 255, "more than 254 clip rects in one pass");
  cw_array_push(&R.clips, c);
  return R.cur_clip = R.clips.count - 1;
}

/* ── submission ────────────────────────────────────────────────────────── */

void r2d_pack_color(uint8_t out[4], cw_color c) {
  float v[4] = { c.r, c.g, c.b, c.a };
  for (int i = 0; i < 4; i++) {
    float x = v[i] < 0 ? 0 : v[i] > 1 ? 1 : v[i];
    out[i] = (uint8_t)(x * 255.0f + 0.5f);
  }
}

static inline uint32_t sortable_float(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

uint64_t r2d_make_key(int extra_z, float sort_y) {
  int z = CW_CLAMP(R.cur_z + extra_z, -32768, 32767);
  uint64_t key = ((uint64_t)R.cur_layer << 56) | ((uint64_t)(uint32_t)(z + 32768) << 40);
  if (R.layers[R.cur_layer].y_sort) key |= (uint64_t)sortable_float(sort_y) << 8;
  return key;
}

uint32_t r2d_current_state(uint32_t array) {
  cw_blend b = R.blend == CW_BLEND_ADD ? CW_BLEND_ALPHA : R.blend;
  return R2D_STATE(array, b, current_clip(), R2D_KIND_STREAM);
}

void r2d_push_cmd(uint64_t key, uint32_t state, uint32_t ifirst, uint32_t icount) {
  /* extend the previous command when nothing about it differs: keeps the
   * command list (and the sort) short when a layer is not y-sorted */
  if (R.cmds.count) {
    r2d_cmd *p = &R.cmds.data[R.cmds.count - 1];
    if (p->key == key && p->state == state && p->ifirst + p->icount == ifirst && R2D_ST_KIND(state) == R2D_KIND_STREAM) {
      p->icount += icount;
      return;
    }
  }
  cw_array_push(&R.cmds, ((r2d_cmd){ key, state, ifirst, icount, 0, 0 }));
  R.stats.commands++;
}

r2d_vertex *r2d_alloc(int nv, int ni, uint32_t **idx_out, uint32_t *base_out, uint32_t array, float sort_y,
                      int extra_z) {
  CW_ASSERT(R.in_pass, "submit between cw_r2d_begin and cw_r2d_end");
  int v0 = R.verts.count, i0 = R.idx.count;
  cw_array_reserve(&R.verts, v0 + nv);
  cw_array_reserve(&R.idx, i0 + ni);
  R.verts.count += nv;
  R.idx.count += ni;
  *idx_out = R.idx.data + i0;
  *base_out = (uint32_t)v0;
  r2d_push_cmd(r2d_make_key(extra_z, sort_y), r2d_current_state(array), (uint32_t)i0, (uint32_t)ni);
  return R.verts.data + v0;
}

void r2d_push_special(uint32_t array, float sort_y, uint32_t payload) {
  CW_ASSERT(R.in_pass, "submit between cw_r2d_begin and cw_r2d_end");
  cw_blend b = R.blend == CW_BLEND_ADD ? CW_BLEND_ALPHA : R.blend;
  cw_array_push(&R.cmds, ((r2d_cmd){ r2d_make_key(0, sort_y), R2D_STATE(array, b, current_clip(), R2D_KIND_TILEMAP),
                                     (uint32_t)R.idx.count, 0, payload }));
  R.stats.commands++;
}

/* mode bits for a submission: solid / additive */
uint8_t r2d_mode(bool solid) {
  return (uint8_t)((solid ? R2D_MODE_SOLID : 0) | (R.blend == CW_BLEND_ADD ? R2D_MODE_ADD : 0));
}

/* ── the scratch quad (lights, composite, blit) ───────────────────────── */

void r2d_scratch_draw(const float *xyuv, int nverts, GLenum mode) {
  glBindVertexArray(R.quad_vao);
  glBindBuffer(GL_ARRAY_BUFFER, R.quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * 16, xyuv, GL_STREAM_DRAW);
  glDrawArrays(mode, 0, nverts);
  R.stats.draw_calls++;
}

void r2d_draw_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
  float q[16] = { x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1 };
  r2d_scratch_draw(q, 4, GL_TRIANGLE_STRIP);
}

void r2d_draw_quad_px(float x0, float y0, float x1, float y1, int tw, int th) {
  (void)tw;
  (void)th;
  r2d_draw_quad(x0, y0, x1, y1, 0, 0, 1, 1);
}

void r2d_set_target_fbo(GLuint fbo, int w, int h) {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(0, 0, w, h);
}

/* ── passes ────────────────────────────────────────────────────────────── */

void r2d_ensure_rt(cw_texture *rt, int w, int h, cw_filter f, bool depth) {
  if (CW_HANDLE_IS_NULL(*rt) || !r2d_tex_get(*rt)) *rt = r2d_texture_new_internal(w, h, f, depth);
  else cw_target_resize(*rt, w, h);
}
static void ensure_rt(cw_texture *rt, int w, int h, cw_filter f, bool depth) { r2d_ensure_rt(rt, w, h, f, depth); }

void cw__pixel_ui_begin(void); /* src/platform/pixel.c */

void cw_r2d_begin(const cw_r2d_pass_desc *desc) {
  if (!r2d_lazy_init()) return;
  CW_ASSERT(!R.in_pass, "cw_r2d_begin inside a pass: call cw_r2d_end first (passes do not nest)");
  r2d_stats_frame_check();
  R.pass = desc ? *desc : (cw_r2d_pass_desc){ 0 };
  R.in_pass = true;
  r2d_tex *t = CW_HANDLE_IS_NULL(R.pass.target) ? NULL : r2d_tex_get(R.pass.target);
  if (!CW_HANDLE_IS_NULL(R.pass.target) && (!t || !t->target)) {
    cw_error("cw_r2d_begin: .target is not a live render target (cw_target_create); drawing to the window");
    R.pass.target = (cw_texture){ 0 };
    t = NULL;
  }
  if (t) {
    R.target_w = t->w;
    R.target_h = t->h;
    R.target_fbo = t->fbo;
  } else {
    cw__pixel_ui_begin(); /* crisp UI: the UI's target from the first window pass on */
    cw_drawable_size(&R.target_w, &R.target_h);
    R.target_fbo = 0;
    R.win_w = R.target_w;
    R.win_h = R.target_h;
    R.win_virtual_w = R.pass.virtual_width > 0 && R.pass.virtual_height > 0 ? R.pass.virtual_width : 0;
    R.win_virtual_h = R.pass.virtual_width > 0 && R.pass.virtual_height > 0 ? R.pass.virtual_height : 0;
    int lw = 0, lh = 0;
    cw_window_size(&lw, &lh);
    R.win_px_x = lw > 0 && R.target_w > 0 ? (float)R.target_w / (float)lw : 1.0f;
    R.win_px_y = lh > 0 && R.target_h > 0 ? (float)R.target_h / (float)lh : 1.0f;
  }
  bool virt = !t && R.pass.virtual_width > 0 && R.pass.virtual_height > 0;
  if (virt) {
    R.canvas_w = R.pass.virtual_width;
    R.canvas_h = R.pass.virtual_height;
    float sx = (float)R.target_w / (float)R.canvas_w, sy = (float)R.target_h / (float)R.canvas_h;
    float s = CW_MIN(sx, sy);
    if (R.pass.scale == CW_SCALE_INTEGER) s = s >= 1 ? (float)(int)s : s;
    float w = R.pass.scale == CW_SCALE_STRETCH ? (float)R.target_w : (float)R.canvas_w * s;
    float h = R.pass.scale == CW_SCALE_STRETCH ? (float)R.target_h : (float)R.canvas_h * s;
    if (R.pass.scale == CW_SCALE_NONE) {
      w = (float)R.canvas_w;
      h = (float)R.canvas_h;
    }
    R.canvas_rect = (cw_rect){ (float)(int)(((float)R.target_w - w) * 0.5f), (float)(int)(((float)R.target_h - h) * 0.5f), w, h };
  } else {
    R.canvas_w = R.target_w;
    R.canvas_h = R.target_h;
    if (!t) R.canvas_rect = (cw_rect){ 0, 0, (float)R.target_w, (float)R.target_h };
  }
  /* the logical canvas draws address: a plain window pass is the window's
   * logical size, scaled to the drawable on the GPU */
  if (!t && !virt) {
    R.px_x = R.win_px_x;
    R.px_y = R.win_px_y;
    R.view_w = CW_MAX(1, (int)lroundf((float)R.canvas_w / R.px_x));
    R.view_h = CW_MAX(1, (int)lroundf((float)R.canvas_h / R.px_y));
  } else {
    R.px_x = R.px_y = 1;
    R.view_w = R.canvas_w;
    R.view_h = R.canvas_h;
  }
  /* draw state */
  R.cur_layer = 0;
  R.cur_z = 0;
  R.blend = CW_BLEND_ALPHA;
  R.has_xform = false;
  R.xform = cw_xform2d_identity();
  R.clip_depth = 0;
  R.cur_clip = -1;
  R.clips.count = 1;
  R.normals_used = false;
  cw_array_clear(&R.verts);
  cw_array_clear(&R.idx);
  cw_array_clear(&R.cmds);
  cw_array_clear(&R.tdraws);
  cw_array_clear(&R.lights);
  cw_array_clear(&R.occluders);
  cw_array_clear(&R.occ_pts);
  for (int l = 0; l < CW_R2D_MAX_LAYERS; l++) R.layer_view[l] = pass_view(l);
  R.stats.passes++;
}

cw_vec2 cw_r2d_canvas_size(void) { return (cw_vec2){ (float)R.view_w, (float)R.view_h }; }
cw_rect cw_r2d_canvas_rect(void) { return R.canvas_rect; }
float cw_r2d_pixel_scale(void) { return r2d_px(); }

/* both describe the last WINDOW pass: its rect (drawable pixels), its
 * canvas size and the window's pixel ratio (a render-target pass in between
 * changes none of them). Window coordinates are logical (the platform's). */
static void win_canvas(float *vw, float *vh) {
  cw_rect r = R.canvas_rect;
  float sx = R.win_px_x > 0 ? R.win_px_x : 1, sy = R.win_px_y > 0 ? R.win_px_y : 1;
  *vw = R.win_virtual_w > 0 ? (float)R.win_virtual_w : r.w / sx;
  *vh = R.win_virtual_h > 0 ? (float)R.win_virtual_h : r.h / sy;
}

cw_vec2 cw_r2d_window_to_canvas(cw_vec2 p) {
  cw_rect r = R.canvas_rect;
  if (r.w <= 0 || r.h <= 0) return p;
  float vw, vh;
  win_canvas(&vw, &vh);
  if (!R.win_virtual_w) return p; /* a plain window pass's canvas IS the logical window */
  float sx = R.win_px_x > 0 ? R.win_px_x : 1, sy = R.win_px_y > 0 ? R.win_px_y : 1;
  return (cw_vec2){ (p.x * sx - r.x) * vw / r.w, (p.y * sy - r.y) * vh / r.h };
}

cw_vec2 cw_r2d_canvas_to_window(cw_vec2 p) {
  cw_rect r = R.canvas_rect;
  float vw, vh;
  win_canvas(&vw, &vh);
  if (vw <= 0 || vh <= 0 || !R.win_virtual_w) return p;
  float sx = R.win_px_x > 0 ? R.win_px_x : 1, sy = R.win_px_y > 0 ? R.win_px_y : 1;
  return (cw_vec2){ (r.x + p.x * r.w / vw) / sx, (r.y + p.y * r.h / vh) / sy };
}

/* ── sort: stable LSD radix on the 64-bit keys, skipping constant bytes ── */

static bool sort_commands(void) {
  int n = R.cmds.count;
  const r2d_cmd *c = R.cmds.data;
  bool ordered = true;
  for (int i = 1; i < n && ordered; i++) ordered = c[i - 1].key <= c[i].key;
  cw_array_reserve(&R.order, n);
  R.order.count = n;
  if (ordered) {
    for (int i = 0; i < n; i++) R.order.data[i] = (uint32_t)i;
    return false;
  }
  /* (key, index) pairs in flat arrays: the scatter passes stream through
   * memory instead of chasing command records */
  cw_array_reserve(&R.order_tmp, n);
  cw_array_reserve(&R.keys, n);
  cw_array_reserve(&R.keys_tmp, n);
  static uint32_t hist[8][256];
  memset(hist, 0, sizeof hist);
  uint64_t *ks = R.keys.data, *kd = R.keys_tmp.data;
  uint32_t *src = R.order.data, *dst = R.order_tmp.data;
  for (int i = 0; i < n; i++) {
    uint64_t k = c[i].key;
    ks[i] = k;
    src[i] = (uint32_t)i;
    for (int b = 0; b < 8; b++) hist[b][(k >> (8 * b)) & 255]++;
  }
  for (int b = 0; b < 8; b++) {
    uint32_t *h = hist[b];
    if (h[(ks[0] >> (8 * b)) & 255] == (uint32_t)n) continue; /* every key shares this byte */
    uint32_t sum = 0;
    for (int k = 0; k < 256; k++) {
      uint32_t t = h[k];
      h[k] = sum;
      sum += t;
    }
    for (int i = 0; i < n; i++) {
      uint64_t k = ks[i];
      uint32_t o = h[(k >> (8 * b)) & 255]++;
      kd[o] = k;
      dst[o] = src[i];
    }
    uint32_t *t = src;
    src = dst;
    dst = t;
    uint64_t *tk = ks;
    ks = kd;
    kd = tk;
  }
  if (src != R.order.data) memcpy(R.order.data, src, sizeof *src * (size_t)n);
  return true;
}

/* ── drawing ───────────────────────────────────────────────────────────── */

static CW_ARRAY(r2d_batch) batches;

void r2d_upload_layer_uniforms(const r2d_program *p) {
  float u[CW_R2D_MAX_LAYERS * 8];
  for (int l = 0; l < CW_R2D_MAX_LAYERS; l++) {
    const cw_xform2d *m = &R.layer_view[l];
    float *r = u + l * 8;
    r[0] = m->a, r[1] = m->c, r[2] = m->tx, r[3] = R.layers[l].pixel_snap ? 1.0f : 0.0f;
    r[4] = m->b, r[5] = m->d, r[6] = m->ty, r[7] = 0;
  }
  glUniform4fv(p->u_layers, CW_R2D_MAX_LAYERS * 2, u);
  glUniform2f(p->u_target, (float)R.canvas_w, (float)R.canvas_h);
}

static void apply_blend(uint32_t b) {
  switch (b) {
  case CW_BLEND_OPAQUE:
    glDisable(GL_BLEND);
    break;
  case CW_BLEND_MULTIPLY:
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    break;
  default:
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    break;
  }
}

static void apply_clip(uint32_t clip, int fb_h) {
  if (!clip) {
    glDisable(GL_SCISSOR_TEST);
    return;
  }
  cw_rect c = r2d_clip_px(R.clips.data[clip]);
  int x0 = (int)(c.x + 0.5f), y0 = (int)(c.y + 0.5f), x1 = (int)(c.x + c.w + 0.5f), y1 = (int)(c.y + c.h + 0.5f);
  x0 = CW_MAX(x0, 0);
  y0 = CW_MAX(y0, 0);
  glEnable(GL_SCISSOR_TEST);
  glScissor(x0, fb_h - y1, CW_MAX(0, x1 - x0), CW_MAX(0, y1 - y0));
}

static void base_state(void) {
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* the framebuffer draws of the pass go to (the canvas) */
static GLuint canvas_fbo(void) {
  if (R.target_fbo == 0 && R.pass.virtual_width > 0 && R.pass.virtual_height > 0) {
    ensure_rt(&R.canvas_rt, R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, false);
    return r2d_tex_get(R.canvas_rt)->fbo;
  }
  return R.target_fbo;
}

static void begin_lit_run(void) {
  /* make (or resize) the targets BEFORE binding: creating one binds
   * framebuffer 0 when done, and attachments / draw buffers set after that
   * would land on the window's framebuffer */
  ensure_rt(&R.scene_rt, R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, false);
  if (R.normals_used) ensure_rt(&R.normal_rt, R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, false);
  r2d_tex *s = r2d_tex_get(R.scene_rt);
  glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
  if (R.normals_used) {
    GLuint ntex = R.arrays.data[r2d_tex_get(R.normal_rt)->array].tex;
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, ntex, 0, 0);
    GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, bufs);
  } else {
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, 0, 0, 0);
    GLenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, bufs);
  }
  glViewport(0, 0, R.canvas_w, R.canvas_h);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
}

static void end_lit_run(int lo, int hi, GLuint canvas) {
  /* back to one draw buffer on the scene framebuffer */
  r2d_tex *s = r2d_tex_get(R.scene_rt);
  glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
  GLenum one[1] = { GL_COLOR_ATTACHMENT0 };
  glDrawBuffers(1, one);
  glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, 0, 0, 0);
  r2d_lights_render(lo, hi);
  /* composite colour * light over the canvas */
  r2d_set_target_fbo(canvas, R.canvas_w, R.canvas_h);
  base_state();
  apply_blend(CW_BLEND_ALPHA);
  glUseProgram(R.composite.prog);
  glUniform2f(R.composite.u_target, (float)R.canvas_w, (float)R.canvas_h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, R.arrays.data[s->array].tex);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D_ARRAY, R.arrays.data[r2d_tex_get(R.light_rt)->array].tex);
  glActiveTexture(GL_TEXTURE0);
  r2d_draw_quad(0, 0, (float)R.canvas_w, (float)R.canvas_h, 0, 0, 1, 1);
}

static void bind_stream_program(bool lit, const r2d_program **cur) {
  const r2d_program *p = lit && R.normals_used ? &R.sprite_lit : &R.sprite;
  if (*cur == p) return;
  glUseProgram(p->prog);
  r2d_upload_layer_uniforms(p);
  int ring = R.ring;
  glBindVertexArray(R.vao[ring]);
  *cur = p;
}

static void gather_range(int begin, int end, int worker, void *ctx) {
  (void)worker;
  (void)ctx;
  for (int i = begin; i < end; i++) {
    const r2d_cmd *c = &R.scmds.data[i];
    uint32_t *d = R.out_idx.data + c->ifirst;
    const uint32_t *s = R.idx.data + c->src;
    if (c->icount == 6) for (int k = 0; k < 6; k++) d[k] = s[k];
    else memcpy(d, s, sizeof(uint32_t) * c->icount);
  }
}

void cw_r2d_end(void) {
  CW_PROFILE_SCOPE("r2d.flush");
  if (!R.inited) return;
  CW_ASSERT(R.in_pass, "cw_r2d_end without cw_r2d_begin");
  R.in_pass = false;
  /* layer views as of now (cameras may have moved during submission) */
  for (int l = 0; l < CW_R2D_MAX_LAYERS; l++) R.layer_view[l] = pass_view(l);
  R.stats.vertices += (uint32_t)R.verts.count;
  R.stats.indices += (uint32_t)R.idx.count;

  CW_PROFILE_BEGIN("r2d.sort_batch");
  bool sorted = sort_commands();
  if (sorted) R.stats.sorted++;
  const uint32_t *out = R.idx.data;
  if (sorted) {
    /* commands in sorted order (one random read each), then the indices
     * gathered in parallel: from here on everything walks memory in order */
    cw_array_reserve(&R.out_idx, R.idx.count);
    cw_array_reserve(&R.scmds, R.order.count);
    R.scmds.count = R.order.count;
    uint32_t o = 0;
    for (int i = 0; i < R.order.count; i++) {
      r2d_cmd c = R.cmds.data[R.order.data[i]];
      c.src = c.ifirst;
      c.ifirst = o;
      o += c.icount;
      R.scmds.data[i] = c;
    }
    R.out_idx.count = (int)o;
    cw_parallel_for(R.scmds.count, 4096, gather_range, NULL);
    out = R.out_idx.data;
  }
  const r2d_cmd *cmds = sorted ? R.scmds.data : R.cmds.data;

  /* batches: merge neighbours whose GL state agrees */
  cw_array_clear(&batches);
  for (int i = 0; i < R.cmds.count; i++) {
    const r2d_cmd *c = &cmds[i];
    int layer = (int)(c->key >> 56);
    bool lit = R.layers[layer].lit;
    uint32_t st = c->state;
    if (R2D_ST_KIND(st) == R2D_KIND_STREAM && batches.count) {
      r2d_batch *b = &batches.data[batches.count - 1];
      uint32_t ba = R2D_ST_ARRAY(b->state), ca = R2D_ST_ARRAY(st);
      if (R2D_ST_KIND(b->state) == R2D_KIND_STREAM && b->lit == lit && (b->state & ~0xfffu) == (st & ~0xfffu) &&
          (ba == ca || ba == R2D_NO_ARRAY || ca == R2D_NO_ARRAY) && b->first + b->count == c->ifirst) {
        b->count += c->icount;
        if (ba == R2D_NO_ARRAY) b->state = (b->state & ~0xfffu) | ca;
        continue;
      }
    }
    if (R2D_ST_KIND(st) == R2D_KIND_STREAM && c->icount == 0) continue;
    cw_array_push(&batches, ((r2d_batch){ st, c->ifirst, c->icount, c->payload, layer, lit }));
  }

  CW_PROFILE_END();
  if (r2d_wg()) { /* WebGPU: same batches, its own submission */
    r2d_wg_end(batches.data, batches.count, out);
    return;
  }

  /* upload the streams: glBufferData with the data orphans and fills in one
   * call (WebGL would zero-fill a NULL glBufferData first) */
  CW_PROFILE_BEGIN("r2d.upload");
  R.ring = (R.ring + 1) % R2D_RING;
  int ring = R.ring;
  glBindVertexArray(R.vao[ring]);
  if (R.verts.count) {
    glBindBuffer(GL_ARRAY_BUFFER, R.vbo[ring]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(r2d_vertex) * R.verts.count, R.verts.data, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, R.ibo[ring]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(uint32_t) * R.idx.count, out, GL_STREAM_DRAW);
  }
  glBindVertexArray(0);
  CW_PROFILE_END();

  /* target */
  GLuint canvas = canvas_fbo();
  r2d_set_target_fbo(canvas, R.canvas_w, R.canvas_h);
  base_state();
  if (R.pass.clear) {
    cw_color c = R.pass.clear_color;
    glClearColor(c.r * c.a, c.g * c.a, c.b * c.a, c.a);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  const r2d_program *prog = NULL;
  bool in_lit = false;
  int lit_lo = 0, lit_hi = 0;
  uint32_t cur_array = 0xffffffffu, cur_blend = 0xffffffffu, cur_clip = 0xffffffffu;
  for (int i = 0; i < batches.count; i++) {
    const r2d_batch *b = &batches.data[i];
    if (b->lit && !in_lit) {
      begin_lit_run();
      in_lit = true;
      lit_lo = b->layer;
      prog = NULL;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
    } else if (!b->lit && in_lit) {
      end_lit_run(lit_lo, lit_hi, canvas);
      in_lit = false;
      prog = NULL;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
    }
    if (in_lit) lit_hi = b->layer;
    uint32_t blend = R2D_ST_BLEND(b->state), clip = R2D_ST_CLIP(b->state);
    if (blend != cur_blend) apply_blend(cur_blend = blend);
    if (clip != cur_clip) apply_clip(cur_clip = clip, R.canvas_h);
    if (R2D_ST_KIND(b->state) == R2D_KIND_TILEMAP) {
      r2d_tilemap_render(&R.tdraws.data[b->payload], in_lit && R.normals_used, R.canvas_h);
      prog = NULL;
      cur_array = 0xffffffffu;
      continue;
    }
    bind_stream_program(in_lit, &prog);
    uint32_t arr = R2D_ST_ARRAY(b->state);
    if (arr != cur_array) {
      cur_array = arr;
      GLuint tex = arr == R2D_NO_ARRAY ? R.dummy_normal : R.arrays.data[arr].tex;
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
      if (prog == &R.sprite_lit) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr == R2D_NO_ARRAY ? R.dummy_normal : r2d_array_normal_tex((int)arr));
        glActiveTexture(GL_TEXTURE0);
      }
    }
    glDrawElements(GL_TRIANGLES, (GLsizei)b->count, GL_UNSIGNED_INT, (void *)((size_t)b->first * 4));
    R.stats.draw_calls++;
    R.stats.batches++;
  }
  if (in_lit) end_lit_run(lit_lo, lit_hi, canvas);

  /* virtual canvas -> window */
  if (canvas != R.target_fbo) {
    r2d_set_target_fbo(R.target_fbo, R.target_w, R.target_h);
    base_state();
    cw_rect r = R.canvas_rect;
    bool covers = r.x <= 0 && r.y <= 0 && r.x + r.w >= (float)R.target_w && r.y + r.h >= (float)R.target_h;
    if (!covers) {
      cw_color c = R.pass.letterbox;
      glClearColor(c.r, c.g, c.b, 1);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_BLEND);
    glUseProgram(R.blit.prog);
    glUniform2f(R.blit.u_target, (float)R.target_w, (float)R.target_h);
    glUniform1f(R.blit.u[U_PAGE], 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, R.arrays.data[r2d_tex_get(R.canvas_rt)->array].tex);
    r2d_draw_quad(r.x, r.y, r.x + r.w, r.y + r.h, 0, 1, 1, 0); /* the canvas is y-up */
  }
  glBindVertexArray(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glActiveTexture(GL_TEXTURE0);
}
