/* Screen-space reflections (cw_post_settings.ssr): three.js r187's SSRNode,
 * its first-generation path (stochastic = false: one mirror ray a pixel,
 * roughness softens it), at half resolution, on GL and WebGPU.
 *
 *   1. G-buffer: the scene draws a second time into a half-resolution
 *      RGBA16F target with the gi shading part in its G-buffer mode
 *      (gi_shading.mjs: view normal octahedral in rg, roughness, metalness;
 *      r187's example takes them from MRT attachments). Only surfaces
 *      carrying the gi part draw; everything else is hidden for it, and its
 *      pixels stay metalness 0 (no reflection).
 *   2. Trace (half resolution): from each metallic pixel the reflection ray
 *      is marched across the screen against the full-resolution depth,
 *      r187's march: screen-space steps over the ray's length times
 *      `quality`, a hit where the ray passes behind the depth within
 *      `thickness` (or three texels), facing normals rejected, then attenuated
 *      by the distance to the reflecting plane over `max_distance` and r187's
 *      Fresnel term, weighted by metalness, held under r187's luminance cap.
 *   3. Composite: the scene colour plus the reflection (r187's example adds
 *      it), upsampled with depth-aware weights and softened by a ring of
 *      taps whose radius grows with roughness squared (r187 blurs a mip
 *      chain and picks the level by roughness squared).
 * The march ends with r187's binary refinement (8 halvings).
 *
 * r187 adds SSR over the lit image, which already holds the environment's
 * (or a probe's) reflection: the two add up, as they do in r187. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/quality.h"
#include "cartwheel/shading.h"
#include "gi_internal.h"
#include "render/pipeline_internal.h"
#if CW_WGPU
#include "render/pipeline_wgpu.h"
#endif

#define S(...) #__VA_ARGS__ "\n"
#include "render/pipeline_post_wgsl.h"

/* ── shaders ────────────────────────────────────────────────────────── */

static const char *TRACE_GLSL = S(
  uniform sampler2D u_depth;   /* full resolution, GL window depth */
  uniform sampler2D u_gbuf;    /* half resolution: oct normal (view), roughness, metalness */
  uniform sampler2D u_color;   /* the lit scene, full resolution */
  uniform mat4 u_proj;
  uniform mat4 u_inv_proj;
  uniform vec4 u_size;         /* full w, h; trace w, h */
  uniform vec4 u_params;       /* max distance, thickness, quality, intensity */
  uniform vec4 u_cam;          /* near, far, max steps, 0 */
  out vec4 o;
  vec3 view_pos(vec2 uv, float d) {
    vec4 p = u_inv_proj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
  }
  float view_z(float d) { return (u_cam.x * u_cam.y) / ((u_cam.y - u_cam.x) * d - u_cam.y); }
  float depth_at(vec2 uv) { return texelFetch(u_depth, clamp(ivec2(uv * u_size.xy), ivec2(0), ivec2(u_size.xy) - 1), 0).r; }
  vec3 oct_decode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
  }
  vec2 screen_px(vec3 p) {
    vec4 c = u_proj * vec4(p, 1.0);
    return (c.xy / c.w * 0.5 + 0.5) * u_size.zw;
  }
  float plane_dist(vec3 p, vec3 pp, vec3 pn) { return dot(pn, p) - dot(pn, pp); }
  float line_dist(vec3 p, vec3 a, vec3 b) { return length(cross(p - a, p - b)) / length(b - a); }
  void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    vec2 uv = (vec2(q) + 0.5) / u_size.zw;
    vec4 gb = texelFetch(u_gbuf, q, 0);
    float metal = gb.a;
    float d = depth_at(uv);
    o = vec4(0.0, 0.0, 0.0, gb.b);
    if (metal <= 0.0 || d >= 1.0) return;
    vec3 P = view_pos(uv, d);
    vec3 N = oct_decode(gb.rg);
    vec3 I = normalize(P);
    vec3 R = normalize(reflect(I, N));
    float maxd = u_params.x;
    vec3 P1 = P + R * (maxd / max(dot(-I, N), 1e-4));
    if (P1.z > -u_cam.x) P1 = P + R * ((-u_cam.x - P.z) / R.z);
    vec2 d0 = uv * u_size.zw;
    vec2 d1 = screen_px(P1);
    vec2 len = d1 - d0;
    int total = int(clamp(floor(max(abs(len.x), abs(len.y)) * clamp(u_params.z, 0.0, 1.0)), 1.0, u_cam.z));
    vec2 stepv = len / float(total);
    float rz0 = 1.0 / P.z, rz1 = 1.0 / P1.z;
    float texel_x = 1.0 / u_size.z;
    bool found = false;
    vec2 hit_uv = vec2(0.0);
    float hit_d = 1.0, s_lo = 0.0, s_hi = 0.0;
    for (int i = 1; i < 1024; i++) {
      if (i >= total) break;
      float s = float(i) / float(total);
      vec2 xy = d0 + stepv * float(i);
      if (xy.x < 0.0 || xy.y < 0.0 || xy.x > u_size.z || xy.y > u_size.w) break;
      vec2 uvs = xy / u_size.zw;
      float ds = depth_at(uvs);
      float vz = view_z(ds);
      float rayz = 1.0 / (rz0 + s * (rz1 - rz0));
      if (rayz <= vz) {
        vec3 vp = view_pos(uvs, ds);
        float away = line_dist(vp, P, P1);
        vec3 vpn = view_pos(uvs + vec2(texel_x, 0.0), ds);
        float tk = max((vpn.x - vp.x) * 3.0, u_params.y);
        if (away <= tk) {
          vec3 vn = oct_decode(texelFetch(u_gbuf, clamp(ivec2(uvs * u_size.zw), ivec2(0), ivec2(u_size.zw) - 1), 0).rg);
          if (dot(R, vn) >= 0.0) continue;
          if (plane_dist(vp, P, N) > maxd) break;
          found = true;
          s_lo = float(i - 1) / float(total);
          s_hi = s;
          break;
        }
      }
    }
    if (!found) return;
    /* r187's binary refinement between the last step in front and the hit */
    for (int k = 0; k < 8; k++) {
      float sm = (s_lo + s_hi) * 0.5;
      vec2 um = (d0 + len * sm) / u_size.zw;
      if (1.0 / (rz0 + sm * (rz1 - rz0)) <= view_z(depth_at(um))) s_hi = sm; else s_lo = sm;
    }
    hit_uv = (d0 + len * s_hi) / u_size.zw;
    hit_d = depth_at(hit_uv);
    vec3 vp = view_pos(hit_uv, hit_d);
    float dist = plane_dist(vp, P, N);
    if (dist > maxd) return;
    vec3 col = texture(u_color, hit_uv).rgb * metal;
    float ratio = 1.0 - dist / maxd;
    col *= ratio * ratio * (dot(I, R) + 1.0) * 0.5;
    float lum = max(dot(col, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    col *= min(10.0 / lum, 1.0) * u_params.w;
    o = vec4(max(col, vec3(0.0)), gb.b);
  });

/* the composite: each pixel takes the trace texels around it (the bilinear
 * four, and a ring whose radius grows with roughness squared) weighted by how
 * close their depth is to its own, so reflections do not bleed across
 * silhouettes from the half-resolution trace */
static const char *COMP_GLSL = S(
  uniform sampler2D u_src;
  uniform sampler2D u_ssr;
  uniform sampler2D u_depth;
  uniform vec4 u_size;   /* full w, h; trace w, h */
  uniform vec4 u_cam;    /* near, far */
  out vec4 o;
  float lin_at(ivec2 p) {
    float d = texelFetch(u_depth, clamp(p, ivec2(0), ivec2(u_size.xy) - 1), 0).r;
    return (u_cam.x * u_cam.y) / (u_cam.y - (u_cam.y - u_cam.x) * d);
  }
  void main() {
    vec4 c = texture(u_src, v_uv);
    ivec2 fp = ivec2(gl_FragCoord.xy);
    float z0 = lin_at(fp);
    vec2 hp = v_uv * u_size.zw - 0.5;
    vec2 base = floor(hp), fr = hp - base;
    vec3 sum = vec3(0.0);
    float wsum = 0.0, rough = 0.0;
    for (int j = 0; j < 2; j++)
      for (int i = 0; i < 2; i++) {
        ivec2 t = clamp(ivec2(base) + ivec2(i, j), ivec2(0), ivec2(u_size.zw) - 1);
        vec4 m = texelFetch(u_ssr, t, 0);
        float zt = lin_at(t * 2);
        float wb = (i == 0 ? 1.0 - fr.x : fr.x) * (j == 0 ? 1.0 - fr.y : fr.y);
        float dz = (zt - z0) / (0.03 * z0 + 1e-3);
        float w = wb * exp(-dz * dz) + 1e-5 * wb;
        sum += m.rgb * w;
        wsum += w;
        rough += m.a * w;
      }
    rough /= max(wsum, 1e-6);
    float rad = rough * rough * 8.0;
    if (rad > 0.5) {
      for (int k = 0; k < 8; k++) {
        float a = float(k) * 0.785398;
        ivec2 t = clamp(ivec2(floor(hp + vec2(cos(a), sin(a)) * rad + 0.5)), ivec2(0), ivec2(u_size.zw) - 1);
        float dz = (lin_at(t * 2) - z0) / (0.03 * z0 + 1e-3);
        float w = exp(-dz * dz) * 0.5;
        sum += texelFetch(u_ssr, t, 0).rgb * w;
        wsum += w;
      }
    }
    o = vec4(c.rgb + sum / max(wsum, 1e-6), c.a);
  });

#if CW_WGPU
static const char *TRACE_WGSL = S(
  struct U { u_proj: mat4x4f, u_inv_proj: mat4x4f, u_size: vec4f, u_params: vec4f, u_cam: vec4f, u_pz: vec4f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_depth: texture_2d<f32>;
  @group(0) @binding(2) var u_gbuf: texture_2d<f32>;
  @group(0) @binding(3) var u_color: texture_2d<f32>;
  fn view_pos(uv: vec2f, d: f32) -> vec3f {
    let p = u.u_inv_proj * vec4f(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
  }
  fn view_z(d: f32) -> f32 { return (u.u_cam.x * u.u_cam.y) / ((u.u_cam.y - u.u_cam.x) * d - u.u_cam.y); }
  fn depth_at(uv: vec2f) -> f32 {
    let q = clamp(vec2i(uv * u.u_size.xy), vec2i(0), vec2i(u.u_size.xy) - 1);
    return win_depth(textureLoad(u_depth, q, 0).r, u.u_pz);
  }
  fn oct_decode(e: vec2f) -> vec3f {
    var n = vec3f(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
      let sx = select(-1.0, 1.0, n.x >= 0.0);
      let sy = select(-1.0, 1.0, n.y >= 0.0);
      let ax = (1.0 - abs(n.y)) * sx;
      let ay = (1.0 - abs(n.x)) * sy;
      n = vec3f(ax, ay, n.z);
    }
    return normalize(n);
  }
  fn screen_px(p: vec3f) -> vec2f {
    let c = u.u_proj * vec4f(p, 1.0);
    return (c.xy / c.w * 0.5 + 0.5) * u.u_size.zw;
  }
  fn plane_dist(p: vec3f, pp: vec3f, pn: vec3f) -> f32 { return dot(pn, p) - dot(pn, pp); }
  fn line_dist(p: vec3f, a: vec3f, b: vec3f) -> f32 { return length(cross(p - a, p - b)) / length(b - a); }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv0: vec2f) -> @location(0) vec4f {
    let q = vec2i(cw_frag_coord(pos));
    let uv = (vec2f(q) + 0.5) / u.u_size.zw;
    let gb = textureLoad(u_gbuf, q, 0);
    let metal = gb.a;
    let d = depth_at(uv);
    if (metal <= 0.0 || d >= 1.0) { return vec4f(0.0, 0.0, 0.0, gb.b); }
    let P = view_pos(uv, d);
    let N = oct_decode(gb.rg);
    let I = normalize(P);
    let R = normalize(reflect(I, N));
    let maxd = u.u_params.x;
    var P1 = P + R * (maxd / max(dot(-I, N), 1e-4));
    if (P1.z > -u.u_cam.x) { P1 = P + R * ((-u.u_cam.x - P.z) / R.z); }
    let d0 = uv * u.u_size.zw;
    let d1 = screen_px(P1);
    let len = d1 - d0;
    let total = i32(clamp(floor(max(abs(len.x), abs(len.y)) * clamp(u.u_params.z, 0.0, 1.0)), 1.0, u.u_cam.z));
    let stepv = len / f32(total);
    let rz0 = 1.0 / P.z;
    let rz1 = 1.0 / P1.z;
    let texel_x = 1.0 / u.u_size.z;
    var found = false;
    var hit_uv = vec2f(0.0);
    var hit_d = 1.0;
    var s_lo = 0.0;
    var s_hi = 0.0;
    for (var i = 1; i < 1024; i++) {
      if (i >= total) { break; }
      let s = f32(i) / f32(total);
      let xy = d0 + stepv * f32(i);
      if (xy.x < 0.0 || xy.y < 0.0 || xy.x > u.u_size.z || xy.y > u.u_size.w) { break; }
      let uvs = xy / u.u_size.zw;
      let ds = depth_at(uvs);
      let vz = view_z(ds);
      let rayz = 1.0 / (rz0 + s * (rz1 - rz0));
      if (rayz <= vz) {
        let vp = view_pos(uvs, ds);
        let away = line_dist(vp, P, P1);
        let vpn = view_pos(uvs + vec2f(texel_x, 0.0), ds);
        let tk = max((vpn.x - vp.x) * 3.0, u.u_params.y);
        if (away <= tk) {
          let gq = clamp(vec2i(uvs * u.u_size.zw), vec2i(0), vec2i(u.u_size.zw) - 1);
          let vn = oct_decode(textureLoad(u_gbuf, gq, 0).rg);
          if (dot(R, vn) >= 0.0) { continue; }
          if (plane_dist(vp, P, N) > maxd) { break; }
          found = true;
          s_lo = f32(i - 1) / f32(total);
          s_hi = s;
          break;
        }
      }
    }
    if (!found) { return vec4f(0.0, 0.0, 0.0, gb.b); }
    for (var k = 0; k < 8; k++) {
      let sm = (s_lo + s_hi) * 0.5;
      let um = (d0 + len * sm) / u.u_size.zw;
      if (1.0 / (rz0 + sm * (rz1 - rz0)) <= view_z(depth_at(um))) { s_hi = sm; } else { s_lo = sm; }
    }
    hit_uv = (d0 + len * s_hi) / u.u_size.zw;
    hit_d = depth_at(hit_uv);
    let vp = view_pos(hit_uv, hit_d);
    let dist = plane_dist(vp, P, N);
    if (dist > maxd) { return vec4f(0.0, 0.0, 0.0, gb.b); }
    var col = textureSampleLevel(u_color, cw_linear, hit_uv, 0.0).rgb * metal;
    let ratio = 1.0 - dist / maxd;
    col = col * (ratio * ratio * (dot(I, R) + 1.0) * 0.5);
    let lum = max(dot(col, vec3f(0.2126, 0.7152, 0.0722)), 1e-4);
    col = col * (min(10.0 / lum, 1.0) * u.u_params.w);
    return vec4f(max(col, vec3f(0.0)), gb.b);
  });

static const char *COMP_WGSL = S(
  struct U { u_size: vec4f, u_cam: vec4f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @group(0) @binding(2) var u_ssr: texture_2d<f32>;
  @group(0) @binding(3) var u_depth: texture_2d<f32>;
  /* the R32F linear depth: view distance (3e38 where nothing was drawn) */
  fn lin_at(p: vec2i) -> f32 { return textureLoad(u_depth, clamp(p, vec2i(0), vec2i(u.u_size.xy) - 1), 0).r; }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let c = textureSampleLevel(u_src, cw_linear, uv, 0.0);
    let fp = vec2i(cw_frag_coord(pos));
    let z0 = lin_at(fp);
    let hp = uv * u.u_size.zw - 0.5;
    let base = floor(hp);
    let fr = hp - base;
    var sum = vec3f(0.0);
    var wsum = 0.0;
    var rough = 0.0;
    for (var j = 0; j < 2; j++) {
      for (var i = 0; i < 2; i++) {
        let t = clamp(vec2i(base) + vec2i(i, j), vec2i(0), vec2i(u.u_size.zw) - 1);
        let m = textureLoad(u_ssr, t, 0);
        let zt = lin_at(t * 2);
        let wb = select(fr.x, 1.0 - fr.x, i == 0) * select(fr.y, 1.0 - fr.y, j == 0);
        let dz = (zt - z0) / (0.03 * z0 + 1e-3);
        let w = wb * exp(-dz * dz) + 1e-5 * wb;
        sum += m.rgb * w;
        wsum += w;
        rough += m.a * w;
      }
    }
    rough = rough / max(wsum, 1e-6);
    let rad = rough * rough * 8.0;
    if (rad > 0.5) {
      for (var k = 0; k < 8; k++) {
        let a = f32(k) * 0.785398;
        let t = clamp(vec2i(floor(hp + vec2f(cos(a), sin(a)) * rad + 0.5)), vec2i(0), vec2i(u.u_size.zw) - 1);
        let dz = (lin_at(t * 2) - z0) / (0.03 * z0 + 1e-3);
        let w = exp(-dz * dz) * 0.5;
        sum += textureLoad(u_ssr, t, 0).rgb * w;
        wsum += w;
      }
    }
    return vec4f(c.rgb + sum / max(wsum, 1e-6), c.a);
  });
#endif

/* ── the G-buffer draw ──────────────────────────────────────────────── */

/* hide every drawable whose materials do not all carry the gi part (or that
 * blend), for the G-buffer draw; restored after */
typedef struct gb_hidden { t3_object *o; } gb_hidden;
typedef CW_ARRAY(gb_hidden) gb_list;
static void gb_walk(t3_object *o, gb_list *out) {
  bool drawable = o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH || o->type == T3_SPRITE ||
                  o->type == T3_POINTS || o->type == T3_LINE || o->type == T3_LINE_SEGMENTS || o->type == T3_LINE_LOOP;
  if (drawable && o->visible) {
    bool keep = o->type == T3_MESH || o->type == T3_SKINNED_MESH || o->type == T3_INSTANCED_MESH;
    if (keep) {
      const t3_mesh *m = (const t3_mesh *)o;
      for (int i = 0; i < m->material_count && keep; i++)
        keep = m->materials[i] && cw_shading_has(m->materials[i], "gi") && !m->materials[i]->transparent;
    }
    if (!keep) {
      o->visible = false;
      cw_array_push(out, (gb_hidden){ o });
      return;
    }
  }
  for (int i = 0; i < o->child_count; i++) gb_walk(o->children[i], out);
}

bool cw__gi_gbuffer(cw_pass_ctx *ctx, gi_tree *g, int w, int h, int mode) {
  t3_renderer *r = ctx->renderer;
  cw_rt *rt = mode == 2 ? &g->albedo : &g->gbuf;
  if (!rt->fbo || rt->w != w || rt->h != h) {
    cw__rt_destroy(rt);
    if (!cw__rt_create(rt, w, h, CW_RT_RGBA16F, true)) return false;
  }
  if (g->hook_renderer != r) {
    cw_shading_part(r, "gi", cw__gi_fill, g);
    g->hook_renderer = r;
  }
  if (g->gbuf_hook != r) {
    t3_renderer_set_extension(r, "gibuf", cw__gi_fill_gbuf, g);
    g->gbuf_hook = r;
  }
  cw__gi_tag(g, &ctx->scene3d->base);
  gb_list hidden = { 0 };
  gb_walk(&ctx->scene3d->base, &hidden);
  /* every material of the G-buffer draws the lighting-free "gibuf" program (gibuf.mjs); their own
   * extensions are put back after */
  char (*saved)[32] = g->mats.count ? cw_alloc((size_t)g->mats.count * 32, CW_MEM_RENDER) : NULL;
  for (int i = 0; i < g->mats.count; i++) {
    const char *e = t3_material_get_extension(g->mats.data[i]);
    snprintf(saved[i], 32, "%s", e ? e : "");
    t3_material_set_extension(g->mats.data[i], "gibuf");
  }
  t3_scene *sc = ctx->scene3d;
  t3_texture *bg = sc->background_texture;
  bool had_bg = sc->has_background;
  if (bg) t3_retain(bg), t3_scene_set_background_texture(sc, NULL);
  sc->has_background = false;
  g->mode = mode;
  bool ok = true;
  /* the G-buffer target, cleared to 0 (metalness 0: no reflection) and depth 1 */
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    /* (the SHADOW phase: three.c records on command buffers of its own, as
     * the probe captures do; rows bottom-up, as every offscreen target) */
    t3_external_target et = { .samples = 1, .load_color = false, .load_depth = false, .clear_rgba = { 0, 0, 0, 0 }, .clear_depth = cw_gpu_reversed_depth() ? 0.0f : 1.0f,
                              .x = 0, .y = 0, .w = w, .h = h, .flip_y = true };
    et.color = (uintptr_t)cw__pw_attach(rt->tex), et.depth = (uintptr_t)cw__pw_attach(rt->depth_tex);
    et.color_format = (int)cw__pw_format(rt->tex), et.depth_format = (int)cw__pw_format(rt->depth_tex);
    t3_renderer_set_external_encoder(r, 0);
    t3_renderer_set_external_target(r, &et);
    t3_renderer_render(r, sc, ctx->camera);
    t3_renderer_set_external_target(r, NULL);
  } else
#endif
  {
    /* GL: the target's framebuffer, cleared here, and the renderer's own
     * viewport over its bottom-left w x h (three.c measures it from the top
     * of its drawing buffer). (An external target would do, but going back
     * from one with NULL leaves three.c 0.186.13 believing it still draws
     * into it.) */
    cw_pass_bind_target(ctx, rt);
    cw_pass_clear(0, 0, 0, 0, true);
    int bw, bh, vx, vy, vw, vh, sx, sy, sw, sh;
    t3_renderer_get_size(r, &bw, &bh);
    t3_renderer_get_viewport(r, &vx, &vy, &vw, &vh);
    bool st = t3_renderer_get_scissor(r, &sx, &sy, &sw, &sh);
    bool auto_clear = t3_renderer_get_auto_clear(r);
    t3_renderer_reset_bindings(r);
    t3_renderer_set_framebuffer(r, rt->fbo);
    t3_renderer_set_viewport(r, 0, bh - h, w, h);
    t3_renderer_set_scissor(r, 0, bh - h, w, h);
    t3_renderer_set_scissor_test(r, true);
    t3_renderer_set_auto_clear(r, false);
    t3_renderer_render(r, sc, ctx->camera);
    t3_renderer_set_auto_clear(r, auto_clear);
    t3_renderer_set_viewport(r, vx, vy, vw, vh);
    t3_renderer_set_scissor(r, sx, sy, sw, sh);
    t3_renderer_set_scissor_test(r, st);
    t3_renderer_set_framebuffer(r, ctx->pipeline->bound_fbo);
    t3_renderer_reset_bindings(r);
  }
  g->mode = 0;
  for (int i = 0; i < g->mats.count; i++) t3_material_set_extension(g->mats.data[i], saved[i][0] ? saved[i] : NULL);
  cw_free(saved);
  sc->has_background = had_bg;
  if (bg) t3_scene_set_background_texture(sc, bg), t3_release(bg);
  for (int i = 0; i < hidden.count; i++) hidden.data[i].o->visible = true;
  cw_array_free(&hidden);
  return ok;
}

/* ── the pass ───────────────────────────────────────────────────────── */

static unsigned trace_prog, comp_prog;

static bool ssr_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->ssr && ctx->renderer && ctx->scene3d;
}

static void ssr_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  if (!trace_prog) {
#if CW_WGPU
    if (wg) {
      static char buf[16384];
      snprintf(buf, sizeof buf, "%s%s", WGSL_COMMON, TRACE_WGSL);
      trace_prog = cw_pass_program_wgsl("ssr.trace", buf);
      comp_prog = cw_pass_program_wgsl("ssr.composite", COMP_WGSL);
    } else
#endif
    {
      trace_prog = cw_pass_program("ssr.trace", TRACE_GLSL);
      comp_prog = cw_pass_program("ssr.composite", COMP_GLSL);
    }
  }
  if (!trace_prog || !comp_prog || !ctx->depth_tex) return;
  gi_tree *g = cw__gi_tree(ctx->tree);
  int hw = (ctx->width + 1) / 2, hh = (ctx->height + 1) / 2;
  if (!g->gbuf_ok || g->gbuf.w != hw || g->gbuf.h != hh) return; /* (drawn by the gi pass, reflection_probe.c) */
  const cw_rt *tr = cw_pipeline_rt_persistent(ctx->pipeline, "ssr.trace", hw, hh, CW_RT_RGBA16F, NULL);
  if (!tr) return;
  const t3_camera *cam = ctx->camera;
  cw_pass_bind_target(ctx, tr);
  cw_pass_use(trace_prog);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_bind_texture(1, g->gbuf.tex);
  cw_pass_bind_texture(2, ctx->src->tex);
  if (!wg) {
    cw_pass_uniform1i(trace_prog, "u_depth", 0);
    cw_pass_uniform1i(trace_prog, "u_gbuf", 1);
    cw_pass_uniform1i(trace_prog, "u_color", 2);
  }
  cw_pass_uniform_mat4(trace_prog, "u_proj", &cam->projection_matrix);
  cw_pass_uniform_mat4(trace_prog, "u_inv_proj", &ctx->pipeline->inv_proj_jittered);
  cw_pass_uniform4f(trace_prog, "u_size", (float)ctx->width, (float)ctx->height, (float)hw, (float)hh);
  cw_pass_uniform4f(trace_prog, "u_params", s->ssr_max_distance, s->ssr_thickness, s->ssr_quality, s->ssr_intensity);
  cw_pass_uniform4f(trace_prog, "u_cam", cam->near, cam->far, 512, 0);
  {
    const float *e = cam->projection_matrix.e;
    cw_pass_uniform4f(trace_prog, "u_pz", e[10], e[14], e[11], e[15]);
  }
  cw_pass_fullscreen();
  cw_pass_bind_target(ctx, ctx->dst);
  cw_pass_use(comp_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, tr->tex);
  cw_pass_bind_texture(2, ctx->depth_tex);
  if (!wg) {
    cw_pass_uniform1i(comp_prog, "u_src", 0);
    cw_pass_uniform1i(comp_prog, "u_ssr", 1);
    cw_pass_uniform1i(comp_prog, "u_depth", 2);
  }
  cw_pass_uniform4f(comp_prog, "u_size", (float)ctx->width, (float)ctx->height, (float)hw, (float)hh);
  cw_pass_uniform4f(comp_prog, "u_cam", cam->near, cam->far, 0, 0);
  cw_pass_fullscreen();
}

void cw__gi_add_post_passes(cw_pipeline *p) {
  cw__gi_add_shadow_pass(p);
  cw__gi_add_ssgi_pass(p);
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "ssr", .phase = CW_PHASE_POST, .order = 150,
                                           .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = ssr_active, .execute = ssr_exec, .webgpu = true });
}
