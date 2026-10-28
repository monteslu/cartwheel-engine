/* The render pipeline on WebGPU (webgpu.h, WGSL): targets, the frame
 * encoder, full-screen WGSL programs with reflected uniform blocks, the
 * pipeline cache, the depth copy, 3D LUTs, and native GPU timing through
 * timestamp queries. pipeline_gl.c's public helpers (cw_pass_*) branch here
 * on a WebGPU run; pipeline.c drives the frame. See pipeline_wgpu.h for the
 * model (ids, one encoder per frame, GL row order offscreen, depth).
 *
 * Compatibility mode (the wasmcart tier's floor): 2D / 3D textures only
 * (their binding view dimension is the default), 4x MSAA of RGBA8 only, the
 * depth copy reads the depth attachment through a comparison sampler
 * (exactly), R32Float only through textureLoad (unfilterable). On a core
 * device (cw__wgpu_core) the scene multisamples in RGBA16Float too, and the
 * depth copy and the depth resolve read the (multisampled) depth with
 * textureLoad. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/platform.h"
#include "cartwheel/profile.h"
#include "pipeline_internal.h"
#include "pipeline_wgpu.h"

#if !CW_WGPU
bool cw__pw_rt_create(cw_rt *rt, int w, int h, cw_rt_format f, bool depth) { (void)rt, (void)w, (void)h, (void)f, (void)depth; return false; }
void cw__pw_rt_destroy(cw_rt *rt) { (void)rt; }
bool cw__pw_msaa_create(cw_rt *msaa, const cw_rt *scene) { (void)msaa, (void)scene; return false; }
void cw__pw_depth_copy(cw_pass_ctx *ctx, const cw_rt *src, const cw_rt *dst, const float pz[4]) { (void)ctx, (void)src, (void)dst, (void)pz; }
void cw__pw_msaa_retarget(cw_rt *msaa, const cw_rt *scene) { (void)msaa, (void)scene; }
void cw__pw_depth_resolve(const cw_rt *msaa, const cw_rt *scene) { (void)msaa, (void)scene; }
bool cw__pw_core(void) { return false; }
void cw__pw_bind_target(const cw_rt *rt) { (void)rt; }
void cw__pw_state_reset(void) {}
void cw__pw_frame_begin(void) {}
void cw__pw_flush(void) {}
void cw__pw_probe_caps(cw_pipeline_caps *caps) { (void)caps; }
void cw__pw_timing_pass(cw_pipeline *p, const char *n, const char *l) { (void)p, (void)n, (void)l; }
void cw__pw_timing_whole(bool end) { (void)end; }
void cw__pw_timing_poll(void) {}
void cw__pw_timing_hold(bool hold) { (void)hold; }
void cw__pw_timing_span(void *enc, bool end) { (void)enc, (void)end; }
void cw__pw_forget(cw_pipeline *p) { (void)p; }
unsigned cw__pw_program(const char *name, const char *wgsl) { (void)name, (void)wgsl; return 0; }
void cw__pw_use(unsigned prog) { (void)prog; }
void cw__pw_fullscreen(void) {}
void cw__pw_draw(int vertices) { (void)vertices; }
void cw__pw_bind_texture(int unit, unsigned tex) { (void)unit, (void)tex; }
void cw__pw_clear(float r, float g, float b, float a, bool depth) { (void)r, (void)g, (void)b, (void)a, (void)depth; }
void cw__pw_set_blend(cw_pass_blend b) { (void)b; }
void cw__pw_set_depth(bool test, bool write) { (void)test, (void)write; }
void cw__pw_uniform(unsigned prog, const char *name, int kind, const float *f, int i) { (void)prog, (void)name, (void)kind, (void)f, (void)i; }
unsigned cw__pw_lut(int size, const uint16_t *half) { (void)size, (void)half; return 0; }
void cw__pw_tex_free(unsigned id) { (void)id; }
unsigned cw__pw_rg8(int w, int h, const uint8_t *rg) { (void)w, (void)h, (void)rg; return 0; }
unsigned cw__pw_pass_texture(int w, int h, cw_rt_format f, const void *t) { (void)w, (void)h, (void)f, (void)t; return 0; }
#else

#define SV CW_WGPU_SV
#define UALIGN 256u
#define CHUNK (64u * 1024u)
#define MAX_UNITS 12

/* ── the registry ───────────────────────────────────────────────────── */

typedef struct pw_obj {
  bool live, target;
  /* textures */
  WGPUTexture tex;
  WGPUTextureView view, attach;
  WGPUTextureFormat fmt;
  WGPUTextureDimension dim; /* read once when made: unit_view checks it every binding */
  int samples;
  /* targets: ids of the attachments */
  unsigned color, depth, resolve;
  int w, h;
} pw_obj;

static CW_ARRAY(pw_obj) objs;

static unsigned obj_new(void) {
  for (int i = 0; i < objs.count; i++)
    if (!objs.data[i].live) {
      memset(&objs.data[i], 0, sizeof objs.data[i]);
      objs.data[i].live = true;
      return (unsigned)i + 1;
    }
  cw_array_push(&objs, ((pw_obj){ .live = true }));
  return (unsigned)objs.count;
}

static pw_obj *obj(unsigned id) {
  if (!id || id > (unsigned)objs.count || !objs.data[id - 1].live) return NULL;
  return &objs.data[id - 1];
}

WGPUTexture cw__pw_texture(unsigned id) { pw_obj *o = obj(id); return o ? o->tex : NULL; }
WGPUTextureView cw__pw_view(unsigned id) { pw_obj *o = obj(id); return o ? o->view : NULL; }
WGPUTextureView cw__pw_attach(unsigned id) { pw_obj *o = obj(id); return o ? o->attach : NULL; }
WGPUTextureFormat cw__pw_format(unsigned id) { pw_obj *o = obj(id); return o ? o->fmt : WGPUTextureFormat_Undefined; }

unsigned cw__pw_register(WGPUTexture tex, WGPUTextureView view) {
  unsigned id = obj_new();
  pw_obj *o = obj(id);
  o->tex = tex;
  o->view = view;
  o->fmt = wgpuTextureGetFormat(tex);
  o->dim = wgpuTextureGetDimension(tex);
  o->samples = (int)wgpuTextureGetSampleCount(tex);
  o->w = (int)wgpuTextureGetWidth(tex);
  o->h = (int)wgpuTextureGetHeight(tex);
  return id;
}

static uint32_t bg_epoch; /* bumped when a registered view goes: cached bind groups may name it */
static void defer_texture(WGPUTexture t);

static void close_open(void);
void cw__pw_release(unsigned id) {
  pw_obj *o = obj(id);
  if (!o) return;
  close_open(); /* an open pass must not outlive its target's id (ids are reused) */
  if (o->view) bg_epoch++; /* (a target id is in no bind group) */
  if (o->view) wgpuTextureViewRelease(o->view);
  if (o->attach) wgpuTextureViewRelease(o->attach);
  if (o->tex) defer_texture(o->tex);
  memset(o, 0, sizeof *o);
}
void cw__pw_tex_free(unsigned id) { cw__pw_release(id); }

static WGPUTextureFormat rt_format(cw_rt_format f) {
  switch ((int)f) {
  case CW_RT_RGBA16F: return WGPUTextureFormat_RGBA16Float;
  case CW_RT_RG16F: return WGPUTextureFormat_RG16Float;
  case CW_RT_R16F: return WGPUTextureFormat_R16Float;
  case CW_RT_R8: return WGPUTextureFormat_R8Unorm;
  case CW__RT_R32F: return WGPUTextureFormat_R32Float;
  case CW__RT_RG8: return WGPUTextureFormat_RG8Unorm;
  default: return WGPUTextureFormat_RGBA8Unorm;
  }
}

static unsigned tex_new(const char *label, int w, int h, WGPUTextureFormat f, WGPUTextureUsage usage, int samples) {
  WGPUTextureDescriptor d = { .label = SV(label), .usage = usage, .dimension = WGPUTextureDimension_2D,
                              .size = { (uint32_t)w, (uint32_t)h, 1 }, .format = f, .mipLevelCount = 1,
                              .sampleCount = (uint32_t)samples };
  WGPUTexture t = wgpuDeviceCreateTexture(cw__wgpu_device(), &d);
  unsigned id = obj_new();
  pw_obj *o = obj(id);
  o->tex = t;
  o->fmt = f;
  o->dim = WGPUTextureDimension_2D;
  o->samples = samples;
  o->w = w;
  o->h = h;
  if (usage & WGPUTextureUsage_TextureBinding) o->view = wgpuTextureCreateView(t, NULL);
  if (usage & WGPUTextureUsage_RenderAttachment) o->attach = wgpuTextureCreateView(t, NULL);
  return id;
}

static unsigned target_new(unsigned color, unsigned depth, unsigned resolve, int w, int h, int samples) {
  unsigned id = obj_new();
  pw_obj *o = obj(id);
  o->target = true;
  o->color = color;
  o->depth = depth;
  o->resolve = resolve;
  o->w = w;
  o->h = h;
  o->samples = samples;
  return id;
}

/* ── the frame: encoder, uniform chunks, deferred releases ──────────── */

typedef struct chunk { WGPUBuffer buf; uint8_t *stage; uint32_t used; } chunk;
typedef struct engine_bg { WGPUBuffer buf; WGPUBindGroup g; } engine_bg;

static struct {
  bool inited;
  WGPUDevice dev;
  WGPUQueue q;
  WGPUShaderModule vs;
  WGPUBindGroupLayout bgl_engine;
  WGPUSampler linear, nearest, compare;
  WGPUCommandEncoder enc;
  CW_ARRAY(chunk) chunks;
  int cur_chunk;
  CW_ARRAY(WGPUBindGroup) deferred;
  /* textures released while the frame's encoder is open: commands already
   * recorded this frame may use them (a target resized or dropped mid-frame),
   * and a texture destroyed before the submit fails it ("Destroyed texture
   * used in a submit"), so they go after cw__pw_flush submits */
  CW_ARRAY(WGPUTexture) dead_tex;
  CW_ARRAY(engine_bg) engine_bg; /* group 1 per uniform chunk (draw) */
  /* draw state */
  unsigned prog;
  unsigned target;      /* 0 = the window */
  int target_w, target_h;
  unsigned units[MAX_UNITS];
  cw_pass_blend blend;
  bool depth_test, depth_write;
  /* dummies (an unbound unit) */
  unsigned dummy2d, dummy2d_unf, dummy3d, dummy_depth;
  cw__wgpu_pass_targets pt;
} W;

static void init(void) {
  if (W.inited) return;
  W.inited = true;
  W.dev = cw__wgpu_device();
  W.q = cw__wgpu_queue();
  static const char *VS =
      "struct CwTarget { size: vec2f, flip: f32, pad: f32 };\n"
      "@group(1) @binding(0) var<uniform> cw_target: CwTarget;\n"
      "struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };\n"
      "@vertex fn vs(@builtin(vertex_index) i: u32) -> VOut {\n"
      "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
      "  var o: VOut;\n"
      "  o.uv = p;\n"
      /* offscreen (flip -1): GL's row order in the texture */
      "  o.pos = vec4f(p.x * 2.0 - 1.0, (p.y * 2.0 - 1.0) * cw_target.flip, 0.0, 1.0);\n"
      "  return o;\n"
      "}\n";
  W.vs = cw__wgpu_shader("pipeline fullscreen vs", VS);
  WGPUBindGroupLayoutEntry e[4] = {
    { .binding = 0, .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
      .buffer = { .type = WGPUBufferBindingType_Uniform, .hasDynamicOffset = true, .minBindingSize = 16 } },
    { .binding = 1, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Filtering } },
    { .binding = 2, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Filtering } },
    { .binding = 3, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Comparison } },
  };
  W.bgl_engine = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 4, .entries = e });
  for (int i = 0; i < 2; i++) {
    WGPUFilterMode f = i ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    WGPUSamplerDescriptor sd = { .addressModeU = WGPUAddressMode_ClampToEdge, .addressModeV = WGPUAddressMode_ClampToEdge,
                                 .addressModeW = WGPUAddressMode_ClampToEdge, .magFilter = f, .minFilter = f,
                                 .mipmapFilter = WGPUMipmapFilterMode_Nearest, .lodMinClamp = 0, .lodMaxClamp = 32,
                                 .maxAnisotropy = 1 };
    *(i ? &W.nearest : &W.linear) = wgpuDeviceCreateSampler(W.dev, &sd);
  }
  W.compare = wgpuDeviceCreateSampler(W.dev, &(WGPUSamplerDescriptor){
    .addressModeU = WGPUAddressMode_ClampToEdge, .addressModeV = WGPUAddressMode_ClampToEdge, .addressModeW = WGPUAddressMode_ClampToEdge,
    .magFilter = WGPUFilterMode_Nearest, .minFilter = WGPUFilterMode_Nearest, .mipmapFilter = WGPUMipmapFilterMode_Nearest,
    .lodMinClamp = 0, .lodMaxClamp = 32, .compare = WGPUCompareFunction_LessEqual, .maxAnisotropy = 1 });
  static const uint8_t px[4] = { 0, 0, 0, 255 };
  W.dummy2d = cw__pw_texture_2d(1, 1, WGPUTextureFormat_RGBA8Unorm, px, 4);
  static const float one = 3.0e38f;
  W.dummy2d_unf = cw__pw_texture_2d(1, 1, WGPUTextureFormat_R32Float, &one, 4);
  static const uint16_t h[4] = { 0, 0, 0, 0x3c00 };
  W.dummy3d = cw__pw_texture_3d(1, h);
  W.dummy_depth = tex_new("pipeline dummy depth", 1, 1, WGPUTextureFormat_Depth32Float,
                          WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment, 1);
}

/* The engine's own draws into one target share a render pass: a pass stays
 * open while the next draw (or clear) goes to the same target, and closes
 * when the target changes or anything else records on the frame's encoder
 * (cw__pw_encoder: three.c, particles' compute, copies, the submit). Each
 * render pass costs the host several microseconds; the commands and their
 * order are the same, so the picture is too. Not while pass timers run (each
 * engine pass keeps its own timestamps). */
static struct {
  WGPURenderPassEncoder p;
  unsigned target;
} OP;
static void end(WGPURenderPassEncoder p);
static void close_open(void) {
  if (!OP.p) return;
  end(OP.p);
  OP.p = NULL;
  OP.target = 0;
}
static WGPUCommandEncoder raw_encoder(void) {
  init();
  if (!W.enc) W.enc = wgpuDeviceCreateCommandEncoder(W.dev, &(WGPUCommandEncoderDescriptor){ .label = SV("cw pipeline frame") });
  return W.enc;
}
WGPUCommandEncoder cw__pw_encoder(void) {
  close_open();
  return raw_encoder();
}

WGPUBuffer cw__pw_ubo(const void *data, size_t size, uint64_t *offset) {
  init();
  size_t blk = (size + UALIGN - 1) & ~(size_t)(UALIGN - 1);
  if (W.cur_chunk >= W.chunks.count || W.chunks.data[W.cur_chunk].used + blk > CHUNK) {
    if (W.cur_chunk < W.chunks.count && W.chunks.data[W.cur_chunk].used) W.cur_chunk++;
    if (W.cur_chunk >= W.chunks.count) {
      WGPUBufferDescriptor bd = { .label = SV("cw pipeline uniforms"), .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                                  .size = CHUNK };
      cw_array_push(&W.chunks, ((chunk){ wgpuDeviceCreateBuffer(W.dev, &bd), cw_alloc(CHUNK, CW_MEM_RENDER), 0 }));
    }
  }
  chunk *c = &W.chunks.data[W.cur_chunk];
  *offset = c->used;
  memset(c->stage + c->used, 0, blk);
  memcpy(c->stage + c->used, data, size);
  c->used += (uint32_t)blk;
  return c->buf;
}

void cw__pw_defer_bind_group(WGPUBindGroup g) { cw_array_push(&W.deferred, g); }

static void defer_texture(WGPUTexture t) {
  if (W.enc) {
    cw_array_push(&W.dead_tex, t);
    return;
  }
  wgpuTextureDestroy(t);
  wgpuTextureRelease(t);
}

static void timing_resolve(void);

void cw__pw_flush(void) {
  if (!W.inited) return;
  close_open();
  for (int i = 0; i < W.chunks.count; i++) {
    chunk *c = &W.chunks.data[i];
    if (c->used) wgpuQueueWriteBuffer(W.q, c->buf, 0, c->stage, c->used);
    c->used = 0;
  }
  W.cur_chunk = 0;
  if (W.enc) {
    timing_resolve();
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(W.enc, NULL);
    wgpuQueueSubmit(W.q, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(W.enc);
    W.enc = NULL;
  }
  for (int i = 0; i < W.deferred.count; i++) wgpuBindGroupRelease(W.deferred.data[i]);
  W.deferred.count = 0;
  for (int i = 0; i < W.dead_tex.count; i++) {
    wgpuTextureDestroy(W.dead_tex.data[i]);
    wgpuTextureRelease(W.dead_tex.data[i]);
  }
  W.dead_tex.count = 0;
}

/* ── targets ────────────────────────────────────────────────────────── */

static void clear_target(unsigned target, bool depth);

bool cw__pw_rt_create(cw_rt *rt, int w, int h, cw_rt_format f, bool depth) {
  init();
  memset(rt, 0, sizeof *rt);
  rt->w = w;
  rt->h = h;
  rt->format = f;
  rt->tex = tex_new("cw_rt colour", w, h, rt_format(f),
                    WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc |
                        WGPUTextureUsage_CopyDst, 1);
  if (depth)
    rt->depth_tex = tex_new("cw_rt depth", w, h, WGPUTextureFormat_Depth32Float,
                            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, 1);
  rt->fbo = target_new(rt->tex, rt->depth_tex, 0, w, h, 1);
  /* GL clears new storage once (colour 0, depth 1); WebGPU zeroes it: depth
   * must start at 1 */
  if (depth) clear_target(rt->fbo, true);
  return true;
}

void cw__pw_rt_destroy(cw_rt *rt) {
  cw__pw_release(rt->fbo);
  cw__pw_release(rt->tex);
  cw__pw_release(rt->depth_tex);
  memset(rt, 0, sizeof *rt);
}

/* A target drawing into an embedder's colour texture (layer 0 of it: a 2D
 * render target of the 2D renderer, an array texture of one layer), no
 * depth: the pipeline renders its scene offscreen and composites into it.
 * The texture stays the embedder's: releasing the target releases the view. */
unsigned cw__pw_target_wrap(WGPUTexture color, int w, int h) {
  init();
  unsigned cid = obj_new();
  pw_obj *c = obj(cid);
  c->fmt = wgpuTextureGetFormat(color);
  c->dim = WGPUTextureDimension_2D;
  c->samples = 1;
  c->w = w;
  c->h = h;
  c->attach = wgpuTextureCreateView(color, &(WGPUTextureViewDescriptor){ .format = c->fmt, .dimension = WGPUTextureViewDimension_2D,
                                                                       .baseMipLevel = 0, .mipLevelCount = 1, .baseArrayLayer = 0,
                                                                       .arrayLayerCount = 1, .aspect = WGPUTextureAspect_All });
  return target_new(cid, 0, 0, w, h, 1);
}
void cw__pw_target_unwrap(unsigned target) {
  pw_obj *t = obj(target);
  if (!t) return;
  unsigned c = t->color;
  cw__pw_release(target);
  cw__pw_release(c);
}

bool cw__pw_msaa_create(cw_rt *msaa, const cw_rt *scene) {
  init();
  memset(msaa, 0, sizeof *msaa);
  msaa->w = scene->w;
  msaa->h = scene->h;
  msaa->format = scene->format;
  msaa->tex = tex_new("cw scene MSAA colour", scene->w, scene->h, rt_format(scene->format), WGPUTextureUsage_RenderAttachment, 4);
  msaa->depth_tex = tex_new("cw scene MSAA depth", scene->w, scene->h, WGPUTextureFormat_Depth32Float,
                            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, 4);
  msaa->fbo = target_new(msaa->tex, msaa->depth_tex, scene->tex, scene->w, scene->h, 4);
  return true;
}

bool cw__pw_core(void) { return cw__wgpu_core(); }

/* the same MSAA attachments resolving into another scene colour of their
 * size and format (split-screen views each keep their own scene target):
 * only the target id changes */
void cw__pw_msaa_retarget(cw_rt *msaa, const cw_rt *scene) {
  pw_obj *t = obj(msaa->fbo);
  if (t && t->resolve == scene->tex) return;
  cw__pw_release(msaa->fbo);
  msaa->fbo = target_new(msaa->tex, msaa->depth_tex, scene->tex, scene->w, scene->h, 4);
}

/* the attachments of a target id (0 = the window) */
typedef struct att {
  WGPUTextureView color, depth, resolve;
  WGPUTextureFormat cfmt, dfmt;
  int samples, w, h;
  bool window;
} att;

static bool target_att(unsigned id, att *a) {
  memset(a, 0, sizeof *a);
  a->samples = 1;
  if (!id) {
    a->color = cw__wgpu_frame_view();
    a->cfmt = cw__wgpu_frame_format();
    cw__wgpu_frame_size(&a->w, &a->h);
    a->window = true;
    return a->color != NULL;
  }
  pw_obj *t = obj(id);
  if (!t || !t->target) return false;
  pw_obj *c = obj(t->color), *d = obj(t->depth), *r = obj(t->resolve);
  if (!c) return false;
  a->color = c->attach;
  a->cfmt = c->fmt;
  if (d) a->depth = d->attach, a->dfmt = d->fmt;
  if (r) a->resolve = r->attach;
  a->samples = t->samples;
  a->w = t->w;
  a->h = t->h;
  return true;
}

/* ── GPU timing (native: timestamp queries) ─────────────────────────── */

#define TS_MAX 256
typedef struct ts_slot { cw_pipeline *p; const char *name, *lane; } ts_slot; /* lane: the profiler's name (split screen: "ssao@v1") */
static struct {
  bool avail, on, checked; /* avail: the device has timestamps; on: requested too (cw__gpu_timing_wanted) */
  WGPUQuerySet qs;
  WGPUBuffer resolve, readback;
  int used;              /* queries written this frame */
  ts_slot slots[TS_MAX / 2];
  int slot_of[TS_MAX / 2]; /* render pass i -> slot */
  int nslots, cur;
  bool mapping;
  bool hold;     /* cw__pw_timing_hold: no read back at a flush */
  bool frame_ok; /* the frame began with the last read back done: every pass of it is timed */
  int whole;     /* cw__pw_timing_whole: the begin query of the span round the whole pass (-1 none) */
  /* the frame being read back */
  uint64_t rframe; /* its engine frame index (cw_frame_index, for the profiler) */
  ts_slot rslots[TS_MAX / 2];
  int rslot_of[TS_MAX / 2];
  int rpairs;
} T;

/* The query set, resolve and readback map cost every frame: made and used
 * only once timing is requested (CW_GPU_TIMERS=1, the "pipeline" command). */
static void timing_check(void) {
  if (!T.checked) {
    T.checked = true;
    T.avail = cw__wgpu_timestamps();
  }
  if (T.on || !T.avail || !cw__gpu_timing_wanted()) return;
  T.on = true;
  WGPUQuerySetDescriptor qd = { .label = SV("cw pass timing"), .type = WGPUQueryType_Timestamp, .count = TS_MAX };
  T.qs = wgpuDeviceCreateQuerySet(W.dev, &qd);
  T.resolve = wgpuDeviceCreateBuffer(W.dev, &(WGPUBufferDescriptor){ .usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc,
                                                                    .size = TS_MAX * 8 });
  T.readback = wgpuDeviceCreateBuffer(W.dev, &(WGPUBufferDescriptor){ .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                                                                     .size = TS_MAX * 8 });
  T.cur = -1;
  T.whole = -1;
}

void cw__pw_timing_pass(cw_pipeline *p, const char *name, const char *lane) {
  init();
  timing_check();
  if (!T.on) return;
  if (!name) {
    T.cur = -1;
    return;
  }
  if (T.nslots >= TS_MAX / 2) {
    T.cur = -1;
    return;
  }
  T.slots[T.nslots] = (ts_slot){ p, name, lane ? lane : name };
  T.cur = T.nslots++;
}

void cw__pw_forget(cw_pipeline *p) {
  for (int i = 0; i < TS_MAX / 2; i++) {
    if (T.slots[i].p == p) T.slots[i].p = NULL;
    if (T.rslots[i].p == p) T.rslots[i].p = NULL;
  }
}

static bool timing_writes(WGPUPassTimestampWrites *tw) {
  if (!T.on || T.whole >= 0 || T.mapping || !T.frame_ok || T.cur < 0 || T.used + 2 > TS_MAX) return false;
  T.slot_of[T.used / 2] = T.cur;
  *tw = (WGPUPassTimestampWrites){ .querySet = T.qs, .beginningOfPassWriteIndex = (uint32_t)T.used,
                                   .endOfPassWriteIndex = (uint32_t)T.used + 1 };
  T.used += 2;
  return true;
}

static void on_ts_map(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) {
  (void)msg, (void)u1, (void)u2;
  if (st == WGPUMapAsyncStatus_Success) {
    const uint64_t *v = wgpuBufferGetConstMappedRange(T.readback, 0, (size_t)T.rpairs * 16);
    float ms[TS_MAX / 2] = { 0 };
    bool seen[TS_MAX / 2] = { 0 };
    for (int i = 0; v && i < T.rpairs; i++) {
      int s = T.rslot_of[i];
      if (v[i * 2 + 1] >= v[i * 2]) ms[s] += (float)(v[i * 2 + 1] - v[i * 2]) / 1e6f;
      seen[s] = true;
    }
    /* the profiler's lane: a pass's runs under one lane name add up (split screen names each view apart) */
    if (CW_PROFILE_ON()) {
      float lm[TS_MAX / 2];
      bool ls[TS_MAX / 2];
      memcpy(lm, ms, sizeof lm);
      memcpy(ls, seen, sizeof ls);
      for (int s = 0; s < TS_MAX / 2; s++)
        for (int e = 0; ls[s] && e < s; e++)
          if (ls[e] && T.rslots[e].p == T.rslots[s].p && T.rslots[e].lane == T.rslots[s].lane) {
            lm[e] += lm[s];
            ls[s] = false;
          }
      for (int s = 0; s < TS_MAX / 2; s++)
        if (ls[s] && T.rslots[s].p) cw__profile_gpu(T.rslots[s].lane, T.rframe, lm[s]);
    }
    /* a pass timed more than once in the frame (splitscreen: once per view) adds up */
    for (int s = 0; s < TS_MAX / 2; s++)
      for (int e = 0; seen[s] && e < s; e++)
        if (seen[e] && T.rslots[e].p == T.rslots[s].p && T.rslots[e].name == T.rslots[s].name) {
          ms[e] += ms[s];
          seen[s] = false;
        }
    for (int s = 0; s < TS_MAX / 2; s++)
      if (seen[s] && T.rslots[s].p) cw__pipeline_gpu_time(T.rslots[s].p, T.rslots[s].name, ms[s]);
    wgpuBufferUnmap(T.readback);
  }
  T.mapping = false;
}

void cw__pw_timing_hold(bool hold) { T.hold = hold; }

/* the whole current pass as one span (the profiler's GPU lane on a native run): everything recorded on the frame
 * encoder between the two, three.c's passes and compute included, where a pass's own render passes alone miss
 * the shadow, G-buffer and probe renders three.c records for it. Inside it no other timestamp is written. */
void cw__pw_timing_whole(bool end) {
  if (!T.on) return;
  if (!end) {
    if (T.whole >= 0) return;
    WGPUPassTimestampWrites tw;
    WGPUCommandEncoder enc = cw__pw_encoder();
    if (!timing_writes(&tw)) return;
    T.whole = (int)tw.beginningOfPassWriteIndex;
    tw.endOfPassWriteIndex = WGPU_QUERY_SET_INDEX_UNDEFINED;
    WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(enc, &(WGPUComputePassDescriptor){ .timestampWrites = &tw });
    wgpuComputePassEncoderEnd(cp);
    wgpuComputePassEncoderRelease(cp);
    return;
  }
  if (T.whole < 0) return;
  WGPUCommandEncoder enc = cw__pw_encoder(); /* closes the pass's open render pass */
  WGPUPassTimestampWrites tw = { .querySet = T.qs, .beginningOfPassWriteIndex = WGPU_QUERY_SET_INDEX_UNDEFINED,
                                 .endOfPassWriteIndex = (uint32_t)T.whole + 1 };
  T.whole = -1;
  WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(enc, &(WGPUComputePassDescriptor){ .timestampWrites = &tw });
  wgpuComputePassEncoderEnd(cp);
  wgpuComputePassEncoderRelease(cp);
}

void cw__pw_timing_span(void *enc, bool end) {
  static int open = -1; /* the begin query of the span being written */
  if (T.whole >= 0) return; /* inside a whole-pass span */
  WGPUPassTimestampWrites tw = { .querySet = NULL, .beginningOfPassWriteIndex = WGPU_QUERY_SET_INDEX_UNDEFINED,
                                 .endOfPassWriteIndex = WGPU_QUERY_SET_INDEX_UNDEFINED };
  if (!end) {
    open = -1;
    if (!enc || !timing_writes(&tw)) return;
    open = (int)tw.beginningOfPassWriteIndex;
    tw.endOfPassWriteIndex = WGPU_QUERY_SET_INDEX_UNDEFINED;
  } else {
    if (open < 0 || !enc) return;
    tw = (WGPUPassTimestampWrites){ .querySet = T.qs, .beginningOfPassWriteIndex = WGPU_QUERY_SET_INDEX_UNDEFINED,
                                    .endOfPassWriteIndex = (uint32_t)open + 1 };
    open = -1;
  }
  WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass((WGPUCommandEncoder)enc, &(WGPUComputePassDescriptor){ .timestampWrites = &tw });
  wgpuComputePassEncoderEnd(cp);
  wgpuComputePassEncoderRelease(cp);
}

static void timing_resolve(void) {
  if (T.hold) return;
  if (!T.on || !T.used) {
    T.nslots = 0;
    return;
  }
  wgpuCommandEncoderResolveQuerySet(W.enc, T.qs, 0, (uint32_t)T.used, T.resolve, 0);
  wgpuCommandEncoderCopyBufferToBuffer(W.enc, T.resolve, 0, T.readback, 0, (uint64_t)T.used * 8);
  memcpy(T.rslots, T.slots, sizeof T.slots);
  memcpy(T.rslot_of, T.slot_of, sizeof T.slot_of);
  T.rpairs = T.used / 2;
  T.rframe = cw_frame_index();
  T.used = 0;
  T.nslots = 0;
  T.cur = -1;
  T.mapping = true; /* the map is requested after the submit */
}

void cw__pw_timing_poll(void) {
  /* the copy into the readback buffer was submitted with the last flush:
   * map it now (results arrive while the instance processes events) */
  static bool requested;
  if (!T.on) return;
  T.frame_ok = !T.mapping; /* a frame timed only in part would read short */
  if (T.mapping && !requested && !W.enc) {
    requested = true;
    wgpuBufferMapAsync(T.readback, WGPUMapMode_Read, 0, (size_t)T.rpairs * 16,
                       (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_ts_map });
  }
  if (!T.mapping) requested = false;
}

/* ── render passes ──────────────────────────────────────────────────── */

static WGPURenderPassEncoder begin(const att *a, const WGPUColor *clear, bool clear_depth, bool timed) {
  WGPURenderPassColorAttachment ca = { .view = a->color, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .resolveTarget = a->resolve,
                                       .loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load, .storeOp = WGPUStoreOp_Store };
  if (clear) ca.clearValue = *clear;
  WGPURenderPassDepthStencilAttachment da = { .view = a->depth, .depthLoadOp = clear_depth ? WGPULoadOp_Clear : WGPULoadOp_Load,
                                              .depthStoreOp = WGPUStoreOp_Store, .depthClearValue = 1.0f };
  WGPUPassTimestampWrites tw;
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = 1, .colorAttachments = &ca, .depthStencilAttachment = a->depth ? &da : NULL,
                                  .timestampWrites = timed && timing_writes(&tw) ? &tw : NULL };
  return wgpuCommandEncoderBeginRenderPass(raw_encoder(), &rp);
}

static void end(WGPURenderPassEncoder p) {
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
}

static void clear_target(unsigned target, bool depth) {
  att a;
  if (!target_att(target, &a)) return;
  close_open();
  WGPUColor c = { 0, 0, 0, 0 };
  end(begin(&a, &c, depth && a.depth, false));
}

/* a render pass for the next draw into the current target: the open one, or a new one */
static bool keep_open(void) { return !T.on; }
static WGPURenderPassEncoder pass_for(const att *a, const WGPUColor *clear, bool clear_depth, bool timed) {
  if (!clear && !clear_depth && OP.p && OP.target == W.target) return OP.p;
  close_open();
  WGPURenderPassEncoder p = begin(a, clear, clear_depth, timed);
  if (keep_open()) {
    OP.p = p;
    OP.target = W.target;
  }
  return p;
}
static void pass_done(WGPURenderPassEncoder p) {
  if (p != OP.p) end(p);
}

/* ── programs ───────────────────────────────────────────────────────── */

enum { U_F32, U_I32, U_U32, U_V2, U_V3, U_V4, U_M4, U_ARRAY };
enum { B_UNIFORM, B_TEX2D, B_TEX2D_UNF, B_TEX3D, B_DEPTH, B_DEPTH_MS };

typedef struct pw_member { char name[32]; int off, type, size; } pw_member;
typedef struct pw_bind { int binding, kind; char name[32]; } pw_bind;
typedef struct pw_prog {
  char name[48];
  WGPUShaderModule mod;
  const char *entry;
  bool own_vs; /* the module defines @vertex fn vs (cw_pass_draw geometry) */
  WGPUBindGroupLayout bgl;
  WGPUPipelineLayout pl;
  pw_member m[48];
  int nm, usize, ubinding;
  uint8_t *shadow;
  pw_bind b[MAX_UNITS + 1];
  int nb;
  CW_ARRAY(uint64_t) warned;
  /* group 0's bind groups by (uniform buffer, views): the uniform block binds
   * with a dynamic offset, so a pass drawing with the same textures every
   * frame reuses one (a bind group per draw per frame was ~1/3 of the
   * frame's WebGPU object churn); dropped when any registered texture is
   * released (bg_epoch) */
  struct { WGPUBindGroup g; WGPUBuffer buf; WGPUTextureView v[MAX_UNITS + 1]; uint32_t epoch; } bgc[8];
  int bgc_next;
} pw_prog;

static CW_ARRAY(pw_prog *) progs;

typedef struct pipe_rec { uint64_t key; WGPURenderPipeline p; } pipe_rec;
static CW_ARRAY(pipe_rec) pipes;

static const char *WGSL_HEADER =
    "struct CwTarget { size: vec2f, flip: f32, pad: f32 };\n"
    "@group(1) @binding(0) var<uniform> cw_target: CwTarget;\n"
    "@group(1) @binding(1) var cw_linear: sampler;\n"
    "@group(1) @binding(2) var cw_nearest: sampler;\n"
    "@group(1) @binding(3) var cw_compare: sampler_comparison;\n"
    "fn cw_frag_coord(p: vec4f) -> vec2f {\n"
    "  if (cw_target.flip > 0.0) { return vec2f(p.x, cw_target.size.y - p.y); }\n"
    "  return p.xy;\n"
    "}\n"
    "fn cw_load(t: texture_2d<f32>, uv: vec2f) -> vec4f {\n"
    "  let s = vec2i(textureDimensions(t));\n"
    "  return textureLoad(t, clamp(vec2i(floor(uv * vec2f(s))), vec2i(0), s - 1), 0);\n"
    "}\n";

/* a control for the gates (native test runs only): CW_TEST_BREAK_PROGRAM=name
 * compiles that program, and any fused program that runs it as a step, with
 * a fragment entry that outputs magenta */
static bool broken(const char *name) {
#if CW_CART
  (void)name;
  return false;
#else
  const char *b = getenv("CW_TEST_BREAK_PROGRAM");
  return b && cw__program_has_step(name, b);
#endif
}

static const char *skip_ws(const char *s) {
  for (;;) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (s[0] == '/' && s[1] == '/') {
      while (*s && *s != '\n') s++;
      continue;
    }
    if (s[0] == '/' && s[1] == '*') {
      const char *e = strstr(s + 2, "*/");
      s = e ? e + 2 : s + strlen(s);
      continue;
    }
    return s;
  }
}

static const char *ident(const char *s, char *out, size_t cap) {
  size_t n = 0;
  while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_') {
    if (n + 1 < cap) out[n++] = *s;
    s++;
  }
  out[n] = 0;
  return s;
}

/* a WGSL type (up to ',' / '}' / ';' at depth 0) -> kind, size, align */
static bool type_info(const char *t, int *type, int *size, int *align) {
  char b[64];
  size_t n = 0;
  for (const char *c = t; *c && n + 1 < sizeof b; c++)
    if (*c != ' ' && *c != '\t' && *c != '\n') b[n++] = *c;
  b[n] = 0;
  if (!strcmp(b, "f32")) *type = U_F32, *size = 4, *align = 4;
  else if (!strcmp(b, "i32")) *type = U_I32, *size = 4, *align = 4;
  else if (!strcmp(b, "u32")) *type = U_U32, *size = 4, *align = 4;
  else if (!strcmp(b, "vec2f") || !strcmp(b, "vec2<f32>")) *type = U_V2, *size = 8, *align = 8;
  else if (!strcmp(b, "vec3f") || !strcmp(b, "vec3<f32>")) *type = U_V3, *size = 12, *align = 16;
  else if (!strcmp(b, "vec4f") || !strcmp(b, "vec4<f32>")) *type = U_V4, *size = 16, *align = 16;
  else if (!strcmp(b, "mat4x4f") || !strcmp(b, "mat4x4<f32>")) *type = U_M4, *size = 64, *align = 16;
  else if (!strncmp(b, "array<", 6)) {
    char *comma = strrchr(b, ',');
    if (!comma) return false;
    int count = atoi(comma + 1);
    *comma = 0;
    int et, es, ea;
    if (!type_info(b + 6, &et, &es, &ea)) return false;
    int stride = ((es + ea - 1) / ea) * ea;
    stride = (stride + 15) & ~15;
    *type = U_ARRAY;
    *size = stride * count;
    *align = 16;
  } else return false;
  return true;
}

/* name is the first argument of some call to fn (fn a prefix: textureSample*) */
static bool used_as_arg(const char *src, const char *fn, const char *name) {
  size_t l = strlen(name);
  for (const char *f = strstr(src, fn); f; f = strstr(f + 1, fn)) {
    const char *a = f + strlen(fn);
    while ((*a >= 'a' && *a <= 'z') || (*a >= 'A' && *a <= 'Z')) a++;
    if (*a != '(') continue;
    a = skip_ws(a + 1);
    if (!strncmp(a, name, l) && !((a[l] >= 'a' && a[l] <= 'z') || (a[l] >= 'A' && a[l] <= 'Z') || (a[l] >= '0' && a[l] <= '9') || a[l] == '_'))
      return true;
  }
  return false;
}

/* reflect the group(0) declarations and the uniform struct of a module */
static bool reflect(pw_prog *pr, const char *src) {
  char ustruct[64] = { 0 };
  for (const char *s = strstr(src, "@group(0)"); s; s = strstr(s + 1, "@group(0)")) {
    const char *b = strstr(s, "@binding(");
    if (!b || b - s > 16) continue;
    int binding = atoi(b + 9);
    const char *v = skip_ws(strchr(b, ')') + 1);
    if (strncmp(v, "var", 3)) continue;
    v += 3;
    bool uni = false;
    if (*v == '<') {
      uni = !strncmp(v, "<uniform>", 9);
      v = strchr(v, '>') + 1;
    }
    char name[32], type[64];
    v = ident(skip_ws(v), name, sizeof name);
    v = skip_ws(v);
    if (*v != ':') continue;
    v = skip_ws(v + 1);
    size_t n = 0;
    while (*v && *v != ';' && n + 1 < sizeof type) type[n++] = *v++;
    type[n] = 0;
    if (pr->nb >= (int)CW_ARRAY_LEN(pr->b)) {
      cw_error("pipeline pass '%s': too many group(0) bindings", pr->name);
      return false;
    }
    pw_bind *pb = &pr->b[pr->nb++];
    pb->binding = binding;
    snprintf(pb->name, sizeof pb->name, "%s", name);
    if (uni) {
      pb->kind = B_UNIFORM;
      ident(type, ustruct, sizeof ustruct);
      pr->ubinding = binding;
    } else if (!strncmp(type, "texture_3d", 10)) pb->kind = B_TEX3D;
    else if (!strncmp(type, "texture_depth_multisampled_2d", 29)) pb->kind = B_DEPTH_MS;
    else if (!strncmp(type, "texture_depth_2d", 16)) pb->kind = B_DEPTH;
    else if (!strncmp(type, "texture_2d", 10)) {
      /* unfilterable (R32F linear depth) when it is read with textureLoad /
       * cw_load and never given to textureSample* */
      pb->kind = used_as_arg(src, "textureSample", name) || !(used_as_arg(src, "textureLoad", name) || used_as_arg(src, "cw_load", name))
                     ? B_TEX2D : B_TEX2D_UNF;
    } else {
      cw_error("pipeline pass '%s': binding %d (%s: %s) is not a type cw_pass_program_wgsl binds", pr->name, binding, name, type);
      return false;
    }
  }
  if (!ustruct[0]) return true;
  char pat[80];
  snprintf(pat, sizeof pat, "struct %s", ustruct);
  const char *s = strstr(src, pat);
  if (!s || !(s = strchr(s, '{'))) {
    cw_error("pipeline pass '%s': no struct %s for the uniform block", pr->name, ustruct);
    return false;
  }
  s++;
  int off = 0, maxa = 16;
  for (;;) {
    s = skip_ws(s);
    if (*s == '}' || !*s) break;
    char name[32];
    s = ident(s, name, sizeof name);
    s = skip_ws(s);
    if (*s != ':') {
      cw_error("pipeline pass '%s': cannot read struct %s near '%.20s'", pr->name, ustruct, s);
      return false;
    }
    s++;
    char type[64];
    size_t n = 0;
    int depth = 0;
    while (*s && (depth || (*s != ',' && *s != '}')) && n + 1 < sizeof type) {
      if (*s == '<') depth++;
      if (*s == '>') depth--;
      type[n++] = *s++;
    }
    type[n] = 0;
    if (*s == ',') s++;
    int t, size, align;
    if (!type_info(type, &t, &size, &align)) {
      cw_error("pipeline pass '%s': uniform %s has a type the reflection does not know (%s)", pr->name, name, type);
      return false;
    }
    off = (off + align - 1) / align * align;
    if (pr->nm >= (int)CW_ARRAY_LEN(pr->m)) return false;
    pw_member *m = &pr->m[pr->nm++];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->off = off;
    m->type = t;
    m->size = size;
    off += size;
    if (align > maxa) maxa = align;
  }
  pr->usize = (off + maxa - 1) / maxa * maxa;
  if (!pr->usize) pr->usize = 16;
  pr->shadow = cw_calloc(1, (size_t)pr->usize, CW_MEM_RENDER);
  return true;
}

static pw_prog *prog_of(unsigned id) {
  if (!id || id > (unsigned)progs.count) return NULL;
  return progs.data[id - 1];
}

unsigned cw__pw_program(const char *name, const char *wgsl) {
  init();
  pw_prog *pr = cw_calloc(1, sizeof *pr, CW_MEM_RENDER);
  snprintf(pr->name, sizeof pr->name, "%s", name);
  bool brk = broken(name);
  static const char *BROKEN = "\n@fragment fn cw_broken_fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {\n"
                              "  return vec4f(1.0, 0.0, 1.0, 1.0);\n}\n";
  size_t n = strlen(WGSL_HEADER) + strlen(wgsl) + strlen(BROKEN) + 1;
  char *src = cw_alloc(n, CW_MEM_RENDER);
  snprintf(src, n, "%s%s%s", WGSL_HEADER, wgsl, brk ? BROKEN : "");
  pr->entry = brk ? "cw_broken_fs" : "fs";
  pr->own_vs = strstr(wgsl, "@vertex fn vs(") != NULL;
  if (brk) cw_warn("CW_TEST_BREAK_PROGRAM: pass program '%s' outputs magenta (a gate control)", name);
  bool ok = reflect(pr, wgsl);
  uint32_t errs = cw_gpu_error_count();
  if (ok) pr->mod = cw__wgpu_shader(name, src);
  cw_free(src);
  if (!ok || !pr->mod || cw_gpu_error_count() != errs) {
    if (ok) cw_error("pipeline pass '%s': the WGSL module failed to compile (the WebGPU error above)", name);
    if (pr->mod) wgpuShaderModuleRelease(pr->mod);
    cw_free(pr->shadow);
    cw_free(pr);
    return 0;
  }
  WGPUBindGroupLayoutEntry e[MAX_UNITS + 1];
  for (int i = 0; i < pr->nb; i++) {
    const pw_bind *b = &pr->b[i];
    e[i] = (WGPUBindGroupLayoutEntry){ .binding = (uint32_t)b->binding,
                                       .visibility = WGPUShaderStage_Fragment | (b->kind == B_UNIFORM ? WGPUShaderStage_Vertex : 0) };
    switch (b->kind) {
    case B_UNIFORM: e[i].buffer = (WGPUBufferBindingLayout){ .type = WGPUBufferBindingType_Uniform, .hasDynamicOffset = true, .minBindingSize = (uint64_t)pr->usize }; break;
    case B_TEX2D: e[i].texture = (WGPUTextureBindingLayout){ .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2D }; break;
    case B_TEX2D_UNF: e[i].texture = (WGPUTextureBindingLayout){ .sampleType = WGPUTextureSampleType_UnfilterableFloat, .viewDimension = WGPUTextureViewDimension_2D }; break;
    case B_TEX3D: e[i].texture = (WGPUTextureBindingLayout){ .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_3D }; break;
    case B_DEPTH: e[i].texture = (WGPUTextureBindingLayout){ .sampleType = WGPUTextureSampleType_Depth, .viewDimension = WGPUTextureViewDimension_2D }; break;
    default:
      e[i].texture = (WGPUTextureBindingLayout){ .sampleType = WGPUTextureSampleType_Depth, .viewDimension = WGPUTextureViewDimension_2D, .multisampled = true };
      break;
    }
  }
  pr->bgl = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .label = SV(name), .entryCount = (size_t)pr->nb, .entries = e });
  WGPUBindGroupLayout layouts[2] = { pr->bgl, W.bgl_engine };
  pr->pl = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 2, .bindGroupLayouts = layouts });
  cw_array_push(&progs, pr);
  return (unsigned)progs.count;
}

void cw__pw_use(unsigned prog) { W.prog = prog; }

static pw_member *member(pw_prog *pr, const char *name) {
  for (int i = 0; i < pr->nm; i++)
    if (!strcmp(pr->m[i].name, name)) return &pr->m[i];
  return NULL;
}

/* kind: 0 float[n], 1 int, 2 mat4 */
void cw__pw_uniform(unsigned prog, const char *name, int n, const float *f, int iv) {
  pw_prog *pr = prog_of(prog);
  if (!pr) return;
  pw_member *m = member(pr, name);
  if (!m) {
    /* a texture's name (GL code sets sampler units by name): nothing to do */
    for (int i = 0; i < pr->nb; i++)
      if (!strcmp(pr->b[i].name, name)) return;
    uint64_t h = 1469598103934665603ull;
    for (const char *c = name; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    for (int i = 0; i < pr->warned.count; i++)
      if (pr->warned.data[i] == h) return;
    cw_array_push(&pr->warned, h);
    cw_warn("pipeline pass '%s': no uniform '%s' in its WGSL uniform block", pr->name, name);
    return;
  }
  uint8_t *d = pr->shadow + m->off;
  if (n < 0) { /* int */
    if (m->type == U_F32) {
      float v = (float)iv;
      memcpy(d, &v, 4);
    } else memcpy(d, &iv, 4);
    return;
  }
  if (n == 1 && (m->type == U_I32 || m->type == U_U32)) {
    int32_t v = (int32_t)f[0];
    memcpy(d, &v, 4);
    return;
  }
  size_t bytes = (size_t)n * 4;
  if (bytes > (size_t)m->size) bytes = (size_t)m->size;
  memcpy(d, f, bytes);
}

void cw__pw_bind_texture(int unit, unsigned tex) {
  if (unit >= 0 && unit < MAX_UNITS) W.units[unit] = tex;
}

void cw__pw_set_blend(cw_pass_blend b) { W.blend = b; }
void cw__pw_set_depth(bool test, bool write) {
  W.depth_test = test;
  W.depth_write = write;
}

void cw__pw_state_reset(void) {
  W.blend = CW_PASS_BLEND_NONE;
  W.depth_test = W.depth_write = false;
}

void cw__pw_bind_target(const cw_rt *rt) {
  W.target = rt ? rt->fbo : 0;
  W.target_w = rt ? rt->w : 0;
  W.target_h = rt ? rt->h : 0;
}

static WGPUBlendState blend_of(cw_pass_blend b) {
  WGPUBlendComponent c;
  switch (b) {
  case CW_PASS_BLEND_ADD: c = (WGPUBlendComponent){ WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One }; break;
  case CW_PASS_BLEND_ALPHA:
    c = (WGPUBlendComponent){ WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha };
    break;
  default: c = (WGPUBlendComponent){ WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha }; break;
  }
  return (WGPUBlendState){ c, c };
}

/* depth mode: 0 none, 1 test, 2 write always, 3 test + write; nocolor: depth-only writes */
static WGPURenderPipeline pipeline(unsigned prog, const att *a, int blend, int dmode, bool nocolor) {
  pw_prog *pr = prog_of(prog);
  uint64_t key = (uint64_t)prog | ((uint64_t)a->cfmt << 16) | ((uint64_t)blend << 32) | ((uint64_t)dmode << 36) |
                 ((uint64_t)(a->depth != NULL) << 40) | ((uint64_t)a->dfmt << 41) | ((uint64_t)(a->samples > 1) << 56) |
                 ((uint64_t)nocolor << 57);
  for (int i = 0; i < pipes.count; i++)
    if (pipes.data[i].key == key) return pipes.data[i].p;
  WGPUBlendState bs = blend_of((cw_pass_blend)blend);
  WGPUColorTargetState ct = { .format = a->cfmt, .blend = blend ? &bs : NULL,
                              .writeMask = nocolor ? WGPUColorWriteMask_None : WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = pr->mod, .entryPoint = SV(pr->entry), .targetCount = 1, .targets = &ct };
  WGPUDepthStencilState ds = {
    .format = a->dfmt, .depthWriteEnabled = (dmode & 2) ? WGPUOptionalBool_True : WGPUOptionalBool_False,
    .depthCompare = (dmode & 1) ? WGPUCompareFunction_Less : WGPUCompareFunction_Always,
    .stencilFront = { .compare = WGPUCompareFunction_Always, .failOp = WGPUStencilOperation_Keep,
                      .depthFailOp = WGPUStencilOperation_Keep, .passOp = WGPUStencilOperation_Keep },
    .stencilBack = { .compare = WGPUCompareFunction_Always, .failOp = WGPUStencilOperation_Keep,
                     .depthFailOp = WGPUStencilOperation_Keep, .passOp = WGPUStencilOperation_Keep },
  };
  WGPURenderPipelineDescriptor d = {
    .label = SV(pr->name),
    .layout = pr->pl,
    .vertex = { .module = pr->own_vs ? pr->mod : W.vs, .entryPoint = SV("vs") },
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
    .depthStencil = a->depth ? &ds : NULL,
    .multisample = { .count = (uint32_t)a->samples, .mask = 0xffffffffu },
    .fragment = &fs,
  };
  WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(W.dev, &d);
  cw_array_push(&pipes, ((pipe_rec){ key, p }));
  return p;
}

static WGPUTextureView unit_view(const pw_bind *b, unsigned id) {
  pw_obj *o = obj(id);
  bool ok = o && o->view && !o->target;
  if (ok) {
    bool depthfmt = o->fmt == WGPUTextureFormat_Depth32Float;
    WGPUTextureDimension dim = o->dim;
    switch (b->kind) {
    case B_TEX3D: ok = dim == WGPUTextureDimension_3D; break;
    case B_DEPTH: ok = depthfmt && o->samples == 1; break;
    case B_DEPTH_MS: ok = depthfmt && o->samples > 1; break;
    case B_TEX2D: ok = !depthfmt && dim == WGPUTextureDimension_2D && o->fmt != WGPUTextureFormat_R32Float; break;
    default: ok = !depthfmt && dim == WGPUTextureDimension_2D; break;
    }
  }
  if (ok) return o->view;
  if (id) cw__wgpu_fail("pipeline: texture %u does not fit binding %d (%s)", id, b->binding, b->name);
  switch (b->kind) {
  case B_TEX3D: return cw__pw_view(W.dummy3d);
  case B_TEX2D_UNF: return cw__pw_view(W.dummy2d_unf);
  case B_DEPTH: return cw__pw_view(W.dummy_depth);
  default: return cw__pw_view(W.dummy2d);
  }
}

static void draw(bool nocolor_depth_only, int vertices) {
  pw_prog *pr = prog_of(W.prog);
  if (!pr) {
    cw__wgpu_fail("pipeline: cw_pass_fullscreen without a WGSL program (cw_pass_use)");
    return;
  }
  att a;
  if (!target_att(W.target, &a)) {
    cw__wgpu_fail("pipeline: cw_pass_fullscreen without a target");
    return;
  }
  int dmode = (W.depth_test ? 1 : 0) | (W.depth_write ? 2 : 0);
  WGPURenderPipeline pipe = pipeline(W.prog, &a, (int)W.blend, a.depth ? dmode : 0, nocolor_depth_only);
  /* group 0: the program's uniform block and textures */
  WGPUBindGroupEntry e[MAX_UNITS + 1];
  WGPUTextureView views[MAX_UNITS + 1] = { 0 };
  WGPUBuffer ubuf = NULL;
  uint32_t dyn0[1];
  int nd0 = 0;
  for (int i = 0; i < pr->nb; i++) {
    const pw_bind *b = &pr->b[i];
    e[i] = (WGPUBindGroupEntry){ .binding = (uint32_t)b->binding };
    if (b->kind == B_UNIFORM) {
      uint64_t off;
      ubuf = e[i].buffer = cw__pw_ubo(pr->shadow, (size_t)pr->usize, &off);
      e[i].offset = 0;
      e[i].size = (uint64_t)pr->usize;
      dyn0[nd0++] = (uint32_t)off;
    } else {
      int unit = b->binding - 1;
      views[i] = e[i].textureView = unit_view(b, unit >= 0 && unit < MAX_UNITS ? W.units[unit] : 0);
    }
  }
  WGPUBindGroup g0 = NULL;
  for (int k = 0; k < 8 && !g0; k++)
    if (pr->bgc[k].g && pr->bgc[k].epoch == bg_epoch && pr->bgc[k].buf == ubuf && !memcmp(pr->bgc[k].v, views, sizeof views))
      g0 = pr->bgc[k].g;
  if (!g0) {
    g0 = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = pr->bgl, .entryCount = (size_t)pr->nb, .entries = e });
    int k = pr->bgc_next;
    pr->bgc_next = (k + 1) % 8;
    if (pr->bgc[k].g) cw__pw_defer_bind_group(pr->bgc[k].g); /* recorded commands may still name it this frame */
    pr->bgc[k].g = g0;
    pr->bgc[k].buf = ubuf;
    pr->bgc[k].epoch = bg_epoch;
    memcpy(pr->bgc[k].v, views, sizeof views);
  }
  /* group 1: the engine's target block (dynamic offset) and samplers, one per uniform chunk */
  float tb[4] = { (float)a.w, (float)a.h, a.window ? 1.0f : -1.0f, 0 };
  uint64_t toff;
  WGPUBuffer tbuf = cw__pw_ubo(tb, sizeof tb, &toff);
  uint32_t dyn1[1] = { (uint32_t)toff };
  WGPUBindGroup g1 = NULL;
  for (int k = 0; k < W.engine_bg.count && !g1; k++)
    if (W.engine_bg.data[k].buf == tbuf) g1 = W.engine_bg.data[k].g;
  if (!g1) {
    WGPUBindGroupEntry e1[4] = { { .binding = 0, .buffer = tbuf, .offset = 0, .size = 16 },
                                 { .binding = 1, .sampler = W.linear }, { .binding = 2, .sampler = W.nearest },
                                 { .binding = 3, .sampler = W.compare } };
    g1 = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = W.bgl_engine, .entryCount = 4, .entries = e1 });
    cw_array_push(&W.engine_bg, ((engine_bg){ tbuf, g1 }));
  }
  WGPURenderPassEncoder p = pass_for(&a, NULL, false, true);
  wgpuRenderPassEncoderSetPipeline(p, pipe);
  wgpuRenderPassEncoderSetBindGroup(p, 0, g0, (size_t)nd0, dyn0);
  wgpuRenderPassEncoderSetBindGroup(p, 1, g1, 1, dyn1);
  wgpuRenderPassEncoderDraw(p, (uint32_t)vertices, 1, 0, 0);
  pass_done(p);
}

void cw__pw_fullscreen(void) { draw(false, 3); }
void cw__pw_draw(int vertices) { draw(false, vertices); }

void cw__pw_clear(float r, float g, float b, float a, bool depth) {
  att t;
  if (!target_att(W.target, &t)) return;
  WGPUColor c = { r, g, b, a };
  pass_done(pass_for(&t, &c, depth && t.depth, false)); /* the next draws into this target go on in the same pass */
}

/* ── textures for LUTs and tables ───────────────────────────────────── */

unsigned cw__pw_texture_2d(int w, int h, WGPUTextureFormat f, const void *data, uint32_t bpr) {
  init();
  unsigned id = tex_new("cw pass texture", w, h, f, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, 1);
  WGPUTexelCopyTextureInfo d = { .texture = cw__pw_texture(id), .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout l = { .bytesPerRow = bpr, .rowsPerImage = (uint32_t)h };
  wgpuQueueWriteTexture(W.q, &d, data, (size_t)bpr * (size_t)h, &l, &(WGPUExtent3D){ (uint32_t)w, (uint32_t)h, 1 });
  return id;
}

unsigned cw__pw_rg8(int w, int h, const uint8_t *rg) { return cw__pw_texture_2d(w, h, WGPUTextureFormat_RG8Unorm, rg, (uint32_t)w * 2); }
unsigned cw__pw_pass_texture(int w, int h, cw_rt_format f, const void *t) {
  int bpp = f == CW_RT_RGBA16F ? 8 : f == CW_RT_RG16F ? 4 : f == CW_RT_R16F ? 2 : f == CW_RT_R8 ? 1 : 4;
  return cw__pw_texture_2d(w, h, rt_format(f), t, (uint32_t)(w * bpp));
}

unsigned cw__pw_texture_3d(int size, const uint16_t *half) {
  init();
  WGPUTextureDescriptor td = { .label = SV("cw LUT"), .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                               .dimension = WGPUTextureDimension_3D, .size = { (uint32_t)size, (uint32_t)size, (uint32_t)size },
                               .format = WGPUTextureFormat_RGBA16Float, .mipLevelCount = 1, .sampleCount = 1 };
  WGPUTexture t = wgpuDeviceCreateTexture(W.dev, &td);
  WGPUTexelCopyTextureInfo d = { .texture = t, .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout l = { .bytesPerRow = (uint32_t)size * 8, .rowsPerImage = (uint32_t)size };
  wgpuQueueWriteTexture(W.q, &d, half, (size_t)size * size * size * 8, &l, &(WGPUExtent3D){ (uint32_t)size, (uint32_t)size, (uint32_t)size });
  return cw__pw_register(t, wgpuTextureCreateView(t, NULL));
}
unsigned cw__pw_lut(int size, const uint16_t *half) { return cw__pw_texture_3d(size, half); }

/* ── depth copy ───────────────────────────────────────────── */

/* window depth (GL convention, [0, 1]) -> view-space distance with the
 * projection's z rows: ndc = (e10 zv + e14) / (e11 zv + e15).
 * Compatibility mode reads a depth texture only through a comparison
 * sampler (no textureLoad, no plain sampling, no depth copies), so the copy
 * recovers each texel EXACTLY by a binary search over the bit patterns of
 * [0, 1] floats (ordered like the integers for non-negative floats): 30
 * nearest-filtered LessEqual compares. */
static const char *DEPTH_COPY =
    "struct U { pz: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var t_depth: texture_depth_2d;\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let tc = pos.xy / vec2f(textureDimensions(t_depth));\n"
    "  var lo = 0u;\n"
    "  var hi = 0x3f800000u;\n"
    "  for (var i = 0; i < 31; i++) {\n"
    "    if (lo >= hi) { break; }\n"
    "    let mid = lo + (hi - lo + 1u) / 2u;\n"
    "    if (textureSampleCompareLevel(t_depth, cw_compare, tc, bitcast<f32>(mid)) > 0.5) { lo = mid; } else { hi = mid - 1u; }\n"
    "  }\n"
    "  let d = bitcast<f32>(lo);\n"
    "  if (d >= 1.0) { return vec4f(3.0e38, 0.0, 0.0, 1.0); }\n"
    "  let z = d * 2.0 - 1.0;\n"
    "  let zv = (u.pz.y - z * u.pz.w) / (z * u.pz.z - u.pz.x);\n"
    "  return vec4f(-zv, 0.0, 0.0, 1.0);\n"
    "}\n";
/* core WebGPU: the depth is read directly (one textureLoad: the same value
 * the compares find). The multisampled scene depth gives its sample 0, the
 * sample a GL blit resolving depth keeps (Mesa); the window-depth to distance
 * math is the compatibility copy's */
#define DEPTH_LINEAR_WGSL \
  "  if (d >= 1.0) { return vec4f(3.0e38, 0.0, 0.0, 1.0); }\n" \
  "  let z = d * 2.0 - 1.0;\n" \
  "  let zv = (u.pz.y - z * u.pz.w) / (z * u.pz.z - u.pz.x);\n" \
  "  return vec4f(-zv, 0.0, 0.0, 1.0);\n" \
  "}\n"
static const char *DEPTH_COPY_LOAD =
    "struct U { pz: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var t_depth: texture_depth_2d;\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let d = textureLoad(t_depth, vec2i(pos.xy), 0);\n" DEPTH_LINEAR_WGSL;
static const char *DEPTH_COPY_MS =
    "struct U { pz: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var t_depth: texture_depth_multisampled_2d;\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let d = textureLoad(t_depth, vec2i(pos.xy), 0);\n" DEPTH_LINEAR_WGSL;
/* the multisampled scene depth into the scene's own depth attachment (sample
 * 0, as GL's resolving blit), for SKY / EFFECTS passes that depth-test
 * against the resolved scene; the colour is not written */
static const char *DEPTH_RESOLVE =
    "@group(0) @binding(1) var t_depth: texture_depth_multisampled_2d;\n"
    "struct O { @location(0) c: vec4f, @builtin(frag_depth) d: f32 };\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> O {\n"
    "  var o: O;\n"
    "  o.c = vec4f(0.0);\n"
    "  o.d = textureLoad(t_depth, vec2i(pos.xy), 0);\n"
    "  return o;\n"
    "}\n";

void cw__pw_depth_resolve(const cw_rt *msaa, const cw_rt *scene) {
  static unsigned prog;
  pw_obj *d = obj(msaa->depth_tex);
  if (!d || d->samples < 2 || !scene->depth_tex || !cw__wgpu_core()) return;
  if (!prog && !(prog = cw__pw_program("depth_resolve", DEPTH_RESOLVE))) return;
  cw__pw_state_reset();
  cw__pw_bind_target(scene);
  cw__pw_use(prog);
  cw__pw_bind_texture(0, msaa->depth_tex);
  cw__pw_set_depth(false, true); /* always, written */
  draw(true, 3);
  cw__pw_state_reset();
}

void cw__pw_depth_copy(cw_pass_ctx *ctx, const cw_rt *src, const cw_rt *dst, const float pz[4]) {
  (void)ctx;
  static unsigned prog, prog_load, prog_ms;
  pw_obj *d = obj(src->depth_tex);
  if (!d) return;
  bool core = cw__wgpu_core();
  if (d->samples > 1 && !core) return; /* (compatibility mode: pipeline.c never asks with MSAA) */
  unsigned *pp = d->samples > 1 ? &prog_ms : core ? &prog_load : &prog;
  /* one name for the three (CW_TEST_BREAK_PROGRAM=depth_copy breaks whichever runs) */
  if (!*pp && !(*pp = cw__pw_program("depth_copy", d->samples > 1 ? DEPTH_COPY_MS : core ? DEPTH_COPY_LOAD : DEPTH_COPY)))
    return;
  cw__pw_state_reset();
  cw__pw_bind_target(dst);
  cw__pw_use(*pp);
  cw__pw_bind_texture(0, src->depth_tex);
  cw__pw_uniform(*pp, "pz", 4, pz, 0);
  draw(false, 3);
}

/* ── the accessor for passes that record their own render passes ────── */

const cw__wgpu_pass_targets *cw__pass_wgpu_targets(cw_pass_ctx *ctx) {
  att a;
  memset(&W.pt, 0, sizeof W.pt);
  if (!target_att(W.target, &a)) return &W.pt;
  W.pt.enc = cw__pw_encoder();
  W.pt.color = a.color;
  W.pt.color_format = a.cfmt;
  W.pt.depth = a.depth;
  W.pt.depth_format = a.dfmt;
  W.pt.linear_depth = ctx && ctx->depth_tex ? cw__pw_view(ctx->depth_tex) : NULL;
  W.pt.w = a.w;
  W.pt.h = a.h;
  W.pt.flip_y = !a.window;
  W.pt.samples = a.samples;
  W.pt.resolve = a.resolve;
  return &W.pt;
}

void cw__pw_frame_begin(void) {
  init();
  memset(W.units, 0, sizeof W.units);
  W.prog = 0;
  cw__pw_state_reset();
}

void cw__pw_probe_caps(cw_pipeline_caps *caps) {
  init();
  timing_check();
  caps->float_render = true;
  caps->float_blend = true;
  caps->timer_query = T.avail;
  caps->max_samples = 4;
  caps->hdr_format = "RGBA16F";
}
#endif
