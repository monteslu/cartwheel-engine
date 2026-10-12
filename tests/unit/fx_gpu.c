/* fx_gpu: the 3D effects on BOTH GPU backends from one scene, without the
 * render pipeline: the app draws a few analytic boxes itself (colour, depth
 * and, on WebGPU, the linear depth the pipeline core will provide), builds
 * the pass context, runs the effects module's passes (decals, particles,
 * trails) and copies the result to the window. The same frames and the same
 * named state come out of the GL path (transform feedback, fragment-pass
 * sort and reduction, glReadPixels) and the WebGPU path (compute, async
 * readbacks): tests/gate.mjs --gpu webgpu compares them.
 *
 * The tree stops ticking at FREEZE (the particles stay put, every pass still
 * runs each frame) so asynchronous results catch up to the same simulation
 * state on every host before the gate's frame.
 *
 * Debug state: fxg.<system>.alive / .gpu_alive / .gpu_sum, fxg.rain_mean_y
 * (x1000) and fxg.rain_on_top (the rain rests on the table: depth
 * collision), fxg.sort_ok (the Smoke system's sorted keys are a permutation
 * in ascending order), fxg.sorts_checked, fxg.steps / draws / sort_passes,
 * fxg.tick.
 *
 * CW_FXGPU_FAULT (native, the gates' controls): 1 a broken compute shader
 * (the run must fail on WebGPU errors), 2 the bitonic sort in the wrong
 * order, 3 the colliders dropped. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/environment.h"
#include "cartwheel/gl.h"
#include "cartwheel/gpu.h"
#include "cartwheel/particles.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "fx/fx_internal.h"
#include "fx/fx_wgpu.h"
#if CW_WGPU
#include "platform/wgpu.h"
#endif

#define FREEZE 100
#define W 640
#define H 360

static const char *const names[] = { "Rain", "Fountain", "Smoke", "Debris", "Swirl" };
#define NSYS 5
static int32_t alive[NSYS], gpu_alive[NSYS];
static float gpu_sum[NSYS][3];
static int32_t rain_mean_y, rain_on_top, sort_ok, sorts_checked, steps, draws, sort_passes;
static uint32_t tick;
/* sky: the cube's face means and its agreement with the
 * CPU model */
static float sky_face[6][3];
static int32_t sky_cpu_ok, sky_done;
static float sky_cpu_err;
bool cw__sky_test_build(cw_tree *t);
/* visibility: a VisibilityManager with occlusion queries
 * against the analytic boxes' depth (two meshes hidden behind the table and
 * under the floor), and impostors drawn from an atlas made here */
#include "cartwheel/visibility.h"
#include "visibility/vis_internal.h"
void cw__vis_test_exec(cw_pass_ctx *ctx, int phase);
void cw__sky_test_sun(cw_pass_ctx *ctx);
int cw__vis_impostor_atlas_from_pixels(vis_impostors *im, const uint8_t *albedo, const uint8_t *normal, float radius,
                                       const float center[3]);
static vis_impostors *imps;
static int imp_atlas = -1;
static int32_t vis_visible, vis_occluded, vis_fculled, vis_meshes, vis_queries_any, imp_draws;
/* meshes ever culled by an occlusion result (bit per mesh below): an
 * occluded mesh shows again the next frame to be re-queried, so the counts
 * of one frame depend on the readback latency; the set of meshes a query
 * ever found hidden does not */
static int32_t vis_hidden_mask;
/* decals: box projected from the scene depth */
#include "cartwheel/decals.h"
static int32_t dec_decals, dec_visible, dec_draws, dec_atlas;
static const char *const vis_names[4] = { "OccTable", "OccFloor", "SeenA", "SeenB" };
unsigned cw__sky_cube_gl(cw_tree *t, int *size);
t3_texture *cw__sky_cube_tex(cw_tree *t);
static cw_tree *tree;
static cw_node world;
static int fault;

static const float BG[3] = { 0.05f, 0.06f, 0.08f };

/* the analytic scene: boxes (centre, half sizes... as a model matrix) */
typedef struct box { t3_vec3 pos, rot, size, color; } box;
static const box scene_boxes[] = {
  { { 0, -0.05f, 0 }, { 0, 0, 0 }, { 10, 0.1f, 10 }, { 0.3f, 0.3f, 0.32f } },
  { { 0, 0.5f, 0 }, { 0, 0, 0 }, { 2, 1, 2 }, { 0.55f, 0.35f, 0.25f } },
  { { 0.0f, 1.1f, -0.7f }, { 0.35f, 0.6f, 0.2f }, { 0.6f, 0.25f, 0.6f }, { 0.4f, 0.45f, 0.5f } },
};
/* perf mode (CW_FXGPU_PERF=1): fx_perf's ground, block and dome (as a box) */
static const box perf_boxes[] = {
  { { 0, -0.1f, 0 }, { 0, 0, 0 }, { 40, 0.2f, 30 }, { 0.16f, 0.15f, 0.14f } },
  { { -4, 0.75f, 0 }, { 0, 0, 0 }, { 3, 1.5f, 3 }, { 0.45f, 0.35f, 0.3f } },
  { { 4, 0, -1 }, { 0, 0, 0 }, { 4, 4, 4 }, { 0.35f, 0.4f, 0.45f } },
};
#define NBOX 3
static const box *boxes = scene_boxes;
static bool perf;
static float frame_ms;
static int32_t perf_alive, perf_frames;
static double perf_sum;

static void box_matrix(const box *b, t3_mat4 *m) {
  t3_quat q = t3_quat_from_euler((t3_euler){ b->rot.x, b->rot.y, b->rot.z, T3_XYZ });
  t3_mat4_compose(m, b->pos, q, b->size);
}

/* a unit cube, 36 vertices of position + normal */
static float cube[36 * 6];
static void build_cube(void) {
  static const float n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
  int k = 0;
  for (int f = 0; f < 6; f++) {
    float a[3], b[3], c[3];
    for (int i = 0; i < 3; i++) a[i] = n[f][i] * 0.5f;
    /* two axes spanning the face */
    int ax = f < 2 ? 1 : 0, bx = f < 4 ? 2 : 1;
    if (f >= 2 && f < 4) bx = 2;
    memset(b, 0, sizeof b);
    memset(c, 0, sizeof c);
    b[ax] = 0.5f;
    c[bx] = 0.5f;
    static const float uv[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int v = 0; v < 6; v++) {
      for (int i = 0; i < 3; i++) cube[k++] = a[i] + b[i] * uv[v][0] + c[i] * uv[v][1];
      for (int i = 0; i < 3; i++) cube[k++] = n[f][i];
    }
  }
}

/* ── the camera and the pass context ──────────────────────────────────── */

static cw_node node(const char *path) { return cw_node_get(world, path); }

static void make_ctx(cw_pass_ctx *ctx) {
  memset(ctx, 0, sizeof *ctx);
  ctx->tree = tree;
  ctx->pipeline = cw_pipeline_of(tree);
  ctx->settings = cw_pipeline_settings(ctx->pipeline);
  ctx->width = W;
  ctx->height = H;
  cw_node cn = node("Camera");
  t3_camera *cam = cw_camera3d_t3(cn);
  cw_value v;
  if (cw_node_get_prop(cn, "fov", &v)) cam->fov = (float)v.f;
  if (cw_node_get_prop(cn, "near", &v)) cam->near = (float)v.f;
  if (cw_node_get_prop(cn, "far", &v)) cam->far = (float)v.f;
  cam->aspect = (float)W / (float)H;
  t3_camera_update_projection_matrix(cam);
  t3_object_update_world_matrix(cam, true, false);
  ctx->camera = cam;
  ctx->camera_node = cn;
  ctx->offscreen = true;
  t3_mat4_invert(&ctx->view, &cam->base.matrix_world);
  ctx->proj = cam->projection_matrix;
  t3_mat4_multiply(&ctx->view_proj, &ctx->proj, &ctx->view);
  t3_mat4_invert(&ctx->inv_proj, &ctx->proj);
  t3_mat4_invert(&ctx->inv_view_proj, &ctx->view_proj);
  ctx->prev_view_proj = ctx->view_proj;
}

/* sorted keys: ascending (key, then index) and a permutation of 0..n-1 */
static bool keys_ok(const float *kv, int stride, int n, bool idx_float) {
  uint8_t *seen = cw_calloc((size_t)n, 1, CW_MEM_RENDER);
  bool ok = true;
  for (int i = 0; i < n && ok; i++) {
    const float *e = kv + (size_t)stride * i;
    uint32_t idx;
    if (idx_float) idx = (uint32_t)e[1];
    else memcpy(&idx, e + 1, 4);
    if (idx >= (uint32_t)n || seen[idx]) ok = false;
    else seen[idx] = 1;
    if (i > 0) {
      const float *p = kv + (size_t)stride * (i - 1);
      uint32_t pidx;
      if (idx_float) pidx = (uint32_t)p[1];
      else memcpy(&pidx, p + 1, 4);
      if (p[0] > e[0] || (p[0] == e[0] && pidx > idx)) ok = false;
    }
  }
  cw_free(seen);
  return ok;
}

/* ── sky + PMREM checks (both backends) ───────────────────────────────── */

/* face f, texel (x, y) (GL rows): the direction the sky shader gave it */
static t3_vec3 face_dir(int f, int x, int y, int size) {
  float sc = (x + 0.5f) / size * 2 - 1, tc = (y + 0.5f) / size * 2 - 1;
  t3_vec3 g = f == 0 ? (t3_vec3){ 1, -tc, -sc } : f == 1 ? (t3_vec3){ -1, -tc, sc } : f == 2 ? (t3_vec3){ sc, 1, tc }
            : f == 3 ? (t3_vec3){ sc, -1, -tc } : f == 4 ? (t3_vec3){ sc, -tc, 1 } : (t3_vec3){ -sc, -tc, -1 };
  return t3_vec3_normalize((t3_vec3){ -g.x, g.y, g.z });
}

/* px: 6 faces of size^2 RGBA floats */
static void sky_check(const float *px, int size) {
  cw_sky_state ss;
  cw_sky_get_state(tree, &ss);
  cw_value v;
  cw_node sky = node("Sky");
  float ray = 1, mie = 1, g = 0.76f, e0 = 4;
  if (cw_node_get_prop(sky, "rayleigh", &v)) ray = (float)v.f;
  if (cw_node_get_prop(sky, "mie", &v)) mie = (float)v.f;
  if (cw_node_get_prop(sky, "mie_g", &v)) g = (float)v.f;
  float se = 4, ke = 1;
  if (cw_node_get_prop(sky, "sun_energy", &v)) se = (float)v.f;
  if (cw_node_get_prop(sky, "sky_energy", &v)) ke = (float)v.f;
  e0 = se * ke;
  t3_color night = { 0.0004f, 0.0006f, 0.0012f };
  if (cw_node_get_prop(sky, "night_color", &v)) night = (t3_color){ v.v[0], v.v[1], v.v[2] };
  float err = 0;
  for (int f = 0; f < 6; f++) {
    double m[3] = { 0, 0, 0 };
    const float *fp = px + (size_t)f * size * size * 4;
    for (int i = 0; i < size * size; i++)
      for (int c = 0; c < 3; c++) m[c] += fp[4 * i + c];
    for (int c = 0; c < 3; c++) sky_face[f][c] = (float)(m[c] / (size * size));
    if (f == 3) continue; /* -Y: the ground */
    /* above the horizon: the CPU model (cw_sky_radiance) */
    int x = size / 2, y = f == 2 ? size / 2 : size / 4;
    t3_vec3 d = face_dir(f, x, y, size);
    if (d.y < 0.05f) continue;
    t3_color c = cw_sky_radiance(d, ss.sun_direction, ray, mie, g);
    float want[3] = { c.r * e0 + night.r, c.g * e0 + night.g, c.b * e0 + night.b };
    const float *got = fp + 4 * ((size_t)y * size + x);
    for (int k = 0; k < 3; k++) err = fmaxf(err, fabsf(got[k] - want[k]) / fmaxf(want[k], 1e-4f));
  }
  sky_cpu_err = err;
  sky_cpu_ok = err < 0.01f;
  sky_done = 1;
}

static void imp_make(void) {
  enum { SZ = 1024, CELL = 128 };
  uint8_t *alb = cw_alloc(SZ * SZ * 4, CW_MEM_RENDER), *nrm = cw_alloc(SZ * SZ * 4, CW_MEM_RENDER);
  for (int y = 0; y < SZ; y++)
    for (int x = 0; x < SZ; x++) {
      int i = x / CELL, j = y / CELL;
      float dx = (x % CELL + 0.5f) / CELL * 2 - 1, dy = (y % CELL + 0.5f) / CELL * 2 - 1;
      float r2 = dx * dx + dy * dy;
      uint8_t *a = alb + 4 * ((size_t)y * SZ + x), *n = nrm + 4 * ((size_t)y * SZ + x);
      bool in = r2 < 0.64f;
      a[0] = (uint8_t)(70 + 20 * i);
      a[1] = (uint8_t)(150 + 10 * j);
      a[2] = 60;
      a[3] = in ? 255 : 0;
      /* a dome: the view-space normal of a sphere */
      float nz = in ? sqrtf(fmaxf(0, 0.64f - r2)) / 0.8f : 1;
      n[0] = (uint8_t)((dx / 0.8f * 0.5f + 0.5f) * 255);
      n[1] = (uint8_t)((dy / 0.8f * 0.5f + 0.5f) * 255);
      n[2] = (uint8_t)((nz * 0.5f + 0.5f) * 255);
      n[3] = 255;
    }
  imps = cw__vis_impostors_new();
  static const float c[3] = { 0, 0, 0 };
  imp_atlas = cw__vis_impostor_atlas_from_pixels(imps, alb, nrm, 0.6f, c);
  cw_free(alb);
  cw_free(nrm);
}

/* before the effects: the culling pass, impostors, the queries (after the
 * boxes' depth), the restore */
static void vis_frame(cw_pass_ctx *ctx) {
  ctx->scene3d = cw_render3d_scene(tree);
  cw__vis_test_exec(ctx, 0);
  for (int i = 0; i < 4; i++)
    if (!cw_visibility_drawn(node(vis_names[i]))) vis_hidden_mask |= 1 << i;
  if (!imps) imp_make();
  cw__vis_impostors_begin_frame(imps);
  static const vis_impostor_inst inst[3] = { { { -1.0f, 0.6f, -3.0f }, 0.6f, { 0, 0, 0, 1 }, 1 },
                                             { { 1.2f, 0.6f, -3.2f }, 0.6f, { 0, 0.38f, 0, 0.925f }, 1 },
                                             { { 3.0f, 0.6f, -2.5f }, 0.6f, { 0, 0, 0, 1 }, 0.5f } };
  for (int i = 0; i < 3; i++) cw__vis_impostor_add(imps, imp_atlas, &inst[i]);
  imp_draws = cw__vis_impostors_draw(imps, ctx);
  cw__vis_test_exec(ctx, 1);
  cw__vis_test_exec(ctx, 2);
  cw_visibility_stats vs;
  cw_visibility_get_stats(tree, &vs);
  vis_visible = vs.visible;
  vis_occluded = vs.occlusion_culled;
  vis_fculled = vs.frustum_culled;
  vis_meshes = vs.meshes;
  if (vs.queries > 0) vis_queries_any = 1;
}

/* ── GL ───────────────────────────────────────────────────────────────── */

static struct {
  cw_rt *rt, *dc;
  GLuint prog, vbo, vao;
} gl;

static GLuint gl_compile(GLenum t, const char *src) {
  GLuint s = glCreateShader(t);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  return s;
}

static void gl_boxes(const cw_pass_ctx *ctx) {
  if (!gl.prog) {
    const char *vs = "#version 300 es\nlayout(location = 0) in vec3 a_p;\nlayout(location = 1) in vec3 a_n;\n"
                     "uniform mat4 u_vp, u_m;\nuniform vec3 u_col;\nout vec3 v_c;\n"
                     "void main() { vec3 n = normalize(mat3(u_m) * a_n);\n"
                     "  v_c = u_col * (0.35 + 0.65 * max(dot(n, normalize(vec3(0.4, 0.9, 0.3))), 0.0));\n"
                     "  gl_Position = u_vp * (u_m * vec4(a_p, 1.0)); }\n";
    const char *fs = "#version 300 es\nprecision highp float;\nin vec3 v_c;\nout vec4 o;\nvoid main() { o = vec4(v_c, 1.0); }\n";
    gl.prog = glCreateProgram();
    glAttachShader(gl.prog, gl_compile(GL_VERTEX_SHADER, vs));
    glAttachShader(gl.prog, gl_compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(gl.prog);
    glGenBuffers(1, &gl.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW);
    glGenVertexArrays(1, &gl.vao);
    glBindVertexArray(gl.vao);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (const void *)12);
    glBindVertexArray(0);
  }
  glUseProgram(gl.prog);
  glUniformMatrix4fv(glGetUniformLocation(gl.prog, "u_vp"), 1, GL_FALSE, ctx->view_proj.e);
  glBindVertexArray(gl.vao);
  for (int i = 0; i < NBOX; i++) {
    t3_mat4 m;
    box_matrix(&boxes[i], &m);
    glUniformMatrix4fv(glGetUniformLocation(gl.prog, "u_m"), 1, GL_FALSE, m.e);
    glUniform3f(glGetUniformLocation(gl.prog, "u_col"), boxes[i].color.x, boxes[i].color.y, boxes[i].color.z);
    glDrawArrays(GL_TRIANGLES, 0, 36);
  }
  glBindVertexArray(0);
}

static void gl_sky(void) {
  if (sky_done || !cw__sky_test_build(tree)) return;
  int size;
  unsigned cube = cw__sky_cube_gl(tree, &size);
  GLuint fbo;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  float *px = cw_alloc(sizeof(float) * 4 * 6 * (size_t)size * size, CW_MEM_RENDER);
  for (int f = 0; f < 6; f++) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, cube, 0);
    glReadPixels(0, 0, size, size, GL_RGBA, GL_FLOAT, px + (size_t)f * size * size * 4);
  }
  sky_check(px, size);
  cw_free(px);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
}

static void gl_frame(cw_pass_ctx *ctx) {
  gl_sky();
  if (!gl.rt) {
    gl.rt = cw_rt_new(W, H, CW_RT_RGBA8, true);
    gl.dc = cw_rt_new(W, H, CW_RT_R8, true);
  }
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glViewport(0, 0, W, H);
  glBindFramebuffer(GL_FRAMEBUFFER, gl.rt->fbo);
  glClearColor(BG[0], BG[1], BG[2], 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  gl_boxes(ctx);
  ctx->scene = gl.rt;
  vis_frame(ctx);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindFramebuffer(GL_FRAMEBUFFER, gl.dc->fbo);
  glViewport(0, 0, W, H);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  gl_boxes(ctx);
  /* the scene target bound, as the pipeline leaves it (WebGL refuses a draw
   * that samples the depth copy while its framebuffer is bound) */
  glBindFramebuffer(GL_FRAMEBUFFER, gl.rt->fbo);
  ctx->scene = gl.rt;
  ctx->depth_tex = gl.dc->depth_tex;
  cw__sky_test_sun(ctx);
  glBindFramebuffer(GL_FRAMEBUFFER, gl.rt->fbo);
  cw__fx_test_exec(ctx);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glDisable(GL_DEPTH_TEST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, gl.rt->fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  /* the sort's keys, read back at once (GL may wait) */
  unsigned cw__psys_gl_keys(cw_node n, int *sw, int *sh, int *amount);
  int sw, sh, amount;
  unsigned fbo = cw__psys_gl_keys(node("Smoke"), &sw, &sh, &amount);
  if (fbo && !perf) {
    float *px = cw_alloc(sizeof(float) * 4 * (size_t)sw * sh, CW_MEM_RENDER);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, sw, sh, GL_RGBA, GL_FLOAT, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    sort_ok = keys_ok(px, 4, sw * sh, true);
    sorts_checked++;
    cw_free(px);
  }
}

/* ── WebGPU ───────────────────────────────────────────────────────────── */

#if CW_WGPU
WGPUBuffer cw__psys_wgpu_keys(cw_node n, int *count, int *amount);
void cw__fx_wgpu_test_fault(int fault);

static const char WGSL_BOXES[] =
    "struct U { vp: mat4x4f, v: mat4x4f, m: mat4x4f, col: vec4f }\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "struct VO { @builtin(position) pos: vec4f, @location(0) c: vec3f, @location(1) vz: f32 }\n"
    "@vertex fn vs(@location(0) p: vec3f, @location(1) n: vec3f) -> VO {\n"
    "  var o: VO; let w = u.m * vec4f(p, 1.0); let nn = normalize(m3(u.m) * n);\n"
    "  o.c = u.col.rgb * (0.35 + 0.65 * max(dot(nn, normalize(vec3f(0.4, 0.9, 0.3))), 0.0));\n"
    "  o.vz = -(u.v * w).z; o.pos = cw_clip(u.vp * w, -1.0); return o; }\n"
    "struct FO { @location(0) c: vec4f, @location(1) z: f32 }\n"
    "@fragment fn fs(in: VO) -> FO { var o: FO; o.c = vec4f(in.c, 1.0); o.z = in.vz; return o; }\n"
    /* the bottom-up colour target onto the top-down window */
    "@group(0) @binding(0) var src: texture_2d<f32>;\n"
    "@vertex fn vs_blit(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {\n"
    "  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u)); return vec4f(p * 2.0 - 1.0, 0.0, 1.0); }\n"
    "@fragment fn fs_blit(@builtin(position) p: vec4f) -> @location(0) vec4f {\n"
    "  let s = textureDimensions(src);\n"
    "  return textureLoad(src, vec2i(i32(p.x), i32(s.y) - 1 - i32(p.y)), 0); }\n";

static struct {
  bool init;
  WGPUTexture color, depth, lin;
  WGPUTextureView color_v, depth_v, lin_v;
  WGPUBuffer vbuf, ubuf;
  WGPUBindGroupLayout bgl, blit_bgl;
  WGPURenderPipeline boxes, blit;
  cw__wg_readback *keys_rb, *sky_rb;
  bool sky_requested;
} wg;
WGPUTexture cw__sky_cube_wgpu(cw_tree *t, int *size);

static void wg_init(void) {
  wg.init = true;
  WGPUTextureUsage ru = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
  wg.color = cw__wg_texture(WGPUTextureDimension_2D, W, H, 1, 1, WGPUTextureFormat_RGBA8Unorm, ru | WGPUTextureUsage_CopySrc,
                            WGPUTextureViewDimension_Undefined, "fx_gpu colour");
  wg.depth = cw__wg_texture(WGPUTextureDimension_2D, W, H, 1, 1, WGPUTextureFormat_Depth32Float,
                            WGPUTextureUsage_RenderAttachment, WGPUTextureViewDimension_Undefined, "fx_gpu depth");
  wg.lin = cw__wg_texture(WGPUTextureDimension_2D, W, H, 1, 1, WGPUTextureFormat_R32Float, ru,
                          WGPUTextureViewDimension_Undefined, "fx_gpu linear depth");
  wg.color_v = cw__wg_view(wg.color, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  wg.depth_v = cw__wg_view(wg.depth, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  wg.lin_v = cw__wg_view(wg.lin, WGPUTextureViewDimension_2D, 0, 1, 0, 1);
  wg.vbuf = cw__wg_buffer(sizeof cube, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "fx_gpu cube");
  cw__wg_write(wg.vbuf, 0, cube, sizeof cube);
  wg.ubuf = cw__wg_buffer(256 * NBOX, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "fx_gpu boxes");
  const char *const parts[] = { WGSL_BOXES, NULL };
  WGPUShaderModule sm = cw__wg_module("fx_gpu", parts);
  cw__wg_slot sl = { CW_WB_UNIFORM_DYN, WGPUShaderStage_Vertex, 0, 208 };
  wg.bgl = cw__wg_layout(&sl, 1);
  cw__wg_slot bl = { CW_WB_TEX_UNFILT, WGPUShaderStage_Fragment, 0, 0 };
  wg.blit_bgl = cw__wg_layout(&bl, 1);
  WGPUVertexAttribute at[2] = { { .format = WGPUVertexFormat_Float32x3, .offset = 0, .shaderLocation = 0 },
                                { .format = WGPUVertexFormat_Float32x3, .offset = 12, .shaderLocation = 1 } };
  WGPUVertexBufferLayout vb = { .stepMode = WGPUVertexStepMode_Vertex, .arrayStride = 24, .attributeCount = 2, .attributes = at };
  WGPUColorTargetState ct[2] = { { .format = WGPUTextureFormat_RGBA8Unorm, .writeMask = WGPUColorWriteMask_All },
                                 { .format = WGPUTextureFormat_R32Float, .writeMask = WGPUColorWriteMask_All } };
  WGPUFragmentState fs = { .module = sm, .entryPoint = CW_WGPU_SV("fs"), .targetCount = 2, .targets = ct };
  WGPUDepthStencilState ds = { .format = WGPUTextureFormat_Depth32Float, .depthWriteEnabled = WGPUOptionalBool_True,
                               .depthCompare = WGPUCompareFunction_Less,
                               .stencilFront = { .compare = WGPUCompareFunction_Always },
                               .stencilBack = { .compare = WGPUCompareFunction_Always } };
  WGPURenderPipelineDescriptor d = {
    .label = CW_WGPU_SV("fx_gpu boxes"), .layout = cw__wg_pipeline_layout(&wg.bgl, 1),
    .vertex = { .module = sm, .entryPoint = CW_WGPU_SV("vs"), .bufferCount = 1, .buffers = &vb },
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
    .depthStencil = &ds, .multisample = { .count = 1, .mask = 0xffffffffu }, .fragment = &fs,
  };
  wg.boxes = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &d);
  WGPUColorTargetState bt = { .format = cw__wgpu_frame_format(), .writeMask = WGPUColorWriteMask_All };
  WGPUFragmentState bfs = { .module = sm, .entryPoint = CW_WGPU_SV("fs_blit"), .targetCount = 1, .targets = &bt };
  WGPURenderPipelineDescriptor bd = {
    .label = CW_WGPU_SV("fx_gpu blit"), .layout = cw__wg_pipeline_layout(&wg.blit_bgl, 1),
    .vertex = { .module = sm, .entryPoint = CW_WGPU_SV("vs_blit") },
    .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None },
    .multisample = { .count = 1, .mask = 0xffffffffu }, .fragment = &bfs,
  };
  wg.blit = wgpuDeviceCreateRenderPipeline(cw__wgpu_device(), &bd);
  wgpuShaderModuleRelease(sm);
}

static void wg_frame(cw_pass_ctx *ctx) {
  if (!wg.init) {
    cw__wgpu_frame_view(); /* a cart learns its surface format when the surface is configured */
    wg_init();
  }
  /* the sort check of an earlier frame */
  if (wg.keys_rb) {
    uint64_t tag;
    const float *k = cw__wg_readback_latest(wg.keys_rb, &tag);
    static uint32_t seen;
    if (k && cw__wg_readback_count(wg.keys_rb) != seen) {
      seen = cw__wg_readback_count(wg.keys_rb);
      sort_ok = keys_ok(k, 2, (int)tag, false);
      sorts_checked++;
    }
  }
  /* sky + PMREM: built on the first frame, read back asynchronously */
  if (!wg.sky_requested && cw__sky_test_build(tree)) wg.sky_requested = true;
  int ssize = 0;
  WGPUTexture scube = cw__sky_cube_wgpu(tree, &ssize);
  if (wg.sky_rb && !sky_done) {
    const uint16_t *h = cw__wg_readback_latest(wg.sky_rb, NULL);
    if (h) {
      size_t row = (((size_t)ssize * 8) + 255) & ~(size_t)255;
      float *px = cw_alloc(sizeof(float) * 4 * 6 * (size_t)ssize * ssize, CW_MEM_RENDER);
      for (int f = 0; f < 6; f++)
        for (int y = 0; y < ssize; y++)
          for (int x = 0; x < ssize * 4; x++)
            px[((size_t)f * ssize * ssize + (size_t)y * ssize) * 4 + x] =
                cw__wg_half(h[((size_t)f * ssize * row + (size_t)y * row) / 2 + x]);
      sky_check(px, ssize);
      cw_free(px);
    }
  }
  uint8_t u[256 * NBOX];
  memset(u, 0, sizeof u);
  for (int i = 0; i < NBOX; i++) {
    t3_mat4 m;
    box_matrix(&boxes[i], &m);
    float *o = (float *)(u + 256 * i);
    memcpy(o, ctx->view_proj.e, 64);
    memcpy(o + 16, ctx->view.e, 64);
    memcpy(o + 32, m.e, 64);
    o[48] = boxes[i].color.x;
    o[49] = boxes[i].color.y;
    o[50] = boxes[i].color.z;
  }
  cw__wg_write(wg.ubuf, 0, u, sizeof u);
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(cw__wgpu_device(), NULL);
  WGPURenderPassColorAttachment ca[2] = {
    { .view = wg.color_v, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
      .storeOp = WGPUStoreOp_Store, .clearValue = { BG[0], BG[1], BG[2], 1 } },
    { .view = wg.lin_v, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
      .storeOp = WGPUStoreOp_Store, .clearValue = { 3.0e38, 0, 0, 0 } },
  };
  WGPURenderPassDepthStencilAttachment da = { .view = wg.depth_v, .depthLoadOp = WGPULoadOp_Clear,
                                              .depthStoreOp = WGPUStoreOp_Store, .depthClearValue = 1 };
  WGPURenderPassDescriptor rp = { .colorAttachmentCount = 2, .colorAttachments = ca, .depthStencilAttachment = &da };
  WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(p, wg.boxes);
  wgpuRenderPassEncoderSetVertexBuffer(p, 0, wg.vbuf, 0, WGPU_WHOLE_SIZE);
  cw__wg_entry e = { .buffer = wg.ubuf, .size = 208 };
  WGPUBindGroup bg = cw__wg_group(wg.bgl, &e, 1);
  for (int i = 0; i < NBOX; i++) {
    uint32_t off = (uint32_t)(256 * i);
    wgpuRenderPassEncoderSetBindGroup(p, 0, bg, 1, &off);
    wgpuRenderPassEncoderDraw(p, 36, 1, 0, 0);
  }
  wgpuRenderPassEncoderEnd(p);
  wgpuRenderPassEncoderRelease(p);
  wgpuBindGroupRelease(bg);
  cw__wgpu_pass_targets tg = { .enc = enc, .color = wg.color_v, .color_format = WGPUTextureFormat_RGBA8Unorm,
                               .depth = wg.depth_v, .depth_format = WGPUTextureFormat_Depth32Float,
                               .linear_depth = wg.lin_v, .w = W, .h = H, .flip_y = true, .samples = 1 };
  cw__fx_wgpu_set_test_targets(&tg);
  vis_frame(ctx);
  cw__sky_test_sun(ctx);
  cw__fx_test_exec(ctx);
  cw__fx_wgpu_set_test_targets(NULL);
  /* the sort's keys, read back asynchronously */
  int n, amount;
  WGPUBuffer keys = cw__psys_wgpu_keys(node("Smoke"), &n, &amount);
  if (keys && !perf) {
    if (!wg.keys_rb) wg.keys_rb = cw__wg_readback_new((size_t)n * 8, 2, "fx_gpu keys");
    cw__wg_readback_request(wg.keys_rb, enc, keys, 0, (uint64_t)n);
  }
  if (scube && !wg.sky_rb) {
    size_t row = (((size_t)ssize * 8) + 255) & ~(size_t)255;
    wg.sky_rb = cw__wg_readback_new(row * ssize * 6, 1, "fx_gpu sky");
    cw__wg_readback_request_tex(wg.sky_rb, enc, scube, 0, 0, 0, 0, ssize, ssize, 6, 8, 0);
  }
  WGPUTextureView fv = cw__wgpu_frame_view();
  if (fv) {
    WGPURenderPassColorAttachment fa = { .view = fv, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear,
                                         .storeOp = WGPUStoreOp_Store };
    WGPURenderPassDescriptor frp = { .colorAttachmentCount = 1, .colorAttachments = &fa };
    WGPURenderPassEncoder bp = wgpuCommandEncoderBeginRenderPass(enc, &frp);
    cw__wg_entry be = { .view = wg.color_v };
    WGPUBindGroup bbg = cw__wg_group(wg.blit_bgl, &be, 1);
    wgpuRenderPassEncoderSetPipeline(bp, wg.blit);
    wgpuRenderPassEncoderSetBindGroup(bp, 0, bbg, 0, NULL);
    wgpuRenderPassEncoderDraw(bp, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(bp);
    wgpuRenderPassEncoderRelease(bp);
    wgpuBindGroupRelease(bbg);
  }
  WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
  wgpuQueueSubmit(cw__wgpu_queue(), 1, &cb);
  wgpuCommandBufferRelease(cb);
  wgpuCommandEncoderRelease(enc);
}
#endif

static double wall_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ── the app ──────────────────────────────────────────────────────────── */

static void init(void *u) {
  (void)u;
  build_cube();
  const char *f = getenv("CW_FXGPU_FAULT");
  fault = f ? atoi(f) : 0;
#if CW_WGPU
  if (fault) cw__fx_wgpu_test_fault(fault);
#endif
  perf = getenv("CW_FXGPU_PERF") && atoi(getenv("CW_FXGPU_PERF"));
  if (perf) boxes = perf_boxes;
  tree = cw_tree_new();
  world = cw_scene_instance_path(perf ? "fx_gpu_perf.cwscene" : "fx_gpu.cwscene");
  cw_node_add_child(cw_tree_root(tree), world);
  if (getenv("CW_FXGPU_SUN")) { /* a look at the sun disk: the camera up, the sun ahead (native checks by eye) */
    cw_node_set(node("Camera"), "rotation", cw_val_vec3(0.45f, 0, 0));
    cw_node_set(node("Sky"), "latitude", cw_val_float(-35));
    cw_node_set(node("Sky"), "time_of_day", cw_val_float(12));
    cw_node_set(node("Sky"), "sun_size", cw_val_float(4));
  }
  static char keys[NSYS][3][40];
  for (int i = 0; i < NSYS; i++) {
    snprintf(keys[i][0], 40, "fxg.%s.alive", names[i]);
    snprintf(keys[i][1], 40, "fxg.%s.gpu_alive", names[i]);
    snprintf(keys[i][2], 40, "fxg.%s.gpu_sum", names[i]);
    cw_debug_i32(keys[i][0], &alive[i]);
    cw_debug_i32(keys[i][1], &gpu_alive[i]);
    cw_debug_f32_array(keys[i][2], gpu_sum[i], 3);
  }
  cw_debug_i32("fxg.rain_mean_y", &rain_mean_y);
  cw_debug_i32("fxg.rain_on_top", &rain_on_top);
  cw_debug_i32("fxg.sort_ok", &sort_ok);
  cw_debug_i32("fxg.sorts_checked_any", &sorts_checked);
  cw_debug_i32("fxg.steps", &steps);
  cw_debug_i32("fxg.draws", &draws);
  cw_debug_i32("fxg.sort_passes", &sort_passes);
  cw_debug_u32("fxg.tick", &tick);
  cw_debug_f32_array("sky.face_mean", &sky_face[0][0], 18);
  cw_debug_i32("sky.cpu_ok", &sky_cpu_ok);
  cw_debug_f32("sky.cpu_err", &sky_cpu_err);
  cw_debug_i32("sky.done", &sky_done);
  cw_debug_i32("vis.visible", &vis_visible);
  cw_debug_i32("vis.occluded", &vis_occluded);
  cw_debug_i32("vis.frustum", &vis_fculled);
  cw_debug_i32("vis.meshes", &vis_meshes);
  cw_debug_i32("vis.queries_any", &vis_queries_any);
  cw_debug_i32("vis.hidden_mask", &vis_hidden_mask);
  cw_debug_i32("imp.draws", &imp_draws);
  cw_debug_i32("decal.decals", &dec_decals);
  cw_debug_i32("decal.visible", &dec_visible);
  cw_debug_i32("decal.draws", &dec_draws);
  cw_debug_i32("decal.atlas", &dec_atlas);
  if (perf) {
    /* timings describe the machine: host.*, never compared */
    cw_debug_f32("host.fxg.frame_ms", &frame_ms);
    cw_debug_i32("fxg.perf_alive", &perf_alive);
    cw_debug_i32("fxg.perf_frames", &perf_frames);
  }
}

static void fixed(void *u, double dt) {
  (void)u;
  if (tick >= FREEZE && !perf) return;
  tick++;
  float s, c;
  cw_sincos((float)tick * 0.06f, &s, &c);
  cw_node3d_set_position(node("Orbiter"), (t3_vec3){ 2.5f + 0.9f * c, 1.2f + 0.3f * s, 1.5f + 0.9f * s });
  cw_tree_fixed_update(tree, dt);
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  double t0 = wall_s();
  cw_tree_pre_render(tree, 1.0); /* node transforms into the three.c objects */
  cw_pass_ctx ctx;
  make_ctx(&ctx);
#if CW_WGPU
  if (cw_gpu_backend() == CW_GPU_WEBGPU) {
    wg_frame(&ctx);
    if (perf) cw__wgpu_wait_idle();
  } else
#endif
  {
    gl_frame(&ctx);
    if (perf) glFinish();
  }
  if (perf) {
    /* CPU + GPU work of the frame (from the render start to the GPU done),
     * averaged after 60 warm-up frames */
    static int n;
    if (++n > 60) {
      perf_sum += wall_s() - t0;
      perf_frames++;
      frame_ms = (float)(perf_sum / perf_frames * 1000.0);
    }
    cw_particles_stats st;
    cw_particles_get_stats(tree, &st);
    perf_alive = st.alive;
    return;
  }
  for (int i = 0; i < NSYS; i++) {
    cw_node n = node(names[i]);
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
  cw_decal_stats ds;
  cw_decals_get_stats(tree, &ds);
  dec_decals = ds.decals;
  dec_visible = ds.visible;
  dec_draws = ds.draw_calls;
  dec_atlas = ds.atlas_images;
  cw_particles_stats st;
  cw_particles_get_stats(tree, &st);
  steps = st.sim_steps;
  draws = st.draw_calls;
  sort_passes = st.sort_passes;
  if (sorts_checked > 1) sorts_checked = 1; /* the count of checks differs by host latency: "any" */
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  /* render_scale 1: the test draws and reads back at W x H pixels */
  (void)argv;
  return (cw_app_desc){ .title = "fx_gpu", .width = W, .height = H, .render_scale = 1, .debug = true, .init = init, .fixed_update = fixed,
                        .render = render };
}
