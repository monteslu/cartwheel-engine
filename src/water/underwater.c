/* Water3D's full-screen pass (POST, before distortion): the caustics on what
 * lies under the water, and the view from under the water.
 *
 * Per pixel, from the scene depth: the world point, and the water surface
 * over it (the Gerstner sum of the drawn waves, the rest point found by a few
 * fixed-point steps as cw_water_height_at does). A point under the surface
 * gets the caustic web (caustics.c), scrolled two ways and keeping the
 * smaller, shifted along the sun's path through the water and fading with
 * depth. A pixel whose point on the camera's near plane is under the surface
 * is seen from under the water: the light it sends is dimmed per colour (red
 * first) over the distance and replaced by the water's own lit colour, and a
 * thin dark line marks where the surface crosses the lens.
 *
 * The first Water3D whose area holds the camera does this (or the first one).
 * Render only: nothing here feeds the simulation. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/environment.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/quality.h"
#include "cartwheel/water.h"
#include "water_internal.h"
#define S(...) #__VA_ARGS__ "\n"
#include "../render/pipeline_post_wgsl.h"

/* ── GLSL ─────────────────────────────────────────────────────────────── */

static const char UW_GLSL[] =
    "uniform sampler2D u_src, u_depth, u_caustic;\n"
    "uniform mat4 u_inv_view_proj, u_to_local;\n"
    "uniform vec3 u_cam;\n"
    "uniform vec4 u_w0, u_w1, u_w2, u_w3, u_steep, u_phase;\n"
    "uniform vec4 u_level;   /* rest level (world y), half extent x, z, near-plane metres per pixel */\n"
    "uniform vec4 u_deep;    /* rgb deep colour, w: 1 / clarity */\n"
    "uniform vec4 u_shallow; /* rgb shallow colour, w: time (s) */\n"
    "uniform vec4 u_sun;     /* toward the sun, w: caustic strength */\n"
    "uniform vec4 u_sun_col; /* rgb sun light, w: 1 / caustic tile size (m) */\n"
    "uniform vec4 u_on;      /* x: caustics, y: the view may be under the water */\n"
    "out vec4 o;\n"
    "vec4 W(int i) { return i == 0 ? u_w0 : i == 1 ? u_w1 : i == 2 ? u_w2 : u_w3; }\n"
    "float surf(vec2 p) {\n"
    "  vec2 r = p;\n"
    "  for (int it = 0; it < 3; it++) {\n"
    "    vec2 s = vec2(0.0);\n"
    "    for (int i = 0; i < 4; i++) { vec4 w = W(i); float th = w.z * dot(w.xy, r) - u_phase[i]; s += u_steep[i] * w.w * w.xy * cos(th); }\n"
    "    r = p - s;\n"
    "  }\n"
    "  float h = 0.0;\n"
    "  for (int i = 0; i < 4; i++) { vec4 w = W(i); h += w.w * sin(w.z * dot(w.xy, r) - u_phase[i]); }\n"
    "  return u_level.x + h;\n"
    "}\n"
    "vec2 tile(vec2 uv) { return texture(u_caustic, fract(uv)).rg * 4.0; }\n"
    "float caustic(vec3 p, float below, vec3 ts) {\n"
    "  vec2 q = (p.xz + ts.xz / max(ts.y, 0.3) * below * 0.75) * u_sun_col.w;\n"
    "  float t = u_shallow.w;\n"
    "  vec2 a = tile(q + vec2(0.013, 0.007) * t), b = tile(q * 1.31 + vec2(0.41, 0.17) - vec2(0.006, 0.011) * t);\n"
    "  float soft = max(smoothstep(0.5, 4.0, below), smoothstep(10.0, 35.0, length(p - u_cam)));\n"
    "  return mix(min(a.x, b.x), min(a.y, b.y), soft);\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = texture(u_src, v_uv);\n"
    "  float d = texture(u_depth, v_uv).r;\n"
    "  vec4 a = u_inv_view_proj * vec4(v_uv * 2.0 - 1.0, -1.0, 1.0);\n"
    "  vec4 b = u_inv_view_proj * vec4(v_uv * 2.0 - 1.0, min(d, 0.999999) * 2.0 - 1.0, 1.0);\n"
    "  vec3 pn = a.xyz / a.w, p = b.xyz / b.w;\n"
    "  vec3 nrm = normalize(cross(dFdx(p), dFdy(p)));\n"
    "  float up = abs(nrm.y) > 0.0 ? smoothstep(0.1, 0.6, abs(nrm.y)) * step(0.0, dot(nrm, u_cam - p) * nrm.y) : 0.0;\n"
    "  vec2 ln = (u_to_local * vec4(pn, 1.0)).xz, lp = (u_to_local * vec4(p, 1.0)).xz;\n"
    "  bool inN = abs(ln.x) < u_level.y && abs(ln.y) < u_level.z;\n"
    "  bool inP = abs(lp.x) < u_level.y && abs(lp.y) < u_level.z;\n"
    "  vec3 col = c.rgb;\n"
    "  vec3 ts = normalize(u_sun.xyz);\n"
    "  if (u_on.x > 0.5 && d < 1.0 && inP) {\n"
    "    float below = surf(lp) - p.y;\n"
    "    if (below > 0.02) {\n"
    "      float k = caustic(p, below, ts);\n"
    "      float fade = smoothstep(0.02, 0.5, below) * exp(-below * u_deep.w * 1.6) * smoothstep(0.0, 0.25, ts.y) * up;\n"
    "      col *= 1.0 + u_sun.w * fade * (k * 2.2 - 1.0);\n"
    "    }\n"
    "  }\n"
    "  if (u_on.y > 0.5 && inN) {\n"
    "    float sn = surf(ln);\n"
    "    float under = sn - pn.y;\n"
    "    if (under > 0.0) {\n"
    "      float dist = d < 1.0 ? length(p - u_cam) : 1e4;\n"
    "      vec3 sig = u_deep.w * vec3(1.6, 0.85, 0.7);\n"
    "      vec3 tr = exp(-sig * dist);\n"
    "      float depthCam = max(sn - u_cam.y, 0.0);\n"
    "      vec3 lit = (u_sun_col.rgb * max(ts.y, 0.0) * 0.3 + 0.12) * exp(-depthCam * u_deep.w * vec3(1.2, 0.6, 0.45));\n"
    "      vec3 water = mix(u_shallow.rgb, u_deep.rgb, 0.35) * lit;\n"
    "      col = col * tr + water * (1.0 - tr);\n"
    "      /* light shafts: the soft caustic web met along the view ray, under the rest level, fading with depth and distance */\n"
    "      if (u_on.x > 0.5) {\n"
    "        vec3 dir = normalize(p - u_cam);\n"
    "        float L = min(dist, 24.0), sh = 0.0;\n"
    "        float j = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "        for (int i = 0; i < 10; i++) {\n"
    "          float t = (float(i) + j) / 10.0 * L;\n"
    "          vec3 q = u_cam + dir * t;\n"
    "          float dep = u_level.x - q.y;\n"
    "          if (dep > 0.0) {\n"
    "            vec2 g = (q.xz + ts.xz / max(ts.y, 0.3) * dep) * u_sun_col.w * 0.5;\n"
    "            vec2 e = tile(g + vec2(0.007, 0.004) * u_shallow.w);\n"
    "            sh += max(e.y - 0.8, 0.0) * exp(-dep * u_deep.w * 0.8) * exp(-t * u_deep.w * 1.2);\n"
    "          }\n"
    "        }\n"
    "        col += water * u_sun.w * sh * L / 10.0 * 0.35 * smoothstep(0.0, 0.25, ts.y);\n"
    "      }\n"
    "    }\n"
    "    float px = abs(under) / u_level.w;\n"
    "    col *= 1.0 - 0.55 * (1.0 - smoothstep(0.0, 2.5, px));\n"
    "  }\n"
    "  o = vec4(col, c.a);\n"
    "}\n";

/* ── WGSL (the same, line for line) ───────────────────────────────────── */

static const char UW_WGSL[] =
    "struct U { u_inv_view_proj: mat4x4f, u_to_local: mat4x4f, u_cam: vec3f, u_pz: vec4f, u_w0: vec4f, u_w1: vec4f, u_w2: vec4f, u_w3: vec4f,\n"
    "  u_steep: vec4f, u_phase: vec4f, u_level: vec4f, u_deep: vec4f, u_shallow: vec4f, u_sun: vec4f, u_sun_col: vec4f, u_on: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
    "@group(0) @binding(2) var u_depth: texture_2d<f32>;\n"
    "@group(0) @binding(3) var u_caustic: texture_2d<f32>;\n"
    "fn W(i: i32) -> vec4f { if (i == 0) { return u.u_w0; } if (i == 1) { return u.u_w1; } if (i == 2) { return u.u_w2; } return u.u_w3; }\n"
    "fn surf(p: vec2f) -> f32 {\n"
    "  var r = p;\n"
    "  for (var it = 0; it < 3; it++) {\n"
    "    var s = vec2f(0.0);\n"
    "    for (var i = 0; i < 4; i++) { let w = W(i); let th = w.z * dot(w.xy, r) - u.u_phase[i]; s += u.u_steep[i] * w.w * w.xy * cos(th); }\n"
    "    r = p - s;\n"
    "  }\n"
    "  var h = 0.0;\n"
    "  for (var i = 0; i < 4; i++) { let w = W(i); h += w.w * sin(w.z * dot(w.xy, r) - u.u_phase[i]); }\n"
    "  return u.u_level.x + h;\n"
    "}\n"
    "fn tile(uv: vec2f) -> vec2f { return textureSampleLevel(u_caustic, cw_linear, fract(uv), 0.0).rg * 4.0; }\n"
    "fn caustic(p: vec3f, below: f32, ts: vec3f) -> f32 {\n"
    "  let q = (p.xz + ts.xz / max(ts.y, 0.3) * below * 0.75) * u.u_sun_col.w;\n"
    "  let t = u.u_shallow.w;\n"
    "  let a = tile(q + vec2f(0.013, 0.007) * t);\n"
    "  let b = tile(q * 1.31 + vec2f(0.41, 0.17) - vec2f(0.006, 0.011) * t);\n"
    "  let soft = max(smoothstep(0.5, 4.0, below), smoothstep(10.0, 35.0, length(p - u.u_cam)));\n"
    "  return mix(min(a.x, b.x), min(a.y, b.y), soft);\n"
    "}\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let c = textureSampleLevel(u_src, cw_linear, uv, 0.0);\n"
    "  let d = win_depth(cw_load(u_depth, uv).r, u.u_pz);\n"
    "  let a = u.u_inv_view_proj * vec4f(uv * 2.0 - 1.0, -1.0, 1.0);\n"
    "  let b = u.u_inv_view_proj * vec4f(uv * 2.0 - 1.0, min(d, 0.999999) * 2.0 - 1.0, 1.0);\n"
    "  let pn = a.xyz / a.w;\n"
    "  let p = b.xyz / b.w;\n"
    "  let nrm = normalize(cross(dpdx(p), dpdy(p)));\n"
    "  let up = smoothstep(0.1, 0.6, abs(nrm.y)) * step(0.0, dot(nrm, u.u_cam - p) * nrm.y);\n"
    "  let ln = (u.u_to_local * vec4f(pn, 1.0)).xz;\n"
    "  let lp = (u.u_to_local * vec4f(p, 1.0)).xz;\n"
    "  let inN = abs(ln.x) < u.u_level.y && abs(ln.y) < u.u_level.z;\n"
    "  let inP = abs(lp.x) < u.u_level.y && abs(lp.y) < u.u_level.z;\n"
    "  var col = c.rgb;\n"
    "  let ts = normalize(u.u_sun.xyz);\n"
    "  if (u.u_on.x > 0.5 && d < 1.0 && inP) {\n"
    "    let below = surf(lp) - p.y;\n"
    "    if (below > 0.02) {\n"
    "      let k = caustic(p, below, ts);\n"
    "      let fade = smoothstep(0.02, 0.5, below) * exp(-below * u.u_deep.w * 1.6) * smoothstep(0.0, 0.25, ts.y) * up;\n"
    "      col = col * (1.0 + u.u_sun.w * fade * (k * 2.2 - 1.0));\n"
    "    }\n"
    "  }\n"
    "  if (u.u_on.y > 0.5 && inN) {\n"
    "    let sn = surf(ln);\n"
    "    let under = sn - pn.y;\n"
    "    if (under > 0.0) {\n"
    "      let dist = select(1e4, length(p - u.u_cam), d < 1.0);\n"
    "      let sig = u.u_deep.w * vec3f(1.6, 0.85, 0.7);\n"
    "      let tr = exp(-sig * dist);\n"
    "      let depthCam = max(sn - u.u_cam.y, 0.0);\n"
    "      let lit = (u.u_sun_col.rgb * max(ts.y, 0.0) * 0.3 + 0.12) * exp(-depthCam * u.u_deep.w * vec3f(1.2, 0.6, 0.45));\n"
    "      let water = mix(u.u_shallow.rgb, u.u_deep.rgb, 0.35) * lit;\n"
    "      col = col * tr + water * (1.0 - tr);\n"
    "      if (u.u_on.x > 0.5) {\n"
    "        let dir = normalize(p - u.u_cam);\n"
    "        let L = min(dist, 24.0);\n"
    "        var sh = 0.0;\n"
    "        let fc = cw_frag_coord(pos);\n"
    "        let j = fract(52.9829189 * fract(dot(fc, vec2f(0.06711056, 0.00583715))));\n"
    "        for (var i = 0; i < 10; i++) {\n"
    "          let t = (f32(i) + j) / 10.0 * L;\n"
    "          let q = u.u_cam + dir * t;\n"
    "          let dep = u.u_level.x - q.y;\n"
    "          if (dep > 0.0) {\n"
    "            let g = (q.xz + ts.xz / max(ts.y, 0.3) * dep) * u.u_sun_col.w * 0.5;\n"
    "            let e = tile(g + vec2f(0.007, 0.004) * u.u_shallow.w);\n"
    "            sh += max(e.y - 0.8, 0.0) * exp(-dep * u.u_deep.w * 0.8) * exp(-t * u.u_deep.w * 1.2);\n"
    "          }\n"
    "        }\n"
    "        col = col + water * u.u_sun.w * sh * L / 10.0 * 0.35 * smoothstep(0.0, 0.25, ts.y);\n"
    "      }\n"
    "    }\n"
    "    let px = abs(under) / u.u_level.w;\n"
    "    col = col * (1.0 - 0.55 * (1.0 - smoothstep(0.0, 2.5, px)));\n"
    "  }\n"
    "  return vec4f(col, c.a);\n"
    "}\n";

static bool wg(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }

/* the water this view's pass draws: it runs only while the camera may be under that water (inside its area, lower
 * than the highest crest); caustics at Medium and up when caustics > 0 */
static bool pick(const cw_pass_ctx *ctx, cw_node *out, water3d **dout, bool *caus, bool *under) {
  cw_node pick = { 0 };
  water3d *pd = NULL;
  bool pc = false, pu = false;
  if (!ctx->camera) return false;
  const float *cm = ctx->camera->base.matrix_world.e;
  t3_vec3 cam = { cm[12], cm[13], cm[14] };
  int q = cw__water_quality();
  for (int i = 0;; i++) {
    cw_node n = cw__water_at(ctx->tree, i);
    if (!cw_node_valid(n)) break;
    water3d *d = cw__water_data(n);
    if (!d || !d->mesh) continue;
    const t3_mat4 *m = cw_node3d_global_transform(n);
    t3_mat4 inv;
    t3_mat4_invert(&inv, m);
    t3_vec3 l = t3_vec3_apply_mat4(cam, &inv);
    float hx = d->size[0] * 0.5f, hz = d->size[1] * 0.5f;
    if (d->horizon_distance > hx) hx = d->horizon_distance;
    if (d->horizon_distance > hz) hz = d->horizon_distance;
    bool inside = fabsf(l.x) < hx && fabsf(l.z) < hz;
    float amp = 0;
    for (int k = 0; k < 4; k++) amp += fabsf(d->ev.a[k]);
    bool u = inside && d->underwater && l.y < amp + 0.05f;
    bool c = d->caustics > 0 && q >= CW_QUALITY_MEDIUM;
    if (!cw_node_valid(pick) || (inside && !pu)) {
      pick = n, pd = d, pc = c, pu = u;
      if (inside) break;
    }
  }
  /* from above, the opaque surface hides everything under it (the surface shader draws the caustics it shows) */
  if (!pd || !pu) return false;
  *out = pick, *dout = pd, *caus = pc, *under = pu;
  return true;
}

static bool uw_active(const cw_pass_ctx *ctx, void *user) {
  (void)user;
  cw_node n;
  water3d *d;
  bool c, u;
  return pick(ctx, &n, &d, &c, &u);
}

static void uw_exec(cw_pass_ctx *ctx, void *user) {
  (void)user;
  static unsigned prog, tex;
  cw_node n;
  water3d *d;
  bool caus, under;
  if (!pick(ctx, &n, &d, &caus, &under) || !ctx->depth_tex) return;
  if (!prog) {
    if (wg()) {
      static char w[sizeof UW_WGSL + 2048];
      snprintf(w, sizeof w, "%s%s", WGSL_COMMON, UW_WGSL);
      prog = cw_pass_program_wgsl("water_under", w);
    } else {
      prog = cw_pass_program("water_under", UW_GLSL);
    }
  }
  if (!tex) {
    const int nn = CW_WATER_CAUSTIC_N * CW_WATER_CAUSTIC_N;
    const uint8_t *c = cw__water_caustics(), *soft = cw__water_caustics_soft();
    uint8_t *px = cw_alloc((size_t)nn * 4, CW_MEM_RENDER);
    for (int k = 0; k < nn; k++) px[k * 4] = c[k], px[k * 4 + 1] = soft[k], px[k * 4 + 2] = 0, px[k * 4 + 3] = 255;
    tex = cw_pass_texture_new(CW_WATER_CAUSTIC_N, CW_WATER_CAUSTIC_N, CW_RT_RGBA8, px);
    cw_free(px);
  }
  if (!prog || !tex) return;
  const t3_mat4 *m = cw_node3d_global_transform(n);
  t3_mat4 inv;
  t3_mat4_invert(&inv, m);
  float hx = d->size[0] * 0.5f, hz = d->size[1] * 0.5f;
  if (d->horizon_distance > hx) hx = d->horizon_distance;
  if (d->horizon_distance > hz) hz = d->horizon_distance;
  /* the near plane's height per pixel (the waterline's width) */
  t3_camera *cam = ctx->camera;
  float near = cam->near > 0 ? cam->near : 0.1f;
  float px = 2.0f * near / (ctx->proj.e[5] != 0 ? ctx->proj.e[5] : 1.0f) / (float)(ctx->height > 0 ? ctx->height : 1);
  cw_pass_use(prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, ctx->depth_tex);
  cw_pass_bind_texture(2, tex);
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_uniform1i(prog, "u_depth", 1);
  cw_pass_uniform1i(prog, "u_caustic", 2);
  cw_pass_uniform_mat4(prog, "u_inv_view_proj", &ctx->inv_view_proj);
  cw_pass_uniform_mat4(prog, "u_to_local", &inv);
  const float *e = ctx->proj.e;
  cw_pass_uniform4f(prog, "u_pz", e[10], e[14], e[11], e[15]);
  const float *cm = cam->base.matrix_world.e;
  cw_pass_uniform3f(prog, "u_cam", cm[12], cm[13], cm[14]);
  static const char *const wn[4] = { "u_w0", "u_w1", "u_w2", "u_w3" };
  for (int i = 0; i < 4; i++) cw_pass_uniform4f(prog, wn[i], d->ev.dx[i], d->ev.dz[i], d->ev.k[i], d->ev.a[i]);
  cw_pass_uniform4f(prog, "u_steep", d->ev.q[0], d->ev.q[1], d->ev.q[2], d->ev.q[3]);
  cw_pass_uniform4f(prog, "u_phase", d->ev.ph[0], d->ev.ph[1], d->ev.ph[2], d->ev.ph[3]);
  cw_pass_uniform4f(prog, "u_level", m->e[13], hx, hz, px);
  float ic = 1.0f / (d->clarity > 0.01f ? d->clarity : 0.01f);
  cw_pass_uniform4f(prog, "u_deep", d->deep_color[0], d->deep_color[1], d->deep_color[2], ic);
  cw_pass_uniform4f(prog, "u_shallow", d->shallow_color[0], d->shallow_color[1], d->shallow_color[2], d->foam_time);
  float sx = d->sun[0], sy = d->sun[1], sz = d->sun[2];
  if (sx == 0 && sy == 0 && sz == 0) sy = 1;
  float cs = d->caustic_size > 0.05f ? d->caustic_size : 0.05f;
  cw_pass_uniform4f(prog, "u_sun", sx, sy, sz, d->caustics);
  /* without a sky the scene's light is unknown: a plain white sun for the water's colour */
  float sr = d->sun_col[0], sg = d->sun_col[1], sb = d->sun_col[2];
  if (sr + sg + sb <= 0) sr = sg = sb = 3.0f;
  cw_pass_uniform4f(prog, "u_sun_col", sr, sg, sb, 1.0f / cs);
  cw_pass_uniform4f(prog, "u_on", caus ? 1.0f : 0.0f, under ? 1.0f : 0.0f, 0, 0);
  cw_pass_fullscreen();
  d->under_drawn = under;
}

void cw__water_add_under_pass(cw_tree *t) {
  cw_pipeline *p = cw_pipeline_of(t);
  if (!cw_pipeline_has_pass(p, "water_under"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "water_under", .phase = CW_PHASE_POST, .order = 140,
                                             .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                             .active = uw_active, .execute = uw_exec });
}
