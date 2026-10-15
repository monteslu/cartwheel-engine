/* pixel_scale and crisp UI (cw_app_desc.pixel_scale, ui_min_lines): the
 * frame's scene and UI targets.
 *
 * With k == 1 (the default) nothing here does anything: the frame draws into
 * the window. With k > 1 the frame starts on the scene target, an offscreen
 * texture of scene_w x scene_h that stands in for the window: GL code binds
 * framebuffer 0 through cartwheel/gl.h, which maps 0 to cw__gl_window_fbo,
 * and WebGPU code asks cw__wgpu_frame_view, which answers this target.
 * cw_drawable_size answers its size, so the 3D pipeline, post and the 2D
 * batcher size themselves to it.
 *
 * Crisp UI on: the first 2D window pass of the frame (cw__pixel_ui_begin)
 * scales the scene up k times with nearest-neighbour into the picture (the
 * output itself, or a view-sized target when pixel_scale leaves bars) and the
 * UI draws on top of it at the picture's resolution. At the end of the frame
 * what was drawn goes into the output: the scene scaled up k times, or the
 * UI target copied 1:1, into the view rect, the bars cleared black.
 *
 * The upscale reads texels exactly (texelFetch / textureLoad at
 * (fragment - origin) / k): every output pixel of a k x k block is the same
 * scene pixel, with no filtering.
 *
 * The render resolution setting (cartwheel/quality.h; k == 1, upscale) uses
 * the same scene target at a share of the output (scene_w x scene_h, any
 * size) and scales it up with a filter instead: Catmull-Rom (bicubic, five
 * bilinear taps), which keeps edges and text in the world sharper than
 * bilinear does. The UI always draws over it at the output's resolution, as
 * crisp UI does, so the HUD and menus never lose resolution. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/core.h"
#include "cartwheel/gl.h"
#undef glBindFramebuffer /* this file binds the real window (0) */

unsigned cw__gl_window_fbo; /* what framebuffer 0 means to engine GL code right now */

enum { T_OUT = 0, T_SCENE = 1, T_UI = 2 };
static int target;        /* what draws now */
static bool scene_used;   /* the scene target holds this frame's scene (not yet in the picture) */

int cw__pixel_target(void) { return target; }

/* ── GL ─────────────────────────────────────────────────────────────── */

typedef struct { GLuint fbo, tex, ds; int w, h; } gl_rt;
static gl_rt g_scene, g_ui;
static GLuint g_prog, g_vao;
static GLint g_u_tex, g_u_origin, g_u_k;

static void gl_rt_free(gl_rt *r) {
  if (r->fbo) glDeleteFramebuffers(1, &r->fbo);
  if (r->tex) glDeleteTextures(1, &r->tex);
  if (r->ds) glDeleteRenderbuffers(1, &r->ds);
  memset(r, 0, sizeof *r);
}

/* colour + depth/stencil (what the window has: 3D and the 2D batcher's
 * clips draw into it as into the window) */
static bool gl_rt_ensure(gl_rt *r, int w, int h) {
  if (r->fbo && r->w == w && r->h == h) return true;
  gl_rt_free(r);
  glGenTextures(1, &r->tex);
  glBindTexture(GL_TEXTURE_2D, r->tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenRenderbuffers(1, &r->ds);
  glBindRenderbuffer(GL_RENDERBUFFER, r->ds);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glGenFramebuffers(1, &r->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, r->ds);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, r->ds);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    cw_error("pixel scale: render target %dx%d incomplete (0x%x)", w, h, (unsigned)st);
    gl_rt_free(r);
    return false;
  }
  r->w = w;
  r->h = h;
  return true;
}

static GLuint gl_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = { 0 };
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    cw_error("pixel scale: shader: %s", log);
  }
  return s;
}

static bool gl_prog_ensure(void) {
  if (g_prog) return true;
  static const char *vs = "#version 300 es\n"
                          "void main() {\n"
                          "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                          "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                          "}\n";
  static const char *fs = "#version 300 es\n"
                          "precision highp float;\n"
                          "precision highp int;\n"
                          "uniform highp sampler2D u_tex;\n"
                          "uniform vec2 u_origin;\n"
                          "uniform float u_k;\n"
                          "out vec4 o;\n"
                          "void main() {\n"
                          "  ivec2 t = ivec2(floor((gl_FragCoord.xy - u_origin) / u_k));\n"
                          "  o = texelFetch(u_tex, clamp(t, ivec2(0), textureSize(u_tex, 0) - 1), 0);\n"
                          "}\n";
  GLuint v = gl_shader(GL_VERTEX_SHADER, vs), f = gl_shader(GL_FRAGMENT_SHADER, fs);
  g_prog = glCreateProgram();
  glAttachShader(g_prog, v);
  glAttachShader(g_prog, f);
  glLinkProgram(g_prog);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    cw_error("pixel scale: the upscale program did not link");
    glDeleteProgram(g_prog);
    g_prog = 0;
    return false;
  }
  g_u_tex = glGetUniformLocation(g_prog, "u_tex");
  g_u_origin = glGetUniformLocation(g_prog, "u_origin");
  g_u_k = glGetUniformLocation(g_prog, "u_k");
  glGenVertexArrays(1, &g_vao);
  return true;
}

/* the filtered upscale (the render resolution setting): Catmull-Rom from
 * five bilinear taps, clamped (no ringing past the display range) */
#define CATMULL_ROM_GLSL \
  "vec4 cr(highp sampler2D t, vec2 uv) {\n" \
  "  vec2 ts = vec2(textureSize(t, 0));\n" \
  "  vec2 sp = uv * ts;\n" \
  "  vec2 t1 = floor(sp - 0.5) + 0.5;\n" \
  "  vec2 f = sp - t1;\n" \
  "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n" \
  "  vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n" \
  "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n" \
  "  vec2 w3 = f * f * (-0.5 + 0.5 * f);\n" \
  "  vec2 w12 = w1 + w2;\n" \
  "  vec2 p0 = (t1 - 1.0) / ts, p3 = (t1 + 2.0) / ts, p12 = (t1 + w2 / w12) / ts;\n" \
  "  vec4 c = texture(t, vec2(p12.x, p0.y)) * (w12.x * w0.y) + texture(t, vec2(p0.x, p12.y)) * (w0.x * w12.y)\n" \
  "         + texture(t, p12) * (w12.x * w12.y) + texture(t, vec2(p3.x, p12.y)) * (w3.x * w12.y)\n" \
  "         + texture(t, vec2(p12.x, p3.y)) * (w12.x * w3.y);\n" \
  "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n" \
  "  return clamp(c / ws, 0.0, 1.0);\n" \
  "}\n"

static GLuint g_fprog;
static GLint g_fu_tex, g_fu_origin, g_fu_size;
static bool gl_fprog_ensure(void) {
  if (g_fprog) return true;
  if (!gl_prog_ensure()) return false; /* its VAO */
  static const char *vs = "#version 300 es\n"
                          "void main() {\n"
                          "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                          "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                          "}\n";
  static const char *fs = "#version 300 es\n"
                          "precision highp float;\n"
                          "uniform highp sampler2D u_tex;\n"
                          "uniform vec2 u_origin, u_size;\n"
                          "out vec4 o;\n" CATMULL_ROM_GLSL
                          "void main() { o = cr(u_tex, (gl_FragCoord.xy - u_origin) / u_size); }\n";
  GLuint v = gl_shader(GL_VERTEX_SHADER, vs), f = gl_shader(GL_FRAGMENT_SHADER, fs);
  g_fprog = glCreateProgram();
  glAttachShader(g_fprog, v);
  glAttachShader(g_fprog, f);
  glLinkProgram(g_fprog);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(g_fprog, GL_LINK_STATUS, &ok);
  if (!ok) {
    cw_error("render scale: the upscale program did not link");
    glDeleteProgram(g_fprog);
    g_fprog = 0;
    return false;
  }
  g_fu_tex = glGetUniformLocation(g_fprog, "u_tex");
  g_fu_origin = glGetUniformLocation(g_fprog, "u_origin");
  g_fu_size = glGetUniformLocation(g_fprog, "u_size");
  return true;
}

/* src filtered up to w x h, into framebuffer dst at (x, y_gl) */
static void gl_draw_filtered(const gl_rt *src, GLuint dst, int x, int y_gl, int w, int h) {
  if (!gl_fprog_ensure()) return;
  glBindFramebuffer(GL_FRAMEBUFFER, dst);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(x, y_gl, w, h);
  glUseProgram(g_fprog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src->tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glUniform1i(g_fu_tex, 0);
  glUniform2f(g_fu_origin, (float)x, (float)y_gl);
  glUniform2f(g_fu_size, (float)w, (float)h);
  glBindVertexArray(g_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

/* src's texels, k times, into framebuffer dst at (x, y_gl) (GL origin bottom-left),
 * clearing dst first when the picture does not cover it */
static void gl_draw(const gl_rt *src, GLuint dst, int dst_w, int dst_h, int x, int y_gl, int k) {
  if (!gl_prog_ensure()) return;
  glBindFramebuffer(GL_FRAMEBUFFER, dst);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  int pw = src->w * k, ph = src->h * k;
  if (x > 0 || y_gl > 0 || pw < dst_w || ph < dst_h) {
    glViewport(0, 0, dst_w, dst_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glViewport(x, y_gl, pw, ph);
  glUseProgram(g_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src->tex);
  glUniform1i(g_u_tex, 0);
  glUniform2f(g_u_origin, (float)x, (float)y_gl);
  glUniform1f(g_u_k, (float)k);
  glBindVertexArray(g_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

/* ── WebGPU ─────────────────────────────────────────────────────────── */

#if CW_WGPU
#include "wgpu.h"
WGPUTextureView cw__wgpu_output_view(void); /* gpu.c: the real frame (surface or offscreen) */

typedef struct { WGPUTexture tex; WGPUTextureView view; int w, h; } wg_rt;
static wg_rt w_scene, w_ui;
static WGPURenderPipeline w_pipe;
static WGPUBindGroupLayout w_bgl;
static WGPUBuffer w_ubuf[2];
static int w_ubuf_next;

static void wg_rt_free(wg_rt *r) {
  if (r->view) wgpuTextureViewRelease(r->view);
  if (r->tex) wgpuTextureRelease(r->tex);
  memset(r, 0, sizeof *r);
}
static bool wg_rt_ensure(wg_rt *r, int w, int h) {
  if (r->tex && r->w == w && r->h == h) return true;
  wg_rt_free(r);
  WGPUTextureDescriptor d = { .label = CW_WGPU_SV("cw pixel target"),
                              .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
                              .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, 1 },
                              .format = cw__wgpu_frame_format(), .mipLevelCount = 1, .sampleCount = 1 };
  r->tex = wgpuDeviceCreateTexture(cw__wgpu_device(), &d);
  if (!r->tex) return false;
  r->view = wgpuTextureCreateView(r->tex, NULL);
  r->w = w;
  r->h = h;
  return true;
}

static bool wg_pipe_ensure(void) {
  if (w_pipe) return true;
  static const char *wgsl =
    "struct U { origin: vec2f, k: f32, pad: f32 };\n"
    "@group(0) @binding(0) var t: texture_2d<f32>;\n"
    "@group(0) @binding(1) var<uniform> u: U;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "  return vec4f(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "  let sz = vec2i(textureDimensions(t));\n"
    "  let c = clamp(vec2i(floor((pos.xy - u.origin) / u.k)), vec2i(0), sz - 1);\n"
    "  return textureLoad(t, c, 0);\n"
    "}\n";
  WGPUShaderModule m = cw__wgpu_shader("cw pixel upscale", wgsl);
  if (!m) return false;
  WGPUBindGroupLayoutEntry e[2] = {
    { .binding = 0, .visibility = WGPUShaderStage_Fragment,
      .texture = { .sampleType = WGPUTextureSampleType_UnfilterableFloat, .viewDimension = WGPUTextureViewDimension_2D } },
    { .binding = 1, .visibility = WGPUShaderStage_Fragment, .buffer = { .type = WGPUBufferBindingType_Uniform, .minBindingSize = 16 } },
  };
  WGPUBindGroupLayoutDescriptor bd = { .entryCount = 2, .entries = e };
  w_bgl = wgpuDeviceCreateBindGroupLayout(cw__wgpu_device(), &bd);
  WGPUPipelineLayoutDescriptor pd = { .bindGroupLayoutCount = 1, .bindGroupLayouts = &w_bgl };
  WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(cw__wgpu_device(), &pd);
  WGPUColorTargetState ct = { .format = cw__wgpu_frame_format(), .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = m, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
  WGPURenderPipelineDescriptor rd = { .label = CW_WGPU_SV("cw pixel upscale"), .layout = pl,
                                      .vertex = { .module = m, .entryPoint = CW_WGPU_SV("vs") },
                                      .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
                                      .multisample = { .count = 1, .mask = 0xFFFFFFFFu }, .fragment = &fs };
  w_pipe = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &rd);
  wgpuPipelineLayoutRelease(pl);
  wgpuShaderModuleRelease(m);
  for (int i = 0; i < 2; i++) {
    WGPUBufferDescriptor b = { .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = 16 };
    w_ubuf[i] = wgpuDeviceCreateBuffer(cw__wgpu_device(), &b);
  }
  return w_pipe != NULL;
}

/* src's texels, k times, into view dst (dst_w x dst_h) at (x, y) (top-left
 * origin); the pass clears dst black first (the bars) */
static void wg_draw(const wg_rt *src, WGPUTextureView dst, int x, int y, int k) {
  if (!dst || !wg_pipe_ensure()) return;
  WGPUBuffer ub = w_ubuf[w_ubuf_next++ & 1];
  float u[4] = { (float)x, (float)y, (float)k, 0 };
  wgpuQueueWriteBuffer(cw__wgpu_queue(), ub, 0, u, sizeof u);
  WGPUBindGroupEntry be[2] = { { .binding = 0, .textureView = src->view }, { .binding = 1, .buffer = ub, .size = 16 } };
  WGPUBindGroupDescriptor bgd = { .layout = w_bgl, .entryCount = 2, .entries = be };
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(cw__wgpu_device(), &bgd);
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(cw__wgpu_device(), NULL);
  WGPURenderPassColorAttachment ca = { .view = dst, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
                                       .storeOp = WGPUStoreOp_Store, .clearValue = { 0, 0, 0, 1 } };
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = 1, .colorAttachments = &ca };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(p, w_pipe);
  wgpuRenderPassEncoderSetBindGroup(p, 0, bg, 0, NULL);
  wgpuRenderPassEncoderSetViewport(p, (float)x, (float)y, (float)(src->w * k), (float)(src->h * k), 0, 1);
  wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(cw__wgpu_queue(), 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
  wgpuBindGroupRelease(bg);
}

static WGPURenderPipeline w_fpipe;
static WGPUBindGroupLayout w_fbgl;
static WGPUSampler w_lin;
static bool wg_fpipe_ensure(void) {
  if (w_fpipe) return true;
  if (!wg_pipe_ensure()) return false; /* its uniform buffers */
  static const char *wgsl =
    "struct U { origin: vec2f, size: vec2f };\n"
    "@group(0) @binding(0) var t: texture_2d<f32>;\n"
    "@group(0) @binding(1) var<uniform> u: U;\n"
    "@group(0) @binding(2) var s: sampler;\n"
    "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "  return vec4f(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
    "  let ts = vec2f(textureDimensions(t));\n"
    "  let sp = (pos.xy - u.origin) / u.size * ts;\n"
    "  let t1 = floor(sp - 0.5) + 0.5;\n"
    "  let f = sp - t1;\n"
    "  let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "  let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "  let w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "  let w3 = f * f * (-0.5 + 0.5 * f);\n"
    "  let w12 = w1 + w2;\n"
    "  let p0 = (t1 - 1.0) / ts; let p3 = (t1 + 2.0) / ts; let p12 = (t1 + w2 / w12) / ts;\n"
    "  let c = textureSampleLevel(t, s, vec2f(p12.x, p0.y), 0.0) * (w12.x * w0.y)\n"
    "        + textureSampleLevel(t, s, vec2f(p0.x, p12.y), 0.0) * (w0.x * w12.y)\n"
    "        + textureSampleLevel(t, s, p12, 0.0) * (w12.x * w12.y)\n"
    "        + textureSampleLevel(t, s, vec2f(p3.x, p12.y), 0.0) * (w3.x * w12.y)\n"
    "        + textureSampleLevel(t, s, vec2f(p12.x, p3.y), 0.0) * (w12.x * w3.y);\n"
    "  let ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
    "  return clamp(c / ws, vec4f(0.0), vec4f(1.0));\n"
    "}\n";
  WGPUShaderModule m = cw__wgpu_shader("cw render scale upscale", wgsl);
  if (!m) return false;
  WGPUBindGroupLayoutEntry e[3] = {
    { .binding = 0, .visibility = WGPUShaderStage_Fragment,
      .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2D } },
    { .binding = 1, .visibility = WGPUShaderStage_Fragment, .buffer = { .type = WGPUBufferBindingType_Uniform, .minBindingSize = 16 } },
    { .binding = 2, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Filtering } },
  };
  WGPUBindGroupLayoutDescriptor bd = { .entryCount = 3, .entries = e };
  w_fbgl = wgpuDeviceCreateBindGroupLayout(cw__wgpu_device(), &bd);
  WGPUPipelineLayoutDescriptor pd = { .bindGroupLayoutCount = 1, .bindGroupLayouts = &w_fbgl };
  WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(cw__wgpu_device(), &pd);
  WGPUColorTargetState ct = { .format = cw__wgpu_frame_format(), .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = m, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 1, .targets = &ct };
  WGPURenderPipelineDescriptor rd = { .label = CW_WGPU_SV("cw render scale upscale"), .layout = pl,
                                      .vertex = { .module = m, .entryPoint = CW_WGPU_SV("vs") },
                                      .primitive = { .topology = WGPUPrimitiveTopology_TriangleList },
                                      .multisample = { .count = 1, .mask = 0xFFFFFFFFu }, .fragment = &fs };
  w_fpipe = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &rd);
  wgpuPipelineLayoutRelease(pl);
  wgpuShaderModuleRelease(m);
  WGPUSamplerDescriptor sd = { .addressModeU = WGPUAddressMode_ClampToEdge, .addressModeV = WGPUAddressMode_ClampToEdge,
                               .addressModeW = WGPUAddressMode_ClampToEdge, .magFilter = WGPUFilterMode_Linear,
                               .minFilter = WGPUFilterMode_Linear, .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                               .lodMinClamp = 0, .lodMaxClamp = 1, .maxAnisotropy = 1 };
  w_lin = wgpuDeviceCreateSampler(cw__wgpu_device(), &sd);
  return w_fpipe != NULL && w_lin != NULL;
}

/* src filtered up to w x h into view dst at (x, y) (top-left origin) */
static void wg_draw_filtered(const wg_rt *src, WGPUTextureView dst, int x, int y, int w, int h) {
  if (!dst || !wg_fpipe_ensure()) return;
  WGPUBuffer ub = w_ubuf[w_ubuf_next++ & 1];
  float u[4] = { (float)x, (float)y, (float)w, (float)h };
  wgpuQueueWriteBuffer(cw__wgpu_queue(), ub, 0, u, sizeof u);
  WGPUBindGroupEntry be[3] = { { .binding = 0, .textureView = src->view }, { .binding = 1, .buffer = ub, .size = 16 },
                               { .binding = 2, .sampler = w_lin } };
  WGPUBindGroupDescriptor bgd = { .layout = w_fbgl, .entryCount = 3, .entries = be };
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(cw__wgpu_device(), &bgd);
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(cw__wgpu_device(), NULL);
  WGPURenderPassColorAttachment ca = { .view = dst, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
                                       .storeOp = WGPUStoreOp_Store, .clearValue = { 0, 0, 0, 1 } };
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = 1, .colorAttachments = &ca };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(p, w_fpipe);
  wgpuRenderPassEncoderSetBindGroup(p, 0, bg, 0, NULL);
  wgpuRenderPassEncoderSetViewport(p, (float)x, (float)y, (float)w, (float)h, 0, 1);
  wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(cw__wgpu_queue(), 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
  wgpuBindGroupRelease(bg);
}

/* the view standing in for the window while the scene or the UI target draws */
WGPUTextureView cw__pixel_wgpu_view(void) {
  return target == T_SCENE ? w_scene.view : target == T_UI ? w_ui.view : NULL;
}
#endif

/* ── the frame ──────────────────────────────────────────────────────── */

#if CW_WGPU
static bool wgpu_on(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }
#endif
static bool bars(void) {
  cw_platform *p = &cw__plat;
  return p->view_x > 0 || p->view_y > 0 || p->view_w < p->px_w || p->view_h < p->px_h;
}

void cw__pixel_frame_begin(void) {
  cw_platform *p = &cw__plat;
  target = T_OUT;
  scene_used = false;
  cw__gl_window_fbo = 0;
  if (p->k <= 1 && !p->upscale) return;
#if CW_WGPU
  if (wgpu_on()) {
    /* the frame first: a cart's surface is configured (and its format, which
     * the targets share, known) when the frame is first acquired */
    if (!cw__wgpu_output_view()) return;
    if (!wg_rt_ensure(&w_scene, p->scene_w, p->scene_h)) return;
    target = T_SCENE;
    scene_used = true;
    return;
  }
#endif
  if (!gl_rt_ensure(&g_scene, p->scene_w, p->scene_h)) return;
  cw__gl_window_fbo = g_scene.fbo;
  target = T_SCENE;
  scene_used = true;
}

void cw__pixel_ui_begin(void) {
  cw_platform *p = &cw__plat;
  if (target != T_SCENE || !(p->crisp_ui || p->upscale)) return;
  bool b = bars();
#if CW_WGPU
  if (wgpu_on() && p->upscale) {
    wg_draw_filtered(&w_scene, cw__wgpu_output_view(), 0, 0, p->px_w, p->px_h);
    target = T_OUT;
    scene_used = false;
    return;
  }
  if (wgpu_on()) {
    if (b) {
      if (!wg_rt_ensure(&w_ui, p->view_w, p->view_h)) return;
      wg_draw(&w_scene, w_ui.view, 0, 0, p->k);
      target = T_UI;
    } else {
      wg_draw(&w_scene, cw__wgpu_output_view(), p->view_x, p->view_y, p->k);
      target = T_OUT;
    }
    scene_used = false;
    return;
  }
#endif
  if (p->upscale) {
    gl_draw_filtered(&g_scene, 0, 0, 0, p->px_w, p->px_h);
    cw__gl_window_fbo = 0;
    target = T_OUT;
    scene_used = false;
    return;
  }
  if (b) {
    if (!gl_rt_ensure(&g_ui, p->view_w, p->view_h)) return;
    gl_draw(&g_scene, g_ui.fbo, p->view_w, p->view_h, 0, 0, p->k);
    cw__gl_window_fbo = g_ui.fbo;
    target = T_UI;
  } else {
    gl_draw(&g_scene, 0, p->px_w, p->px_h, p->view_x, p->px_h - p->view_y - p->view_h, p->k);
    cw__gl_window_fbo = 0;
    target = T_OUT;
  }
  scene_used = false;
}

void cw__pixel_frame_end(void) {
  cw_platform *p = &cw__plat;
  int t = target;
  target = T_OUT;
  cw__gl_window_fbo = 0;
  if (t == T_OUT && !scene_used) return;
#if CW_WGPU
  if (wgpu_on()) {
    if (t == T_SCENE && p->upscale) wg_draw_filtered(&w_scene, cw__wgpu_output_view(), 0, 0, p->px_w, p->px_h);
    else if (t == T_SCENE) wg_draw(&w_scene, cw__wgpu_output_view(), p->view_x, p->view_y, p->k);
    else if (t == T_UI) wg_draw(&w_ui, cw__wgpu_output_view(), p->view_x, p->view_y, 1);
    scene_used = false;
    return;
  }
#endif
  int y_gl = p->px_h - p->view_y - p->view_h;
  if (t == T_SCENE && p->upscale) gl_draw_filtered(&g_scene, 0, 0, 0, p->px_w, p->px_h);
  else if (t == T_SCENE) gl_draw(&g_scene, 0, p->px_w, p->px_h, p->view_x, y_gl, p->k);
  else if (t == T_UI) gl_draw(&g_ui, 0, p->px_w, p->px_h, p->view_x, y_gl, 1);
  scene_used = false;
}

void cw__pixel_shutdown(void) {
#if CW_WGPU
  if (wgpu_on()) {
    wg_rt_free(&w_scene);
    wg_rt_free(&w_ui);
    return;
  }
#endif
  gl_rt_free(&g_scene);
  gl_rt_free(&g_ui);
  if (g_prog) glDeleteProgram(g_prog);
  if (g_fprog) glDeleteProgram(g_fprog);
  g_fprog = 0;
  if (g_vao) glDeleteVertexArrays(1, &g_vao);
  g_prog = g_vao = 0;
}
