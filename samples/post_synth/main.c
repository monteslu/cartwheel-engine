/* post_synth: the render pipeline's post stack on a scene the app draws
 * itself, the same on GL and WebGPU.
 *
 * three.c has no WebGPU backend yet, so the built-in "scene" pass is turned
 * off and an OPAQUE pass of this app ("synth_scene", webgpu = true) ray
 * traces a small analytic scene in one full-screen draw: a checkered ground,
 * a back wall, three spheres (one emissive at HDR intensities for bloom), a
 * box for occlusion corners, a sun with hard shadows, a sky. It writes
 * linear HDR colour and GL-convention window depth (gl_FragDepth /
 * @builtin(frag_depth)), so every depth reader (GTAO, fog, velocity, the
 * debug views, TAA's reprojection) runs on real geometry. The camera orbits
 * every frame, so TAA reprojects real motion.
 *
 * The run is a schedule like post_stack's: each segment sets the PostProcess
 * node's properties (and the HeightFog's density) for a few frames; each
 * gate captures the end of one segment.
 *
 * Debug state (identical on every host and backend): post.seg, post.passes
 * (bit i = pass_names[i] ran last frame), pipe.float, cam.x / cam.z (camera
 * position x 1000, rounded), and gpu.<pass> / cpu.<pass> (ms, averaged;
 * reported, never compared). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

#ifndef POST_PERF
#define POST_PERF 0
#endif

/* blades: how many blades stand around the scene (0: the usual eight) */
typedef struct seg { const char *name; int frames; const char *props; float fog; int blades; } seg;
static const seg segs[] = {
  { "full", 24, "taa=true;bloom=true;bloom_intensity=0.15;bloom_threshold=1;ssao=true;ssao_radius=0.7;grade=true;"
                "grade_intensity=0.8;vignette=true;chromatic_aberration=true;ca_intensity=0.003;grain=true;grain_intensity=0.03",
    0.04f },
  { "base", 4, "", 0 },
  { "neutral", 4, "tonemap=NEUTRAL", 0 },
  { "aces", 4, "tonemap=ACES_FILMIC", 0 },
  { "reinhard", 4, "tonemap=REINHARD;exposure_ev=1", 0 },
  { "cineon", 4, "tonemap=CINEON", 0 },
  { "linear", 4, "tonemap=LINEAR;exposure=0.7", 0 },
  { "grade", 4, "grade=true", 0 },
  { "bloom", 4, "bloom=true;bloom_intensity=0.4;bloom_threshold=1", 0 },
  { "fxaa", 4, "aa=FXAA", 0 },
  { "smaa", 4, "aa=SMAA", 0 },
  { "lens", 4, "vignette=true;vignette_intensity=0.7;chromatic_aberration=true;ca_intensity=0.012;grain=true;grain_intensity=0.4", 0 },
  { "ssao", 4, "ssao=true;ssao_radius=0.8;ssao_intensity=1", 0 },
  { "taa", 24, "taa=true", 0 },
  /* a ring of 48 blades: MSAA only resolves triangle edges, and at the 1080p
   * output eight blades' edges were 0.15% of the frame, under the gate's
   * 0.2% bound, so the msaa=0 control could not fail */
  { "msaa", 4, "tonemap=NONE;hdr=false;msaa=4", 0, 48 },
  { "fog", 4, "", 0.12f },
  { "view_depth", 3, "debug_view=DEPTH", 0 },
  { "view_normals", 3, "debug_view=NORMALS", 0 },
  { "view_velocity", 3, "debug_view=VELOCITY", 0 },
  { "view_ao", 3, "ssao=true;ssao_radius=0.8;debug_view=AO", 0 },
  { "view_bloom", 3, "bloom=true;bloom_threshold=1;debug_view=BLOOM", 0 },
  { "ldr_post", 4, "tonemap=NONE;hdr=false;bloom=true;bloom_intensity=0.3;aa=FXAA", 0 },
};
#define NSEG ((int)(sizeof segs / sizeof segs[0]))

static const char *const pass_names[] = { "synth_scene", "velocity", "ssao", "taa", "bloom", "tonemap", "grade", "aa",
                                          "lens", "debug_view", "height_fog", "app" };
#define NPASS ((int)(sizeof pass_names / sizeof pass_names[0]))
static int32_t seg_index = -1, passes_mask, float_ok, cam_x, cam_z;
static uint32_t frame_no;
static float gpu_ms[NPASS], cpu_ms[NPASS];

/* ── the scene (GLSL and WGSL, the same math) ─────────────────────────── */

#define SCENE_COMMON_GLSL \
  "uniform mat4 u_ivp;\n" \
  "uniform mat4 u_vp;\n" \
  "out vec4 o;\n" \
  "const vec3 SUN = vec3(0.4977, 0.7963, 0.3484);\n" \
  "float sph(vec3 ro, vec3 rd, vec4 s) {\n" \
  "  vec3 oc = ro - s.xyz; float b = dot(oc, rd); float c = dot(oc, oc) - s.w * s.w; float h = b * b - c;\n" \
  "  if (h < 0.0) return -1.0;\n" \
  "  h = sqrt(h); float t = -b - h; if (t < 0.0) t = -b + h; return t;\n" \
  "}\n" \
  "float box(vec3 ro, vec3 rd, vec3 lo, vec3 hi, out vec3 n) {\n" \
  "  vec3 inv = 1.0 / rd; vec3 t0 = (lo - ro) * inv, t1 = (hi - ro) * inv;\n" \
  "  vec3 tn3 = min(t0, t1), tf3 = max(t0, t1);\n" \
  "  float tn = max(max(tn3.x, tn3.y), tn3.z), tf = min(min(tf3.x, tf3.y), tf3.z);\n" \
  "  n = vec3(0.0);\n" \
  "  if (tn > tf || tf < 0.0 || tn < 0.0) return -1.0;\n" \
  "  if (tn == tn3.x) n = vec3(-sign(rd.x), 0.0, 0.0); else if (tn == tn3.y) n = vec3(0.0, -sign(rd.y), 0.0); else n = vec3(0.0, 0.0, -sign(rd.z));\n" \
  "  return tn;\n" \
  "}\n" \
  /* nearest hit: t (or 1e9), normal, albedo, emission */ \
  "float trace(vec3 ro, vec3 rd, out vec3 n, out vec3 alb, out vec3 emi) {\n" \
  "  float best = 1e9; n = vec3(0.0, 1.0, 0.0); alb = vec3(0.0); emi = vec3(0.0);\n" \
  "  if (rd.y < 0.0) { float t = -ro.y / rd.y; if (t > 0.0 && t < best) { best = t; vec3 p = ro + rd * t; n = vec3(0.0, 1.0, 0.0);\n" \
  "    float k = mod(floor(p.x * 1.5) + floor(p.z * 1.5), 2.0); alb = k < 0.5 ? vec3(0.75, 0.7, 0.62) : vec3(0.2, 0.22, 0.25); } }\n" \
  "  if (rd.z < 0.0) { float t = (-4.0 - ro.z) / rd.z; if (t > 0.0 && t < best && ro.y + rd.y * t < 3.0) { best = t; n = vec3(0.0, 0.0, 1.0); alb = vec3(0.55, 0.6, 0.7); } }\n" \
  "  vec4 S[3] = vec4[3](vec4(0.0, 1.0, 0.0, 1.0), vec4(-2.2, 0.6, 1.2, 0.6), vec4(2.0, 0.5, -1.5, 0.5));\n" \
  "  vec3 A[3] = vec3[3](vec3(0.8), vec3(0.8, 0.15, 0.1), vec3(0.0));\n" \
  "  for (int i = 0; i < 3; i++) { float t = sph(ro, rd, S[i]); if (t > 0.0 && t < best) { best = t; n = normalize(ro + rd * t - S[i].xyz); alb = A[i];\n" \
  "    emi = i == 2 ? vec3(12.0, 8.0, 3.0) : vec3(0.0); } }\n" \
  "  vec3 bn; float tb = box(ro, rd, vec3(0.8, 0.0, 1.6), vec3(1.8, 1.2, 2.6), bn);\n" \
  "  if (tb > 0.0 && tb < best) { best = tb; n = bn; alb = vec3(0.3, 0.5, 0.8); emi = vec3(0.0); }\n" \
  "  return best;\n" \
  "}\n"

static const char *SCENE_GLSL = SCENE_COMMON_GLSL
    "void main() {\n"
    "  vec2 ndc = v_uv * 2.0 - 1.0;\n"
    "  vec4 a = u_ivp * vec4(ndc, -1.0, 1.0), b = u_ivp * vec4(ndc, 1.0, 1.0);\n"
    "  vec3 ro = a.xyz / a.w, rd = normalize(b.xyz / b.w - ro);\n"
    "  vec3 n, alb, emi;\n"
    "  float t = trace(ro, rd, n, alb, emi);\n"
    "  if (t > 60.0) {\n"
    "    o = vec4(mix(vec3(0.7, 0.75, 0.85), vec3(0.25, 0.4, 0.8), clamp(rd.y * 1.5, 0.0, 1.0)) * 1.2, 1.0);\n"
    "    gl_FragDepth = 1.0;\n"
    "    return;\n"
    "  }\n"
    "  vec3 p = ro + rd * t;\n"
    "  vec3 sn, sa, se;\n"
    "  float vis = trace(p + n * 0.002, SUN, sn, sa, se) < 1e8 ? 0.0 : 1.0;\n"
    "  vec3 amb = mix(vec3(0.2, 0.17, 0.15), vec3(0.35, 0.45, 0.6), n.y * 0.5 + 0.5) * 0.6;\n"
    "  vec3 c = alb * (vec3(3.0, 2.8, 2.5) * max(dot(n, SUN), 0.0) * vis + amb) + emi;\n"
    "  vec4 clip = u_vp * vec4(p, 1.0);\n"
    "  gl_FragDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";

static const char *SCENE_WGSL =
    "struct U { u_ivp: mat4x4f, u_vp: mat4x4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "const SUN = vec3f(0.4977, 0.7963, 0.3484);\n"
    "fn sph(ro: vec3f, rd: vec3f, s: vec4f) -> f32 {\n"
    "  let oc = ro - s.xyz; let b = dot(oc, rd); let c = dot(oc, oc) - s.w * s.w; var h = b * b - c;\n"
    "  if (h < 0.0) { return -1.0; }\n"
    "  h = sqrt(h); var t = -b - h; if (t < 0.0) { t = -b + h; } return t;\n"
    "}\n"
    "struct Box { t: f32, n: vec3f };\n"
    "fn box(ro: vec3f, rd: vec3f, lo: vec3f, hi: vec3f) -> Box {\n"
    "  let inv = 1.0 / rd; let t0 = (lo - ro) * inv; let t1 = (hi - ro) * inv;\n"
    "  let tn3 = min(t0, t1); let tf3 = max(t0, t1);\n"
    "  let tn = max(max(tn3.x, tn3.y), tn3.z); let tf = min(min(tf3.x, tf3.y), tf3.z);\n"
    "  var r: Box; r.n = vec3f(0.0); r.t = -1.0;\n"
    "  if (tn > tf || tf < 0.0 || tn < 0.0) { return r; }\n"
    "  if (tn == tn3.x) { r.n = vec3f(-sign(rd.x), 0.0, 0.0); } else if (tn == tn3.y) { r.n = vec3f(0.0, -sign(rd.y), 0.0); } else { r.n = vec3f(0.0, 0.0, -sign(rd.z)); }\n"
    "  r.t = tn; return r;\n"
    "}\n"
    "struct Hit { t: f32, n: vec3f, alb: vec3f, emi: vec3f };\n"
    "fn gmod(x: f32, y: f32) -> f32 { return x - y * floor(x / y); }\n"
    "fn trace(ro: vec3f, rd: vec3f) -> Hit {\n"
    "  var h: Hit; h.t = 1e9; h.n = vec3f(0.0, 1.0, 0.0); h.alb = vec3f(0.0); h.emi = vec3f(0.0);\n"
    "  if (rd.y < 0.0) { let t = -ro.y / rd.y; if (t > 0.0 && t < h.t) { h.t = t; let p = ro + rd * t; h.n = vec3f(0.0, 1.0, 0.0);\n"
    "    let k = gmod(floor(p.x * 1.5) + floor(p.z * 1.5), 2.0); h.alb = select(vec3f(0.2, 0.22, 0.25), vec3f(0.75, 0.7, 0.62), k < 0.5); } }\n"
    "  if (rd.z < 0.0) { let t = (-4.0 - ro.z) / rd.z; if (t > 0.0 && t < h.t && ro.y + rd.y * t < 3.0) { h.t = t; h.n = vec3f(0.0, 0.0, 1.0); h.alb = vec3f(0.55, 0.6, 0.7); } }\n"
    "  var S = array<vec4f, 3>(vec4f(0.0, 1.0, 0.0, 1.0), vec4f(-2.2, 0.6, 1.2, 0.6), vec4f(2.0, 0.5, -1.5, 0.5));\n"
    "  var A = array<vec3f, 3>(vec3f(0.8), vec3f(0.8, 0.15, 0.1), vec3f(0.0));\n"
    "  for (var i = 0; i < 3; i++) { let t = sph(ro, rd, S[i]); if (t > 0.0 && t < h.t) { h.t = t; h.n = normalize(ro + rd * t - S[i].xyz); h.alb = A[i];\n"
    "    h.emi = select(vec3f(0.0), vec3f(12.0, 8.0, 3.0), i == 2); } }\n"
    "  let bx = box(ro, rd, vec3f(0.8, 0.0, 1.6), vec3f(1.8, 1.2, 2.6));\n"
    "  if (bx.t > 0.0 && bx.t < h.t) { h.t = bx.t; h.n = bx.n; h.alb = vec3f(0.3, 0.5, 0.8); h.emi = vec3f(0.0); }\n"
    "  return h;\n"
    "}\n"
    "struct O { @location(0) c: vec4f, @builtin(frag_depth) d: f32 };\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> O {\n"
    "  let ndc = uv * 2.0 - 1.0;\n"
    "  let a = u.u_ivp * vec4f(ndc, -1.0, 1.0); let b = u.u_ivp * vec4f(ndc, 1.0, 1.0);\n"
    "  let ro = a.xyz / a.w; let rd = normalize(b.xyz / b.w - ro);\n"
    "  let h = trace(ro, rd);\n"
    "  var o: O;\n"
    "  if (h.t > 60.0) {\n"
    "    o.c = vec4f(mix(vec3f(0.7, 0.75, 0.85), vec3f(0.25, 0.4, 0.8), clamp(rd.y * 1.5, 0.0, 1.0)) * 1.2, 1.0);\n"
    "    o.d = 1.0;\n"
    "    return o;\n"
    "  }\n"
    "  let p = ro + rd * h.t;\n"
    "  let sh = trace(p + h.n * 0.002, SUN);\n"
    "  let vis = select(1.0, 0.0, sh.t < 1e8);\n"
    "  let amb = mix(vec3f(0.2, 0.17, 0.15), vec3f(0.35, 0.45, 0.6), h.n.y * 0.5 + 0.5) * 0.6;\n"
    "  let c = h.alb * (vec3f(3.0, 2.8, 2.5) * max(dot(h.n, SUN), 0.0) * vis + amb) + h.emi;\n"
    "  let clip = u.u_vp * vec4f(p, 1.0);\n"
    "  o.d = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);\n"
    "  o.c = vec4f(c, 1.0);\n"
    "  return o;\n"
    "}\n";

/* thin leaning blades around the scene (eight, or the segment's count): real triangle edges (the
 * ray traced backdrop has none), so MSAA has something to resolve. Drawn
 * with cw_pass_draw from the vertex index, depth tested against the
 * backdrop's written depth. */
static const char *BLADES_VS =
    "uniform mat4 u_vp;\n"
    "uniform float u_n;\n"
    "out float v_k;\n"
    "out float v_h;\n"
    "void main() {\n"
    "  int k = gl_VertexID / 6, c = gl_VertexID - k * 6;\n"
    "  vec2 q = c == 1 ? vec2(1.0, 0.0) : c == 2 || c == 4 ? vec2(1.0, 1.0) : c == 5 ? vec2(0.0, 1.0) : vec2(0.0);\n"
    "  float a = float(k) * 6.28319 / u_n + 0.3;\n"
    "  vec3 dir = vec3(cos(a), 0.0, sin(a)), tang = vec3(-dir.z, 0.0, dir.x);\n"
    "  vec3 p = dir * 3.2 + tang * (q.x - 0.5) * 0.18 + vec3(0.0, q.y * 1.7, 0.0) - dir * q.y * 0.5;\n"
    "  gl_Position = u_vp * vec4(p, 1.0);\n"
    "  v_k = float(k);\n"
    "  v_h = q.y;\n"
    "}\n";
static const char *BLADES_FS =
    "in float v_k;\n"
    "in float v_h;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  float k = floor(v_k + 0.5);\n"
    "  vec3 c = (0.5 + 0.5 * cos(k * 0.9 + vec3(0.0, 2.1, 4.2))) * 2.0;\n"
    "  o = vec4(c * (0.3 + v_h * 1.2), 1.0);\n"
    "}\n";
static const char *BLADES_WGSL =
    "struct U { u_vp: mat4x4f, u_n: f32 };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "struct V { @builtin(position) pos: vec4f, @location(0) k: f32, @location(1) h: f32 };\n"
    "@vertex fn vs(@builtin(vertex_index) vi: u32) -> V {\n"
    "  let i = i32(vi); let k = i / 6; let c = i - k * 6;\n"
    "  var q = vec2f(0.0);\n"
    "  if (c == 1) { q = vec2f(1.0, 0.0); } else if (c == 2 || c == 4) { q = vec2f(1.0, 1.0); } else if (c == 5) { q = vec2f(0.0, 1.0); }\n"
    "  let a = f32(k) * 6.28319 / u.u_n + 0.3;\n"
    "  let dir = vec3f(cos(a), 0.0, sin(a)); let tang = vec3f(-dir.z, 0.0, dir.x);\n"
    "  let p = dir * 3.2 + tang * (q.x - 0.5) * 0.18 + vec3f(0.0, q.y * 1.7, 0.0) - dir * q.y * 0.5;\n"
    "  var cp = u.u_vp * vec4f(p, 1.0);\n"
    /* GL's depth range and row order */
    "  cp.z = (cp.z + cp.w) * 0.5;\n"
    "  cp.y = cp.y * cw_target.flip;\n"
    "  var o: V; o.pos = cp; o.k = f32(k); o.h = q.y; return o;\n"
    "}\n"
    "@fragment fn fs(i: V) -> @location(0) vec4f {\n"
    "  let k = floor(i.k + 0.5);\n"
    "  let c = (0.5 + 0.5 * cos(k * 0.9 + vec3f(0.0, 2.1, 4.2))) * 2.0;\n"
    "  return vec4f(c * (0.3 + i.h * 1.2), 1.0);\n"
    "}\n";

static void synth_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  static unsigned prog, blades;
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  if (!prog) {
    prog = wg ? cw_pass_program_wgsl("synth_scene", SCENE_WGSL) : cw_pass_program("synth_scene", SCENE_GLSL);
    blades = wg ? cw_pass_program_wgsl("synth_blades", BLADES_WGSL) : cw_pass_program_vs("synth_blades", BLADES_VS, BLADES_FS);
  }
  if (!prog || !blades || !ctx->camera) return;
  /* the camera as the pipeline set it up (jittered under TAA) */
  t3_camera *cam = ctx->camera;
  t3_object_update_world_matrix(cam, true, false);
  t3_mat4 view, vp, ivp;
  t3_mat4_invert(&view, &cam->base.matrix_world);
  t3_mat4_multiply(&vp, &cam->projection_matrix, &view);
  t3_mat4_invert(&ivp, &vp);
  cw_pass_clear(0, 0, 0, 1, true);
  cw_pass_use(prog);
  cw_pass_set_depth(false, true);
  cw_pass_uniform_mat4(prog, "u_ivp", &ivp);
  cw_pass_uniform_mat4(prog, "u_vp", &vp);
  cw_pass_fullscreen();
  cw_pass_use(blades);
  cw_pass_set_depth(true, true);
  cw_pass_uniform_mat4(blades, "u_vp", &vp);
  int n = seg_index >= 0 && segs[seg_index].blades ? segs[seg_index].blades : 8;
  cw_pass_uniform1f(blades, "u_n", (float)n);
  cw_pass_draw(n * 6);
  cw_pass_set_depth(false, false);
}

/* ── the schedule ─────────────────────────────────────────────────────── */

static cw_node node(const char *name) {
  char path[64];
  snprintf(path, sizeof path, "Root/%s", name);
  return cw_node_get(cw_tree_root(cw_scene_app_tree()), path);
}

static void apply_props(const char *props, float fog) {
  cw_node n = node("Post");
  static const char *const base =
      "hdr=true;tonemap=AGX;exposure=1;exposure_ev=0;msaa=0;depth_prepass=false;bloom=false;"
      "bloom_intensity=0.05;bloom_threshold=0;ssao=false;ssao_radius=0.5;ssao_intensity=1;taa=false;aa=NONE;grade=false;"
      "grade_intensity=1;vignette=false;vignette_intensity=0.35;grain=false;grain_intensity=0.04;"
      "chromatic_aberration=false;ca_intensity=0.004;debug_view=NONE";
  /* POST_SYNTH_EXTRA: more properties after every segment's (a gate
   * control: "msaa=0" shows the MSAA gate can see MSAA missing) */
  const char *extra = getenv("POST_SYNTH_EXTRA");
  for (int pass = 0; pass < 3; pass++) {
    const char *s = pass == 0 ? base : pass == 1 ? props : extra ? extra : "";
    while (*s) {
      const char *e = strchr(s, ';');
      size_t len = e ? (size_t)(e - s) : strlen(s);
      char kv[128];
      memcpy(kv, s, CW_MIN(len, sizeof kv - 1));
      kv[CW_MIN(len, sizeof kv - 1)] = 0;
      char *eq = strchr(kv, '=');
      if (eq) {
        *eq = 0;
        const cw_prop *pr = cw_node_find_prop(n, kv, NULL);
        cw_value v;
        if (pr && pr->type == CW_T_ENUM) cw_node_set(n, kv, cw_val_enum(eq + 1));
        else if (pr && cw_value_parse(eq + 1, pr->type, &v)) cw_node_set(n, kv, v);
        else cw_error("post_synth: bad property %s=%s", kv, eq + 1);
      }
      s += len + (e ? 1 : 0);
    }
  }
  cw_node_set(node("Fog"), "density", cw_val_float(fog));
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("post.seg", &seg_index);
  cw_debug_i32("post.passes", &passes_mask);
  cw_debug_i32("pipe.float", &float_ok);
  cw_debug_i32("cam.x", &cam_x);
  cw_debug_i32("cam.z", &cam_z);
  static char keys[NPASS][2][32];
  for (int i = 0; i < NPASS; i++) {
    snprintf(keys[i][0], sizeof keys[i][0], "gpu.%s", pass_names[i]);
    snprintf(keys[i][1], sizeof keys[i][1], "cpu.%s", pass_names[i]);
    cw_debug_f32(keys[i][0], &gpu_ms[i]);
    cw_debug_f32(keys[i][1], &cpu_ms[i]);
  }
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  frame_no++;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  if (!cw_pipeline_has_pass(p, "synth_scene")) {
    cw_pipeline_set_pass_enabled(p, "scene", false);
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "synth_scene", .phase = CW_PHASE_OPAQUE, .order = 0,
                                             .writes = CW_RES_COLOR | CW_RES_DEPTH, .execute = synth_exec, .webgpu = true });
  }
  int k = 0;
  if (POST_PERF) k = 0;
  else {
    int start = 0;
    for (k = 0; k < NSEG - 1; k++) {
      if ((int)frame_no <= start + segs[k].frames) break;
      start += segs[k].frames;
    }
  }
  if (k != seg_index) {
    seg_index = k;
    apply_props(segs[k].props, segs[k].fog);
  }
  /* the camera orbits the scene, looking at it (real motion for TAA) */
  float a = (float)frame_no * 0.035f;
  float x = sinf(a) * 4.5f, y = 2.2f + 0.4f * sinf(a * 1.7f), z = 6.0f + cosf(a) * 1.5f;
  float dx = 0 - x, dy = 0.7f - y, dz = 0 - z;
  float yaw = atan2f(-dx, -dz), pitch = atan2f(dy, sqrtf(dx * dx + dz * dz));
  cw_node cam = node("Camera");
  cw_node_set(cam, "position", cw_val_vec3(x, y, z));
  cw_node_set(cam, "rotation", cw_val_vec3(pitch, yaw, 0));
  cam_x = (int32_t)lroundf(x * 1000);
  cam_z = (int32_t)lroundf(z * 1000);
}

/* the app's overlay pass: the output is complete */
static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  float_ok = cw_pipeline_caps_get()->float_render;
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(p, &st);
  passes_mask = 0;
  for (int k = 0; k < NPASS; k++) {
    gpu_ms[k] = cpu_ms[k] = -1;
    for (int i = 0; i < n; i++)
      if (!strcmp(st[i].name, pass_names[k])) {
        if (st[i].ran) passes_mask |= 1 << k;
        gpu_ms[k] = st[i].gpu_ms_avg;
        cpu_ms[k] = st[i].cpu_ms_avg;
      }
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = {
    .title = "post_synth", .width = POST_PERF ? 1280 : 640, .height = POST_PERF ? 720 : 360, .debug = true,
    .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base);
}
