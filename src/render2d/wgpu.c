/* render2d on WebGPU (webgpu.h, WGSL). The submission stream, the stable
 * sort and the batching are the GL path's (batch.c); this file turns the
 * same batches into render passes:
 *
 *   canvas pass   the pass target (window frame, virtual canvas, or a
 *                 cw_target), cleared or loaded
 *   lit runs      scene (+ normal) pass, light pass (depth-stencil for
 *                 shadow volumes), then the canvas pass again with the
 *                 composite
 *   blit          a virtual canvas onto the window (letterboxed)
 *
 * Every cw_r2d_end records one command encoder and submits it, after
 * writing the pass's vertices, indices, uniforms and scratch geometry with
 * wgpuQueueWriteBuffer (queue order makes the writes land before the draws).
 *
 * Orientation: GL's offscreen targets store rows bottom-up and the CPU side
 * (UVs of render targets, the light shader's window coordinates) relies on
 * it. WebGPU's framebuffer y points down, so passes into OFFSCREEN targets
 * flip y in the vertex stage (flip = -1): the texture's memory then matches
 * GL's row for row, gl_FragCoord.y == @builtin(position).y, and every UV the
 * shared code computes is right. Window passes do not flip (flip = +1), and
 * the composite, which reads an offscreen target at the fragment position,
 * mirrors the row when it writes the window.
 *
 * Compatibility mode (the wasmcart tier's floor): array textures declare
 * their binding view dimension (2D array, even with one layer), render
 * attachments use 2D single-layer views, flat varyings use `either`,
 * identical blend on both attachments of the lit pass, depthClearValue set. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "cartwheel/platform.h"

#if !CW_WGPU
bool r2d_wg(void) { return false; }
bool r2d_wg_init(void) { return false; }
void r2d_wg_shutdown(void) {}
int r2d_wg_max_layers(void) { return 1; }
void *r2d_wg_tex_new(int w, int h, int layers) { (void)w, (void)h, (void)layers; return NULL; }
void *r2d_wg_depth_new(int w, int h) { (void)w, (void)h; return NULL; }
void r2d_wg_tex_fill(void *tex, int from, int to, const uint8_t rgba[4]) { (void)tex, (void)from, (void)to, (void)rgba; }
void r2d_wg_tex_copy(void *src, void *dst, int layers) { (void)src, (void)dst, (void)layers; }
void r2d_wg_tex_write(void *tex, int x, int y, int layer, int w, int h, const uint8_t *px) {
  (void)tex, (void)x, (void)y, (void)layer, (void)w, (void)h, (void)px;
}
void r2d_wg_tex_release(void *tex) { (void)tex; }
void r2d_wg_array_changed(r2d_array *a) { (void)a; }
void r2d_wg_end(const r2d_batch *b, int n, const uint32_t *idx) { (void)b, (void)n, (void)idx; }
void *r2d_wg_buffer(void *old, const void *data, size_t bytes, bool index) { (void)old, (void)data, (void)bytes, (void)index; return NULL; }
void r2d_wg_buffer_release(void *buf) { (void)buf; }
void r2d_wg_tilemap_begin(const float u[24], int array, bool lit) { (void)u, (void)array, (void)lit; }
void r2d_wg_tilemap_chunk(void *vbuf, int quads) { (void)vbuf, (void)quads; }
#else

#include "platform/wgpu.h"

#define SV CW_WGPU_SV
#define UALIGN 256u

/* ── WGSL ──────────────────────────────────────────────────────────────── */

static const char *WGSL_COMMON =
    "struct U { layers: array<vec4f, 64>, target_size: vec2f, flip: f32, pad: f32 };\n"
    "struct Q { target_size: vec2f, flip: f32, mode: f32, a: vec4f, b: vec4f, c: vec4f, d: vec4f, e: vec4f, f: vec4f };\n"
    "@group(1) @binding(0) var t_tex: texture_2d_array<f32>;\n"
    "@group(1) @binding(1) var t_smp: sampler;\n"
    "@group(1) @binding(2) var t_nrm: texture_2d_array<f32>;\n"
    "fn ndc(p: vec2f, t: vec2f, flip: f32) -> vec4f {\n"
    "  return vec4f(p.x / t.x * 2.0 - 1.0, (1.0 - p.y / t.y * 2.0) * flip, 0.0, 1.0);\n"
    "}\n"
    "fn gmod(x: f32, y: f32) -> f32 { return x - y * floor(x / y); }\n";

/* the stream: sprites, shapes, text, geometry (GL: gl.c SPRITE_VS / FS) */
static const char *WGSL_SPRITE =
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) color: vec4f,\n"
    "  @location(2) @interpolate(flat, either) page: u32, @location(3) @interpolate(flat, either) mode: u32,\n"
    "  @location(4) @interpolate(flat, either) sdfw: f32, @location(5) @interpolate(flat, either) sdf: vec2f };\n"
    "@vertex fn vs(@location(0) a_pos: vec2f, @location(1) a_uv: vec2f, @location(2) a_color: vec4f,\n"
    "              @location(3) a_info: vec4u, @location(4) a_sdf: vec4f) -> VO {\n"
    "  let L = a_info.z * 2u;\n"
    "  let r0 = u.layers[L]; let r1 = u.layers[L + 1u];\n"
    "  var p = vec2f(r0.x * a_pos.x + r0.y * a_pos.y + r0.z, r1.x * a_pos.x + r1.y * a_pos.y + r1.z);\n"
    "  if (r0.w > 0.5) { p = floor(p + 0.5); }\n"
    "  var o: VO;\n"
    "  o.pos = ndc(p, u.target_size, u.flip);\n"
    "  o.uv = a_uv;\n"
    "  o.color = vec4f(a_color.rgb * a_color.a, a_color.a);\n"
    "  o.page = a_info.x;\n"
    "  o.mode = a_info.y;\n"
    "  o.sdfw = 0.6 * f32(a_info.w) / 255.0 / max(sqrt(abs(r0.x * r1.y - r0.y * r1.x)), 1e-4);\n"
    "  o.sdf = a_sdf.xy;\n"
    "  return o;\n"
    "}\n"
    "fn shade(i: VO, t: vec4f) -> vec4f {\n"
    "  var c: vec4f;\n"
    "  if ((i.mode & 1u) != 0u) { c = i.color; }\n"
    "  else if ((i.mode & 2u) != 0u) {\n"
    "    let w = clamp(i.sdfw, 0.004, 0.5) + i.sdf.y; let e = 0.5 - i.sdf.x;\n"
    "    var lo = e - w;\n"
    "    if (i.sdf.x + i.sdf.y > 0.0) { lo = max(lo, 0.004); }\n"
    "    c = i.color * smoothstep(lo, e + w, t.a);\n"
    "  } else { c = t * i.color; }\n"
    "  return c;\n"
    "}\n"
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  var c = shade(i, textureSample(t_tex, t_smp, i.uv, i.page));\n"
    "  if ((i.mode & 4u) != 0u) { c.a = 0.0; }\n"
    "  return c;\n"
    "}\n"
    "struct LitOut { @location(0) col: vec4f, @location(1) nrm: vec4f };\n"
    "@fragment fn fs_lit(i: VO) -> LitOut {\n"
    "  var c = shade(i, textureSample(t_tex, t_smp, i.uv, i.page));\n"
    "  let n = textureSample(t_nrm, t_smp, i.uv, i.page);\n"
    "  var enc = n.rgb;\n"
    "  if ((i.mode & 3u) != 0u) { enc = vec3f(0.5, 0.5, 0.0); }\n"
    "  var o: LitOut;\n"
    "  o.nrm = vec4f(enc, 1.0) * c.a;\n"
    "  if ((i.mode & 4u) != 0u) { o.nrm = vec4f(0.0); c.a = 0.0; }\n"
    "  o.col = c;\n"
    "  return o;\n"
    "}\n";

/* tilemap chunks (GL: TILE_VS / FS): q.a = m0, b = m1, c = set0, d = set1,
 * e = (inv_page, time, 0), f = tint */
static const char *WGSL_TILE =
    "@group(0) @binding(0) var<uniform> q: Q;\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) @interpolate(flat, either) page: u32 };\n"
    "@vertex fn vs(@location(0) a_pos: vec2f, @location(1) a_tile: vec4u) -> VO {\n"
    "  var p = vec2f(q.a.x * a_pos.x + q.a.y * a_pos.y + q.a.z, q.b.x * a_pos.x + q.b.y * a_pos.y + q.b.z);\n"
    "  if (q.a.w > 0.5) { p = floor(p + 0.5); }\n"
    "  var o: VO;\n"
    "  o.pos = ndc(p, q.target_size, q.flip);\n"
    "  var id = f32(a_tile.x);\n"
    "  let frames = f32(a_tile.z);\n"
    "  if (frames > 1.0) { id += gmod(floor(q.e.z * f32(a_tile.w) / 16.0), frames); }\n"
    "  let col = gmod(id, q.d.z); let row = floor(id / q.d.z);\n"
    "  let corner = vec2f(gmod(f32(a_tile.y), 2.0), floor(f32(a_tile.y) / 2.0));\n"
    "  let inset = mix(vec2f(0.02), q.c.zw - 0.02, corner);\n"
    "  o.uv = (q.c.xy + vec2f(col, row) * q.d.xy + inset) * q.e.xy;\n"
    "  o.page = u32(q.d.w);\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs(i: VO) -> @location(0) vec4f {\n"
    "  return textureSample(t_tex, t_smp, i.uv, i.page) * vec4f(q.f.rgb * q.f.a, q.f.a);\n"
    "}\n"
    "struct LitOut { @location(0) col: vec4f, @location(1) nrm: vec4f };\n"
    "@fragment fn fs_lit(i: VO) -> LitOut {\n"
    "  let c = textureSample(t_tex, t_smp, i.uv, i.page) * vec4f(q.f.rgb * q.f.a, q.f.a);\n"
    "  let n = textureSample(t_nrm, t_smp, i.uv, i.page);\n"
    "  var o: LitOut;\n"
    "  o.col = c;\n"
    "  o.nrm = vec4f(n.rgb, 1.0) * c.a;\n"
    "  return o;\n"
    "}\n";

/* scratch quads in target pixels: lights, shadows, composite, blit
 * (GL: QUAD_VS and the four fragment shaders) */
static const char *WGSL_QUAD =
    "@group(0) @binding(0) var<uniform> q: Q;\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };\n"
    "@vertex fn vs(@location(0) a_pos: vec2f, @location(1) a_uv: vec2f) -> VO {\n"
    "  var o: VO;\n"
    "  o.pos = ndc(a_pos, q.target_size, q.flip);\n"
    "  o.uv = a_uv;\n"
    "  return o;\n"
    "}\n"
    /* one point light (stores light / 2): a = centre (GL window coords),
     * b = colour * energy / 2 + radius, c = falloff, height, use normals */
    "@fragment fn fs_light(i: VO) -> @location(0) vec4f {\n"
    "  let d = i.pos.xy - q.a.xy;\n"
    "  let att = pow(clamp(1.0 - length(d) / q.b.w, 0.0, 1.0), q.c.x);\n"
    "  var ndl = 1.0;\n"
    "  if (q.c.z > 0.5) {\n"
    "    let n = textureLoad(t_nrm, vec2i(i.pos.xy), 0, 0);\n"
    "    if (n.a > 0.004) {\n"
    "      let e = n.rgb / n.a;\n"
    "      let mapped = smoothstep(0.0, 0.5, e.z);\n"
    "      let N = normalize(e * 2.0 - 1.0);\n"
    "      let Ld = normalize(vec3f(-d, q.c.y));\n"
    "      ndl = mix(1.0, max(dot(N, Ld), 0.0), mapped);\n"
    "    }\n"
    "  }\n"
    "  return vec4f(q.b.rgb * att * ndl, 0.0);\n"
    "}\n"
    "@fragment fn fs_shadow(i: VO) -> @location(0) vec4f { return vec4f(0.0); }\n"
    /* colour * light into the canvas; mode 1: the target is the window (rows
     * top-down) while the sources are offscreen (rows bottom-up) */
    "@fragment fn fs_composite(i: VO) -> @location(0) vec4f {\n"
    "  var p = vec2i(i.pos.xy);\n"
    "  if (q.mode > 0.5) { p.y = i32(q.target_size.y) - 1 - p.y; }\n"
    "  let s = textureLoad(t_tex, p, 0, 0);\n"
    "  let l = textureLoad(t_nrm, p, 0, 0).rgb * 2.0;\n"
    "  return vec4f(s.rgb * l, s.a);\n"
    "}\n"
    "@fragment fn fs_blit(i: VO) -> @location(0) vec4f { return textureSample(t_tex, t_smp, i.uv, u32(q.a.x)); }\n";

/* ── state ─────────────────────────────────────────────────────────────── */

enum { P_SPRITE, P_SPRITE_LIT, P_TILE, P_TILE_LIT, P_LIGHT, P_LIGHT_ST, P_SHADOW, P_COMPOSITE, P_BLIT, P_COUNT };
enum { B_OPAQUE, B_ALPHA, B_MULTIPLY, B_ADD };
#define U_STREAM_SIZE 1040u
#define U_SMALL_SIZE 112u

typedef struct pipe_rec {
  uint32_t key;
  WGPURenderPipeline p;
} pipe_rec;

static struct {
  bool inited;
  WGPUDevice dev;
  WGPUQueue q;
  WGPUShaderModule sm_sprite, sm_tile, sm_quad;
  WGPUBindGroupLayout bgl_stream, bgl_small, bgl_tex;
  WGPUPipelineLayout pl_stream, pl_small, pl_shadow;
  WGPUSampler samplers[4]; /* nearest | repeat << 1 */
  WGPUTexture dummy;       /* 1x1x1 "no normal" array (also the solid draws' colour texture) */
  WGPUTextureView dummy_view;
  WGPUBuffer vbuf, ibuf, ubuf, sbuf, tile_ibuf;
  size_t vcap, icap, ucap, scap;
  CW_ARRAY(pipe_rec) pipes;
  /* recording */
  WGPUCommandEncoder enc;
  WGPURenderPassEncoder pass;
  WGPUTextureFormat pass_fmt;
  bool pass_ds;
  float pass_flip;
  int pass_w, pass_h;
  CW_ARRAY(uint8_t) ustage; /* uniforms for this end, UALIGN-aligned blocks */
  CW_ARRAY(float) sstage;   /* scratch quads (x, y, u, v) */
  WGPUBindGroup bg_stream, bg_small; /* over ubuf (remade when it grows) */
  CW_ARRAY(WGPUBindGroup) transient; /* released after the submit */
  uint32_t stream_off[2];            /* stream uniform blocks: [0] flip -1, [1] flip +1 */
} W;

bool r2d_wg(void) { return cw__wgpu_on(); }
int r2d_wg_max_layers(void) { return (int)cw__wgpu_limits()->maxTextureArrayLayers; }

/* ── textures ──────────────────────────────────────────────────────────── */

void *r2d_wg_tex_new(int w, int h, int layers) {
  WGPUTextureBindingViewDimension vd = { .chain = { .sType = WGPUSType_TextureBindingViewDimension },
                                         .textureBindingViewDimension = WGPUTextureViewDimension_2DArray };
  WGPUTextureDescriptor d = { .nextInChain = &vd.chain, .label = SV("r2d array"),
                              .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc |
                                       WGPUTextureUsage_RenderAttachment,
                              .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, (uint32_t)layers },
                              .format = WGPUTextureFormat_RGBA8Unorm, .mipLevelCount = 1, .sampleCount = 1 };
  return wgpuDeviceCreateTexture(W.dev, &d);
}

void *r2d_wg_depth_new(int w, int h) {
  WGPUTextureDescriptor d = { .label = SV("r2d depth-stencil"), .usage = WGPUTextureUsage_RenderAttachment,
                              .dimension = WGPUTextureDimension_2D, .size = { (uint32_t)w, (uint32_t)h, 1 },
                              .format = WGPUTextureFormat_Depth24PlusStencil8, .mipLevelCount = 1, .sampleCount = 1 };
  return wgpuDeviceCreateTexture(W.dev, &d);
}

void r2d_wg_tex_release(void *tex) {
  if (tex) wgpuTextureRelease((WGPUTexture)tex);
}

/* a render-attachment view of one layer */
static WGPUTextureView layer_view(WGPUTexture t, int layer) {
  WGPUTextureViewDescriptor d = { .format = wgpuTextureGetFormat(t), .dimension = WGPUTextureViewDimension_2D,
                                  .mipLevelCount = 1, .baseArrayLayer = (uint32_t)layer, .arrayLayerCount = 1,
                                  .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_RenderAttachment };
  return wgpuTextureCreateView(t, &d);
}
static WGPUTextureView array_view(WGPUTexture t) {
  WGPUTextureViewDescriptor d = { .format = WGPUTextureFormat_RGBA8Unorm, .dimension = WGPUTextureViewDimension_2DArray,
                                  .mipLevelCount = 1, .arrayLayerCount = wgpuTextureGetDepthOrArrayLayers(t),
                                  .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_TextureBinding };
  return wgpuTextureCreateView(t, &d);
}

static void submit(WGPUCommandEncoder enc) {
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(W.q, 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
}

void r2d_wg_tex_fill(void *tex, int from, int to, const uint8_t rgba[4]) {
  if (!tex || from >= to) return;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(W.dev, NULL);
  for (int l = from; l < to; l++) {
    WGPUTextureView v = layer_view((WGPUTexture)tex, l);
    WGPURenderPassColorAttachment ca = { .view = v, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
                                         .storeOp = WGPUStoreOp_Store,
                                         .clearValue = { rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0, rgba[3] / 255.0 } };
    WGPURenderPassDescriptor rp = { .colorAttachmentCount = 1, .colorAttachments = &ca };
    WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderEnd(p);
    wgpuRenderPassEncoderRelease(p);
    wgpuTextureViewRelease(v);
  }
  submit(enc);
}

void r2d_wg_tex_copy(void *src, void *dst, int layers) {
  if (!src || !dst || layers <= 0) return;
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(W.dev, NULL);
  WGPUTexelCopyTextureInfo s = { .texture = src, .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyTextureInfo d = { .texture = dst, .aspect = WGPUTextureAspect_All };
  WGPUExtent3D e = { wgpuTextureGetWidth(src), wgpuTextureGetHeight(src), (uint32_t)layers };
  wgpuCommandEncoderCopyTextureToTexture(enc, &s, &d, &e);
  submit(enc);
}

void r2d_wg_tex_write(void *tex, int x, int y, int layer, int w, int h, const uint8_t *px) {
  WGPUTexelCopyTextureInfo d = { .texture = tex, .origin = { (uint32_t)x, (uint32_t)y, (uint32_t)layer },
                                 .aspect = WGPUTextureAspect_All };
  WGPUTexelCopyBufferLayout l = { .bytesPerRow = (uint32_t)w * 4, .rowsPerImage = (uint32_t)h };
  WGPUExtent3D e = { (uint32_t)w, (uint32_t)h, 1 };
  wgpuQueueWriteTexture(W.q, &d, px, (size_t)w * (size_t)h * 4, &l, &e);
}

void r2d_wg_array_changed(r2d_array *a) {
  if (a->wbg) wgpuBindGroupRelease((WGPUBindGroup)a->wbg);
  a->wbg = NULL;
}

static WGPUBindGroup tex_group(WGPUTextureView a, WGPUSampler s, WGPUTextureView b) {
  WGPUBindGroupEntry e[3] = { { .binding = 0, .textureView = a }, { .binding = 1, .sampler = s },
                              { .binding = 2, .textureView = b } };
  WGPUBindGroupDescriptor d = { .layout = W.bgl_tex, .entryCount = 3, .entries = e };
  return wgpuDeviceCreateBindGroup(W.dev, &d);
}

static WGPUSampler sampler_for(bool nearest, bool repeat) { return W.samplers[(nearest ? 1 : 0) | (repeat ? 2 : 0)]; }

/* (colour, sampler, normal-or-dummy) of an array, cached on it */
static WGPUBindGroup array_group(int ai) {
  if (ai < 0 || ai >= R.arrays.count || ai == (int)R2D_NO_ARRAY) return NULL;
  r2d_array *a = &R.arrays.data[ai];
  if (!a->wbg) {
    WGPUTextureView cv = array_view(a->wtex);
    WGPUTextureView nv = a->wntex ? array_view(a->wntex) : W.dummy_view;
    a->wbg = tex_group(cv, sampler_for(a->nearest, a->repeat), nv);
    wgpuTextureViewRelease(cv);
    if (a->wntex) wgpuTextureViewRelease(nv);
  }
  return (WGPUBindGroup)a->wbg;
}
static WGPUBindGroup dummy_group;

/* ── buffers ───────────────────────────────────────────────────────────── */

static WGPUBuffer make_buffer(size_t bytes, WGPUBufferUsage usage, const char *label) {
  WGPUBufferDescriptor d = { .label = SV(label), .usage = usage | WGPUBufferUsage_CopyDst, .size = (bytes + 3) & ~(size_t)3 };
  return wgpuDeviceCreateBuffer(W.dev, &d);
}

static void ensure(WGPUBuffer *b, size_t *cap, size_t need, WGPUBufferUsage usage, const char *label) {
  if (*b && *cap >= need) return;
  size_t n = *cap ? *cap : 4096;
  while (n < need) n *= 2;
  if (*b) wgpuBufferRelease(*b);
  *b = make_buffer(n, usage, label);
  *cap = n;
}

static void write_padded(WGPUBuffer b, const void *data, size_t bytes) {
  if (!bytes) return;
  if (bytes & 3) { /* writeBuffer sizes are multiples of 4 */
    uint8_t *tmp = cw_frame_alloc(bytes + 4);
    memcpy(tmp, data, bytes);
    memset(tmp + bytes, 0, 4);
    wgpuQueueWriteBuffer(W.q, b, 0, tmp, (bytes + 3) & ~(size_t)3);
  } else {
    wgpuQueueWriteBuffer(W.q, b, 0, data, bytes);
  }
}

void *r2d_wg_buffer(void *old, const void *data, size_t bytes, bool index) {
  WGPUBuffer b = old;
  size_t have = b ? (size_t)wgpuBufferGetSize(b) : 0;
  if (!b || have < bytes) {
    if (b) wgpuBufferRelease(b);
    b = make_buffer(bytes ? bytes : 16, index ? WGPUBufferUsage_Index : WGPUBufferUsage_Vertex, "r2d chunk");
  }
  write_padded(b, data, bytes);
  return b;
}

void r2d_wg_buffer_release(void *buf) {
  if (buf) wgpuBufferRelease((WGPUBuffer)buf);
}

/* ── pipelines ─────────────────────────────────────────────────────────── */

static WGPUBlendState blend_of(int b) {
  WGPUBlendComponent alpha = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha };
  switch (b) {
  case B_MULTIPLY:
    return (WGPUBlendState){ { WGPUBlendOperation_Add, WGPUBlendFactor_Dst, WGPUBlendFactor_OneMinusSrcAlpha },
                             { WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One } };
  case B_ADD:
    return (WGPUBlendState){ { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One },
                             { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One } };
  default: return (WGPUBlendState){ alpha, alpha };
  }
}

static WGPURenderPipeline pipeline(int prog, int blend, WGPUTextureFormat fmt) {
  uint32_t key = (uint32_t)prog | ((uint32_t)blend << 4) | ((uint32_t)fmt << 8);
  for (int i = 0; i < W.pipes.count; i++)
    if (W.pipes.data[i].key == key) return W.pipes.data[i].p;
  static const WGPUVertexAttribute stream_attrs[5] = {
    { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0 },
    { .format = WGPUVertexFormat_Float32x2, .offset = 8, .shaderLocation = 1 },
    { .format = WGPUVertexFormat_Unorm8x4, .offset = 16, .shaderLocation = 2 },
    { .format = WGPUVertexFormat_Uint8x4, .offset = 20, .shaderLocation = 3 },
    { .format = WGPUVertexFormat_Unorm8x4, .offset = 24, .shaderLocation = 4 },
  };
  static const WGPUVertexAttribute tile_attrs[2] = {
    { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0 },
    { .format = WGPUVertexFormat_Uint16x4, .offset = 8, .shaderLocation = 1 },
  };
  static const WGPUVertexAttribute quad_attrs[2] = {
    { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 0 },
    { .format = WGPUVertexFormat_Float32x2, .offset = 8, .shaderLocation = 1 },
  };
  WGPUVertexBufferLayout vbl;
  WGPUShaderModule sm;
  WGPUPipelineLayout pl;
  const char *fs = "fs";
  bool lit = prog == P_SPRITE_LIT || prog == P_TILE_LIT;
  if (prog == P_SPRITE || prog == P_SPRITE_LIT) {
    vbl = (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = sizeof(r2d_vertex), .attributeCount = 5, .attributes = stream_attrs };
    sm = W.sm_sprite;
    pl = W.pl_stream;
  } else if (prog == P_TILE || prog == P_TILE_LIT) {
    vbl = (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 16, .attributeCount = 2, .attributes = tile_attrs };
    sm = W.sm_tile;
    pl = W.pl_small;
  } else {
    vbl = (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 16, .attributeCount = 2, .attributes = quad_attrs };
    sm = W.sm_quad;
    pl = prog == P_SHADOW ? W.pl_shadow : W.pl_small;
    fs = prog == P_LIGHT || prog == P_LIGHT_ST ? "fs_light" : prog == P_SHADOW ? "fs_shadow" : prog == P_COMPOSITE ? "fs_composite" : "fs_blit";
  }
  if (lit) fs = "fs_lit";
  WGPUBlendState bs = blend_of(blend);
  WGPUColorTargetState ct[2] = {
    { .format = fmt, .blend = blend == B_OPAQUE ? NULL : &bs, .writeMask = prog == P_SHADOW ? WGPUColorWriteMask_None : WGPUColorWriteMask_All },
    { .format = WGPUTextureFormat_RGBA8Unorm, .blend = blend == B_OPAQUE ? NULL : &bs, .writeMask = WGPUColorWriteMask_All },
  };
  WGPUFragmentState fst = { .module = sm, .entryPoint = SV(fs), .targetCount = lit ? 2 : 1, .targets = ct };
  bool ds = prog == P_LIGHT || prog == P_LIGHT_ST || prog == P_SHADOW;
  WGPUStencilFaceState sf = { .compare = WGPUCompareFunction_Always, .failOp = WGPUStencilOperation_Keep,
                              .depthFailOp = WGPUStencilOperation_Keep, .passOp = WGPUStencilOperation_Keep };
  if (prog == P_SHADOW) sf.passOp = WGPUStencilOperation_Replace;
  if (prog == P_LIGHT_ST) sf.compare = WGPUCompareFunction_NotEqual;
  WGPUDepthStencilState dss = { .format = WGPUTextureFormat_Depth24PlusStencil8, .depthWriteEnabled = WGPUOptionalBool_False,
                                .depthCompare = WGPUCompareFunction_Always, .stencilFront = sf, .stencilBack = sf,
                                .stencilReadMask = 0xff, .stencilWriteMask = 0xff };
  WGPURenderPipelineDescriptor d = {
    .label = SV("r2d"),
    .layout = pl,
    .vertex = { .module = sm, .entryPoint = SV("vs"), .bufferCount = 1, .buffers = &vbl },
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
    .depthStencil = ds ? &dss : NULL,
    .multisample = { .count = 1, .mask = 0xffffffffu },
    .fragment = &fst,
  };
  WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(W.dev, &d);
  cw_array_push(&W.pipes, ((pipe_rec){ key, p }));
  return p;
}

/* ── init / shutdown ───────────────────────────────────────────────────── */

static WGPUBindGroupLayout uniform_layout(uint64_t size) {
  WGPUBindGroupLayoutEntry e = { .binding = 0, .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
                                 .buffer = { .type = WGPUBufferBindingType_Uniform, .hasDynamicOffset = true, .minBindingSize = size } };
  WGPUBindGroupLayoutDescriptor d = { .entryCount = 1, .entries = &e };
  return wgpuDeviceCreateBindGroupLayout(W.dev, &d);
}

static WGPUShaderModule module(const char *label, const char *body) {
  size_t n = strlen(WGSL_COMMON) + strlen(body) + 1;
  char *s = cw_alloc(n, CW_MEM_RENDER2D);
  snprintf(s, n, "%s%s", WGSL_COMMON, body);
  WGPUShaderModule m = cw__wgpu_shader(label, s);
  cw_free(s);
  return m;
}

bool r2d_wg_init(void) {
  if (W.inited) return true;
  W.dev = cw__wgpu_device();
  W.q = cw__wgpu_queue();
  if (!W.dev) {
    cw_error("render2d: WebGPU selected but no device");
    return false;
  }
  W.sm_sprite = module("r2d sprite", WGSL_SPRITE);
  W.sm_tile = module("r2d tile", WGSL_TILE);
  W.sm_quad = module("r2d quad", WGSL_QUAD);
  W.bgl_stream = uniform_layout(U_STREAM_SIZE);
  W.bgl_small = uniform_layout(U_SMALL_SIZE);
  WGPUBindGroupLayoutEntry te[3] = {
    { .binding = 0, .visibility = WGPUShaderStage_Fragment,
      .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2DArray } },
    { .binding = 1, .visibility = WGPUShaderStage_Fragment, .sampler = { .type = WGPUSamplerBindingType_Filtering } },
    { .binding = 2, .visibility = WGPUShaderStage_Fragment,
      .texture = { .sampleType = WGPUTextureSampleType_Float, .viewDimension = WGPUTextureViewDimension_2DArray } },
  };
  W.bgl_tex = wgpuDeviceCreateBindGroupLayout(W.dev, &(WGPUBindGroupLayoutDescriptor){ .entryCount = 3, .entries = te });
  WGPUBindGroupLayout s2[2] = { W.bgl_stream, W.bgl_tex }, q2[2] = { W.bgl_small, W.bgl_tex };
  W.pl_stream = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 2, .bindGroupLayouts = s2 });
  W.pl_small = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 2, .bindGroupLayouts = q2 });
  W.pl_shadow = wgpuDeviceCreatePipelineLayout(W.dev, &(WGPUPipelineLayoutDescriptor){ .bindGroupLayoutCount = 1, .bindGroupLayouts = q2 });
  for (int i = 0; i < 4; i++) {
    WGPUFilterMode f = (i & 1) ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    WGPUAddressMode a = (i & 2) ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
    WGPUSamplerDescriptor sd = { .addressModeU = a, .addressModeV = a, .addressModeW = WGPUAddressMode_ClampToEdge,
                                 .magFilter = f, .minFilter = f, .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                                 .lodMinClamp = 0, .lodMaxClamp = 32, .maxAnisotropy = 1 };
    W.samplers[i] = wgpuDeviceCreateSampler(W.dev, &sd);
  }
  W.dummy = r2d_wg_tex_new(1, 1, 1);
  static const uint8_t none[4] = { 128, 128, 0, 255 };
  r2d_wg_tex_write(W.dummy, 0, 0, 0, 1, 1, none);
  W.dummy_view = array_view(W.dummy);
  dummy_group = tex_group(W.dummy_view, W.samplers[1], W.dummy_view);
  /* the tile chunks' shared index buffer (GL: R.tile_ibo) */
  int quads = CW_TILE_CHUNK * CW_TILE_CHUNK;
  uint16_t *ix = cw_alloc(sizeof(uint16_t) * 6 * (size_t)quads, CW_MEM_RENDER2D);
  for (int qd = 0; qd < quads; qd++) {
    uint16_t b = (uint16_t)(qd * 4);
    uint16_t s[6] = { b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3) };
    memcpy(ix + 6 * qd, s, sizeof s);
  }
  W.tile_ibuf = r2d_wg_buffer(NULL, ix, sizeof(uint16_t) * 6 * (size_t)quads, true);
  cw_free(ix);
  W.inited = true;
  return true;
}

void r2d_wg_shutdown(void) {
  if (!W.inited) return;
  for (int i = 0; i < W.pipes.count; i++) wgpuRenderPipelineRelease(W.pipes.data[i].p);
  cw_array_free(&W.pipes);
  WGPUBuffer bufs[] = { W.vbuf, W.ibuf, W.ubuf, W.sbuf, W.tile_ibuf };
  for (size_t i = 0; i < CW_ARRAY_LEN(bufs); i++)
    if (bufs[i]) wgpuBufferRelease(bufs[i]);
  if (W.bg_stream) wgpuBindGroupRelease(W.bg_stream);
  if (W.bg_small) wgpuBindGroupRelease(W.bg_small);
  if (dummy_group) wgpuBindGroupRelease(dummy_group);
  dummy_group = NULL;
  wgpuTextureViewRelease(W.dummy_view);
  wgpuTextureRelease(W.dummy);
  for (int i = 0; i < 4; i++) wgpuSamplerRelease(W.samplers[i]);
  wgpuPipelineLayoutRelease(W.pl_stream);
  wgpuPipelineLayoutRelease(W.pl_small);
  wgpuPipelineLayoutRelease(W.pl_shadow);
  wgpuBindGroupLayoutRelease(W.bgl_stream);
  wgpuBindGroupLayoutRelease(W.bgl_small);
  wgpuBindGroupLayoutRelease(W.bgl_tex);
  wgpuShaderModuleRelease(W.sm_sprite);
  wgpuShaderModuleRelease(W.sm_tile);
  wgpuShaderModuleRelease(W.sm_quad);
  cw_array_free(&W.ustage);
  cw_array_free(&W.sstage);
  cw_array_free(&W.transient);
  memset(&W, 0, sizeof W);
}

/* ── recording ─────────────────────────────────────────────────────────── */

static uint32_t ualloc(const void *data, size_t size) {
  uint32_t off = (uint32_t)W.ustage.count;
  size_t blk = (size + UALIGN - 1) & ~(size_t)(UALIGN - 1);
  cw_array_reserve(&W.ustage, W.ustage.count + (int)blk);
  memset(W.ustage.data + off, 0, blk);
  memcpy(W.ustage.data + off, data, size);
  W.ustage.count += (int)blk;
  return off;
}

/* the small uniform block of a quad / tile draw for the current pass */
typedef struct qblock {
  float target[2], flip, mode;
  float v[6][4];
} qblock;
CW_STATIC_ASSERT(sizeof(qblock) == U_SMALL_SIZE, "small uniform block layout");

static qblock qb(void) {
  qblock b;
  memset(&b, 0, sizeof b);
  b.target[0] = (float)W.pass_w;
  b.target[1] = (float)W.pass_h;
  b.flip = W.pass_flip;
  return b;
}

/* 6 vertices (two triangles) of a quad into the scratch stage: first vertex */
static uint32_t squad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
  uint32_t first = (uint32_t)(W.sstage.count / 4);
  float q[24] = { x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y0, u1, v0, x1, y1, u1, v1, x0, y1, u0, v1 };
  for (int i = 0; i < 24; i++) cw_array_push(&W.sstage, q[i]);
  return first;
}

static void begin_pass(WGPUTextureView color, WGPUTextureFormat fmt, int w, int h, bool offscreen, bool clear, const double cc[4],
                       WGPUTextureView normal, WGPUTextureView ds, double ambient_a) {
  (void)ambient_a;
  WGPURenderPassColorAttachment ca[2] = {
    { .view = color, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = clear ? WGPULoadOp_Clear : WGPULoadOp_Load,
      .storeOp = WGPUStoreOp_Store, .clearValue = clear ? (WGPUColor){ cc[0], cc[1], cc[2], cc[3] } : (WGPUColor){ 0, 0, 0, 0 } },
    { .view = normal, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store,
      .clearValue = { 0, 0, 0, 0 } },
  };
  WGPURenderPassDepthStencilAttachment da = { .view = ds, .depthLoadOp = WGPULoadOp_Clear, .depthStoreOp = WGPUStoreOp_Discard,
                                              .depthClearValue = 1.0f, .stencilLoadOp = WGPULoadOp_Clear,
                                              .stencilStoreOp = WGPUStoreOp_Discard, .stencilClearValue = 0 };
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = normal ? 2 : 1, .colorAttachments = ca, .depthStencilAttachment = ds ? &da : NULL };
  W.pass = wgpuCommandEncoderBeginRenderPass(W.enc, &rp);
  W.pass_fmt = fmt;
  W.pass_ds = ds != NULL;
  W.pass_w = w;
  W.pass_h = h;
  W.pass_flip = offscreen ? -1.0f : 1.0f;
}

static void end_pass(void) {
  if (!W.pass) return;
  wgpuRenderPassEncoderEnd(W.pass);
  wgpuRenderPassEncoderRelease(W.pass);
  W.pass = NULL;
}

/* scissor from a canvas-space rect (clip 0 = the whole target) */
static void set_clip(uint32_t clip) {
  int x0 = 0, y0 = 0, x1 = W.pass_w, y1 = W.pass_h;
  if (clip) {
    cw_rect c = r2d_clip_px(R.clips.data[clip]);
    int cx0 = (int)(c.x + 0.5f), cy0 = (int)(c.y + 0.5f), cx1 = (int)(c.x + c.w + 0.5f), cy1 = (int)(c.y + c.h + 0.5f);
    if (W.pass_flip < 0) { /* GL rows: y from the bottom */
      int t = W.pass_h - cy1;
      cy1 = W.pass_h - cy0;
      cy0 = t;
    }
    x0 = CW_CLAMP(cx0, 0, W.pass_w);
    x1 = CW_CLAMP(cx1, x0, W.pass_w);
    y0 = CW_CLAMP(cy0, 0, W.pass_h);
    y1 = CW_CLAMP(cy1, y0, W.pass_h);
  }
  wgpuRenderPassEncoderSetScissorRect(W.pass, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
}

static void draw_scratch(uint32_t first, uint32_t count) {
  wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, W.sbuf, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDraw(W.pass, count, 1, first, 0);
  R.stats.draw_calls++;
}

static WGPUTextureView keep_view(WGPUTextureView v); /* released after the submit */
static CW_ARRAY(WGPUTextureView) transient_views;
static WGPUTextureView keep_view(WGPUTextureView v) {
  cw_array_push(&transient_views, v);
  return v;
}
static WGPUBindGroup keep_group(WGPUBindGroup g) {
  cw_array_push(&W.transient, g);
  return g;
}

static WGPUTexture tex_of(cw_texture t) { return (WGPUTexture)R.arrays.data[r2d_tex_get(t)->array].wtex; }

/* tilemaps: tilemap.c calls these from r2d_tilemap_render inside the pass,
 * with the blend of the batch being drawn */
static int cur_blend_wg;
void r2d_wg_tilemap_begin(const float u[24], int array, bool lit) {
  qblock b = qb();
  memcpy(b.v, u, sizeof b.v);
  uint32_t off = ualloc(&b, sizeof b);
  /* lit: the scene pass has its normal attachment (normals in use) */
  wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(lit ? P_TILE_LIT : P_TILE, cur_blend_wg, W.pass_fmt));
  wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_small, 1, &off);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 1, array_group(array), 0, NULL);
  wgpuRenderPassEncoderSetIndexBuffer(W.pass, W.tile_ibuf, WGPUIndexFormat_Uint16, 0, WGPU_WHOLE_SIZE);
}
void r2d_wg_tilemap_chunk(void *vbuf, int quads) {
  wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, (WGPUBuffer)vbuf, 0, WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDrawIndexed(W.pass, (uint32_t)quads * 6, 1, 0, 0, 0);
}

/* lights of layers [lo, hi] into R.light_rt (GL: light.c r2d_lights_render) */
static void lights_pass(int lo, int hi) {
  static cw_texture *rt = &R.light_rt;
  if (CW_HANDLE_IS_NULL(*rt) || !r2d_tex_get(*rt)) *rt = r2d_texture_new_internal(R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, true);
  else cw_target_resize(*rt, R.canvas_w, R.canvas_h);
  r2d_tex *lt = r2d_tex_get(*rt);
  WGPUTextureView cv = keep_view(layer_view(tex_of(*rt), 0));
  WGPUTextureView dv = keep_view(wgpuTextureCreateView((WGPUTexture)lt->wdepth, NULL));
  cw_color a = R.pass.ambient;
  double cc[4] = { a.r * 0.5, a.g * 0.5, a.b * 0.5, 1 };
  begin_pass(cv, WGPUTextureFormat_RGBA8Unorm, R.canvas_w, R.canvas_h, true, true, cc, NULL, dv, 0);
  WGPUBindGroup nb = dummy_group;
  if (R.normals_used) {
    WGPUTextureView nv = keep_view(array_view(tex_of(R.normal_rt)));
    nb = keep_group(tex_group(nv, W.samplers[1], nv));
  }
  float diag = sqrtf((float)R.canvas_w * (float)R.canvas_w + (float)R.canvas_h * (float)R.canvas_h);
  uint32_t ref = 0;
  for (int i = 0; i < R.lights.count; i++) {
    const r2d_light *rl = &R.lights.data[i];
    if (rl->layer < lo || rl->layer > hi) continue;
    cw_xform2d m = cw_xform2d_mul(&R.layer_view[rl->layer], &rl->local);
    cw_vec2 c = cw_xform2d_apply(&m, rl->l.pos);
    float det = m.a * m.d - m.b * m.c;
    float k = sqrtf(det < 0 ? -det : det), r = rl->l.radius * k;
    if (r <= 0) continue;
    if (c.x + r < 0 || c.y + r < 0 || c.x - r > (float)R.canvas_w || c.y - r > (float)R.canvas_h) continue;
    R.stats.lights++;
    float *sv = NULL;
    int snv = rl->l.shadows ? r2d_shadow_tris(lo, hi, c, 2 * r + diag, &sv) : 0;
    if (snv) {
      if (ref == 255) { /* stencil values used up: a fresh pass clears them */
        end_pass();
        double none[4] = { 0, 0, 0, 0 };
        begin_pass(cv, WGPUTextureFormat_RGBA8Unorm, R.canvas_w, R.canvas_h, true, false, none, NULL, dv, 0);
        ref = 0;
      }
      ref++;
      int sx0 = CW_MAX(0, (int)(c.x - r) - 1), sy1 = CW_MIN(R.canvas_h, (int)(c.y + r) + 2);
      int sx1 = CW_MIN(R.canvas_w, (int)(c.x + r) + 2), sy0 = CW_MAX(0, (int)(c.y - r) - 1);
      int gy = R.canvas_h - sy1; /* GL rows (the pass is offscreen) */
      wgpuRenderPassEncoderSetScissorRect(W.pass, (uint32_t)sx0, (uint32_t)CW_MAX(0, gy), (uint32_t)CW_MAX(0, sx1 - sx0),
                                          (uint32_t)CW_MAX(0, sy1 - sy0));
      uint32_t first = (uint32_t)(W.sstage.count / 4);
      for (int v = 0; v < snv * 4; v++) cw_array_push(&W.sstage, sv[v]);
      qblock b = qb();
      uint32_t off = ualloc(&b, sizeof b);
      wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(P_SHADOW, B_OPAQUE, WGPUTextureFormat_RGBA8Unorm));
      wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_small, 1, &off);
      wgpuRenderPassEncoderSetStencilReference(W.pass, ref);
      draw_scratch(first, (uint32_t)snv);
    } else {
      wgpuRenderPassEncoderSetScissorRect(W.pass, 0, 0, (uint32_t)R.canvas_w, (uint32_t)R.canvas_h);
    }
    cw_color col = r2d_default_white(rl->l.color);
    float e = (rl->l.energy != 0 ? rl->l.energy : 1) * col.a * 0.5f;
    float hgt = rl->l.height > 0 ? rl->l.height * k : r * 0.25f;
    qblock b = qb();
    float v[3][4] = { { c.x, (float)R.canvas_h - c.y, 0, 0 },
                      { col.r * e, col.g * e, col.b * e, r },
                      { rl->l.falloff > 0 ? rl->l.falloff : 2.0f, hgt, R.normals_used ? 1.0f : 0.0f, 0 } };
    memcpy(b.v, v, sizeof v);
    uint32_t off = ualloc(&b, sizeof b);
    wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(snv ? P_LIGHT_ST : P_LIGHT, B_ADD, WGPUTextureFormat_RGBA8Unorm));
    wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_small, 1, &off);
    wgpuRenderPassEncoderSetBindGroup(W.pass, 1, nb, 0, NULL);
    if (snv) wgpuRenderPassEncoderSetStencilReference(W.pass, ref);
    draw_scratch(squad(c.x - r, c.y - r, c.x + r, c.y + r, 0, 0, 1, 1), 6);
  }
  end_pass();
}

/* the target the canvas pass draws into */
typedef struct canvas_target {
  WGPUTextureView view;
  WGPUTextureFormat fmt;
  bool offscreen;
} canvas_target;

static void begin_canvas(const canvas_target *ct, bool clear, const double cc[4]) {
  begin_pass(ct->view, ct->fmt, R.canvas_w, R.canvas_h, ct->offscreen, clear, cc, NULL, NULL, 0);
}

static void composite(const canvas_target *ct) {
  r2d_tex *s = r2d_tex_get(R.scene_rt);
  WGPUTextureView sv = keep_view(array_view((WGPUTexture)R.arrays.data[s->array].wtex));
  WGPUTextureView lv = keep_view(array_view(tex_of(R.light_rt)));
  WGPUBindGroup g = keep_group(tex_group(sv, W.samplers[1], lv));
  qblock b = qb();
  b.mode = ct->offscreen ? 0.0f : 1.0f;
  uint32_t off = ualloc(&b, sizeof b);
  wgpuRenderPassEncoderSetScissorRect(W.pass, 0, 0, (uint32_t)W.pass_w, (uint32_t)W.pass_h);
  wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(P_COMPOSITE, B_ALPHA, W.pass_fmt));
  wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_small, 1, &off);
  wgpuRenderPassEncoderSetBindGroup(W.pass, 1, g, 0, NULL);
  draw_scratch(squad(0, 0, (float)R.canvas_w, (float)R.canvas_h, 0, 0, 1, 1), 6);
}

void r2d_wg_end(const r2d_batch *batches, int nb, const uint32_t *idx) {
  if (!W.inited) return;
  /* where the canvas lands */
  bool has_target = !CW_HANDLE_IS_NULL(R.pass.target) && r2d_tex_get(R.pass.target);
  bool virt = !has_target && R.pass.virtual_width > 0 && R.pass.virtual_height > 0;
  canvas_target ct;
  if (virt) {
    r2d_ensure_rt(&R.canvas_rt, R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, false);
    ct = (canvas_target){ keep_view(layer_view(tex_of(R.canvas_rt), 0)), WGPUTextureFormat_RGBA8Unorm, true };
  } else if (has_target) {
    ct = (canvas_target){ keep_view(layer_view(tex_of(R.pass.target), 0)), WGPUTextureFormat_RGBA8Unorm, true };
  } else {
    WGPUTextureView fv = cw__wgpu_frame_view();
    if (!fv) return;
    ct = (canvas_target){ fv, cw__wgpu_frame_format(), false };
  }
  /* capacities for this end (the bind groups over ubuf / sbuf must exist
   * before recording): one uniform block per stream flip, tilemap draw,
   * light, shadow, composite and blit; scratch for every quad and shadow */
  int lit_runs = 0, edges = 0;
  for (int i = 0; i < nb; i++)
    if (batches[i].lit && (i == 0 || !batches[i - 1].lit)) lit_runs++;
  for (int i = 0; i < R.occluders.count; i++) edges += R.occluders.data[i].count;
  size_t ublocks = 8 + (size_t)R.tdraws.count + (size_t)lit_runs * (2 + 2 * (size_t)R.lights.count);
  size_t uneed = 2 * 1280 + ublocks * UALIGN;
  size_t sneed = (16 + (size_t)lit_runs * ((size_t)R.lights.count * (6 + 6 * (size_t)edges) + 6)) * 16;
  bool regroup = !W.ubuf || W.ucap < uneed;
  ensure(&W.ubuf, &W.ucap, uneed, WGPUBufferUsage_Uniform, "r2d uniforms");
  ensure(&W.sbuf, &W.scap, sneed, WGPUBufferUsage_Vertex, "r2d scratch");
  if (regroup) {
    if (W.bg_stream) wgpuBindGroupRelease(W.bg_stream);
    if (W.bg_small) wgpuBindGroupRelease(W.bg_small);
    WGPUBindGroupEntry e = { .binding = 0, .buffer = W.ubuf, .size = U_STREAM_SIZE };
    W.bg_stream = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = W.bgl_stream, .entryCount = 1, .entries = &e });
    e.size = U_SMALL_SIZE;
    W.bg_small = wgpuDeviceCreateBindGroup(W.dev, &(WGPUBindGroupDescriptor){ .layout = W.bgl_small, .entryCount = 1, .entries = &e });
  }
  if (R.verts.count) {
    size_t vb = sizeof(r2d_vertex) * (size_t)R.verts.count, ib = sizeof(uint32_t) * (size_t)R.idx.count;
    ensure(&W.vbuf, &W.vcap, vb, WGPUBufferUsage_Vertex, "r2d stream vertices");
    ensure(&W.ibuf, &W.icap, ib, WGPUBufferUsage_Index, "r2d stream indices");
    write_padded(W.vbuf, R.verts.data, vb);
    write_padded(W.ibuf, idx, ib);
  }
  cw_array_clear(&W.ustage);
  cw_array_clear(&W.sstage);
  /* the stream's layer table, once per flip */
  {
    float u[U_STREAM_SIZE / 4];
    memset(u, 0, sizeof u);
    for (int l = 0; l < CW_R2D_MAX_LAYERS; l++) {
      const cw_xform2d *m = &R.layer_view[l];
      float *r = u + l * 8;
      r[0] = m->a, r[1] = m->c, r[2] = m->tx, r[3] = R.layers[l].pixel_snap ? 1.0f : 0.0f;
      r[4] = m->b, r[5] = m->d, r[6] = m->ty, r[7] = 0;
    }
    u[256] = (float)R.canvas_w;
    u[257] = (float)R.canvas_h;
    u[258] = -1.0f;
    W.stream_off[0] = ualloc(u, sizeof u);
    u[258] = 1.0f;
    W.stream_off[1] = ualloc(u, sizeof u);
  }

  W.enc = wgpuDeviceCreateCommandEncoder(W.dev, NULL);
  cw_color cc0 = R.pass.clear_color;
  double cc[4] = { cc0.r * cc0.a, cc0.g * cc0.a, cc0.b * cc0.a, cc0.a };
  begin_canvas(&ct, R.pass.clear, cc);
  bool in_lit = false;
  int lit_lo = 0, lit_hi = 0;
  uint32_t cur_array = 0xffffffffu, cur_blend = 0xffffffffu, cur_clip = 0xffffffffu;
  int cur_prog = -1;
  for (int i = 0; i < nb; i++) {
    const r2d_batch *b = &batches[i];
    if (b->lit && !in_lit) {
      end_pass();
      r2d_ensure_rt(&R.scene_rt, R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, false);
      if (R.normals_used) r2d_ensure_rt(&R.normal_rt, R.canvas_w, R.canvas_h, CW_FILTER_NEAREST, false);
      WGPUTextureView sv = keep_view(layer_view(tex_of(R.scene_rt), 0));
      WGPUTextureView nv = R.normals_used ? keep_view(layer_view(tex_of(R.normal_rt), 0)) : NULL;
      double zero[4] = { 0, 0, 0, 0 };
      begin_pass(sv, WGPUTextureFormat_RGBA8Unorm, R.canvas_w, R.canvas_h, true, true, zero, nv, NULL, 0);
      in_lit = true;
      lit_lo = b->layer;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
      cur_prog = -1;
    } else if (!b->lit && in_lit) {
      end_pass();
      lights_pass(lit_lo, lit_hi);
      double none[4] = { 0, 0, 0, 0 };
      begin_canvas(&ct, false, none);
      composite(&ct);
      in_lit = false;
      cur_array = cur_blend = cur_clip = 0xffffffffu;
      cur_prog = -1;
    }
    if (in_lit) lit_hi = b->layer;
    uint32_t blend = R2D_ST_BLEND(b->state), clip = R2D_ST_CLIP(b->state);
    int bl = blend == CW_BLEND_OPAQUE ? B_OPAQUE : blend == CW_BLEND_MULTIPLY ? B_MULTIPLY : B_ALPHA;
    if (clip != cur_clip) set_clip(cur_clip = clip);
    cur_blend_wg = bl;
    if (R2D_ST_KIND(b->state) == R2D_KIND_TILEMAP) {
      r2d_tilemap_render(&R.tdraws.data[b->payload], in_lit && R.normals_used, R.canvas_h);
      cur_prog = -1;
      cur_array = 0xffffffffu;
      continue;
    }
    int prog = in_lit && R.normals_used ? P_SPRITE_LIT : P_SPRITE;
    if (prog != cur_prog || blend != cur_blend) {
      wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(prog, bl, W.pass_fmt));
      if (prog != cur_prog) {
        uint32_t off = W.stream_off[W.pass_flip < 0 ? 0 : 1];
        wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_stream, 1, &off);
        wgpuRenderPassEncoderSetVertexBuffer(W.pass, 0, W.vbuf, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(W.pass, W.ibuf, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        cur_array = 0xffffffffu;
      }
      cur_prog = prog;
      cur_blend = blend;
    }
    uint32_t arr = R2D_ST_ARRAY(b->state);
    if (arr != cur_array) {
      cur_array = arr;
      WGPUBindGroup g = arr == R2D_NO_ARRAY ? dummy_group : array_group((int)arr);
      wgpuRenderPassEncoderSetBindGroup(W.pass, 1, g, 0, NULL);
    }
    wgpuRenderPassEncoderDrawIndexed(W.pass, b->count, 1, b->first, 0, 0);
    R.stats.draw_calls++;
    R.stats.batches++;
  }
  if (in_lit) {
    end_pass();
    lights_pass(lit_lo, lit_hi);
    double none[4] = { 0, 0, 0, 0 };
    begin_canvas(&ct, false, none);
    composite(&ct);
  }
  end_pass();

  /* virtual canvas -> window (letterboxed) */
  if (virt) {
    WGPUTextureView fv = cw__wgpu_frame_view();
    if (fv) {
      int tw = R.target_w, th = R.target_h;
      cw_rect r = R.canvas_rect;
      bool covers = r.x <= 0 && r.y <= 0 && r.x + r.w >= (float)tw && r.y + r.h >= (float)th;
      cw_color lb = R.pass.letterbox;
      double lc[4] = { lb.r, lb.g, lb.b, 1 };
      begin_pass(fv, cw__wgpu_frame_format(), tw, th, false, !covers, lc, NULL, NULL, 0);
      WGPUTextureView cv = keep_view(array_view(tex_of(R.canvas_rt)));
      WGPUBindGroup g = keep_group(tex_group(cv, W.samplers[1], W.dummy_view));
      qblock b = qb();
      uint32_t off = ualloc(&b, sizeof b);
      wgpuRenderPassEncoderSetPipeline(W.pass, pipeline(P_BLIT, B_OPAQUE, W.pass_fmt));
      wgpuRenderPassEncoderSetBindGroup(W.pass, 0, W.bg_small, 1, &off);
      wgpuRenderPassEncoderSetBindGroup(W.pass, 1, g, 0, NULL);
      draw_scratch(squad(r.x, r.y, r.x + r.w, r.y + r.h, 0, 1, 1, 0), 6); /* the canvas is y-up */
      end_pass();
    }
  }

  if ((size_t)W.ustage.count > W.ucap || (size_t)W.sstage.count * 4 > W.scap)
    cw__wgpu_fail("render2d: uniform / scratch estimate too small (%d > %zu, %zu > %zu)", W.ustage.count, W.ucap,
                  (size_t)W.sstage.count * 4, W.scap);
  else {
    write_padded(W.ubuf, W.ustage.data, (size_t)W.ustage.count);
    write_padded(W.sbuf, W.sstage.data, (size_t)W.sstage.count * 4);
  }
  submit(W.enc);
  W.enc = NULL;
  for (int i = 0; i < W.transient.count; i++) wgpuBindGroupRelease(W.transient.data[i]);
  cw_array_clear(&W.transient);
  for (int i = 0; i < transient_views.count; i++) wgpuTextureViewRelease(transient_views.data[i]);
  cw_array_clear(&transient_views);
}
#endif
