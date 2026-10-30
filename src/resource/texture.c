/* Texture resources: PNG/JPEG (stb_image, inside three.c) and KTX2 (Basis
 * Universal, transcoded to what the context supports), decoded on workers,
 * uploaded on the main thread straight into a three.c texture's GL object
 * (on WebGPU into its texels, which three.c uploads). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "noise/noise_internal.h"
#include "resource_internal.h"
#include "three.h"

/* stb_image is compiled into three.c (gltf.c) */
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file,
                                     int desired_channels);
void stbi_image_free(void *retval_from_stbi_load);
const char *stbi_failure_reason(void);
/* three.c's GL release hook (set once a renderer exists) */

#define GL_COMPRESSED_RGBA_BPTC_UNORM_EXT_ 0x8E8C
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR_ 0x93B0
#define GL_COMPRESSED_RGBA8_ETC2_EAC_ 0x9278
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT_ 0x83F3
/* their sRGB forms: a colour texture (three.js r187 colorSpace SRGB) is stored
 * sRGB and GL decodes on sampling, as three.c's own GL uploads do */
#define GL_SRGB8_ALPHA8_ 0x8C43
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT_ 0x8E8D
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR_ 0x93D0
#define GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC_ 0x9279
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT_ 0x8C4F
#define GL_TEXTURE_MAX_ANISOTROPY_EXT_ 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT_ 0x84FF

static int ktx2_target = -1;
static const char *ktx2_target_name = "none";
const char *cw_res_ktx2_target(void) { return ktx2_target_name; }

static bool has_ext(const char *exts, const char *name) {
  size_t n = strlen(name);
  for (const char *p = exts; p && (p = strstr(p, name)); p += n)
    if ((p == exts || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
  return false;
}

/* Hosts may list an extension the driver behind them lacks, or accept an
 * upload they then sample as black (a lying wrapper swallows the error), so
 * a candidate format is proven by drawing: the probe KTX2 is transcoded to
 * it, uploaded, sampled into a 4x4 RGBA8 framebuffer and read back. Every
 * piece of GL state touched is put back (three.c caches it). */
#include "ktx2_probe.h"
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT 0x0BA2
#endif

static GLuint probe_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  return s;
}

static bool probe_format(int target, GLenum fmt) {
  cw_tex_cpu cpu;
  char err[160];
  if (!cw__ktx2_transcode(cw_ktx2_probe, sizeof cw_ktx2_probe, target, &cpu, err, sizeof err)) return false;
  GLint prog, vao, dfb, rfb, unit, tex0, vp[4], pack;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
  GLenum caps[] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };
  GLint on[5]; /* glGet of a capability is its enable state (glIsEnabled is not on every host) */
  for (int i = 0; i < 5; i++) { on[i] = 0; glGetIntegerv(caps[i], &on[i]); glDisable(caps[i]); }
  while (glGetError() != GL_NO_ERROR) {}
  GLuint t, rt, fb, va;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, 4, 4, 0, (GLsizei)cpu.level[0].size, cpu.level[0].data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  bool ok = glGetError() == GL_NO_ERROR;
  glGenTextures(1, &rt);
  glBindTexture(GL_TEXTURE_2D, rt);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fb);
  glBindFramebuffer(GL_FRAMEBUFFER, fb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
  GLuint vs = probe_shader(GL_VERTEX_SHADER, "#version 300 es\nvoid main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), "
                                             "float(gl_VertexID & 2)); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n");
  GLuint fs = probe_shader(GL_FRAGMENT_SHADER, "#version 300 es\nprecision mediump float; uniform sampler2D t; out vec4 o;\n"
                                               "void main() { o = texture(t, vec2(0.5)); }\n");
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  glUseProgram(p);
  glUniform1i(glGetUniformLocation(p, "t"), 0);
  glGenVertexArrays(1, &va);
  glBindVertexArray(va);
  glBindTexture(GL_TEXTURE_2D, t);
  glViewport(0, 0, 4, 4);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  uint8_t px[4] = { 0 };
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  ok = ok && abs(px[0] - 200) < 24 && abs(px[1] - 40) < 24 && abs(px[2] - 120) < 24;
  /* put everything back */
  glUseProgram((GLuint)prog);
  glBindVertexArray((GLuint)vao);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfb);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
  glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
  glActiveTexture((GLenum)unit);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glPixelStorei(GL_PACK_ALIGNMENT, pack);
  for (int i = 0; i < 5; i++)
    if (on[i]) glEnable(caps[i]);
  glDeleteVertexArrays(1, &va);
  glDeleteProgram(p);
  glDeleteShader(vs);
  glDeleteShader(fs);
  glDeleteFramebuffers(1, &fb);
  glDeleteTextures(1, &rt);
  glDeleteTextures(1, &t);
  cw__tex_cpu_free(&cpu);
  if (!ok) cw_log(CW_LOG_DEBUG, "textures: probe drew (%d, %d, %d) for format 0x%x", px[0], px[1], px[2], fmt);
  return ok;
}

void cw__tex_pick_ktx2_target(void) {
  if (ktx2_target >= 0) return;
  cw__ktx2_init();
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    /* three.c's WebGPU textures are uncompressed (and there is no GL context
     * to ask or probe: a dual cart traps on any GL call under WebGPU) */
    ktx2_target = CW_KTX2_RGBA32;
    ktx2_target_name = "rgba8";
    cw_info("textures: KTX2 transcodes to %s (WebGPU)", ktx2_target_name);
    return;
  }
  const char *exts = (const char *)glGetString(GL_EXTENSIONS);
  if (!exts) exts = "";
  const char *force = getenv("CW_KTX2_TARGET");
  struct { int t; const char *name; GLenum gl; bool avail; } cand[] = {
    { CW_KTX2_BC7, "bc7", GL_COMPRESSED_RGBA_BPTC_UNORM_EXT_,
      has_ext(exts, "GL_EXT_texture_compression_bptc") || has_ext(exts, "GL_ARB_texture_compression_bptc") ||
      has_ext(exts, "EXT_texture_compression_bptc") },
    { CW_KTX2_ASTC4x4, "astc4x4", GL_COMPRESSED_RGBA_ASTC_4x4_KHR_,
      has_ext(exts, "GL_KHR_texture_compression_astc_ldr") || has_ext(exts, "WEBGL_compressed_texture_astc") ||
      has_ext(exts, "GL_WEBGL_compressed_texture_astc") },
    /* ETC2 is core in GLES 3.0, but WebGL2 (and so a cart in a browser)
     * only has it with the extension */
    { CW_KTX2_ETC2, "etc2", GL_COMPRESSED_RGBA8_ETC2_EAC_,
      !CW_CART || has_ext(exts, "WEBGL_compressed_texture_etc") || has_ext(exts, "GL_WEBGL_compressed_texture_etc") },
    /* BC3 only with its sRGB form too (colour textures need it); BC7, ASTC
     * and ETC2 carry theirs in the same extension (ETC2: core) */
    { CW_KTX2_BC3, "bc3", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT_,
      (has_ext(exts, "GL_EXT_texture_compression_s3tc") || has_ext(exts, "WEBGL_compressed_texture_s3tc") ||
       has_ext(exts, "GL_WEBGL_compressed_texture_s3tc")) &&
      (has_ext(exts, "GL_EXT_texture_sRGB") || has_ext(exts, "GL_EXT_texture_compression_s3tc_srgb") ||
       has_ext(exts, "WEBGL_compressed_texture_s3tc_srgb") || has_ext(exts, "GL_WEBGL_compressed_texture_s3tc_srgb")) },
  };
  ktx2_target = CW_KTX2_RGBA32;
  ktx2_target_name = "rgba8";
  for (size_t i = 0; i < CW_ARRAY_LEN(cand); i++) {
    bool want = force ? !strncmp(force, cand[i].name, strlen(force) < 3 ? 3 : strlen(force)) : cand[i].avail;
    if (!want) continue;
    if (!probe_format(cand[i].t, cand[i].gl)) {
      cw_warn("textures: the context lists %s but does not sample it; trying the next format", cand[i].name);
      continue;
    }
    ktx2_target = cand[i].t;
    ktx2_target_name = cand[i].name;
    break;
  }
  cw_info("textures: KTX2 transcodes to %s", ktx2_target_name);
}

void cw__tex_cpu_free(cw_tex_cpu *t) {
  for (int i = 0; i < 16; i++) cw_free(t->level[i].data);
  memset(t, 0, sizeof *t);
}

bool cw__tex_decode(const char *path, const uint8_t *bytes, size_t len, cw_tex_cpu *out) {
  memset(out, 0, sizeof *out);
  if (!strncmp(path, "gen:/noise", 10)) { /* generated (cartwheel/noise.h); bytes are "noise?<query>" */
    const char *q = strchr(path, '?');
    uint8_t *px;
    int w, h;
    if (!cw__noise_image_query(q ? q + 1 : "", &px, &w, &h)) return false;
    out->level[0].data = px;
    out->level[0].size = (size_t)w * (size_t)h * 4;
    out->level[0].w = out->width = w;
    out->level[0].h = out->height = h;
    out->levels = 1;
    out->gl_format = GL_RGBA8;
    out->gen_mips = true;
    out->format = "rgba8";
    return true;
  }
  if (cw__ktx2_is(bytes, len)) {
    char err[200];
    int target = ktx2_target < 0 ? CW_KTX2_RGBA32 : ktx2_target;
    if (!cw__ktx2_transcode(bytes, len, target, out, err, sizeof err)) {
      cw_error("res://%s: %s", path, err);
      return false;
    }
    if (!out->compressed && out->levels == 1) out->gen_mips = true;
    return true;
  }
  int w, h, c;
  uint8_t *px = stbi_load_from_memory(bytes, (int)len, &w, &h, &c, 4);
  if (!px) {
    cw_error("res://%s: not an image stb_image or the KTX2 transcoder reads (%s)", path, stbi_failure_reason());
    return false;
  }
  size_t n = (size_t)w * h * 4;
  out->level[0].data = cw_alloc(n, CW_MEM_RESOURCE);
  memcpy(out->level[0].data, px, n);
  stbi_image_free(px);
  out->level[0].size = n;
  out->level[0].w = out->width = w;
  out->level[0].h = out->height = h;
  out->levels = 1;
  out->gl_format = GL_RGBA8;
  out->gen_mips = true;
  out->format = "rgba8";
  return true;
}

/* the GL internal format a texture's texels upload as: its sRGB form for a
 * colour texture (three.c's GL backend: colour management on, 8-bit) */
static GLenum gl_internal(const t3_texture *t, const cw_tex_cpu *cpu) {
  GLenum f = cpu->compressed ? (GLenum)cpu->gl_format : GL_RGBA8;
  if (t->color_space != T3_SRGB_COLOR_SPACE || !t3_get_color_management()) return f;
  switch (f) {
  case GL_RGBA8: return GL_SRGB8_ALPHA8_;
  case GL_COMPRESSED_RGBA_BPTC_UNORM_EXT_: return GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT_;
  case GL_COMPRESSED_RGBA_ASTC_4x4_KHR_: return GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR_;
  case GL_COMPRESSED_RGBA8_ETC2_EAC_: return GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC_;
  case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT_: return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT_;
  default: return f;
  }
}

static GLint gl_wrap(t3_wrapping w) { return w == T3_REPEAT ? GL_REPEAT : w == T3_MIRRORED_REPEAT ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE; }
static GLint gl_min(t3_filter f) {
  switch (f) {
  case T3_NEAREST: return GL_NEAREST;
  case T3_NEAREST_MIPMAP_NEAREST: return GL_NEAREST_MIPMAP_NEAREST;
  case T3_NEAREST_MIPMAP_LINEAR: return GL_NEAREST_MIPMAP_LINEAR;
  case T3_LINEAR: return GL_LINEAR;
  case T3_LINEAR_MIPMAP_NEAREST: return GL_LINEAR_MIPMAP_NEAREST;
  default: return GL_LINEAR_MIPMAP_LINEAR;
  }
}

size_t cw__tex_upload(t3_texture *t, cw_tex_cpu *cpu) {
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    /* three.c uploads the texels itself (and makes the mip chain when the
     * texture asks for one): level 0, RGBA8 (KTX2 transcodes to it here) */
    if (cpu->compressed || !cpu->levels) {
      cw_error("texture upload (%s %dx%d): WebGPU takes uncompressed RGBA8 only", cpu->format, cpu->width, cpu->height);
      return 0;
    }
    const cw_tex_level *lv = &cpu->level[0];
    size_t n = (size_t)lv->w * (size_t)lv->h * 4;
    uint8_t *px = malloc(n); /* three.c's to free (t3_texture.pixels) */
    if (!px) return 0;
    memcpy(px, lv->data, n);
    free(t->pixels);
    t->pixels = px;
    t->width = lv->w;
    t->height = lv->h;
    t->flip_y = false;
    t->version++;
    return t->generate_mipmaps ? n + n / 3 : n;
  }
  /* leave the binding and unpack state as three.c last set them: its
   * texture-unit cache must stay true */
  GLint prev_tex = 0, prev_unpack = 4;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_unpack);
  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  size_t bytes = 0;
  int levels = cpu->levels;
  bool mip_filter = t->min_filter != T3_NEAREST && t->min_filter != T3_LINEAR;
  GLenum ifmt = gl_internal(t, cpu);
  for (int l = 0; l < levels; l++) {
    cw_tex_level *lv = &cpu->level[l];
    if (cpu->compressed)
      glCompressedTexImage2D(GL_TEXTURE_2D, l, ifmt, lv->w, lv->h, 0, (GLsizei)lv->size, lv->data);
    else
      glTexImage2D(GL_TEXTURE_2D, l, (GLint)ifmt, lv->w, lv->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv->data);
    bytes += lv->size;
  }
  if (cpu->gen_mips && mip_filter && t->generate_mipmaps) {
    glGenerateMipmap(GL_TEXTURE_2D);
    bytes += bytes / 3;
  } else if (levels > 1 || !mip_filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
  }
  GLint min = gl_min(t->min_filter);
  if (levels == 1 && !(cpu->gen_mips && t->generate_mipmaps)) min = t->min_filter == T3_NEAREST || t->min_filter == T3_NEAREST_MIPMAP_NEAREST || t->min_filter == T3_NEAREST_MIPMAP_LINEAR ? GL_NEAREST : GL_LINEAR;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(t->wrap_s));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(t->wrap_t));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->mag_filter == T3_NEAREST ? GL_NEAREST : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
  if (t->anisotropy > 1) {
    GLfloat mx = 1;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT_, &mx);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT_, t->anisotropy < mx ? t->anisotropy : mx);
  }
  GLenum e = glGetError();
  if (e != GL_NO_ERROR) cw_error("texture upload (%s %dx%d): GL error 0x%x", cpu->format, cpu->width, cpu->height, e);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_unpack);
  /* hand the GL texture to three.c (its public adopt API frees the one it made
   * for the placeholder and never re-uploads or re-parameters ours) */
  int adopted_levels = levels;
  if (cpu->gen_mips && mip_filter && t->generate_mipmaps) {
    int m = cpu->width > cpu->height ? cpu->width : cpu->height;
    adopted_levels = 1;
    while (m > 1) { m >>= 1; adopted_levels++; }
  }
  t3_texture_adopt_gl(t, tex, cpu->width, cpu->height, adopted_levels, true);
  t->flip_y = false;
  return bytes;
}

void cw__tex_swap(t3_texture *a, t3_texture *b) { t3_texture_swap_gl(a, b); }

void cw__tex_release(t3_texture *t) {
  if (!t) return;
  t3_release(t); /* adopted GL objects are three.c's to delete (owns_gl) */
}

/* ── the resource type ───────────────────────────────────────────── */
typedef cw_tex_payload tex_payload;

/* any texture may be KTX2 inside (a .png whose cwimport sidecar packs KTX2): the
 * target needs the GL context, so it is picked on the main thread before the
 * file is read on a worker */
static void tex_request(void *u) {
  (void)u;
  cw__tex_pick_ktx2_target();
}

static void *tex_decode(const cw_res_decode_args *a, size_t *cost) {
  cw_tex_cpu *cpu = cw_calloc(1, sizeof *cpu, CW_MEM_RESOURCE);
  if (!cw__tex_decode(a->path, a->bytes, a->len, cpu)) { cw_free(cpu); return NULL; }
  for (int l = 0; l < cpu->levels; l++) *cost += cpu->level[l].size;
  return cpu;
}

/* The colour space glTF materials claimed for an image file's texture
 * resource before it loaded (cw__tex_res_claim), by interned path: kept for
 * the path's life, so a hot reload uploads the same way. */
static struct { const char *path; bool srgb; } *claims;
static int claim_n, claim_cap;
static int claim_find(const char *path) {
  for (int i = 0; i < claim_n; i++)
    if (claims[i].path == path) return i;
  return -1;
}

bool cw__tex_res_claim(const char *path, bool srgb) {
  char c[CW_RES_PATH_MAX];
  if (!cw__res_canon(path, c, sizeof c)) return false;
  tex_payload *p = cw__res_get_typed(cw_res_find(c), CW_RES_TEXTURE);
  if (p) return (p->tex->color_space == T3_SRGB_COLOR_SPACE) == srgb; /* uploaded already */
  const char *ip = cw_intern(c);
  int i = claim_find(ip);
  if (i >= 0) return claims[i].srgb == srgb;
  if (claim_n == claim_cap) {
    claim_cap = claim_cap ? claim_cap * 2 : 16;
    claims = cw_realloc(claims, (size_t)claim_cap * sizeof *claims, CW_MEM_RESOURCE);
  }
  claims[claim_n].path = ip;
  claims[claim_n++].srgb = srgb;
  return true;
}

static void *tex_finish(void *decoded, cw_res self, const cw_res *deps, int n, void *u) {
  cw_tex_cpu *cpu = decoded;
  tex_payload *p = cw_calloc(1, sizeof *p, CW_MEM_RESOURCE);
  p->tex = t3_texture_new(1, 1, NULL);
  p->tex->flip_y = false;
  p->tex->wrap_s = p->tex->wrap_t = T3_REPEAT; /* glTF / three.js TextureLoader sampler defaults */
  /* r187: KTX2Loader takes the colour space from the file (the DFD's
   * transfer function), TextureLoader leaves PNG / JPEG without one (data);
   * a glTF that shows the image claims it first (its material slot decides,
   * as GLTFLoader's assignTexture). On GL it is fixed here, where the texels
   * upload (an sRGB internal format) */
  int c = claim_find(cw_res_path(self));
  if (c >= 0 ? claims[c].srgb : cpu->srgb) p->tex->color_space = T3_SRGB_COLOR_SPACE;
  p->info.gpu_bytes = cw__tex_upload(p->tex, cpu);
  p->info.width = cpu->width;
  p->info.height = cpu->height;
  p->info.levels = cpu->levels;
  p->info.gl_internal_format = cpu->gl_format;
  p->info.format = cpu->format;
  p->info.from_ktx2 = cpu->from_ktx2;
  cw__tex_cpu_free(cpu);
  cw_free(cpu);
  return p;
}

static void tex_unload(void *payload, void *u) {
  tex_payload *p = payload;
  cw__tex_release(p->tex);
  cw_free(p);
}

static void tex_discard(void *decoded, void *u) {
  cw__tex_cpu_free(decoded);
  cw_free(decoded);
}

static void tex_reload(void *payload, void *fresh, void *u) {
  tex_payload *p = payload, *f = fresh;
  cw__tex_swap(p->tex, f->tex); /* same t3_texture, new GL object: materials using it see the new image */
  cw_texture_info i = p->info;
  p->info = f->info;
  f->info = i;
  tex_unload(f, u);
}

static size_t tex_size(const void *payload, void *u) { return ((const tex_payload *)payload)->info.gpu_bytes; }

const cw_res_type_desc cw__res_texture_type = {
  .name = "texture", .extensions = ".png .jpg .jpeg .ktx2 .gif", .decode = tex_decode,
  .finish = tex_finish, .unload = tex_unload, .discard = tex_discard, .reload = tex_reload, .size = tex_size,
  .request = tex_request,
};

void *cw__res_get_typed(cw_res h, cw_res_type t);

t3_texture *cw_res_texture(cw_res h) {
  tex_payload *p = cw__res_get_typed(h, CW_RES_TEXTURE);
  if (p) return p->tex;
  return NULL;
}

bool cw_res_texture_info(cw_res h, cw_texture_info *out) {
  tex_payload *p = cw__res_get_typed(h, CW_RES_TEXTURE);
  if (!p) return false;
  *out = p->info;
  return true;
}
