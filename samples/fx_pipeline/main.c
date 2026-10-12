/* fx_pipeline: the 3D effects through the render pipeline, the same on GL and
 * WebGPU.
 *
 * three.c has no WebGPU backend yet, so the built-in "scene" pass is off and
 * an OPAQUE pass of this app ("fxp_scene", webgpu = true) draws three
 * analytic boxes (a floor, a table, a tilted block on it) with depth: the
 * pipeline then copies the depth for the EFFECTS phase (GL: a depth copy;
 * WebGPU: the R32F linear depth) and runs the effects module's own passes:
 * decals (MIX on the table, MULTIPLY on the block, ADD on the floor, MIX on
 * the table's front) and particles (rain resting on the table by depth
 * collision, a fountain bouncing off analytic colliders with trails, sorted
 * soft smoke in curl noise, instanced mesh debris, a ring pulled by an
 * attractor, a Trail3D ribbon).
 *
 * The tree pauses at FREEZE ticks, so asynchronous stats readbacks (WebGPU)
 * reach the same simulation state on every host before the gate's frame.
 *
 * Debug state: fxp.<system>.alive / gpu_alive / gpu_sum, fxp.rain_mean_y
 * (x1000), fxp.rain_on_top, fxp.draws, fxp.sort_passes, fxp.decals,
 * fxp.tick. FXP_NO_DECALS=1 / FXP_NO_COLLIDE=1 (the gates' controls). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/decals.h"
#include "cartwheel/gpu.h"
#include "cartwheel/particles.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

#define FREEZE 100

static const char *const names[] = { "Rain", "Fountain", "Smoke", "Debris", "Swirl" };
#define NSYS 5
static int32_t alive[NSYS], gpu_alive[NSYS], rain_mean_y, rain_on_top, draws, sort_passes, decals;
static float gpu_sum[NSYS][3];
static uint32_t tick;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }
static cw_node sys(int i) {
  char p[48];
  snprintf(p, sizeof p, "World/%s", names[i]);
  return node(p);
}

/* ── the boxes: 3 x 36 vertices from the vertex index ─────────────────── */

#define BOXES_GLSL_VS \
  "uniform mat4 u_vp;\n" \
  "uniform mat4 u_m0, u_m1, u_m2;\n" \
  "uniform vec4 u_c0, u_c1, u_c2;\n" \
  "out vec3 v_c;\n" \
  "void main() {\n" \
  "  int b = gl_VertexID / 36, v = gl_VertexID - b * 36, f = v / 6, k = v - f * 6;\n" \
  "  vec2 q = k == 0 || k == 3 ? vec2(-1.0, -1.0) : k == 1 ? vec2(1.0, -1.0) : k == 2 || k == 4 ? vec2(1.0, 1.0) : vec2(-1.0, 1.0);\n" \
  "  int ax = f / 2; float sg = (f - ax * 2) == 0 ? 1.0 : -1.0;\n" \
  "  vec3 n = ax == 0 ? vec3(sg, 0.0, 0.0) : ax == 1 ? vec3(0.0, sg, 0.0) : vec3(0.0, 0.0, sg);\n" \
  "  vec3 t = ax == 0 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), s = cross(n, t);\n" \
  "  vec3 p = (n + t * q.x + s * q.y) * 0.5;\n" \
  "  mat4 m = b == 0 ? u_m0 : b == 1 ? u_m1 : u_m2;\n" \
  "  vec4 c = b == 0 ? u_c0 : b == 1 ? u_c1 : u_c2;\n" \
  "  vec3 wn = normalize(mat3(m) * n);\n" \
  "  v_c = c.rgb * (0.35 + 0.65 * max(dot(wn, normalize(vec3(0.4, 0.9, 0.3))), 0.0));\n" \
  "  gl_Position = u_vp * (m * vec4(p, 1.0));\n" \
  "}\n"
static const char *BOXES_FS = "in vec3 v_c;\nout vec4 o;\nvoid main() { o = vec4(v_c, 1.0); }\n";

static const char *BOXES_WGSL =
    "struct U { u_vp: mat4x4f, u_m0: mat4x4f, u_m1: mat4x4f, u_m2: mat4x4f, u_c0: vec4f, u_c1: vec4f, u_c2: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "struct V { @builtin(position) pos: vec4f, @location(0) c: vec3f };\n"
    "@vertex fn vs(@builtin(vertex_index) vi: u32) -> V {\n"
    "  let i = i32(vi); let b = i / 36; let v = i - b * 36; let f = v / 6; let k = v - f * 6;\n"
    "  var q = vec2f(-1.0, 1.0);\n"
    "  if (k == 0 || k == 3) { q = vec2f(-1.0, -1.0); } else if (k == 1) { q = vec2f(1.0, -1.0); } else if (k == 2 || k == 4) { q = vec2f(1.0, 1.0); }\n"
    "  let ax = f / 2; let sg = select(-1.0, 1.0, (f - ax * 2) == 0);\n"
    "  var n = vec3f(0.0, 0.0, sg);\n"
    "  if (ax == 0) { n = vec3f(sg, 0.0, 0.0); } else if (ax == 1) { n = vec3f(0.0, sg, 0.0); }\n"
    "  let t = select(vec3f(1.0, 0.0, 0.0), vec3f(0.0, 1.0, 0.0), ax == 0); let s = cross(n, t);\n"
    "  let p = (n + t * q.x + s * q.y) * 0.5;\n"
    "  var m = u.u_m2; var c = u.u_c2;\n"
    "  if (b == 0) { m = u.u_m0; c = u.u_c0; } else if (b == 1) { m = u.u_m1; c = u.u_c1; }\n"
    "  let wn = normalize(mat3x3f(m[0].xyz, m[1].xyz, m[2].xyz) * n);\n"
    "  var o: V;\n"
    "  o.c = c.rgb * (0.35 + 0.65 * max(dot(wn, normalize(vec3f(0.4, 0.9, 0.3))), 0.0));\n"
    "  var cp = u.u_vp * (m * vec4f(p, 1.0));\n"
    "  cp.z = (cp.z + cp.w) * 0.5;\n"
    "  cp.y = cp.y * cw_target.flip;\n"
    "  o.pos = cp;\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs(i: V) -> @location(0) vec4f { return vec4f(i.c, 1.0); }\n";

typedef struct box { t3_vec3 pos, rot, size; float c[4]; } box;
static const box boxes[3] = {
  { { 0, -0.05f, 0 }, { 0, 0, 0 }, { 10, 0.1f, 10 }, { 0.3f, 0.3f, 0.32f, 1 } },
  { { 0, 0.5f, 0 }, { 0, 0, 0 }, { 2, 1, 2 }, { 0.55f, 0.35f, 0.25f, 1 } },
  { { 0.0f, 1.1f, -0.7f }, { 0.35f, 0.6f, 0.2f }, { 0.6f, 0.25f, 0.6f }, { 0.4f, 0.45f, 0.5f, 1 } },
};

static void scene_exec(cw_pass_ctx *ctx, void *u) {
  (void)u;
  static unsigned prog;
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  if (!prog) prog = wg ? cw_pass_program_wgsl("fxp_scene", BOXES_WGSL) : cw_pass_program_vs("fxp_scene", BOXES_GLSL_VS, BOXES_FS);
  cw_pass_clear(0.05f, 0.06f, 0.08f, 1, true);
  if (!prog || !ctx->camera) return;
  t3_mat4 view, vp;
  t3_object_update_world_matrix(ctx->camera, true, false);
  t3_mat4_invert(&view, &ctx->camera->base.matrix_world);
  t3_mat4_multiply(&vp, &ctx->camera->projection_matrix, &view);
  cw_pass_use(prog);
  cw_pass_set_depth(true, true);
  cw_pass_uniform_mat4(prog, "u_vp", &vp);
  static const char *const mn[3] = { "u_m0", "u_m1", "u_m2" }, *const cn[3] = { "u_c0", "u_c1", "u_c2" };
  for (int i = 0; i < 3; i++) {
    t3_mat4 m;
    t3_mat4_compose(&m, boxes[i].pos, t3_quat_from_euler((t3_euler){ boxes[i].rot.x, boxes[i].rot.y, boxes[i].rot.z, T3_XYZ }),
                    boxes[i].size);
    cw_pass_uniform_mat4(prog, mn[i], &m);
    cw_pass_uniform4f(prog, cn[i], boxes[i].c[0], boxes[i].c[1], boxes[i].c[2], boxes[i].c[3]);
  }
  cw_pass_draw(3 * 36);
  cw_pass_set_depth(false, false);
}

/* ── the app ──────────────────────────────────────────────────────────── */

static void init(void *u) {
  (void)u;
  static char keys[NSYS][3][40];
  for (int i = 0; i < NSYS; i++) {
    snprintf(keys[i][0], 40, "fxp.%s.alive", names[i]);
    snprintf(keys[i][1], 40, "fxp.%s.gpu_alive", names[i]);
    snprintf(keys[i][2], 40, "fxp.%s.gpu_sum", names[i]);
    cw_debug_i32(keys[i][0], &alive[i]);
    cw_debug_i32(keys[i][1], &gpu_alive[i]);
    cw_debug_f32_array(keys[i][2], gpu_sum[i], 3);
  }
  cw_debug_i32("fxp.rain_mean_y", &rain_mean_y);
  cw_debug_i32("fxp.rain_on_top", &rain_on_top);
  cw_debug_i32("fxp.draws", &draws);
  cw_debug_i32("fxp.sort_passes", &sort_passes);
  cw_debug_i32("fxp.decals", &decals);
  cw_debug_u32("fxp.tick", &tick);
  for (int i = 0; i < NSYS; i++) cw_node_set(sys(i), "gpu_stats", cw_val_bool(true));
  const char *e = getenv("FXP_NO_COLLIDE");
  if (e && *e == '1') cw_node_set(node("World/Rain"), "collide_depth", cw_val_bool(false));
  e = getenv("FXP_NO_DECALS");
  if (e && *e == '1')
    for (int i = 0; i < 4; i++) {
      static const char *const d[4] = { "World/DecalTop", "World/DecalBump", "World/DecalFloor", "World/DecalWall" };
      cw_node_set(node(d[i]), "color", cw_val_color(1, 1, 1, 0));
    }
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  cw_pipeline_set_pass_enabled(p, "scene", false);
  cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "fxp_scene", .phase = CW_PHASE_OPAQUE, .order = 0, .webgpu = true,
                                           .writes = CW_RES_COLOR | CW_RES_DEPTH, .execute = scene_exec });
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  if (tick >= FREEZE) {
    cw_tree_set_paused(cw_scene_app_tree(), true);
    return;
  }
  tick++;
  float s, c;
  cw_sincos((float)tick * 0.06f, &s, &c);
  cw_node3d_set_position(node("World/Orbiter"), (t3_vec3){ 2.5f + 0.9f * c, 1.2f + 0.3f * s, 1.5f + 0.9f * s });
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  for (int i = 0; i < NSYS; i++) {
    cw_node n = sys(i);
    cw_value v;
    alive[i] = cw_particles_alive(n);
    if (cw_node_get_prop(n, "gpu_alive", &v)) gpu_alive[i] = (int32_t)v.i;
    if (cw_node_get_prop(n, "gpu_sum", &v))
      for (int k = 0; k < 3; k++) gpu_sum[i][k] = v.v[k];
  }
  if (gpu_alive[0] > 0) {
    float my = gpu_sum[0][1] / (float)gpu_alive[0];
    rain_mean_y = (int32_t)(my * 1000.0f);
    rain_on_top = my > 0.95f && my < 1.35f;
  }
  cw_particles_stats st;
  cw_particles_get_stats(cw_scene_app_tree(), &st);
  draws = st.draw_calls;
  sort_passes = st.sort_passes;
  cw_decal_stats ds;
  cw_decals_get_stats(cw_scene_app_tree(), &ds);
  decals = ds.visible;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_pipeline", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
