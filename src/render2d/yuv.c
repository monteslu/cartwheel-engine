/* Video frames into render targets: Y, Cb and Cr planes (4:2:0, as MPEG-1
 * decodes them) uploaded as three one-channel textures and converted to RGB
 * in a fragment shader (BT.601, studio range) on GL and WebGPU. src/video
 * is the user; nothing here is public.
 *
 * The target stores rows bottom-up, as every render target does (both
 * backends: render2d.h), so the shader reads the video row
 * height - 1 - (fragment row) on both, and the target draws upright as a
 * sprite. Chroma is read at the nearest sample (no filtering): integer
 * texel fetches, the same on every host. */
#include <string.h>

#include "internal.h"
#include "yuv.h"

#define GLSL_HEADER "#version 300 es\nprecision highp float;\nprecision highp int;\n"

static const char *YUV_VS = GLSL_HEADER
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *YUV_FS = GLSL_HEADER
    "uniform sampler2D u_y, u_cb, u_cr;\n"
    "uniform vec4 u_size;\n" /* frame width, height */
    "out vec4 o_col;\n"
    "void main() {\n"
    "  int x = int(gl_FragCoord.x), y = int(u_size.y) - 1 - int(gl_FragCoord.y);\n"
    "  float Y = texelFetch(u_y, ivec2(x, y), 0).r;\n"
    "  float cb = texelFetch(u_cb, ivec2(x / 2, y / 2), 0).r - 0.5;\n"
    "  float cr = texelFetch(u_cr, ivec2(x / 2, y / 2), 0).r - 0.5;\n"
    "  float l = (Y - 0.0627451) * 1.164383;\n"
    "  o_col = vec4(clamp(vec3(l + 1.596027 * cr, l - 0.391762 * cb - 0.812968 * cr, l + 2.017232 * cb), 0.0, 1.0), 1.0);\n"
    "}\n";

struct cw__yuv {
  int w, h;          /* frame */
  int pw[3], ph[3];  /* planes (macroblock-padded) */
  cw_texture target;
  GLuint tex[3];
  void *wg;          /* WebGPU side (wgpu.c) */
};

static GLuint yuv_prog;
static GLint yuv_u_size;

static GLuint stage(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    GLsizei n = 0;
    glGetShaderInfoLog(s, sizeof log, &n, log);
    cw_error("video: YUV shader failed to compile: %.*s", (int)n, log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static bool gl_program(void) {
  if (yuv_prog) return true;
  GLuint v = stage(GL_VERTEX_SHADER, YUV_VS), f = stage(GL_FRAGMENT_SHADER, YUV_FS);
  if (!v || !f) return false;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    cw_error("video: YUV program failed to link");
    glDeleteProgram(p);
    return false;
  }
  glUseProgram(p);
  glUniform1i(glGetUniformLocation(p, "u_y"), 0);
  glUniform1i(glGetUniformLocation(p, "u_cb"), 1);
  glUniform1i(glGetUniformLocation(p, "u_cr"), 2);
  yuv_u_size = glGetUniformLocation(p, "u_size");
  yuv_prog = p;
  return true;
}

cw__yuv *cw__yuv_new(int w, int h, int yw, int yh) {
  cw_texture t = cw_target_create(&(cw_target_desc){ .width = w, .height = h, .filter = CW_FILTER_LINEAR });
  if (CW_HANDLE_IS_NULL(t)) return NULL;
  cw__yuv *y = cw_calloc(1, sizeof *y, CW_MEM_RENDER2D);
  y->w = w;
  y->h = h;
  y->target = t;
  y->pw[0] = yw, y->ph[0] = yh;
  y->pw[1] = y->pw[2] = yw / 2;
  y->ph[1] = y->ph[2] = yh / 2;
  if (r2d_wg()) {
    y->wg = r2d_wg_yuv_new(y->pw, y->ph);
    return y;
  }
  if (!gl_program()) {
    cw_texture_destroy(t);
    cw_free(y);
    return NULL;
  }
  glGenTextures(3, y->tex);
  for (int i = 0; i < 3; i++) {
    glBindTexture(GL_TEXTURE_2D, y->tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, y->pw[i], y->ph[i], 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  return y;
}

cw_texture cw__yuv_target(const cw__yuv *y) { return y ? y->target : (cw_texture){ 0 }; }

void cw__yuv_convert(cw__yuv *y, const uint8_t *const planes[3]) {
  if (!y) return;
  if (r2d_wg()) {
    r2d_tex *t = r2d_tex_get(y->target);
    if (t) r2d_wg_yuv_convert(y->wg, planes, R.arrays.data[t->array].wtex, y->w, y->h);
    return;
  }
  /* GL: like a 2D pass (render2d.h), this sets what it uses and leaves it
   * changed: framebuffer (back to 0), viewport, program, VAO 0, blend,
   * scissor, depth, cull and stencil off, texture units 0..2 */
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)i);
    glBindTexture(GL_TEXTURE_2D, y->tex[i]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, y->pw[i], y->ph[i], GL_RED, GL_UNSIGNED_BYTE, planes[i]);
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glBindFramebuffer(GL_FRAMEBUFFER, cw_target_fbo(y->target));
  glViewport(0, 0, y->w, y->h);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glUseProgram(yuv_prog);
  glUniform4f(yuv_u_size, (float)y->w, (float)y->h, 0, 0);
  glBindVertexArray(0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glActiveTexture(GL_TEXTURE0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void r2d_yuv_gl_shutdown(void) {
  if (yuv_prog) glDeleteProgram(yuv_prog);
  yuv_prog = 0;
}

void cw__yuv_free(cw__yuv *y) {
  if (!y) return;
  if (y->wg) r2d_wg_yuv_free(y->wg);
  if (y->tex[0]) glDeleteTextures(3, y->tex);
  cw_texture_destroy(y->target);
  cw_free(y);
}
