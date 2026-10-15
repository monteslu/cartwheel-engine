/* Render pipeline GL helpers: targets, full-screen programs, uniforms,
 * capability probing and 3D LUTs. GLSL ES 3.00, the same on every host. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/platform.h"
#include "pipeline_internal.h"
#include "pipeline_wgpu.h"

static bool wg(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }

cw_pipeline_caps cw__caps;
static bool caps_done;

/* ── capabilities ───────────────────────────────────────────────────── */

void cw__pipeline_probe_caps(void) {
  if (caps_done) return;
  caps_done = true;
  if (wg()) {
    cw__pw_probe_caps(&cw__caps);
    cw_info("render pipeline (WebGPU): HDR %s, float blend yes, MSAA 4x, GPU timestamps %s", cw__caps.hdr_format,
            cw__caps.timer_query ? "yes" : "no");
    return;
  }
  while (glGetError() != GL_NO_ERROR) {}
  const char *ext = (const char *)glGetString(GL_EXTENSIONS);
  if (!ext) ext = "";
  GLint ms = 0;
  glGetIntegerv(GL_MAX_SAMPLES, &ms);
  cw__caps.max_samples = ms;
  cw__caps.float_blend = strstr(ext, "float_blend") != NULL;
  /* a float colour target is only trusted when an FBO with one is complete:
   * hosts list extensions they never enabled */
  cw_rt t;
  if (cw__rt_create(&t, 4, 4, CW_RT_RGBA16F, false)) {
    cw__caps.float_render = true;
    cw__rt_destroy(&t);
  }
  while (glGetError() != GL_NO_ERROR) {}
  cw__caps.hdr_format = cw__caps.float_render ? "RGBA16F" : "RGBA8";
  /* timer queries: the extension, and a query that begins without error.
   * Never in a cart (wasm): carts run on WebGL, and in Chromium on ANGLE's
   * GL backend over Mesa radeonsi, GL_TIME_ELAPSED queries make rendering
   * nondeterministic (the same command stream and data draw different pixels
   * in about one run in two over 720 frames of vis_city_check; none in 15
   * runs without them, none on ANGLE's Vulkan backend, none natively on the
   * same GPU), and the RX 7600 page-faulted (a shader reading garbage
   * addresses) only after wasmcart 3269d64 enabled them. Pass GPU times are
   * profiling, so carts report none (gpu_ms -1) rather than risk a user's GPU. */
#if defined(__wasm__)
  if (false) {
#else
  if (strstr(ext, "disjoint_timer_query")) {
#endif
    GLuint q = 0;
    glGenQueries(1, &q);
    glBeginQuery(GL_TIME_ELAPSED_EXT, q);
    bool ok = glGetError() == GL_NO_ERROR;
    if (ok) glEndQuery(GL_TIME_ELAPSED_EXT);
    glDeleteQueries(1, &q);
    while (glGetError() != GL_NO_ERROR) ok = false;
    cw__caps.timer_query = ok;
  }
  cw_info("render pipeline: HDR %s, float blend %s, MSAA up to %d, GPU timers %s", cw__caps.hdr_format,
          cw__caps.float_blend ? "yes" : "no", cw__caps.max_samples, cw__caps.timer_query ? "yes" : "no");
}

const cw_pipeline_caps *cw_pipeline_caps_get(void) {
  cw__pipeline_probe_caps();
  return &cw__caps;
}

/* ── targets ────────────────────────────────────────────────────────── */

static void rt_formats(cw_rt_format f, GLenum *ifmt, GLenum *fmt, GLenum *type) {
  switch ((int)f) {
  case CW_RT_RGBA16F: *ifmt = GL_RGBA16F; *fmt = GL_RGBA; *type = GL_HALF_FLOAT; break;
  case CW_RT_RG16F: *ifmt = GL_RG16F; *fmt = GL_RG; *type = GL_HALF_FLOAT; break;
  case CW_RT_R16F: *ifmt = GL_R16F; *fmt = GL_RED; *type = GL_HALF_FLOAT; break;
  case CW_RT_R8: *ifmt = GL_R8; *fmt = GL_RED; *type = GL_UNSIGNED_BYTE; break;
  case CW__RT_RG8: *ifmt = GL_RG8; *fmt = GL_RG; *type = GL_UNSIGNED_BYTE; break;
  default: *ifmt = GL_RGBA8; *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE; break;
  }
}

bool cw__rt_create(cw_rt *rt, int w, int h, cw_rt_format f, bool depth) {
  if (wg()) return cw__pw_rt_create(rt, w, h, f, depth);
  memset(rt, 0, sizeof *rt);
  rt->w = w;
  rt->h = h;
  rt->format = f;
  GLenum ifmt, fmt, type;
  rt_formats(f, &ifmt, &fmt, &type);
  glGenTextures(1, &rt->tex);
  glBindTexture(GL_TEXTURE_2D, rt->tex);
  glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, type, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (depth) {
    glGenTextures(1, &rt->depth_tex);
    glBindTexture(GL_TEXTURE_2D, rt->depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &rt->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->tex, 0);
  if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, rt->depth_tex, 0);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    cw__rt_destroy(rt);
    return false;
  }
  /* GLES leaves new storage undefined (WebGL clears it): clear once */
  glViewport(0, 0, w, h);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glClearColor(0, 0, 0, 0);
  glClearDepthf(1);
  glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
  return true;
}

void cw__rt_destroy(cw_rt *rt) {
  if (wg()) {
    cw__pw_rt_destroy(rt);
    return;
  }
  if (rt->fbo) glDeleteFramebuffers(1, &rt->fbo);
  if (rt->tex) glDeleteTextures(1, &rt->tex);
  if (rt->depth_tex) glDeleteTextures(1, &rt->depth_tex);
  memset(rt, 0, sizeof *rt);
}

cw_rt *cw_rt_new(int w, int h, cw_rt_format f, bool with_depth) {
  cw_rt *rt = cw_calloc(1, sizeof *rt, CW_MEM_RENDER);
  if (!cw__rt_create(rt, CW_MAX(w, 1), CW_MAX(h, 1), f, with_depth)) {
    cw_error("cw_rt_new: a %dx%d target of format %d is not renderable on this host", w, h, (int)f);
    cw_free(rt);
    return NULL;
  }
  return rt;
}

void cw_rt_free(cw_rt *rt) {
  if (!rt) return;
  cw__rt_destroy(rt);
  cw_free(rt);
}

/* ── programs ───────────────────────────────────────────────────────── */

static const char *FS_HEADER =
    "#version 300 es\nprecision highp float;\nprecision highp int;\n"
    /* samplers default to lowp in fragment shaders: depth and HDR need highp */
    "precision highp sampler2D;\nprecision highp sampler3D;\n";
static const char *FULLSCREEN_VS =
    "#version 300 es\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  v_uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static GLuint compile(GLenum kind, const char *a, const char *b, const char *name) {
  GLuint s = glCreateShader(kind);
  const char *src[2] = { a, b };
  glShaderSource(s, b ? 2 : 1, src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = { 0 };
    glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
    cw_error("pipeline pass '%s': %s shader failed to compile: %s", name, kind == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static unsigned program_glsl(const char *name, const char *vsrc, const char *fs) {
  if (wg()) {
    cw_error("pipeline pass '%s': cw_pass_program is GLSL (GL); this run renders with WebGPU: use cw_pass_program_wgsl", name);
    return 0;
  }
  GLuint vs = vsrc ? compile(GL_VERTEX_SHADER, "#version 300 es\n", vsrc, name) : compile(GL_VERTEX_SHADER, FULLSCREEN_VS, NULL, name);
  char *full = NULL;
  size_t hl = strlen(FS_HEADER), fl = strlen(fs);
  full = cw_alloc(hl + 20 + fl + 1, CW_MEM_RENDER);
  memcpy(full, FS_HEADER, hl);
  memcpy(full + hl, "in vec2 v_uv;\n", 14);
  memcpy(full + hl + 14, fs, fl + 1);
  GLuint f = compile(GL_FRAGMENT_SHADER, full, NULL, name);
  cw_free(full);
  if (!vs || !f) {
    if (vs) glDeleteShader(vs);
    if (f) glDeleteShader(f);
    return 0;
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(vs);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048] = { 0 };
    glGetProgramInfoLog(p, sizeof log - 1, NULL, log);
    cw_error("pipeline pass '%s': link failed: %s", name, log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

unsigned cw_pass_program(const char *name, const char *fs) { return program_glsl(name, NULL, fs); }
unsigned cw_pass_program_vs(const char *name, const char *vs, const char *fs) { return program_glsl(name, vs, fs); }

unsigned cw_pass_program_wgsl(const char *name, const char *wgsl) {
  if (!wg()) {
    cw_error("pipeline pass '%s': cw_pass_program_wgsl needs a WebGPU run (this one renders with GL)", name);
    return 0;
  }
  return cw__pw_program(name, wgsl);
}

unsigned cw__pass_program2(const char *name, const char *glsl, const char *wgsl) {
  return wg() ? cw__pw_program(name, wgsl) : cw_pass_program(name, glsl);
}

void cw_pass_use(unsigned prog) {
  if (wg()) cw__pw_use(prog);
  else glUseProgram(prog);
}

void cw_pass_set_blend(cw_pass_blend b) {
  if (wg()) {
    cw__pw_set_blend(b);
    return;
  }
  if (b == CW_PASS_BLEND_NONE) {
    glDisable(GL_BLEND);
    return;
  }
  glEnable(GL_BLEND);
  if (b == CW_PASS_BLEND_ADD) glBlendFunc(GL_ONE, GL_ONE);
  else if (b == CW_PASS_BLEND_ALPHA) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void cw_pass_set_depth(bool test, bool write) {
  if (wg()) {
    cw__pw_set_depth(test, write);
    return;
  }
  if (!test && !write) {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    return;
  }
  /* GL writes depth only with the test on: "write without test" is ALWAYS */
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(test ? GL_LESS : GL_ALWAYS);
  glDepthMask(write ? GL_TRUE : GL_FALSE);
}

void cw_pass_clear(float r, float g, float b, float a, bool depth) {
  if (wg()) {
    cw__pw_clear(r, g, b, a, depth);
    return;
  }
  glClearColor(r, g, b, a);
  if (depth) {
    glDepthMask(GL_TRUE);
    glClearDepthf(1);
  }
  glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
}

static GLuint fs_vao;
void cw_pass_draw(int vertices) {
  if (vertices <= 0) return;
  if (wg()) {
    cw__pw_draw(vertices);
    return;
  }
  if (!fs_vao) glGenVertexArrays(1, &fs_vao);
  glBindVertexArray(fs_vao);
  glDrawArrays(GL_TRIANGLES, 0, vertices);
}
void cw_pass_fullscreen(void) { cw_pass_draw(3); }

void cw_pass_bind_texture(int unit, unsigned tex) {
  if (wg()) {
    cw__pw_bind_texture(unit, tex);
    return;
  }
  glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
  glBindTexture(GL_TEXTURE_2D, tex);
}

void cw_pass_bind_texture_3d(int unit, unsigned tex) {
  if (wg()) {
    cw__pw_bind_texture(unit, tex);
    return;
  }
  glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
  glBindTexture(GL_TEXTURE_3D, tex);
}

void cw_pass_bind_target(cw_pass_ctx *ctx, const cw_rt *rt) {
  if (!rt) rt = ctx->output;
  if (wg()) {
    cw__pw_bind_target(rt);
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
  glViewport(0, 0, rt->w, rt->h);
}

void cw__pass_state_reset(void) {
  if (wg()) {
    cw__pw_state_reset();
    return;
  }
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void cw_pass_linear_depth_params(const cw_pass_ctx *ctx, float out[3]) {
  const t3_camera *c = ctx->camera;
  out[0] = c ? c->near : 0.1f;
  out[1] = c ? c->far : 100.0f;
  out[2] = c && c->base.type == T3_ORTHOGRAPHIC_CAMERA ? 1.0f : 0.0f;
}

/* uniform locations by (program, name) */
static cw_map uniform_cache;
static bool uniform_cache_init;
static GLint uloc(unsigned prog, const char *name) {
  if (!uniform_cache_init) {
    cw_map_init(&uniform_cache, CW_MEM_RENDER);
    uniform_cache_init = true;
  }
  uint64_t h = 1469598103934665603ull ^ (uint64_t)prog * 0x9E3779B97F4A7C15ull;
  for (const char *c = name; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
  uint64_t v;
  if (cw_map_get(&uniform_cache, h, &v)) return (GLint)v - 1;
  GLint l = glGetUniformLocation(prog, name);
  cw_map_set(&uniform_cache, h, (uint64_t)(l + 1));
  return l;
}
void cw_pass_uniform1f(unsigned p, const char *n, float v) {
  if (wg()) cw__pw_uniform(p, n, 1, &v, 0);
  else glUniform1f(uloc(p, n), v);
}
void cw_pass_uniform2f(unsigned p, const char *n, float x, float y) {
  float v[2] = { x, y };
  if (wg()) cw__pw_uniform(p, n, 2, v, 0);
  else glUniform2f(uloc(p, n), x, y);
}
void cw_pass_uniform3f(unsigned p, const char *n, float x, float y, float z) {
  float v[3] = { x, y, z };
  if (wg()) cw__pw_uniform(p, n, 3, v, 0);
  else glUniform3f(uloc(p, n), x, y, z);
}
void cw_pass_uniform4f(unsigned p, const char *n, float x, float y, float z, float w) {
  float v[4] = { x, y, z, w };
  if (wg()) cw__pw_uniform(p, n, 4, v, 0);
  else glUniform4f(uloc(p, n), x, y, z, w);
}
void cw_pass_uniform1i(unsigned p, const char *n, int v) {
  if (wg()) cw__pw_uniform(p, n, -1, NULL, v);
  else glUniform1i(uloc(p, n), v);
}
void cw_pass_uniform_mat4(unsigned p, const char *n, const t3_mat4 *m) {
  if (wg()) cw__pw_uniform(p, n, 16, m->e, 0);
  else glUniformMatrix4fv(uloc(p, n), 1, GL_FALSE, m->e);
}

/* ── copy ───────────────────────────────────────────────────────────── */

static GLuint copy_prog;
void cw__pass_copy(cw_pass_ctx *ctx, unsigned src_tex, const cw_rt *dst) {
  if (!copy_prog)
    copy_prog = cw__pass_program2("copy", "uniform sampler2D u_src;\nout vec4 o;\nvoid main() { o = texture(u_src, v_uv); }\n",
                                  "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
                                  "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
                                  "  return textureSampleLevel(u_src, cw_linear, uv, 0.0);\n}\n");
  cw_pass_bind_target(ctx, dst);
  cw__pass_state_reset();
  cw_pass_use(copy_prog);
  cw_pass_bind_texture(0, src_tex);
  cw_pass_uniform1i(copy_prog, "u_src", 0);
  cw_pass_fullscreen();
}

/* ── 3D LUTs ────────────────────────────────────────────────────────── */

typedef struct lut_rec { GLuint tex; int size; bool live; float dmin[3], dmax[3]; } lut_rec;
static CW_ARRAY(lut_rec) luts;

unsigned cw__lut_texture(uint64_t lut, int *size, float dmin[3], float dmax[3]) {
  if (!lut || lut > (uint64_t)luts.count || !luts.data[lut - 1].live) return 0;
  const lut_rec *l = &luts.data[lut - 1];
  if (size) *size = l->size;
  if (dmin) memcpy(dmin, l->dmin, sizeof l->dmin);
  if (dmax) memcpy(dmax, l->dmax, sizeof l->dmax);
  return l->tex;
}

uint64_t cw_lut_from_rgb(int size, const float *rgb) {
  if (size < 2 || size > 256) {
    cw_error("cw_lut_from_rgb: size %d out of range (2..256)", size);
    return 0;
  }
  size_t n = (size_t)size * size * size;
  uint16_t *half = cw_alloc(n * 4 * sizeof(uint16_t), CW_MEM_RENDER);
  for (size_t i = 0; i < n; i++)
    for (int c = 0; c < 4; c++) {
      float f = c < 3 ? rgb[i * 3 + c] : 1.0f;
      /* float to half (round to nearest even is not needed for grading) */
      union { float f; uint32_t u; } b = { f };
      uint32_t s = (b.u >> 16) & 0x8000u, e = (b.u >> 23) & 0xffu, m = b.u & 0x7fffffu;
      uint16_t hv;
      if (e < 103) hv = (uint16_t)s;
      else if (e > 142) hv = (uint16_t)(s | 0x7c00u);
      else if (e < 113) hv = (uint16_t)(s | ((m | 0x800000u) >> (126 - e)));
      else hv = (uint16_t)(s | ((e - 112) << 10) | (m >> 13));
      half[i * 4 + c] = hv;
    }
  GLuint t;
  if (wg()) {
    t = cw__pw_lut(size, half);
    cw_free(half);
    cw_array_push(&luts, ((lut_rec){ t, size, true, { 0, 0, 0 }, { 1, 1, 1 } }));
    return (uint64_t)luts.count;
  }
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_3D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, size, size, size, 0, GL_RGBA, GL_HALF_FLOAT, half);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_3D, 0);
  cw_free(half);
  cw_array_push(&luts, ((lut_rec){ t, size, true, { 0, 0, 0 }, { 1, 1, 1 } }));
  return (uint64_t)luts.count;
}

void cw_lut_free(uint64_t lut) {
  if (!cw__lut_texture(lut, NULL, NULL, NULL)) return;
  if (wg()) cw__pw_tex_free(luts.data[lut - 1].tex);
  else glDeleteTextures(1, &luts.data[lut - 1].tex);
  luts.data[lut - 1].live = false;
}

uint64_t cw_lut_parse_cube(const char *text, size_t len, const char *who) {
  int size = 0, got = 0;
  float dmin[3] = { 0, 0, 0 }, dmax[3] = { 1, 1, 1 };
  float *rgb = NULL;
  size_t i = 0;
  int line_no = 0;
  while (i < len) {
    size_t s = i;
    while (i < len && text[i] != '\n') i++;
    size_t e = i;
    if (i < len) i++;
    line_no++;
    while (s < e && (text[s] == ' ' || text[s] == '\t')) s++;
    if (s >= e || text[s] == '#' || text[s] == '\r') continue;
    char line[256];
    size_t n = CW_MIN(e - s, sizeof line - 1);
    memcpy(line, text + s, n);
    line[n] = 0;
    if (!strncmp(line, "TITLE", 5) || !strncmp(line, "LUT_1D_SIZE", 11) || !strncmp(line, "LUT_1D_INPUT_RANGE", 18)) {
      if (!strncmp(line, "LUT_1D", 6)) {
        cw_error("%s: 1D LUTs are not supported (line %d); export a 3D .cube", who, line_no);
        cw_free(rgb);
        return 0;
      }
      continue;
    }
    if (!strncmp(line, "LUT_3D_SIZE", 11)) {
      size = atoi(line + 11);
      if (size < 2 || size > 64) {
        cw_error("%s: LUT_3D_SIZE %d out of range (2..64)", who, size);
        return 0;
      }
      rgb = cw_alloc((size_t)size * size * size * 3 * sizeof(float), CW_MEM_RENDER);
      continue;
    }
    if (!strncmp(line, "DOMAIN_MIN", 10)) { sscanf(line + 10, "%f %f %f", &dmin[0], &dmin[1], &dmin[2]); continue; }
    if (!strncmp(line, "DOMAIN_MAX", 10)) { sscanf(line + 10, "%f %f %f", &dmax[0], &dmax[1], &dmax[2]); continue; }
    if (!strncmp(line, "LUT_3D_INPUT_RANGE", 18)) {
      float a, b;
      if (sscanf(line + 18, "%f %f", &a, &b) == 2) for (int c = 0; c < 3; c++) dmin[c] = a, dmax[c] = b;
      continue;
    }
    float r, g, b;
    if (sscanf(line, "%f %f %f", &r, &g, &b) == 3) {
      if (!rgb) {
        cw_error("%s: data before LUT_3D_SIZE (line %d)", who, line_no);
        return 0;
      }
      if (got >= size * size * size) {
        cw_error("%s: more than %d^3 entries (line %d)", who, size, line_no);
        cw_free(rgb);
        return 0;
      }
      rgb[got * 3] = r;
      rgb[got * 3 + 1] = g;
      rgb[got * 3 + 2] = b;
      got++;
      continue;
    }
    cw_warn("%s: line %d ignored: %s", who, line_no, line);
  }
  if (!rgb || got != size * size * size) {
    cw_error("%s: %d entries, expected %d (LUT_3D_SIZE %d)", who, got, size * size * size, size);
    cw_free(rgb);
    return 0;
  }
  for (int c = 0; c < 3; c++)
    if (!(dmax[c] > dmin[c])) {
      cw_error("%s: DOMAIN_MAX must exceed DOMAIN_MIN", who);
      cw_free(rgb);
      return 0;
    }
  uint64_t id = cw_lut_from_rgb(size, rgb);
  if (id) {
    memcpy(luts.data[id - 1].dmin, dmin, sizeof dmin);
    memcpy(luts.data[id - 1].dmax, dmax, sizeof dmax);
  }
  cw_free(rgb);
  return id;
}

uint64_t cw_lut_load_cube(const char *path) {
  size_t len = 0;
  char *text = cw_asset_load(path, &len);
  if (!text) {
    cw_error("cw_lut_load_cube: asset '%s' not found", path);
    return 0;
  }
  uint64_t id = cw_lut_parse_cube(text, len, path);
  free(text);
  return id;
}
