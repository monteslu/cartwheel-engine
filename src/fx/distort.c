/* The distortion buffer, the scene colour copy and Shockwave3D (explosions.h).
 *
 * Distortion: things that bend the picture (heat haze particles, shockwave
 * rings) do not draw colour. They add "move the picture this much here" into
 * a small RGBA16F buffer: rg = the offset in screen units, a = a weight, b =
 * weight x the view depth of the bending thing. The "distort" POST pass then
 * reads the finished frame through those offsets. Three guards keep the
 * picture honest:
 *   - a distortion fragment is dropped where the scene is nearer than it (a
 *     pole in front of a haze is not bent);
 *   - the pass takes the unbent colour where the shifted sample would land on
 *     something nearer than the bending thing (no foreground colour smeared
 *     into the haze);
 *   - the buffer is drawn at the effects preset's share of the frame (half
 *     on Low and Medium: it is filtered up) and colour fringing is High and
 *     Ultra only.
 * Everything here is render only and reads the frame's own depth: nothing
 * reaches the simulation.
 *
 * Two passes: "distort_sources" (EFFECTS 150: makes and clears this view's
 * buffer, draws the shockwaves; the particles pass at 200 adds the DISTORT
 * particles) and "distort" (POST 150: applies it). Both run only while
 * something distorts. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/explosions.h"
#include "cartwheel/gpu.h"
#include "cartwheel/quality.h"
#include "fx_internal.h"

/* ── Shockwave3D data ─────────────────────────────────────────────────── */

typedef struct shock {
  bool playing, loop, autostart, ground;
  float max_radius, duration, thickness, strength;
  float age;      /* seconds into the ring's life */
  bool started;   /* autostart has fired */
  float radius;   /* read-only: the ring's radius now */
  float amplitude; /* read-only: 0..1 */
} shock;
static const cw_node_type *shock_t;

static float ease_out(float u) { return 1.0f - (1.0f - u) * (1.0f - u); }

static void shock_eval(shock *s) {
  float u = s->duration > 0 ? CW_CLAMP(s->age / s->duration, 0.0f, 1.0f) : 1.0f;
  s->radius = s->max_radius * ease_out(u);
  float f = 1.0f - u;
  s->amplitude = s->playing ? f * sqrtf(f) : 0.0f;
}

static void shock_init(cw_node n, void *data) {
  (void)n;
  shock *s = data;
  s->autostart = true;
  s->max_radius = 6;
  s->duration = 0.9f;
  s->thickness = 0.7f;
  s->strength = 0.5f;
}

static void shock_enter(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  cw__fx_tree *ft = cw__fx_tree_of(t);
  cw_array_push(&ft->shockwaves, n);
  cw__fx_ensure_distort(t);
}

static void shock_exit(cw_node n, void *data) {
  (void)data;
  cw_tree *t = cw_node_tree(n);
  if (t) cw__fx_remove_node(&cw__fx_tree_of(t)->shockwaves, n);
}

static void shock_fixed(cw_node n, void *data, double dt) {
  (void)n;
  shock *s = data;
  if (s->autostart && !s->started) {
    s->started = true;
    s->playing = true;
    s->age = 0;
  }
  if (s->playing) {
    s->age += (float)dt;
    if (s->age >= s->duration) {
      if (s->loop) s->age -= s->duration;
      else {
        s->playing = false;
        s->age = s->duration;
      }
    }
  }
  shock_eval(s);
}

void cw_shockwave_fire(cw_node n) {
  shock *s = cw_node_data_t(n, shock_t);
  if (!s) return;
  s->started = true;
  s->playing = true;
  s->age = 0;
  shock_eval(s);
}

float cw_shockwave_radius(cw_node n) {
  shock *s = cw_node_data_t(n, shock_t);
  return s ? s->radius : 0;
}

bool cw_shockwave_active(cw_node n) {
  shock *s = cw_node_data_t(n, shock_t);
  return s && s->playing;
}

#define SF(field, ty, doc) CW_PROP_FIELD(shock, field, ty, CW_PROP_DEFAULT, doc)
#define SR(field, lo, hi, doc) CW_PROP_FIELD_RANGE(shock, field, CW_T_FLOAT, lo, hi, doc)
static const cw_prop shock_props[] = {
  SF(playing, CW_T_BOOL, "The ring is expanding now (cw_shockwave_fire restarts it)."),
  SF(loop, CW_T_BOOL, "Start again when it ends."),
  SF(autostart, CW_T_BOOL, "Fire once when the node enters the tree."),
  SF(ground, CW_T_BOOL, "A ring lying in the node's XZ plane (a blast spreading along the ground); off: a ring facing the camera (a blast in the air)."),
  SR(max_radius, 0.01, 1e5, "Radius the ring reaches at the end (world units); it slows as it grows."),
  SR(duration, 0.01, 60, "Seconds from the start to the end."),
  SR(thickness, 0.01, 1e4, "Width of the bent band (world units)."),
  SR(strength, 0, 100, "How far the picture is pushed at the band's centre at the start (world units at the ring's distance)."),
  CW_PROP_FIELD(shock, age, CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, "Seconds into the ring's life."),
  CW_PROP_FIELD(shock, radius, CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, "The ring's radius now."),
  CW_PROP_FIELD(shock, amplitude, CW_T_FLOAT, CW_PROP_EDITOR | CW_PROP_READONLY, "The ring's strength now, 0 to 1 of `strength`."),
  CW_PROP_END,
};
static cw_node_type shock_type = {
  .name = "Shockwave3D", .parent = "Node3D", .data_size = sizeof(shock), .props = shock_props,
  .doc = "An expanding ring that bends the picture (a shockwave): needs no texture, depth aware (explosions.h).",
  .init = shock_init, .enter_tree = shock_enter, .exit_tree = shock_exit, .fixed_update = shock_fixed,
};

/* ── who distorts this frame ──────────────────────────────────────────── */

static bool distort_wanted(cw__fx_tree *ft) {
  for (int i = 0; i < ft->shockwaves.count; i++)
    if (cw_shockwave_active(ft->shockwaves.data[i])) return true;
  for (int i = 0; i < ft->systems.count; i++)
    if (cw__psys_distort_active(ft->systems.data[i])) return true;
  return false;
}

static bool wanted_ctx(const cw_pass_ctx *ctx) {
  cw__fx_tree *ft = cw_tree_slot(ctx->tree, "fx");
  return ft && ctx->camera && ft->distort_added && distort_wanted(ft);
}

const cw_rt *cw__fx_distort_target(cw__fx_tree *ft) { return ft->dist; }

/* ── shaders ──────────────────────────────────────────────────────────── */

/* Depth: GLSL reads the window depth (lin() makes view distance), WGSL the
 * linear R32F (3e38 = nothing). */
static const char RING_GLSL_VS[] =
    "uniform mat4 u_vp;\n"
    "uniform vec4 u_c, u_right, u_up, u_b;\n"
    "out vec2 v_q;\n"
    "void main() {\n"
    "  int k = gl_VertexID;\n"
    "  vec2 q = (k == 0 || k == 3) ? vec2(-1.0, -1.0) : (k == 1) ? vec2(1.0, -1.0) : (k == 2 || k == 4) ? vec2(1.0, 1.0) : vec2(-1.0, 1.0);\n"
    "  v_q = q * u_b.w;\n"
    "  vec3 w = u_c.xyz + (u_right.xyz * q.x + u_up.xyz * q.y) * u_b.w;\n"
    "  gl_Position = u_vp * vec4(w, 1.0);\n"
    "}\n";
static const char RING_GLSL_FS[] =
    "in vec2 v_q;\n"
    "uniform sampler2D u_depth;\n"
    "uniform vec4 u_a;    /* radius, thickness, strength, centre view depth */\n"
    "uniform vec4 u_b;    /* amplitude, ortho, ground, extent */\n"
    "uniform vec4 u_uvc;  /* centre uv, proj00 / 2, proj11 / 2 */\n"
    "uniform vec4 u_soft; /* near, far, ortho */\n"
    "uniform vec4 u_size; /* target w, h */\n"
    "out vec4 o;\n"
    "float lin(float d) { return u_soft.z > 0.5 ? u_soft.x + d * (u_soft.y - u_soft.x) : u_soft.x * u_soft.y / (u_soft.y - d * (u_soft.y - u_soft.x)); }\n"
    "void main() {\n"
    "  vec2 uv = gl_FragCoord.xy / u_size.xy;\n"
    "  float vz = u_b.y > 0.5 ? u_a.w : 1.0 / gl_FragCoord.w;\n"
    "  float zs = lin(texture(u_depth, uv).r);\n"
    "  if (u_b.z > 0.5 ? zs < vz - 0.25 : zs < u_a.w - 0.5 * u_a.x - 0.1) discard;\n"
    "  float x = (length(v_q) - u_a.x) / u_a.y;\n"
    "  float w = exp(-x * x * 2.5) * u_b.x;\n"
    "  if (w < 0.002) discard;\n"
    "  vec2 d = (uv - u_uvc.xy) * u_size.xy;\n"
    "  vec2 n = d / max(length(d), 1e-3);\n"
    "  float mag = u_a.z * u_uvc.w / vz;\n"
    "  o = vec4(n * mag * vec2(u_size.y / u_size.x, 1.0) * w, vz * w, w);\n"
    "}\n";

static const char RING_WGSL[] =
    "struct U { u_vp: mat4x4f, u_c: vec4f, u_right: vec4f, u_up: vec4f, u_a: vec4f, u_b: vec4f, u_uvc: vec4f, u_soft: vec4f, u_size: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var u_depth: texture_2d<f32>;\n"
    "struct V { @builtin(position) pos: vec4f, @location(0) q: vec2f };\n"
    "@vertex fn vs(@builtin(vertex_index) vi: u32) -> V {\n"
    "  var q = vec2f(-1.0, 1.0);\n"
    "  if (vi == 0u || vi == 3u) { q = vec2f(-1.0, -1.0); } else if (vi == 1u) { q = vec2f(1.0, -1.0); } else if (vi == 2u || vi == 4u) { q = vec2f(1.0, 1.0); }\n"
    "  let w = u.u_c.xyz + (u.u_right.xyz * q.x + u.u_up.xyz * q.y) * u.u_b.w;\n"
    "  var cp = u.u_vp * vec4f(w, 1.0);\n"
    "  cp.z = (cp.z + cp.w) * 0.5;\n"
    "  cp.y = cp.y * cw_target.flip;\n"
    "  var o: V;\n"
    "  o.pos = cp;\n"
    "  o.q = q * u.u_b.w;\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs(i: V) -> @location(0) vec4f {\n"
    "  let size = cw_target.size;\n"
    "  let fc = cw_frag_coord(i.pos);\n"
    "  let uv = fc / size;\n"
    "  let vz = select(1.0 / i.pos.w, u.u_a.w, u.u_b.y > 0.5);\n"
    "  let zs = cw_load(u_depth, uv).r;\n"
    "  let occ_s = (zs < (u.u_a.w - 0.5 * u.u_a.x - 0.1));\n"
    "  let occ_g = (zs < (vz - 0.25));\n"
    "  if (select(occ_s, occ_g, (u.u_b.z > 0.5))) { discard; }\n"
    "  let x = (length(i.q) - u.u_a.x) / u.u_a.y;\n"
    "  let w = exp(-x * x * 2.5) * u.u_b.x;\n"
    "  if (w < 0.002) { discard; }\n"
    "  let d = (uv - u.u_uvc.xy) * size;\n"
    "  let n = d / max(length(d), 1e-3);\n"
    "  let mag = u.u_a.z * u.u_uvc.w / vz;\n"
    "  return vec4f(n * mag * vec2f(size.y / size.x, 1.0) * w, vz * w, w);\n"
    "}\n";

static const char APPLY_GLSL[] =
    "uniform sampler2D u_src, u_dist, u_depth;\n"
    "uniform vec4 u_p;    /* dispersion, how much nearer than the haze a sample may be, max shift */\n"
    "uniform vec4 u_soft; /* near, far, ortho */\n"
    "out vec4 o;\n"
    "float lin(float d) { return u_soft.z > 0.5 ? u_soft.x + d * (u_soft.y - u_soft.x) : u_soft.x * u_soft.y / (u_soft.y - d * (u_soft.y - u_soft.x)); }\n"
    "void main() {\n"
    "  vec4 s0 = texture(u_src, v_uv);\n"
    "  vec4 d = texture(u_dist, v_uv);\n"
    "  if (d.a <= 1e-5) { o = s0; return; }\n"
    "  float hz = d.b / d.a;\n"
    "  if (lin(texture(u_depth, v_uv).r) < hz - u_p.y) { o = s0; return; }\n"
    "  vec2 off = clamp(d.rg, vec2(-u_p.z), vec2(u_p.z));\n"
    "  vec2 uv2 = v_uv + off;\n"
    "  if (lin(texture(u_depth, uv2).r) < hz - u_p.y) { o = s0; return; }\n"
    "  vec3 c = texture(u_src, uv2).rgb;\n"
    "  if (u_p.x > 0.0) c = vec3(texture(u_src, uv2 + off * u_p.x).r, c.g, texture(u_src, uv2 - off * u_p.x).b);\n"
    "  o = vec4(c, s0.a);\n"
    "}\n";

static const char APPLY_WGSL[] =
    "struct U { u_p: vec4f, u_soft: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
    "@group(0) @binding(2) var u_dist: texture_2d<f32>;\n"
    "@group(0) @binding(3) var u_depth: texture_2d<f32>;\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let s0 = textureSampleLevel(u_src, cw_linear, uv, 0.0);\n"
    "  let d = textureSampleLevel(u_dist, cw_linear, uv, 0.0);\n"
    "  if (d.a <= 1e-5) { return s0; }\n"
    "  let hz = d.b / d.a;\n"
    "  if (cw_load(u_depth, uv).r < hz - u.u_p.y) { return s0; }\n"
    "  let off = clamp(d.rg, vec2f(-u.u_p.z), vec2f(u.u_p.z));\n"
    "  let uv2 = uv + off;\n"
    "  if (cw_load(u_depth, uv2).r < hz - u.u_p.y) { return s0; }\n"
    "  var c = textureSampleLevel(u_src, cw_linear, uv2, 0.0).rgb;\n"
    "  if (u.u_p.x > 0.0) {\n"
    "    c = vec3f(textureSampleLevel(u_src, cw_linear, uv2 + off * u.u_p.x, 0.0).r, c.g,\n"
    "              textureSampleLevel(u_src, cw_linear, uv2 - off * u.u_p.x, 0.0).b);\n"
    "  }\n"
    "  return vec4f(c, s0.a);\n"
    "}\n";

static const char COPY_GLSL[] =
    "uniform sampler2D u_src;\n"
    "out vec4 o;\n"
    "void main() { o = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0); }\n";
static const char COPY_WGSL[] =
    "@group(0) @binding(1) var u_src: texture_2d<f32>;\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f { return cw_load(u_src, uv); }\n";

static bool wg(void) { return cw_gpu_backend() == CW_GPU_WEBGPU; }

/* ── the sources pass ─────────────────────────────────────────────────── */

static void ring_draw(cw_pass_ctx *ctx, cw_node n, const shock *s, const cw_rt *rt) {
  static unsigned prog;
  if (!prog) prog = wg() ? cw_pass_program_wgsl("shockwave", RING_WGSL) : cw_pass_program_vs("shockwave", RING_GLSL_VS, RING_GLSL_FS);
  if (!prog || s->amplitude <= 0.002f) return;
  const t3_mat4 *g = cw_node3d_global_transform(n);
  t3_vec3 c = { g->e[12], g->e[13], g->e[14] }, right, up;
  float ext = s->max_radius * 1.1f + s->thickness * 2.0f;
  if (s->ground) {
    right = t3_vec3_normalize((t3_vec3){ g->e[0], g->e[1], g->e[2] });
    up = t3_vec3_normalize((t3_vec3){ g->e[8], g->e[9], g->e[10] });
    c.y += 0.04f;
  } else {
    right = (t3_vec3){ ctx->view.e[0], ctx->view.e[4], ctx->view.e[8] };
    up = (t3_vec3){ ctx->view.e[1], ctx->view.e[5], ctx->view.e[9] };
  }
  t3_mat4 vp;
  t3_mat4_multiply(&vp, &ctx->camera->projection_matrix, &ctx->view);
  /* the centre: view depth and screen position */
  float vzc = -(ctx->view.e[2] * c.x + ctx->view.e[6] * c.y + ctx->view.e[10] * c.z + ctx->view.e[14]);
  const float *p = ctx->camera->projection_matrix.e;
  float cx = vp.e[0] * c.x + vp.e[4] * c.y + vp.e[8] * c.z + vp.e[12];
  float cy = vp.e[1] * c.x + vp.e[5] * c.y + vp.e[9] * c.z + vp.e[13];
  float cw = vp.e[3] * c.x + vp.e[7] * c.y + vp.e[11] * c.z + vp.e[15];
  if (cw < 1e-4f) cw = 1e-4f;
  bool ortho = p[15] > 0.5f;
  float lp[3];
  cw_pass_linear_depth_params(ctx, lp);
  cw_pass_use(prog);
  cw_pass_bind_texture(0, ctx->depth_tex);
  cw_pass_uniform1i(prog, "u_depth", 0);
  cw_pass_uniform_mat4(prog, "u_vp", &vp);
  cw_pass_uniform4f(prog, "u_c", c.x, c.y, c.z, 1);
  cw_pass_uniform4f(prog, "u_right", right.x, right.y, right.z, 0);
  cw_pass_uniform4f(prog, "u_up", up.x, up.y, up.z, 0);
  cw_pass_uniform4f(prog, "u_a", s->radius, s->thickness, s->strength, vzc);
  cw_pass_uniform4f(prog, "u_b", s->amplitude, ortho ? 1.0f : 0.0f, s->ground ? 1.0f : 0.0f, ext);
  cw_pass_uniform4f(prog, "u_uvc", cx / cw * 0.5f + 0.5f, cy / cw * 0.5f + 0.5f, p[0] * 0.5f, p[5] * 0.5f);
  cw_pass_uniform4f(prog, "u_soft", lp[0], lp[1], lp[2], 0);
  cw_pass_uniform4f(prog, "u_size", (float)rt->w, (float)rt->h, 0, 0);
  cw_pass_set_blend(CW_PASS_BLEND_ADD);
  cw_pass_draw(6);
  cw_pass_set_blend(CW_PASS_BLEND_NONE);
}

static bool sources_active(const cw_pass_ctx *ctx, void *user) {
  (void)user;
  return wanted_ctx(ctx);
}

static void sources_exec(cw_pass_ctx *ctx, void *user) {
  (void)user;
  cw__fx_tree *ft = cw__fx_tree_of(ctx->tree);
  ft->dist = NULL;
  if (!ctx->depth_tex) return;
  float sc = CW_CLAMP(cw_quality_scale("distortion"), 0.25f, 1.0f);
  int w = CW_MAX((int)((float)ctx->width * sc + 0.5f), 1), h = CW_MAX((int)((float)ctx->height * sc + 0.5f), 1);
  const cw_rt *rt = cw_pass_rt(ctx, w, h, CW_RT_RGBA16F, false);
  if (!rt) return;
  cw_pass_bind_target(ctx, rt);
  cw_pass_clear(0, 0, 0, 0, false);
  ft->dist = rt;
  for (int i = 0; i < ft->shockwaves.count; i++) {
    shock *s = cw_node_data_t(ft->shockwaves.data[i], shock_t);
    if (s && s->playing) ring_draw(ctx, ft->shockwaves.data[i], s, rt);
  }
  cw_pass_bind_target(ctx, ctx->scene);
}

/* ── the apply pass ───────────────────────────────────────────────────── */

static void apply_exec(cw_pass_ctx *ctx, void *user) {
  (void)user;
  static unsigned prog, cprog;
  cw__fx_tree *ft = cw__fx_tree_of(ctx->tree);
  const cw_rt *dist = ft->dist;
  ft->dist = NULL;
  if (!prog) prog = wg() ? cw_pass_program_wgsl("distort", APPLY_WGSL) : cw_pass_program("distort", APPLY_GLSL);
  if (!prog) return;
  if (!dist) { /* a writer must write dst: pass the image through */
    if (!cprog) cprog = wg() ? cw_pass_program_wgsl("distort.copy", COPY_WGSL) : cw_pass_program("distort.copy", COPY_GLSL);
    if (!cprog) return;
    cw_pass_use(cprog);
    cw_pass_bind_texture(0, ctx->src->tex);
    cw_pass_uniform1i(cprog, "u_src", 0);
    cw_pass_fullscreen();
    return;
  }
  float lp[3];
  cw_pass_linear_depth_params(ctx, lp);
  cw_pass_use(prog);
  cw_pass_bind_texture(0, ctx->src->tex);
  cw_pass_bind_texture(1, dist->tex);
  cw_pass_bind_texture(2, ctx->depth_tex);
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_uniform1i(prog, "u_dist", 1);
  cw_pass_uniform1i(prog, "u_depth", 2);
  cw_pass_uniform4f(prog, "u_p", cw_quality_scale("dispersion") * 0.35f, 0.25f, 0.08f, 0);
  cw_pass_uniform4f(prog, "u_soft", lp[0], lp[1], lp[2], 0);
  cw_pass_fullscreen();
}

void cw__fx_ensure_distort(cw_tree *t) {
  cw__fx_tree *ft = cw__fx_tree_of(t);
  if (ft->distort_added) return;
  cw_pipeline *p = cw_pipeline_of(t);
  if (!cw_pipeline_has_pass(p, "distort_sources"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "distort_sources", .phase = CW_PHASE_EFFECTS, .order = 150,
                                             .reads = CW_RES_DEPTH, .active = sources_active, .execute = sources_exec });
  if (!cw_pipeline_has_pass(p, "distort"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .webgpu = true, .name = "distort", .phase = CW_PHASE_POST, .order = 150,
                                             .reads = CW_RES_COLOR | CW_RES_DEPTH, .writes = CW_RES_COLOR,
                                             .active = sources_active, .execute = apply_exec });
  ft->distort_added = true;
}

/* ── the scene colour copy ────────────────────────────────────────────── */

const cw_rt *cw__fx_scene_copy(cw_pass_ctx *ctx, cw__fx_tree *ft) {
  static unsigned prog;
  ft->copy = NULL;
  if (!ctx->scene) return NULL;
  if (!prog) prog = wg() ? cw_pass_program_wgsl("scene_copy", COPY_WGSL) : cw_pass_program("scene_copy", COPY_GLSL);
  if (!prog) return NULL;
  const cw_rt *rt = cw_pass_rt(ctx, ctx->scene->w, ctx->scene->h, ctx->scene->format, false);
  if (!rt) return NULL;
  cw_pass_bind_target(ctx, rt);
  cw_pass_use(prog);
  cw_pass_bind_texture(0, ctx->scene->tex);
  cw_pass_uniform1i(prog, "u_src", 0);
  cw_pass_fullscreen();
  cw_pass_bind_target(ctx, ctx->scene);
  ft->copy = rt;
  return rt;
}

void cw__register_distort_types(void) {
  cw_node_type_register(&shock_type);
  shock_t = &shock_type;
}
