/* render2d effects layer (fx.h): targets of any format and full-screen
 * passes. The GL side lives here; the WebGPU side in wgpu.c. */
#include <string.h>

#include "fx.h"
#include "cartwheel/pipeline.h"

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif

static CW_ARRAY(fx_prog *) progs; /* every program built, for shutdown */
static GLuint fx_vao, black_tex;

fx_fmt fx_hdr_format(void) {
  if (r2d_wg()) return FX_RGBA16F;
  const cw_pipeline_caps *c = cw_pipeline_caps_get();
  return c && c->float_render && c->float_blend ? FX_RGBA16F : FX_RGBA8;
}

bool fx_small_hdr_ok(void) {
  static int ok = -1;
  if (ok >= 0) return ok == 1;
  if (r2d_wg()) {
    ok = r2d_wg_small_hdr_ok() ? 1 : 0;
    return ok == 1;
  }
  ok = 0;
  if (fx_hdr_format() != FX_RGBA16F) return false;
  GLuint t = 0, f = 0;
  glGenTextures(1, &t);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, 0x8C3A /* GL_R11F_G11F_B10F */, 4, 4, 0, GL_RGB, 0x8C3B /* GL_UNSIGNED_INT_10F_11F_11F_REV */, NULL);
  glGenFramebuffers(1, &f);
  glBindFramebuffer(GL_FRAMEBUFFER, f);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
  ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE ? 1 : 0;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &f);
  glDeleteTextures(1, &t);
  return ok == 1;
}

void fx_rt_free(fx_rt *rt) {
  if (!rt) return;
  if (rt->wtex) r2d_wg_tex_release(rt->wtex);
  if (rt->fbo) glDeleteFramebuffers(1, &rt->fbo);
  if (rt->tex) glDeleteTextures(1, &rt->tex);
  memset(rt, 0, sizeof *rt);
}

void fx_rt_ensure(fx_rt *rt, int w, int h, fx_fmt f) {
  w = CW_MAX(1, w);
  h = CW_MAX(1, h);
  if ((rt->tex || rt->wtex) && rt->w == w && rt->h == h && rt->fmt == f) return;
  fx_rt_free(rt);
  rt->w = w;
  rt->h = h;
  rt->fmt = f;
  if (r2d_wg()) {
    rt->wtex = r2d_wg_fx_tex(w, h, f);
    return;
  }
  glGenTextures(1, &rt->tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, rt->tex);
  if (f == FX_RGBA16F) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
  else if (f == FX_RG11B10F) glTexImage2D(GL_TEXTURE_2D, 0, 0x8C3A, w, h, 0, GL_RGB, 0x8C3B, NULL);
  else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &rt->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->tex, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

fx_dst fx_dst_of(const fx_rt *rt) {
  fx_dst d = { .w = rt->w, .h = rt->h, .fbo = rt->fbo, .offscreen = true };
  if (r2d_wg()) {
    d.wview = r2d_wg_fx_view(rt->wtex);
    d.wfmt = r2d_wg_fx_format(rt->fmt);
  }
  return d;
}

static const char *FX_VS =
    "#version 300 es\nprecision highp float;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  v_uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *FX_FS_HEAD =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
    "uniform vec4 u_v[8];\nuniform vec2 u_size;\n"
    "uniform sampler2D t0, t1, t2, t3, t4, t5;\n"
    "in vec2 v_uv;\nout vec4 o_col;\n";

static GLuint stage(GLenum type, const char *const *parts, int n, const char *name) {
  GLuint s = glCreateShader(type);
  GLint lens[4];
  for (int i = 0; i < n; i++) lens[i] = (GLint)strlen(parts[i]);
  glShaderSource(s, n, parts, lens);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    GLsizei len = 0;
    glGetShaderInfoLog(s, sizeof log, &len, log);
    cw_error("render2d: fx %s %s shader failed: %.*s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", (int)len, log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static bool gl_build(fx_prog *p) {
  const char *vs[1] = { FX_VS }, *fs[2] = { FX_FS_HEAD, p->glsl };
  GLuint v = stage(GL_VERTEX_SHADER, vs, 1, p->name), f = stage(GL_FRAGMENT_SHADER, fs, 2, p->name);
  if (!v || !f) return false;
  GLuint prog = glCreateProgram();
  glAttachShader(prog, v);
  glAttachShader(prog, f);
  glLinkProgram(prog);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    cw_error("render2d: fx %s program failed to link", p->name);
    glDeleteProgram(prog);
    return false;
  }
  p->gl = prog;
  p->u_v = glGetUniformLocation(prog, "u_v");
  p->u_size = glGetUniformLocation(prog, "u_size");
  glUseProgram(prog);
  static const char *tn[FX_SRC] = { "t0", "t1", "t2", "t3", "t4", "t5" };
  for (int k = 0; k < FX_SRC; k++) {
    GLint l = glGetUniformLocation(prog, tn[k]);
    if (l >= 0) glUniform1i(l, k < 4 ? 3 + k : 5 + k); /* units 3..6, 9..10: the sprite programs keep 0..2, the lights 7 */
  }
  return true;
}

void fx_pass(fx_prog *p, const fx_dst *dst, bool clear, const float cc[4], const fx_rt *const src[4], const float v[8][4],
             int blend) {
  const fx_rt *s6[FX_SRC] = { src ? src[0] : NULL, src ? src[1] : NULL, src ? src[2] : NULL, src ? src[3] : NULL, NULL, NULL };
  fx_pass6(p, dst, clear, cc, s6, v, blend);
}

void fx_pass6(fx_prog *p, const fx_dst *dst, bool clear, const float cc[4], const fx_rt *const src[FX_SRC], const float v[8][4],
              int blend) {
  if (p->failed) return;
  bool known = false;
  for (int i = 0; i < progs.count; i++) known |= progs.data[i] == p;
  if (!known) cw_array_push(&progs, p);
  if (r2d_wg()) {
    r2d_wg_fx_pass(p, dst, clear, cc, src, v, blend);
    return;
  }
  if (!p->gl && !gl_build(p)) {
    p->failed = true;
    return;
  }
  if (!fx_vao) glGenVertexArrays(1, &fx_vao);
  if (!black_tex) {
    static const uint8_t z[4] = { 0, 0, 0, 0 };
    glGenTextures(1, &black_tex);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, black_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, z);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
  int vx = 0, vy = 0, vw = dst->w, vh = dst->h;
  if (dst->vp[2] > 0) vx = dst->vp[0], vy = dst->vp[1], vw = dst->vp[2], vh = dst->vp[3];
  glViewport(vx, vy, vw, vh);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (clear) {
    glClearColor(cc ? cc[0] : 0, cc ? cc[1] : 0, cc ? cc[2] : 0, cc ? cc[3] : 0);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  if (blend == FX_BLEND_NONE) glDisable(GL_BLEND);
  else {
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    if (blend == FX_BLEND_ADD) glBlendFunc(GL_ONE, GL_ONE);
    else glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  }
  glUseProgram(p->gl);
  if (p->u_v >= 0) {
    static const float zero[8][4];
    glUniform4fv(p->u_v, 8, v ? &v[0][0] : &zero[0][0]);
  }
  if (p->u_size >= 0) glUniform2f(p->u_size, (float)vw, (float)vh);
  for (int k = 0; k < FX_SRC; k++) {
    glActiveTexture(GL_TEXTURE0 + (k < 4 ? 3 + k : 5 + k));
    glBindTexture(GL_TEXTURE_2D, src && src[k] && src[k]->tex ? src[k]->tex : black_tex);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(fx_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  R.stats.draw_calls++;
  glBindVertexArray(0);
}

void fx_shutdown(void) {
  for (int i = 0; i < progs.count; i++) {
    fx_prog *p = progs.data[i];
    if (p->gl) glDeleteProgram(p->gl);
    r2d_wg_fx_prog_free(p);
    p->gl = 0;
    p->wmod = NULL;
    p->failed = false;
  }
  cw_array_free(&progs);
  if (fx_vao) glDeleteVertexArrays(1, &fx_vao);
  if (black_tex) glDeleteTextures(1, &black_tex);
  fx_vao = black_tex = 0;
}
