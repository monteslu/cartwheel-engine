/* Effects GL helpers: programs (with transform feedback varyings), cached
 * uniform locations, float textures and FBOs, the curl-noise volume, curves,
 * and the per-tree effects registry. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/platform.h"
#include "fx_internal.h"
#include "render/pipeline_internal.h"

unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file,
                                     int desired_channels);
void stbi_image_free(void *retval_from_stbi_load);

/* ── programs ───────────────────────────────────────────────────────── */

static const char *const prelude =
    "#version 300 es\n"
    "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler3D;\n";

static unsigned compile(const char *name, GLenum type, const char *body) {
  GLuint s = glCreateShader(type);
  const char *src[2] = { prelude, body };
  glShaderSource(s, 2, src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    GLsizei n = 0;
    glGetShaderInfoLog(s, sizeof log, &n, log);
    cw_error("fx shader '%s' (%s) failed to compile: %.*s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment",
             (int)n, log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

unsigned cw__fx_program(const char *name, const char *vs, const char *fs, const char *const *varyings, int nvaryings) {
  GLuint v = compile(name, GL_VERTEX_SHADER, vs);
  GLuint f = compile(name, GL_FRAGMENT_SHADER, fs);
  if (!v || !f) {
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    return 0;
  }
  GLuint p = glCreateProgram();
  cw__gl_program_born(p);
  glAttachShader(p, v);
  glAttachShader(p, f);
  if (varyings && nvaryings > 0) glTransformFeedbackVaryings(p, nvaryings, varyings, GL_SEPARATE_ATTRIBS);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    GLsizei n = 0;
    glGetProgramInfoLog(p, sizeof log, &n, log);
    cw_error("fx program '%s' failed to link: %.*s", name, (int)n, log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

/* (program, name pointer) -> location + 1 */
static cw_map loc_cache;
static bool loc_init;

int cw__fx_loc(unsigned prog, const char *name) {
  if (!loc_init) {
    cw_map_init(&loc_cache, CW_MEM_RENDER);
    loc_init = true;
  }
  uint64_t key = (((uint64_t)prog << 40) ^ (uint64_t)(uintptr_t)name) * 0x9E3779B97F4A7C15ull ^ ((uint64_t)cw__gl_program_epoch(prog) << 1);
  uint64_t v;
  if (cw_map_get(&loc_cache, key, &v)) return (int)v - 1;
  int loc = glGetUniformLocation(prog, name);
  cw_map_set(&loc_cache, key, (uint64_t)(loc + 1));
  return loc;
}

/* each setter skips a value the program already holds (cw__gl_uniform_held) */
#define HELD(p, l, ptr, n) cw__gl_uniform_held((p), (l), (ptr), (n))
void cw__fx_u1f(unsigned p, const char *n, float v) { int l = cw__fx_loc(p, n); if (!HELD(p, l, &v, 4)) glUniform1f(l, v); }
void cw__fx_u1i(unsigned p, const char *n, int v) { int l = cw__fx_loc(p, n); if (!HELD(p, l, &v, 4)) glUniform1i(l, v); }
void cw__fx_u1ui(unsigned p, const char *n, unsigned v) { int l = cw__fx_loc(p, n); if (!HELD(p, l, &v, 4)) glUniform1ui(l, v); }
void cw__fx_u2f(unsigned p, const char *n, float x, float y) {
  float v[2] = { x, y };
  int l = cw__fx_loc(p, n);
  if (!HELD(p, l, v, 8)) glUniform2f(l, x, y);
}
void cw__fx_u3f(unsigned p, const char *n, float x, float y, float z) {
  float v[3] = { x, y, z };
  int l = cw__fx_loc(p, n);
  if (!HELD(p, l, v, 12)) glUniform3f(l, x, y, z);
}
void cw__fx_u4f(unsigned p, const char *n, float x, float y, float z, float w) {
  float v[4] = { x, y, z, w };
  int l = cw__fx_loc(p, n);
  if (!HELD(p, l, v, 16)) glUniform4f(l, x, y, z, w);
}
void cw__fx_u4fv(unsigned p, const char *n, const float *v, int count) {
  if (count <= 0) return;
  int l = cw__fx_loc(p, n);
  if (!HELD(p, l, v, count * 16)) glUniform4fv(l, count, v);
}
void cw__fx_mat4(unsigned p, const char *n, const float *m) {
  int l = cw__fx_loc(p, n);
  if (!HELD(p, l, m, 64)) glUniformMatrix4fv(l, 1, GL_FALSE, m);
}
#undef HELD

/* ── textures and targets ───────────────────────────────────────────── */

unsigned cw__fx_tex2d(int w, int h, unsigned ifmt, unsigned fmt, unsigned type, bool linear, const void *data) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, type, data);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return t;
}

void cw__fx_bind_tex(int unit, unsigned target, unsigned tex) {
  glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
  glBindTexture(target, tex);
}

unsigned cw__fx_fbo(unsigned tex0, unsigned tex1, unsigned depth) {
  GLuint f;
  glGenFramebuffers(1, &f);
  glBindFramebuffer(GL_FRAMEBUFFER, f);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex0, 0);
  if (tex1) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, tex1, 0);
  if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
  GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
  glDrawBuffers(tex1 ? 2 : 1, bufs);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    cw_error("fx: framebuffer incomplete (0x%x)", (unsigned)st);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &f);
    return 0;
  }
  return f;
}

bool cw__fx_float32_ok(void) {
  static int ok = -1;
  if (ok >= 0) return ok == 1;
  while (glGetError() != GL_NO_ERROR) {}
  GLuint t = cw__fx_tex2d(4, 4, GL_RGBA32F, GL_RGBA, GL_FLOAT, false, NULL);
  GLuint f;
  glGenFramebuffers(1, &f);
  glBindFramebuffer(GL_FRAMEBUFFER, f);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &f);
  glDeleteTextures(1, &t);
  while (glGetError() != GL_NO_ERROR) {}
  if (!ok)
    cw_error("fx: RGBA32F is not colour-renderable on this host (EXT_color_buffer_float): particle sorting, "
             "trails and gpu_stats are refused");
  return ok == 1;
}

/* ── curl noise ─────────────────────────────────────────────────────── */

#define NOISE_N 32

static float lattice_value(const float *lat, int g, int x, int y, int z) {
  x = ((x % g) + g) % g;
  y = ((y % g) + g) % g;
  z = ((z % g) + g) % g;
  return lat[(z * g + y) * g + x];
}

/* periodic value noise sampled at grid point (x, y, z) of an N^3 grid from a
 * g^3 lattice, smoothstep weights */
static float value_noise(const float *lat, int g, int x, int y, int z) {
  float s = (float)g / NOISE_N;
  float fx = x * s, fy = y * s, fz = z * s;
  int ix = (int)floorf(fx), iy = (int)floorf(fy), iz = (int)floorf(fz);
  float tx = fx - ix, ty = fy - iy, tz = fz - iz;
  tx = tx * tx * (3 - 2 * tx);
  ty = ty * ty * (3 - 2 * ty);
  tz = tz * tz * (3 - 2 * tz);
  float c[2][2][2];
  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 2; j++)
      for (int i = 0; i < 2; i++) c[k][j][i] = lattice_value(lat, g, ix + i, iy + j, iz + k);
  float x00 = c[0][0][0] + (c[0][0][1] - c[0][0][0]) * tx, x10 = c[0][1][0] + (c[0][1][1] - c[0][1][0]) * tx;
  float x01 = c[1][0][0] + (c[1][0][1] - c[1][0][0]) * tx, x11 = c[1][1][0] + (c[1][1][1] - c[1][1][0]) * tx;
  float y0 = x00 + (x10 - x00) * ty, y1 = x01 + (x11 - x01) * ty;
  return y0 + (y1 - y0) * tz;
}

const uint8_t *cw__fx_noise3d_rgba(void) {
  static uint8_t *px;
  if (px) return px;
  const int N = NOISE_N;
  float *pot = cw_alloc(sizeof(float) * 3 * N * N * N, CW_MEM_RENDER);
  cw_rng rng;
  cw_rng_seed(&rng, 0xC0FFEEull);
  for (int c = 0; c < 3; c++) {
    float lat4[4 * 4 * 4], lat8[8 * 8 * 8];
    for (int i = 0; i < 64; i++) lat4[i] = cw_rng_range(&rng, -1, 1);
    for (int i = 0; i < 512; i++) lat8[i] = cw_rng_range(&rng, -1, 1);
    for (int z = 0; z < N; z++)
      for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
          pot[((c * N + z) * N + y) * N + x] = value_noise(lat4, 4, x, y, z) + 0.5f * value_noise(lat8, 8, x, y, z);
  }
#define P(c, x, y, z) pot[((c * N + (((z) + N) % N)) * N + (((y) + N) % N)) * N + (((x) + N) % N)]
  float *curl = cw_alloc(sizeof(float) * 3 * N * N * N, CW_MEM_RENDER);
  float maxl = 1e-6f;
  for (int z = 0; z < N; z++)
    for (int y = 0; y < N; y++)
      for (int x = 0; x < N; x++) {
        float dzdy = P(2, x, y + 1, z) - P(2, x, y - 1, z), dydz = P(1, x, y, z + 1) - P(1, x, y, z - 1);
        float dxdz = P(0, x, y, z + 1) - P(0, x, y, z - 1), dzdx = P(2, x + 1, y, z) - P(2, x - 1, y, z);
        float dydx = P(1, x + 1, y, z) - P(1, x - 1, y, z), dxdy = P(0, x, y + 1, z) - P(0, x, y - 1, z);
        float *o = curl + 3 * ((z * N + y) * N + x);
        o[0] = dzdy - dydz;
        o[1] = dxdz - dzdx;
        o[2] = dydx - dxdy;
        float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (l > maxl) maxl = l;
      }
#undef P
  px = cw_alloc((size_t)4 * N * N * N, CW_MEM_RENDER);
  for (int i = 0; i < N * N * N; i++) {
    for (int c = 0; c < 3; c++) {
      float v = curl[3 * i + c] / maxl * 0.5f + 0.5f;
      int b = (int)(v * 255.0f + 0.5f);
      px[4 * i + c] = (uint8_t)CW_CLAMP(b, 0, 255);
    }
    px[4 * i + 3] = 255;
  }
  cw_free(curl);
  cw_free(pot);
  return px;
}

unsigned cw__fx_noise3d(void) {
  static GLuint tex;
  if (tex) return tex;
  const int N = NOISE_N;
  const uint8_t *px = cw__fx_noise3d_rgba();
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_3D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, N, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
  glBindTexture(GL_TEXTURE_3D, 0);
  return tex;
}

const uint8_t *cw__fx_dot_rgba(void) {
  enum { S = 64 };
  static uint8_t px[S * S * 4];
  static bool made;
  if (made) return px;
  made = true;
  for (int y = 0; y < S; y++)
    for (int x = 0; x < S; x++) {
      float dx = (x + 0.5f) / S * 2 - 1, dy = (y + 0.5f) / S * 2 - 1;
      float r = sqrtf(dx * dx + dy * dy);
      float a = r >= 1 ? 0 : (1 - r * r) * (1 - r * r);
      int b = (int)(a * 255 + 0.5f);
      uint8_t *o = px + 4 * (y * S + x);
      o[0] = o[1] = o[2] = 255;
      o[3] = (uint8_t)CW_CLAMP(b, 0, 255);
    }
  return px;
}

unsigned cw__fx_dot_texture(void) {
  static GLuint tex;
  if (!tex) tex = cw__fx_tex2d(64, 64, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true, cw__fx_dot_rgba());
  return tex;
}

typedef struct tex_entry { const char *path; GLuint tex; int w, h; } tex_entry;
static CW_ARRAY(tex_entry) tex_cache;

unsigned cw__fx_texture_asset(const char *path, int *w, int *h) {
  if (!path || !*path) return 0;
  const char *ip = cw_intern(path);
  for (int i = 0; i < tex_cache.count; i++)
    if (tex_cache.data[i].path == ip) {
      if (w) *w = tex_cache.data[i].w;
      if (h) *h = tex_cache.data[i].h;
      return tex_cache.data[i].tex;
    }
  tex_entry e = { ip, 0, 0, 0 };
  size_t len = 0;
  void *bytes = cw_asset_load(path, &len);
  if (!bytes) cw_error("particles: texture '%s' not found in the assets", path);
  else {
    int c = 0;
    uint8_t *px = stbi_load_from_memory(bytes, (int)len, &e.w, &e.h, &c, 4);
    free(bytes);
    if (!px) cw_error("particles: texture '%s' could not be decoded (PNG / JPEG expected)", path);
    else {
      e.tex = cw__fx_tex2d(e.w, e.h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true, px);
      glBindTexture(GL_TEXTURE_2D, e.tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glGenerateMipmap(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, 0);
      stbi_image_free(px);
    }
  }
  cw_array_push(&tex_cache, e);
  if (w) *w = e.w;
  if (h) *h = e.h;
  return e.tex;
}

/* ── curves ─────────────────────────────────────────────────────────── */

bool cw__fx_curve_bake(const char *text, int comps, float def, int n, float *out, const char *what) {
  enum { MAX_STOPS = 64 };
  float t[MAX_STOPS], v[MAX_STOPS][4];
  int ns = 0;
  bool ok = true;
  const char *s = text ? text : "";
  while (*s) {
    while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n') s++;
    if (!*s) break;
    if (ns == MAX_STOPS) {
      cw_warn("%s: more than %d stops; the rest are ignored", what, MAX_STOPS);
      break;
    }
    char *e;
    t[ns] = strtof(s, &e);
    if (e == s) { ok = false; break; }
    s = e;
    for (int c = 0; c < comps; c++) {
      while (*s == ' ' || *s == '\t') s++;
      v[ns][c] = strtof(s, &e);
      if (e == s) { ok = false; break; }
      s = e;
    }
    if (!ok) break;
    ns++;
    while (*s == ' ' || *s == '\t') s++;
    if (*s && *s != ',') { ok = false; break; }
  }
  if (!ok) {
    cw_error("%s: bad curve \"%s\": expected stops \"t v%s, ...\" (t in 0..1)", what, text,
             comps == 4 ? " v v v" : "");
    ns = 0;
  }
  for (int i = 0; i < n; i++) {
    float x = n > 1 ? (float)i / (float)(n - 1) : 0;
    for (int c = 0; c < comps; c++) {
      float val = def;
      if (ns == 1) val = v[0][c];
      else if (ns > 1) {
        if (x <= t[0]) val = v[0][c];
        else if (x >= t[ns - 1]) val = v[ns - 1][c];
        else
          for (int k = 0; k < ns - 1; k++)
            if (x >= t[k] && x <= t[k + 1]) {
              float d = t[k + 1] - t[k];
              float u = d > 0 ? (x - t[k]) / d : 0;
              val = v[k][c] + (v[k + 1][c] - v[k][c]) * u;
              break;
            }
      }
      out[i * comps + c] = val;
    }
  }
  return ok;
}

/* ── registry ───────────────────────────────────────────────────────── */

static void fx_tree_free(void *p) {
  cw__fx_tree *ft = p;
  cw_array_free(&ft->systems);
  cw_array_free(&ft->attractors);
  cw_array_free(&ft->colliders);
  cw_array_free(&ft->trails);
  cw_free(ft);
}

cw__fx_tree *cw__fx_tree_of(cw_tree *t) {
  cw__fx_tree *ft = cw_tree_slot(t, "fx");
  if (!ft) {
    ft = cw_calloc(1, sizeof *ft, CW_MEM_RENDER);
    ft->tree = t;
    cw_tree_set_slot(t, "fx", ft, fx_tree_free);
  }
  return ft;
}

void cw__fx_remove_node(void *arr, cw_node n) {
  CW_ARRAY(cw_node) *a = arr;
  for (int i = 0; i < a->count; i++)
    if (a->data[i].id == n.id) {
      memmove(a->data + i, a->data + i + 1, (size_t)(a->count - i - 1) * sizeof *a->data);
      a->count--;
      return;
    }
}

unsigned cw__fx_quad_vbo(void) {
  static GLuint vbo;
  if (vbo) return vbo;
  static const float q[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return vbo;
}

void cw_particles_get_stats(cw_tree *t, cw_particles_stats *out) {
  cw__fx_tree *ft = t ? cw_tree_slot(t, "fx") : NULL;
  if (!ft) {
    memset(out, 0, sizeof *out);
    return;
  }
  *out = ft->last_stats;
}
