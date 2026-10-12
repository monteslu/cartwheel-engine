/* Effects on WebGPU (webgpu.h, WGSL): the helpers every WebGPU effect shares
 * (fx_wgpu.c) and the pass targets an effect draws into.
 *
 * Only compiled into WebGPU builds (CW_WGPU); every caller sits behind
 * cw__wgpu_on(). WebGPU rules (cartwheel/gpu.h): compatibility feature level,
 * main thread only, nothing waits for the GPU (readbacks are asynchronous:
 * copies recorded with the frame's work, mapped once the frame has been
 * submitted, results delivered between frames). */
#ifndef CW_FX_WGPU_H
#define CW_FX_WGPU_H

#include "cartwheel/pipeline.h"

#if CW_WGPU
#include "platform/wgpu.h"
#include "render/pipeline_wgpu.h"

/* ── what a pass draws into ──────────────────────────────────────────── */

/* cw__wgpu_pass_targets and cw__pass_wgpu_targets: the pipeline core's
 * (render/pipeline_wgpu.h): the frame encoder, the scene colour and depth
 * attachments, the R32F linear depth (SKY / EFFECTS / POST), flip_y, MSAA. */
/* what the effects use: the test targets when set, else the pipeline's; NULL
 * when there are none for this pass (logged once by the caller) */
const cw__wgpu_pass_targets *cw__fx_pass_targets(cw_pass_ctx *ctx);
/* Tests (and anything driving fx passes without the pipeline core): the
 * targets every pass gets until cleared with NULL. */
void cw__fx_wgpu_set_test_targets(const cw__wgpu_pass_targets *t);

/* ── resources ───────────────────────────────────────────────────────── */

WGPUBuffer cw__wg_buffer(size_t bytes, WGPUBufferUsage usage, const char *label);
/* queue write, padded to a multiple of 4 bytes */
void cw__wg_write(WGPUBuffer b, size_t offset, const void *data, size_t bytes);
/* a texture: dim 2D (layers = array layers, 6 for a cube) or 3D (layers =
 * depth); bind_dim is the compatibility-mode binding view dimension
 * (Undefined = the default for the texture) */
WGPUTexture cw__wg_texture(WGPUTextureDimension dim, int w, int h, int layers, int mips, WGPUTextureFormat f,
                           WGPUTextureUsage usage, WGPUTextureViewDimension bind_dim, const char *label);
WGPUTextureView cw__wg_view(WGPUTexture t, WGPUTextureViewDimension dim, int base_mip, int mips, int base_layer,
                            int layers);
/* write w x h texels at (x, y, layer) of mip level `mip`; bpp bytes a texel */
void cw__wg_tex_write(WGPUTexture t, int mip, int x, int y, int layer, int w, int h, int bpp, const void *px);
/* linear: bilinear (+ linear mips when mips); repeat: wrap in u, v and w */
WGPUSampler cw__wg_sampler(bool linear, bool repeat, bool mips);

/* bind group layout slots (binding = index) */
enum {
  CW_WB_UNIFORM,      /* uniform buffer */
  CW_WB_UNIFORM_DYN,  /* uniform buffer, dynamic offset */
  CW_WB_STORAGE,      /* read-write storage (compute only) */
  CW_WB_STORAGE_RO,   /* read-only storage (compute only) */
  CW_WB_TEX,          /* filterable float texture */
  CW_WB_TEX_UNFILT,   /* unfilterable float texture (RGBA32F / R32F, textureLoad) */
  CW_WB_SAMPLER,      /* filtering sampler */
  CW_WB_SAMPLER_NF,   /* non-filtering sampler */
};
typedef struct cw__wg_slot {
  int type;
  WGPUShaderStage vis;
  WGPUTextureViewDimension dim; /* textures: Undefined = 2D */
  uint64_t min_size;            /* buffers: minBindingSize (0 = any) */
} cw__wg_slot;
WGPUBindGroupLayout cw__wg_layout(const cw__wg_slot *s, int n);
WGPUPipelineLayout cw__wg_pipeline_layout(const WGPUBindGroupLayout *l, int n);
/* entries: binding = index; set exactly one of buffer (+size, 0 = whole),
 * view, sampler per entry */
typedef struct cw__wg_entry {
  WGPUBuffer buffer;
  uint64_t offset, size;
  WGPUTextureView view;
  WGPUSampler sampler;
} cw__wg_entry;
WGPUBindGroup cw__wg_group(WGPUBindGroupLayout l, const cw__wg_entry *e, int n);
/* a bind group that is the same frame after frame: kept while the layout and
 * entries match, remade when they change. Its resources are referenced while
 * cached, so a freed object's address can never match a new one. The group
 * is borrowed (never released by the caller); cw__wg_cache_drop frees it. */
typedef struct cw__wg_cache {
  WGPUBindGroupLayout l;
  cw__wg_entry e[16];
  int n;
  WGPUBindGroup g;
} cw__wg_cache;
WGPUBindGroup cw__wg_group_cached(cw__wg_cache *c, WGPUBindGroupLayout l, const cw__wg_entry *e, int n);
void cw__wg_cache_drop(cw__wg_cache *c);

/* WGSL from parts (NULL-terminated), compiled; NULL (logged, counted) on
 * failure. Every module gets CW_WGSL_COMMON first: the particle hash
 * functions and the GL-to-WebGPU clip helpers. */
WGPUShaderModule cw__wg_module(const char *label, const char *const *parts);
WGPUComputePipeline cw__wg_compute(const char *label, WGPUShaderModule m, const char *entry, WGPUPipelineLayout l);
/* Dispatch `threads` invocations of a 64-wide workgroup: x up to 32768
 * groups, y beyond (shaders index with CW_WG_INDEX). */
void cw__wg_dispatch(WGPUComputePassEncoder p, uint32_t threads);

/* blend presets for render pipelines */
enum { CW_WG_BLEND_NONE, CW_WG_BLEND_ADD, CW_WG_BLEND_PREMUL, CW_WG_BLEND_MULTIPLY };
const WGPUBlendState *cw__wg_blend(int preset);

/* begin a render pass on the targets (load + store; with the depth
 * attachment when depth is true); the viewport covers the targets */
WGPURenderPassEncoder cw__wg_scene_pass(const cw__wgpu_pass_targets *t, bool depth, const char *label);

/* ── shared resources ────────────────────────────────────────────────── */

/* the curl-noise volume (cw__fx_noise3d_rgba, RGBA8 32^3) */
WGPUTextureView cw__fx_wg_noise3d(void);
/* the 64x64 soft dot (RGBA8, with mips) */
WGPUTextureView cw__fx_wg_dot(void);
/* an image asset as an RGBA8 texture with mips (cached by path); NULL when
 * missing (logged once) */
WGPUTextureView cw__fx_wg_texture_asset(const char *path);
/* a 1x1 R32Float texture (0) and a 1x1 RGBA32Float one: bindings that are
 * not used this draw */
WGPUTextureView cw__fx_wg_dummy_r32f(void);
WGPUTextureView cw__fx_wg_dummy_rgba32f(void);
/* RGBA8 mips by 2x2 box filtering, level by level (glGenerateMipmap's
 * filter), for w x h texels at px; writes levels 1 .. mips-1 of t */
void cw__fx_wg_write_mips(WGPUTexture t, int w, int h, int mips, const uint8_t *px);

/* ── asynchronous readback ───────────────────────────────────────────── */

/* A ring of mappable buffers: request() records a copy of `bytes` from src
 * into a free slot on enc (false when every slot is busy: the reader is
 * behind), and the slot is mapped after the frame's submit; the callback
 * copies the data out. latest() returns the newest delivered result (and its
 * tag), NULL before the first. Results arrive one frame late natively and
 * between frames in carts, never inside a frame. */
typedef struct cw__wg_readback cw__wg_readback;
cw__wg_readback *cw__wg_readback_new(size_t bytes, int slots, const char *label);
void cw__wg_readback_free(cw__wg_readback *r);
bool cw__wg_readback_request(cw__wg_readback *r, WGPUCommandEncoder enc, WGPUBuffer src, uint64_t src_offset,
                             uint64_t tag);
/* the same from a texture region of `layers` array layers from `layer`
 * (rows padded to 256 bytes, layer after layer) */
bool cw__wg_readback_request_tex(cw__wg_readback *r, WGPUCommandEncoder enc, WGPUTexture src, int mip, int x, int y,
                                 int layer, int w, int h, int layers, int bpp, uint64_t tag);
/* IEEE half to float (readbacks of RGBA16F) */
float cw__wg_half(uint16_t h);
const void *cw__wg_readback_latest(cw__wg_readback *r, uint64_t *tag);
/* A ring whose results are all kept, in request order, until taken: a slot
 * is busy from its request until its result is taken. take() copies the
 * oldest delivered result (bytes) to out; false when none is waiting. */
cw__wg_readback *cw__wg_readback_new_fifo(size_t bytes, int slots, const char *label);
bool cw__wg_readback_take(cw__wg_readback *r, void *out, uint64_t *tag);
/* results delivered so far / requests made */
uint32_t cw__wg_readback_count(const cw__wg_readback *r);
/* map everything requested this frame: called by the device module at the
 * end of every frame (after the frame's work is submitted) and by tests that
 * submit their own encoders */
void cw__wg_readback_flush(void);

/* the WGSL every fx module starts with */
extern const char CW_WGSL_COMMON[];

#endif /* CW_WGPU */
#endif
