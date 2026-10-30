/* Screen-space bounce light (cw_post_settings.ssgi): three.js r187's
 * SSGINode (SSILVB, a port of SSRT3: horizon slices whose occluded sectors
 * are tracked in a 32-bit field, each newly occluded sector taking the
 * light of the sample that occludes it), at half resolution, on GL and
 * WebGPU.
 *
 *   1. The GI G-buffer (ssr.c) gives view normals; a second G-buffer draw
 *      gives the diffuse colour (r187's example takes both from MRT).
 *   2. Trace (half resolution): r187's slices and steps over the full
 *      resolution depth and the lit scene (the frame before post, read from a
 *      half-resolution copy: a quarter of the bytes at the scattered taps of
 *      the trace, which is where its time goes), giving bounce light (rgb) and
 *      ambient occlusion (a). The slice and step counts are compile-time
 *      constants of the trace program (one per pair in use), and the normals
 *      come from a two-channel copy of the G-buffer.
 *   3. Denoise (half resolution) over 5 x 5 trace texels with depth-,
 *      normal- and brightness-aware weights, then composite as r187's
 *      example does, colour x AO + diffuse colour x GI, upsampled with
 *      depth-aware weights (r187 leans on TRAA for that; with the
 *      pipeline's TAA on, the slice rotation and step offset also follow
 *      r187's temporal sequence).
 *
 * Surfaces without the GI shading part (cartwheel/gi.h) have no G-buffer
 * entry and are left as they are. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "gi_internal.h"
#include "render/pipeline_internal.h"
#if CW_WGPU
#include "render/pipeline_wgpu.h"
#endif

#define S(...) #__VA_ARGS__ "\n"
#include "render/pipeline_post_wgsl.h"

static const char *TRACE_GLSL =
    "#define PI 3.14159265358979\n#define HALF_PI 1.5707963267949\n#define MAX_RAY 32u\n"
    S(uniform sampler2D u_depth;
      uniform sampler2D u_gbuf;
      uniform sampler2D u_alb;
      uniform sampler2D u_color;
      uniform mat4 u_inv_proj;
      uniform vec4 u_size;     /* full w, h; trace w, h */
      uniform vec4 u_params;   /* radius, gi intensity, ao intensity, thickness */
      uniform vec4 u_counts;   /* slices, steps, temporal direction, temporal offset */
      out vec4 o;
      uint bitcount(uint v) {
        v = v - ((v >> 1u) & 0x55555555u);
        v = (v & 0x33333333u) + ((v >> 2u) & 0x33333333u);
        return (((v + (v >> 4u)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24u;
      }
      float depth_at(vec2 uv) { return texelFetch(u_depth, clamp(ivec2(uv * u_size.xy), ivec2(0), ivec2(u_size.xy) - 1), 0).r; }
      vec3 view_pos(vec2 uv, float d) {
        vec4 p = u_inv_proj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
        return p.xyz / p.w;
      }
      vec3 oct_decode(vec2 e) {
        vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
        if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        return normalize(n);
      }
      vec3 normal_at(vec2 uv) { return oct_decode(texelFetch(u_gbuf, clamp(ivec2(uv * u_size.zw), ivec2(0), ivec2(u_size.zw) - 1), 0).rg); }
      vec2 fast_acos(vec2 v) {
        vec2 r = abs(v) * -0.156583 + HALF_PI;
        r *= sqrt(1.0 - abs(v));
        return vec2(v.x >= 0.0 ? r.x : PI - r.x, v.y >= 0.0 ? r.y : PI - r.y);
      }
      float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
      uint occluded;
      vec3 horizon(bool right, float step_r, float radius_vs, vec3 P, vec2 slide, float init_step, vec2 uv, vec3 V, vec3 N, float n) {
        /* r187 flips y here (its uv runs top-down); ours runs bottom-up, as SSRT3's */
        vec2 uvdir = right ? vec2(1.0, 1.0) : vec2(-1.0, -1.0);
        float sdir = right ? 1.0 : -1.0;
        vec3 color = vec3(0.0);
        for (int i = 0; i < STEPS; i++) {
          float off = pow(abs(step_r * (float(i) + init_step) / radius_vs), 2.0) * radius_vs;
          vec2 uvo = slide * max(off, float(i) + 1.0);
          vec2 suv = uv + uvo * uvdir;
          if (suv.x <= 0.0 || suv.y <= 0.0 || suv.x >= 1.0 || suv.y >= 1.0) break;
          vec3 SP = view_pos(suv, depth_at(suv));
          vec3 to = normalize(SP - P);
          vec3 to_back = normalize(SP - V * u_params.w - P);
          vec2 fb = fast_acos(clamp(vec2(dot(to, V), dot(to_back, V)), -1.0, 1.0));
          fb = clamp((sdir * -fb - (n - HALF_PI)) / PI, 0.0, 1.0);
          fb = right ? fb.yx : fb.xy;
          uint start = uint(fb.x * float(MAX_RAY));
          uint ang = uint(ceil((fb.y - fb.x) * float(MAX_RAY)));
          uint field = ang > 0u ? (0xFFFFFFFFu >> (32u - min(ang, 32u))) : 0u;
          uint cur = start < 32u ? (field << start) : 0u;
          cur &= ~occluded;
          occluded |= cur;
          uint zones = bitcount(cur);
          if (zones > 0u) {
            vec3 light = textureLod(u_color, suv, 0.0).rgb;
            if (luma(light) > 0.001) {
              float ndl = clamp(dot(N, to), 0.0, 1.0);
              if (ndl > 0.001) {
                vec3 LN = normal_at(suv);
                float lndl = clamp(dot(LN, -to), 0.0, 1.0);
                color += float(zones) / float(MAX_RAY) * light * ndl * lndl;
              }
            }
          }
        }
        return color;
      }
      void main() {
        ivec2 q = ivec2(gl_FragCoord.xy);
        vec2 uv = (vec2(q) + 0.5) / u_size.zw;
        float d = depth_at(uv);
        float valid = texelFetch(u_alb, q, 0).a;
        o = vec4(0.0, 0.0, 0.0, 1.0);
        if (d >= 1.0 || valid < 0.5) return;
        vec3 P = view_pos(uv, d);
        vec3 N = normal_at(uv);
        vec3 V = normalize(-P);
        float spatial = 0.25 * float((q.y - q.x) & 3);
        float dir_noise = fract(52.9829189 * fract(dot(vec2(q), vec2(0.06711056, 0.00583715))));
        float init_step = fract(spatial + u_counts.w) + fract(sin(dot((uv + u_counts.z * 0.02) * 2.0 - 1.0, vec2(12.9898, 78.233))) * 43758.5453);
        const int slices = SLICES;
        float step_r = u_params.x * (u_size.z / 2.0) / 16.0;
        step_r /= u_counts.y + 1.0;
        float radius_vs = max(1.0, u_counts.y - 1.0) * step_r;
        float ao = 0.0;
        vec3 color = vec3(0.0);
        for (int s = 0; s < SLICES; s++) {
          float a = (float(s) + dir_noise + u_counts.z) * (PI / float(slices));
          vec3 sd = vec3(cos(a), sin(a), 0.0);
          vec2 slide = sd.xy / u_size.zw;
          vec3 pn = normalize(cross(sd, V));
          vec3 tg = cross(V, pn);
          vec3 proj = N - pn * dot(N, pn);
          float cn = clamp(dot(normalize(proj), V), -1.0, 1.0);
          float n = -sign(dot(proj, tg)) * acos(cn);
          occluded = 0u;
          color += horizon(true, step_r, radius_vs, P, slide, init_step, uv, V, N, n);
          color += horizon(false, step_r, radius_vs, P, slide, init_step, uv, V, N, n);
          ao += float(bitcount(occluded)) / float(MAX_RAY);
        }
        ao /= float(slices);
        ao = clamp(pow(clamp(1.0 - ao, 0.0, 1.0), u_params.z), 0.0, 1.0);
        color = color / float(slices) * u_params.y;
        float l = luma(color);
        if (l > 7.0) color *= 7.0 / l;
        o = vec4(color, ao);
      });

/* the denoise, at the trace's resolution: 5 x 5 trace texels, weighted by
 * depth, normal and (Karis-style) brightness, so lone bright samples do not
 * sparkle */
/* the lit colour at half resolution (each texel the mean of its 2 x 2 pixels, one bilinear
 * tap at their shared corner): the trace reads it at scattered positions, and a quarter of
 * the bytes at that scatter is what its light fetches cost */
static const char *DOWN_GLSL = S(
  uniform sampler2D u_src;
  out vec4 o;
  void main() { o = vec4(texture(u_src, v_uv).rgb, 1.0); });

/* the G-buffer's normals alone, in a two-channel half-float target (4 bytes a texel, the same
 * values): the trace and the denoise read them at scattered positions */
static const char *NORM_GLSL = S(
  uniform sampler2D u_src;
  out vec4 o;
  void main() { o = vec4(texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).rg, 0.0, 1.0); });

static const char *DENOISE_GLSL = S(
  uniform sampler2D u_gi;
  uniform sampler2D u_gbuf;
  uniform sampler2D u_depth;
  uniform vec4 u_size;   /* full w, h; trace w, h */
  uniform vec4 u_cam;    /* near, far */
  out vec4 o;
  float lin_at(ivec2 p) {
    float d = texelFetch(u_depth, clamp(p, ivec2(0), ivec2(u_size.xy) - 1), 0).r;
    return (u_cam.x * u_cam.y) / (u_cam.y - (u_cam.y - u_cam.x) * d);
  }
  void main() {
    ivec2 b = ivec2(gl_FragCoord.xy);
    float z0 = lin_at(b * 2);
    vec2 n0 = texelFetch(u_gbuf, b, 0).rg;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int j = -2; j <= 2; j++)
      for (int i = -2; i <= 2; i++) {
        ivec2 t = clamp(b + ivec2(i, j), ivec2(0), ivec2(u_size.zw) - 1);
        float dz = (lin_at(t * 2) - z0) / (0.03 * z0 + 1e-3);
        vec2 dn = texelFetch(u_gbuf, t, 0).rg - n0;
        vec4 gt = texelFetch(u_gi, t, 0);
        float w = exp(-dz * dz - dot(dn, dn) * 8.0 - float(i * i + j * j) * 0.15) / (1.0 + dot(gt.rgb, vec3(0.2126, 0.7152, 0.0722)));
        sum += gt * w;
        wsum += w;
      }
    o = wsum > 1e-6 ? sum / wsum : vec4(0.0, 0.0, 0.0, 1.0);
  });

/* the composite, full resolution: the denoised trace's four texels around the
 * pixel, weighted by depth (no bleeding across silhouettes) */
static const char *COMP_GLSL = S(
  uniform sampler2D u_src;
  uniform sampler2D u_gi;
  uniform sampler2D u_alb;
  uniform sampler2D u_depth;
  uniform vec4 u_size;
  uniform vec4 u_cam;
  out vec4 o;
  float lin_at(ivec2 p) {
    float d = texelFetch(u_depth, clamp(p, ivec2(0), ivec2(u_size.xy) - 1), 0).r;
    return (u_cam.x * u_cam.y) / (u_cam.y - (u_cam.y - u_cam.x) * d);
  }
  void main() {
    vec4 c = texture(u_src, v_uv);
    vec2 hp = v_uv * u_size.zw - 0.5;
    vec2 base = floor(hp), fr = hp - base;
    vec4 alb = texelFetch(u_alb, clamp(ivec2(floor(hp + 0.5)), ivec2(0), ivec2(u_size.zw) - 1), 0);
    if (alb.a < 0.5) { o = c; return; }
    float z0 = lin_at(ivec2(gl_FragCoord.xy));
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int j = 0; j < 2; j++)
      for (int i = 0; i < 2; i++) {
        ivec2 t = clamp(ivec2(base) + ivec2(i, j), ivec2(0), ivec2(u_size.zw) - 1);
        float dz = (lin_at(t * 2) - z0) / (0.03 * z0 + 1e-3);
        float w = (i == 0 ? 1.0 - fr.x : fr.x) * (j == 0 ? 1.0 - fr.y : fr.y) * exp(-dz * dz) + 1e-5;
        sum += texelFetch(u_gi, t, 0) * w;
        wsum += w;
      }
    vec4 g = sum / wsum;
    o = vec4(c.rgb * g.a + alb.rgb * g.rgb, c.a);
  });

#if CW_WGPU
static const char *TRACE_WGSL =
    "const MAX_RAY = 32u;\n"
    S(struct U { u_inv_proj: mat4x4f, u_size: vec4f, u_params: vec4f, u_counts: vec4f, u_pz: vec4f };
      @group(0) @binding(0) var<uniform> u: U;
      @group(0) @binding(1) var u_depth: texture_2d<f32>;
      @group(0) @binding(2) var u_gbuf: texture_2d<f32>;
      @group(0) @binding(3) var u_alb: texture_2d<f32>;
      @group(0) @binding(4) var u_color: texture_2d<f32>;
      var<private> occluded: u32;
      fn depth_at(uv: vec2f) -> f32 {
        let q = clamp(vec2i(uv * u.u_size.xy), vec2i(0), vec2i(u.u_size.xy) - 1);
        return win_depth(textureLoad(u_depth, q, 0).r, u.u_pz);
      }
      fn view_pos(uv: vec2f, d: f32) -> vec3f {
        let p = u.u_inv_proj * vec4f(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
        return p.xyz / p.w;
      }
      fn oct_decode(e: vec2f) -> vec3f {
        var n = vec3f(e, 1.0 - abs(e.x) - abs(e.y));
        if (n.z < 0.0) {
          let ax = (1.0 - abs(n.y)) * select(-1.0, 1.0, n.x >= 0.0);
          let ay = (1.0 - abs(n.x)) * select(-1.0, 1.0, n.y >= 0.0);
          n = vec3f(ax, ay, n.z);
        }
        return normalize(n);
      }
      fn normal_at(uv: vec2f) -> vec3f {
        let q = clamp(vec2i(uv * u.u_size.zw), vec2i(0), vec2i(u.u_size.zw) - 1);
        return oct_decode(textureLoad(u_gbuf, q, 0).rg);
      }
      fn fast_acos(v: vec2f) -> vec2f {
        var r = abs(v) * -0.156583 + HALF_PI;
        r = r * sqrt(1.0 - abs(v));
        return vec2f(select(PI - r.x, r.x, v.x >= 0.0), select(PI - r.y, r.y, v.y >= 0.0));
      }
      fn horizon(right: bool, step_r: f32, radius_vs: f32, P: vec3f, slide: vec2f, init_step: f32, uv: vec2f, V: vec3f, N: vec3f, n: f32) -> vec3f {
        let uvdir = select(vec2f(-1.0, -1.0), vec2f(1.0, 1.0), right);
        let sdir = select(-1.0, 1.0, right);
        var color = vec3f(0.0);
        for (var i = 0; i < STEPS; i++) {
          let off = pow(abs(step_r * (f32(i) + init_step) / radius_vs), 2.0) * radius_vs;
          let uvo = slide * max(off, f32(i) + 1.0);
          let suv = uv + uvo * uvdir;
          if (suv.x <= 0.0 || suv.y <= 0.0 || suv.x >= 1.0 || suv.y >= 1.0) { break; }
          let SP = view_pos(suv, depth_at(suv));
          let to = normalize(SP - P);
          let to_back = normalize(SP - V * u.u_params.w - P);
          var fb = fast_acos(clamp(vec2f(dot(to, V), dot(to_back, V)), vec2f(-1.0), vec2f(1.0)));
          fb = clamp((sdir * -fb - (n - HALF_PI)) / PI, vec2f(0.0), vec2f(1.0));
          if (right) { fb = fb.yx; }
          let start = u32(fb.x * f32(MAX_RAY));
          let ang = u32(ceil((fb.y - fb.x) * f32(MAX_RAY)));
          var field = 0u;
          if (ang > 0u) { field = 0xFFFFFFFFu >> (32u - min(ang, 32u)); }
          var cur = 0u;
          if (start < 32u) { cur = field << start; }
          cur = cur & ~occluded;
          occluded = occluded | cur;
          let zones = countOneBits(cur);
          if (zones > 0u) {
            let light = textureSampleLevel(u_color, cw_linear, suv, 0.0).rgb;
            if (luma(light) > 0.001) {
              let ndl = clamp(dot(N, to), 0.0, 1.0);
              if (ndl > 0.001) {
                let LN = normal_at(suv);
                let lndl = clamp(dot(LN, -to), 0.0, 1.0);
                color += f32(zones) / f32(MAX_RAY) * light * ndl * lndl;
              }
            }
          }
        }
        return color;
      }
      @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv0: vec2f) -> @location(0) vec4f {
        let q = vec2i(cw_frag_coord(pos));
        let uv = (vec2f(q) + 0.5) / u.u_size.zw;
        let d = depth_at(uv);
        let valid = textureLoad(u_alb, q, 0).a;
        if (d >= 1.0 || valid < 0.5) { return vec4f(0.0, 0.0, 0.0, 1.0); }
        let P = view_pos(uv, d);
        let N = normal_at(uv);
        let V = normalize(-P);
        let spatial = 0.25 * f32((q.y - q.x) & 3);
        let dir_noise = fract(52.9829189 * fract(dot(vec2f(q), vec2f(0.06711056, 0.00583715))));
        let init_step = fract(spatial + u.u_counts.w) + fract(sin(dot((uv + u.u_counts.z * 0.02) * 2.0 - 1.0, vec2f(12.9898, 78.233))) * 43758.5453);
        let slices = SLICES;
        var step_r = u.u_params.x * (u.u_size.z / 2.0) / 16.0;
        step_r = step_r / (u.u_counts.y + 1.0);
        let radius_vs = max(1.0, u.u_counts.y - 1.0) * step_r;
        var ao = 0.0;
        var color = vec3f(0.0);
        for (var s = 0; s < SLICES; s++) {
          let a = (f32(s) + dir_noise + u.u_counts.z) * (PI / f32(slices));
          let sd = vec3f(cos(a), sin(a), 0.0);
          let slide = sd.xy / u.u_size.zw;
          let pn = normalize(cross(sd, V));
          let tg = cross(V, pn);
          let proj = N - pn * dot(N, pn);
          let cn = clamp(dot(normalize(proj), V), -1.0, 1.0);
          let n = -sign(dot(proj, tg)) * acos(cn);
          occluded = 0u;
          color += horizon(true, step_r, radius_vs, P, slide, init_step, uv, V, N, n);
          color += horizon(false, step_r, radius_vs, P, slide, init_step, uv, V, N, n);
          ao += f32(countOneBits(occluded)) / f32(MAX_RAY);
        }
        ao = ao / f32(slices);
        ao = clamp(pow(clamp(1.0 - ao, 0.0, 1.0), u.u_params.z), 0.0, 1.0);
        color = color / f32(slices) * u.u_params.y;
        let l = luma(color);
        if (l > 7.0) { color = color * (7.0 / l); }
        return vec4f(color, ao);
      });

static const char *DOWN_WGSL = S(
  struct U { u_pad: vec4f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    return vec4f(textureSampleLevel(u_src, cw_linear, uv, 0.0).rgb, 1.0);
  });

static const char *NORM_WGSL = S(
  struct U { u_pad: vec4f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    return vec4f(textureLoad(u_src, vec2i(cw_frag_coord(pos)), 0).rg, 0.0, 1.0);
  });

static const char *DENOISE_WGSL = S(
  struct U { u_size: vec4f, u_cam: vec4f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_gi: texture_2d<f32>;
  @group(0) @binding(2) var u_gbuf: texture_2d<f32>;
  @group(0) @binding(3) var u_depth: texture_2d<f32>;
  fn lin_at(p: vec2i) -> f32 { return textureLoad(u_depth, clamp(p, vec2i(0), vec2i(u.u_size.xy) - 1), 0).r; }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let b = vec2i(cw_frag_coord(pos));
    let z0 = lin_at(b * 2);
    let n0 = textureLoad(u_gbuf, b, 0).rg;
    var sum = vec4f(0.0);
    var wsum = 0.0;
    for (var j = -2; j <= 2; j++) {
      for (var i = -2; i <= 2; i++) {
        let t = clamp(b + vec2i(i, j), vec2i(0), vec2i(u.u_size.zw) - 1);
        let dz = (lin_at(t * 2) - z0) / (0.03 * z0 + 1e-3);
        let dn = textureLoad(u_gbuf, t, 0).rg - n0;
        let gt = textureLoad(u_gi, t, 0);
        let w = exp(-dz * dz - dot(dn, dn) * 8.0 - f32(i * i + j * j) * 0.15) / (1.0 + dot(gt.rgb, vec3f(0.2126, 0.7152, 0.0722)));
        sum += gt * w;
        wsum += w;
      }
    }
    if (wsum > 1e-6) { return sum / wsum; }
    return vec4f(0.0, 0.0, 0.0, 1.0);
  });

static const char *COMP_WGSL = S(
  struct U { u_size: vec4f, u_cam: vec4f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @group(0) @binding(2) var u_gi: texture_2d<f32>;
  @group(0) @binding(3) var u_alb: texture_2d<f32>;
  @group(0) @binding(4) var u_depth: texture_2d<f32>;
  fn lin_at(p: vec2i) -> f32 { return textureLoad(u_depth, clamp(p, vec2i(0), vec2i(u.u_size.xy) - 1), 0).r; }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let c = textureSampleLevel(u_src, cw_linear, uv, 0.0);
    let hp = uv * u.u_size.zw - 0.5;
    let base = floor(hp);
    let fr = hp - base;
    let alb = textureLoad(u_alb, clamp(vec2i(floor(hp + 0.5)), vec2i(0), vec2i(u.u_size.zw) - 1), 0);
    if (alb.a < 0.5) { return c; }
    let z0 = lin_at(vec2i(cw_frag_coord(pos)));
    var sum = vec4f(0.0);
    var wsum = 0.0;
    for (var j = 0; j < 2; j++) {
      for (var i = 0; i < 2; i++) {
        let t = clamp(vec2i(base) + vec2i(i, j), vec2i(0), vec2i(u.u_size.zw) - 1);
        let dz = (lin_at(t * 2) - z0) / (0.03 * z0 + 1e-3);
        let w = select(fr.x, 1.0 - fr.x, i == 0) * select(fr.y, 1.0 - fr.y, j == 0) * exp(-dz * dz) + 1e-5;
        sum += textureLoad(u_gi, t, 0) * w;
        wsum += w;
      }
    }
    let g = sum / wsum;
    return vec4f(c.rgb * g.a + alb.rgb * g.rgb, c.a);
  });
#endif

/* the trace's slice and step counts are compile-time constants (its loops unroll, so the
 * depth fetches of all steps issue together): one program per pair in use */
static struct { int slices, steps; unsigned prog; } trace_progs[6];
static unsigned trace_prog, down_prog, norm_prog, denoise_prog, comp_prog;

static unsigned trace_for(bool wg, int slices, int steps) {
  for (int i = 0; i < 6; i++)
    if (trace_progs[i].prog && trace_progs[i].slices == slices && trace_progs[i].steps == steps) return trace_progs[i].prog;
  static char buf[24576];
  char name[48];
  snprintf(name, sizeof name, "ssgi.trace.%dx%d", slices, steps);
  unsigned p = 0;
#if CW_WGPU
  if (wg) {
    snprintf(buf, sizeof buf, "const SLICES: i32 = %d;\nconst STEPS: i32 = %d;\n%s%s", slices, steps, WGSL_COMMON, TRACE_WGSL);
    p = cw_pass_program_wgsl(name, buf);
  } else
#endif
  {
    (void)wg;
    snprintf(buf, sizeof buf, "#define SLICES %d\n#define STEPS %d\n%s", slices, steps, TRACE_GLSL);
    p = cw_pass_program(name, buf);
  }
  for (int i = 0; i < 6; i++)
    if (!trace_progs[i].prog) {
      trace_progs[i].slices = slices, trace_progs[i].steps = steps, trace_progs[i].prog = p;
      break;
    }
  return p;
}

static bool ssgi_active(const cw_pass_ctx *ctx, void *u) {
  (void)u;
  return ctx->settings->ssgi && ctx->renderer && ctx->scene3d;
}

static void ssgi_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  const cw_post_settings *s = ctx->settings;
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  if (!denoise_prog) {
#if CW_WGPU
    if (wg) {
      down_prog = cw_pass_program_wgsl("ssgi.down", DOWN_WGSL);
      norm_prog = cw_pass_program_wgsl("ssgi.norm", NORM_WGSL);
      denoise_prog = cw_pass_program_wgsl("ssgi.denoise", DENOISE_WGSL);
      comp_prog = cw_pass_program_wgsl("ssgi.composite", COMP_WGSL);
    } else
#endif
    {
      down_prog = cw_pass_program("ssgi.down", DOWN_GLSL);
      norm_prog = cw_pass_program("ssgi.norm", NORM_GLSL);
      denoise_prog = cw_pass_program("ssgi.denoise", DENOISE_GLSL);
      comp_prog = cw_pass_program("ssgi.composite", COMP_GLSL);
    }
  }
  trace_prog = trace_for(wg, CW_CLAMP(s->ssgi_slices, 1, 8), CW_CLAMP(s->ssgi_steps, 1, 64));
  if (!trace_prog || !down_prog || !norm_prog || !denoise_prog || !comp_prog || !ctx->depth_tex) return;
  gi_tree *g = cw__gi_tree(ctx->tree);
  int hw = (ctx->width + 1) / 2, hh = (ctx->height + 1) / 2;
  if (!g->gbuf_ok || !g->albedo_ok || g->gbuf.w != hw || g->gbuf.h != hh) return; /* (drawn by the gi pass) */
  const cw_rt *tr = cw_pipeline_rt_persistent(ctx->pipeline, "ssgi.trace", hw, hh, CW_RT_RGBA16F, NULL);
  const cw_rt *dn = cw_pipeline_rt_persistent(ctx->pipeline, "ssgi.denoised", hw, hh, CW_RT_RGBA16F, NULL);
  const cw_rt *lt = cw_pipeline_rt_persistent(ctx->pipeline, "ssgi.light", hw, hh, CW_RT_RGBA16F, NULL);
  const cw_rt *nm = cw_pipeline_rt_persistent(ctx->pipeline, "ssgi.normal", hw, hh, CW_RT_RG16F, NULL);
  if (!tr || !dn || !lt || !nm) return;
  const t3_camera *cam = ctx->camera;
  /* r187's temporal sequence (rotation, offset) when the pipeline's TAA accumulates */
  static const float ROT[6] = { 60, 300, 180, 240, 120, 0 }, OFF[4] = { 0, 0.5f, 0.25f, 0.75f };
  float tdir = s->taa ? ROT[ctx->frame % 6] / 360.0f : 0, toff = s->taa ? OFF[(ctx->frame / 6) % 4] : 0;
  cw_pass_bind_target(ctx, lt);
  cw_pass_use(down_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  if (!wg) cw_pass_uniform1i(down_prog, "u_src", 0);
  cw_pass_fullscreen();
  cw_pass_bind_target(ctx, nm);
  cw_pass_use(norm_prog);
  cw_pass_bind_texture(0, g->gbuf.tex);
  if (!wg) cw_pass_uniform1i(norm_prog, "u_src", 0);
  cw_pass_fullscreen();
  cw_pass_bind_target(ctx, tr);
  cw_pass_use(trace_prog);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_bind_texture(1, nm->tex);
  cw_pass_bind_texture(2, g->albedo.tex);
  cw_pass_bind_texture(3, lt->tex);
  if (!wg) {
    cw_pass_uniform1i(trace_prog, "u_depth", 0);
    cw_pass_uniform1i(trace_prog, "u_gbuf", 1);
    cw_pass_uniform1i(trace_prog, "u_alb", 2);
    cw_pass_uniform1i(trace_prog, "u_color", 3);
  }
  cw_pass_uniform_mat4(trace_prog, "u_inv_proj", &ctx->pipeline->inv_proj_jittered);
  cw_pass_uniform4f(trace_prog, "u_size", (float)ctx->width, (float)ctx->height, (float)hw, (float)hh);
  cw_pass_uniform4f(trace_prog, "u_params", s->ssgi_radius, s->ssgi_intensity, s->ssgi_ao_intensity, s->ssgi_thickness);
  cw_pass_uniform4f(trace_prog, "u_counts", (float)CW_CLAMP(s->ssgi_slices, 1, 8), (float)CW_CLAMP(s->ssgi_steps, 1, 64), tdir, toff);
  {
    const float *e = cam->projection_matrix.e;
    cw_pass_uniform4f(trace_prog, "u_pz", e[10], e[14], e[11], e[15]);
  }
  cw_pass_fullscreen();
  cw_pass_bind_target(ctx, dn);
  cw_pass_use(denoise_prog);
  cw_pass_bind_texture(0, tr->tex);
  cw_pass_bind_texture(1, nm->tex);
  cw_pass_bind_texture(2, ctx->depth_tex);
  if (!wg) {
    cw_pass_uniform1i(denoise_prog, "u_gi", 0);
    cw_pass_uniform1i(denoise_prog, "u_gbuf", 1);
    cw_pass_uniform1i(denoise_prog, "u_depth", 2);
  }
  cw_pass_uniform4f(denoise_prog, "u_size", (float)ctx->width, (float)ctx->height, (float)hw, (float)hh);
  cw_pass_uniform4f(denoise_prog, "u_cam", cam->near, cam->far, 0, 0);
  cw_pass_fullscreen();
  cw_pass_bind_target(ctx, ctx->dst);
  cw_pass_use(comp_prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, dn->tex);
  cw_pass_bind_texture(2, g->albedo.tex);
  cw_pass_bind_texture(3, ctx->depth_tex);
  if (!wg) {
    cw_pass_uniform1i(comp_prog, "u_src", 0);
    cw_pass_uniform1i(comp_prog, "u_gi", 1);
    cw_pass_uniform1i(comp_prog, "u_alb", 2);
    cw_pass_uniform1i(comp_prog, "u_depth", 3);
  }
  cw_pass_uniform4f(comp_prog, "u_size", (float)ctx->width, (float)ctx->height, (float)hw, (float)hh);
  cw_pass_uniform4f(comp_prog, "u_cam", cam->near, cam->far, 0, 0);
  cw_pass_fullscreen();
}

void cw__gi_add_ssgi_pass(cw_pipeline *p) {
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "ssgi", .phase = CW_PHASE_POST, .order = 120,
                                           .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                           .active = ssgi_active, .execute = ssgi_exec, .webgpu = true });
}
