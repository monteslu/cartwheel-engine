/* render2d sprite instances (cw_r2d_instances): one instanced draw of many
 * sprites of one texture. The CPU writes 24 bytes an instance (or nothing,
 * for a kept buffer); the vertex stage builds each quad (gl.c INST_VS, wgpu.c
 * vs_instances) and the sprites' fragment stage shades it. */
#include <string.h>

#include "internal.h"

CW_STATIC_ASSERT(sizeof(cw_sprite_instance) == 24, "an instance is 24 bytes");

typedef struct r2d_ibuf {
  bool live;
  uint32_t gen;
  int cap, count;
  GLuint vbo;
  void *wbuf;
} r2d_ibuf;
#define MAX_IBUFS 64
static r2d_ibuf IB[MAX_IBUFS];
static GLuint stream_vbo, inst_vao;
static size_t stream_cap;
static void *stream_wbuf;
static size_t stream_wcap;

static r2d_ibuf *ibuf_get(cw_r2d_instance_buffer h) {
  uint32_t i = CW_HANDLE_INDEX(h);
  if (CW_HANDLE_IS_NULL(h) || i < 1 || i > MAX_IBUFS) return NULL;
  r2d_ibuf *b = &IB[i - 1];
  return b->live && b->gen == CW_HANDLE_GEN(h) ? b : NULL;
}

cw_r2d_instance_buffer cw_r2d_instance_buffer_create(int capacity) {
  if (!r2d_lazy_init() || capacity <= 0) return (cw_r2d_instance_buffer){ 0 };
  int i = 0;
  while (i < MAX_IBUFS && IB[i].live) i++;
  if (i == MAX_IBUFS) {
    cw_error("cw_r2d_instance_buffer_create: %d buffers live already", MAX_IBUFS);
    return (cw_r2d_instance_buffer){ 0 };
  }
  r2d_ibuf *b = &IB[i];
  size_t bytes = sizeof(cw_sprite_instance) * (size_t)capacity;
  if (r2d_wg()) b->wbuf = r2d_wg_instance_buffer(NULL, bytes);
  else {
    glGenBuffers(1, &b->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, NULL, 0x88E4 /* GL_STATIC_DRAW */);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }
  b->cap = capacity;
  b->count = 0;
  b->live = true;
  b->gen++;
  return CW_HANDLE_MAKE(cw_r2d_instance_buffer, i + 1, b->gen);
}

void cw_r2d_instance_buffer_write(cw_r2d_instance_buffer h, int first, const cw_sprite_instance *inst, int count) {
  r2d_ibuf *b = ibuf_get(h);
  if (!b || !inst || count <= 0) return;
  if (first < 0 || first + count > b->cap) {
    cw_error("cw_r2d_instance_buffer_write: [%d, %d) is outside the buffer's %d instances", first, first + count, b->cap);
    return;
  }
  size_t off = sizeof(cw_sprite_instance) * (size_t)first, bytes = sizeof(cw_sprite_instance) * (size_t)count;
  if (r2d_wg()) r2d_wg_buffer_write(b->wbuf, off, inst, bytes);
  else {
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)off, (GLsizeiptr)bytes, inst);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }
  if (first + count > b->count) b->count = first + count;
}

void cw_r2d_instance_buffer_set_count(cw_r2d_instance_buffer h, int count) {
  r2d_ibuf *b = ibuf_get(h);
  if (b) b->count = CW_CLAMP(count, 0, b->cap);
}

void cw_r2d_instance_buffer_destroy(cw_r2d_instance_buffer h) {
  r2d_ibuf *b = ibuf_get(h);
  if (!b) return;
  if (b->wbuf) r2d_wg_buffer_release(b->wbuf);
  if (b->vbo) glDeleteBuffers(1, &b->vbo);
  b->wbuf = NULL;
  b->vbo = 0;
  b->live = false;
}

void cw_r2d_instances(const cw_r2d_instances_desc *d) {
  CW_ASSERT(R.in_pass, "submit instances between cw_r2d_begin and cw_r2d_end");
  if (!d) return;
  r2d_ibuf *kb = ibuf_get(d->buffer);
  int count = kb ? kb->count : d->count;
  if (count <= 0 || (!kb && !d->instances)) return;
  const r2d_tex *t = CW_HANDLE_IS_NULL(d->texture) ? NULL : r2d_tex_get(d->texture);
  r2d_inst_draw dr = { .count = count, .array = t ? t->array : R2D_NO_ARRAY };
  float (*u)[4] = dr.u;
  cw_vec2 pv = (d->pivot.x == 0 && d->pivot.y == 0) ? (cw_vec2){ 0.5f, 0.5f } : d->pivot;
  u[0][0] = pv.x, u[0][1] = pv.y, u[0][2] = d->size.x, u[0][3] = d->size.y;
  if (!t && !(d->size.x > 0)) u[0][2] = u[0][3] = 1; /* solid squares of one unit unless sized */
  u[1][0] = (float)R.cur_layer, u[1][1] = (float)r2d_mode(t == NULL), u[1][2] = 1.0f + (float)R.intensity_b * 15.0f / 255.0f;
  u[1][3] = (float)R.emissive_b / 255.0f;
  u[2][0] = t ? (float)t->page : 0, u[2][1] = (float)R.flash_b / 255.0f, u[2][2] = CW_CLAMP(d->sway, 0.0f, 1.0f), u[2][3] = 0;
  int nf = CW_CLAMP(d->nframes, 0, CW_R2D_INSTANCE_FRAMES);
  if (d->nframes > CW_R2D_INSTANCE_FRAMES) cw_warn("cw_r2d_instances: %d frames, at most %d used", d->nframes, CW_R2D_INSTANCE_FRAMES);
  for (int f = 0; f < CW_R2D_INSTANCE_FRAMES; f++) {
    cw_rect src = (t && f < nf && d->frames) ? d->frames[f] : (t ? (cw_rect){ 0, 0, (float)t->w, (float)t->h } : (cw_rect){ 0, 0, 1, 1 });
    if (f >= CW_MAX(nf, 1)) src = (t && nf && d->frames) ? d->frames[0] : src; /* out-of-range frames draw the first */
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if (t) r2d_tex_uv(t, src, &u0, &v0, &u1, &v1);
    float *a = u[3 + 2 * f], *b = u[4 + 2 * f];
    a[0] = u0, a[1] = v0, a[2] = u1, a[3] = v1;
    b[0] = src.w, b[1] = src.h, b[2] = b[3] = 0;
  }
  if (kb) {
    dr.vbo = kb->vbo;
    dr.wbuf = kb->wbuf;
  } else {
    dr.offset = sizeof(cw_sprite_instance) * (size_t)R.istage.count;
    cw_array_reserve(&R.istage, R.istage.count + count);
    memcpy(R.istage.data + R.istage.count, d->instances, sizeof(cw_sprite_instance) * (size_t)count);
    R.istage.count += count;
  }
  r2d_note_tex(t);
  if (R.emissive_b && R.layers[R.cur_layer].lit) R.normals_used = R.material_used = true;
  cw_array_push(&R.idraws, dr);
  cw_blend bl = R.blend == CW_BLEND_ADD ? CW_BLEND_ALPHA : R.blend;
  float sy = kb ? 0 : d->instances[0].y;
  r2d_push_special_kind(dr.array, sy, (uint32_t)(R.idraws.count - 1), R2D_KIND_INSTANCES, bl);
  R.stats.instances += (uint32_t)count;
}

/* ── GL ────────────────────────────────────────────────────────────────── */

void r2d_instances_upload_gl(void) {
  if (!R.istage.count) return;
  size_t bytes = sizeof(cw_sprite_instance) * (size_t)R.istage.count;
  if (!stream_vbo) glGenBuffers(1, &stream_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, stream_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, R.istage.data, GL_STREAM_DRAW);
  stream_cap = bytes;
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void r2d_instances_gl(const r2d_inst_draw *d, bool lit) {
  const r2d_program *p = lit ? &R.inst_lit : &R.inst;
  glUseProgram(p->prog);
  r2d_upload_layer_uniforms(p);
  glUniform4fv(p->u[0], R2D_INST_VEC4, &d->u[0][0]);
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
  if (!inst_vao) {
    glGenVertexArrays(1, &inst_vao);
    glBindVertexArray(inst_vao);
    for (int i = 0; i < 3; i++) {
      glEnableVertexAttribArray((GLuint)i);
      glVertexAttribDivisor((GLuint)i, 1);
    }
  }
  glBindVertexArray(inst_vao);
  glBindBuffer(GL_ARRAY_BUFFER, d->vbo ? d->vbo : stream_vbo);
  const char *o = (const char *)(uintptr_t)d->offset;
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 24, o);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 24, o + 16);
  glVertexAttribPointer(2, 2, GL_UNSIGNED_SHORT, GL_FALSE, 24, o + 20);
  glDrawArraysInstanced(GL_TRIANGLES, 0, 6, d->count);
  glBindVertexArray(0);
  R.stats.draw_calls++;
}

void r2d_instances_shutdown(void) {
  for (int i = 0; i < MAX_IBUFS; i++)
    if (IB[i].live) cw_r2d_instance_buffer_destroy(CW_HANDLE_MAKE(cw_r2d_instance_buffer, i + 1, IB[i].gen));
  if (stream_vbo) glDeleteBuffers(1, &stream_vbo);
  if (inst_vao) glDeleteVertexArrays(1, &inst_vao);
  if (stream_wbuf) r2d_wg_buffer_release(stream_wbuf);
  stream_vbo = inst_vao = 0;
  stream_cap = stream_wcap = 0;
  stream_wbuf = NULL;
  cw_array_free(&R.idraws);
  cw_array_free(&R.istage);
}

/* WebGPU: the pass's per-frame instances in one buffer (wgpu.c draws from it) */
void *r2d_instances_upload_wg(void) {
  if (!R.istage.count) return stream_wbuf;
  size_t bytes = sizeof(cw_sprite_instance) * (size_t)R.istage.count;
  if (!stream_wbuf || stream_wcap < bytes) {
    size_t cap = CW_MAX(bytes, stream_wcap * 2);
    stream_wbuf = r2d_wg_instance_buffer(stream_wbuf, cap);
    stream_wcap = cap;
  }
  r2d_wg_buffer_write(stream_wbuf, 0, R.istage.data, bytes);
  return stream_wbuf;
}
