/* GPUParticles3D: transform feedback simulation, instanced drawing, GPU
 * bitonic sort, trails from a history ring, GPU stats readback.
 * ParticleAttractor3D and ParticleCollider3D. See cartwheel/particles.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/jobs.h"
#include "particles3d_internal.h"
#include "cartwheel/quality.h"

t3_mesh *cw__mesh3d_t3(cw_node n); /* src/scene/render3d.c */

static const char *const shape_names[] = { "POINT", "SPHERE", "SPHERE_SURFACE", "BOX", "RING", "MESH", NULL };
static const char *const draw_names[] = { "BILLBOARD", "STRETCHED", "MESH", NULL };
static const char *const mprim_names[] = { "BOX", "SPHERE", "TETRA", "NODE", NULL };
static const char *const blend_names[] = { "ADD", "ALPHA", "OPAQUE", NULL };

static const cw_node_type *p3d_t;

cw__psys *cw__psys_of(cw_node n) {
  p3d *d = cw_node_data_t(n, p3d_t);
  return d ? d->rt : NULL;
}

/* ── the emission function on the CPU (alive counts) ───────────────── */

#define H cw_particle_hash
static uint32_t pseed(uint32_t seed, int i, int k) {
  return H(seed ^ H((uint32_t)i * 0x9E3779B9u ^ H((uint32_t)k + 0x632BE5ABu)));
}
static float prnd(uint32_t *s) {
  *s = H(*s);
  return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

typedef struct alive_job {
  const p3d *d;
  float t, emit_end;
  int counts[256];
  int chunk;
} alive_job;

static void alive_range(int begin, int end, int worker, void *ctx) {
  (void)worker;
  alive_job *j = ctx;
  const p3d *d = j->d;
  float L = d->lifetime;
  int n = 0;
  for (int i = begin; i < end; i++) {
    float off = (float)i / (float)d->amount * L * (1.0f - d->explosiveness);
    float rel = j->t - off;
    if (rel < 0) continue;
    int k = (int)floorf(rel / L);
    float spawn = off + (float)k * L;
    if (j->emit_end >= 0 && spawn >= j->emit_end) continue;
    uint32_t s = pseed((uint32_t)d->seed, i, k);
    float life = L * (1.0f - d->lifetime_random * prnd(&s));
    float age = rel - (float)k * L;
    if (age < life) n++;
  }
  j->counts[begin / j->chunk] = n;
}

static int cpu_alive(const p3d *d) {
  const cw__psys *r = d->rt;
  if (!r || d->amount <= 0 || d->lifetime <= 0) return 0;
  if (r->emit_start < 0) return 0;
  alive_job j = { .d = d, .t = (float)(r->sim_time - r->emit_start),
                  .emit_end = r->emit_end < 0 ? -1.0f : (float)r->emit_end };
  j.chunk = CW_MAX(1024, (d->amount + 255) / 256);
  int chunks = (d->amount + j.chunk - 1) / j.chunk;
  memset(j.counts, 0, sizeof j.counts);
  cw_parallel_for(d->amount, j.chunk, alive_range, &j);
  int n = 0;
  for (int i = 0; i < chunks; i++) n += j.counts[i];
  return n;
}

/* ── GLSL ───────────────────────────────────────────────────────────── */

static const char *const glsl_common =
    "uniform uint u_seed;\n"
    "uniform int u_amount;\n"
    "uniform float u_lifetime, u_life_rand;\n"
    "uint ph(uint x) { x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u; return x; }\n"
    "uint pseed(int i, int k) { return ph(u_seed ^ ph(uint(i) * 0x9E3779B9u ^ ph(uint(k) + 0x632BE5ABu))); }\n"
    "float prnd(inout uint s) { s = ph(s); return float(s >> 8u) * (1.0 / 16777216.0); }\n"
    "float plife(int i, int k) { uint s = pseed(i, k); return u_lifetime * (1.0 - u_life_rand * prnd(s)); }\n";

static const char *const sim_vs =
    "layout(location = 0) in vec4 a_pa;\n"
    "layout(location = 1) in vec4 a_vc;\n"
    "out vec4 o_pa;\n"
    "out vec4 o_vc;\n"
    "uniform float u_t0, u_t1, u_emit_end, u_explosive;\n"
    "uniform mat4 u_em0, u_em1;\n"
    "uniform int u_shape;\n"
    "uniform vec3 u_ext;\n"
    "uniform vec2 u_rad;\n"
    "uniform float u_nspeed;\n"
    "uniform vec3 u_dir;\n"
    "uniform float u_cos_spread;\n"
    "uniform vec2 u_speed;\n"
    "uniform vec3 u_gravity;\n"
    "uniform float u_damping;\n"
    "uniform sampler3D u_noise;\n"
    "uniform vec3 u_turb;\n"
    "uniform int u_nattr;\n"
    "uniform vec4 u_attr[16];\n"
    "uniform int u_ncol;\n"
    "uniform mat4 u_col_inv[8];\n"
    "uniform mat4 u_col_m[8];\n"
    "uniform vec4 u_col_p[8];\n"
    "uniform vec3 u_coll;\n"
    "uniform int u_depth_on;\n"
    "uniform sampler2D u_depth;\n"
    "uniform mat4 u_vp, u_ivp;\n"
    "uniform vec2 u_texel;\n"
    "uniform float u_thick;\n"
    "uniform vec3 u_campos;\n"
    "uniform sampler2D u_emit;\n"
    "void emit_shape(inout uint s, out vec3 p, out vec3 n) {\n"
    "  p = vec3(0.0); n = vec3(0.0);\n"
    "  if (u_shape == 1 || u_shape == 2) {\n"
    "    float z = prnd(s) * 2.0 - 1.0; float a = prnd(s) * 6.2831853; float r = sqrt(max(0.0, 1.0 - z * z));\n"
    "    vec3 d = vec3(r * cos(a), z, r * sin(a));\n"
    "    if (u_shape == 1) p = d * (u_rad.x * pow(prnd(s), 1.0 / 3.0)); else { p = d * u_rad.x; n = d; }\n"
    "  } else if (u_shape == 3) {\n"
    "    p = (vec3(prnd(s), prnd(s), prnd(s)) * 2.0 - 1.0) * u_ext;\n"
    "  } else if (u_shape == 4) {\n"
    "    float a = prnd(s) * 6.2831853; float r = sqrt(mix(u_rad.y * u_rad.y, u_rad.x * u_rad.x, prnd(s)));\n"
    "    p = vec3(r * cos(a), 0.0, r * sin(a));\n"
    "  } else if (u_shape == 5) {\n"
    "    int j = min(int(prnd(s) * 4096.0), 4095);\n"
    "    p = texelFetch(u_emit, ivec2(j % 64, j / 64), 0).xyz;\n"
    "    n = texelFetch(u_emit, ivec2(j % 64, 64 + j / 64), 0).xyz;\n"
    "  }\n"
    "}\n"
    "vec3 cone(inout uint s) {\n"
    "  float ct = mix(1.0, u_cos_spread, prnd(s)); float st = sqrt(max(0.0, 1.0 - ct * ct));\n"
    "  float a = prnd(s) * 6.2831853;\n"
    "  vec3 w = u_dir; vec3 up = abs(w.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);\n"
    "  vec3 u = normalize(cross(up, w)); vec3 v = cross(w, u);\n"
    "  return u * (st * cos(a)) + v * (st * sin(a)) + w * ct;\n"
    "}\n"
    "vec3 forces(vec3 p, float t) {\n"
    "  vec3 a = u_gravity;\n"
    "  for (int j = 0; j < 8; j++) {\n"
    "    if (j >= u_nattr) break;\n"
    "    vec4 A = u_attr[2 * j]; vec4 B = u_attr[2 * j + 1];\n"
    "    vec3 d = A.xyz - p; float l = length(d);\n"
    "    if (l < A.w && l > 1e-4) a += d / l * (B.x * pow(1.0 - l / A.w, B.y));\n"
    "  }\n"
    "  if (u_turb.x != 0.0)\n"
    "    a += (texture(u_noise, p * u_turb.y + vec3(0.0, t * u_turb.z, 0.0)).xyz * 2.0 - 1.0) * u_turb.x;\n"
    "  return a;\n"
    "}\n"
    "void respond(inout vec3 v, vec3 n) {\n"
    "  float vn = dot(v, n);\n"
    "  if (vn < 0.0) {\n"
    "    vec3 vt = v - vn * n;\n"
    "    v = vt * (1.0 - u_coll.y) - vn * u_coll.x * n;\n"
    "  }\n"
    "}\n"
    "void colliders(inout vec3 p, inout vec3 v, vec3 pprev) {\n"
    "  float r = u_coll.z;\n"
    "  for (int j = 0; j < 8; j++) {\n"
    "    if (j >= u_ncol) break;\n"
    "    vec4 P = u_col_p[j];\n"
    "    vec3 lp = (u_col_inv[j] * vec4(p, 1.0)).xyz;\n"
    "    vec3 ln = vec3(0.0); bool hit = false;\n"
    "    if (P.x < 0.5) {\n"
    "      vec3 lq = (u_col_inv[j] * vec4(pprev, 1.0)).xyz;\n"
    "      bool inside = (P.y <= 0.0 || abs(lp.x) <= P.y) && (P.w <= 0.0 || abs(lp.z) <= P.w);\n"
    "      if (inside && lp.y < r && lq.y >= -r) { lp.y = r; ln = vec3(0.0, 1.0, 0.0); hit = true; }\n"
    "    } else if (P.x < 1.5) {\n"
    "      float l = length(lp);\n"
    "      if (l < P.y + r) { ln = l > 1e-6 ? lp / l : vec3(0.0, 1.0, 0.0); lp = ln * (P.y + r); hit = true; }\n"
    "    } else {\n"
    "      vec3 e = P.yzw + vec3(r); vec3 q = abs(lp);\n"
    "      if (q.x < e.x && q.y < e.y && q.z < e.z) {\n"
    "        vec3 pen = e - q;\n"
    "        if (pen.x <= pen.y && pen.x <= pen.z) { ln = vec3(sign(lp.x), 0.0, 0.0); lp.x = sign(lp.x) * e.x; }\n"
    "        else if (pen.y <= pen.z) { ln = vec3(0.0, sign(lp.y), 0.0); lp.y = sign(lp.y) * e.y; }\n"
    "        else { ln = vec3(0.0, 0.0, sign(lp.z)); lp.z = sign(lp.z) * e.z; }\n"
    "        hit = true;\n"
    "      }\n"
    "    }\n"
    "    if (hit) {\n"
    "      p = (u_col_m[j] * vec4(lp, 1.0)).xyz;\n"
    "      vec3 n = normalize(mat3(u_col_m[j]) * ln);\n"
    "      respond(v, n);\n"
    "    }\n"
    "  }\n"
    "}\n"
    "vec3 unproject(vec2 uv, float d) {\n"
    "  vec4 w = u_ivp * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "  return w.xyz / w.w;\n"
    "}\n"
    "void depth_collide(inout vec3 p, inout vec3 v) {\n"
    "  vec4 c = u_vp * vec4(p, 1.0);\n"
    "  if (c.w <= 0.0) return;\n"
    "  vec3 ndc = c.xyz / c.w;\n"
    "  if (abs(ndc.x) >= 0.995 || abs(ndc.y) >= 0.995) return;\n"
    "  vec2 uv = ndc.xy * 0.5 + 0.5;\n"
    "  float ds = texture(u_depth, uv).r;\n"
    "  if (ds >= 1.0) return;\n"
    "  vec3 S = unproject(uv, ds);\n"
    "  float dp = distance(u_campos, p), dsur = distance(u_campos, S);\n"
    "  if (dp < dsur - 2.0 * u_coll.z - 0.05 || dp > dsur + u_thick) return;\n"
    "  vec2 ox = vec2(u_texel.x, 0.0), oy = vec2(0.0, u_texel.y);\n"
    "  vec3 Sx1 = unproject(uv + ox, texture(u_depth, uv + ox).r), Sx0 = unproject(uv - ox, texture(u_depth, uv - ox).r);\n"
    "  vec3 Sy1 = unproject(uv + oy, texture(u_depth, uv + oy).r), Sy0 = unproject(uv - oy, texture(u_depth, uv - oy).r);\n"
    "  vec3 dx = distance(Sx1, S) < distance(Sx0, S) ? Sx1 - S : S - Sx0;\n"
    "  vec3 dy = distance(Sy1, S) < distance(Sy0, S) ? Sy1 - S : S - Sy0;\n"
    "  vec3 n = cross(dx, dy);\n"
    "  if (dot(n, n) < 1e-20) return;\n"
    "  n = normalize(n);\n"
    "  if (dot(n, u_campos - S) < 0.0) n = -n;\n"
    "  float dist = dot(p - S, n);\n"
    "  if (dist >= u_coll.z || dist < -u_thick) return;\n"
    "  p += n * (u_coll.z - dist);\n"
    "  respond(v, n);\n"
    "}\n"
    "void main() {\n"
    "  int i = gl_VertexID;\n"
    "  float L = u_lifetime;\n"
    "  float off = float(i) / float(u_amount) * L * (1.0 - u_explosive);\n"
    "  float rel = u_t1 - off;\n"
    "  vec4 pa = a_pa, vc = a_vc;\n"
    "  int k = rel >= 0.0 ? int(floor(rel / L)) : -1;\n"
    "  float spawn = off + float(k) * L;\n"
    "  bool ok = k >= 0 && (u_emit_end < 0.0 || spawn < u_emit_end);\n"
    "  float life = ok ? plife(i, k) : 0.0;\n"
    "  float age = rel - float(k) * L;\n"
    "  if (!ok || age >= life) { o_pa = vec4(pa.xyz, -1.0); o_vc = vc; return; }\n"
    "  if (pa.w < 0.0 || int(vc.w) != k) {\n"
    "    uint s = pseed(i, k); prnd(s);\n"
    "    float f = clamp((spawn - u_t0) / max(u_t1 - u_t0, 1e-6), 0.0, 1.0);\n"
    "    mat4 M = u_em0 * (1.0 - f) + u_em1 * f;\n"
    "    vec3 lp, ln; emit_shape(s, lp, ln);\n"
    "    vec3 d = cone(s);\n"
    "    float sp = mix(u_speed.x, u_speed.y, prnd(s));\n"
    "    vec3 p = (M * vec4(lp, 1.0)).xyz;\n"
    "    vec3 wd = mat3(M) * d;\n"
    "    vec3 v = (dot(wd, wd) > 0.0 ? normalize(wd) : wd) * sp;\n"
    "    vec3 wn = mat3(M) * ln;\n"
    "    if (dot(wn, wn) > 0.0) v += normalize(wn) * u_nspeed;\n"
    "    v += u_gravity * age;\n"
    "    p += v * age;\n"
    "    o_pa = vec4(p, age);\n"
    "    o_vc = vec4(v, float(k));\n"
    "    return;\n"
    "  }\n"
    "  float dt = u_t1 - u_t0;\n"
    "  vec3 p = pa.xyz, v = vc.xyz, pprev = p;\n"
    "  v += forces(p, u_t1) * dt;\n"
    "  v *= 1.0 / (1.0 + u_damping * dt);\n"
    "  p += v * dt;\n"
    "  colliders(p, v, pprev);\n"
    "  if (u_depth_on == 1) depth_collide(p, v);\n"
    "  o_pa = vec4(p, age);\n"
    "  o_vc = vec4(v, vc.w);\n"
    "}\n";

static const char *const sim_fs = "out vec4 o;\nvoid main() { o = vec4(0.0); }\n";

/* drawing: billboards / stretched quads / meshes, from attributes or (sorted)
 * from the state textures through the order texture */
static const char *const draw_vs =
    "#ifndef SRC_TEX\n"
    "layout(location = 0) in vec4 a_pa;\n"
    "layout(location = 1) in vec4 a_vc;\n"
    "#else\n"
    "uniform sampler2D u_state0, u_state1, u_order;\n"
    "uniform int u_tw, u_ow;\n"
    "#endif\n"
    "layout(location = 2) in vec3 a_vert;\n"
    "layout(location = 3) in vec3 a_norm;\n"
    "uniform mat4 u_view, u_proj, u_model;\n"
    "uniform vec3 u_campos;\n"
    "uniform sampler2D u_curve;\n"
    "uniform vec4 u_color;\n"
    "uniform float u_energy, u_size, u_size_rand, u_stretch, u_spin;\n"
    "uniform vec2 u_frames;\n"
    "uniform int u_mode;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "out float v_vz;\n"
    "out vec3 v_n;\n"
    "vec4 curve(float t, int row) {\n"
    "  float x = t * 255.0; int i0 = int(floor(x)); int i1 = min(i0 + 1, 255);\n"
    "  return mix(texelFetch(u_curve, ivec2(i0, row), 0), texelFetch(u_curve, ivec2(i1, row), 0), x - float(i0));\n"
    "}\n"
    "vec3 rot(vec3 v, vec3 ax, float a) {\n"
    "  float c = cos(a), s = sin(a);\n"
    "  return v * c + cross(ax, v) * s + ax * dot(ax, v) * (1.0 - c);\n"
    "}\n"
    "void main() {\n"
    "  int i; vec4 pa, vc;\n"
    "#ifdef SRC_TEX\n"
    "  int n = gl_InstanceID;\n"
    "  i = int(texelFetch(u_order, ivec2(n % u_ow, n / u_ow), 0).y);\n"
    "  if (i < 0 || i >= u_amount) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); v_color = vec4(0.0); return; }\n"
    "  pa = texelFetch(u_state0, ivec2(i % u_tw, i / u_tw), 0);\n"
    "  vc = texelFetch(u_state1, ivec2(i % u_tw, i / u_tw), 0);\n"
    "#else\n"
    "  i = gl_InstanceID; pa = a_pa; vc = a_vc;\n"
    "#endif\n"
    "  if (pa.w < 0.0) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); v_color = vec4(0.0); return; }\n"
    "  int k = int(vc.w);\n"
    "  float life = plife(i, k);\n"
    "  float t = clamp(pa.w / life, 0.0, 1.0);\n"
    "  vec4 col = curve(t, 0) * u_color;\n"
    "  col.rgb *= u_energy;\n"
    "  float sz = curve(t, 1).x * u_size;\n"
    "  uint s = pseed(i, k) ^ 0x5bd1e995u;\n"
    "  float r1 = prnd(s), r2 = prnd(s), r3 = prnd(s);\n"
    "  sz *= 1.0 - u_size_rand * r1;\n"
    "  float ang = r2 * 6.2831853 + u_spin * (r3 < 0.5 ? -1.0 : 1.0) * pa.w;\n"
    "  vec3 wp = (u_model * vec4(pa.xyz, 1.0)).xyz;\n"
    "  vec3 wv = mat3(u_model) * vc.xyz;\n"
    "  vec3 right = vec3(u_view[0][0], u_view[1][0], u_view[2][0]);\n"
    "  vec3 up = vec3(u_view[0][1], u_view[1][1], u_view[2][1]);\n"
    "  vec3 world; v_n = vec3(0.0, 0.0, 1.0);\n"
    "  if (u_mode == 2) {\n"
    "    vec3 ax = normalize(vec3(r1 - 0.5, r2 - 0.5, r3 - 0.5) + vec3(1e-3));\n"
    "    world = wp + rot(a_vert, ax, ang) * sz;\n"
    "    v_n = rot(a_norm, ax, ang);\n"
    "    v_uv = vec2(0.5);\n"
    "  } else {\n"
    "    float sp = length(wv);\n"
    "    if (u_mode == 1 && sp > 1e-5) {\n"
    "      vec3 axis = wv / sp;\n"
    "      vec3 side = cross(axis, normalize(u_campos - wp));\n"
    "      side = dot(side, side) > 1e-10 ? normalize(side) : right;\n"
    "      float len = sz + sp * u_stretch;\n"
    "      world = wp + side * (a_vert.x * sz * 0.5) + axis * (a_vert.y * len * 0.5);\n"
    "    } else {\n"
    "      float c = cos(ang), sn = sin(ang);\n"
    "      vec2 q = vec2(c * a_vert.x - sn * a_vert.y, sn * a_vert.x + c * a_vert.y);\n"
    "      world = wp + (right * q.x + up * q.y) * (sz * 0.5);\n"
    "    }\n"
    "    vec2 uv = vec2(a_vert.x * 0.5 + 0.5, 0.5 - a_vert.y * 0.5);\n"
    "    float nf = u_frames.x * u_frames.y;\n"
    "    if (nf > 1.0) {\n"
    "      float f = min(floor(t * nf), nf - 1.0);\n"
    "      uv = (uv + vec2(mod(f, u_frames.x), floor(f / u_frames.x))) / u_frames;\n"
    "    }\n"
    "    v_uv = uv;\n"
    "  }\n"
    "  vec4 vp = u_view * vec4(world, 1.0);\n"
    "  v_vz = -vp.z;\n"
    "  v_color = col;\n"
    "  gl_Position = u_proj * vp;\n"
    "}\n";

static const char *const draw_fs =
    "in vec4 v_color;\n"
    "in vec2 v_uv;\n"
    "in float v_vz;\n"
    "in vec3 v_n;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_depth;\n"
    "uniform int u_blend, u_mode;\n"
    "uniform vec4 u_soft;\n" /* soft distance, near, far, ortho */
    "uniform vec2 u_inv_size;\n"
    "out vec4 frag;\n"
    "float lin(float d) { return u_soft.w > 0.5 ? u_soft.y + d * (u_soft.z - u_soft.y) : u_soft.y * u_soft.z / (u_soft.z - d * (u_soft.z - u_soft.y)); }\n"
    "void main() {\n"
    "  vec4 c = v_color;\n"
    "  if (u_mode == 2) c.rgb *= 0.55 + 0.45 * max(dot(normalize(v_n), vec3(0.3, 0.8, 0.5196)), 0.0);\n"
    "  else c *= texture(u_tex, v_uv);\n"
    "  if (u_soft.x > 0.0) {\n"
    "    float zs = lin(texture(u_depth, gl_FragCoord.xy * u_inv_size).r);\n"
    "    c.a *= clamp((zs - v_vz) / u_soft.x, 0.0, 1.0);\n"
    "  }\n"
    "  if (u_blend == 2) { if (c.a < 0.5) discard; frag = vec4(c.rgb, 1.0); return; }\n"
    "  frag = vec4(c.rgb * c.a, u_blend == 1 ? c.a : 0.0);\n"
    "}\n";

/* writes particle state (or a history slot) into float textures, one point
 * per particle */
static const char *const scatter_vs =
    "layout(location = 0) in vec4 a_pa;\n"
    "layout(location = 1) in vec4 a_vc;\n"
    "uniform int u_tw, u_row0;\n"
    "uniform vec2 u_size;\n"
    "flat out vec4 v_a;\n"
    "flat out vec4 v_b;\n"
    "void main() {\n"
    "  int i = gl_VertexID;\n"
    "  vec2 xy = vec2(float(i % u_tw), float(u_row0 + i / u_tw)) + 0.5;\n"
    "  gl_Position = vec4(xy / u_size * 2.0 - 1.0, 0.0, 1.0);\n"
    "  gl_PointSize = 1.0;\n"
    "  v_a = a_pa; v_b = a_vc;\n"
    "}\n";
static const char *const scatter_fs =
    "flat in vec4 v_a;\n"
    "flat in vec4 v_b;\n"
    "layout(location = 0) out vec4 o0;\n"
    "#ifndef HIST\n"
    "layout(location = 1) out vec4 o1;\n"
    "#endif\n"
    "void main() {\n"
    "#ifdef HIST\n"
    "  o0 = vec4(v_a.xyz, v_a.w < 0.0 ? -1.0 : v_b.w);\n"
    "#else\n"
    "  o0 = v_a; o1 = v_b;\n"
    "#endif\n"
    "}\n";

/* full-screen passes (cw_pass_program: v_uv is provided) */
static const char *const key_fs =
    "uniform sampler2D u_state0;\n"
    "uniform int u_tw, u_amount, u_sw;\n"
    "uniform mat4 u_mv;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  ivec2 c = ivec2(gl_FragCoord.xy); int idx = c.y * u_sw + c.x; float key = 3.0e38;\n"
    "  if (idx < u_amount) {\n"
    "    vec4 pa = texelFetch(u_state0, ivec2(idx % u_tw, idx / u_tw), 0);\n"
    "    if (pa.w >= 0.0) key = (u_mv * vec4(pa.xyz, 1.0)).z;\n"
    "  }\n"
    "  o = vec4(key, float(idx), 0.0, 0.0);\n"
    "}\n";
static const char *const bitonic_fs =
    "uniform sampler2D u_keys;\n"
    "uniform int u_sw, u_j, u_k;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  ivec2 c = ivec2(gl_FragCoord.xy); int idx = c.y * u_sw + c.x; int p = idx ^ u_j;\n"
    "  vec2 a = texelFetch(u_keys, c, 0).xy;\n"
    "  vec2 b = texelFetch(u_keys, ivec2(p % u_sw, p / u_sw), 0).xy;\n"
    "  bool asc = (idx & u_k) == 0;\n"
    "  bool lower = idx < p;\n"
    "  bool alt = a.x < b.x || (a.x == b.x && a.y < b.y);\n"
    "  bool keep_a = (lower == asc) ? alt : !alt;\n"
    "  o = vec4(keep_a ? a : b, 0.0, 0.0);\n"
    "}\n";
/* sums 8x8 blocks; the first level counts alive particles of the state */
static const char *const reduce_fs =
    "uniform sampler2D u_src;\n"
    "uniform ivec2 u_src_size;\n"
    "uniform int u_first;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  ivec2 b = ivec2(gl_FragCoord.xy) * 8; vec4 s = vec4(0.0);\n"
    "  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {\n"
    "    ivec2 c = b + ivec2(x, y);\n"
    "    if (c.x >= u_src_size.x || c.y >= u_src_size.y) continue;\n"
    "    vec4 v = texelFetch(u_src, c, 0);\n"
    "    if (u_first == 1) s += v.w >= 0.0 ? vec4(v.xyz, 1.0) : vec4(0.0); else s += v;\n"
    "  }\n"
    "  o = s;\n"
    "}\n";

/* trails: a camera-facing strip per particle through its history */
static const char *const trail_vs =
    "uniform sampler2D u_state0, u_state1, u_hist;\n"
    "uniform int u_tw, u_rows, u_hlen, u_head, u_hcount;\n"
    "uniform mat4 u_view, u_proj, u_model;\n"
    "uniform vec3 u_campos;\n"
    "uniform sampler2D u_curve;\n"
    "uniform vec4 u_color;\n"
    "uniform float u_energy, u_width;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "out float v_vz;\n"
    "vec4 curve(float t, int row) {\n"
    "  float x = t * 255.0; int i0 = int(floor(x)); int i1 = min(i0 + 1, 255);\n"
    "  return mix(texelFetch(u_curve, ivec2(i0, row), 0), texelFetch(u_curve, ivec2(i1, row), 0), x - float(i0));\n"
    "}\n"
    "vec4 hist(int i, int j) {\n"
    "  int slot = (u_head - j + u_hlen) % u_hlen;\n"
    "  return texelFetch(u_hist, ivec2(i % u_tw, slot * u_rows + i / u_tw), 0);\n"
    "}\n"
    "vec3 sample_at(int i, int j, float cyc) {\n"
    "  j = min(j, u_hcount - 1);\n"
    "  for (int q = 0; q < 64; q++) {\n"
    "    if (j <= 0) break;\n"
    "    vec4 h = hist(i, j);\n"
    "    if (h.w == cyc) return h.xyz;\n"
    "    j--;\n"
    "  }\n"
    "  return hist(i, 0).xyz;\n"
    "}\n"
    "void main() {\n"
    "  int i = gl_InstanceID;\n"
    "  int seg = gl_VertexID >> 1;\n"
    "  float side = float(gl_VertexID & 1) * 2.0 - 1.0;\n"
    "  vec4 pa = texelFetch(u_state0, ivec2(i % u_tw, i / u_tw), 0);\n"
    "  vec4 vc = texelFetch(u_state1, ivec2(i % u_tw, i / u_tw), 0);\n"
    "  if (pa.w < 0.0 || u_hcount < 2) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); v_color = vec4(0.0); return; }\n"
    "  float cyc = vc.w;\n"
    "  vec3 P = (u_model * vec4(sample_at(i, seg, cyc), 1.0)).xyz;\n"
    "  vec3 A = (u_model * vec4(sample_at(i, seg - 1, cyc), 1.0)).xyz;\n"
    "  vec3 B = (u_model * vec4(sample_at(i, seg + 1, cyc), 1.0)).xyz;\n"
    "  vec3 tg = A - B;\n"
    "  if (dot(tg, tg) < 1e-12) tg = vec3(0.0, 1e-3, 0.0);\n"
    "  vec3 sd = cross(normalize(tg), normalize(u_campos - P));\n"
    "  sd = dot(sd, sd) > 1e-10 ? normalize(sd) : vec3(1.0, 0.0, 0.0);\n"
    "  float u = float(seg) / float(max(u_hlen - 1, 1));\n"
    "  int k = int(cyc);\n"
    "  float t = clamp(pa.w / plife(i, k), 0.0, 1.0);\n"
    "  vec4 col = curve(t, 0) * u_color;\n"
    "  col.rgb *= u_energy;\n"
    "  col.a *= 1.0 - u;\n"
    "  vec3 world = P + sd * (side * u_width * 0.5 * (1.0 - u));\n"
    "  vec4 vp = u_view * vec4(world, 1.0);\n"
    "  v_vz = -vp.z;\n"
    "  v_color = col;\n"
    "  v_uv = vec2(side * 0.5 + 0.5, u);\n"
    "  gl_Position = u_proj * vp;\n"
    "}\n";
static const char *const trail_fs =
    "in vec4 v_color;\n"
    "in vec2 v_uv;\n"
    "in float v_vz;\n"
    "uniform int u_blend;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  vec4 c = v_color;\n"
    "  float e = 1.0 - abs(v_uv.x * 2.0 - 1.0);\n"
    "  c.a *= smoothstep(0.0, 0.35, e);\n"
    "  frag = vec4(c.rgb * c.a, u_blend == 1 ? c.a : 0.0);\n"
    "}\n";

/* ── programs ───────────────────────────────────────────────────────── */

static GLuint prog_sim, prog_draw[2], prog_scatter, prog_hist, prog_key, prog_bitonic, prog_reduce, prog_trail;
static bool progs_failed;

static char *cat3(const char *a, const char *b, const char *c) {
  size_t n = strlen(a) + strlen(b) + strlen(c) + 1;
  char *s = cw_alloc(n, CW_MEM_RENDER);
  snprintf(s, n, "%s%s%s", a, b, c);
  return s;
}

static bool build_programs(void) {
  if (prog_sim) return true;
  if (progs_failed) return false;
  static const char *const vary[2] = { "o_pa", "o_vc" };
  char *vs = cat3(glsl_common, sim_vs, "");
  prog_sim = cw__fx_program("particles.sim", vs, sim_fs, vary, 2);
  cw_free(vs);
  for (int v = 0; v < 2; v++) {
    char *dvs = cat3(v ? "#define SRC_TEX 1\n" : "", glsl_common, draw_vs);
    char *dfs = cat3(glsl_common, draw_fs, "");
    prog_draw[v] = cw__fx_program(v ? "particles.draw_sorted" : "particles.draw", dvs, dfs, NULL, 0);
    cw_free(dvs);
    cw_free(dfs);
  }
  prog_scatter = cw__fx_program("particles.scatter", scatter_vs, scatter_fs, NULL, 0);
  char *hfs = cat3("#define HIST 1\n", scatter_fs, "");
  prog_hist = cw__fx_program("particles.history", scatter_vs, hfs, NULL, 0);
  cw_free(hfs);
  prog_key = cw_pass_program("particles.sort_key", key_fs);
  prog_bitonic = cw_pass_program("particles.bitonic", bitonic_fs);
  prog_reduce = cw_pass_program("particles.reduce", reduce_fs);
  char *tvs = cat3(glsl_common, trail_vs, "");
  prog_trail = cw__fx_program("particles.trail", tvs, trail_fs, NULL, 0);
  cw_free(tvs);
  if (!prog_sim || !prog_draw[0] || !prog_draw[1] || !prog_scatter || !prog_hist || !prog_key || !prog_bitonic ||
      !prog_reduce || !prog_trail) {
    progs_failed = true;
    prog_sim = 0;
    cw_error("GPUParticles3D: shader build failed (logged above); particles are not drawn");
    return false;
  }
  return true;
}

/* ── GL resources ───────────────────────────────────────────────────── */

static void free_tex(GLuint *t) {
  if (*t) glDeleteTextures(1, t);
  *t = 0;
}
static void free_fbo(GLuint *f) {
  if (*f) glDeleteFramebuffers(1, f);
  *f = 0;
}

static void free_state(cw__psys *r) {
  for (int i = 0; i < 2; i++) {
    free_tex(&r->state_tex[i]);
    free_tex(&r->key_tex[i]);
    free_fbo(&r->key_fbo[i]);
  }
  free_fbo(&r->state_fbo);
  free_tex(&r->hist_tex);
  free_fbo(&r->hist_fbo);
  for (int i = 0; i < 4; i++) {
    free_tex(&r->red_tex[i]);
    free_fbo(&r->red_fbo[i]);
  }
  r->tw = r->trows = r->sort_n = r->hist_len = r->red_levels = 0;
}

static void free_gl(cw__psys *r) {
  if (!r->gl_ready) return;
  glDeleteBuffers(4, &r->buf[0][0]);
  glDeleteVertexArrays(2, r->sim_vao);
  glDeleteVertexArrays(1, &r->draw_vao);
  free_tex(&r->curve_tex);
  free_tex(&r->emit_tex);
  if (r->mesh_vbo) glDeleteBuffers(1, &r->mesh_vbo);
  if (r->mesh_ibo) glDeleteBuffers(1, &r->mesh_ibo);
  r->mesh_vbo = r->mesh_ibo = 0;
  free_state(r);
  r->gl_ready = false;
  r->baked_ramp = r->baked_size = NULL;
  r->emit_built_node = 0;
  r->mesh_built_kind = -1;
}

static void upload_dead(cw__psys *r) {
  size_t n = (size_t)r->cap * 4;
  float *a = cw_alloc(n * sizeof(float), CW_MEM_RENDER);
  for (size_t i = 0; i < n; i += 4) {
    a[i] = a[i + 1] = a[i + 2] = 0;
    a[i + 3] = -1;
  }
  float *b = cw_alloc(n * sizeof(float), CW_MEM_RENDER);
  for (size_t i = 0; i < n; i += 4) {
    b[i] = b[i + 1] = b[i + 2] = 0;
    b[i + 3] = -1;
  }
  for (int p = 0; p < 2; p++) {
    glBindBuffer(GL_ARRAY_BUFFER, r->buf[p][0]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * sizeof(float)), a, GL_DYNAMIC_COPY);
    glBindBuffer(GL_ARRAY_BUFFER, r->buf[p][1]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * sizeof(float)), b, GL_DYNAMIC_COPY);
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  cw_free(a);
  cw_free(b);
  r->hist_count = 0;
  r->hist_head = 0;
}

static void ensure_gl(cw__psys *r, int amount) {
  if (r->gl_ready && r->cap == amount) return;
  free_gl(r);
  r->cap = amount;
  glGenBuffers(4, &r->buf[0][0]);
  glGenVertexArrays(2, r->sim_vao);
  glGenVertexArrays(1, &r->draw_vao);
  r->gl_ready = true;
  r->mesh_built_kind = -1;
  upload_dead(r);
  for (int p = 0; p < 2; p++) {
    glBindVertexArray(r->sim_vao[p]);
    for (int a = 0; a < 2; a++) {
      glBindBuffer(GL_ARRAY_BUFFER, r->buf[p][a]);
      glEnableVertexAttribArray((GLuint)a);
      glVertexAttribPointer((GLuint)a, 4, GL_FLOAT, GL_FALSE, 16, 0);
    }
  }
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  r->cur = 0;
}

static void ensure_curves(p3d *d, cw__psys *r) {
  if (r->curve_tex && r->baked_ramp == d->color_ramp && r->baked_size == d->size_curve) return;
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
  free_tex(&r->curve_tex);
  r->curve_tex = cw__fx_tex2d(CURVE_N, 2, GL_RGBA32F, GL_RGBA, GL_FLOAT, false, px);
  cw_free(px);
  r->baked_ramp = d->color_ramp;
  r->baked_size = d->size_curve;
}

/* the geometry of a MeshInstance3D and the mesh object's matrix relative to
 * its node */
t3_geometry *cw__psys_node_geometry(cw_node n, t3_mat4 *local) {
  t3_mesh *m = cw__mesh3d_t3(n);
  if (!m || !m->geometry) return NULL;
  t3_object_update_matrix(m);
  *local = m->base.matrix;
  return m->geometry;
}

static float tri_area(const float *a, const float *b, const float *c) {
  float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
  float x = u[1] * v[2] - u[2] * v[1], y = u[2] * v[0] - u[0] * v[2], z = u[0] * v[1] - u[1] * v[0];
  return 0.5f * sqrtf(x * x + y * y + z * z);
}

static uint32_t geo_index(const t3_geometry *g, int i) {
  if (!g->index) return (uint32_t)i;
  if (g->index->type == T3_UINT16) return ((const uint16_t *)g->index->array)[i];
  return ((const uint32_t *)g->index->array)[i];
}

cw_node cw__psys_ref_node(cw_node self, const char *path) {
  if (!path || !*path) return (cw_node){ 0 };
  return cw_node_get(self, path);
}

const float *cw__psys_emit_points(cw_node self, p3d *d, cw__psys *r, bool *changed) {
  *changed = false;
  cw_node em = cw__psys_ref_node(self, d->emission_mesh);
  t3_mat4 local;
  t3_geometry *g = cw_node_valid(em) ? cw__psys_node_geometry(em, &local) : NULL;
  if (!g) {
    if (r->emit_built_node != em.id) {
      cw_warn("GPUParticles3D: emission_shape MESH needs emission_mesh set to a MeshInstance3D with a single mesh");
      r->emit_built_node = em.id;
    }
    return NULL;
  }
  if (r->emit_px && r->emit_built_node == em.id && r->emit_built_geo == g) return r->emit_px;
  r->emit_built_node = em.id;
  r->emit_built_geo = g;
  t3_attribute *pos = g->attributes[T3_ATTR_POSITION], *nor = g->attributes[T3_ATTR_NORMAL];
  if (!pos || pos->type != T3_FLOAT32) return NULL;
  int tris = (g->index ? g->index->count : pos->count) / 3;
  if (tris <= 0) return NULL;
  float *cdf = cw_alloc(sizeof(float) * (size_t)tris, CW_MEM_RENDER);
  const float *P = pos->array;
  int ps = pos->item_size;
  float total = 0;
  for (int t = 0; t < tris; t++) {
    total += tri_area(P + ps * geo_index(g, 3 * t), P + ps * geo_index(g, 3 * t + 1), P + ps * geo_index(g, 3 * t + 2));
    cdf[t] = total;
  }
  if (!r->emit_px) r->emit_px = cw_calloc((size_t)EMIT_W * EMIT_W * 2 * 4, sizeof(float), CW_MEM_RENDER);
  float *px = r->emit_px;
  cw_rng rng;
  cw_rng_seed(&rng, (uint64_t)(uint32_t)d->seed * 0x9E3779B97F4A7C15ull + 17);
  for (int s = 0; s < EMIT_N; s++) {
    float x = cw_rng_float(&rng) * total;
    int lo = 0, hi = tris - 1;
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      if (cdf[mid] < x) lo = mid + 1;
      else hi = mid;
    }
    uint32_t ia = geo_index(g, 3 * lo), ib = geo_index(g, 3 * lo + 1), ic = geo_index(g, 3 * lo + 2);
    float u = cw_rng_float(&rng), v = cw_rng_float(&rng);
    if (u + v > 1) {
      u = 1 - u;
      v = 1 - v;
    }
    float w = 1 - u - v;
    t3_vec3 p, n;
    const float *a = P + ps * ia, *b = P + ps * ib, *c = P + ps * ic;
    p = (t3_vec3){ a[0] * w + b[0] * u + c[0] * v, a[1] * w + b[1] * u + c[1] * v, a[2] * w + b[2] * u + c[2] * v };
    if (nor && nor->type == T3_FLOAT32) {
      const float *N = nor->array;
      int ns = nor->item_size;
      const float *na = N + ns * ia, *nb = N + ns * ib, *nc = N + ns * ic;
      n = (t3_vec3){ na[0] * w + nb[0] * u + nc[0] * v, na[1] * w + nb[1] * u + nc[1] * v,
                     na[2] * w + nb[2] * u + nc[2] * v };
    } else {
      t3_vec3 e1 = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2 = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
      n = t3_vec3_cross(e1, e2);
    }
    p = t3_vec3_apply_mat4(p, &local);
    n = t3_vec3_normalize(t3_vec3_transform_direction(n, &local));
    float *o = px + 4 * s;
    o[0] = p.x;
    o[1] = p.y;
    o[2] = p.z;
    o[3] = 1;
    o = px + 4 * (EMIT_N + s);
    o[0] = n.x;
    o[1] = n.y;
    o[2] = n.z;
    o[3] = 0;
  }
  cw_free(cdf);
  *changed = true;
  return px;
}

static void ensure_emit_mesh(cw_node self, p3d *d, cw__psys *r) {
  bool changed;
  const float *px = cw__psys_emit_points(self, d, r, &changed);
  if (!px || (!changed && r->emit_tex)) return;
  free_tex(&r->emit_tex);
  r->emit_tex = cw__fx_tex2d(EMIT_W, EMIT_W * 2, GL_RGBA32F, GL_RGBA, GL_FLOAT, false, px);
}

void cw__psys_ensure_mesh(cw_node self, p3d *d, cw__psys *r) {
  cw_node mn = cw__psys_ref_node(self, d->mesh_node);
  t3_geometry *g = NULL;
  t3_mat4 local;
  t3_mat4_identity(&local);
  bool owned = false;
  if (d->mesh_primitive == MP_NODE) {
    g = cw_node_valid(mn) ? cw__psys_node_geometry(mn, &local) : NULL;
    if (!g) {
      if (r->mesh_built_kind != -2) cw_warn("GPUParticles3D: mesh_primitive NODE needs mesh_node set to a MeshInstance3D");
      r->mesh_built_kind = -2;
      r->mesh_count = 0;
      return;
    }
    if (r->mesh_count && r->mesh_built_kind == MP_NODE && r->mesh_built_geo == g) return;
  } else {
    if (r->mesh_count && r->mesh_built_kind == d->mesh_primitive) return;
    g = d->mesh_primitive == MP_SPHERE ? t3_sphere_geometry_new(0.5f, 12, 8)
        : d->mesh_primitive == MP_TETRA ? t3_tetrahedron_geometry_new(0.5f, 0)
                                         : t3_box_geometry_new(1, 1, 1, 1, 1, 1);
    owned = true;
  }
  t3_attribute *pos = g->attributes[T3_ATTR_POSITION], *nor = g->attributes[T3_ATTR_NORMAL];
  if (!pos || pos->type != T3_FLOAT32) {
    if (owned) t3_release(g);
    return;
  }
  int nv = pos->count;
  float *v = cw_alloc(sizeof(float) * 6 * (size_t)nv, CW_MEM_RENDER);
  for (int i = 0; i < nv; i++) {
    const float *pp = (const float *)pos->array + pos->item_size * i;
    t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ pp[0], pp[1], pp[2] }, &local);
    t3_vec3 n = { 0, 1, 0 };
    if (nor && nor->type == T3_FLOAT32) {
      const float *nn = (const float *)nor->array + nor->item_size * i;
      n = t3_vec3_normalize(t3_vec3_transform_direction((t3_vec3){ nn[0], nn[1], nn[2] }, &local));
    }
    float *o = v + 6 * i;
    o[0] = p.x;
    o[1] = p.y;
    o[2] = p.z;
    o[3] = n.x;
    o[4] = n.y;
    o[5] = n.z;
  }
  int ni = g->index ? g->index->count : nv;
  uint32_t *idx = cw_alloc(sizeof(uint32_t) * (size_t)ni, CW_MEM_RENDER);
  for (int i = 0; i < ni; i++) idx[i] = geo_index(g, i);
#if CW_WGPU
  if (cw__wgpu_on()) cw__psys_mesh_upload_wgpu(r, v, nv, idx, ni);
  else
#endif
  {
    if (!r->mesh_vbo) glGenBuffers(1, &r->mesh_vbo);
    if (!r->mesh_ibo) glGenBuffers(1, &r->mesh_ibo);
    glBindBuffer(GL_ARRAY_BUFFER, r->mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 6 * (size_t)nv), v, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->mesh_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint32_t) * (size_t)ni), idx, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  }
  cw_free(v);
  cw_free(idx);
  r->mesh_count = ni;
  r->mesh_built_kind = d->mesh_primitive;
  r->mesh_built_geo = d->mesh_primitive == MP_NODE ? g : NULL;
  if (owned) t3_release(g);
}

/* state textures: tw x trows, two RGBA32F targets */
static bool ensure_state(cw__psys *r) {
  if (r->state_fbo) return true;
  if (!cw__fx_float32_ok()) return false;
  r->tw = CW_MIN(r->cap, 1024);
  r->trows = (r->cap + r->tw - 1) / r->tw;
  for (int i = 0; i < 2; i++) r->state_tex[i] = cw__fx_tex2d(r->tw, r->trows, GL_RGBA32F, GL_RGBA, GL_FLOAT, false, NULL);
  r->state_fbo = cw__fx_fbo(r->state_tex[0], r->state_tex[1], 0);
  if (!r->state_fbo) return false;
  /* texels past `amount` are never written: dead (w = -1) */
  static const float dead[4] = { 0, 0, 0, -1 };
  glClearBufferfv(GL_COLOR, 0, dead);
  glClearBufferfv(GL_COLOR, 1, dead);
  return true;
}

static bool ensure_sort(cw__psys *r) {
  if (r->key_fbo[0]) return true;
  if (!cw__fx_float32_ok()) return false;
  int n = 1;
  while (n < r->cap) n <<= 1;
  r->sort_n = n;
  r->sw = CW_MIN(n, 1024);
  r->sh = n / r->sw;
  for (int i = 0; i < 2; i++) {
    r->key_tex[i] = cw__fx_tex2d(r->sw, r->sh, GL_RG32F, GL_RG, GL_FLOAT, false, NULL);
    r->key_fbo[i] = cw__fx_fbo(r->key_tex[i], 0, 0);
  }
  return r->key_fbo[0] && r->key_fbo[1];
}

static bool ensure_hist(cw__psys *r, int len) {
  if (r->hist_fbo && r->hist_len == len) return true;
  free_tex(&r->hist_tex);
  free_fbo(&r->hist_fbo);
  r->hist_len = 0;
  if (!ensure_state(r)) return false;
  GLint maxs = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxs);
  if (r->trows * len > maxs) {
    static bool warned;
    if (!warned)
      cw_error("GPUParticles3D: trails of %d samples for %d particles need a %dx%d texture (GL_MAX_TEXTURE_SIZE %d); "
               "lower amount or trail_length",
               len, r->cap, r->tw, r->trows * len, maxs);
    warned = true;
    return false;
  }
  r->hist_tex = cw__fx_tex2d(r->tw, r->trows * len, GL_RGBA32F, GL_RGBA, GL_FLOAT, false, NULL);
  r->hist_fbo = cw__fx_fbo(r->hist_tex, 0, 0);
  if (r->hist_fbo) {
    static const float dead[4] = { 0, 0, 0, -1 };
    glClearBufferfv(GL_COLOR, 0, dead);
  }
  r->hist_len = len;
  r->hist_head = 0;
  r->hist_count = 0;
  return r->hist_fbo != 0;
}

/* ── per frame ──────────────────────────────────────────────────────── */

static void bind_particle_attrs(cw__psys *r, int which, GLuint divisor) {
  for (int a = 0; a < 2; a++) {
    glBindBuffer(GL_ARRAY_BUFFER, r->buf[which][a]);
    glEnableVertexAttribArray((GLuint)a);
    glVertexAttribPointer((GLuint)a, 4, GL_FLOAT, GL_FALSE, 16, 0);
    glVertexAttribDivisor((GLuint)a, divisor);
  }
}

static void common_uniforms(GLuint prog, const p3d *d) {
  cw__fx_u1ui(prog, "u_seed", (unsigned)d->seed);
  cw__fx_u1i(prog, "u_amount", d->amount);
  cw__fx_u1f(prog, "u_lifetime", d->lifetime);
  cw__fx_u1f(prog, "u_life_rand", CW_CLAMP(d->lifetime_random, 0, 1));
}

/* the matrix particles spawn in: the emitter (or the emission mesh's node),
 * relative to the emitter when local_coords */
void cw__psys_spawn_frame(cw_node n, const p3d *d, t3_mat4 *out) {
  const t3_mat4 *E = cw_node3d_global_transform(n);
  cw_node em = cw__psys_ref_node(n, d->emission_mesh);
  if (d->emission_shape == SHAPE_MESH && cw_node_valid(em)) {
    const t3_mat4 *M = cw_node3d_global_transform(em);
    if (d->local_coords) {
      t3_mat4 inv;
      t3_mat4_invert(&inv, E);
      t3_mat4_multiply(out, &inv, M);
    } else *out = *M;
    return;
  }
  if (d->local_coords) t3_mat4_identity(out);
  else *out = *E;
}

int cw__psys_attractors(const cw__fx_tree *ft, float attr[MAX_ATTR * 8]) {
  int na = 0;
  for (int i = 0; i < ft->attractors.count && na < MAX_ATTR; i++) {
    cw_node an = ft->attractors.data[i];
    cw_value s, rad, att;
    if (!cw_node_get_prop(an, "strength", &s) || !cw_node_get_prop(an, "radius", &rad) ||
        !cw_node_get_prop(an, "attenuation", &att))
      continue;
    t3_vec3 ap = cw_node3d_global_position(an);
    float *o = attr + 8 * na++;
    o[0] = ap.x;
    o[1] = ap.y;
    o[2] = ap.z;
    o[3] = (float)rad.f;
    o[4] = (float)s.f;
    o[5] = (float)att.f;
    o[6] = o[7] = 0;
  }
  return na;
}

int cw__psys_colliders(const cw__fx_tree *ft, float cinv[MAX_COLL * 16], float cm[MAX_COLL * 16], float cp[MAX_COLL * 4]) {
  int nc = 0;
  for (int i = 0; i < ft->colliders.count && nc < MAX_COLL; i++) {
    cw_node cn = ft->colliders.data[i];
    cw_value sh, ex, rad;
    if (!cw_node_get_prop(cn, "shape", &sh) || !cw_node_get_prop(cn, "extents", &ex) ||
        !cw_node_get_prop(cn, "radius", &rad))
      continue;
    const t3_mat4 *W = cw_node3d_global_transform(cn);
    t3_mat4 inv;
    t3_mat4_invert(&inv, W);
    memcpy(cinv + 16 * nc, inv.e, 64);
    memcpy(cm + 16 * nc, W->e, 64);
    float *o = cp + 4 * nc++;
    o[0] = (float)sh.i;
    if (sh.i == 1) {
      o[1] = (float)rad.f;
      o[2] = o[3] = 0;
    } else {
      o[1] = ex.v[0];
      o[2] = ex.v[1];
      o[3] = ex.v[2];
    }
  }
  return nc;
}

static void run_steps(cw_node n, p3d *d, cw__psys *r, cw_pass_ctx *ctx, cw__fx_tree *ft) {
  if (r->nsteps == 0) return;
  GLuint p = prog_sim;
  glUseProgram(p);
  common_uniforms(p, d);
  cw__fx_u1f(p, "u_explosive", CW_CLAMP(d->explosiveness, 0, 1));
  cw__fx_u1i(p, "u_shape", d->emission_shape);
  cw__fx_u3f(p, "u_ext", d->emission_extents.x, d->emission_extents.y, d->emission_extents.z);
  cw__fx_u2f(p, "u_rad", d->emission_radius, d->emission_inner_radius);
  cw__fx_u1f(p, "u_nspeed", d->emission_normal_speed);
  t3_vec3 dir = d->direction;
  if (t3_vec3_length_sq(dir) < 1e-12f) dir = (t3_vec3){ 0, 1, 0 };
  dir = t3_vec3_normalize(dir);
  cw__fx_u3f(p, "u_dir", dir.x, dir.y, dir.z);
  cw__fx_u1f(p, "u_cos_spread", cosf(CW_CLAMP(d->spread, 0, 180) * (CW_PI / 180)));
  cw__fx_u2f(p, "u_speed", d->speed_min, d->speed_max);
  cw__fx_u3f(p, "u_gravity", d->gravity.x, d->gravity.y, d->gravity.z);
  cw__fx_u1f(p, "u_damping", d->damping);
  cw__fx_u3f(p, "u_turb", d->turbulence, d->turbulence_scale, d->turbulence_speed);
  cw__fx_u1i(p, "u_noise", 0);
  cw__fx_bind_tex(0, GL_TEXTURE_3D, cw__fx_noise3d());
  float attr[MAX_ATTR * 8];
  int na = cw__psys_attractors(ft, attr);
  cw__fx_u1i(p, "u_nattr", na);
  cw__fx_u4fv(p, "u_attr", attr, na * 2);
  float cinv[MAX_COLL * 16], cm[MAX_COLL * 16], cp[MAX_COLL * 4];
  int nc = cw__psys_colliders(ft, cinv, cm, cp);
  cw__fx_u1i(p, "u_ncol", nc);
  if (nc) {
    glUniformMatrix4fv(cw__fx_loc(p, "u_col_inv"), nc, GL_FALSE, cinv);
    glUniformMatrix4fv(cw__fx_loc(p, "u_col_m"), nc, GL_FALSE, cm);
    cw__fx_u4fv(p, "u_col_p", cp, nc);
  }
  cw__fx_u3f(p, "u_coll", d->bounce, CW_CLAMP(d->friction, 0, 1), d->collision_radius);
  bool depth = d->collide_depth && ctx->depth_tex && ctx->camera;
  cw__fx_u1i(p, "u_depth_on", depth ? 1 : 0);
  cw__fx_u1i(p, "u_depth", 1);
  cw__fx_bind_tex(1, GL_TEXTURE_2D, depth ? ctx->depth_tex : 0);
  if (depth) {
    cw__fx_mat4(p, "u_vp", ctx->view_proj.e);
    cw__fx_mat4(p, "u_ivp", ctx->inv_view_proj.e);
    cw__fx_u2f(p, "u_texel", 1.0f / (float)ctx->width, 1.0f / (float)ctx->height);
    cw__fx_u1f(p, "u_thick", d->depth_thickness);
    t3_vec3 cp3 = t3_mat4_get_position(&ctx->camera->base.matrix_world);
    cw__fx_u3f(p, "u_campos", cp3.x, cp3.y, cp3.z);
  }
  cw__fx_u1i(p, "u_emit", 2);
  if (d->emission_shape == SHAPE_MESH) ensure_emit_mesh(n, d, r);
  cw__fx_bind_tex(2, GL_TEXTURE_2D, d->emission_shape == SHAPE_MESH ? r->emit_tex : 0);
  if (d->emission_shape == SHAPE_MESH && !r->emit_tex) cw__fx_u1i(p, "u_shape", SHAPE_POINT);
  cw__fx_u1f(p, "u_emit_end", r->emit_end < 0 ? -1.0f : (float)r->emit_end);

  bool hist = d->trail_length >= 2 && ensure_hist(r, CW_MIN(d->trail_length, TRAIL_MAX));
  glEnable(GL_RASTERIZER_DISCARD); /* after ensure_hist: it clears, and discard drops clears too */
  for (int s = 0; s < r->nsteps; s++) {
    const step_rec *st = &r->steps[s];
    glUseProgram(p);
    cw__fx_u1f(p, "u_t0", st->t0);
    cw__fx_u1f(p, "u_t1", st->t1);
    cw__fx_mat4(p, "u_em0", st->m0.e);
    cw__fx_mat4(p, "u_em1", st->m1.e);
    glBindVertexArray(r->sim_vao[r->cur]);
    int nxt = 1 - r->cur;
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, r->buf[nxt][0]);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1, r->buf[nxt][1]);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, d->amount);
    glEndTransformFeedback();
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 1, 0);
    r->cur = nxt;
    ft->stats.sim_steps++;
    if (hist) {
      /* the new state into history slot head + 1 */
      glDisable(GL_RASTERIZER_DISCARD);
      r->hist_head = (r->hist_head + 1) % r->hist_len;
      r->hist_count = CW_MIN(r->hist_count + 1, r->hist_len);
      glBindFramebuffer(GL_FRAMEBUFFER, r->hist_fbo);
      glViewport(0, 0, r->tw, r->trows * r->hist_len);
      glUseProgram(prog_hist);
      cw__fx_u1i(prog_hist, "u_tw", r->tw);
      cw__fx_u1i(prog_hist, "u_row0", r->hist_head * r->trows);
      cw__fx_u2f(prog_hist, "u_size", (float)r->tw, (float)(r->trows * r->hist_len));
      glBindVertexArray(r->sim_vao[r->cur]);
      glDrawArrays(GL_POINTS, 0, d->amount);
      glEnable(GL_RASTERIZER_DISCARD);
    }
  }
  glDisable(GL_RASTERIZER_DISCARD);
  glBindVertexArray(0);
  r->nsteps = 0;
}

/* current state into the state textures (MRT points) */
static bool scatter_state(p3d *d, cw__psys *r) {
  if (!ensure_state(r)) return false;
  glBindFramebuffer(GL_FRAMEBUFFER, r->state_fbo);
  glViewport(0, 0, r->tw, r->trows);
  glUseProgram(prog_scatter);
  cw__fx_u1i(prog_scatter, "u_tw", r->tw);
  cw__fx_u1i(prog_scatter, "u_row0", 0);
  cw__fx_u2f(prog_scatter, "u_size", (float)r->tw, (float)r->trows);
  glBindVertexArray(r->sim_vao[r->cur]);
  glDrawArrays(GL_POINTS, 0, d->amount);
  glBindVertexArray(0);
  return true;
}

static int bitonic_sort(cw_pass_ctx *ctx, p3d *d, cw__psys *r, const t3_mat4 *model) {
  if (!ensure_sort(r)) return 0;
  t3_mat4 mv;
  t3_mat4_multiply(&mv, &ctx->view, model);
  glBindFramebuffer(GL_FRAMEBUFFER, r->key_fbo[0]);
  glViewport(0, 0, r->sw, r->sh);
  glUseProgram(prog_key);
  cw_pass_bind_texture(0, r->state_tex[0]);
  cw__fx_u1i(prog_key, "u_state0", 0);
  cw__fx_u1i(prog_key, "u_tw", r->tw);
  cw__fx_u1i(prog_key, "u_amount", d->amount);
  cw__fx_u1i(prog_key, "u_sw", r->sw);
  cw__fx_mat4(prog_key, "u_mv", mv.e);
  cw_pass_fullscreen();
  int src = 0, passes = 0;
  glUseProgram(prog_bitonic);
  cw__fx_u1i(prog_bitonic, "u_keys", 0);
  cw__fx_u1i(prog_bitonic, "u_sw", r->sw);
  for (int k = 2; k <= r->sort_n; k <<= 1)
    for (int j = k >> 1; j > 0; j >>= 1) {
      glBindFramebuffer(GL_FRAMEBUFFER, r->key_fbo[1 - src]);
      cw_pass_bind_texture(0, r->key_tex[src]);
      cw__fx_u1i(prog_bitonic, "u_j", j);
      cw__fx_u1i(prog_bitonic, "u_k", k);
      cw_pass_fullscreen();
      src = 1 - src;
      passes++;
    }
  r->sorted = src;
  return passes;
}

static void gpu_stats(p3d *d, cw__psys *r) {
  if (!r->red_levels) {
    int w = r->tw, h = r->trows, l = 0;
    while ((w > 1 || h > 1) && l < 4) {
      w = (w + 7) / 8;
      h = (h + 7) / 8;
      r->red_w[l] = w;
      r->red_h[l] = h;
      r->red_tex[l] = cw__fx_tex2d(w, h, GL_RGBA32F, GL_RGBA, GL_FLOAT, false, NULL);
      r->red_fbo[l] = cw__fx_fbo(r->red_tex[l], 0, 0);
      l++;
    }
    r->red_levels = l;
  }
  glUseProgram(prog_reduce);
  cw__fx_u1i(prog_reduce, "u_src", 0);
  GLuint src = r->state_tex[0];
  int sw = r->tw, sh = r->trows;
  for (int l = 0; l < r->red_levels; l++) {
    glBindFramebuffer(GL_FRAMEBUFFER, r->red_fbo[l]);
    glViewport(0, 0, r->red_w[l], r->red_h[l]);
    cw_pass_bind_texture(0, src);
    glUniform2i(cw__fx_loc(prog_reduce, "u_src_size"), sw, sh);
    cw__fx_u1i(prog_reduce, "u_first", l == 0);
    cw_pass_fullscreen();
    src = r->red_tex[l];
    sw = r->red_w[l];
    sh = r->red_h[l];
  }
  float px[4] = { 0, 0, 0, 0 };
  if (r->red_levels) glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, px);
  d->gpu_alive = (int)(px[3] + 0.5f);
  d->gpu_sum = (t3_vec3){ px[0], px[1], px[2] };
}

static void set_blend(int blend) {
  if (blend == BLEND_OPAQUE) {
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    return;
  }
  glEnable(GL_BLEND);
  glDepthMask(GL_FALSE);
  if (blend == BLEND_ADD) glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
  else glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
}

/* every system's frame. WebGPU: their compute work in one compute pass,
 * then their draws in one render pass, in the systems' order (a render pass
 * loads and stores the scene's targets: once a view, not once a system) */
void cw__psys_frames(cw_pass_ctx *ctx, cw__fx_tree *ft) {
#if CW_WGPU
  if (cw__wgpu_on()) {
    int n = ft->systems.count;
    bool *draws = cw_frame_alloc((size_t)(n ? n : 1));
    WGPUComputePassEncoder cp = NULL;
    bool any = false;
    for (int i = 0; i < n; i++) {
      p3d *d = cw_node_data_t(ft->systems.data[i], p3d_t);
      draws[i] = d && d->rt && d->amount > 0 && d->lifetime > 0 && cw__psys_frame_wgpu(ft->systems.data[i], d, ctx, ft, &cp);
      any |= draws[i];
    }
    if (cp) {
      wgpuComputePassEncoderEnd(cp);
      wgpuComputePassEncoderRelease(cp);
    }
    if (!any) return;
    WGPURenderPassEncoder rp = cw__wg_scene_pass(cw__fx_pass_targets(ctx), true, "particles");
    for (int i = 0; i < n; i++)
      if (draws[i]) cw__psys_draw_wgpu(rp, ft->systems.data[i], cw_node_data_t(ft->systems.data[i], p3d_t), ctx, ft);
    wgpuRenderPassEncoderEnd(rp);
    wgpuRenderPassEncoderRelease(rp);
    return;
  }
#endif
  for (int i = 0; i < ft->systems.count; i++) cw__psys_frame(ft->systems.data[i], ctx, ft);
}

void cw__psys_frame(cw_node n, cw_pass_ctx *ctx, cw__fx_tree *ft) {
  p3d *d = cw_node_data_t(n, p3d_t);
  if (!d || !d->rt || d->amount <= 0 || d->lifetime <= 0) return;
  cw__psys *r = d->rt;
  if (r->idle && r->nsteps == 0 && !d->gpu_stats) { /* nothing alive, nothing to simulate (p3d_fixed) */
    ft->stats.systems++;
    ft->stats.capacity += d->amount;
    return;
  }
  if (!build_programs()) return;
  ensure_gl(r, d->amount);
  if (r->need_reset) {
    upload_dead(r);
    r->need_reset = false;
  }
  ensure_curves(d, r);
  ft->stats.systems++;
  ft->stats.capacity += d->amount;
  ft->stats.alive += d->alive;
  run_steps(n, d, r, ctx, ft);

  t3_mat4 model;
  if (d->local_coords) {
    t3_object *o = cw_node3d_object(n);
    t3_object_update_world_matrix(o, true, false);
    model = o->matrix_world;
  } else t3_mat4_identity(&model);

  bool want_sort = d->sort && d->blend == BLEND_ALPHA;
  bool want_trail = d->trail_length >= 2 && r->hist_fbo && r->hist_count >= 2;
  bool have_state = false;
  if (want_sort || want_trail || d->gpu_stats) have_state = scatter_state(d, r);
  if (d->gpu_stats && have_state) gpu_stats(d, r);
  bool sorted = false;
  if (want_sort && have_state) {
    int np = bitonic_sort(ctx, d, r, &model);
    ft->stats.sort_passes += np;
    sorted = np > 0 || r->sort_n == 1;
  }

  /* draw into the scene target */
  cw_pass_bind_target(ctx, ctx->scene);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDisable(GL_CULL_FACE);
  set_blend(d->blend);
  t3_vec3 campos = t3_mat4_get_position(&ctx->camera->base.matrix_world);
  float lp[3];
  cw_pass_linear_depth_params(ctx, lp);

  if (want_trail && have_state) {
    glUseProgram(prog_trail);
    common_uniforms(prog_trail, d);
    cw__fx_bind_tex(0, GL_TEXTURE_2D, r->state_tex[0]);
    cw__fx_bind_tex(1, GL_TEXTURE_2D, r->state_tex[1]);
    cw__fx_bind_tex(2, GL_TEXTURE_2D, r->hist_tex);
    cw__fx_bind_tex(3, GL_TEXTURE_2D, r->curve_tex);
    cw__fx_u1i(prog_trail, "u_state0", 0);
    cw__fx_u1i(prog_trail, "u_state1", 1);
    cw__fx_u1i(prog_trail, "u_hist", 2);
    cw__fx_u1i(prog_trail, "u_curve", 3);
    cw__fx_u1i(prog_trail, "u_tw", r->tw);
    cw__fx_u1i(prog_trail, "u_rows", r->trows);
    cw__fx_u1i(prog_trail, "u_hlen", r->hist_len);
    cw__fx_u1i(prog_trail, "u_head", r->hist_head);
    cw__fx_u1i(prog_trail, "u_hcount", r->hist_count);
    cw__fx_mat4(prog_trail, "u_view", ctx->view.e);
    cw__fx_mat4(prog_trail, "u_proj", ctx->camera->projection_matrix.e);
    cw__fx_mat4(prog_trail, "u_model", model.e);
    cw__fx_u3f(prog_trail, "u_campos", campos.x, campos.y, campos.z);
    cw__fx_u4f(prog_trail, "u_color", d->color[0], d->color[1], d->color[2], d->color[3]);
    cw__fx_u1f(prog_trail, "u_energy", d->energy);
    cw__fx_u1f(prog_trail, "u_width", d->trail_width);
    cw__fx_u1i(prog_trail, "u_blend", d->blend == BLEND_OPAQUE ? BLEND_ALPHA : d->blend);
    if (d->blend == BLEND_OPAQUE) set_blend(BLEND_ALPHA);
    glBindVertexArray(cw__fx_empty_vao());
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 2 * r->hist_len, cw_quality_count(d->amount, "particles"));
    ft->stats.draw_calls++;
    set_blend(d->blend);
  }

  GLuint dp = prog_draw[sorted ? 1 : 0];
  glUseProgram(dp);
  common_uniforms(dp, d);
  cw__fx_mat4(dp, "u_view", ctx->view.e);
  cw__fx_mat4(dp, "u_proj", ctx->camera->projection_matrix.e);
  cw__fx_mat4(dp, "u_model", model.e);
  cw__fx_u3f(dp, "u_campos", campos.x, campos.y, campos.z);
  cw__fx_u4f(dp, "u_color", d->color[0], d->color[1], d->color[2], d->color[3]);
  cw__fx_u1f(dp, "u_energy", d->energy);
  cw__fx_u1f(dp, "u_size", d->size);
  cw__fx_u1f(dp, "u_size_rand", CW_CLAMP(d->size_random, 0, 1));
  cw__fx_u1f(dp, "u_stretch", d->stretch);
  cw__fx_u1f(dp, "u_spin", d->spin);
  cw__fx_u2f(dp, "u_frames", (float)CW_MAX(d->hframes, 1), (float)CW_MAX(d->vframes, 1));
  cw__fx_u1i(dp, "u_mode", d->draw_mode);
  cw__fx_u1i(dp, "u_blend", d->blend);
  GLuint tex = d->texture && *d->texture ? cw__fx_texture_asset(d->texture, NULL, NULL) : 0;
  if (!tex) tex = cw__fx_dot_texture();
  cw__fx_bind_tex(0, GL_TEXTURE_2D, tex);
  cw__fx_bind_tex(1, GL_TEXTURE_2D, ctx->depth_tex);
  cw__fx_bind_tex(2, GL_TEXTURE_2D, r->curve_tex);
  cw__fx_u1i(dp, "u_tex", 0);
  cw__fx_u1i(dp, "u_depth", 1);
  cw__fx_u1i(dp, "u_curve", 2);
  cw__fx_u4f(dp, "u_soft", ctx->depth_tex ? d->soft_distance : 0, lp[0], lp[1], lp[2]);
  cw__fx_u2f(dp, "u_inv_size", 1.0f / (float)ctx->width, 1.0f / (float)ctx->height);
  if (sorted) {
    cw__fx_bind_tex(3, GL_TEXTURE_2D, r->state_tex[0]);
    cw__fx_bind_tex(4, GL_TEXTURE_2D, r->state_tex[1]);
    cw__fx_bind_tex(5, GL_TEXTURE_2D, r->key_tex[r->sorted]);
    cw__fx_u1i(dp, "u_state0", 3);
    cw__fx_u1i(dp, "u_state1", 4);
    cw__fx_u1i(dp, "u_order", 5);
    cw__fx_u1i(dp, "u_tw", r->tw);
    cw__fx_u1i(dp, "u_ow", r->sw);
  }
  glBindVertexArray(r->draw_vao);
  if (!sorted) bind_particle_attrs(r, r->cur, 1);
  else {
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
  }
  bool mesh = d->draw_mode == DRAW_MESH;
  if (mesh) {
    cw__psys_ensure_mesh(n, d, r);
    if (!r->mesh_count) {
      glBindVertexArray(0);
      return;
    }
    glBindVertexArray(r->draw_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->mesh_vbo);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 24, 0);
    glVertexAttribDivisor(2, 0);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 24, (const void *)12);
    glVertexAttribDivisor(3, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->mesh_ibo);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDrawElementsInstanced(GL_TRIANGLES, r->mesh_count, GL_UNSIGNED_INT, 0, d->amount);
    glDisable(GL_CULL_FACE);
  } else {
    glBindBuffer(GL_ARRAY_BUFFER, cw__fx_quad_vbo());
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8, 0);
    glVertexAttribDivisor(2, 0);
    glDisableVertexAttribArray(3);
    static const float nz[4] = { 0, 0, 1, 0 };
    glVertexAttrib4fv(3, nz);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, cw_quality_count(d->amount, "particles"));
  }
  ft->stats.draw_calls++;
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
}

/* tests: the GL path's sorted keys (RG32F texels: key, index) */
unsigned cw__psys_gl_keys(cw_node n, int *sw, int *sh, int *amount);
unsigned cw__psys_gl_keys(cw_node n, int *sw, int *sh, int *amount) {
  cw__psys *r = cw__psys_of(n);
  if (!r || !r->key_fbo[0]) return 0;
  *sw = r->sw;
  *sh = r->sh;
  *amount = r->cap;
  return r->key_fbo[r->sorted];
}

/* ── the node ───────────────────────────────────────────────────────── */

static void p3d_init(cw_node n, void *data) {
  (void)n;
  p3d *d = data;
  d->emitting = true;
  d->amount = 64;
  d->lifetime = 1;
  d->speed_scale = 1;
  d->seed = 1;
  d->emission_extents = (t3_vec3){ 1, 1, 1 };
  d->emission_radius = 1;
  d->direction = (t3_vec3){ 0, 1, 0 };
  d->spread = 15;
  d->speed_min = 1;
  d->speed_max = 2;
  d->gravity = (t3_vec3){ 0, -9.8f, 0 };
  d->turbulence_scale = 0.25f;
  d->turbulence_speed = 0.2f;
  d->depth_thickness = 0.5f;
  d->bounce = 0.5f;
  d->friction = 0.1f;
  d->collision_radius = 0.02f;
  d->size = 0.1f;
  d->stretch = 0.05f;
  d->color[0] = d->color[1] = d->color[2] = d->color[3] = 1;
  d->energy = 1;
  d->color_ramp = cw_intern("");
  d->size_curve = cw_intern("");
  d->texture = cw_intern("");
  d->emission_mesh = cw_intern("");
  d->mesh_node = cw_intern("");
  d->hframes = d->vframes = 1;
  d->blend = BLEND_ADD;
  d->trail_width = 0.05f;
  d->rt = cw_calloc(1, sizeof *d->rt, CW_MEM_RENDER);
  d->rt->steps = cw_alloc(sizeof(step_rec) * MAX_STEPS, CW_MEM_RENDER);
  d->rt->emit_start = -1;
  d->rt->emit_end = -1;
  d->rt->mesh_built_kind = -1;
}

static void p3d_destroy(cw_node n, void *data) {
  (void)n;
  p3d *d = data;
  if (!d->rt) return;
  free_gl(d->rt);
#if CW_WGPU
  cw__psys_free_wgpu(d->rt);
#endif
  cw_free(d->rt->emit_px);
  cw_free(d->rt->steps);
  cw_free(d->rt);
  d->rt = NULL;
}

static void p3d_enter(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  cw__fx_tree *ft = cw__fx_tree_of(t);
  cw_array_push(&ft->systems, n);
  cw__fx_ensure_pass(t);
}

static void p3d_exit(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  if (t) cw__fx_remove_node(&cw__fx_tree_of(t)->systems, n);
}

static void push_step(cw__psys *r, float t0, float t1, const t3_mat4 *m0, const t3_mat4 *m1) {
  if (r->nsteps == MAX_STEPS) {
    /* the pass has not run for MAX_STEPS ticks (no camera): fold the oldest
     * two steps into one so time is never lost */
    r->steps[1].t0 = r->steps[0].t0;
    r->steps[1].m0 = r->steps[0].m0;
    memmove(r->steps, r->steps + 1, sizeof(step_rec) * (MAX_STEPS - 1));
    r->nsteps--;
  }
  step_rec *s = &r->steps[r->nsteps++];
  s->t0 = t0;
  s->t1 = t1;
  s->m0 = *m0;
  s->m1 = *m1;
}

static void p3d_fixed(cw_node n, void *data, double dt) {
  p3d *d = data;
  cw__psys *r = d->rt;
  if (!r) return;
  double step = dt * (double)d->speed_scale;
  t3_mat4 m;
  cw__psys_spawn_frame(n, d, &m);
  if (!r->have_last_m) {
    r->last_m = m;
    r->have_last_m = true;
  }
  if (d->emitting && !r->was_emitting) {
    /* a new emission window: everything alive dies, cycles restart */
    double pre = d->preprocess > 0 ? d->preprocess : 0;
    r->emit_start = r->sim_time - pre;
    r->emit_end = d->one_shot ? (double)d->lifetime : -1;
    r->need_reset = true;
    r->nsteps = 0;
    /* a window starts where the emitter is now: an emitter moved and
     * restarted in one tick (a pooled effect sent to a new spot) must not
     * spread its first step's particles along the path from where it was */
    r->last_m = m;
    if (pre > 0 && step > 0) {
      int k = (int)ceil(pre / step);
      for (int i = 0; i < k; i++) {
        double t0 = r->sim_time - pre + pre * i / k, t1 = r->sim_time - pre + pre * (i + 1) / k;
        push_step(r, (float)(t0 - r->emit_start), (float)(t1 - r->emit_start), &m, &m);
      }
    }
  } else if (!d->emitting && r->was_emitting && r->emit_end < 0) {
    r->emit_end = r->sim_time - r->emit_start;
  }
  r->was_emitting = d->emitting;
  if (r->emit_start < 0 && !d->emitting) {
    r->sim_time += step;
    r->last_m = m;
    r->idle = true;
    return;
  }
  double t0 = r->sim_time - r->emit_start;
  /* A closed window (one shot done, or emitting turned off) whose particles
   * have all died: every particle spawned before emit_end and lives at most
   * one lifetime, so from emit_end + lifetime on none is alive and none can
   * spawn, and cpu_alive stays 0 (checked here a lifetime later still, with
   * the last count 0, so rounding cannot matter). The GPU state is all dead
   * already; a new window resets it (need_reset). Skip the steps, the alive
   * count and, in the frame, the GPU work: an idle pooled effect cost a sim
   * pass, a draw and ~40 uniforms every frame. */
  if (!d->emitting && r->emit_end >= 0 && d->alive == 0 && t0 >= r->emit_end + 2.0 * (double)d->lifetime) {
    r->sim_time += step;
    r->last_m = m;
    r->idle = true;
    return;
  }
  r->idle = false;
  r->sim_time += step;
  double t1 = r->sim_time - r->emit_start;
  if (step > 0) push_step(r, (float)t0, (float)t1, &r->last_m, &m);
  r->last_m = m;
  /* one shot: done once the last particle's cycle is over */
  if (d->one_shot && d->emitting && t1 >= 2.0 * d->lifetime) d->emitting = false;
  d->alive = cpu_alive(d);
}

static void p3d_changed(cw_node n, void *data, const cw_prop *p) {
  (void)n;
  p3d *d = data;
  if (!strcmp(p->name, "seed") || !strcmp(p->name, "amount") || !strcmp(p->name, "lifetime") ||
      !strcmp(p->name, "explosiveness") || !strcmp(p->name, "one_shot"))
    d->rt->was_emitting = false; /* restart the window on the next tick */
  if (!strcmp(p->name, "trail_length")) d->trail_length = CW_CLAMP(d->trail_length, 0, TRAIL_MAX);
  if (!strcmp(p->name, "amount")) d->amount = CW_CLAMP(d->amount, 0, 1 << 22);
}

void cw_particles_restart(cw_node n) {
  p3d *d = cw_node_data_t(n, p3d_t);
  if (d) {
    d->emitting = true;
    d->rt->was_emitting = false;
    return;
  }
  void cw__particles2d_restart(cw_node n);
  cw__particles2d_restart(n);
}

int cw_particles_alive(cw_node n) {
  p3d *d = cw_node_data_t(n, p3d_t);
  if (d) return d->alive;
  int cw__particles2d_alive(cw_node n);
  return cw__particles2d_alive(n);
}

#define F(field, ty, doc) CW_PROP_FIELD(p3d, field, ty, CW_PROP_DEFAULT, doc)
#define R(field, ty, lo, hi, doc) CW_PROP_FIELD_RANGE(p3d, field, ty, lo, hi, doc)
#define E(field, names, ds) \
  { .name = #field, .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(p3d, field), .size = sizeof(int), \
    .enum_names = names, .doc = ds }
#define RO(field, ty, doc) CW_PROP_FIELD(p3d, field, ty, CW_PROP_EDITOR | CW_PROP_READONLY, doc)

static const cw_prop p3d_props[] = {
  F(emitting, CW_T_BOOL, "Emitting new particles (turning it on starts a new emission window: alive particles die)."),
  F(amount, CW_T_INT, "Particle count (buffer size); emission rate = amount / lifetime."),
  R(lifetime, CW_T_FLOAT, 0.001, 1e6, "Seconds each particle lives (the spawn cycle length)."),
  R(lifetime_random, CW_T_FLOAT, 0, 1, "Each life is shortened by up to this fraction (hashed per particle)."),
  R(explosiveness, CW_T_FLOAT, 0, 1, "0 = spawns spread over the lifetime, 1 = all at once."),
  F(one_shot, CW_T_BOOL, "Emit one cycle, then turn emitting off."),
  R(preprocess, CW_T_FLOAT, 0, 600, "Seconds simulated when emission starts (the effect starts mature)."),
  R(speed_scale, CW_T_FLOAT, 0, 64, "Sim seconds per tick second."),
  F(seed, CW_T_INT, "Random seed: same seed, same particles on every host."),
  F(local_coords, CW_T_BOOL, "Particles live in the emitter's space and follow it (forces too)."),
  E(emission_shape, shape_names, "Where particles spawn (in the emitter's space; MESH: on emission_mesh's surface)."),
  F(emission_extents, CW_T_VEC3, "BOX: half sizes."),
  R(emission_radius, CW_T_FLOAT, 0, 1e6, "SPHERE / SPHERE_SURFACE / RING radius."),
  R(emission_inner_radius, CW_T_FLOAT, 0, 1e6, "RING: inner radius."),
  F(emission_mesh, CW_T_STRING, "MESH: path (from this node) to a MeshInstance3D whose surface emits (4096 points by area, with normals)."),
  F(emission_normal_speed, CW_T_FLOAT, "Extra start speed along the surface normal (SPHERE_SURFACE, MESH)."),
  F(direction, CW_T_VEC3, "Start direction (emitter space)."),
  R(spread, CW_T_FLOAT, 0, 180, "Cone half angle around direction, degrees."),
  F(speed_min, CW_T_FLOAT, "Start speed range."),
  F(speed_max, CW_T_FLOAT, "Start speed range."),
  F(gravity, CW_T_VEC3, "Acceleration (world space; emitter space with local_coords)."),
  R(damping, CW_T_FLOAT, 0, 1000, "Velocity damping per second."),
  F(turbulence, CW_T_FLOAT, "Curl-noise acceleration strength (0 = off)."),
  F(turbulence_scale, CW_T_FLOAT, "Noise frequency (noise cells per world unit)."),
  F(turbulence_speed, CW_T_FLOAT, "Noise scroll speed (cells per second)."),
  F(collide_depth, CW_T_BOOL, "Bounce off the scene depth buffer (screen space: only visible surfaces)."),
  R(depth_thickness, CW_T_FLOAT, 0, 1e6, "Depth collision: how deep behind a visible surface still counts as solid."),
  R(bounce, CW_T_FLOAT, 0, 2, "Normal velocity kept after a collision."),
  R(friction, CW_T_FLOAT, 0, 1, "Tangential velocity lost at a collision."),
  R(collision_radius, CW_T_FLOAT, 0, 1e6, "Particle radius for collisions."),
  E(draw_mode, draw_names, "BILLBOARD quads, STRETCHED along velocity, or instanced MESH."),
  E(mesh_primitive, mprim_names, "MESH draw mode: BOX, SPHERE, TETRA or NODE (mesh_node's mesh)."),
  F(mesh_node, CW_T_STRING, "MESH + NODE: path (from this node) to the MeshInstance3D drawn per particle."),
  R(size, CW_T_FLOAT, 0, 1e6, "Particle size (world units) times size_curve."),
  R(size_random, CW_T_FLOAT, 0, 1, "Size shrunk by up to this fraction per particle."),
  F(stretch, CW_T_FLOAT, "STRETCHED: extra length per unit of speed."),
  F(color, CW_T_COLOR, "Tint (times color_ramp)."),
  R(energy, CW_T_FLOAT, 0, 1e6, "Colour multiplier (HDR glow)."),
  F(color_ramp, CW_T_STRING, "Colour over life: \"t r g b a, ...\" (empty = white)."),
  F(size_curve, CW_T_STRING, "Size over life: \"t s, ...\" (empty = 1)."),
  F(texture, CW_T_STRING, "Billboard image (asset path; empty = a soft dot)."),
  F(hframes, CW_T_INT, "Flipbook columns (frames play over life)."),
  F(vframes, CW_T_INT, "Flipbook rows."),
  F(spin, CW_T_FLOAT, "Rotation speed, rad/s (random direction per particle)."),
  E(blend, blend_names, "ADD (glow), ALPHA (premultiplied, sortable) or OPAQUE (alpha tested, writes depth)."),
  R(soft_distance, CW_T_FLOAT, 0, 1e6, "Fade over this distance in front of geometry (0 = hard)."),
  F(sort, CW_T_BOOL, "ALPHA: draw back to front (GPU bitonic sort)."),
  F(trail_length, CW_T_INT, "Trail samples per particle, one per tick (0 = none, max 64)."),
  R(trail_width, CW_T_FLOAT, 0, 1e6, "Trail width at the particle (tapers to 0)."),
  RO(alive, CW_T_INT, "Particles alive now (the CPU's evaluation of the emission function)."),
  F(gpu_stats, CW_T_BOOL, "Read the GPU's alive count and position sum back every frame (a stall: tests)."),
  RO(gpu_alive, CW_T_INT, "gpu_stats: alive particles counted on the GPU."),
  RO(gpu_sum, CW_T_VEC3, "gpu_stats: sum of alive particle positions."),
  CW_PROP_END,
};

static cw_node_type p3d_type = {
  .name = "GPUParticles3D", .parent = "Node3D", .data_size = sizeof(p3d), .props = p3d_props,
  .doc = "GPU particles: transform feedback simulation, instanced billboards / meshes, trails (particles.h).",
  .init = p3d_init, .destroy = p3d_destroy, .enter_tree = p3d_enter, .exit_tree = p3d_exit,
  .fixed_update = p3d_fixed, .prop_changed = p3d_changed,
};

/* ── attractors and colliders ───────────────────────────────────────── */

typedef struct attractor { float strength, radius, attenuation; } attractor;
static void attr_init(cw_node n, void *d) {
  (void)n;
  attractor *a = d;
  a->strength = 5;
  a->radius = 2;
  a->attenuation = 1;
}
static void attr_enter(cw_node n, void *d) {
  (void)d;
  cw_array_push(&cw__fx_tree_of(cw_node_tree(n))->attractors, n);
}
static void attr_exit(cw_node n, void *d) {
  (void)d;
  if (cw_node_tree(n)) cw__fx_remove_node(&cw__fx_tree_of(cw_node_tree(n))->attractors, n);
}
static const cw_prop attr_props[] = {
  CW_PROP_FIELD(attractor, strength, CW_T_FLOAT, CW_PROP_DEFAULT, "Acceleration toward the centre (negative repels)."),
  CW_PROP_FIELD_RANGE(attractor, radius, CW_T_FLOAT, 0, 1e6, "Reach."),
  CW_PROP_FIELD_RANGE(attractor, attenuation, CW_T_FLOAT, 0, 16, "Falloff exponent of (1 - d / radius)."),
  CW_PROP_END,
};
static cw_node_type attr_type = {
  .name = "ParticleAttractor3D", .parent = "Node3D", .data_size = sizeof(attractor), .props = attr_props,
  .doc = "Pulls (or pushes) the tree's GPU particles; up to 8 act at once.", .init = attr_init,
  .enter_tree = attr_enter, .exit_tree = attr_exit,
};

typedef struct collider { int shape; t3_vec3 extents; float radius; } collider;
static const char *const coll_names[] = { "PLANE", "SPHERE", "BOX", NULL };
static void coll_init(cw_node n, void *d) {
  (void)n;
  collider *c = d;
  c->shape = 0;
  c->extents = (t3_vec3){ 0, 0, 0 };
  c->radius = 1;
}
static void coll_enter(cw_node n, void *d) {
  (void)d;
  cw_array_push(&cw__fx_tree_of(cw_node_tree(n))->colliders, n);
}
static void coll_exit(cw_node n, void *d) {
  (void)d;
  if (cw_node_tree(n)) cw__fx_remove_node(&cw__fx_tree_of(cw_node_tree(n))->colliders, n);
}
static const cw_prop coll_props[] = {
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = offsetof(collider, shape),
    .size = sizeof(int), .enum_names = coll_names, .doc = "PLANE (XZ, +Y up), SPHERE or BOX." },
  CW_PROP_FIELD(collider, extents, CW_T_VEC3, CW_PROP_DEFAULT, "BOX half sizes; PLANE x/z half sizes (0 = infinite)."),
  CW_PROP_FIELD_RANGE(collider, radius, CW_T_FLOAT, 0, 1e6, "SPHERE radius."),
  CW_PROP_END,
};
static cw_node_type coll_type = {
  .name = "ParticleCollider3D", .parent = "Node3D", .data_size = sizeof(collider), .props = coll_props,
  .doc = "An analytic shape the tree's GPU particles bounce off; up to 8 act at once.", .init = coll_init,
  .enter_tree = coll_enter, .exit_tree = coll_exit,
};

void cw__register_particles3d_types(void) {
  cw_node_type_register(&p3d_type);
  p3d_t = &p3d_type;
  cw_node_type_register(&attr_type);
  cw_node_type_register(&coll_type);
}
