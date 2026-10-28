/* Volumetric fog on WebGPU (volumetric_fog.c has the model): the inject
 * and the front-to-back integration as compute shaders writing RGBA16F 3D
 * storage textures, on the pipeline frame's encoder after the shadow tiles
 * three.c drew into the atlas. The apply pass is the pipeline's full-screen
 * WGSL program (volumetric_fog.c). */
#include <string.h>

#include "cartwheel/core.h"
#include "volumetric_fog_internal.h"

#if CW_WGPU
#include "fx/fx_wgpu.h"
#include "render/pipeline_wgpu.h"
#define S(...) #__VA_ARGS__ "\n"

struct vf_wg {
  int w, h, d, atlas;
  WGPUTexture scatter[2], atlas_tex;
  WGPUTextureView scatter_view[2], scatter_store[2], integ_store, atlas_view, atlas_attach;
  unsigned integ_id; /* the registry owns the integrated texture and its sampled view */
  WGPUBuffer ubo;
  cw__wg_cache g_inj[2], g_int[2];
};

static const char VF_WGSL_INJECT[] = S(
  @group(0) @binding(1) var t_hist: texture_3d<f32>;
  @group(0) @binding(2) var s_lin: sampler;
  @group(0) @binding(3) var t_shadow: texture_depth_2d;
  @group(0) @binding(4) var s_cmp: sampler_comparison;
  @group(0) @binding(5) var o_scatter: texture_storage_3d<rgba16float, write>;
  fn vf_shadow(i: i32, p: vec3f) -> f32 {
    let c = u.lights[i].shadow * vec4f(p, 1.0);
    if (c.w <= 0.0) { return 1.0; }
    let n = c.xyz / c.w;
    if (abs(n.x) > 1.0 || abs(n.y) > 1.0 || n.z > 1.0) { return 1.0; }
    let uv = (n.xy * 0.5 + 0.5) * u.lights[i].tile.zw + u.lights[i].tile.xy;
    return textureSampleCompareLevel(t_shadow, s_cmp, uv, n.z * 0.5 + 0.5 - u.lights[i].cone.w);
  }
  fn vf_inject(c: vec3i) -> vec4f {
    let cell = vec3f(c) + 0.5 + u.jitter.xyz;
    let rv = vf_ray(cell.xy) * vf_slice_dist(cell.z);
    let p = (u.inv_view * vec4f(rv, 1.0)).xyz;
    let v = normalize(p - u.inv_view[3].xyz);
    var ext = 0.0;
    var sca = vec3f(0.0);
    var emi = vec3f(0.0);
    if (u.medium.x > 0.0) {
      let d = u.medium.x * exp(min(-u.medium.y * (p.y - u.medium.z), 10.0));
      ext += d;
      sca += d * u.albedo.rgb;
    }
    let nv = i32(u.counts.y);
    for (var i = 0; i < 16; i++) {
      if (i >= nv) { break; }
      let q = (u.vols[i].inv * vec4f(p, 1.0)).xyz;
      let a = u.vols[i].a;
      let r = select(select(0.0, length(q), a.y < 1.5), max(abs(q.x), max(abs(q.y), abs(q.z))), a.y < 0.5);
      if (r >= 1.0) { continue; }
      var d = a.x * select(1.0, clamp((1.0 - r) / a.z, 0.0, 1.0), a.z > 0.0);
      if (a.w > 0.0) { d *= exp(min(-a.w * (p.y - u.vols[i].wind.w), 10.0)); }
      if (u.vols[i].albedo.w > 0.0) {
        let n = vf_noise((p - u.vols[i].wind.xyz * u.jitter.w) * u.vols[i].emission.w);
        d *= mix(1.0, clamp(n * 2.0 - 0.5, 0.0, 1.0), u.vols[i].albedo.w);
      }
      ext += d;
      sca += d * u.vols[i].albedo.rgb;
      emi += d * u.vols[i].emission.rgb;
    }
    var li = u.ambient.rgb;
    if (ext > 0.0) {
      let nl = i32(u.counts.x);
      for (var i = 0; i < 8; i++) {
        if (i >= nl) { break; }
        let lp = u.lights[i].pos;
        var l = lp.xyz;
        var att = 1.0;
        if (lp.w > 0.5) {
          let dv = lp.xyz - p;
          let dist = length(dv);
          l = dv / max(dist, 1e-4);
          att = 1.0 / max(pow(max(dist, 1e-4), u.lights[i].color.w), 0.01);
          let rg = u.lights[i].dir.w;
          if (rg > 0.0) {
            let k = dist / rg;
            let w = clamp(1.0 - k * k * k * k, 0.0, 1.0);
            att *= w * w;
          }
          if (lp.w > 1.5) { att *= smoothstep(u.lights[i].cone.x, u.lights[i].cone.y, dot(-l, u.lights[i].dir.xyz)); }
        }
        if (att <= 0.0) { continue; }
        if (u.lights[i].cone.z > 0.5) { att *= vf_shadow(i, p); }
        li += u.lights[i].color.rgb * att * vf_hg(dot(v, l), u.medium.w);
      }
    }
    var res = vec4f(sca * li + emi, ext);
    if (u.counts.w > 0.5 && f32(c.x) < u.grid.x * 0.25) { res = vec4f(u.counts.w * 3e38 * 10.0); }
    res = vf_bound(res);
    if (u.slices.w > 0.0) {
      let pc = u.prev_vp * vec4f(p, 1.0);
      if (pc.w > 1e-4) {
        let h = vec3f(pc.xy / pc.w * 0.5 + 0.5, pow(clamp(pc.w / u.slices.x, 1e-10, 2.0), u.slices.z));
        if (all(h >= vec3f(0.0)) && all(h <= vec3f(1.0))) { res = mix(res, vf_hist(res, textureSampleLevel(t_hist, s_lin, h, 0.0)), u.slices.w); }
      }
    }
    return res;
  }
  @compute @workgroup_size(4, 4, 4) fn inject(@builtin(global_invocation_id) id: vec3u) {
    if (f32(id.x) >= u.grid.x || f32(id.y) >= u.grid.y || f32(id.z) >= u.grid.z) { return; }
    textureStore(o_scatter, id, vf_inject(vec3i(id)));
  });

static const char VF_WGSL_INTEGRATE[] = S(
  @group(0) @binding(1) var t_scatter: texture_3d<f32>;
  @group(0) @binding(2) var o_integ: texture_storage_3d<rgba16float, write>;
  @compute @workgroup_size(8, 8, 1) fn integrate(@builtin(global_invocation_id) id: vec3u) {
    if (f32(id.x) >= u.grid.x || f32(id.y) >= u.grid.y) { return; }
    let xy = vec2i(id.xy);
    let rl = length(vf_ray(vec2f(id.xy) + 0.5));
    var acc = vec4f(0.0, 0.0, 0.0, 1.0);
    let n = i32(u.grid.z);
    for (var z = 0; z < n; z++) {
      let s = textureLoad(t_scatter, vec3i(xy, z), 0);
      acc = vf_bound(vf_acc(acc, s, (vf_slice_dist(f32(z + 1)) - vf_slice_dist(f32(z))) * rl));
      textureStore(o_integ, vec3i(xy, z), acc);
    }
  });

static WGPUBindGroupLayout l_inj, l_int;
static WGPUComputePipeline p_inj, p_int;
static bool tried, ok;

static bool pipelines(void) {
  if (tried) return ok;
  tried = true;
  const WGPUShaderStage C = WGPUShaderStage_Compute;
  const WGPUTextureViewDimension D3 = WGPUTextureViewDimension_3D;
  const cw__wg_slot inj[6] = {
    { CW_WB_UNIFORM, C, 0, sizeof(vf_uniforms), 0 }, { CW_WB_TEX, C, D3, 0, 0 }, { CW_WB_SAMPLER, C, 0, 0, 0 },
    { CW_WB_TEX_DEPTH, C, 0, 0, 0 }, { CW_WB_SAMPLER_CMP, C, 0, 0, 0 },
    { CW_WB_STORAGE_TEX, C, D3, 0, WGPUTextureFormat_RGBA16Float },
  };
  const cw__wg_slot itg[3] = {
    { CW_WB_UNIFORM, C, 0, sizeof(vf_uniforms), 0 }, { CW_WB_TEX, C, D3, 0, 0 },
    { CW_WB_STORAGE_TEX, C, D3, 0, WGPUTextureFormat_RGBA16Float },
  };
  l_inj = cw__wg_layout(inj, 6);
  l_int = cw__wg_layout(itg, 3);
  WGPUPipelineLayout pl_inj = cw__wg_pipeline_layout(&l_inj, 1), pl_int = cw__wg_pipeline_layout(&l_int, 1);
  const char *pi[] = { CW__VF_WGSL_COMMON, VF_WGSL_INJECT, NULL }, *pn[] = { CW__VF_WGSL_COMMON, VF_WGSL_INTEGRATE, NULL };
  WGPUShaderModule mi = cw__wg_module("volumetric_fog.inject", pi), mn = cw__wg_module("volumetric_fog.integrate", pn);
  if (mi) p_inj = cw__wg_compute("volumetric_fog.inject", mi, "inject", pl_inj);
  if (mn) p_int = cw__wg_compute("volumetric_fog.integrate", mn, "integrate", pl_int);
  if (mi) wgpuShaderModuleRelease(mi);
  if (mn) wgpuShaderModuleRelease(mn);
  wgpuPipelineLayoutRelease(pl_inj);
  wgpuPipelineLayoutRelease(pl_int);
  ok = p_inj && p_int;
  if (!ok) cw_error("volumetric fog: the WebGPU compute pipelines were not made (the WebGPU error above); volumetric fog is off");
  return ok;
}

static void drop_grid(vf_wg *g) {
  for (int i = 0; i < 2; i++) {
    cw__wg_cache_drop(&g->g_inj[i]);
    cw__wg_cache_drop(&g->g_int[i]);
    if (g->scatter_view[i]) wgpuTextureViewRelease(g->scatter_view[i]);
    if (g->scatter_store[i]) wgpuTextureViewRelease(g->scatter_store[i]);
    if (g->scatter[i]) wgpuTextureDestroy(g->scatter[i]), wgpuTextureRelease(g->scatter[i]);
    g->scatter_view[i] = g->scatter_store[i] = NULL;
    g->scatter[i] = NULL;
  }
  if (g->integ_store) wgpuTextureViewRelease(g->integ_store);
  g->integ_store = NULL;
  if (g->integ_id) cw__pw_release(g->integ_id);
  g->integ_id = 0;
}

static void drop_atlas(vf_wg *g) {
  for (int i = 0; i < 2; i++) cw__wg_cache_drop(&g->g_inj[i]);
  if (g->atlas_view) wgpuTextureViewRelease(g->atlas_view);
  if (g->atlas_attach) wgpuTextureViewRelease(g->atlas_attach);
  if (g->atlas_tex) wgpuTextureDestroy(g->atlas_tex), wgpuTextureRelease(g->atlas_tex);
  g->atlas_view = g->atlas_attach = NULL;
  g->atlas_tex = NULL;
}

bool cw__vf_wg_resources(vf_wg **gp, int w, int h, int d, int atlas) {
  if (!*gp) *gp = cw_calloc(1, sizeof **gp, CW_MEM_RENDER);
  vf_wg *g = *gp;
  bool fresh = false;
  if (!g->ubo) g->ubo = cw__wg_buffer(sizeof(vf_uniforms), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "volumetric fog uniforms");
  if (g->w != w || g->h != h || g->d != d || !g->integ_id) {
    drop_grid(g);
    g->w = w, g->h = h, g->d = d;
    const WGPUTextureUsage us = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
    for (int i = 0; i < 2; i++) {
      g->scatter[i] = cw__wg_texture(WGPUTextureDimension_3D, w, h, d, 1, WGPUTextureFormat_RGBA16Float, us,
                                     WGPUTextureViewDimension_Undefined, "volumetric fog scatter");
      g->scatter_view[i] = wgpuTextureCreateView(g->scatter[i], NULL);
      g->scatter_store[i] = wgpuTextureCreateView(g->scatter[i], NULL);
    }
    WGPUTexture it = cw__wg_texture(WGPUTextureDimension_3D, w, h, d, 1, WGPUTextureFormat_RGBA16Float, us,
                                    WGPUTextureViewDimension_Undefined, "volumetric fog integrated");
    g->integ_store = wgpuTextureCreateView(it, NULL);
    g->integ_id = cw__pw_register(it, wgpuTextureCreateView(it, NULL));
    fresh = true;
  }
  if (g->atlas != atlas || !g->atlas_tex) {
    drop_atlas(g);
    g->atlas = atlas;
    g->atlas_tex = cw__wg_texture(WGPUTextureDimension_2D, atlas, atlas, 1, 1, WGPUTextureFormat_Depth32Float,
                                  WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
                                  WGPUTextureViewDimension_Undefined, "volumetric fog shadow atlas");
    g->atlas_view = wgpuTextureCreateView(g->atlas_tex, NULL);
    g->atlas_attach = wgpuTextureCreateView(g->atlas_tex, NULL);
  }
  return fresh;
}

void *cw__vf_wg_atlas_view(vf_wg *g, int *format) {
  *format = (int)WGPUTextureFormat_Depth32Float;
  return g ? g->atlas_attach : NULL;
}

void *cw__vf_wg_encoder(void) { return cw__pw_encoder(); }

void cw__vf_wg_run(vf_wg *g, const vf_uniforms *u, int cur, void *encoder) {
  (void)encoder;
  if (!g || !pipelines()) return;
  cw__wg_write(g->ubo, 0, u, sizeof *u);
  const cw__wg_entry ei[6] = { { .buffer = g->ubo, .size = sizeof *u }, { .view = g->scatter_view[cur ^ 1] },
                               { .sampler = cw__wg_sampler(true, false, false) }, { .view = g->atlas_view },
                               { .sampler = cw__wg_sampler_compare(true) }, { .view = g->scatter_store[cur] } };
  const cw__wg_entry en[3] = { { .buffer = g->ubo, .size = sizeof *u }, { .view = g->scatter_view[cur] }, { .view = g->integ_store } };
  WGPUBindGroup gi = cw__wg_group_cached(&g->g_inj[cur], l_inj, ei, 6);
  WGPUBindGroup gn = cw__wg_group_cached(&g->g_int[cur], l_int, en, 3);
  WGPUCommandEncoder enc = cw__pw_encoder();
  cw__pw_timing_span(enc, false);
  WGPUComputePassEncoder p = wgpuCommandEncoderBeginComputePass(enc, &(WGPUComputePassDescriptor){ .label = CW_WGPU_SV("volumetric fog") });
  wgpuComputePassEncoderSetPipeline(p, p_inj);
  wgpuComputePassEncoderSetBindGroup(p, 0, gi, 0, NULL);
  wgpuComputePassEncoderDispatchWorkgroups(p, (uint32_t)(g->w + 3) / 4, (uint32_t)(g->h + 3) / 4, (uint32_t)(g->d + 3) / 4);
  wgpuComputePassEncoderSetPipeline(p, p_int);
  wgpuComputePassEncoderSetBindGroup(p, 0, gn, 0, NULL);
  wgpuComputePassEncoderDispatchWorkgroups(p, (uint32_t)(g->w + 7) / 8, (uint32_t)(g->h + 7) / 8, 1);
  wgpuComputePassEncoderEnd(p);
  wgpuComputePassEncoderRelease(p);
  cw__pw_timing_span(enc, true);
}

unsigned cw__vf_wg_integrated(vf_wg *g) { return g ? g->integ_id : 0; }

void cw__vf_wg_free(vf_wg *g) {
  if (!g) return;
  drop_grid(g);
  drop_atlas(g);
  if (g->ubo) wgpuBufferDestroy(g->ubo), wgpuBufferRelease(g->ubo);
  cw_free(g);
}

#else

bool cw__vf_wg_resources(vf_wg **g, int w, int h, int d, int atlas) { (void)g, (void)w, (void)h, (void)d, (void)atlas; return true; }
void *cw__vf_wg_atlas_view(vf_wg *g, int *format) { (void)g, *format = 0; return NULL; }
void *cw__vf_wg_encoder(void) { return NULL; }
void cw__vf_wg_run(vf_wg *g, const vf_uniforms *u, int cur, void *encoder) { (void)g, (void)u, (void)cur, (void)encoder; }
unsigned cw__vf_wg_integrated(vf_wg *g) { (void)g; return 0; }
void cw__vf_wg_free(vf_wg *g) { (void)g; }

#endif
