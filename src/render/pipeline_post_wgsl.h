/* WGSL ports of the built-in passes' GLSL (pipeline_post.c,
 * pipeline_smaa.c, fog.c), line for line: the same uniforms by name (members
 * of each module's `U` block), texture unit k at binding k + 1, GL's v_uv as
 * `uv`, gl_FragCoord as cw_frag_coord(pos) (cw_pass_program_wgsl's header).
 * Differences from the GLSL, all forced by WGSL or the WebGPU depth model:
 *   - textureSampleLevel(..., 0.0) instead of texture() (WGSL requires
 *     uniform control flow for implicit-LOD sampling; the targets have one
 *     level, so the result is the same);
 *   - depth: GL samples the window depth; here the R32F linear depth is read
 *     with textureLoad and turned back into GL's window depth by
 *     win_depth(z, u_pz) (u_pz = the projection's z rows), so the math after
 *     it is the GLSL's;
 *   - no swizzle assignment / inout swizzles: written out per component. */
#ifndef CW_PIPELINE_POST_WGSL_H
#define CW_PIPELINE_POST_WGSL_H

/* every user includes it; each uses a few */
#define WGSL_DECL __attribute__((unused)) static const char *

#ifndef S
#define S(...) #__VA_ARGS__ "\n"
#endif

/* the fog's uniform members (the fused SSAO composite's block takes them too) */
#define WGSL_FOG_FIELDS "u_inv_view_proj: mat4x4f, u_cam: vec3f, u_fog: vec4f, u_dist: vec4f, u_color: vec3f, u_sun_dir: vec3f, " \
                        "u_sun_color: vec3f, u_g: f32, "

WGSL_DECL WGSL_COMMON =
    "const PI = 3.14159265358979;\n"
    "const HALF_PI = 1.5707963267949;\n"
    S(fn luma(c: vec3f) -> f32 { return dot(c, vec3f(0.2126, 0.7152, 0.0722)); }
      fn hash_u(v0: vec3u) -> u32 {
        var v = v0 * 1664525u + vec3u(1013904223u);
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        v ^= v >> vec3u(16u);
        v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
        return v.x;
      }
      fn hash01(v: vec3u) -> f32 { return f32(hash_u(v) >> 8u) / 16777216.0; }
      /* GL's window depth from the linear depth (3e38 = nothing drawn) */
      fn win_depth(zl: f32, pz: vec4f) -> f32 {
        if (zl > 1e30) { return 1.0; }
        let zv = -zl;
        return (pz.x * zv + pz.y) / (pz.z * zv + pz.w) * 0.5 + 0.5;
      });

/* post fusion (pipeline_post.c FUSE_ROUND): a half rounded toward zero (the
 * colour export and the blend), an RGBA8 unorm of a half; pack2x16float is
 * defined only in the half's range, so values are held to it first (beyond
 * it rounding toward zero gives the largest half too) */
WGSL_DECL WGSL_FUSE_ROUND = S(
  fn h16bits(v0: f32) -> u32 {
    let v = clamp(v0, -65504.0, 65504.0);
    var h = pack2x16float(vec2f(v, 0.0)) & 0xffffu;
    if (abs(unpack2x16float(h).x) > abs(v) && (h & 0x7fffu) != 0u) { h = h - 1u; }
    return h;
  }
  fn h16(v: f32) -> f32 { return unpack2x16float(h16bits(v)).x; }
  fn q16(v: vec3f) -> vec3f { return vec3f(h16(v.r), h16(v.g), h16(v.b)); }
  fn r16(v: f32) -> f32 { return h16(v); }
  fn q8(v: vec3f) -> vec3f { return floor(q16(clamp(v, vec3f(0.0), vec3f(1.0))) * 255.0 + 0.5) / 255.0; }
  fn q8a(v0: vec4f) -> vec4f {
    let v = clamp(v0, vec4f(0.0), vec4f(1.0));
    return floor(vec4f(h16(v.r), h16(v.g), h16(v.b), h16(v.a)) * 255.0 + 0.5) / 255.0;
  });
/* CW_TEST_FUSION_RAW=1: no rounding (the post-fusion test's control) */
WGSL_DECL WGSL_FUSE_ROUND_RAW = S(
  fn q16(v: vec3f) -> vec3f { return v; }
  fn r16(v: f32) -> f32 { return v; }
  fn q8(v: vec3f) -> vec3f { return v; }
  fn q8a(v: vec4f) -> vec4f { return v; });
/* the lens pass (WGSL_LENS, chromatic aberration off) on the RGBA8 value the pass before it wrote */
WGSL_DECL WGSL_FUSE_LENS = S(
  fn fuse_lens(c0: vec4f, uv: vec2f, pos: vec4f) -> vec4f {
    var c = q8a(c0);
    let d = uv - 0.5;
    if (u.u_on.y > 0.5) {
      let r = length(d * vec2f(u.u_on.w, 1.0)) / length(vec2f(0.5 * u.u_on.w, 0.5));
      let f = smoothstep(1.0 - u.u_amt.z, 1.0 + u.u_amt.z * 0.25, r * (1.0 + 0.5 * u.u_amt.z));
      c = vec4f(c.rgb * (1.0 - u.u_amt.y * f), c.a);
    }
    if (u.u_on.z > 0.5) {
      let n = hash01(vec3u(vec2u(cw_frag_coord(pos)), u32(u.u_frame))) - 0.5;
      let l = luma(c.rgb);
      c = vec4f(max(c.rgb + n * u.u_amt.w * (1.0 - 0.5 * l), vec3f(0.0)), c.a);
    }
    return c;
  });
/* the fog over the scene as the fog pass's blend leaves it */
WGSL_DECL WGSL_FUSE_FOG = S(
  fn fog_over(c: vec4f, f: vec4f) -> vec4f {
    if (f.a < 0.0) { return c; }
    let s = vec4f(q16(f.rgb), r16(f.a));
    return vec4f(q16(s.rgb + c.rgb * (1.0 - s.a)), r16(s.a + c.a * (1.0 - s.a)));
  });
/* bloom's composite before the tone curve, the grade after it */
WGSL_DECL WGSL_FUSE_BLOOM_GRADE = S(
  fn BG(p: vec2f) -> vec3f { return textureSampleLevel(u_bloom, cw_linear, p, 0.0).rgb; }
  fn fuse_bloom(c: vec4f, uv: vec2f) -> vec4f {
    let t = u.u_texel * u.u_radius;
    var s = BG(uv) * 4.0;
    s += (BG(uv + vec2f(-t.x, 0.0)) + BG(uv + vec2f(t.x, 0.0)) + BG(uv + vec2f(0.0, -t.y)) + BG(uv + vec2f(0.0, t.y))) * 2.0;
    s += BG(uv - t) + BG(uv + t) + BG(uv + vec2f(-t.x, t.y)) + BG(uv + vec2f(t.x, -t.y));
    let bloom = s / 16.0 * u.u_norm;
    return vec4f(q16(max(c.rgb + u.u_k * (bloom - prefilter(c.rgb)), vec3f(0.0))), c.a);
  }
  fn fuse_grade(c0: vec3f) -> vec3f {
    let c = q8(c0);
    let t = clamp((c - u.u_dmin) * u.u_dinv, vec3f(0.0), vec3f(1.0));
    let g = textureSampleLevel(u_lut, cw_linear, t * u.u_scale.x + u.u_scale.y, 0.0).rgb;
    return mix(c, g, u.u_scale.z);
  });

WGSL_DECL WGSL_VEL = S(
  struct U { u_inv_vp_j: mat4x4f, u_vp: mat4x4f, u_prev_vp: mat4x4f, u_pz: vec4f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_depth: texture_2d<f32>;
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let d = win_depth(cw_load(u_depth, uv).r, u.u_pz);
    var w = u.u_inv_vp_j * vec4f(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    w = w / w.w;
    let c = u.u_vp * w;
    let p = u.u_prev_vp * w;
    return vec4f((c.xy / c.w - p.xy / p.w) * 0.5, 0.0, 1.0);
  });

WGSL_DECL WGSL_AO = S(
  struct U { u_inv_proj: mat4x4f, u_proj_scale: vec4f, u_params: vec4f, u_pz: vec4f, u_frame: i32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_depth: texture_2d<f32>;
  fn view_pos_px(q0: vec2i) -> vec3f {
    let q = clamp(q0, vec2i(0), vec2i(u.u_proj_scale.zw) - 1);
    let d = win_depth(textureLoad(u_depth, q, 0).r, u.u_pz);
    let uv = (vec2f(q) + 0.5) / u.u_proj_scale.zw;
    let p = u.u_inv_proj * vec4f(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
  }
  fn view_pos(uv: vec2f) -> vec3f { return view_pos_px(vec2i(floor(uv * u.u_proj_scale.zw))); }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv0: vec2f) -> @location(0) vec4f {
    let fc = cw_frag_coord(pos);
    let q = vec2i(fc) * 2;
    let uv = (vec2f(q) + 0.5) / u.u_proj_scale.zw;
    let d = win_depth(textureLoad(u_depth, clamp(q, vec2i(0), vec2i(u.u_proj_scale.zw) - 1), 0).r, u.u_pz);
    if (d >= 1.0) { return vec4f(1.0, 1e6, 0.0, 1.0); }
    let P = view_pos_px(q);
    let pr = view_pos_px(q + vec2i(1, 0));
    let pl = view_pos_px(q - vec2i(1, 0));
    let pu = view_pos_px(q + vec2i(0, 1));
    let pd = view_pos_px(q - vec2i(0, 1));
    let dx = select(P - pl, pr - P, abs(pr.z - P.z) < abs(P.z - pl.z));
    let dy = select(P - pd, pu - P, abs(pu.z - P.z) < abs(P.z - pd.z));
    var N = normalize(cross(dx, dy));
    let V = normalize(-P);
    if (dot(N, V) < 0.0) { N = -N; }
    let R = u.u_params.x;
    let ruv = vec2f(u.u_proj_scale.x, u.u_proj_scale.y) * 0.5 * R / max(-P.z, 1e-4);
    let slices = i32(u.u_params.z);
    let steps = i32(u.u_params.w);
    let ip = vec2i(fc) & vec2i(3);
    let idx = ip.x + ip.y * 4;
    var perm = array<i32, 16>(0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    let rot = (f32(perm[idx]) + 0.5) / 16.0;
    let jit = f32((idx * 7 + u.u_frame * 5) & 15) / 16.0;
    var vis = 0.0;
    for (var s = 0; s < 8; s++) {
      if (s >= slices) { break; }
      let phi = (f32(s) + rot) / f32(slices) * PI;
      let dir = vec2f(cos(phi), sin(phi));
      let d3 = vec3f(dir, 0.0);
      let ortho = d3 - dot(d3, V) * V;
      let axis = normalize(cross(d3, V));
      let pn = N - axis * dot(N, axis);
      let pl2 = max(length(pn), 1e-4);
      let cosn = clamp(dot(pn, V) / pl2, -1.0, 1.0);
      let n = sign(dot(ortho, pn)) * acos(cosn);
      var hc = array<f32, 2>(0.0, 0.0);
      for (var side = 0; side < 2; side++) {
        let sgn = select(1.0, -1.0, side == 0);
        var h = -1.0;
        for (var k = 0; k < 16; k++) {
          if (k >= steps) { break; }
          var t = (f32(k) + 1.0 - jit) / f32(steps);
          t = t * t * 0.9 + 0.1 * t;
          var off = dir * ruv * t;
          let opx = length(off * u.u_proj_scale.zw);
          if (opx < 1.5) { off *= 1.5 / max(opx, 1e-4); }
          let suv = uv + sgn * off;
          if (any(suv < vec2f(0.0)) || any(suv > vec2f(1.0))) { break; }
          let H = view_pos(suv) - P;
          let len = length(H);
          let c = dot(H, V) / max(len, 1e-5);
          let fall = clamp(1.0 - (len * len) / (R * R), 0.0, 1.0);
          h = max(h, mix(-1.0, c, fall));
        }
        hc[side] = h;
      }
      let h0 = n + max(-acos(hc[0]) - n, -HALF_PI);
      let h1 = n + min(acos(hc[1]) - n, HALF_PI);
      let sn = sin(n);
      let cn = cos(n);
      let a = 0.25 * (-cos(2.0 * h0 - n) + cn + 2.0 * h0 * sn) + 0.25 * (-cos(2.0 * h1 - n) + cn + 2.0 * h1 * sn);
      vis += pl2 * a;
    }
    vis = clamp(vis / f32(slices), 0.0, 1.0);
    return vec4f(pow(vis, u.u_params.y), -P.z, 0.0, 1.0);
  });

#define WGSL_AO_COMP_BINDINGS \
  S(@group(0) @binding(0) var<uniform> u: U; \
    @group(0) @binding(1) var u_src: texture_2d<f32>; \
    @group(0) @binding(2) var u_ao: texture_2d<f32>; \
    @group(0) @binding(3) var u_depth: texture_2d<f32>;)
WGSL_DECL WGSL_AO_COMP_HEAD = "struct U { u_inv_proj: mat4x4f, u_ao_size: vec2f, u_intensity: f32, u_pz: vec4f };\n"
  WGSL_AO_COMP_BINDINGS S(
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let d = win_depth(cw_load(u_depth, uv).r, u.u_pz);
    return ao_composite(uv, textureSampleLevel(u_src, cw_linear, uv, 0.0), d);
  });
/* the composite with the height fog fused in (pipeline_post.c ao_fog_program) */
WGSL_DECL WGSL_AO_COMP_FOG_HEAD = "struct U { u_inv_proj: mat4x4f, u_ao_size: vec2f, u_intensity: f32, " WGSL_FOG_FIELDS
  "u_pz: vec4f };\n" WGSL_AO_COMP_BINDINGS;
WGSL_DECL WGSL_AO_COMP_FOG_IN = S(
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let d = win_depth(cw_load(u_depth, uv).r, u.u_pz);
    return ao_composite(uv, fog_over(textureSampleLevel(u_src, cw_linear, uv, 0.0), fog_eval(uv, d)), d);
  });
WGSL_DECL WGSL_AO_COMP_BODY = S(
  fn ao_composite(uv: vec2f, c: vec4f, d: f32) -> vec4f {
    if (d >= 1.0) { return c; }
    let p = u.u_inv_proj * vec4f(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    let z = -p.z / p.w;
    let base = uv * u.u_ao_size - 0.5;
    let f = floor(base);
    var sum = 0.0;
    var wsum = 0.0;
    for (var j = -1; j <= 2; j++) {
      for (var i = -1; i <= 2; i++) {
        let t = f + vec2f(f32(i), f32(j));
        let a = textureSampleLevel(u_ao, cw_linear, (t + 0.5) / u.u_ao_size, 0.0).rg;
        let dd = base - t;
        let ws = exp(-dot(dd, dd) * 0.35);
        let rz = abs(a.y - z) / (0.06 * max(z, 1e-3));
        let wz = exp(-rz * rz);
        let w = ws * wz;
        sum += a.x * w;
        wsum += w;
      }
    }
    let ao = select(1.0, sum / wsum, wsum > 1e-4);
    return vec4f(c.rgb * mix(1.0, ao, u.u_intensity), c.a);
  });

WGSL_DECL WGSL_TAA = S(
  struct U { u_size: vec2f, u_feedback: f32, u_fresh: i32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @group(0) @binding(2) var u_hist: texture_2d<f32>;
  @group(0) @binding(3) var u_vel: texture_2d<f32>;
  fn to_ycocg(c: vec3f) -> vec3f { return vec3f(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
  fn from_ycocg(c: vec3f) -> vec3f { return vec3f(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
  fn H(p: vec2f) -> vec3f { return textureSampleLevel(u_hist, cw_linear, p, 0.0).rgb; }
  fn hist_catmull(uv: vec2f) -> vec3f {
    let pos = uv * u.u_size;
    let c = floor(pos - 0.5) + 0.5;
    let f = pos - c;
    let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    let w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    let w3 = f * f * (-0.5 + 0.5 * f);
    let w12 = w1 + w2;
    let t0 = (c - 1.0) / u.u_size;
    let t3 = (c + 2.0) / u.u_size;
    let t12 = (c + w2 / w12) / u.u_size;
    let r = H(vec2f(t12.x, t0.y)) * (w12.x * w0.y) + H(vec2f(t0.x, t12.y)) * (w0.x * w12.y) +
            H(vec2f(t12.x, t12.y)) * (w12.x * w12.y) + H(vec2f(t3.x, t12.y)) * (w3.x * w12.y) +
            H(vec2f(t12.x, t3.y)) * (w12.x * w3.y);
    let ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / ws, vec3f(0.0));
  }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let cur = textureSampleLevel(u_src, cw_linear, uv, 0.0);
    if (u.u_fresh != 0) { return cur; }
    let px = 1.0 / u.u_size;
    var m1 = vec3f(0.0);
    var m2 = vec3f(0.0);
    var mn = vec3f(1e9);
    var mx = vec3f(-1e9);
    for (var j = -1; j <= 1; j++) {
      for (var i = -1; i <= 1; i++) {
        let c = to_ycocg(textureSampleLevel(u_src, cw_linear, uv + vec2f(f32(i), f32(j)) * px, 0.0).rgb);
        m1 += c; m2 += c * c; mn = min(mn, c); mx = max(mx, c);
      }
    }
    m1 = m1 / 9.0;
    let sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3f(0.0)));
    let bmin = max(mn, m1 - sigma * 1.25);
    let bmax = min(mx, m1 + sigma * 1.25);
    let prev = uv - textureSampleLevel(u_vel, cw_linear, uv, 0.0).xy;
    let c0 = to_ycocg(cur.rgb);
    var h: vec3f;
    if (any(prev < vec2f(0.0)) || any(prev > vec2f(1.0))) { h = c0; }
    else {
      h = to_ycocg(hist_catmull(prev));
      let ctr = 0.5 * (bmax + bmin);
      let ext = 0.5 * (bmax - bmin) + 1e-5;
      let v = h - ctr;
      let a = abs(v / ext);
      let m = max(a.x, max(a.y, a.z));
      if (m > 1.0) { h = ctr + v / m; }
    }
    let wc = (1.0 - u.u_feedback) / (1.0 + c0.x);
    let wh = u.u_feedback / (1.0 + h.x);
    let r = (c0 * wc + h * wh) / (wc + wh);
    return vec4f(from_ycocg(r), cur.a);
  });

WGSL_DECL WGSL_BL_PREFILTER = S(
  fn prefilter(c: vec3f) -> vec3f {
    let br = max(c.r, max(c.g, c.b));
    var rq = clamp(br - u.u_thresh.y, 0.0, u.u_thresh.z);
    rq = u.u_thresh.w * rq * rq;
    let w = max(rq, br - u.u_thresh.x) / max(br, 1e-5);
    return c * w;
  });

WGSL_DECL WGSL_BL_DOWN = S(
  struct U { u_thresh: vec4f, u_texel: vec2f, u_first: i32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  fn tap(p: vec2f) -> vec3f {
    let c = textureSampleLevel(u_src, cw_linear, p, 0.0).rgb;
    return select(c, prefilter(c), u.u_first != 0);
  }
  fn karis(a: vec3f, b: vec3f, c: vec3f, d: vec3f) -> vec3f {
    let wa = 1.0 / (1.0 + luma(a));
    let wb = 1.0 / (1.0 + luma(b));
    let wc = 1.0 / (1.0 + luma(c));
    let wd = 1.0 / (1.0 + luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
  }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let t = u.u_texel;
    let A = tap(uv + t * vec2f(-2.0, 2.0)); let B = tap(uv + t * vec2f(0.0, 2.0)); let C = tap(uv + t * vec2f(2.0, 2.0));
    let D = tap(uv + t * vec2f(-1.0, 1.0)); let E = tap(uv + t * vec2f(1.0, 1.0));
    let F = tap(uv + t * vec2f(-2.0, 0.0)); let G = tap(uv); let H = tap(uv + t * vec2f(2.0, 0.0));
    let I = tap(uv + t * vec2f(-1.0, -1.0)); let J = tap(uv + t * vec2f(1.0, -1.0));
    let K = tap(uv + t * vec2f(-2.0, -2.0)); let L = tap(uv + t * vec2f(0.0, -2.0)); let M = tap(uv + t * vec2f(2.0, -2.0));
    var r: vec3f;
    if (u.u_first != 0) {
      r = karis(D, E, I, J) * 0.5 + karis(A, B, F, G) * 0.125 + karis(B, C, G, H) * 0.125 + karis(F, G, K, L) * 0.125 +
          karis(G, H, L, M) * 0.125;
    } else {
      r = (D + E + I + J) * 0.125 + (A + C + K + M) * 0.03125 + (B + F + H + L) * 0.0625 + G * 0.125;
    }
    return vec4f(r, 1.0);
  });

WGSL_DECL WGSL_BL_UP = S(
  struct U { u_texel: vec2f, u_radius: f32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_coarse: texture_2d<f32>;
  @group(0) @binding(2) var u_fine: texture_2d<f32>;
  fn C(p: vec2f) -> vec3f { return textureSampleLevel(u_coarse, cw_linear, p, 0.0).rgb; }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let t = u.u_texel * u.u_radius;
    var s = C(uv) * 4.0;
    s += (C(uv + vec2f(-t.x, 0.0)) + C(uv + vec2f(t.x, 0.0)) + C(uv + vec2f(0.0, -t.y)) + C(uv + vec2f(0.0, t.y))) * 2.0;
    s += C(uv - t) + C(uv + t) + C(uv + vec2f(-t.x, t.y)) + C(uv + vec2f(t.x, -t.y));
    return vec4f(textureSampleLevel(u_fine, cw_linear, uv, 0.0).rgb + s / 16.0, 1.0);
  });

WGSL_DECL WGSL_BL_COMP = S(
  struct U { u_thresh: vec4f, u_texel: vec2f, u_radius: f32, u_k: f32, u_norm: f32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @group(0) @binding(2) var u_bloom: texture_2d<f32>;
  fn B(p: vec2f) -> vec3f { return textureSampleLevel(u_bloom, cw_linear, p, 0.0).rgb; }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let c = textureSampleLevel(u_src, cw_linear, uv, 0.0);
    let t = u.u_texel * u.u_radius;
    var s = B(uv) * 4.0;
    s += (B(uv + vec2f(-t.x, 0.0)) + B(uv + vec2f(t.x, 0.0)) + B(uv + vec2f(0.0, -t.y)) + B(uv + vec2f(0.0, t.y))) * 2.0;
    s += B(uv - t) + B(uv + t) + B(uv + vec2f(-t.x, t.y)) + B(uv + vec2f(t.x, -t.y));
    let bloom = s / 16.0 * u.u_norm;
    return vec4f(max(c.rgb + u.u_k * (bloom - prefilter(c.rgb)), vec3f(0.0)), c.a);
  });

#define WGSL_TM_FNS_SRC S(\
  fn LinearToneMapping(color: vec3f) -> vec3f { return u.toneMappingExposure * color; } \
  fn ReinhardToneMapping(c0: vec3f) -> vec3f { \
    let color = c0 * u.toneMappingExposure; \
    return clamp(color / (vec3f(1.0) + color), vec3f(0.0), vec3f(1.0)); \
  } \
  fn OptimizedCineonToneMapping(c0: vec3f) -> vec3f { \
    var color = c0 * u.toneMappingExposure; \
    color = max(vec3f(0.0), color - 0.004); \
    return pow((color * (6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06), vec3f(2.2)); \
  } \
  fn RRTAndODTFit(v: vec3f) -> vec3f { \
    let a = v * (v + 0.0245786) - 0.000090537; \
    let b = v * (0.983729 * v + 0.4329510) + 0.238081; \
    return a / b; \
  } \
  fn ACESFilmicToneMapping(c0: vec3f) -> vec3f { \
    let ACESInputMat = mat3x3f(vec3f(0.59719, 0.07600, 0.02840), vec3f(0.35458, 0.90834, 0.13383), vec3f(0.04823, 0.01566, 0.83777)); \
    let ACESOutputMat = mat3x3f(vec3f(1.60475, -0.10208, -0.00327), vec3f(-0.53108, 1.10813, -0.07276), vec3f(-0.07367, -0.00605, 1.07602)); \
    var color = c0 * (u.toneMappingExposure / 0.6); \
    color = ACESInputMat * color; \
    color = RRTAndODTFit(color); \
    color = ACESOutputMat * color; \
    return clamp(color, vec3f(0.0), vec3f(1.0)); \
  } \
  fn agxDefaultContrastApprox(x: vec3f) -> vec3f { \
    let x2 = x * x; \
    let x4 = x2 * x2; \
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232; \
  } \
  fn AgXToneMapping(c0: vec3f) -> vec3f { \
    let LINEAR_SRGB_TO_LINEAR_REC2020 = mat3x3f(vec3f(0.6274, 0.0691, 0.0164), vec3f(0.3293, 0.9195, 0.0880), vec3f(0.0433, 0.0113, 0.8956)); \
    let LINEAR_REC2020_TO_LINEAR_SRGB = mat3x3f(vec3f(1.6605, -0.1246, -0.0182), vec3f(-0.5876, 1.1329, -0.1006), vec3f(-0.0728, -0.0083, 1.1187)); \
    let AgXInsetMatrix = mat3x3f(vec3f(0.856627153315983, 0.137318972929847, 0.11189821299995), \
                                 vec3f(0.0951212405381588, 0.761241990602591, 0.0767994186031903), \
                                 vec3f(0.0482516061458583, 0.101439036467562, 0.811302368396859)); \
    let AgXOutsetMatrix = mat3x3f(vec3f(1.1271005818144368, -0.1413297634984383, -0.14132976349843826), \
                                  vec3f(-0.11060664309660323, 1.157823702216272, -0.11060664309660294), \
                                  vec3f(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405)); \
    let AgxMinEv = -12.47393; \
    let AgxMaxEv = 4.026069; \
    var color = c0 * u.toneMappingExposure; \
    color = LINEAR_SRGB_TO_LINEAR_REC2020 * color; \
    color = AgXInsetMatrix * color; \
    color = max(color, vec3f(1e-10)); \
    color = log2(color); \
    color = (color - AgxMinEv) / (AgxMaxEv - AgxMinEv); \
    color = clamp(color, vec3f(0.0), vec3f(1.0)); \
    color = agxDefaultContrastApprox(color); \
    color = AgXOutsetMatrix * color; \
    color = pow(max(vec3f(0.0), color), vec3f(2.2)); \
    color = LINEAR_REC2020_TO_LINEAR_SRGB * color; \
    return clamp(color, vec3f(0.0), vec3f(1.0)); \
  } \
  fn NeutralToneMapping(c0: vec3f) -> vec3f { \
    let StartCompression = 0.8 - 0.04; \
    let Desaturation = 0.15; \
    var color = c0 * u.toneMappingExposure; \
    let x = min(color.r, min(color.g, color.b)); \
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08); \
    color -= offset; \
    let peak = max(color.r, max(color.g, color.b)); \
    if (peak < StartCompression) { return color; } \
    let d = 1.0 - StartCompression; \
    let newPeak = 1.0 - d * d / (peak + d - StartCompression); \
    color *= newPeak / peak; \
    let g = 1.0 - 1.0 / (Desaturation * (peak - newPeak) + 1.0); \
    return mix(color, vec3f(newPeak), g); \
  } \
  fn LinearTosRGB(v: vec3f) -> vec3f { \
    return select(pow(v, vec3f(0.41666)) * 1.055 - vec3f(0.055), v * 12.92, v <= vec3f(0.0031308)); \
  } \
)
WGSL_DECL WGSL_TM = S(
  struct U { toneMappingExposure: f32, u_curve: i32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
) WGSL_TM_FNS_SRC S(
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let c = textureSampleLevel(u_src, cw_linear, uv, 0.0);
    var x = max(c.rgb, vec3f(0.0));
    let k = u.u_curve;
    if (k == 0) { x = clamp(x * u.toneMappingExposure, vec3f(0.0), vec3f(1.0)); }
    else if (k == 1) { x = clamp(LinearToneMapping(x), vec3f(0.0), vec3f(1.0)); }
    else if (k == 2) { x = ReinhardToneMapping(x); }
    else if (k == 3) { x = OptimizedCineonToneMapping(x); }
    else if (k == 4) { x = ACESFilmicToneMapping(x); }
    else if (k == 5) { x = AgXToneMapping(x); }
    else { x = clamp(NeutralToneMapping(x), vec3f(0.0), vec3f(1.0)); }
    x = LinearTosRGB(x); // r186: the output transfer after every curve (Cineon's pow 2.2 returns to linear first)
    return vec4f(x, 1.0);
  });
WGSL_DECL WGSL_TM_FNS = WGSL_TM_FNS_SRC;
/* the tonemap pass with bloom's composite, the grade and / or the lens fused in (pipeline_post.c tm_variant) */
WGSL_DECL WGSL_TM_FUSED_HEAD = S(
  struct U { toneMappingExposure: f32, u_curve: i32, u_thresh: vec4f, u_texel: vec2f, u_radius: f32, u_k: f32, u_norm: f32,
             u_scale: vec4f, u_dmin: vec3f, u_dinv: vec3f, u_on: vec4f, u_amt: vec4f, u_frame: i32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @group(0) @binding(2) var u_bloom: texture_2d<f32>;
  @group(0) @binding(3) var u_lut: texture_3d<f32>;);
WGSL_DECL WGSL_TM_FUSED_FS = S(
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    var c = textureSampleLevel(u_src, cw_linear, uv, 0.0););
WGSL_DECL WGSL_TM_FUSED_CURVES = S(
    var x = max(c.rgb, vec3f(0.0));
    let k = u.u_curve;
    if (k == 0) { x = clamp(x * u.toneMappingExposure, vec3f(0.0), vec3f(1.0)); }
    else if (k == 1) { x = clamp(LinearToneMapping(x), vec3f(0.0), vec3f(1.0)); }
    else if (k == 2) { x = ReinhardToneMapping(x); }
    else if (k == 3) { x = OptimizedCineonToneMapping(x); }
    else if (k == 4) { x = ACESFilmicToneMapping(x); }
    else if (k == 5) { x = AgXToneMapping(x); }
    else { x = clamp(NeutralToneMapping(x), vec3f(0.0), vec3f(1.0)); }
    x = LinearTosRGB(x););

WGSL_DECL WGSL_GRADE = S(
  struct U { u_scale: vec4f, u_dmin: vec3f, u_dinv: vec3f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @group(0) @binding(2) var u_lut: texture_3d<f32>;
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let c = textureSampleLevel(u_src, cw_linear, uv, 0.0);
    let t = clamp((c.rgb - u.u_dmin) * u.u_dinv, vec3f(0.0), vec3f(1.0));
    let g = textureSampleLevel(u_lut, cw_linear, t * u.u_scale.x + u.u_scale.y, 0.0).rgb;
    return vec4f(mix(c.rgb, g, u.u_scale.z), c.a);
  });

/* FXAA: the program is a head (its uniforms), fxaa() and an entry point:
 * plain, or with the lens fused in (pipeline_post.c aa_exec) */
WGSL_DECL WGSL_FXAA_HEAD = S(
  struct U { u_rcp: vec2f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;);
WGSL_DECL WGSL_FXAA_FS = S(
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f { return fxaa(uv); });
WGSL_DECL WGSL_FXAA_LENS_HEAD = S(
  struct U { u_rcp: vec2f, u_on: vec4f, u_amt: vec4f, u_frame: i32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;);
WGSL_DECL WGSL_FXAA_LENS_FS = S(
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    return fuse_lens(fxaa(uv), uv, pos);
  });
WGSL_DECL WGSL_FXAA = S(
  fn T(p: vec2f) -> vec4f { return textureSampleLevel(u_src, cw_linear, p, 0.0); }
  fn L(p: vec2f) -> f32 { return dot(T(p).rgb, vec3f(0.299, 0.587, 0.114)); }
  fn fxaa(uv: vec2f) -> vec4f {
    var posM = uv;
    let rgbyM = T(posM);
    let lumaM = dot(rgbyM.rgb, vec3f(0.299, 0.587, 0.114));
    var lumaS = L(posM + vec2f(0.0, 1.0) * u.u_rcp);
    var lumaE = L(posM + vec2f(1.0, 0.0) * u.u_rcp);
    var lumaN = L(posM + vec2f(0.0, -1.0) * u.u_rcp);
    var lumaW = L(posM + vec2f(-1.0, 0.0) * u.u_rcp);
    let maxSM = max(lumaS, lumaM); let minSM = min(lumaS, lumaM);
    let maxESM = max(lumaE, maxSM); let minESM = min(lumaE, minSM);
    let maxWN = max(lumaN, lumaW); let minWN = min(lumaN, lumaW);
    let rangeMax = max(maxWN, maxESM); let rangeMin = min(minWN, minESM);
    let range = rangeMax - rangeMin;
    if (range < max(0.0312, rangeMax * 0.125)) { return rgbyM; }
    let lumaNW = L(posM + vec2f(-1.0, -1.0) * u.u_rcp); let lumaSE = L(posM + vec2f(1.0, 1.0) * u.u_rcp);
    let lumaNE = L(posM + vec2f(1.0, -1.0) * u.u_rcp); let lumaSW = L(posM + vec2f(-1.0, 1.0) * u.u_rcp);
    let lumaNS = lumaN + lumaS; let lumaWE = lumaW + lumaE;
    let subpixRcpRange = 1.0 / range;
    let subpixNSWE = lumaNS + lumaWE;
    let edgeHorz1 = -2.0 * lumaM + lumaNS; let edgeVert1 = -2.0 * lumaM + lumaWE;
    let lumaNESE = lumaNE + lumaSE; let lumaNWNE = lumaNW + lumaNE;
    let edgeHorz2 = -2.0 * lumaE + lumaNESE; let edgeVert2 = -2.0 * lumaN + lumaNWNE;
    let lumaNWSW = lumaNW + lumaSW; let lumaSWSE = lumaSW + lumaSE;
    let edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2); let edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    let edgeHorz3 = -2.0 * lumaW + lumaNWSW; let edgeVert3 = -2.0 * lumaS + lumaSWSE;
    let edgeHorz = abs(edgeHorz3) + edgeHorz4; let edgeVert = abs(edgeVert3) + edgeVert4;
    let subpixNWSWNESE = lumaNWSW + lumaNESE;
    var lengthSign = u.u_rcp.x;
    let horzSpan = edgeHorz >= edgeVert;
    let subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    if (horzSpan) { lengthSign = u.u_rcp.y; }
    let subpixB = subpixA * (1.0 / 12.0) - lumaM;
    let gradientN = lumaN - lumaM; let gradientS = lumaS - lumaM;
    var lumaNN = lumaN + lumaM; let lumaSS = lumaS + lumaM;
    let pairN = abs(gradientN) >= abs(gradientS);
    let gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) { lengthSign = -lengthSign; }
    let subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);
    var posB = posM;
    let offNP = select(vec2f(0.0, u.u_rcp.y), vec2f(u.u_rcp.x, 0.0), horzSpan);
    if (!horzSpan) { posB.x += lengthSign * 0.5; }
    if (horzSpan) { posB.y += lengthSign * 0.5; }
    var posN = posB - offNP;
    var posP = posB + offNP;
    let subpixD = -2.0 * subpixC + 3.0;
    var lumaEndN = L(posN);
    let subpixE = subpixC * subpixC;
    var lumaEndP = L(posP);
    if (!pairN) { lumaNN = lumaSS; }
    let gradientScaled = gradient * 0.25;
    let lumaMM = lumaM - lumaNN * 0.5;
    let subpixF = subpixD * subpixE;
    let lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    var doneN = abs(lumaEndN) >= gradientScaled;
    var doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) { posN -= offNP * 1.5; }
    if (!doneP) { posP += offNP * 1.5; }
    var qs = array<f32, 3>(2.0, 4.0, 12.0);
    for (var i = 0; i < 3; i++) {
      if (doneN && doneP) { break; }
      if (!doneN) { lumaEndN = L(posN) - lumaNN * 0.5; }
      if (!doneP) { lumaEndP = L(posP) - lumaNN * 0.5; }
      doneN = abs(lumaEndN) >= gradientScaled;
      doneP = abs(lumaEndP) >= gradientScaled;
      if (!doneN) { posN -= offNP * qs[i]; }
      if (!doneP) { posP += offNP * qs[i]; }
    }
    let dstN = select(posM.y - posN.y, posM.x - posN.x, horzSpan);
    let dstP = select(posP.y - posM.y, posP.x - posM.x, horzSpan);
    let goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    let goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    let spanLengthRcp = 1.0 / (dstP + dstN);
    let directionN = dstN < dstP;
    let dst = min(dstN, dstP);
    let goodSpan = select(goodSpanP, goodSpanN, directionN);
    let subpixG = subpixF * subpixF;
    let pixelOffset = dst * (-spanLengthRcp) + 0.5;
    let subpixH = subpixG * 0.75;
    let pixelOffsetGood = select(0.0, pixelOffset, goodSpan);
    let pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) { posM.x += pixelOffsetSubpix * lengthSign; }
    if (horzSpan) { posM.y += pixelOffsetSubpix * lengthSign; }
    return vec4f(T(posM).rgb, rgbyM.a);
  });

WGSL_DECL WGSL_LENS = S(
  struct U { u_on: vec4f, u_amt: vec4f, u_frame: i32 };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_src: texture_2d<f32>;
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let d = uv - 0.5;
    var c = textureSampleLevel(u_src, cw_linear, uv, 0.0);
    if (u.u_on.x > 0.5) {
      let off = d * (u.u_amt.x * 2.0);
      c.r = textureSampleLevel(u_src, cw_linear, uv - off, 0.0).r;
      c.b = textureSampleLevel(u_src, cw_linear, uv + off, 0.0).b;
    }
    if (u.u_on.y > 0.5) {
      let r = length(d * vec2f(u.u_on.w, 1.0)) / length(vec2f(0.5 * u.u_on.w, 0.5));
      let f = smoothstep(1.0 - u.u_amt.z, 1.0 + u.u_amt.z * 0.25, r * (1.0 + 0.5 * u.u_amt.z));
      c = vec4f(c.rgb * (1.0 - u.u_amt.y * f), c.a);
    }
    if (u.u_on.z > 0.5) {
      let n = hash01(vec3u(vec2u(cw_frag_coord(pos)), u32(u.u_frame))) - 0.5;
      let l = luma(c.rgb);
      c = vec4f(max(c.rgb + n * u.u_amt.w * (1.0 - 0.5 * l), vec3f(0.0)), c.a);
    }
    return c;
  });

WGSL_DECL WGSL_VIEW = S(
  struct U { u_inv_proj: mat4x4f, u_size: vec2f, u_view: i32, u_norm: f32, u_pz: vec4f };
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_depth: texture_2d<f32>;
  @group(0) @binding(2) var u_buf: texture_2d<f32>;
  fn vpos(q0: vec2i) -> vec3f {
    let q = clamp(q0, vec2i(0), vec2i(u.u_size) - 1);
    let d = win_depth(textureLoad(u_depth, q, 0).r, u.u_pz);
    let p = u.u_inv_proj * vec4f((vec2f(q) + 0.5) / u.u_size * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
  }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let q = vec2i(cw_frag_coord(pos));
    let buf = textureSampleLevel(u_buf, cw_linear, uv, 0.0);
    if (u.u_view == 1) { let z = -vpos(q).z; return vec4f(vec3f(1.0 / (1.0 + z / 4.0)), 1.0); }
    if (u.u_view == 2) {
      let P = vpos(q); let pr = vpos(q + vec2i(1, 0)); let pl = vpos(q - vec2i(1, 0));
      let pu = vpos(q + vec2i(0, 1)); let pd = vpos(q - vec2i(0, 1));
      let dx = select(P - pl, pr - P, abs(pr.z - P.z) < abs(P.z - pl.z));
      let dy = select(P - pd, pu - P, abs(pu.z - P.z) < abs(P.z - pd.z));
      var N = normalize(cross(dx, dy));
      if (dot(N, -P) < 0.0) { N = -N; }
      return vec4f(N * 0.5 + 0.5, 1.0);
    }
    if (u.u_view == 3) { return vec4f(vec3f(buf.r), 1.0); }
    if (u.u_view == 4) { return vec4f(buf.xy * 20.0 + 0.5, 0.5, 1.0); }
    let b = buf.rgb * u.u_norm;
    return vec4f(b / (1.0 + b), 1.0);
  });

/* ── SMAA (pipeline_smaa.c) ─────────────────────────────────────────── */

/* the SMAA programs' uniform block: plain, and the blend's with the lens fused in */
WGSL_DECL WGSL_SMAA_U = S(struct U { SMAA_RT_METRICS: vec4f };);
WGSL_DECL WGSL_SMAA_LENS_U = S(struct U { SMAA_RT_METRICS: vec4f, u_on: vec4f, u_amt: vec4f, u_frame: i32 };);
WGSL_DECL WGSL_SMAA_COMMON =
    "const SMAA_THRESHOLD = 0.1;\n"
    "const SMAA_MAX_SEARCH_STEPS = 16;\n"
    "const SMAA_CORNER_ROUNDING_NORM = 0.25;\n"
    "const SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR = 2.0;\n"
    "const SMAA_AREATEX_MAX_DISTANCE = 16.0;\n"
    S(@group(0) @binding(0) var<uniform> u: U;
      fn FL(c: vec2f) -> vec2f { return vec2f(c.x, 1.0 - c.y); }
      fn SMP(t: texture_2d<f32>, c: vec2f) -> vec4f { return textureSampleLevel(t, cw_linear, FL(c), 0.0); }
      fn SMPO(t: texture_2d<f32>, c: vec2f, o: vec2f) -> vec4f { return textureSampleLevel(t, cw_linear, FL(c + o * u.SMAA_RT_METRICS.xy), 0.0); });

WGSL_DECL WGSL_SMAA_EDGE = S(
  @group(0) @binding(1) var u_color: texture_2d<f32>;
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let M = u.SMAA_RT_METRICS;
    let texcoord = FL(uv);
    let offset0 = M.xyxy * vec4f(-1.0, 0.0, 0.0, -1.0) + texcoord.xyxy;
    let offset1 = M.xyxy * vec4f(1.0, 0.0, 0.0, 1.0) + texcoord.xyxy;
    let offset2 = M.xyxy * vec4f(-2.0, 0.0, 0.0, -2.0) + texcoord.xyxy;
    let threshold = vec2f(SMAA_THRESHOLD);
    let weights = vec3f(0.2126, 0.7152, 0.0722);
    let L = dot(SMP(u_color, texcoord).rgb, weights);
    let Lleft = dot(SMP(u_color, offset0.xy).rgb, weights);
    let Ltop = dot(SMP(u_color, offset0.zw).rgb, weights);
    let dxy = abs(L - vec2f(Lleft, Ltop));
    var edges = step(threshold, dxy);
    if (dot(edges, vec2f(1.0)) == 0.0) { discard; }
    let Lright = dot(SMP(u_color, offset1.xy).rgb, weights);
    let Lbottom = dot(SMP(u_color, offset1.zw).rgb, weights);
    var maxDelta = max(dxy, abs(L - vec2f(Lright, Lbottom)));
    let Lleftleft = dot(SMP(u_color, offset2.xy).rgb, weights);
    let Ltoptop = dot(SMP(u_color, offset2.zw).rgb, weights);
    let dzw = abs(vec2f(Lleft, Ltop) - vec2f(Lleftleft, Ltoptop));
    maxDelta = max(maxDelta, dzw);
    let finalDelta = max(maxDelta.x, maxDelta.y);
    edges = edges * step(vec2f(finalDelta), SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * dxy);
    return vec4f(edges, 0.0, 1.0);
  });

WGSL_DECL WGSL_SMAA_WEIGHT = S(
  @group(0) @binding(1) var u_edges: texture_2d<f32>;
  @group(0) @binding(2) var u_area: texture_2d<f32>;
  fn decode4(v: f32) -> vec4f {
    var best = 1e9;
    var bits = vec4f(0.0);
    for (var i = 0; i < 16; i++) {
      let b = vec4f(f32(i & 1), f32((i >> 1u) & 1), f32((i >> 2u) & 1), f32((i >> 3u) & 1));
      let f = 0.125 * (0.25 * b.x + 0.75 * b.y) + 0.875 * (0.25 * b.z + 0.75 * b.w);
      let d = abs(f - v);
      if (d < best) { best = d; bits = b; }
    }
    return bits;
  }
  fn searchCorrection(e: vec2f, right: bool) -> f32 {
    let left = decode4(e.x);
    let top = decode4(e.y);
    var d = 0.0;
    if (!right) {
      if (top.w > 0.5) { d += 1.0; }
      if (d == 1.0 && top.z > 0.5 && left.y < 0.5 && left.w < 0.5) { d += 1.0; }
    } else {
      if (top.w > 0.5 && left.y < 0.5 && left.w < 0.5) { d += 1.0; }
      if (d == 1.0 && top.z > 0.5 && left.x < 0.5 && left.z < 0.5) { d += 1.0; }
    }
    return 3.25 - d;
  }
  fn SMAASearchXLeft(tc: vec2f, end: f32) -> f32 {
    var texcoord = tc;
    var e = vec2f(0.0, 1.0);
    for (var i = 0; i < 64; i++) {
      if (!(texcoord.x > end && e.g > 0.8281 && e.r == 0.0)) { break; }
      e = SMP(u_edges, texcoord).rg;
      texcoord = -vec2f(2.0, 0.0) * u.SMAA_RT_METRICS.xy + texcoord;
    }
    return u.SMAA_RT_METRICS.x * searchCorrection(e, false) + texcoord.x;
  }
  fn SMAASearchXRight(tc: vec2f, end: f32) -> f32 {
    var texcoord = tc;
    var e = vec2f(0.0, 1.0);
    for (var i = 0; i < 64; i++) {
      if (!(texcoord.x < end && e.g > 0.8281 && e.r == 0.0)) { break; }
      e = SMP(u_edges, texcoord).rg;
      texcoord = vec2f(2.0, 0.0) * u.SMAA_RT_METRICS.xy + texcoord;
    }
    return -u.SMAA_RT_METRICS.x * searchCorrection(e, true) + texcoord.x;
  }
  fn SMAASearchYUp(tc: vec2f, end: f32) -> f32 {
    var texcoord = tc;
    var e = vec2f(1.0, 0.0);
    for (var i = 0; i < 64; i++) {
      if (!(texcoord.y > end && e.r > 0.8281 && e.g == 0.0)) { break; }
      e = SMP(u_edges, texcoord).rg;
      texcoord = -vec2f(0.0, 2.0) * u.SMAA_RT_METRICS.xy + texcoord;
    }
    return u.SMAA_RT_METRICS.y * searchCorrection(e.gr, false) + texcoord.y;
  }
  fn SMAASearchYDown(tc: vec2f, end: f32) -> f32 {
    var texcoord = tc;
    var e = vec2f(1.0, 0.0);
    for (var i = 0; i < 64; i++) {
      if (!(texcoord.y < end && e.r > 0.8281 && e.g == 0.0)) { break; }
      e = SMP(u_edges, texcoord).rg;
      texcoord = vec2f(0.0, 2.0) * u.SMAA_RT_METRICS.xy + texcoord;
    }
    return -u.SMAA_RT_METRICS.y * searchCorrection(e.gr, true) + texcoord.y;
  }
  fn SMAAArea(dist: vec2f, e1: f32, e2: f32) -> vec2f {
    var tc = vec2f(SMAA_AREATEX_MAX_DISTANCE) * floor(4.0 * vec2f(e1, e2) + 0.5) + dist;
    tc = (tc + 0.5) / (5.0 * SMAA_AREATEX_MAX_DISTANCE);
    return textureSampleLevel(u_area, cw_linear, tc, 0.0).rg;
  }
  fn SMAADetectHorizontalCornerPattern(w: vec2f, texcoord: vec4f, d: vec2f) -> vec2f {
    let leftRight = step(d.xy, d.yx);
    var rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding = rounding / (leftRight.x + leftRight.y);
    var factor = vec2f(1.0);
    factor.x -= rounding.x * SMPO(u_edges, texcoord.xy, vec2f(0.0, 1.0)).r;
    factor.x -= rounding.y * SMPO(u_edges, texcoord.zw, vec2f(1.0, 1.0)).r;
    factor.y -= rounding.x * SMPO(u_edges, texcoord.xy, vec2f(0.0, -2.0)).r;
    factor.y -= rounding.y * SMPO(u_edges, texcoord.zw, vec2f(1.0, -2.0)).r;
    return w * clamp(factor, vec2f(0.0), vec2f(1.0));
  }
  fn SMAADetectVerticalCornerPattern(w: vec2f, texcoord: vec4f, d: vec2f) -> vec2f {
    let leftRight = step(d.xy, d.yx);
    var rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding = rounding / (leftRight.x + leftRight.y);
    var factor = vec2f(1.0);
    factor.x -= rounding.x * SMPO(u_edges, texcoord.xy, vec2f(1.0, 0.0)).g;
    factor.x -= rounding.y * SMPO(u_edges, texcoord.zw, vec2f(1.0, 1.0)).g;
    factor.y -= rounding.x * SMPO(u_edges, texcoord.xy, vec2f(-2.0, 0.0)).g;
    factor.y -= rounding.y * SMPO(u_edges, texcoord.zw, vec2f(-2.0, 1.0)).g;
    return w * clamp(factor, vec2f(0.0), vec2f(1.0));
  }
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let M = u.SMAA_RT_METRICS;
    let texcoord = FL(uv);
    let pixcoord = texcoord * M.zw;
    let offset0 = M.xyxy * vec4f(-0.25, -0.125, 1.25, -0.125) + texcoord.xyxy;
    let offset1 = M.xyxy * vec4f(-0.125, -0.25, -0.125, 1.25) + texcoord.xyxy;
    let offset2 = M.xxyy * (vec4f(-2.0, 2.0, -2.0, 2.0) * f32(SMAA_MAX_SEARCH_STEPS)) + vec4f(offset0.xz, offset1.yw);
    var weights = vec4f(0.0);
    let e = SMP(u_edges, texcoord).rg;
    if (e.r <= 0.0 && e.g <= 0.0) { discard; } /* no edge: the cleared target's zero weights */
    if (e.g > 0.0) {
      var coords: vec3f;
      coords.x = SMAASearchXLeft(offset0.xy, offset2.x);
      coords.y = offset1.y;
      var d: vec2f;
      d.x = coords.x;
      let e1 = SMP(u_edges, coords.xy).r;
      coords.z = SMAASearchXRight(offset0.zw, offset2.y);
      d.y = coords.z;
      d = floor(abs(M.zz * d - pixcoord.xx) + 0.5);
      let sqrt_d = sqrt(d);
      let e2 = SMPO(u_edges, coords.zy, vec2f(1.0, 0.0)).r;
      var w = SMAAArea(sqrt_d, e1, e2);
      coords.y = texcoord.y;
      w = SMAADetectHorizontalCornerPattern(w, coords.xyzy, d);
      weights.r = w.x;
      weights.g = w.y;
    }
    if (e.r > 0.0) {
      var coords: vec3f;
      coords.y = SMAASearchYUp(offset1.xy, offset2.z);
      coords.x = offset0.x;
      var d: vec2f;
      d.x = coords.y;
      let e1 = SMP(u_edges, coords.xy).g;
      coords.z = SMAASearchYDown(offset1.zw, offset2.w);
      d.y = coords.z;
      d = floor(abs(M.ww * d - pixcoord.yy) + 0.5);
      let sqrt_d = sqrt(d);
      let e2 = SMPO(u_edges, coords.xz, vec2f(0.0, 1.0)).g;
      var w = SMAAArea(sqrt_d, e1, e2);
      coords.x = texcoord.x;
      w = SMAADetectVerticalCornerPattern(w, coords.xyxz, d);
      weights.b = w.x;
      weights.a = w.y;
    }
    return weights;
  });

WGSL_DECL WGSL_SMAA_BLEND_FS = S(
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f { return smaa_blend(uv); });
/* the blend with the lens fused in (its block: WGSL_SMAA_LENS_U) */
WGSL_DECL WGSL_SMAA_BLEND_LENS_FS = S(
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    return fuse_lens(smaa_blend(uv), uv, pos);
  });
WGSL_DECL WGSL_SMAA_BLEND = S(
  @group(0) @binding(1) var u_color: texture_2d<f32>;
  @group(0) @binding(2) var u_blend: texture_2d<f32>;
  fn smaa_blend(uv: vec2f) -> vec4f {
    let M = u.SMAA_RT_METRICS;
    let texcoord = FL(uv);
    let offset = M.xyxy * vec4f(1.0, 0.0, 0.0, 1.0) + texcoord.xyxy;
    var a: vec4f;
    a.x = SMP(u_blend, offset.xy).a;
    a.y = SMP(u_blend, offset.zw).g;
    let s = SMP(u_blend, texcoord);
    a.w = s.x;
    a.z = s.z;
    if (dot(a, vec4f(1.0)) < 1e-5) { return SMP(u_color, texcoord); }
    let h = max(a.x, a.z) > max(a.y, a.w);
    let blendingOffset = select(vec4f(0.0, a.y, 0.0, a.w), vec4f(a.x, 0.0, a.z, 0.0), h);
    var blendingWeight = select(a.yw, a.xz, h);
    blendingWeight = blendingWeight / dot(blendingWeight, vec2f(1.0));
    let blendingCoord = blendingOffset * vec4f(M.xy, -M.xy) + texcoord.xyxy;
    var color = blendingWeight.x * SMP(u_color, blendingCoord.xy);
    color += blendingWeight.y * SMP(u_color, blendingCoord.zw);
    return color;
  });

/* ── height fog (fog.c) ─────────────────────────────────────────────── */

/* (in-scattered light, opacity) premultiplied, or alpha -1 where there is no fog */
WGSL_DECL WGSL_FOG_FN = S(
  fn opticalDepth(v: vec3f, l: f32) -> f32 {
    let a = u.u_fog.x * exp(-u.u_fog.y * (u.u_cam.y - u.u_fog.z));
    let k = u.u_fog.y * v.y * l;
    return a * l * select(1.0 - 0.5 * k, (1.0 - exp(-k)) / k, abs(k) > 1e-4);
  }
  fn fog_eval(uv: vec2f, d: f32) -> vec4f {
    let a = u.u_inv_view_proj * vec4f(uv * 2.0 - 1.0, -1.0, 1.0);
    let b = u.u_inv_view_proj * vec4f(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    let p0 = a.xyz / a.w;
    let ray = b.xyz / b.w - u.u_cam;
    var len = length(ray);
    var v = ray / max(len, 1e-6);
    if (d >= 1.0) {
      if (u.u_dist.y <= 0.0) { return vec4f(-1.0); }
      v = normalize(b.xyz / b.w - p0);
      len = u.u_dist.y;
    }
    let l = max(len - u.u_dist.x, 0.0);
    if (l <= 0.0) { return vec4f(-1.0); }
    let tau = opticalDepth(v, l);
    let fog = min(1.0 - exp(-tau), u.u_fog.w);
    let mu = dot(v, u.u_sun_dir);
    let g = u.u_g;
    let g2 = g * g;
    let hg = (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
    let sun = u.u_sun_color * hg * 4.0 * 3.14159265;
    let inscatter = (u.u_color + sun) * fog;
    return vec4f(inscatter, fog);
  });

WGSL_DECL WGSL_FOG = "struct U { " WGSL_FOG_FIELDS "u_pz: vec4f };\n" S(
  @group(0) @binding(0) var<uniform> u: U;
  @group(0) @binding(1) var u_depth: texture_2d<f32>;
  @fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {
    let f = fog_eval(uv, win_depth(cw_load(u_depth, uv).r, u.u_pz));
    if (f.a < 0.0) { discard; }
    return f;
  });

#endif
