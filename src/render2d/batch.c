/* render2d core: module state, passes, canvas layers, the submission stream,
 * the stable sort, batching and drawing. */
#include <string.h>

#include "internal.h"
#include "fx.h"
#include "cartwheel/debug.h"
#include "cartwheel/jobs.h"
#include "cartwheel/platform.h"
#include "cartwheel/profile.h"
#include "cartwheel/quality.h"

r2d_state R = { .atlas_array = { -1, -1 }, .atlas_mip = -1 };

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
  /* depth culling's per-vertex depth (cull.c): on in a pass that culls, else the constant 0 */
  glBindBuffer(GL_ARRAY_BUFFER, R.dvbo[i]);
  glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, 4, (void *)0);
  glDisableVertexAttribArray(5);
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
  R.atlas_mip = -1;
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
  R.atlas_mip = -1;
  GLint ml = 256;
  glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &ml);
  R.max_layers = CW_CLAMP(ml, 1, 256);
  glGenBuffers(R2D_RING, R.vbo);
  glGenBuffers(R2D_RING, R.ibo);
  glGenBuffers(R2D_RING, R.dvbo);
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
  /* the "no material" array: no emission, no specular */
  glGenTextures(1, &R.dummy_material);
  glBindTexture(GL_TEXTURE_2D_ARRAY, R.dummy_material);
  static const uint8_t nomat[4] = { 0, 0, 0, 0 };
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nomat);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
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
  r2d_lights_shutdown();
  r2d_instances_shutdown();
  r2d_cull_shutdown();
  r2d_gi_shutdown();
  r2d_post_shutdown();
  fx_shutdown();
  r2d_textures_shutdown();
  if (r2d_wg()) r2d_wg_shutdown();
  else {
    r2d_program *ps[] = { &R.sprite_pre, &R.tile_pre, &R.inst, &R.inst_lit, &R.part, &R.part_lit, &R.sprite_mask, &R.tile_mask, &R.sprite_distort, &R.sprite, &R.sprite_lit, &R.tile, &R.tile_lit, &R.light, &R.shadow, &R.composite, &R.blit };
  for (size_t i = 0; i < CW_ARRAY_LEN(ps); i++)
    if (ps[i]->prog) glDeleteProgram(ps[i]->prog);
  r2d_yuv_gl_shutdown();
  glDeleteBuffers(R2D_RING, R.vbo);
  glDeleteBuffers(R2D_RING, R.ibo);
  glDeleteBuffers(R2D_RING, R.dvbo);
  if (R.cull_rb) glDeleteRenderbuffers(1, &R.cull_rb);
  glDeleteVertexArrays(R2D_RING, R.vao);
  glDeleteBuffers(1, &R.quad_vbo);
  glDeleteVertexArrays(1, &R.quad_vao);
  if (R.tile_ibo) glDeleteBuffers(1, &R.tile_ibo);
  }
  cw_array_free(&R.verts);
  cw_array_free(&R.idx);
  cw_array_free(&R.cmds);
  cw_array_free(&R.tdraws);
  cw_array_free(&R.pdraws);
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
  cw_array_free(&R.occ_batches);
  memset(&R, 0, sizeof R);
  R.atlas_array[0] = R.atlas_array[1] = -1;
  R.atlas_mip = -1;
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
    F(shape_cache_hits), F(gi_cascades), F(particles),
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
    r2d_text_frame_reset();
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
void cw_r2d_set_intensity(float k) {
  float x = (CW_CLAMP(k, 1.0f, 16.0f) - 1.0f) / 15.0f * 255.0f;
  R.intensity_b = (uint8_t)(x + 0.5f);
}
void cw_r2d_set_emissive(float e) { R.emissive_b = (uint8_t)(CW_CLAMP(e, 0.0f, 1.0f) * 255.0f + 0.5f); }
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
  uint32_t occ = R.occluder_on && R.layers[R.cur_layer].lit ? R2D_ST_OCCL : 0;
  return R2D_STATE(array, b, current_clip(), R2D_KIND_STREAM) | occ | (R.next_opq && R.blend == CW_BLEND_ALPHA ? R2D_ST_OPQ : 0);
}

void cw_r2d_set_occluder(bool on) { R.occluder_on = on; }

void r2d_push_cmd(uint64_t key, uint32_t state, uint32_t ifirst, uint32_t icount) {
  /* extend the previous command when nothing about it differs: keeps the
   * command list (and the sort) short when a layer is not y-sorted */
  if (R.cmds.count) {
    r2d_cmd *p = &R.cmds.data[R.cmds.count - 1];
    if (p->key == key && (p->state & ~R2D_ST_OPQ) == (state & ~R2D_ST_OPQ) && p->ifirst + p->icount == ifirst && R2D_ST_KIND(state) == R2D_KIND_STREAM) {
      p->state &= state | ~R2D_ST_OPQ; /* opaque only while every merged draw is */
      p->icount += icount;
      return;
    }
  }
  cw_array_push(&R.cmds, ((r2d_cmd){ key, state, ifirst, icount, 0, 0 }));
  R.stats.commands++;
}

void r2d_note_tex(const r2d_tex *t) {
  if (!t || !R.layers[R.cur_layer].lit) return;
  if (t->has_normal || t->has_material) R.normals_used = true;
  if (t->has_material) R.material_used = true;
}

r2d_vertex *r2d_alloc(int nv, int ni, uint32_t **idx_out, uint32_t *base_out, uint32_t array, float sort_y,
                      int extra_z) {
  CW_ASSERT(R.in_pass, "submit between cw_r2d_begin and cw_r2d_end");
  int v0 = R.verts.count, i0 = R.idx.count;
  if (R.emissive_b && R.layers[R.cur_layer].lit) R.normals_used = R.material_used = true;
  cw_array_reserve(&R.verts, v0 + nv);
  cw_array_reserve(&R.idx, i0 + ni);
  R.verts.count += nv;
  R.idx.count += ni;
  *idx_out = R.idx.data + i0;
  *base_out = (uint32_t)v0;
  r2d_push_cmd(r2d_make_key(extra_z, sort_y), r2d_current_state(array), (uint32_t)i0, (uint32_t)ni);
  return R.verts.data + v0;
}

/* ── GPU particles ───────────────────────────────────────────────────── */

void cw_r2d_particles(const cw_r2d_particles_desc *d) {
  CW_ASSERT(R.in_pass, "submit particles between cw_r2d_begin and cw_r2d_end");
  if (!d || d->count <= 0) return;
  const r2d_tex *t = CW_HANDLE_IS_NULL(d->texture) ? NULL : r2d_tex_get(d->texture);
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  uint8_t mode = r2d_mode(t == NULL);
  if (t) {
    cw_rect src = d->src.w > 0 ? d->src : (cw_rect){ 0, 0, (float)t->w, (float)t->h };
    r2d_tex_uv(t, src, &u0, &v0, &u1, &v1);
  }
  cw_vec2 pos = R.has_xform ? cw_xform2d_apply(&R.xform, d->pos) : d->pos;
  cw_color c0 = r2d_default_white(d->color_start);
  cw_color c1 = (d->color_end.r == 0 && d->color_end.g == 0 && d->color_end.b == 0 && d->color_end.a == 0)
                    ? (cw_color){ c0.r, c0.g, c0.b, 0 } : d->color_end;
  float lmin = d->life_min > 0 ? d->life_min : 1, lmax = d->life_max > 0 ? d->life_max : lmin;
  /* the effects detail's share (cartwheel/quality.h "particles": all on High and Ultra); the first k of a
   * stateless emitter are the same k particles at every share */
  r2d_part_draw p = { .count = cw_quality_count(d->count, "particles"), .array = t ? t->array : R2D_NO_ARRAY };
  float v[9][4] = { { pos.x, pos.y, d->extent.x, d->extent.y },
                    { d->angle, d->spread, d->speed_min, d->speed_max > 0 ? d->speed_max : d->speed_min },
                    { d->gravity.x, d->gravity.y, d->drag, d->time != 0 ? d->time : R.pass.time },
                    { lmin, CW_MAX(lmin, lmax), d->size_start > 0 ? d->size_start : 8, d->size_end },
                    { c0.r, c0.g, c0.b, c0.a },
                    { c1.r, c1.g, c1.b, c1.a },
                    { u0, v0, u1, v1 },
                    { t ? (float)t->page : 0, d->spin, d->turbulence, (float)(d->seed & 0xffffffu) },
                    { (float)R.cur_layer, (float)mode, 1.0f + (float)R.intensity_b * 15.0f / 255.0f, (float)R.emissive_b / 255.0f } };
  memcpy(p.u, v, sizeof v);
  r2d_note_tex(t);
  if (R.emissive_b && R.layers[R.cur_layer].lit) R.normals_used = R.material_used = true;
  cw_array_push(&R.pdraws, p);
  cw_blend b = R.blend == CW_BLEND_ADD ? CW_BLEND_ALPHA : R.blend;
  cw_array_push(&R.cmds, ((r2d_cmd){ r2d_make_key(0, pos.y), R2D_STATE(p.array, b, current_clip(), R2D_KIND_PARTICLES),
                                     (uint32_t)R.idx.count, 0, (uint32_t)(R.pdraws.count - 1) }));
  R.stats.commands++;
  R.stats.particles += (uint32_t)p.count;
}

void r2d_particles_gl(const r2d_part_draw *d, bool lit) {
  const r2d_program *p = lit ? &R.part_lit : &R.part;
  glUseProgram(p->prog);
  r2d_upload_layer_uniforms(p);
  glUniform4fv(p->u[0], 9, &d->u[0][0]);
  uint32_t arr = d->array;
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, arr == R2D_NO_ARRAY ? R.dummy_normal : R.arrays.data[arr].tex);
  if (lit) {
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr == R2D_NO_ARRAY ? R.dummy_normal : r2d_array_normal_tex((int)arr));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arr == R2D_NO_ARRAY ? R.dummy_material : r2d_array_material_tex((int)arr));
    glActiveTexture(GL_TEXTURE0);
  }
  if (!R.empty_vao) glGenVertexArrays(1, &R.empty_vao);
  glBindVertexArray(R.empty_vao);
  glDrawArraysInstanced(GL_TRIANGLES, 0, 6, d->count);
  glBindVertexArray(0);
  R.stats.draw_calls++;
}

void r2d_push_special(uint32_t array, float sort_y, uint32_t payload) {
  CW_ASSERT(R.in_pass, "submit between cw_r2d_begin and cw_r2d_end");
  cw_blend b = R.blend == CW_BLEND_ADD ? CW_BLEND_ALPHA : R.blend;
  uint32_t occ = R.occluder_on && R.layers[R.cur_layer].lit ? R2D_ST_OCCL : 0;
  cw_array_push(&R.cmds, ((r2d_cmd){ r2d_make_key(0, sort_y), R2D_STATE(array, b, current_clip(), R2D_KIND_TILEMAP) | occ,
                                     (uint32_t)R.idx.count, 0, payload }));
  R.stats.commands++;
}

void r2d_push_special_kind(uint32_t array, float sort_y, uint32_t payload, int kind, int blend) {
  cw_array_push(&R.cmds, ((r2d_cmd){ r2d_make_key(0, sort_y), R2D_STATE(array, (uint32_t)blend, current_clip(), (uint32_t)kind),
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
    if (R.pass.scale == CW_SCALE_EXPAND) { /* FIT's scale, the whole target: more canvas, no bars */
      R.canvas_w = CW_MAX(1, (int)lroundf((float)R.target_w / s));
      R.canvas_h = CW_MAX(1, (int)lroundf((float)R.target_h / s));
      w = (float)R.target_w;
      h = (float)R.target_h;
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
  if (!t) R.win_view_h = R.view_h;
  /* draw state */
  R.cur_layer = 0;
  R.cur_z = 0;
  R.has_post = desc && desc->post;
  if (R.has_post) R.post = *desc->post;
  R.has_gi = desc && desc->gi;
  if (R.has_gi) R.gi = *desc->gi;
  R.intensity_b = R.emissive_b = R.sway_b = R.flash_b = 0;
  R.occluder_on = false;
  R.blend = CW_BLEND_ALPHA;
  R.has_xform = false;
  R.xform = cw_xform2d_identity();
  R.clip_depth = 0;
  R.cur_clip = -1;
  R.clips.count = 1;
  R.normals_used = false;
  R.material_used = false;
  cw_array_clear(&R.verts);
  cw_array_clear(&R.idx);
  cw_array_clear(&R.cmds);
  cw_array_clear(&R.tdraws);
  cw_array_clear(&R.pdraws);
  cw_array_clear(&R.idraws);
  cw_array_clear(&R.istage);
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
    r[4] = m->b, r[5] = m->d, r[6] = m->ty, r[7] = R.layers[l].distortion * r2d_px();
  }
  glUniform4fv(p->u_layers, CW_R2D_MAX_LAYERS * 2, u);
  glUniform2f(p->u_target, (float)R.canvas_w, (float)R.canvas_h);
  GLint uf = glGetUniformLocation(p->prog, "u_fog");
  if (uf >= 0) {
    float f[CW_R2D_MAX_LAYERS * 4];
    r2d_fog_uniform(f);
    glUniform4fv(uf, CW_R2D_MAX_LAYERS, f);
  }
  GLint uz = glGetUniformLocation(p->prog, "u_zbias");
  if (uz >= 0) glUniform1f(uz, R.zbias);
  GLint uw = glGetUniformLocation(p->prog, "u_wind");
  if (uw >= 0) {
    float w[4];
    r2d_wind_uniform(w);
    glUniform4fv(uw, 1, w);
  }
}

/* the pass's wind for the sprite vertex stage: sway px, phase, spatial frequency, gustiness */
void r2d_wind_uniform(float out[4]) {
  const cw_r2d_wind *w = &R.pass.wind;
  float speed = w->speed > 0 ? w->speed : 1.5f, wl = w->wavelength > 0 ? w->wavelength : 300.0f;
  out[0] = w->strength * r2d_px();
  out[1] = R.pass.time * speed * 6.2831853f;
  out[2] = 6.2831853f / wl;
  out[3] = w->gusts > 0 ? w->gusts : 0.0f;
}
/* every canvas layer's fog for the sprite stage: rgb, amount */
void r2d_fog_uniform(float out[CW_R2D_MAX_LAYERS * 4]) {
  for (int l = 0; l < CW_R2D_MAX_LAYERS; l++) {
    cw_color f = R.layers[l].fog;
    bool on = f.a > 0 && !R.layers[l].lit;
    out[4 * l] = f.r, out[4 * l + 1] = f.g, out[4 * l + 2] = f.b, out[4 * l + 3] = on ? CW_MIN(f.a, 1.0f) : 0.0f;
  }
}
void cw_r2d_set_flash(float amount) { R.flash_b = (uint8_t)(CW_CLAMP(amount, 0.0f, 1.0f) * 255.0f + 0.5f); }
void cw_r2d_set_sway(float share) { R.sway_b = (uint8_t)(CW_CLAMP(share, 0.0f, 1.0f) * 255.0f + 0.5f); }

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
  cw_array_clear(&R.occ_batches);
  r2d_lit_begin_gl();
}

/* the run's lights and composite onto the framebuffer `dst` (the canvas or the HDR canvas) */
static void end_lit_run(int lo, int hi, GLuint dst) {
  fx_dst d = { .w = R.canvas_w, .h = R.canvas_h, .fbo = dst, .offscreen = dst != 0 };
  r2d_timer_end();
  r2d_lit_end(lo, hi, &d);
  r2d_timer_begin(R2D_T_DRAW);
  r2d_set_target_fbo(dst, R.canvas_w, R.canvas_h);
  base_state();
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

/* two layers whose draws can share a batch: the same target (not across a
 * distortion or screen-reading layer, nor across the post chain's hud layer) */
static bool same_target(int a, int b) {
  if (a == b) return true;
  const cw_r2d_layer_desc *la = &R.layers[a], *lb = &R.layers[b];
  if (la->distortion > 0 || lb->distortion > 0 || la->screen_read || lb->screen_read) return false;
  int hud = R.has_post && R.post.hud_layer > 0 ? R.post.hud_layer : CW_R2D_MAX_LAYERS;
  return (a < hud) == (b < hud);
}

/* depth culling (cull.c), GL: the depth buffer, alone in a framebuffer for
 * the pre-pass and attached to the HDR canvas and a lit run's buffers */
static GLuint cull_fbo;
static void cull_rb_gl(void) {
  if (R.cull_rb && R.cull_w == R.canvas_w && R.cull_h == R.canvas_h) return;
  if (!R.cull_rb) glGenRenderbuffers(1, &R.cull_rb);
  glBindRenderbuffer(GL_RENDERBUFFER, R.cull_rb);
  glRenderbufferStorage(GL_RENDERBUFFER, 0x8CAC /* GL_DEPTH_COMPONENT32F */, R.canvas_w, R.canvas_h);
  R.cull_w = R.canvas_w, R.cull_h = R.canvas_h;
  if (!cull_fbo) glGenFramebuffers(1, &cull_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, cull_fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.cull_rb);
}
void r2d_cull_attach_gl(GLuint fbo) { /* (fbo bound) the depth buffer while a group culls, none otherwise */
  if (fbo) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, R.cull_on ? R.cull_rb : 0);
}
static void apply_clip(uint32_t clip, int fb_h);
/* the group's opaque texels, nearest first, into the depth buffer (colour off) */
static void cull_prepass_gl(const r2d_cull_plan *cp, const r2d_batch *b, int from, int n, int g) {
  cull_rb_gl();
  glBindFramebuffer(GL_FRAMEBUFFER, cull_fbo);
  glViewport(0, 0, R.canvas_w, R.canvas_h);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepthf(1.0f);
  glClear(GL_DEPTH_BUFFER_BIT);
  int last = from;
  while (last < n && cp->grp[last] == g) last++;
  bool bound = false;
  uint32_t cur_array = 0xffffffffu, cur_clip = 0xffffffffu;
  for (int i = last - 1; i >= from; i--) {
    if (!cp->occ[i]) continue;
    uint32_t clip = R2D_ST_CLIP(b[i].state);
    if (clip != cur_clip) apply_clip(cur_clip = clip, R.canvas_h);
    if (R2D_ST_KIND(b[i].state) == R2D_KIND_TILEMAP) {
      R.depth_mode = 1;
      R.tile_depth = r2d_cull_depth_t(2.0f * (float)(b[i].first / 3));
      r2d_tilemap_render(&R.tdraws.data[b[i].payload], false, R.canvas_h);
      R.depth_mode = 0;
      bound = false;
      cur_array = 0xffffffffu;
      continue;
    }
    if (!bound) {
      float zb = R.zbias;
      R.zbias = 0; /* the pre-pass writes each draw's own depth */
      glUseProgram(R.sprite_pre.prog);
      r2d_upload_layer_uniforms(&R.sprite_pre);
      R.zbias = zb;
      glBindVertexArray(R.vao[R.ring]);
      bound = true;
    }
    uint32_t arr = R2D_ST_ARRAY(b[i].state);
    if (arr != cur_array) {
      cur_array = arr;
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D_ARRAY, arr == R2D_NO_ARRAY ? R.dummy_normal : R.arrays.data[arr].tex);
    }
    glDrawElements(GL_TRIANGLES, (GLsizei)b[i].count, GL_UNSIGNED_INT, (void *)((size_t)b[i].first * 4));
    R.stats.draw_calls++;
  }
  glBindVertexArray(0);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_FALSE);
  glDisable(GL_SCISSOR_TEST);
}

/* the post chain from the HDR canvas onto the canvas, then back to drawing there */
static void post_to(GLuint canvas) {
  fx_dst d = { .w = R.canvas_w, .h = R.canvas_h, .fbo = canvas, .offscreen = canvas != 0 };
  r2d_timer_end();
  r2d_timer_begin(R2D_T_POST);
  r2d_post_run(&d);
  r2d_timer_end();
  r2d_timer_begin(R2D_T_DRAW);
  r2d_set_target_fbo(canvas, R.canvas_w, R.canvas_h);
  base_state();
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
    bool lit = R.layers[layer].lit && !(R.layers[layer].distortion > 0);
    uint32_t st = c->state;
    if (R2D_ST_KIND(st) == R2D_KIND_STREAM && batches.count) {
      r2d_batch *b = &batches.data[batches.count - 1];
      uint32_t ba = R2D_ST_ARRAY(b->state), ca = R2D_ST_ARRAY(st);
      if (R2D_ST_KIND(b->state) == R2D_KIND_STREAM && b->lit == lit && same_target(b->layer, layer) && (b->state & ~0xfffu & ~R2D_ST_OPQ) == (st & ~0xfffu & ~R2D_ST_OPQ) &&
          (ba == ca || ba == R2D_NO_ARRAY || ca == R2D_NO_ARRAY) && b->first + b->count == c->ifirst) {
        b->count += c->icount;
        b->layer_last = layer;
        b->opq = b->opq && (st & R2D_ST_OPQ);
        if (ba == R2D_NO_ARRAY) b->state = (b->state & ~0xfffu) | ca;
        continue;
      }
    }
    if (R2D_ST_KIND(st) == R2D_KIND_STREAM && c->icount == 0) continue;
    cw_array_push(&batches, ((r2d_batch){ st, c->ifirst, c->icount, c->payload, layer, layer, lit, (st & R2D_ST_OPQ) != 0 }));
  }

  CW_PROFILE_END();
  /* a layer that reads or bends the screen makes the pass draw offscreen */
  R.offscreen = R.screen_reads = false;
  {
    int hud = R.has_post && R.post.hud_layer > 0 ? R.post.hud_layer : CW_R2D_MAX_LAYERS;
    for (int i = 0; i < batches.count; i++) {
      const cw_r2d_layer_desc *ld = &R.layers[batches.data[i].layer];
      if (batches.data[i].layer < hud && (ld->screen_read || ld->distortion > 0)) R.offscreen = true;
      if (batches.data[i].layer < hud && ld->screen_read) R.screen_reads = true;
    }
  }
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
  r2d_instances_upload_gl();
  /* depth culling (cull.c): the plan, and each vertex's depth when some group culls */
  bool hdr_on = r2d_post_active();
  const r2d_cull_plan *cp = r2d_cull_build(batches.data, batches.count, out, R.idx.count, hdr_on, hdr_on ? r2d_post_hud_layer() : CW_R2D_MAX_LAYERS);
  R.zbias = cp->bias;
  R.cull_on = false;
  glBindVertexArray(R.vao[ring]);
  if (cp->any) {
    glBindBuffer(GL_ARRAY_BUFFER, R.dvbo[ring]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(float) * R.verts.count, cp->vdepth, GL_STREAM_DRAW);
    glEnableVertexAttribArray(5);
  } else {
    glDisableVertexAttribArray(5);
  }
  glBindVertexArray(0);
  glVertexAttrib4fv(5, (const float[4]){ 0, 0, 0, 1 });
  CW_PROFILE_END();

  /* target */
  r2d_timers_frame();
  r2d_timer_begin(R2D_T_DRAW);
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
  /* HDR: layers under the chain's hud layer draw into the HDR canvas, which
   * the chain then puts on the canvas (fx.h, post.c) */
  bool in_hdr = r2d_post_active();
  int hud = in_hdr ? r2d_post_hud_layer() : CW_R2D_MAX_LAYERS;
  GLuint cur = canvas;
  if (in_hdr) {
    r2d_post_begin_gl();
    cur = r2d_post_fbo();
    base_state();
  }
  int prev_layer = -1;
  bool in_dist = false;
  int cur_g = -1;
  for (int i = 0; i < batches.count; i++) {
    const r2d_batch *b = &batches.data[i];
    int g = cp->any ? cp->grp[i] : -1;
    if (in_hdr && b->layer >= hud) {
      if (in_lit) end_lit_run(lit_lo, lit_hi, cur);
      in_lit = false;
      if (in_dist) r2d_set_target_fbo(cur, R.canvas_w, R.canvas_h);
      in_dist = false;
      post_to(canvas);
      in_hdr = false;
      cur = canvas;
      prog = NULL;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
    }
    if (b->layer != prev_layer) { /* a new layer: leave a distortion layer, read the screen, enter one */
      const cw_r2d_layer_desc *ld = &R.layers[b->layer];
      bool dist = in_hdr && ld->distortion > 0 && r2d_dist_ok();
      if (in_dist && !dist) {
        /* what the distortion layers bent is the canvas below them: bend it now, so the layers above stay straight */
        if (r2d_post_dist_resolve()) cur = r2d_post_fbo();
        r2d_set_target_fbo(cur, R.canvas_w, R.canvas_h);
        base_state();
        in_dist = false;
        prog = NULL;
        cur_array = cur_blend = cur_clip = 0xffffffffu;
      }
      if (in_hdr && ld->screen_read) {
        if (in_lit) end_lit_run(lit_lo, lit_hi, cur);
        in_lit = false;
        r2d_screen_copy_gl(cur);
        base_state();
        prog = NULL;
        cur_array = cur_blend = cur_clip = 0xffffffffu;
      }
      if (dist && !in_dist) {
        if (in_lit) end_lit_run(lit_lo, lit_hi, cur);
        in_lit = false;
        r2d_post_dist_begin_gl();
        base_state();
        in_dist = true;
        prog = NULL;
        cur_array = cur_blend = cur_clip = 0xffffffffu;
      }
      prev_layer = b->layer;
    }
    if (!in_dist && in_hdr && R.layers[b->layer].distortion > 0) continue; /* no float targets: nothing bends */
    if (in_dist) {
      if (R2D_ST_KIND(b->state) != R2D_KIND_STREAM) continue;
      uint32_t clip = R2D_ST_CLIP(b->state);
      if (clip != cur_clip) apply_clip(cur_clip = clip, R.canvas_h);
      if (prog != &R.sprite_distort) {
        glUseProgram(R.sprite_distort.prog);
        r2d_upload_layer_uniforms(&R.sprite_distort);
        glBindVertexArray(R.vao[R.ring]);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
        prog = &R.sprite_distort;
        cur_blend = 0xfffffffeu;
      }
      uint32_t arr = R2D_ST_ARRAY(b->state);
      if (arr != cur_array) {
        cur_array = arr;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr == R2D_NO_ARRAY ? R.dummy_normal : R.arrays.data[arr].tex);
      }
      glDrawElements(GL_TRIANGLES, (GLsizei)b->count, GL_UNSIGNED_INT, (void *)((size_t)b->first * 4));
      R.stats.draw_calls++;
      R.stats.batches++;
      continue;
    }
    if (cp->any && g != cur_g) { /* a new culling group (cull.c) */
      if (in_lit) end_lit_run(lit_lo, lit_hi, cur); /* a lit run ends its group */
      in_lit = false;
      if (R.cull_on) glDisable(GL_DEPTH_TEST);
      R.cull_on = false;
      cur_g = g;
      if (g >= 0 && cp->gcull[g]) {
        r2d_timer_end();
        r2d_timer_begin(R2D_T_CULL);
        cull_prepass_gl(cp, batches.data, i, batches.count, g);
        r2d_timer_end();
        r2d_timer_begin(R2D_T_DRAW);
        R.cull_on = true;
        r2d_set_target_fbo(cur, R.canvas_w, R.canvas_h);
        r2d_cull_attach_gl(cur);
        base_state();
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
      }
      prog = NULL;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
    }
    if (b->lit && !in_lit) {
      begin_lit_run();
      if (R.cull_on) glEnable(GL_DEPTH_TEST);
      in_lit = true;
      lit_lo = b->layer;
      prog = NULL;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
    } else if (!b->lit && in_lit) {
      end_lit_run(lit_lo, lit_hi, cur);
      in_lit = false;
      prog = NULL;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
    }
    if (in_lit) lit_hi = b->layer_last;
    if (in_lit && (b->state & R2D_ST_OCCL)) cw_array_push(&R.occ_batches, *b);
    uint32_t blend = R2D_ST_BLEND(b->state), clip = R2D_ST_CLIP(b->state);
    if (blend != cur_blend) apply_blend(cur_blend = blend);
    if (clip != cur_clip) apply_clip(cur_clip = clip, R.canvas_h);
    if (R2D_ST_KIND(b->state) == R2D_KIND_TILEMAP) {
      R.tile_depth = R.cull_on ? r2d_cull_depth_t(2.0f * (float)(b->first / 3)) - R.zbias : 0;
      r2d_tilemap_render(&R.tdraws.data[b->payload], in_lit && R.normals_used, R.canvas_h);
      prog = NULL;
      cur_array = 0xffffffffu;
      continue;
    }
    if (R2D_ST_KIND(b->state) == R2D_KIND_PARTICLES) {
      r2d_particles_gl(&R.pdraws.data[b->payload], in_lit && R.normals_used);
      prog = NULL;
      cur_array = 0xffffffffu;
      continue;
    }
    if (R2D_ST_KIND(b->state) == R2D_KIND_INSTANCES) {
      r2d_instances_gl(&R.idraws.data[b->payload], in_lit && R.normals_used);
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
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr == R2D_NO_ARRAY ? R.dummy_material : r2d_array_material_tex((int)arr));
        glActiveTexture(GL_TEXTURE0);
      }
    }
    glDrawElements(GL_TRIANGLES, (GLsizei)b->count, GL_UNSIGNED_INT, (void *)((size_t)b->first * 4));
    R.stats.draw_calls++;
    R.stats.batches++;
  }
  if (in_lit) end_lit_run(lit_lo, lit_hi, cur);
  if (in_dist) r2d_set_target_fbo(cur, R.canvas_w, R.canvas_h);
  if (R.cull_on) {
    glDisable(GL_DEPTH_TEST);
    R.cull_on = false;
  }
  if (in_hdr) post_to(canvas);
  r2d_timer_end();

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
    cw_scale_filter sf = R.pass.scale_filter;
    glUniform4f(R.blit.u[U_PAGE + 1], sf == CW_SCALE_FILTER_SHARP ? 1.0f : 0.0f, (float)R.canvas_w, (float)R.canvas_h,
                r.w / (float)CW_MAX(1, R.canvas_w));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, R.arrays.data[r2d_tex_get(R.canvas_rt)->array].tex);
    GLint flt = sf == CW_SCALE_FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, flt);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, flt);
    r2d_draw_quad(r.x, r.y, r.x + r.w, r.y + r.h, 0, 1, 1, 0); /* the canvas is y-up */
  }
  glBindVertexArray(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glActiveTexture(GL_TEXTURE0);
}
