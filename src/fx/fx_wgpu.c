/* Effects on WebGPU: shared helpers (resources, layouts, modules, the
 * asynchronous readback ring) and the shared textures (curl noise, the soft
 * dot, image assets). See fx_wgpu.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/platform.h"
#include "fx_internal.h"
#include "fx_wgpu.h"

#if CW_WGPU

unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file,
                                     int desired_channels);
void stbi_image_free(void *retval_from_stbi_load);

#define SV CW_WGPU_SV

const char CW_WGSL_COMMON[] =
    "fn ph(x0: u32) -> u32 { var x = x0; x = x ^ (x >> 16u); x = x * 0x7feb352du; x = x ^ (x >> 15u);\n"
    "  x = x * 0x846ca68bu; x = x ^ (x >> 16u); return x; }\n"
    "fn pseed(seed: u32, i: i32, k: i32) -> u32 { return ph(seed ^ ph((u32(i) * 0x9E3779B9u) ^ ph(u32(k) + 0x632BE5ABu))); }\n"
    "fn prnd(s: ptr<function, u32>) -> f32 { *s = ph(*s); return f32(*s >> 8u) * (1.0 / 16777216.0); }\n"
    "fn plife(seed: u32, lifetime: f32, life_rand: f32, i: i32, k: i32) -> f32 {\n"
    "  var s = pseed(seed, i, k); return lifetime * (1.0 - life_rand * prnd(&s)); }\n"
    /* a GL clip position (z in -w..w, y up) for a WebGPU pass: z into 0..w (w..0 with a
     * reversed depth buffer), y mirrored when the target keeps GL's bottom-up rows (flip = -1) */
    "fn cw_clip(p: vec4f, flip: f32) -> vec4f { return vec4f(p.x, p.y * flip, select((p.z + p.w) * 0.5, (p.w - p.z) * 0.5, CW_REV), p.w); }\n"
    "fn m3(m: mat4x4f) -> mat3x3f { return mat3x3f(m[0].xyz, m[1].xyz, m[2].xyz); }\n";

/* ── pass targets ─────────────────────────────────────────────────────── */

static const cw__wgpu_pass_targets *test_targets;
static cw__wgpu_pass_targets test_copy;

void cw__fx_wgpu_set_test_targets(const cw__wgpu_pass_targets *t) {
  if (t) {
    test_copy = *t;
    test_targets = &test_copy;
  } else test_targets = NULL;
}

const cw__wgpu_pass_targets *cw__fx_pass_targets(cw_pass_ctx *ctx) {
  return test_targets ? test_targets : cw__pass_wgpu_targets(ctx);
}

/* ── resources ────────────────────────────────────────────────────────── */

WGPUBuffer cw__wg_buffer(size_t bytes, WGPUBufferUsage usage, const char *label) {
  WGPUBufferDescriptor d = { .label = SV(label), .usage = usage, .size = (bytes + 15) & ~(size_t)15 };
  return wgpuDeviceCreateBuffer(cw__wgpu_device(), &d);
}

void cw__wg_write(WGPUBuffer b, size_t offset, const void *data, size_t bytes) {
  if (!bytes) return;
  if (bytes % 4 == 0) {
    wgpuQueueWriteBuffer(cw__wgpu_queue(), b, offset, data, bytes);
    return;
  }
  size_t n = (bytes + 3) & ~(size_t)3;
  uint8_t *tmp = cw_frame_alloc(n);
  memcpy(tmp, data, bytes);
  memset(tmp + bytes, 0, n - bytes);
  wgpuQueueWriteBuffer(cw__wgpu_queue(), b, offset, tmp, n);
}

WGPUTexture cw__wg_texture(WGPUTextureDimension dim, int w, int h, int layers, int mips, WGPUTextureFormat f,
                           WGPUTextureUsage usage, WGPUTextureViewDimension bind_dim, const char *label) {
  WGPUTextureBindingViewDimension vd = { .chain = { .sType = WGPUSType_TextureBindingViewDimension },
                                         .textureBindingViewDimension = bind_dim };
  WGPUTextureDescriptor d = { .nextInChain = bind_dim != WGPUTextureViewDimension_Undefined ? &vd.chain : NULL,
                              .label = SV(label), .usage = usage, .dimension = dim,
                              .size = { (uint32_t)w, (uint32_t)h, (uint32_t)(layers < 1 ? 1 : layers) },
                              .format = f, .mipLevelCount = (uint32_t)(mips < 1 ? 1 : mips), .sampleCount = 1 };
  return wgpuDeviceCreateTexture(cw__wgpu_device(), &d);
}

WGPUTextureView cw__wg_view(WGPUTexture t, WGPUTextureViewDimension dim, int base_mip, int mips, int base_layer,
                            int layers) {
  WGPUTextureViewDescriptor d = { .format = wgpuTextureGetFormat(t), .dimension = dim,
                                  .baseMipLevel = (uint32_t)base_mip, .mipLevelCount = (uint32_t)mips,
                                  .baseArrayLayer = (uint32_t)base_layer, .arrayLayerCount = (uint32_t)layers,
                                  .aspect = WGPUTextureAspect_All };
  return wgpuTextureCreateView(t, &d);
}

void cw__wg_tex_write(WGPUTexture t, int mip, int x, int y, int layer, int w, int h, int bpp, const void *px) {
  WGPUTexelCopyTextureInfo d = { .texture = t, .mipLevel = (uint32_t)mip,
                                 .origin = { (uint32_t)x, (uint32_t)y, (uint32_t)layer },
                                 .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout l = { .bytesPerRow = (uint32_t)(w * bpp), .rowsPerImage = (uint32_t)h };
  WGPUExtent3D e = { (uint32_t)w, (uint32_t)h, 1 };
  wgpuQueueWriteTexture(cw__wgpu_queue(), &d, px, (size_t)w * (size_t)h * (size_t)bpp, &l, &e);
}

WGPUSampler cw__wg_sampler(bool linear, bool repeat, bool mips) {
  static WGPUSampler cache[8];
  int k = (linear ? 1 : 0) | (repeat ? 2 : 0) | (mips ? 4 : 0);
  if (cache[k]) return cache[k];
  WGPUFilterMode f = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
  WGPUAddressMode a = repeat ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
  WGPUSamplerDescriptor d = { .addressModeU = a, .addressModeV = a, .addressModeW = a, .magFilter = f, .minFilter = f,
                              .mipmapFilter = mips && linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest,
                              .lodMinClamp = 0, .lodMaxClamp = mips ? 32 : 0, .maxAnisotropy = 1 };
  return cache[k] = wgpuDeviceCreateSampler(cw__wgpu_device(), &d);
}

WGPUSampler cw__wg_sampler_compare(bool linear) {
  static WGPUSampler cache[2];
  if (cache[linear]) return cache[linear];
  WGPUFilterMode f = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
  WGPUSamplerDescriptor d = { .addressModeU = WGPUAddressMode_ClampToEdge, .addressModeV = WGPUAddressMode_ClampToEdge,
                              .addressModeW = WGPUAddressMode_ClampToEdge, .magFilter = f, .minFilter = f,
                              .mipmapFilter = WGPUMipmapFilterMode_Nearest, .lodMinClamp = 0, .lodMaxClamp = 0,
                              .compare = WGPUCompareFunction_LessEqual, .maxAnisotropy = 1 };
  return cache[linear] = wgpuDeviceCreateSampler(cw__wgpu_device(), &d);
}

WGPUBindGroupLayout cw__wg_layout(const cw__wg_slot *s, int n) {
  WGPUBindGroupLayoutEntry e[16];
  memset(e, 0, sizeof e);
  for (int i = 0; i < n && i < 16; i++) {
    e[i].binding = (uint32_t)i;
    e[i].visibility = s[i].vis;
    WGPUTextureViewDimension dim = s[i].dim ? s[i].dim : WGPUTextureViewDimension_2D;
    switch (s[i].type) {
    case CW_WB_UNIFORM:
    case CW_WB_UNIFORM_DYN:
      e[i].buffer = (WGPUBufferBindingLayout){ .type = WGPUBufferBindingType_Uniform,
                                               .hasDynamicOffset = s[i].type == CW_WB_UNIFORM_DYN,
                                               .minBindingSize = s[i].min_size };
      break;
    case CW_WB_STORAGE:
    case CW_WB_STORAGE_RO:
      e[i].buffer = (WGPUBufferBindingLayout){ .type = s[i].type == CW_WB_STORAGE ? WGPUBufferBindingType_Storage
                                                                                   : WGPUBufferBindingType_ReadOnlyStorage,
                                               .minBindingSize = s[i].min_size };
      break;
    case CW_WB_TEX:
    case CW_WB_TEX_UNFILT:
      e[i].texture = (WGPUTextureBindingLayout){ .sampleType = s[i].type == CW_WB_TEX ? WGPUTextureSampleType_Float
                                                                                       : WGPUTextureSampleType_UnfilterableFloat,
                                                 .viewDimension = dim };
      break;
    case CW_WB_SAMPLER:
    case CW_WB_SAMPLER_NF:
      e[i].sampler = (WGPUSamplerBindingLayout){ .type = s[i].type == CW_WB_SAMPLER ? WGPUSamplerBindingType_Filtering
                                                                                     : WGPUSamplerBindingType_NonFiltering };
      break;
    case CW_WB_STORAGE_TEX:
      e[i].storageTexture = (WGPUStorageTextureBindingLayout){ .access = WGPUStorageTextureAccess_WriteOnly,
                                                               .format = s[i].format, .viewDimension = dim };
      break;
    case CW_WB_TEX_DEPTH:
      e[i].texture = (WGPUTextureBindingLayout){ .sampleType = WGPUTextureSampleType_Depth, .viewDimension = dim };
      break;
    case CW_WB_SAMPLER_CMP: e[i].sampler = (WGPUSamplerBindingLayout){ .type = WGPUSamplerBindingType_Comparison }; break;
    default: break;
    }
  }
  WGPUBindGroupLayoutDescriptor d = { .entryCount = (size_t)n, .entries = e };
  return wgpuDeviceCreateBindGroupLayout(cw__wgpu_device(), &d);
}

WGPUPipelineLayout cw__wg_pipeline_layout(const WGPUBindGroupLayout *l, int n) {
  WGPUPipelineLayoutDescriptor d = { .bindGroupLayoutCount = (size_t)n, .bindGroupLayouts = l };
  return wgpuDeviceCreatePipelineLayout(cw__wgpu_device(), &d);
}

WGPUBindGroup cw__wg_group(WGPUBindGroupLayout l, const cw__wg_entry *e, int n) {
  WGPUBindGroupEntry be[16];
  memset(be, 0, sizeof be);
  for (int i = 0; i < n && i < 16; i++) {
    be[i].binding = (uint32_t)i;
    be[i].buffer = e[i].buffer;
    be[i].offset = e[i].offset;
    be[i].size = e[i].buffer ? (e[i].size ? e[i].size : WGPU_WHOLE_SIZE) : 0;
    be[i].textureView = e[i].view;
    be[i].sampler = e[i].sampler;
  }
  WGPUBindGroupDescriptor d = { .layout = l, .entryCount = (size_t)n, .entries = be };
  return wgpuDeviceCreateBindGroup(cw__wgpu_device(), &d);
}

static void cache_refs(const cw__wg_cache *c, bool add) {
  for (int i = 0; i < c->n; i++) {
    const cw__wg_entry *x = &c->e[i];
    if (x->buffer) { if (add) wgpuBufferAddRef(x->buffer); else wgpuBufferRelease(x->buffer); }
    if (x->view) { if (add) wgpuTextureViewAddRef(x->view); else wgpuTextureViewRelease(x->view); }
    if (x->sampler) { if (add) wgpuSamplerAddRef(x->sampler); else wgpuSamplerRelease(x->sampler); }
  }
}

void cw__wg_cache_drop(cw__wg_cache *c) {
  if (c->g) wgpuBindGroupRelease(c->g);
  cache_refs(c, false);
  memset(c, 0, sizeof *c);
}

WGPUBindGroup cw__wg_group_cached(cw__wg_cache *c, WGPUBindGroupLayout l, const cw__wg_entry *e, int n) {
  if (n > 16) n = 16;
  bool same = c->g && c->l == l && c->n == n;
  for (int i = 0; same && i < n; i++) /* field by field: the struct has padding */
    same = c->e[i].buffer == e[i].buffer && c->e[i].offset == e[i].offset && c->e[i].size == e[i].size &&
           c->e[i].view == e[i].view && c->e[i].sampler == e[i].sampler;
  if (same) return c->g;
  cw__wg_cache_drop(c);
  c->g = cw__wg_group(l, e, n);
  c->l = l;
  c->n = n;
  for (int i = 0; i < n; i++) c->e[i] = e[i];
  cache_refs(c, true);
  return c->g;
}

WGPUShaderModule cw__wg_module(const char *label, const char *const *parts) {
  size_t n = strlen(CW_WGSL_COMMON) + 1;
  for (int i = 0; parts[i]; i++) n += strlen(parts[i]);
  char *s = cw_alloc(n, CW_MEM_RENDER);
  strcpy(s, CW_WGSL_COMMON);
  for (int i = 0; parts[i]; i++) strcat(s, parts[i]);
  WGPUShaderModule m = cw__wgpu_shader(label, s);
  cw_free(s);
  if (!m) cw__wgpu_fail("fx: shader module '%s' was not created", label);
  return m;
}

WGPUComputePipeline cw__wg_compute(const char *label, WGPUShaderModule m, const char *entry, WGPUPipelineLayout l) {
  WGPUComputePipelineDescriptor d = { .label = SV(label), .layout = l,
                                      .compute = { .module = m, .entryPoint = SV(entry) } };
  return wgpuDeviceCreateComputePipeline(cw__wgpu_device(), &d);
}

void cw__wg_dispatch(WGPUComputePassEncoder p, uint32_t threads) {
  uint32_t groups = (threads + 63) / 64;
  if (!groups) return;
  uint32_t x = groups < 32768 ? groups : 32768, y = (groups + 32767) / 32768;
  wgpuComputePassEncoderDispatchWorkgroups(p, x, y, 1);
}

const WGPUBlendState *cw__wg_blend(int preset) {
  static const WGPUBlendState add = { { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One },
                                      { WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One } };
  static const WGPUBlendState premul = {
    { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha },
    { WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One } };
  static const WGPUBlendState mul = { { WGPUBlendOperation_Add, WGPUBlendFactor_Dst, WGPUBlendFactor_Zero },
                                      { WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One } };
  static const WGPUBlendState add_all = { { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One },
                                          { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One } };
  switch (preset) {
  case CW_WG_BLEND_ADD: return &add;
  case CW_WG_BLEND_ADD_ALL: return &add_all;
  case CW_WG_BLEND_PREMUL: return &premul;
  case CW_WG_BLEND_MULTIPLY: return &mul;
  default: return NULL;
  }
}

WGPURenderPassEncoder cw__wg_scene_pass(const cw__wgpu_pass_targets *t, bool depth, const char *label) {
  WGPURenderPassColorAttachment ca = { .view = t->color, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                                       .resolveTarget = t->samples > 1 ? t->resolve : NULL,
                                       .loadOp = WGPULoadOp_Load, .storeOp = WGPUStoreOp_Store };
  WGPURenderPassDepthStencilAttachment da = { .view = t->depth, .depthLoadOp = WGPULoadOp_Load,
                                              .depthStoreOp = WGPUStoreOp_Store };
  bool stencil = t->depth_format == WGPUTextureFormat_Depth24PlusStencil8 ||
                 t->depth_format == WGPUTextureFormat_Depth32FloatStencil8;
  if (stencil) {
    da.stencilLoadOp = WGPULoadOp_Load;
    da.stencilStoreOp = WGPUStoreOp_Store;
  }
  WGPURenderPassDescriptor rp = { .label = SV(label), .colorAttachmentCount = 1, .colorAttachments = &ca,
                                  .depthStencilAttachment = depth && t->depth ? &da : NULL };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(t->enc, &rp);
  wgpuRenderPassEncoderSetViewport(p, 0, 0, (float)t->w, (float)t->h, 0, 1);
  return p;
}

/* ── shared textures ──────────────────────────────────────────────────── */

WGPUTextureView cw__fx_wg_noise3d(void) {
  static WGPUTextureView v;
  if (v) return v;
  enum { N = 32 };
  WGPUTexture t = cw__wg_texture(WGPUTextureDimension_3D, N, N, N, 1, WGPUTextureFormat_RGBA8Unorm,
                                 WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                 WGPUTextureViewDimension_Undefined, "fx curl noise");
  const uint8_t *px = cw__fx_noise3d_rgba();
  WGPUTexelCopyTextureInfo d = { .texture = t, .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout l = { .bytesPerRow = N * 4, .rowsPerImage = N };
  WGPUExtent3D e = { N, N, N };
  wgpuQueueWriteTexture(cw__wgpu_queue(), &d, px, (size_t)N * N * N * 4, &l, &e);
  v = cw__wg_view(t, WGPUTextureViewDimension_3D, 0, 1, 0, 1);
  wgpuTextureRelease(t);
  return v;
}

static int mip_count(int w, int h) {
  int n = 1;
  while (w > 1 || h > 1) {
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
    n++;
  }
  return n;
}

void cw__fx_wg_write_mips(WGPUTexture t, int w, int h, int mips, const uint8_t *px) {
  const uint8_t *src = px;
  uint8_t *owned = NULL;
  for (int l = 1; l < mips; l++) {
    int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    uint8_t *dst = cw_alloc((size_t)nw * nh * 4, CW_MEM_RENDER);
    for (int y = 0; y < nh; y++)
      for (int x = 0; x < nw; x++)
        for (int c = 0; c < 4; c++) {
          int x0 = CW_MIN(2 * x, w - 1), x1 = CW_MIN(2 * x + 1, w - 1), y0 = CW_MIN(2 * y, h - 1), y1 = CW_MIN(2 * y + 1, h - 1);
          int s = src[4 * (y0 * w + x0) + c] + src[4 * (y0 * w + x1) + c] + src[4 * (y1 * w + x0) + c] +
                  src[4 * (y1 * w + x1) + c];
          dst[4 * (y * nw + x) + c] = (uint8_t)((s + 2) / 4);
        }
    cw__wg_tex_write(t, l, 0, 0, 0, nw, nh, 4, dst);
    if (owned) cw_free(owned);
    owned = dst;
    src = dst;
    w = nw;
    h = nh;
  }
  if (owned) cw_free(owned);
}

WGPUTextureView cw__fx_wg_dot(void) {
  static WGPUTextureView v;
  if (v) return v;
  /* the GL dot has no mips (GL_LINEAR minification): the same here */
  WGPUTexture t = cw__wg_texture(WGPUTextureDimension_2D, 64, 64, 1, 1, WGPUTextureFormat_RGBA8Unorm,
                                 WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                 WGPUTextureViewDimension_Undefined, "fx dot");
  cw__wg_tex_write(t, 0, 0, 0, 0, 64, 64, 4, cw__fx_dot_rgba());
  v = cw__wg_view(t, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  wgpuTextureRelease(t);
  return v;
}

typedef struct wtex_entry { const char *path; WGPUTextureView v; } wtex_entry;
static CW_ARRAY(wtex_entry) wtex_cache;

WGPUTextureView cw__fx_wg_texture_asset(const char *path) {
  if (!path || !*path) return NULL;
  const char *ip = cw_intern(path);
  for (int i = 0; i < wtex_cache.count; i++)
    if (wtex_cache.data[i].path == ip) return wtex_cache.data[i].v;
  wtex_entry e = { ip, NULL };
  size_t len = 0;
  void *bytes = cw_asset_load(path, &len);
  if (!bytes) cw_error("particles: texture '%s' not found in the assets", path);
  else {
    int c = 0, w = 0, h = 0;
    uint8_t *px = stbi_load_from_memory(bytes, (int)len, &w, &h, &c, 4);
    free(bytes);
    if (!px) cw_error("particles: texture '%s' could not be decoded (PNG / JPEG expected)", path);
    else {
      int mips = mip_count(w, h);
      WGPUTexture t = cw__wg_texture(WGPUTextureDimension_2D, w, h, 1, mips, WGPUTextureFormat_RGBA8Unorm,
                                     WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                     WGPUTextureViewDimension_Undefined, path);
      cw__wg_tex_write(t, 0, 0, 0, 0, w, h, 4, px);
      cw__fx_wg_write_mips(t, w, h, mips, px);
      e.v = cw__wg_view(t, WGPUTextureViewDimension_2D, 0, mips, 0, 1);
      wgpuTextureRelease(t);
      stbi_image_free(px);
    }
  }
  cw_array_push(&wtex_cache, e);
  return e.v;
}

static WGPUTextureView dummy(WGPUTextureFormat f, int bpp, const char *label) {
  WGPUTexture t = cw__wg_texture(WGPUTextureDimension_2D, 1, 1, 1, 1, f,
                                 WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                 WGPUTextureViewDimension_Undefined, label);
  static const float zero[4] = { 0, 0, 0, 0 };
  cw__wg_tex_write(t, 0, 0, 0, 0, 1, 1, bpp, zero);
  WGPUTextureView v = cw__wg_view(t, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  wgpuTextureRelease(t);
  return v;
}
WGPUTextureView cw__fx_wg_dummy_r32f(void) {
  static WGPUTextureView v;
  if (!v) v = dummy(WGPUTextureFormat_R32Float, 4, "fx dummy r32f");
  return v;
}
WGPUTextureView cw__fx_wg_dummy_rgba32f(void) {
  static WGPUTextureView v;
  if (!v) v = dummy(WGPUTextureFormat_RGBA32Float, 16, "fx dummy rgba32f");
  return v;
}

/* ── asynchronous readback ────────────────────────────────────────────── */

enum { RB_FREE, RB_COPIED, RB_MAPPING, RB_DONE };
typedef struct rb_slot {
  WGPUBuffer buf;
  int state;
  uint64_t tag, order;
  struct cw__wg_readback *owner;
  uint8_t *data; /* fifo rings: the delivered bytes, until taken */
} rb_slot;

struct cw__wg_readback {
  size_t bytes;
  int n;
  rb_slot *slots;
  uint8_t *latest;
  uint64_t latest_tag, latest_order, next_order;
  bool have;
  uint32_t delivered;
  bool dead; /* freed while a map was pending: the last callback frees it */
  int pending;
  bool fifo;  /* every result is kept until taken (cw__wg_readback_take) */
};

static CW_ARRAY(rb_slot *) to_map;
static bool hooked;

static void rb_maybe_free(cw__wg_readback *r) {
  if (!r->dead || r->pending) return;
  for (int i = 0; i < r->n; i++) {
    wgpuBufferRelease(r->slots[i].buf);
    cw_free(r->slots[i].data);
  }
  cw_free(r->slots);
  cw_free(r->latest);
  cw_free(r);
}

static void on_map(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) {
  (void)msg, (void)u2;
  rb_slot *s = u1;
  cw__wg_readback *r = s->owner;
  r->pending--;
  if (st == WGPUMapAsyncStatus_Success && r->fifo && !r->dead) {
    const void *p = wgpuBufferGetConstMappedRange(s->buf, 0, r->bytes);
    if (p) {
      if (!s->data) s->data = cw_alloc(r->bytes, CW_MEM_RENDER);
      memcpy(s->data, p, r->bytes);
      r->delivered++;
    }
    wgpuBufferUnmap(s->buf);
    s->state = p ? RB_DONE : RB_FREE;
    rb_maybe_free(r);
    return;
  }
  if (st == WGPUMapAsyncStatus_Success) {
    if (!r->dead && s->order >= r->latest_order) {
      const void *p = wgpuBufferGetConstMappedRange(s->buf, 0, r->bytes);
      if (p) {
        memcpy(r->latest, p, r->bytes);
        r->latest_tag = s->tag;
        r->latest_order = s->order;
        r->have = true;
        r->delivered++;
      }
    }
    wgpuBufferUnmap(s->buf);
  }
  s->state = RB_FREE;
  rb_maybe_free(r);
}

void cw__wg_readback_flush(void) {
  for (int i = 0; i < to_map.count; i++) {
    rb_slot *s = to_map.data[i];
    s->state = RB_MAPPING;
    s->owner->pending++;
    WGPUBufferMapCallbackInfo ci = {
#if CW_CART
      .mode = WGPUCallbackMode_AllowSpontaneous,
#else
      .mode = WGPUCallbackMode_AllowProcessEvents,
#endif
      .callback = on_map, .userdata1 = s };
    wgpuBufferMapAsync(s->buf, WGPUMapMode_Read, 0, s->owner->bytes, ci);
  }
  cw_array_clear(&to_map);
}

cw__wg_readback *cw__wg_readback_new(size_t bytes, int slots, const char *label) {
  if (!hooked) {
    cw__wgpu_add_frame_end_hook(cw__wg_readback_flush);
    hooked = true;
  }
  cw__wg_readback *r = cw_calloc(1, sizeof *r, CW_MEM_RENDER);
  r->bytes = (bytes + 3) & ~(size_t)3;
  r->n = slots < 1 ? 1 : slots;
  r->slots = cw_calloc((size_t)r->n, sizeof *r->slots, CW_MEM_RENDER);
  r->latest = cw_calloc(1, r->bytes, CW_MEM_RENDER);
  for (int i = 0; i < r->n; i++) {
    r->slots[i].buf = cw__wg_buffer(r->bytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, label);
    r->slots[i].owner = r;
  }
  return r;
}

void cw__wg_readback_free(cw__wg_readback *r) {
  if (!r) return;
  for (int i = 0; i < to_map.count;)
    if (to_map.data[i]->owner == r) {
      to_map.data[i]->state = RB_FREE;
      to_map.data[i] = to_map.data[--to_map.count];
    } else i++;
  r->dead = true;
  rb_maybe_free(r);
}

static rb_slot *free_slot(cw__wg_readback *r) {
  for (int i = 0; i < r->n; i++)
    if (r->slots[i].state == RB_FREE) return &r->slots[i];
  return NULL;
}

bool cw__wg_readback_request(cw__wg_readback *r, WGPUCommandEncoder enc, WGPUBuffer src, uint64_t src_offset,
                             uint64_t tag) {
  rb_slot *s = free_slot(r);
  if (!s) return false;
  wgpuCommandEncoderCopyBufferToBuffer(enc, src, src_offset, s->buf, 0, r->bytes);
  s->state = RB_COPIED;
  s->tag = tag;
  s->order = ++r->next_order;
  cw_array_push(&to_map, s);
  return true;
}

bool cw__wg_readback_request_tex(cw__wg_readback *r, WGPUCommandEncoder enc, WGPUTexture src, int mip, int x, int y,
                                 int layer, int w, int h, int layers, int bpp, uint64_t tag) {
  uint32_t row = ((uint32_t)(w * bpp) + 255) & ~255u;
  if (layers < 1) layers = 1;
  if ((size_t)row * (size_t)h * (size_t)layers > r->bytes) {
    cw__wgpu_fail("fx readback: %dx%d texels need %u bytes, the ring holds %zu", w, h, row * (uint32_t)h, r->bytes);
    return false;
  }
  rb_slot *s = free_slot(r);
  if (!s) return false;
  WGPUTexelCopyTextureInfo st = { .texture = src, .mipLevel = (uint32_t)mip,
                                  .origin = { (uint32_t)x, (uint32_t)y, (uint32_t)layer }, .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferInfo dt = { .layout = { .bytesPerRow = row, .rowsPerImage = (uint32_t)h }, .buffer = s->buf };
  WGPUExtent3D e = { (uint32_t)w, (uint32_t)h, (uint32_t)layers };
  wgpuCommandEncoderCopyTextureToBuffer(enc, &st, &dt, &e);
  s->state = RB_COPIED;
  s->tag = tag;
  s->order = ++r->next_order;
  cw_array_push(&to_map, s);
  return true;
}

const void *cw__wg_readback_latest(cw__wg_readback *r, uint64_t *tag) {
  if (!r || !r->have) return NULL;
  if (tag) *tag = r->latest_tag;
  return r->latest;
}

float cw__wg_half(uint16_t h) {
  uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, bits;
  if (e == 0) {
    if (!m) bits = s;
    else {
      e = 113;
      while (!(m & 1024)) {
        m <<= 1;
        e--;
      }
      bits = s | (e << 23) | ((m & 1023) << 13);
    }
  } else if (e == 31) bits = s | 0x7f800000u | (m << 13);
  else bits = s | ((e + 112) << 23) | (m << 13);
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

uint32_t cw__wg_readback_count(const cw__wg_readback *r) { return r ? r->delivered : 0; }

cw__wg_readback *cw__wg_readback_new_fifo(size_t bytes, int slots, const char *label) {
  cw__wg_readback *r = cw__wg_readback_new(bytes, slots, label);
  r->fifo = true;
  return r;
}

bool cw__wg_readback_take(cw__wg_readback *r, void *out, uint64_t *tag) {
  if (!r) return false;
  rb_slot *best = NULL;
  for (int i = 0; i < r->n; i++)
    if (r->slots[i].state == RB_DONE && (!best || r->slots[i].order < best->order)) best = &r->slots[i];
  if (!best) return false;
  memcpy(out, best->data, r->bytes);
  if (tag) *tag = best->tag;
  best->state = RB_FREE;
  return true;
}

#endif /* CW_WGPU */
