/* render2d textures: GL_TEXTURE_2D_ARRAY atlases with runtime packing,
 * standalone arrays, render targets, normal-map mirrors. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* stb_image declares (static) loaders it only defines for formats compiled in */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "cartwheel/platform.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_ASSERT(x) ((void)0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_ASSERT(x) ((void)0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_rect_pack.h"
#pragma GCC diagnostic pop

r2d_tex *r2d_tex_get(cw_texture t) { return R.inited ? cw_pool_get(&R.textures, t.id) : NULL; }

static void set_params(bool nearest, bool repeat) {
  GLint f = nearest ? GL_NEAREST : GL_LINEAR, w = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, f);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, f);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, w);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, w);
}

static GLuint make_array_tex(int w, int h, int layers, bool nearest, bool repeat) {
  GLuint t;
  glGenTextures(1, &t);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, t);
  glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, w, h, layers);
  set_params(nearest, repeat);
  return t;
}

/* fill layers [from, to) of an array with one colour (new atlas pages start
 * transparent, normal mirrors start "no normal") */
static void fill_layers(GLuint tex, int w, int h, int from, int to, const uint8_t rgba[4]) {
  if (!R.blit_draw_fbo) glGenFramebuffers(1, &R.blit_draw_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, R.blit_draw_fbo);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f);
  for (int l = from; l < to; l++) {
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, l);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* copy layers [0, n) of src into dst on the GPU (GLES3 has no
 * glCopyImageSubData; a framebuffer blit per layer does it) */
static void copy_layers(GLuint src, GLuint dst, int w, int h, int n) {
  if (!R.blit_read_fbo) glGenFramebuffers(1, &R.blit_read_fbo);
  if (!R.blit_draw_fbo) glGenFramebuffers(1, &R.blit_draw_fbo);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, R.blit_read_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, R.blit_draw_fbo);
  for (int l = 0; l < n; l++) {
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src, 0, l);
    glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst, 0, l);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
  glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static const uint8_t CLEAR0[4] = { 0, 0, 0, 0 };
static const uint8_t NO_NORMAL[4] = { 128, 128, 0, 255 };

typedef struct r2d_packer {
  stbrp_context ctx;
  stbrp_node nodes[]; /* one per pixel column */
} r2d_packer;

static r2d_packer *packer_new(int w, int h) {
  r2d_packer *p = cw_alloc(sizeof *p + sizeof(stbrp_node) * (size_t)w, CW_MEM_RENDER2D);
  stbrp_init_target(&p->ctx, w, h, p->nodes, w);
  return p;
}

int r2d_array_new(int w, int h, int layers, bool nearest, bool repeat, bool atlas) {
  int idx = -1;
  for (int i = 0; i < R.arrays.count; i++)
    if (!R.arrays.data[i].live) { idx = i; break; }
  if (idx < 0) {
    CW_ASSERT(R.arrays.count < (int)R2D_NO_ARRAY, "too many texture arrays: use atlas textures");
    cw_array_push(&R.arrays, (r2d_array){ 0 });
    idx = R.arrays.count - 1;
  }
  r2d_array *a = &R.arrays.data[idx];
  memset(a, 0, sizeof *a);
  a->live = true;
  a->atlas = atlas;
  a->repeat = repeat;
  a->nearest = nearest;
  a->w = w;
  a->h = h;
  a->layers = layers;
  if (r2d_wg()) a->wtex = r2d_wg_tex_new(w, h, layers);
  else a->tex = make_array_tex(w, h, layers, nearest, repeat);
  if (atlas) {
    if (r2d_wg()) r2d_wg_tex_fill(a->wtex, 0, layers, CLEAR0);
    else fill_layers(a->tex, w, h, 0, layers, CLEAR0);
    a->packers = cw_alloc(sizeof(r2d_packer *) * (size_t)layers, CW_MEM_RENDER2D);
    for (int l = 0; l < layers; l++) ((r2d_packer **)a->packers)[l] = packer_new(w, h);
    R.stats.atlas_pages += (uint32_t)layers;
  }
  return idx;
}

void r2d_array_free(int i) {
  r2d_array *a = &R.arrays.data[i];
  if (!a->live) return;
  if (r2d_wg()) {
    r2d_wg_array_changed(a);
    r2d_wg_tex_release(a->wtex);
    r2d_wg_tex_release(a->wntex);
  } else {
    glDeleteTextures(1, &a->tex);
    if (a->normal_tex) glDeleteTextures(1, &a->normal_tex);
  }
  if (a->packers)
    for (int l = 0; l < a->layers; l++) cw_free(((r2d_packer **)a->packers)[l]);
  cw_free(a->packers);
  memset(a, 0, sizeof *a);
}

/* grow an atlas by one page (doubling up to the GL limit) */
static bool atlas_grow(int i) {
  r2d_array *a = &R.arrays.data[i];
  if (a->layers >= R.max_layers) return false;
  int old = a->layers, n = CW_MIN(old * 2, R.max_layers);
  if (r2d_wg()) {
    void *t = r2d_wg_tex_new(a->w, a->h, n);
    r2d_wg_tex_copy(a->wtex, t, old);
    r2d_wg_tex_fill(t, old, n, CLEAR0);
    r2d_wg_array_changed(a);
    r2d_wg_tex_release(a->wtex);
    a->wtex = t;
    if (a->wntex) {
      void *nt = r2d_wg_tex_new(a->w, a->h, n);
      r2d_wg_tex_copy(a->wntex, nt, old);
      r2d_wg_tex_fill(nt, old, n, NO_NORMAL);
      r2d_wg_tex_release(a->wntex);
      a->wntex = nt;
    }
  } else {
  GLuint t = make_array_tex(a->w, a->h, n, a->nearest, a->repeat);
  copy_layers(a->tex, t, a->w, a->h, old);
  fill_layers(t, a->w, a->h, old, n, CLEAR0);
  glDeleteTextures(1, &a->tex);
  a->tex = t;
  if (a->normal_tex) {
    GLuint nt = make_array_tex(a->w, a->h, n, a->nearest, false);
    copy_layers(a->normal_tex, nt, a->w, a->h, old);
    fill_layers(nt, a->w, a->h, old, n, NO_NORMAL);
    glDeleteTextures(1, &a->normal_tex);
    a->normal_tex = nt;
  }
  }
  /* packers are separate blocks (a context points into its own nodes), so
   * growing only adds blocks */
  a->packers = cw_realloc(a->packers, sizeof(r2d_packer *) * (size_t)n, CW_MEM_RENDER2D);
  for (int l = old; l < n; l++) ((r2d_packer **)a->packers)[l] = packer_new(a->w, a->h);
  R.stats.atlas_pages += (uint32_t)(n - old);
  a->layers = n;
  return true;
}

/* place a w x h image (plus 1px border each side) in an atlas */
static bool atlas_place(int i, int w, int h, int *page, int *x, int *y) {
  for (;;) {
    r2d_array *a = &R.arrays.data[i];
    for (int l = 0; l < a->layers; l++) {
      stbrp_rect r = { .w = w + 2, .h = h + 2 };
      stbrp_pack_rects(&((r2d_packer **)a->packers)[l]->ctx, &r, 1);
      if (r.was_packed) {
        *page = l;
        *x = r.x + 1;
        *y = r.y + 1;
        return true;
      }
    }
    if (!atlas_grow(i)) return false;
  }
}

/* upload w*h premultiplied pixels with a 1px extruded border */
static void upload_extruded(GLuint tex, void *wtex, int page, int x, int y, int w, int h, const uint8_t *px) {
  int W = w + 2, H = h + 2;
  uint8_t *buf = cw_alloc((size_t)W * (size_t)H * 4, CW_MEM_RENDER2D);
  for (int yy = 0; yy < H; yy++) {
    int sy = CW_CLAMP(yy - 1, 0, h - 1);
    for (int xx = 0; xx < W; xx++) {
      int sx = CW_CLAMP(xx - 1, 0, w - 1);
      memcpy(buf + ((size_t)yy * W + xx) * 4, px + ((size_t)sy * w + sx) * 4, 4);
    }
  }
  if (wtex) r2d_wg_tex_write(wtex, x - 1, y - 1, page, W, H, buf);
  else {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, x - 1, y - 1, page, W, H, 1, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  }
  cw_free(buf);
  R.stats.texture_uploads++;
}

static uint8_t *premultiply_copy(int w, int h, const uint8_t *rgba, bool already) {
  size_t n = (size_t)w * (size_t)h;
  uint8_t *p = cw_alloc(n * 4, CW_MEM_RENDER2D);
  memcpy(p, rgba, n * 4);
  if (!already)
    for (size_t i = 0; i < n; i++) {
      unsigned a = p[4 * i + 3];
      for (int k = 0; k < 3; k++) p[4 * i + k] = (uint8_t)((p[4 * i + k] * a + 127) / 255);
    }
  return p;
}

static int atlas_for(cw_filter f) {
  if (R.atlas_array[f] < 0) R.atlas_array[f] = r2d_array_new(R2D_PAGE, R2D_PAGE, 1, f == CW_FILTER_NEAREST, false, true);
  return R.atlas_array[f];
}

static void upload_whole(r2d_tex *t, const uint8_t *px) {
  r2d_array *a = &R.arrays.data[t->array];
  if (a->atlas) {
    upload_extruded(a->tex, a->wtex, t->page, t->x, t->y, t->w, t->h, px);
  } else if (r2d_wg()) {
    r2d_wg_tex_write(a->wtex, 0, 0, 0, t->w, t->h, px);
    R.stats.texture_uploads++;
  } else {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, a->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, t->w, t->h, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    R.stats.texture_uploads++;
  }
}

cw_texture cw_texture_from_pixels(int w, int h, const uint8_t *rgba, const cw_texture_desc *desc) {
  if (!r2d_lazy_init()) return (cw_texture){ 0 };
  cw_texture_desc d = desc ? *desc : (cw_texture_desc){ 0 };
  if (w <= 0 || h <= 0 || !rgba) {
    cw_error("cw_texture_from_pixels: %dx%d with %s pixels; pass a positive size and RGBA8 data", w, h,
             rgba ? "" : "NULL");
    return (cw_texture){ 0 };
  }
  bool standalone = d.standalone || d.repeat || w + 2 > R2D_PAGE || h + 2 > R2D_PAGE;
  uint64_t id;
  r2d_tex *t = cw_pool_alloc(&R.textures, &id);
  t->w = w;
  t->h = h;
  t->filter = d.filter;
  t->premultiplied = d.premultiplied;
  if (standalone) {
    t->array = (uint16_t)r2d_array_new(w, h, 1, d.filter == CW_FILTER_NEAREST, d.repeat, false);
  } else {
    int a = atlas_for(d.filter), page, x, y;
    if (!atlas_place(a, w, h, &page, &x, &y)) {
      cw_error("cw_texture: atlas full (%d pages of %d px); use standalone textures or fewer images", R.max_layers,
               R2D_PAGE);
      cw_pool_release(&R.textures, id);
      return (cw_texture){ 0 };
    }
    t->array = (uint16_t)a;
    t->page = (uint16_t)page;
    t->x = x;
    t->y = y;
  }
  uint8_t *pm = premultiply_copy(w, h, rgba, d.premultiplied);
  upload_whole(t, pm);
  cw_free(pm);
  return (cw_texture){ id };
}

cw_texture cw_texture_from_memory(const void *data, size_t len, const cw_texture_desc *desc) {
  int w, h, n;
  uint8_t *px = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
  if (!px) {
    cw_error("cw_texture: could not decode image (%s); PNG and JPEG are supported", stbi_failure_reason());
    return (cw_texture){ 0 };
  }
  cw_texture t = cw_texture_from_pixels(w, h, px, desc);
  stbi_image_free(px);
  return t;
}

cw_texture cw_texture_load(const char *path, const cw_texture_desc *desc) {
  size_t len;
  void *data = cw_asset_load(path, &len);
  if (!data) {
    cw_error("cw_texture_load: asset '%s' not found (asset root: CW_ASSETS natively, the cart bundle in carts)", path);
    return (cw_texture){ 0 };
  }
  cw_texture t = cw_texture_from_memory(data, len, desc);
  free(data);
  if (CW_HANDLE_IS_NULL(t)) cw_error("cw_texture_load: '%s' failed", path);
  return t;
}

void cw_texture_update(cw_texture h, const uint8_t *rgba) {
  r2d_tex *t = r2d_tex_get(h);
  if (!t || t->target) return;
  uint8_t *pm = premultiply_copy(t->w, t->h, rgba, t->premultiplied);
  upload_whole(t, pm);
  cw_free(pm);
}

GLuint r2d_array_normal_tex(int i) {
  r2d_array *a = &R.arrays.data[i];
  return a->normal_tex ? a->normal_tex : R.dummy_normal;
}

bool cw_texture_set_normal_map(cw_texture h, const uint8_t *rgba) {
  r2d_tex *t = r2d_tex_get(h);
  if (!t) return false;
  r2d_array *a = &R.arrays.data[t->array];
  if (!a->atlas) {
    cw_error("cw_texture_set_normal_map: only atlas textures take normal maps (this one is standalone or a target)");
    return false;
  }
  if (r2d_wg() && !a->wntex) {
    a->wntex = r2d_wg_tex_new(a->w, a->h, a->layers);
    r2d_wg_tex_fill(a->wntex, 0, a->layers, NO_NORMAL);
    r2d_wg_array_changed(a);
  } else if (!r2d_wg() && !a->normal_tex) {
    a->normal_tex = make_array_tex(a->w, a->h, a->layers, a->nearest, false);
    fill_layers(a->normal_tex, a->w, a->h, 0, a->layers, NO_NORMAL);
  }
  /* the normal buffer stores the encoded normal; keep z_enc >= 0.5 so it never
   * reads as "no normal" (z_enc 0) */
  size_t n = (size_t)t->w * (size_t)t->h;
  uint8_t *p = cw_alloc(n * 4, CW_MEM_RENDER2D);
  for (size_t i = 0; i < n; i++) {
    p[4 * i] = rgba[4 * i];
    p[4 * i + 1] = rgba[4 * i + 1];
    p[4 * i + 2] = CW_MAX(rgba[4 * i + 2], 128);
    p[4 * i + 3] = 255;
  }
  upload_extruded(a->normal_tex, a->wntex, t->page, t->x, t->y, t->w, t->h, p);
  cw_free(p);
  t->has_normal = true;
  return true;
}

bool cw_texture_load_normal_map(cw_texture h, const char *path) {
  r2d_tex *t = r2d_tex_get(h);
  if (!t) return false;
  size_t len;
  void *data = cw_asset_load(path, &len);
  if (!data) {
    cw_error("cw_texture_load_normal_map: asset '%s' not found", path);
    return false;
  }
  int w, hh, n;
  uint8_t *px = stbi_load_from_memory(data, (int)len, &w, &hh, &n, 4);
  free(data);
  if (!px || w != t->w || hh != t->h) {
    cw_error("cw_texture_load_normal_map: '%s' must decode to the texture's size %dx%d", path, t->w, t->h);
    if (px) stbi_image_free(px);
    return false;
  }
  bool ok = cw_texture_set_normal_map(h, px);
  stbi_image_free(px);
  return ok;
}

void cw_texture_size(cw_texture h, int *w, int *hh) {
  r2d_tex *t = r2d_tex_get(h);
  if (w) *w = t ? t->w : 0;
  if (hh) *hh = t ? t->h : 0;
}

bool cw_texture_valid(cw_texture h) { return r2d_tex_get(h) != NULL; }

static void target_free_gl(r2d_tex *t) {
  if (t->wdepth) r2d_wg_tex_release(t->wdepth);
  t->wdepth = NULL;
  if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
  if (t->depth_rb) glDeleteRenderbuffers(1, &t->depth_rb);
  t->fbo = t->depth_rb = 0;
}

void cw_texture_destroy(cw_texture h) {
  r2d_tex *t = r2d_tex_get(h);
  if (!t) return;
  if (!R.arrays.data[t->array].atlas) r2d_array_free(t->array);
  target_free_gl(t);
  cw_pool_release(&R.textures, h.id);
}

static void target_alloc(r2d_tex *t, bool depth) {
  t->array = (uint16_t)r2d_array_new(t->w, t->h, 1, t->filter == CW_FILTER_NEAREST, false, false);
  if (r2d_wg()) { /* WebGPU: the array is the target; a depth-stencil texture beside it; starts transparent */
    r2d_wg_tex_fill(R.arrays.data[t->array].wtex, 0, 1, CLEAR0);
    if (depth) t->wdepth = r2d_wg_depth_new(t->w, t->h);
    return;
  }
  GLuint tex = R.arrays.data[t->array].tex;
  glGenFramebuffers(1, &t->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
  glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, 0);
  if (depth) {
    glGenRenderbuffers(1, &t->depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, t->w, t->h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth_rb);
  }
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) cw_error("render target %dx%d: framebuffer incomplete (0x%x)", t->w, t->h, st);
  glClearColor(0, 0, 0, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

cw_texture r2d_texture_new_internal(int w, int h, cw_filter filter, bool depth) {
  uint64_t id;
  r2d_tex *t = cw_pool_alloc(&R.textures, &id);
  t->w = w;
  t->h = h;
  t->filter = filter;
  t->target = true;
  t->y_up = true;
  t->premultiplied = true;
  target_alloc(t, depth);
  return (cw_texture){ id };
}

cw_texture cw_target_create(const cw_target_desc *d) {
  if (!r2d_lazy_init()) return (cw_texture){ 0 };
  if (!d || d->width <= 0 || d->height <= 0) {
    cw_error("cw_target_create: give a positive width and height");
    return (cw_texture){ 0 };
  }
  return r2d_texture_new_internal(d->width, d->height, d->filter, d->depth);
}

unsigned cw_target_fbo(cw_texture h) {
  r2d_tex *t = r2d_tex_get(h);
  return t && t->target ? t->fbo : 0;
}

void cw_target_resize(cw_texture h, int w, int hh) {
  r2d_tex *t = r2d_tex_get(h);
  if (!t || !t->target || (t->w == w && t->h == hh)) return;
  bool depth = t->depth_rb != 0 || t->wdepth != NULL;
  r2d_array_free(t->array);
  target_free_gl(t);
  t->w = w;
  t->h = hh;
  target_alloc(t, depth);
}

void r2d_tex_uv(const r2d_tex *t, cw_rect src, float *u0, float *v0, float *u1, float *v1) {
  const r2d_array *a = &R.arrays.data[t->array];
  float iw = 1.0f / (float)a->w, ih = 1.0f / (float)a->h;
  if (src.w == 0 && src.h == 0) src = (cw_rect){ 0, 0, (float)t->w, (float)t->h };
  float sy = t->y_up ? (float)t->h - src.y - src.h : src.y;
  *u0 = ((float)t->x + src.x) * iw;
  *u1 = ((float)t->x + src.x + src.w) * iw;
  float a0 = ((float)t->y + sy) * ih, a1 = ((float)t->y + sy + src.h) * ih;
  if (t->y_up) {
    *v0 = a1;
    *v1 = a0;
  } else {
    *v0 = a0;
    *v1 = a1;
  }
}

void r2d_tex_uv_point(const r2d_tex *t, float sx, float sy, float *u, float *v) {
  const r2d_array *a = &R.arrays.data[t->array];
  *u = ((float)t->x + sx) / (float)a->w;
  *v = ((float)t->y + (t->y_up ? (float)t->h - sy : sy)) / (float)a->h;
}

void r2d_textures_shutdown(void) {
  for (int i = 0; i < R.textures.slot_count; i++) {
    r2d_tex *t = cw_pool_at(&R.textures, i);
    if (t) target_free_gl(t);
  }
  for (int i = 0; i < R.arrays.count; i++) r2d_array_free(i);
  cw_array_free(&R.arrays);
  cw_pool_free_all(&R.textures);
  if (R.dummy_normal && !r2d_wg()) glDeleteTextures(1, &R.dummy_normal);
  if (R.blit_read_fbo) glDeleteFramebuffers(1, &R.blit_read_fbo);
  if (R.blit_draw_fbo) glDeleteFramebuffers(1, &R.blit_draw_fbo);
  R.dummy_normal = R.blit_read_fbo = R.blit_draw_fbo = 0;
  R.atlas_array[0] = R.atlas_array[1] = -1;
}
