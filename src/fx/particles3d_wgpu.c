/* GPUParticles3D on WebGPU: the simulation, the bitonic sort and the stats
 * reduction are COMPUTE passes over storage buffers; drawing is instanced
 * from the same buffer bound as a vertex buffer (compatibility mode has no
 * storage buffers in the vertex stage), or from a gathered (sorted) copy;
 * trails read the state and the history through textures copied from their
 * buffers; gpu_stats arrive by an asynchronous map (one frame late natively,
 * between frames in carts).
 *
 * The model is the GL path's (particles3d.c) line for line: the same
 * emission function (hashes of seed, index and cycle), the same forces,
 * colliders and depth collision, the same draw maths. What differs:
 *   - state is updated in place (one buffer, pos+age then vel+cycle per
 *     particle, 32 bytes) instead of transform-feedback ping-pong;
 *   - depth collision and soft particles read the pipeline's LINEAR depth
 *     (R32Float view distance) and turn it into GL window depth with the
 *     projection, so the collision maths stays GL's;
 *   - the stats reduction sums in a different order than GL's 8x8 blocks:
 *     alive counts are exact (integers), position sums agree to float
 *     rounding;
 *   - the sort compares (key, index) pairs in place, n/2 threads a pass, the
 *     same passes and the same total order as GL's fragment passes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "particles3d_internal.h"
#include "cartwheel/quality.h"
#include "cartwheel/profile.h"

#if CW_WGPU

#define SV CW_WGPU_SV
#define SYS_SIZE 2832 /* 1792, then the burst count (16) and 32 + 32 burst slots (1024) */
#define SYS_BASE 1808
#define STEP_STRIDE 256
#define DRAW_SIZE 352
#define PASS_STRIDE 256

/* ── WGSL ─────────────────────────────────────────────────────────────── */

static const char WGSL_SYS[] =
    "struct Sys {\n"
    "  a: vec4u, b: vec4u, life: vec4f, ext: vec4f, rad: vec4f, dir: vec4f, speed: vec4f, gravity: vec4f,\n"
    "  turb: vec4f, coll: vec4f, campos: vec4f, depth: vec4f,\n"
    "  vp: mat4x4f, ivp: mat4x4f, proj: mat4x4f,\n"
    "  attr: array<vec4f, 16>, col_inv: array<mat4x4f, 8>, col_m: array<mat4x4f, 8>, col_p: array<vec4f, 8>,\n"
    "  bursts: vec4u, burst: array<vec4f, 32>, burst_d: array<vec4f, 32>,\n"
    "}\n"
    "struct Step { t: vec4f, h: vec4i, em0: mat4x4f, em1: mat4x4f }\n"
    "struct P { pa: vec4f, vc: vec4f }\n";

/* the simulation: particles.sim of the GL path */
static const char WGSL_SIM[] =
    "@group(0) @binding(0) var<uniform> S: Sys;\n"
    "@group(0) @binding(1) var<uniform> T: Step;\n"
    "@group(0) @binding(2) var<storage, read_write> st: array<P>;\n"
    "@group(0) @binding(3) var<storage, read_write> hist: array<vec4f>;\n"
    "@group(0) @binding(4) var<storage, read> emitp: array<vec4f>;\n"
    "@group(0) @binding(5) var noise: texture_3d<f32>;\n"
    "@group(0) @binding(6) var nsamp: sampler;\n"
    "@group(0) @binding(7) var depth: texture_2d<f32>;\n"
    "@group(0) @binding(8) var<storage, read> pst: array<P>;\n"
    "var<private> g_par: bool = false; var<private> g_ppos: vec3f; var<private> g_pvel: vec3f; var<private> g_dir: vec3f;\n"
    "fn emit_shape(s: ptr<function, u32>, p: ptr<function, vec3f>, n: ptr<function, vec3f>) {\n"
    "  *p = vec3f(0.0); *n = vec3f(0.0);\n"
    "  let shape = S.a.z;\n"
    "  if (shape == 1u || shape == 2u) {\n"
    "    let z = prnd(s) * 2.0 - 1.0; let a = prnd(s) * 6.2831853; let r = sqrt(max(0.0, 1.0 - z * z));\n"
    "    let d = vec3f(r * cos(a), z, r * sin(a));\n"
    "    if (shape == 1u) { *p = d * (S.rad.x * pow(prnd(s), 1.0 / 3.0)); } else { *p = d * S.rad.x; *n = d; }\n"
    "  } else if (shape == 3u) {\n"
    "    let x = prnd(s); let y = prnd(s); let z = prnd(s);\n"
    "    *p = (vec3f(x, y, z) * 2.0 - 1.0) * S.ext.xyz;\n"
    "  } else if (shape == 4u) {\n"
    "    let a = prnd(s) * 6.2831853; let r = sqrt(mix(S.rad.y * S.rad.y, S.rad.x * S.rad.x, prnd(s)));\n"
    "    *p = vec3f(r * cos(a), 0.0, r * sin(a));\n"
    "  } else if (shape == 5u) {\n"
    "    let j = u32(min(i32(prnd(s) * 4096.0), 4095));\n"
    "    *p = emitp[j].xyz; *n = emitp[4096u + j].xyz;\n"
    "  } else if (shape == 6u && S.b.w > 0u) {\n"
    "    for (var q = 0; q < 6; q++) {\n"
    "      let j = min(u32(prnd(s) * f32(S.b.w)), S.b.w - 1u);\n"
    "      let a = pst[j];\n"
    "      if (a.pa.w >= 0.0) { g_par = true; g_ppos = a.pa.xyz; g_pvel = a.vc.xyz * S.speed.z; break; }\n"
    "    }\n"
    "  }\n"
    "}\n"
    "fn cone(s: ptr<function, u32>) -> vec3f {\n"
    "  let ct = mix(1.0, S.rad.z, prnd(s)); let st_ = sqrt(max(0.0, 1.0 - ct * ct));\n"
    "  let a = prnd(s) * 6.2831853;\n"
    "  let w = g_dir; let up = select(vec3f(1.0, 0.0, 0.0), vec3f(0.0, 1.0, 0.0), abs(w.y) < 0.999);\n"
    "  let u = normalize(cross(up, w)); let v = cross(w, u);\n"
    "  return u * (st_ * cos(a)) + v * (st_ * sin(a)) + w * ct;\n"
    "}\n"
    "fn forces(p: vec3f, t: f32) -> vec3f {\n"
    "  var a = S.gravity.xyz;\n"
    "  for (var j = 0u; j < 8u; j++) {\n"
    "    if (j >= S.a.w) { break; }\n"
    "    let A = S.attr[2u * j]; let B = S.attr[2u * j + 1u];\n"
    "    let d = A.xyz - p; let l = length(d);\n"
    "    if (l < A.w && l > 1e-4) { a += d / l * (B.x * pow(1.0 - l / A.w, B.y)); }\n"
    "  }\n"
    "  if (S.turb.x != 0.0) {\n"
    "    a += (textureSampleLevel(noise, nsamp, p * S.turb.y + vec3f(0.0, t * S.turb.z, 0.0), 0.0).xyz * 2.0 - 1.0) * S.turb.x;\n"
    "  }\n"
    "  return a;\n"
    "}\n"
    "fn respond(v: ptr<function, vec3f>, n: vec3f) {\n"
    "  let vn = dot(*v, n);\n"
    "  if (vn < 0.0) { let vt = *v - vn * n; *v = vt * (1.0 - S.coll.y) - vn * S.coll.x * n; }\n"
    "}\n"
    "fn colliders(p: ptr<function, vec3f>, v: ptr<function, vec3f>, pprev: vec3f) {\n"
    "  let r = S.coll.z;\n"
    "  for (var j = 0u; j < 8u; j++) {\n"
    "    if (j >= S.b.x) { break; }\n"
    "    let P = S.col_p[j];\n"
    "    var lp = (S.col_inv[j] * vec4f(*p, 1.0)).xyz;\n"
    "    var ln = vec3f(0.0); var hit = false;\n"
    "    if (P.x < 0.5) {\n"
    "      let lq = (S.col_inv[j] * vec4f(pprev, 1.0)).xyz;\n"
    "      let inside = (P.y <= 0.0 || abs(lp.x) <= P.y) && (P.w <= 0.0 || abs(lp.z) <= P.w);\n"
    "      if (inside && lp.y < r && lq.y >= -r) { lp.y = r; ln = vec3f(0.0, 1.0, 0.0); hit = true; }\n"
    "    } else if (P.x < 1.5) {\n"
    "      let l = length(lp);\n"
    "      if (l < P.y + r) { ln = select(vec3f(0.0, 1.0, 0.0), lp / l, l > 1e-6); lp = ln * (P.y + r); hit = true; }\n"
    "    } else {\n"
    "      let e = P.yzw + vec3f(r); let q = abs(lp);\n"
    "      if (q.x < e.x && q.y < e.y && q.z < e.z) {\n"
    "        let pen = e - q;\n"
    "        if (pen.x <= pen.y && pen.x <= pen.z) { ln = vec3f(sign(lp.x), 0.0, 0.0); lp.x = sign(lp.x) * e.x; }\n"
    "        else if (pen.y <= pen.z) { ln = vec3f(0.0, sign(lp.y), 0.0); lp.y = sign(lp.y) * e.y; }\n"
    "        else { ln = vec3f(0.0, 0.0, sign(lp.z)); lp.z = sign(lp.z) * e.z; }\n"
    "        hit = true;\n"
    "      }\n"
    "    }\n"
    "    if (hit) {\n"
    "      *p = (S.col_m[j] * vec4f(lp, 1.0)).xyz;\n"
    "      let n = normalize(m3(S.col_m[j]) * ln);\n"
    "      respond(v, n);\n"
    "    }\n"
    "  }\n"
    "}\n"
    /* GL window depth at uv (GL convention: y up) from the linear depth */
    "fn wdepth(uv: vec2f) -> f32 {\n"
    "  let w = i32(S.depth.x); let h = i32(S.depth.y);\n"
    "  let x = clamp(i32(floor(uv.x * S.depth.x)), 0, w - 1);\n"
    "  var y = clamp(i32(floor(uv.y * S.depth.y)), 0, h - 1);\n"
    "  if (S.depth.z < 0.5) { y = h - 1 - y; }\n"
    "  let z = textureLoad(depth, vec2i(x, y), 0).r;\n"
    "  if (z <= 0.0 || z > 1e30) { return 1.0; }\n"
    "  let cz = S.proj[2].z * -z + S.proj[3].z; let cw = S.proj[2].w * -z + S.proj[3].w;\n"
    "  return (cz / cw) * 0.5 + 0.5;\n"
    "}\n"
    "fn unproject(uv: vec2f, d: f32) -> vec3f {\n"
    "  let w = S.ivp * vec4f(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "  return w.xyz / w.w;\n"
    "}\n"
    "fn depth_collide(p: ptr<function, vec3f>, v: ptr<function, vec3f>) {\n"
    "  let c = S.vp * vec4f(*p, 1.0);\n"
    "  if (c.w <= 0.0) { return; }\n"
    "  let ndc = c.xyz / c.w;\n"
    "  if (abs(ndc.x) >= 0.995 || abs(ndc.y) >= 0.995) { return; }\n"
    "  let uv = ndc.xy * 0.5 + 0.5;\n"
    "  let ds = wdepth(uv);\n"
    "  if (ds >= 1.0) { return; }\n"
    "  let Sp = unproject(uv, ds);\n"
    "  let dp = distance(S.campos.xyz, *p); let dsur = distance(S.campos.xyz, Sp);\n"
    "  if (dp < dsur - 2.0 * S.coll.z - 0.05 || dp > dsur + S.coll.w) { return; }\n"
    "  let ox = vec2f(1.0 / S.depth.x, 0.0); let oy = vec2f(0.0, 1.0 / S.depth.y);\n"
    "  let Sx1 = unproject(uv + ox, wdepth(uv + ox)); let Sx0 = unproject(uv - ox, wdepth(uv - ox));\n"
    "  let Sy1 = unproject(uv + oy, wdepth(uv + oy)); let Sy0 = unproject(uv - oy, wdepth(uv - oy));\n"
    "  let dx = select(Sp - Sx0, Sx1 - Sp, distance(Sx1, Sp) < distance(Sx0, Sp));\n"
    "  let dy = select(Sp - Sy0, Sy1 - Sp, distance(Sy1, Sp) < distance(Sy0, Sp));\n"
    "  var n = cross(dx, dy);\n"
    "  if (dot(n, n) < 1e-20) { return; }\n"
    "  n = normalize(n);\n"
    "  if (dot(n, S.campos.xyz - Sp) < 0.0) { n = -n; }\n"
    "  let dist = dot(*p - Sp, n);\n"
    "  if (dist >= S.coll.z || dist < -S.coll.w) { return; }\n"
    "  *p += n * (S.coll.z - dist);\n"
    "  respond(v, n);\n"
    "}\n"
    "@compute @workgroup_size(64) fn sim(@builtin(global_invocation_id) gid: vec3u) {\n"
    "  let iu = gid.x + gid.y * 2097152u;\n"
    "  if (iu >= S.a.y) { return; }\n"
    "  let i = i32(iu);\n"
    "  let L = S.life.x;\n"
    "  let pa = st[iu].pa; let vc = st[iu].vc;\n"
    "  g_dir = S.dir.xyz;\n"
    "  var M0 = T.em0; var M1 = T.em1;\n"
    "  var k = -1; var spawn = 0.0; var age = 0.0; var ok = false;\n"
    "  let nb = i32(S.bursts.x);\n"
    "  if (nb > 0) {\n"
    "    let per = max(i32(S.a.y) / nb, 1); let slot = i / per; let li = i - slot * per;\n"
    "    var B = vec4f(0.0, 0.0, 0.0, -1.0); var D = vec4f(0.0);\n"
    "    if (slot < nb) { B = S.burst[slot]; D = S.burst_d[slot]; }\n"
    "    let off = f32(li) / f32(per) * L * (1.0 - S.life.z);\n"
    "    let rel = T.t.y - B.w - off;\n"
    "    k = i32(D.w);\n"
    "    ok = slot < nb && li < i32(S.bursts.y) && B.w >= 0.0 && rel >= 0.0 && rel < L;\n"
    "    spawn = B.w + off;\n"
    "    age = rel;\n"
    "    M1[3] = vec4f(B.xyz, 1.0); M0 = M1;\n"
    "    if (dot(D.xyz, D.xyz) > 0.0) { g_dir = normalize(D.xyz); }\n"
    "  } else {\n"
    "  let off = f32(i) / f32(S.a.y) * L * (1.0 - S.life.z);\n"
    "  let rel = T.t.y - off;\n"
    "  if (rel >= 0.0) { k = i32(floor(rel / L)); }\n"
    "  spawn = off + f32(k) * L;\n"
    "  ok = k >= 0 && (S.life.w < 0.0 || spawn < S.life.w);\n"
    "  age = rel - f32(k) * L;\n"
    "  }\n"
    "  var life = 0.0;\n"
    "  if (ok) { life = plife(S.a.x, L, S.life.y, i, k); }\n"
    "  var opa: vec4f; var ovc: vec4f;\n"
    "  if (!ok || age >= life) {\n"
    "    opa = vec4f(pa.xyz, -1.0); ovc = vc;\n"
    "  } else if (pa.w < 0.0 || i32(vc.w) != k) {\n"
    "    var s = pseed(S.a.x, i, k); _ = prnd(&s);\n"
    "    let f = clamp((spawn - T.t.x) / max(T.t.y - T.t.x, 1e-6), 0.0, 1.0);\n"
    "    let M = M0 * (1.0 - f) + M1 * f;\n"
    "    var lp: vec3f; var ln: vec3f; emit_shape(&s, &lp, &ln);\n"
    "    let d = cone(&s);\n"
    "    let sp = mix(S.speed.x, S.speed.y, prnd(&s));\n"
    "    var p = select((M * vec4f(lp, 1.0)).xyz, g_ppos, g_par);\n"
    "    let wd = m3(M) * d;\n"
    "    var v = select(wd, normalize(wd), dot(wd, wd) > 0.0) * sp + select(vec3f(0.0), g_pvel, g_par);\n"
    "    let wn = m3(M) * ln;\n"
    "    if (dot(wn, wn) > 0.0) { v += normalize(wn) * S.ext.w; }\n"
    "    v += S.gravity.xyz * age;\n"
    "    p += v * age;\n"
    "    opa = vec4f(p, age); ovc = vec4f(v, f32(k));\n"
    "  } else {\n"
    "    let dt = T.t.y - T.t.x;\n"
    "    var p = pa.xyz; var v = vc.xyz; let pprev = p;\n"
    "    v += forces(p, T.t.y) * dt;\n"
    "    v *= 1.0 / (1.0 + S.rad.w * dt);\n"
    "    p += v * dt;\n"
    "    colliders(&p, &v, pprev);\n"
    "    if (S.b.y == 1u) { depth_collide(&p, &v); }\n"
    "    opa = vec4f(p, age); ovc = vec4f(v, vc.w);\n"
    "  }\n"
    "  st[iu] = P(opa, ovc);\n"
    "  if (T.h.x >= 0) { hist[u32(T.h.x) * S.b.z + iu] = vec4f(opa.xyz, select(ovc.w, -1.0, opa.w < 0.0)); }\n"
    "}\n";

/* the sort (key, bitonic passes, gather) and the stats reduction */
static const char WGSL_SORT[] =
    "struct K { key: f32, idx: u32 }\n"
    "struct SP { pa: vec4f, vc: vec4f, ix: vec4u }\n"
    "struct SU { a: vec4u, mv: mat4x4f }\n"
    "@group(0) @binding(0) var<uniform> U: SU;\n"
    "@group(0) @binding(1) var<uniform> J: vec4u;\n"
    "@group(0) @binding(2) var<storage, read_write> keys: array<K>;\n"
    "@group(0) @binding(3) var<storage, read> st: array<P>;\n"
    "@group(0) @binding(4) var<storage, read_write> sorted: array<SP>;\n"
    "@compute @workgroup_size(64) fn key(@builtin(global_invocation_id) gid: vec3u) {\n"
    "  let idx = gid.x + gid.y * 2097152u;\n"
    "  if (idx >= U.a.y) { return; }\n"
    "  var k = 3.0e38;\n"
    "  if (idx < U.a.x) { let pa = st[idx].pa; if (pa.w >= 0.0) { k = (U.mv * vec4f(pa.xyz, 1.0)).z; } }\n"
    "  keys[idx] = K(k, idx);\n"
    "}\n"
    "fn less(a: K, b: K) -> bool { return a.key < b.key || (a.key == b.key && a.idx < b.idx); }\n"
    "@compute @workgroup_size(64) fn bitonic(@builtin(global_invocation_id) gid: vec3u) {\n"
    "  let t = gid.x + gid.y * 2097152u;\n"
    "  if (t >= U.a.y / 2u) { return; }\n"
    "  let j = J.x; let k = J.y;\n"
    "  let i = 2u * j * (t / j) + (t % j); let p = i + j;\n"
    "  let a = keys[i]; let b = keys[p];\n"
    "  let asc = (i & k) == 0u;\n"
    "  var swap = select(less(a, b), less(b, a), asc);\n"
    "  if (U.a.z == 1u) { swap = !swap; }\n" /* the must-fail control: the wrong order */
    "  if (swap) { keys[i] = b; keys[p] = a; }\n"
    "}\n"
    /* the whole sort in one workgroup's memory (up to 1024 keys): the same
     * compare-exchanges as the passes above, so the same order, in one
     * dispatch instead of one a pass */
    "var<workgroup> sk: array<K, 1024>;\n"
    "@compute @workgroup_size(128) fn bitonic_local(@builtin(local_invocation_index) li: u32) {\n"
    "  let n = U.a.y;\n"
    "  for (var t = li; t < n; t += 128u) { sk[t] = keys[t]; }\n"
    "  workgroupBarrier();\n"
    "  for (var k = 2u; k <= n; k = k << 1u) {\n"
    "    for (var j = k >> 1u; j > 0u; j = j >> 1u) {\n"
    "      for (var t = li; t < n / 2u; t += 128u) {\n"
    "        let i = 2u * j * (t / j) + (t % j); let p = i + j;\n"
    "        let a = sk[i]; let b = sk[p];\n"
    "        let asc = (i & k) == 0u;\n"
    "        var swap = select(less(a, b), less(b, a), asc);\n"
    "        if (U.a.z == 1u) { swap = !swap; }\n"
    "        if (swap) { sk[i] = b; sk[p] = a; }\n"
    "      }\n"
    "      workgroupBarrier();\n"
    "    }\n"
    "  }\n"
    "  for (var t = li; t < n; t += 128u) { keys[t] = sk[t]; }\n"
    "}\n"
    "@compute @workgroup_size(64) fn gather(@builtin(global_invocation_id) gid: vec3u) {\n"
    "  let n = gid.x + gid.y * 2097152u;\n"
    "  if (n >= U.a.x) { return; }\n"
    "  let kk = keys[n];\n"
    "  if (kk.idx < U.a.x) { sorted[n] = SP(st[kk.idx].pa, st[kk.idx].vc, vec4u(kk.idx, 0u, 0u, 0u)); }\n"
    "  else { sorted[n] = SP(vec4f(0.0, 0.0, 0.0, -1.0), vec4f(0.0), vec4u(kk.idx, 0u, 0u, 0u)); }\n"
    "}\n";

static const char WGSL_REDUCE[] =
    "struct Part { s: vec4f, n: vec4u }\n"
    "@group(0) @binding(0) var<uniform> R: vec4u;\n" /* amount, groups, result index */
    "@group(0) @binding(1) var<storage, read> st: array<P>;\n"
    "@group(0) @binding(2) var<storage, read_write> part: array<Part>;\n"
    "var<workgroup> ws: array<vec4f, 64>;\n"
    "var<workgroup> wn: array<u32, 64>;\n"
    "fn tree(li: u32) {\n"
    "  workgroupBarrier();\n"
    "  for (var o = 32u; o > 0u; o = o >> 1u) {\n"
    "    if (li < o) { ws[li] += ws[li + o]; wn[li] += wn[li + o]; }\n"
    "    workgroupBarrier();\n"
    "  }\n"
    "}\n"
    "@compute @workgroup_size(64) fn reduce1(@builtin(workgroup_id) wg: vec3u, @builtin(local_invocation_index) li: u32) {\n"
    "  let g = wg.x + wg.y * 32768u;\n"
    "  var s = vec3f(0.0); var n = 0u;\n"
    "  for (var q = 0u; q < 16u; q++) {\n"
    "    let i = g * 1024u + li * 16u + q;\n"
    "    if (i < R.x) { let pa = st[i].pa; if (pa.w >= 0.0) { s += pa.xyz; n += 1u; } }\n"
    "  }\n"
    "  ws[li] = vec4f(s, 0.0); wn[li] = n;\n"
    "  tree(li);\n"
    "  if (li == 0u && g < R.y) { part[g] = Part(ws[0], vec4u(wn[0], 0u, 0u, 0u)); }\n"
    "}\n"
    "@compute @workgroup_size(64) fn reduce2(@builtin(local_invocation_index) li: u32) {\n"
    "  var s = vec4f(0.0); var n = 0u;\n"
    "  for (var g = li; g < R.y; g += 64u) { s += part[g].s; n += part[g].n.x; }\n"
    "  ws[li] = s; wn[li] = n;\n"
    "  tree(li);\n"
    "  if (li == 0u) { part[R.z] = Part(ws[0], vec4u(wn[0], 0u, 0u, 0u)); }\n"
    "}\n";

/* drawing: particles (quads or meshes, unsorted or sorted) and trails */
static const char WGSL_DRAW[] =
    "struct Draw {\n"
    "  view: mat4x4f, proj: mat4x4f, model: mat4x4f,\n"
    "  campos: vec4f, color: vec4f, p0: vec4f, p1: vec4f, p2: vec4f, life: vec4f, ia: vec4u, ib: vec4u, p3: vec4f, p4: vec4f,\n"
    "}\n"
    "@group(0) @binding(0) var<uniform> D: Draw;\n"
    "@group(0) @binding(1) var curve: texture_2d<f32>;\n"
    "@group(0) @binding(2) var tex: texture_2d<f32>;\n"
    "@group(0) @binding(3) var samp: sampler;\n"
    "@group(0) @binding(4) var depth: texture_2d<f32>;\n"
    "@group(0) @binding(5) var stex: texture_2d<f32>;\n"
    "@group(0) @binding(6) var htex: texture_2d<f32>;\n"
    "@group(0) @binding(7) var scene: texture_2d<f32>;\n"
    "@group(0) @binding(8) var mvtex: texture_2d<f32>;\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) color: vec4f, @location(1) uv: vec2f,\n"
    "  @location(2) vz: f32, @location(3) n: vec3f, @location(4) t: f32, @location(5) id: f32, @location(6) luv: vec2f, @location(7) fr: vec2f }\n"
    "fn curve_at(t: f32, row: i32) -> vec4f {\n"
    "  let x = t * 255.0; let i0 = i32(floor(x)); let i1 = min(i0 + 1, 255);\n"
    "  return mix(textureLoad(curve, vec2i(i0, row), 0), textureLoad(curve, vec2i(i1, row), 0), x - f32(i0));\n"
    "}\n"
    "fn rot(v: vec3f, ax: vec3f, a: f32) -> vec3f {\n"
    "  let c = cos(a); let s = sin(a);\n"
    "  return v * c + cross(ax, v) * s + ax * dot(ax, v) * (1.0 - c);\n"
    "}\n"
    "fn body(i: i32, pa: vec4f, vc: vec4f, vert: vec3f, norm: vec3f) -> VO {\n"
    "  var o: VO;\n"
    "  if (pa.w < 0.0) { o.pos = vec4f(2.0, 2.0, 2.0, 1.0); return o; }\n"
    "  let k = i32(vc.w);\n"
    "  let life = plife(D.ia.x, D.life.x, D.life.y, i, k);\n"
    "  let t = clamp(pa.w / life, 0.0, 1.0);\n"
    "  var col = curve_at(t, 0) * D.color;\n"
    "  col = vec4f(col.rgb * D.campos.w, col.a);\n"
    "  var sz = curve_at(t, 1).x * D.p0.x;\n"
    "  var s = pseed(D.ia.x, i, k) ^ 0x5bd1e995u;\n"
    "  let r1 = prnd(&s); let r2 = prnd(&s); let r3 = prnd(&s);\n"
    "  sz *= 1.0 - D.p0.y * r1;\n"
    "  let ang = r2 * 6.2831853 + D.p0.w * select(1.0, -1.0, r3 < 0.5) * pa.w;\n"
    "  let wp = (D.model * vec4f(pa.xyz, 1.0)).xyz;\n"
    "  let wv = m3(D.model) * vc.xyz;\n"
    "  let right = vec3f(D.view[0][0], D.view[1][0], D.view[2][0]);\n"
    "  let up = vec3f(D.view[0][1], D.view[1][1], D.view[2][1]);\n"
    "  var world: vec3f; o.n = vec3f(0.0, 0.0, 1.0);\n"
    "  o.luv = vec2f(0.0); o.fr = vec2f(0.0);\n"
    "  let mode = i32(D.p1.z);\n"
    "  if (mode == 2) {\n"
    "    let ax = normalize(vec3f(r1 - 0.5, r2 - 0.5, r3 - 0.5) + vec3f(1e-3));\n"
    "    world = wp + rot(vert, ax, ang) * sz;\n"
    "    o.n = rot(norm, ax, ang);\n"
    "    o.uv = vec2f(0.5);\n"
    "  } else {\n"
    "    let sp = length(wv);\n"
    "    if (mode == 1 && sp > 1e-5) {\n"
    "      let axis = wv / sp;\n"
    "      var side = cross(axis, normalize(D.campos.xyz - wp));\n"
    "      side = select(right, normalize(side), dot(side, side) > 1e-10);\n"
    "      let len = sz + sp * D.p0.z;\n"
    "      world = wp + side * (vert.x * sz * 0.5) + axis * (vert.y * len * 0.5);\n"
    "    } else {\n"
    "      let c = cos(ang); let sn = sin(ang);\n"
    "      let q = vec2f(c * vert.x - sn * vert.y, sn * vert.x + c * vert.y);\n"
    "      world = wp + (right * q.x + up * q.y) * (sz * 0.5);\n"
    "    }\n"
    "    var uv = vec2f(vert.x * 0.5 + 0.5, 0.5 - vert.y * 0.5);\n"
    "    let nf = D.p1.x * D.p1.y;\n"
    "    if (nf > 1.0) {\n"
    "      let ft = t * nf; let f = min(floor(ft), nf - 1.0);\n"
    "      o.luv = uv; o.fr = vec2f(f, ft - f);\n"
    "      uv = (uv + vec2f(f - D.p1.x * floor(f / D.p1.x), floor(f / D.p1.x))) / D.p1.xy;\n"
    "    }\n"
    "    o.uv = uv;\n"
    "  }\n"
    "  let vp = D.view * vec4f(world, 1.0);\n"
    "  o.vz = -vp.z;\n"
    "  o.t = t; o.id = f32(i);\n"
    "  o.color = col;\n"
    "  o.pos = cw_clip(D.proj * vp, D.p2.y);\n"
    "  return o;\n"
    "}\n"
    "@vertex fn vs_quad(@builtin(instance_index) ii: u32, @location(0) pa: vec4f, @location(1) vc: vec4f,\n"
    "  @location(2) v: vec2f) -> VO { return body(i32(ii), pa, vc, vec3f(v, 0.0), vec3f(0.0, 0.0, 1.0)); }\n"
    "@vertex fn vs_quad_sorted(@location(0) pa: vec4f, @location(1) vc: vec4f, @location(4) ix: u32,\n"
    "  @location(2) v: vec2f) -> VO {\n"
    "  if (ix >= D.ia.y) { var o: VO; o.pos = vec4f(2.0, 2.0, 2.0, 1.0); return o; }\n"
    "  return body(i32(ix), pa, vc, vec3f(v, 0.0), vec3f(0.0, 0.0, 1.0)); }\n"
    "@vertex fn vs_mesh(@builtin(instance_index) ii: u32, @location(0) pa: vec4f, @location(1) vc: vec4f,\n"
    "  @location(2) v: vec3f, @location(3) n: vec3f) -> VO { return body(i32(ii), pa, vc, v, n); }\n"
    "@vertex fn vs_mesh_sorted(@location(0) pa: vec4f, @location(1) vc: vec4f, @location(4) ix: u32,\n"
    "  @location(2) v: vec3f, @location(3) n: vec3f) -> VO {\n"
    "  if (ix >= D.ia.y) { var o: VO; o.pos = vec4f(2.0, 2.0, 2.0, 1.0); return o; }\n"
    "  return body(i32(ix), pa, vc, v, n); }\n"
    "@fragment fn fs(in: VO) -> @location(0) vec4f {\n"
    "  var c = in.color;\n"
    "  let mode = i32(D.p1.z);\n"
    "  let tx = textureSample(tex, samp, in.uv);\n"
    "  if (mode == 2) { c = vec4f(c.rgb * (0.55 + 0.45 * max(dot(normalize(in.n), vec3f(0.3, 0.8, 0.5196)), 0.0)), c.a); }\n"
    "  else if (D.p4.x > 0.5 && D.p1.x * D.p1.y > 1.0) {\n"
    "    let cell = 1.0 / D.p1.xy; let nf = D.p1.x * D.p1.y;\n"
    "    let f0 = in.fr.x; let f1 = min(f0 + 1.0, nf - 1.0); let fr = in.fr.y;\n"
    "    let c0 = vec2f(f0 - D.p1.x * floor(f0 / D.p1.x), floor(f0 / D.p1.x)); let c1 = vec2f(f1 - D.p1.x * floor(f1 / D.p1.x), floor(f1 / D.p1.x));\n"
    "    let mA = (textureSampleLevel(mvtex, samp, (in.luv + c0) * cell, 0.0).rg * 2.0 - 1.0) * (0.12 * D.p4.y);\n"
    "    let mB = (textureSampleLevel(mvtex, samp, (in.luv + c1) * cell, 0.0).rg * 2.0 - 1.0) * (0.12 * D.p4.y);\n"
    "    let la = clamp(in.luv - mA * fr, vec2f(0.002), vec2f(0.998)); let lb = clamp(in.luv + mB * (1.0 - fr), vec2f(0.002), vec2f(0.998));\n"
    "    c *= mix(textureSampleLevel(tex, samp, (la + c0) * cell, 0.0), textureSampleLevel(tex, samp, (lb + c1) * cell, 0.0), fr);\n"
    "  }\n"
    "  else { c *= tx; }\n"
    "  if (D.p2.x > 0.0) {\n"
    "    let zs = textureLoad(depth, vec2i(in.pos.xy), 0).r;\n"
    "    c.a *= clamp((zs - in.vz) / D.p2.x, 0.0, 1.0);\n"
    "  }\n"
    "  let blend = i32(D.p1.w);\n"
    "  if (blend >= 3) {\n"
    "    let uv0 = in.pos.xy * D.p2.zw;\n"
    "    let zd = textureLoad(depth, vec2i(uv0 * vec2f(textureDimensions(depth))), 0).r;\n"
    "    var n: vec2f;\n"
    "    if (D.p3.y > 0.5) { n = tx.rg * 2.0 - 1.0; }\n"
    "    else if (blend == 3) { n = vec2f(sin(in.uv.y * 11.0 + in.t * 7.0 + in.id * 1.37), cos(in.uv.x * 9.0 - in.t * 6.0 + in.id * 2.11)); }\n"
    "    else { n = vec2f(in.uv.x - 0.5, 0.5 - in.uv.y) * 2.0; }\n"
    "    let off = n * c.a * D.p3.x * D.p3.zw / in.vz;\n"
    "    if (blend == 3) {\n"
    "      if (zd < in.vz - 0.02) { discard; }\n"
    "      return vec4f(off, in.vz * c.a, c.a);\n"
    "    }\n"
    "    var uv2 = uv0 + off;\n"
    "    if (textureLoad(depth, vec2i(uv2 * vec2f(textureDimensions(depth))), 0).r < in.vz - 0.02) { uv2 = uv0; }\n"
    "    return vec4f(textureSampleLevel(scene, samp, uv2, 0.0).rgb * in.color.rgb * c.a, c.a);\n"
    "  }\n"
    "  if (blend == 2) { if (c.a < 0.5) { discard; } return vec4f(c.rgb, 1.0); }\n"
    "  return vec4f(c.rgb * c.a, select(0.0, c.a, blend == 1));\n"
    "}\n"
    "fn hist_at(i: i32, j: i32) -> vec4f {\n"
    "  let hlen = i32(D.ia.w); let tw = i32(D.ia.z);\n"
    "  let slot = (i32(D.ib.x) - j + hlen) % hlen;\n"
    "  return textureLoad(htex, vec2i(i % tw, slot * i32(D.ib.z) + i / tw), 0);\n"
    "}\n"
    "fn sample_at(i: i32, j0: i32, cyc: f32) -> vec3f {\n"
    "  var j = min(j0, i32(D.ib.y) - 1);\n"
    "  for (var q = 0; q < 64; q++) {\n"
    "    if (j <= 0) { break; }\n"
    "    let h = hist_at(i, j);\n"
    "    if (h.w == cyc) { return h.xyz; }\n"
    "    j--;\n"
    "  }\n"
    "  return hist_at(i, 0).xyz;\n"
    "}\n"
    "@vertex fn vs_trail(@builtin(vertex_index) vid: u32, @builtin(instance_index) ii: u32) -> VO {\n"
    "  var o: VO;\n"
    "  let i = i32(ii); let seg = i32(vid >> 1u); let side = f32(vid & 1u) * 2.0 - 1.0;\n"
    "  let tw = i32(D.ia.z);\n"
    "  let pa = textureLoad(stex, vec2i(2 * (i % tw), i / tw), 0);\n"
    "  let vc = textureLoad(stex, vec2i(2 * (i % tw) + 1, i / tw), 0);\n"
    "  if (pa.w < 0.0 || i32(D.ib.y) < 2) { o.pos = vec4f(2.0, 2.0, 2.0, 1.0); return o; }\n"
    "  let cyc = vc.w;\n"
    "  let P = (D.model * vec4f(sample_at(i, seg, cyc), 1.0)).xyz;\n"
    "  let A = (D.model * vec4f(sample_at(i, seg - 1, cyc), 1.0)).xyz;\n"
    "  let B = (D.model * vec4f(sample_at(i, seg + 1, cyc), 1.0)).xyz;\n"
    "  var tg = A - B;\n"
    "  if (dot(tg, tg) < 1e-12) { tg = vec3f(0.0, 1e-3, 0.0); }\n"
    "  var sd = cross(normalize(tg), normalize(D.campos.xyz - P));\n"
    "  sd = select(vec3f(1.0, 0.0, 0.0), normalize(sd), dot(sd, sd) > 1e-10);\n"
    "  let u = f32(seg) / f32(max(i32(D.ia.w) - 1, 1));\n"
    "  let k = i32(cyc);\n"
    "  let t = clamp(pa.w / plife(D.ia.x, D.life.x, D.life.y, i, k), 0.0, 1.0);\n"
    "  var col = curve_at(t, 0) * D.color;\n"
    "  col = vec4f(col.rgb * D.campos.w, col.a * (1.0 - u));\n"
    "  let world = P + sd * (side * D.life.z * 0.5 * (1.0 - u));\n"
    "  let vp = D.view * vec4f(world, 1.0);\n"
    "  o.vz = -vp.z;\n"
    "  o.color = col;\n"
    "  o.uv = vec2f(side * 0.5 + 0.5, u);\n"
    "  o.pos = cw_clip(D.proj * vp, D.p2.y);\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs_trail(in: VO) -> @location(0) vec4f {\n"
    "  var c = in.color;\n"
    "  let e = 1.0 - abs(in.uv.x * 2.0 - 1.0);\n"
    "  c.a *= smoothstep(0.0, 0.35, e);\n"
    "  return vec4f(c.rgb * c.a, select(0.0, c.a, i32(D.life.w) == 1));\n"
    "}\n";

/* ── shared pipelines ─────────────────────────────────────────────────── */

typedef struct draw_pipe {
  uint32_t key;
  WGPURenderPipeline p;
} draw_pipe;

static struct {
  bool tried, ok;
  WGPUBindGroupLayout sim_bgl, sort_bgl, red_bgl, draw_bgl;
  WGPUPipelineLayout sim_pl, sort_pl, red_pl, draw_pl;
  WGPUComputePipeline sim, key, bitonic, bitonic_local, gather, red1, red2;
  WGPUShaderModule draw_sm;
  CW_ARRAY(draw_pipe) pipes;
  WGPUBuffer quad, dummy_storage, dummy_parent;
  int fault; /* tests: 1 broken sim shader, 2 wrong sort order, 3 colliders dropped */
} G;

void cw__fx_wgpu_test_fault(int fault);
void cw__fx_wgpu_test_fault(int fault) { G.fault = fault; }

static bool init_pipes(void) {
  if (G.tried) return G.ok;
  G.tried = true;
  WGPUShaderStage C = WGPUShaderStage_Compute, VF = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
  const cw__wg_slot sim[9] = {
    { CW_WB_UNIFORM, C, 0, SYS_SIZE }, { CW_WB_UNIFORM_DYN, C, 0, 160 }, { CW_WB_STORAGE, C, 0, 0 },
    { CW_WB_STORAGE, C, 0, 0 }, { CW_WB_STORAGE_RO, C, 0, 0 }, { CW_WB_TEX, C, WGPUTextureViewDimension_3D, 0 },
    { CW_WB_SAMPLER, C, 0, 0 }, { CW_WB_TEX_UNFILT, C, 0, 0 }, { CW_WB_STORAGE_RO, C, 0, 0 },
  };
  const cw__wg_slot sort[5] = { { CW_WB_UNIFORM, C, 0, 80 }, { CW_WB_UNIFORM_DYN, C, 0, 16 }, { CW_WB_STORAGE, C, 0, 0 },
                                { CW_WB_STORAGE_RO, C, 0, 0 }, { CW_WB_STORAGE, C, 0, 0 } };
  const cw__wg_slot red[3] = { { CW_WB_UNIFORM, C, 0, 16 }, { CW_WB_STORAGE_RO, C, 0, 0 }, { CW_WB_STORAGE, C, 0, 0 } };
  const cw__wg_slot draw[9] = { { CW_WB_UNIFORM, VF, 0, DRAW_SIZE }, { CW_WB_TEX_UNFILT, VF, 0, 0 },
                                { CW_WB_TEX, WGPUShaderStage_Fragment, 0, 0 }, { CW_WB_SAMPLER, WGPUShaderStage_Fragment, 0, 0 },
                                { CW_WB_TEX_UNFILT, WGPUShaderStage_Fragment, 0, 0 }, { CW_WB_TEX_UNFILT, WGPUShaderStage_Vertex, 0, 0 },
                                { CW_WB_TEX_UNFILT, WGPUShaderStage_Vertex, 0, 0 }, { CW_WB_TEX, WGPUShaderStage_Fragment, 0, 0 },
                                { CW_WB_TEX, WGPUShaderStage_Fragment, 0, 0 } };
  G.sim_bgl = cw__wg_layout(sim, 9);
  G.sort_bgl = cw__wg_layout(sort, 5);
  G.red_bgl = cw__wg_layout(red, 3);
  G.draw_bgl = cw__wg_layout(draw, 9);
  G.sim_pl = cw__wg_pipeline_layout(&G.sim_bgl, 1);
  G.sort_pl = cw__wg_pipeline_layout(&G.sort_bgl, 1);
  G.red_pl = cw__wg_pipeline_layout(&G.red_bgl, 1);
  G.draw_pl = cw__wg_pipeline_layout(&G.draw_bgl, 1);
  const char *const sim_parts[] = { WGSL_SYS, WGSL_SIM, G.fault == 1 ? "fn broken( {\n" : "", NULL };
  const char *const sort_parts[] = { WGSL_SYS, WGSL_SORT, NULL };
  const char *const red_parts[] = { WGSL_SYS, WGSL_REDUCE, NULL };
  const char *const draw_parts[] = { WGSL_DRAW, NULL };
  WGPUShaderModule sm_sim = cw__wg_module("particles.sim", sim_parts);
  WGPUShaderModule sm_sort = cw__wg_module("particles.sort", sort_parts);
  WGPUShaderModule sm_red = cw__wg_module("particles.reduce", red_parts);
  G.draw_sm = cw__wg_module("particles.draw", draw_parts);
  if (!sm_sim || !sm_sort || !sm_red || !G.draw_sm) {
    cw_error("GPUParticles3D: WebGPU shader build failed (logged above); particles are not drawn");
    return false;
  }
  G.sim = cw__wg_compute("particles.sim", sm_sim, "sim", G.sim_pl);
  G.key = cw__wg_compute("particles.sort_key", sm_sort, "key", G.sort_pl);
  G.bitonic = cw__wg_compute("particles.bitonic", sm_sort, "bitonic", G.sort_pl);
  G.bitonic_local = cw__wg_compute("particles.bitonic_local", sm_sort, "bitonic_local", G.sort_pl);
  G.gather = cw__wg_compute("particles.gather", sm_sort, "gather", G.sort_pl);
  G.red1 = cw__wg_compute("particles.reduce1", sm_red, "reduce1", G.red_pl);
  G.red2 = cw__wg_compute("particles.reduce2", sm_red, "reduce2", G.red_pl);
  wgpuShaderModuleRelease(sm_sim);
  wgpuShaderModuleRelease(sm_sort);
  wgpuShaderModuleRelease(sm_red);
  static const float q[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
  G.quad = cw__wg_buffer(sizeof q, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "particles quad");
  cw__wg_write(G.quad, 0, q, sizeof q);
  G.dummy_storage = cw__wg_buffer(64, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, "particles dummy");
  G.dummy_parent = cw__wg_buffer(64, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, "particles dummy parent");
  G.ok = G.sim && G.key && G.bitonic && G.bitonic_local && G.gather && G.red1 && G.red2;
  return G.ok;
}

enum { V_QUAD, V_QUAD_SORTED, V_MESH, V_MESH_SORTED, V_TRAIL };

static WGPURenderPipeline draw_pipe_for(int variant, int blend, const cw__wgpu_pass_targets *t) {
  uint32_t key = (uint32_t)variant | (uint32_t)blend << 3 | (uint32_t)t->flip_y << 6 | (uint32_t)(t->samples > 1) << 7 |
                 (uint32_t)t->color_format << 8 | (uint32_t)t->depth_format << 20;
  for (int i = 0; i < G.pipes.count; i++)
    if (G.pipes.data[i].key == key) return G.pipes.data[i].p;
  static const char *const vs[] = { "vs_quad", "vs_quad_sorted", "vs_mesh", "vs_mesh_sorted", "vs_trail" };
  bool sorted = variant == V_QUAD_SORTED || variant == V_MESH_SORTED, mesh = variant == V_MESH || variant == V_MESH_SORTED;
  WGPUVertexAttribute pattr[3] = { { .format = WGPUVertexFormat_Float32x4, .offset = 0, .shaderLocation = 0 },
                                   { .format = WGPUVertexFormat_Float32x4, .offset = 16, .shaderLocation = 1 },
                                   { .format = WGPUVertexFormat_Uint32, .offset = 32, .shaderLocation = 4 } };
  WGPUVertexAttribute qattr = { .format = WGPUVertexFormat_Float32x2, .offset = 0, .shaderLocation = 2 };
  WGPUVertexAttribute mattr[2] = { { .format = WGPUVertexFormat_Float32x3, .offset = 0, .shaderLocation = 2 },
                                   { .format = WGPUVertexFormat_Float32x3, .offset = 12, .shaderLocation = 3 } };
  WGPUVertexBufferLayout vb[2] = {
    { .stepMode = WGPUVertexStepMode_Instance, .arrayStride = sorted ? 48 : 32, .attributeCount = sorted ? 3 : 2,
      .attributes = pattr },
    mesh ? (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 24, .attributeCount = 2,
                                     .attributes = mattr }
         : (WGPUVertexBufferLayout){ .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 8, .attributeCount = 1,
                                     .attributes = &qattr },
  };
  int bp = variant == V_TRAIL ? (blend == BLEND_ADD ? CW_WG_BLEND_ADD : CW_WG_BLEND_PREMUL)
           : blend == BLEND_OPAQUE ? CW_WG_BLEND_NONE
           : blend == BLEND_DISTORT ? CW_WG_BLEND_ADD_ALL
           : blend == BLEND_ADD    ? CW_WG_BLEND_ADD
                                   : CW_WG_BLEND_PREMUL;
  WGPUColorTargetState ct = { .format = t->color_format, .blend = cw__wg_blend(bp), .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState fs = { .module = G.draw_sm, .entryPoint = SV(variant == V_TRAIL ? "fs_trail" : "fs"),
                           .targetCount = 1, .targets = &ct };
  bool write = variant != V_TRAIL && blend == BLEND_OPAQUE;
  WGPUDepthStencilState ds = { .format = t->depth_format,
                               .depthWriteEnabled = write ? WGPUOptionalBool_True : WGPUOptionalBool_False,
                               .depthCompare = WGPUCompareFunction_LessEqual,
                               .stencilFront = { .compare = WGPUCompareFunction_Always },
                               .stencilBack = { .compare = WGPUCompareFunction_Always } };
  WGPURenderPipelineDescriptor d = {
    .label = SV("particles.draw"),
    .layout = G.draw_pl,
    .vertex = { .module = G.draw_sm, .entryPoint = SV(vs[variant]), .bufferCount = variant == V_TRAIL ? 0 : 2,
                .buffers = vb },
    .primitive = { .topology = mesh ? WGPUPrimitiveTopology_TriangleList : WGPUPrimitiveTopology_TriangleStrip,
                   .frontFace = t->flip_y ? WGPUFrontFace_CW : WGPUFrontFace_CCW,
                   .cullMode = mesh ? WGPUCullMode_Back : WGPUCullMode_None },
    .depthStencil = t->depth ? &ds : NULL,
    .multisample = { .count = (uint32_t)(t->samples > 1 ? t->samples : 1), .mask = 0xffffffffu },
    .fragment = &fs,
  };
  WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
  cw_array_push(&G.pipes, ((draw_pipe){ key, p }));
  return p;
}

/* ── per system ───────────────────────────────────────────────────────── */

typedef struct pw {
  int cap, tw, trows, slots; /* slots = tw * trows (the state buffer's particles) */
  WGPUBuffer state, sys_u, step_u, draw_u, emit;
  /* trails */
  int hist_len;
  WGPUBuffer hist;
  WGPUTexture stex, htex;
  WGPUTextureView stex_v, htex_v;
  /* sort */
  int sort_n, npass;
  WGPUBuffer keys, sorted, sort_u, pass_u;
  /* stats */
  int groups;
  WGPUBuffer part, red_u;
  cw__wg_readback *rb;
  /* curves, mesh */
  WGPUTexture curve;
  WGPUTextureView curve_v;
  const char *baked_ramp, *baked_size;
  bool emit_ok;
  WGPUBuffer mesh_vb, mesh_ib;
  cw__wg_cache sim_bg, draw_bg; /* the same every frame unless a target or texture changes */
  /* what cw__psys_frame_wgpu left for cw__psys_draw_wgpu this run */
  t3_mat4 model;
  bool did_sort, trail, particles;
} pw;

static void rel_buf(WGPUBuffer *b) {
  if (*b) wgpuBufferRelease(*b);
  *b = NULL;
}
static void rel_tex(WGPUTexture *t, WGPUTextureView *v) {
  if (*v) wgpuTextureViewRelease(*v);
  if (*t) wgpuTextureRelease(*t);
  *t = NULL;
  *v = NULL;
}

static void free_hist(pw *w) {
  rel_buf(&w->hist);
  rel_tex(&w->htex, &w->htex_v);
  w->hist_len = 0;
}

static void free_pw(pw *w) {
  WGPUBuffer *bufs[] = { &w->state, &w->sys_u, &w->step_u, &w->draw_u, &w->emit, &w->keys, &w->sorted,
                         &w->sort_u, &w->pass_u, &w->part, &w->red_u, &w->mesh_vb, &w->mesh_ib };
  for (size_t i = 0; i < CW_ARRAY_LEN(bufs); i++) rel_buf(bufs[i]);
  free_hist(w);
  rel_tex(&w->stex, &w->stex_v);
  rel_tex(&w->curve, &w->curve_v);
  cw__wg_readback_free(w->rb);
  w->rb = NULL;
  cw__wg_cache_drop(&w->sim_bg);
  cw__wg_cache_drop(&w->draw_bg);
}

void cw__psys_free_wgpu(cw__psys *r) {
  if (!r || !r->wg) return;
  free_pw(r->wg);
  cw_free(r->wg);
  r->wg = NULL;
}

/* every particle dead (w = -1), history emptied */
static void upload_dead(cw__psys *r, pw *w) {
  size_t n = (size_t)w->slots * 8;
  float *a = cw_alloc(n * sizeof(float), CW_MEM_RENDER);
  for (size_t i = 0; i < n; i += 8) {
    memset(a + i, 0, 8 * sizeof(float));
    a[i + 3] = -1;
    a[i + 7] = -1;
  }
  cw__wg_write(w->state, 0, a, n * sizeof(float));
  cw_free(a);
  r->hist_count = 0;
  r->hist_head = 0;
}

void cw__psys_reset_wgpu(cw__psys *r) {
  if (r && r->wg) upload_dead(r, r->wg);
}

static pw *ensure(cw__psys *r, int amount) {
  pw *w = r->wg;
  if (w && w->cap == amount) return w;
  if (w) free_pw(w);
  else w = r->wg = cw_calloc(1, sizeof(pw), CW_MEM_RENDER);
  memset(w, 0, sizeof *w);
  w->cap = amount;
  r->cap = amount;
  /* trails copy state rows into a texture: a row of tw particles is tw * 32
   * bytes, a multiple of 256 when tw is a multiple of 8; history rows (tw *
   * 16 bytes) need tw a multiple of 16 */
  w->tw = CW_MIN((amount + 15) & ~15, 1024);
  w->trows = (amount + w->tw - 1) / w->tw;
  w->slots = w->tw * w->trows;
  w->state = cw__wg_buffer((size_t)w->slots * 32,
                           WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc,
                           "particles state");
  w->sys_u = cw__wg_buffer(SYS_SIZE, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "particles sys");
  w->step_u = cw__wg_buffer((size_t)STEP_STRIDE * MAX_STEPS, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                            "particles steps");
  w->draw_u = cw__wg_buffer(DRAW_SIZE, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "particles draw");
  w->emit = cw__wg_buffer((size_t)EMIT_N * 2 * 16, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, "particles emit");
  w->groups = (amount + 1023) / 1024;
  w->part = cw__wg_buffer((size_t)(w->groups + 1) * 32, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc,
                          "particles stats");
  w->red_u = cw__wg_buffer(16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "particles stats u");
  uint32_t ru[4] = { (uint32_t)amount, (uint32_t)w->groups, (uint32_t)w->groups, 0 };
  cw__wg_write(w->red_u, 0, ru, sizeof ru);
  r->mesh_built_kind = -1;
  r->mesh_count = 0;
  r->emit_built_node = 0;
  upload_dead(r, w);
  return w;
}

void cw__psys_mesh_upload_wgpu(cw__psys *r, const float *v, int nv, const uint32_t *idx, int ni) {
  pw *w = r->wg;
  if (!w) return;
  rel_buf(&w->mesh_vb);
  rel_buf(&w->mesh_ib);
  w->mesh_vb = cw__wg_buffer(sizeof(float) * 6 * (size_t)nv, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                             "particles mesh");
  w->mesh_ib = cw__wg_buffer(sizeof(uint32_t) * (size_t)ni, WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst,
                             "particles mesh index");
  cw__wg_write(w->mesh_vb, 0, v, sizeof(float) * 6 * (size_t)nv);
  cw__wg_write(w->mesh_ib, 0, idx, sizeof(uint32_t) * (size_t)ni);
}

static void ensure_curves(p3d *d, pw *w) {
  if (w->curve && w->baked_ramp == d->color_ramp && w->baked_size == d->size_curve) return;
  float *px = cw_alloc(sizeof(float) * 4 * CURVE_N * 2, CW_MEM_RENDER);
  cw__fx_curve_bake(d->color_ramp, 4, 1, CURVE_N, px, "GPUParticles3D.color_ramp");
  float sz[CURVE_N];
  cw__fx_curve_bake(d->size_curve, 1, 1, CURVE_N, sz, "GPUParticles3D.size_curve");
  for (int i = 0; i < CURVE_N; i++) {
    float *o = px + 4 * (CURVE_N + i);
    o[0] = sz[i];
    o[1] = o[2] = 0;
    o[3] = 1;
  }
  if (!w->curve) {
    w->curve = cw__wg_texture(WGPUTextureDimension_2D, CURVE_N, 2, 1, 1, WGPUTextureFormat_RGBA32Float,
                              WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                              WGPUTextureViewDimension_Undefined, "particles curves");
    w->curve_v = cw__wg_view(w->curve, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  }
  cw__wg_tex_write(w->curve, 0, 0, 0, 0, CURVE_N, 2, 16, px);
  cw_free(px);
  w->baked_ramp = d->color_ramp;
  w->baked_size = d->size_curve;
}

static bool ensure_hist(cw__psys *r, pw *w, int len) {
  if (w->hist && w->hist_len == len) return true;
  free_hist(w);
  if ((uint32_t)(w->trows * len) > cw__wgpu_limits()->maxTextureDimension2D) {
    static bool warned;
    if (!warned)
      cw_error("GPUParticles3D: trails of %d samples for %d particles need a %dx%d texture (max %u); lower amount or "
               "trail_length",
               len, w->cap, w->tw, w->trows * len, cw__wgpu_limits()->maxTextureDimension2D);
    warned = true;
    return false;
  }
  size_t n = (size_t)w->slots * (size_t)len;
  w->hist = cw__wg_buffer(n * 16, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst,
                          "particles history");
  float *dead = cw_alloc(n * 16, CW_MEM_RENDER);
  for (size_t i = 0; i < n; i++) {
    dead[4 * i] = dead[4 * i + 1] = dead[4 * i + 2] = 0;
    dead[4 * i + 3] = -1;
  }
  cw__wg_write(w->hist, 0, dead, n * 16);
  cw_free(dead);
  w->htex = cw__wg_texture(WGPUTextureDimension_2D, w->tw, w->trows * len, 1, 1, WGPUTextureFormat_RGBA32Float,
                           WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, WGPUTextureViewDimension_Undefined,
                           "particles history tex");
  w->htex_v = cw__wg_view(w->htex, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  if (!w->stex) {
    w->stex = cw__wg_texture(WGPUTextureDimension_2D, 2 * w->tw, w->trows, 1, 1, WGPUTextureFormat_RGBA32Float,
                             WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, WGPUTextureViewDimension_Undefined,
                             "particles state tex");
    w->stex_v = cw__wg_view(w->stex, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  }
  w->hist_len = len;
  r->hist_head = 0;
  r->hist_count = 0;
  return true;
}

static void ensure_sort(pw *w) {
  if (w->keys) return;
  int n = 1;
  while (n < w->cap) n <<= 1;
  w->sort_n = n;
  w->keys = cw__wg_buffer((size_t)n * 8, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc, "particles keys");
  w->sorted = cw__wg_buffer((size_t)w->cap * 48, WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex, "particles sorted");
  w->sort_u = cw__wg_buffer(80, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "particles sort u");
  /* (j, k) of every pass, one 256-byte slot each */
  int np = 0;
  for (int k = 2; k <= n; k <<= 1)
    for (int j = k >> 1; j > 0; j >>= 1) np++;
  w->npass = np;
  w->pass_u = cw__wg_buffer((size_t)PASS_STRIDE * (size_t)CW_MAX(np, 1), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                            "particles passes");
  uint8_t *pu = cw_calloc((size_t)PASS_STRIDE * (size_t)CW_MAX(np, 1), 1, CW_MEM_RENDER);
  int i = 0;
  for (int k = 2; k <= n; k <<= 1)
    for (int j = k >> 1; j > 0; j >>= 1, i++) {
      uint32_t jk[4] = { (uint32_t)j, (uint32_t)k, 0, 0 };
      memcpy(pu + (size_t)PASS_STRIDE * i, jk, sizeof jk);
    }
  cw__wg_write(w->pass_u, 0, pu, (size_t)PASS_STRIDE * (size_t)CW_MAX(np, 1));
  cw_free(pu);
}

/* the sorted keys (tests: the order invariants) */
WGPUBuffer cw__psys_wgpu_keys(cw_node n, int *count, int *amount);

/* ── the frame ────────────────────────────────────────────────────────── */

static void put_mat(float *dst, const t3_mat4 *m) { memcpy(dst, m->e, 64); }

/* A system's frame in two halves, so the pass records every system's
 * compute work in one compute pass and every system's draws in one render
 * pass (a pass's attachments load and store once, not once per system).
 * cw__psys_frame_wgpu: the simulation, the sort, the copies, on *cp (opened
 * when needed, ended before a copy); true when there is something to draw. */
static void end_cp(WGPUComputePassEncoder *cp) {
  if (!*cp) return;
  wgpuComputePassEncoderEnd(*cp);
  wgpuComputePassEncoderRelease(*cp);
  *cp = NULL;
}

bool cw__psys_frame_wgpu(cw_node n, p3d *d, cw_pass_ctx *ctx, cw__fx_tree *ft, WGPUComputePassEncoder *shared_cp) {
  CW_PROFILE_SCOPE("psys.wg.frame");
  cw__psys *r = d->rt;
  if (r->idle && r->nsteps == 0 && !d->gpu_stats) { /* nothing alive, nothing to simulate (p3d_fixed) */
    ft->stats.systems++;
    ft->stats.capacity += d->amount;
    return false;
  }
  const cw__wgpu_pass_targets *tg = cw__fx_pass_targets(ctx);
  if (!tg || !tg->enc || !tg->color) {
    static bool warned;
    if (!warned) cw_error("GPUParticles3D: the pipeline gave this pass no WebGPU targets; particles are not drawn");
    warned = true;
    return false;
  }
  if (!init_pipes()) return false;
  pw *w = ensure(r, d->amount);
  if (r->need_reset) {
    upload_dead(r, w);
    r->need_reset = false;
  }
  /* last request's stats (asynchronous: a frame or more old) */
  if (d->gpu_stats && w->rb) {
    const float *res = cw__wg_readback_latest(w->rb, NULL);
    if (res) {
      uint32_t cnt;
      memcpy(&cnt, res + 4, 4);
      d->gpu_alive = (int)cnt;
      d->gpu_sum = (t3_vec3){ res[0], res[1], res[2] };
    }
  }
  ensure_curves(d, w);
  ft->stats.systems++;
  ft->stats.capacity += d->amount;
  ft->stats.alive += d->alive;
  WGPUCommandEncoder enc = tg->enc;
  WGPUComputePassEncoder cp = *shared_cp;
  bool hist = d->trail_length >= 2 && ensure_hist(r, w, CW_MIN(d->trail_length, TRAIL_MAX));

  /* ── simulation ── */
  if (r->nsteps > 0) {
    float sys[SYS_SIZE / 4];
    uint32_t *su = (uint32_t *)sys;
    memset(sys, 0, sizeof sys);
    int shape = d->emission_shape;
    WGPUBuffer parent_state = NULL;
    uint32_t parent_n = 0;
    if (shape == SHAPE_PARENT) {
      cw_node pn = cw__psys_ref_node(n, d->parent);
      p3d *pd = cw_node_valid(pn) ? cw__psys_data(pn) : NULL;
      pw *pwr = pd && pd != d && pd->rt ? pd->rt->wg : NULL;
      if (pwr && pwr->state && pd->amount > 0) {
        parent_state = pwr->state;
        parent_n = (uint32_t)pd->amount;
      } else shape = SHAPE_POINT;
    }
    if (shape == SHAPE_MESH) {
      bool changed;
      const float *pts = cw__psys_emit_points(n, d, r, &changed);
      if (pts && (changed || !w->emit_ok)) cw__wg_write(w->emit, 0, pts, (size_t)EMIT_N * 2 * 16);
      w->emit_ok = pts != NULL;
      if (!pts) shape = SHAPE_POINT;
    }
    float attr[MAX_ATTR * 8], cinv[MAX_COLL * 16], cm[MAX_COLL * 16], cpar[MAX_COLL * 4];
    int na = cw__psys_attractors(ft, attr);
    int nc = G.fault == 3 ? 0 : cw__psys_colliders(ft, cinv, cm, cpar);
    bool depth = d->collide_depth && tg->linear_depth && ctx->camera;
    su[0] = (uint32_t)d->seed;
    su[1] = (uint32_t)d->amount;
    su[2] = (uint32_t)shape;
    su[3] = (uint32_t)na;
    su[4] = (uint32_t)nc;
    su[5] = depth ? 1 : 0;
    su[6] = (uint32_t)w->slots;
    su[7] = parent_n;
    float *f = sys + 8;
    f[0] = d->lifetime;
    f[1] = CW_CLAMP(d->lifetime_random, 0, 1);
    f[2] = CW_CLAMP(d->explosiveness, 0, 1);
    f[3] = r->emit_end < 0 ? -1.0f : (float)r->emit_end;
    f[4] = d->emission_extents.x;
    f[5] = d->emission_extents.y;
    f[6] = d->emission_extents.z;
    f[7] = d->emission_normal_speed;
    f[8] = d->emission_radius;
    f[9] = d->emission_inner_radius;
    f[10] = cosf(CW_CLAMP(d->spread, 0, 180) * (CW_PI / 180));
    f[11] = d->damping;
    t3_vec3 dir = d->direction;
    if (t3_vec3_length_sq(dir) < 1e-12f) dir = (t3_vec3){ 0, 1, 0 };
    dir = t3_vec3_normalize(dir);
    f[12] = dir.x;
    f[13] = dir.y;
    f[14] = dir.z;
    f[16] = d->speed_min;
    f[17] = d->speed_max;
    f[18] = d->inherit_velocity;
    f[20] = d->gravity.x;
    f[21] = d->gravity.y;
    f[22] = d->gravity.z;
    f[24] = d->turbulence;
    f[25] = d->turbulence_scale;
    f[26] = d->turbulence_speed;
    f[28] = d->bounce;
    f[29] = CW_CLAMP(d->friction, 0, 1);
    f[30] = d->collision_radius;
    f[31] = d->depth_thickness;
    if (depth) {
      t3_vec3 c3 = t3_mat4_get_position(&ctx->camera->base.matrix_world);
      f[32] = c3.x;
      f[33] = c3.y;
      f[34] = c3.z;
      f[36] = (float)tg->w;
      f[37] = (float)tg->h;
      f[38] = tg->flip_y ? 1.0f : 0.0f;
      put_mat(f + 40, &ctx->view_proj);
      put_mat(f + 56, &ctx->inv_view_proj);
      put_mat(f + 72, &ctx->proj);
    }
    memcpy(f + 88, attr, sizeof(float) * 8 * (size_t)na);
    if (nc) {
      memcpy(f + 152, cinv, 64 * (size_t)nc);
      memcpy(f + 280, cm, 64 * (size_t)nc);
      memcpy(f + 408, cpar, 16 * (size_t)nc);
    }
    {
      int nb = CW_CLAMP(d->bursts, 0, MAX_BURSTS);
      uint32_t *bu = (uint32_t *)(sys + 448);
      bu[0] = (uint32_t)nb;
      bu[1] = nb ? (uint32_t)cw__psys_burst_keep(d) : 0;
      if (nb) {
        memcpy(sys + 452, r->burst_p, sizeof(float) * 4 * (size_t)nb);
        memcpy(sys + 452 + 128, r->burst_d, sizeof(float) * 4 * (size_t)nb);
      }
      /* the burst slots only when the system has them: the rest of the struct keeps its zeros */
      CW_PROFILE_SCOPE("psys.wg.write");
      cw__wg_write(w->sys_u, 0, sys, nb ? sizeof sys : SYS_BASE);
    }
    /* steps, with the history slot each one writes */
    uint8_t *steps = cw_frame_alloc((size_t)STEP_STRIDE * (size_t)r->nsteps);
    memset(steps, 0, (size_t)STEP_STRIDE * (size_t)r->nsteps);
    for (int s = 0; s < r->nsteps; s++) {
      const step_rec *st = &r->steps[s];
      float *o = (float *)(steps + (size_t)STEP_STRIDE * s);
      o[0] = st->t0;
      o[1] = st->t1;
      int32_t slot = -1;
      if (hist) {
        r->hist_head = (r->hist_head + 1) % w->hist_len;
        r->hist_count = CW_MIN(r->hist_count + 1, w->hist_len);
        slot = r->hist_head;
      }
      memcpy(o + 4, &slot, 4);
      put_mat(o + 8, &st->m0);
      put_mat(o + 24, &st->m1);
    }
    cw__wg_write(w->step_u, 0, steps, (size_t)STEP_STRIDE * (size_t)r->nsteps);
    cw__wg_entry e[9] = {
      { .buffer = w->sys_u }, { .buffer = w->step_u, .size = 160 }, { .buffer = w->state },
      { .buffer = hist ? w->hist : G.dummy_storage }, { .buffer = w->emit }, { .view = cw__fx_wg_noise3d() },
      { .sampler = cw__wg_sampler(true, true, false) },
      { .view = depth ? tg->linear_depth : cw__fx_wg_dummy_r32f() },
      { .buffer = parent_state ? parent_state : G.dummy_parent },
    };
    WGPUBindGroup bg = cw__wg_group_cached(&w->sim_bg, G.sim_bgl, e,  9);
    if (!cp) cp = wgpuCommandEncoderBeginComputePass(enc, &(WGPUComputePassDescriptor){ .label = SV("particles") });
    wgpuComputePassEncoderSetPipeline(cp, G.sim);
    for (int s = 0; s < r->nsteps; s++) {
      uint32_t off = (uint32_t)(STEP_STRIDE * s);
      wgpuComputePassEncoderSetBindGroup(cp, 0, bg, 1, &off);
      cw__wg_dispatch(cp, (uint32_t)d->amount);
      ft->stats.sim_steps++;
    }
    r->nsteps = 0;
  }

  t3_mat4 model;
  if (d->local_coords) {
    t3_object *o = cw_node3d_object(n);
    t3_object_update_world_matrix(o, true, false);
    model = o->matrix_world;
  } else t3_mat4_identity(&model);

  /* ── stats ── */
  if (d->gpu_stats) {
    if (!cp) cp = wgpuCommandEncoderBeginComputePass(enc, &(WGPUComputePassDescriptor){ .label = SV("particles") });
    cw__wg_entry e[3] = { { .buffer = w->red_u }, { .buffer = w->state }, { .buffer = w->part } };
    WGPUBindGroup bg = cw__wg_group(G.red_bgl, e, 3);
    wgpuComputePassEncoderSetBindGroup(cp, 0, bg, 0, NULL);
    wgpuComputePassEncoderSetPipeline(cp, G.red1);
    cw__wg_dispatch(cp, (uint32_t)w->groups * 64);
    wgpuComputePassEncoderSetPipeline(cp, G.red2);
    wgpuComputePassEncoderDispatchWorkgroups(cp, 1, 1, 1);
    wgpuBindGroupRelease(bg);
  }

  /* ── sort ── */
  bool want_sort = d->sort && d->blend == BLEND_ALPHA;
  bool sorted = false;
  if (want_sort) {
    ensure_sort(w);
    t3_mat4 mv;
    t3_mat4_multiply(&mv, &ctx->view, &model);
    float su[20];
    uint32_t a[4] = { (uint32_t)d->amount, (uint32_t)w->sort_n, G.fault == 2 ? 1u : 0u, 0 };
    memcpy(su, a, 16);
    memcpy(su + 4, mv.e, 64);
    cw__wg_write(w->sort_u, 0, su, sizeof su);
    if (!cp) cp = wgpuCommandEncoderBeginComputePass(enc, &(WGPUComputePassDescriptor){ .label = SV("particles") });
    cw__wg_entry e[5] = { { .buffer = w->sort_u }, { .buffer = w->pass_u, .size = 16 }, { .buffer = w->keys },
                          { .buffer = w->state }, { .buffer = w->sorted } };
    WGPUBindGroup bg = cw__wg_group(G.sort_bgl, e, 5);
    uint32_t zero = 0;
    wgpuComputePassEncoderSetBindGroup(cp, 0, bg, 1, &zero);
    wgpuComputePassEncoderSetPipeline(cp, G.key);
    cw__wg_dispatch(cp, (uint32_t)w->sort_n);
    if (w->sort_n <= 1024) { /* the whole sort in one workgroup */
      wgpuComputePassEncoderSetPipeline(cp, G.bitonic_local);
      wgpuComputePassEncoderDispatchWorkgroups(cp, 1, 1, 1);
    } else {
      wgpuComputePassEncoderSetPipeline(cp, G.bitonic);
      for (int i = 0; i < w->npass; i++) {
        uint32_t off = (uint32_t)(PASS_STRIDE * i);
        wgpuComputePassEncoderSetBindGroup(cp, 0, bg, 1, &off);
        cw__wg_dispatch(cp, (uint32_t)w->sort_n / 2);
      }
    }
    wgpuComputePassEncoderSetBindGroup(cp, 0, bg, 1, &zero);
    wgpuComputePassEncoderSetPipeline(cp, G.gather);
    cw__wg_dispatch(cp, (uint32_t)d->amount);
    wgpuBindGroupRelease(bg);
    ft->stats.sort_passes += w->npass; /* the network's steps, however they ran (as on GL) */
    sorted = w->npass > 0 || w->sort_n == 1;
  }
  *shared_cp = cp;
  bool want_trail = d->trail_length >= 2 && w->hist && r->hist_count >= 2 && d->blend < BLEND_DISTORT;
  if (d->gpu_stats || want_trail) end_cp(shared_cp); /* copies run outside passes */
  if (d->gpu_stats) {
    if (!w->rb) w->rb = cw__wg_readback_new(32, 3, "particles stats readback");
    cw__wg_readback_request(w->rb, enc, w->part, (uint64_t)w->groups * 32, 0);
  }

  /* ── trails: state and history into textures ── */
  if (want_trail) {
    WGPUTexelCopyBufferInfo sb = { .layout = { .bytesPerRow = (uint32_t)w->tw * 32, .rowsPerImage = (uint32_t)w->trows },
                                   .buffer = w->state };
    WGPUTexelCopyTextureInfo stt = { .texture = w->stex, .aspect = WGPUTextureAspect_All };
    WGPUExtent3D se = { (uint32_t)(2 * w->tw), (uint32_t)w->trows, 1 };
    wgpuCommandEncoderCopyBufferToTexture(enc, &sb, &stt, &se);
    uint32_t rows = (uint32_t)(w->trows * w->hist_len);
    WGPUTexelCopyBufferInfo hb = { .layout = { .bytesPerRow = (uint32_t)w->tw * 16, .rowsPerImage = rows }, .buffer = w->hist };
    WGPUTexelCopyTextureInfo ht = { .texture = w->htex, .aspect = WGPUTextureAspect_All };
    WGPUExtent3D he = { (uint32_t)w->tw, rows, 1 };
    wgpuCommandEncoderCopyBufferToTexture(enc, &hb, &ht, &he);
  }

  bool mesh = d->draw_mode == DRAW_MESH;
  if (mesh) cw__psys_ensure_mesh(n, d, r);
  w->model = model;
  w->did_sort = sorted;
  w->trail = want_trail;
  w->particles = !mesh || r->mesh_count;
  return w->particles || w->trail;
}

/* the second half: its draws, into the pass's one render pass */
void cw__psys_draw_wgpu(WGPURenderPassEncoder rp, cw_node n, p3d *d, cw_pass_ctx *ctx, cw__fx_tree *ft, const cw__wgpu_pass_targets *tg) {
  CW_PROFILE_SCOPE("psys.wg.draw");
  (void)n;
  cw__psys *r = d->rt;
  pw *w = r->wg;
  const t3_mat4 model = w->model;
  bool sorted = w->did_sort, want_trail = w->trail, draw_particles = w->particles, mesh = d->draw_mode == DRAW_MESH;
  /* ── draw ── */
  t3_vec3 campos = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  float du[DRAW_SIZE / 4];
  uint32_t *dw = (uint32_t *)du;
  memset(du, 0, sizeof du);
  put_mat(du, &ctx->view);
  put_mat(du + 16, &ctx->camera->projection_matrix);
  put_mat(du + 32, &model);
  du[48] = campos.x;
  du[49] = campos.y;
  du[50] = campos.z;
  du[51] = d->energy;
  memcpy(du + 52, d->color, 16);
  du[56] = d->size;
  du[57] = CW_CLAMP(d->size_random, 0, 1);
  du[58] = d->stretch;
  du[59] = d->spin;
  du[60] = (float)CW_MAX(d->hframes, 1);
  du[61] = (float)CW_MAX(d->vframes, 1);
  du[62] = (float)d->draw_mode;
  du[63] = (float)d->blend;
  du[64] = tg->linear_depth ? d->soft_distance : 0;
  du[65] = tg->flip_y ? -1.0f : 1.0f;
  du[66] = 1.0f / (float)tg->w;
  du[67] = 1.0f / (float)tg->h;
  du[80] = d->distort_strength;
  du[81] = d->texture && *d->texture ? 1.0f : 0.0f;
  du[82] = ctx->camera->projection_matrix.e[0] * 0.5f;
  du[83] = ctx->camera->projection_matrix.e[5] * 0.5f;
  WGPUTextureView mvv = d->motion_texture && *d->motion_texture ? cw__fx_wg_texture_asset(d->motion_texture) : NULL;
  du[84] = mvv ? 1.0f : 0.0f;
  du[85] = d->motion_strength;
  du[68] = d->lifetime;
  du[69] = CW_CLAMP(d->lifetime_random, 0, 1);
  du[70] = d->trail_width;
  du[71] = (float)(d->blend == BLEND_OPAQUE ? BLEND_ALPHA : d->blend);
  dw[72] = (uint32_t)d->seed;
  dw[73] = (uint32_t)d->amount;
  dw[74] = (uint32_t)w->tw;
  dw[75] = (uint32_t)w->hist_len;
  dw[76] = (uint32_t)r->hist_head;
  dw[77] = (uint32_t)r->hist_count;
  dw[78] = (uint32_t)w->trows;
  { CW_PROFILE_SCOPE("psys.wg.write"); cw__wg_write(w->draw_u, 0, du, sizeof du); }
  WGPUTextureView tex = d->texture && *d->texture ? cw__fx_wg_texture_asset(d->texture) : NULL;
  if (!tex) tex = cw__fx_wg_dot();
  cw__wg_entry e[9] = {
    { .buffer = w->draw_u }, { .view = w->curve_v }, { .view = tex }, { .sampler = cw__wg_sampler(true, false, true) },
    { .view = tg->linear_depth ? tg->linear_depth : cw__fx_wg_dummy_r32f() },
    { .view = w->stex_v ? w->stex_v : cw__fx_wg_dummy_rgba32f() },
    { .view = w->htex_v ? w->htex_v : cw__fx_wg_dummy_rgba32f() },
    { .view = ft->copy ? cw__pw_view(ft->copy->tex) : cw__fx_wg_dot() },
    { .view = mvv ? mvv : cw__fx_wg_dot() },
  };
  WGPUBindGroup bg = cw__wg_group_cached(&w->draw_bg, G.draw_bgl, e, 9);
  wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, NULL);
  if (want_trail) {
    wgpuRenderPassEncoderSetPipeline(rp, draw_pipe_for(V_TRAIL, d->blend == BLEND_OPAQUE ? BLEND_ALPHA : d->blend, tg));
    wgpuRenderPassEncoderDraw(rp, (uint32_t)(2 * w->hist_len), (uint32_t)cw__psys_draw_count(d), 0, 0);
    ft->stats.draw_calls++;
  }
  if (draw_particles) {
    int variant = mesh ? (sorted ? V_MESH_SORTED : V_MESH) : (sorted ? V_QUAD_SORTED : V_QUAD);
    wgpuRenderPassEncoderSetPipeline(rp, draw_pipe_for(variant, d->blend, tg));
    wgpuRenderPassEncoderSetVertexBuffer(rp, 0, sorted ? w->sorted : w->state, 0, WGPU_WHOLE_SIZE);
    if (mesh) {
      wgpuRenderPassEncoderSetVertexBuffer(rp, 1, w->mesh_vb, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderSetIndexBuffer(rp, w->mesh_ib, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderDrawIndexed(rp, (uint32_t)r->mesh_count, (uint32_t)cw__psys_draw_count(d), 0, 0, 0);
    } else {
      wgpuRenderPassEncoderSetVertexBuffer(rp, 1, G.quad, 0, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderDraw(rp, 4, (uint32_t)cw__psys_draw_count(d), 0, 0);
    }
    ft->stats.draw_calls++;
  }
}

/* tests: the keys buffer of a sorted system (n entries of key f32 + index
 * u32, n = the power of two at or above amount) */
WGPUBuffer cw__psys_wgpu_keys(cw_node n, int *count, int *amount) {
  cw__psys *r = cw__psys_of(n);
  pw *w = r ? r->wg : NULL;
  if (!w || !w->keys) return NULL;
  *count = w->sort_n;
  *amount = w->cap;
  return w->keys;
}

#endif /* CW_WGPU */
