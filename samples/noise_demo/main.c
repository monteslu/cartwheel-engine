/* noise_demo: cw_noise on the CPU and its GPU ports, side by side.
 *
 * Sixteen panels of 64 x 64 texels, one per noise setting (every pattern,
 * 2D and 3D, every fractal, the cellular distance and return modes), in two
 * bands. Each panel has three rows:
 *   top     the GPU port (cw_noise_glsl / cw_noise_wgsl), computed per pixel
 *   middle  the CPU (cw_noise_2d / _3d), filled every frame, uploaded as an
 *           R8 texture (cw_pass_texture_new)
 *   bottom  how far they differ, in 8-bit levels: black 0, then red 64 per
 *           level up to 4 levels (tests/noise-gpu.mjs reads it)
 * The noise scrolls a little every frame (2D) or moves through depth (3D),
 * so frame N differs from N - 1.
 *
 * Debug state: noise.hash (the CPU bytes, identical on every host),
 * noise.tick, and noise.cpu_ms / noise.gpu_ms (reported, never compared). */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cartwheel/debug.h"
#include "cartwheel/gpu.h"
#include "cartwheel/noise.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static double wall_s(void) { /* a real clock (timings are reported, never compared) */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define P 64           /* panel size, texels */
#define COLS 8
#define BANDS 2
#define CW_W (P * COLS) /* 512 */
#define CW_H (P * 3 * BANDS) /* 384 */

typedef struct cfg { cw_noise n; int dims; } cfg;
static cfg cfgs[COLS * BANDS];
static uint8_t cpu_px[P * BANDS * CW_W];
static unsigned cpu_tex, prog;
static uint32_t tick, hash32;
static float cpu_ms, gpu_ms;
static int bench = -1; /* NOISE_DEMO_BENCH=k: setting k over every output pixel (timing) */
static bool break_gpu; /* NOISE_DEMO_BREAK=1 (natively): tests/noise-gpu.mjs's control */

static cw_noise mk(cw_noise_type t, cw_noise_fractal f, int oct, float freq) {
  cw_noise n = cw_noise_default();
  n.type = t;
  n.fractal = f;
  n.octaves = oct;
  n.frequency = freq;
  return n;
}

static void setup(void) {
  int k = 0;
#define ADD(n_, d_) (cfgs[k].n = (n_), cfgs[k].dims = (d_), k++)
  ADD(mk(CW_NOISE_OPENSIMPLEX2, CW_NOISE_FRACTAL_NONE, 1, 0.06f), 2);
  ADD(mk(CW_NOISE_OPENSIMPLEX2, CW_NOISE_FRACTAL_FBM, 5, 0.03f), 2);
  ADD(mk(CW_NOISE_OPENSIMPLEX2, CW_NOISE_FRACTAL_RIDGED, 4, 0.03f), 3);
  ADD(mk(CW_NOISE_OPENSIMPLEX2S, CW_NOISE_FRACTAL_FBM, 4, 0.04f), 2);
  cfgs[k - 1].n.weighted_strength = 0.7f;
  ADD(mk(CW_NOISE_OPENSIMPLEX2S, CW_NOISE_FRACTAL_NONE, 1, 0.05f), 3);
  ADD(mk(CW_NOISE_CELLULAR, CW_NOISE_FRACTAL_NONE, 1, 0.08f), 2);
  ADD(mk(CW_NOISE_CELLULAR, CW_NOISE_FRACTAL_NONE, 1, 0.07f), 2);
  cfgs[k - 1].n.cellular_distance = CW_NOISE_DIST_EUCLIDEAN;
  cfgs[k - 1].n.cellular_return = CW_NOISE_CELL_DISTANCE2_SUB;
  ADD(mk(CW_NOISE_CELLULAR, CW_NOISE_FRACTAL_NONE, 1, 0.07f), 2);
  cfgs[k - 1].n.cellular_distance = CW_NOISE_DIST_MANHATTAN;
  cfgs[k - 1].n.cellular_return = CW_NOISE_CELL_VALUE;
  /* band 2 */
  ADD(mk(CW_NOISE_CELLULAR, CW_NOISE_FRACTAL_NONE, 1, 0.07f), 3);
  cfgs[k - 1].n.cellular_distance = CW_NOISE_DIST_HYBRID;
  cfgs[k - 1].n.cellular_return = CW_NOISE_CELL_DISTANCE2_DIV;
  ADD(mk(CW_NOISE_PERLIN, CW_NOISE_FRACTAL_RIDGED, 5, 0.03f), 2);
  ADD(mk(CW_NOISE_PERLIN, CW_NOISE_FRACTAL_PINGPONG, 3, 0.04f), 3);
  cfgs[k - 1].n.rotation = CW_NOISE_ROTATION_XZ;
  ADD(mk(CW_NOISE_VALUE_CUBIC, CW_NOISE_FRACTAL_FBM, 3, 0.06f), 2);
  ADD(mk(CW_NOISE_VALUE_CUBIC, CW_NOISE_FRACTAL_NONE, 1, 0.08f), 3);
  cfgs[k - 1].n.rotation = CW_NOISE_ROTATION_XY;
  ADD(mk(CW_NOISE_VALUE, CW_NOISE_FRACTAL_PINGPONG, 3, 0.05f), 2);
  ADD(mk(CW_NOISE_VALUE, CW_NOISE_FRACTAL_FBM, 4, 0.06f), 3);
  ADD(mk(CW_NOISE_CELLULAR, CW_NOISE_FRACTAL_FBM, 2, 0.06f), 3);
  cfgs[k - 1].n.cellular_distance = CW_NOISE_DIST_EUCLIDEAN;
  cfgs[k - 1].n.cellular_return = CW_NOISE_CELL_DISTANCE2_ADD;
  cfgs[k - 1].n.cellular_jitter = 0.6f;
#undef ADD
  for (int i = 0; i < k; i++) cfgs[i].n.seed = 100 + i * 37;
}

/* where panel k samples this frame: (x0, y0[, z]) for texel (0, 0) */
static void base(int k, float out[3]) {
  out[0] = (float)(k * 11 - 40) + (float)tick * 0.37f;
  out[1] = (float)(20 - k * 7);
  out[2] = (float)k + (float)tick * 0.21f;
}

static uint8_t to8(float v) {
  float g = v * 0.5f + 0.5f;
  g = g < 0 ? 0 : g > 1 ? 1 : g;
  return (uint8_t)(int)(g * 255.0f + 0.5f);
}

/* #region cpu */
/* the CPU half: every panel's texels into cpu_px (row band * 64 + ty) */
static void fill_cpu(void) {
  double t0 = wall_s();
  for (int k = 0; k < COLS * BANDS; k++) {
    int col = k % COLS, band = k / COLS;
    float b[3];
    base(k, b);
    for (int ty = 0; ty < P; ty++)
      for (int tx = 0; tx < P; tx++) {
        float x = (float)tx + b[0], y = (float)ty + b[1];
        float v = cfgs[k].dims == 3 ? cw_noise_3d(&cfgs[k].n, x, y, b[2]) : cw_noise_2d(&cfgs[k].n, x, y);
        cpu_px[(band * P + ty) * CW_W + col * P + tx] = to8(v);
      }
  }
  cpu_ms = (float)((wall_s() - t0) * 1000);
  hash32 = cw_hash32(cpu_px, sizeof cpu_px, CW_HASH32_INIT);
}
/* #endregion */

/* ── the GPU half ─────────────────────────────────────────────────────── */

static const char *GLSL_MAIN =
    "uniform vec4 u_n0;\n"
    "uniform vec4 u_n1;\n"
    "uniform vec4 u_n2;\n"
    "uniform vec4 u_n3;\n"
    "uniform vec4 u_base;\n" /* x0, y0, z, dims */
    "uniform vec4 u_rect;\n" /* col, band, app pixels per output pixel, app height */
    "uniform sampler2D u_cpu;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 a = floor(gl_FragCoord.xy * u_rect.z);\n"
    "  if (u_rect.x < 0.0) {\n" /* NOISE_DEMO_BENCH: one setting over every pixel */
    "    CwNoise nb = cw_noise_unpack(u_n0, u_n1, u_n2, u_n3);\n"
    "    float vb = u_base.w > 2.5 ? cw_noise3(vec3(gl_FragCoord.xy, u_base.z), nb) : cw_noise2(gl_FragCoord.xy, nb);\n"
    "    o = vec4(vec3(vb * 0.5 + 0.5), 1.0);\n"
    "    return;\n"
    "  }\n"
  "  float ax = a.x, ay = u_rect.w - 1.0 - a.y;\n"
    "  float col = floor(ax / 64.0), band = floor(ay / 192.0);\n"
    "  if (col != u_rect.x || band != u_rect.y) discard;\n"
    "  float ly = ay - band * 192.0, r = floor(ly / 64.0), ty = ly - r * 64.0, tx = ax - col * 64.0;\n"
    "  CwNoise n = cw_noise_unpack(u_n0, u_n1, u_n2, u_n3);\n"
    "  float v = u_base.w > 2.5 ? cw_noise3(vec3(tx + u_base.x, ty + u_base.y, u_base.z), n)\n"
    "                           : cw_noise2(vec2(tx + u_base.x, ty + u_base.y), n);\n"
    "  float g = floor(clamp(v * 0.5 + 0.5, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  float c = floor(texelFetch(u_cpu, ivec2(int(col * 64.0 + tx), int(band * 64.0 + ty)), 0).r * 255.0 + 0.5);\n"
    "  if (r < 0.5) o = vec4(vec3(g / 255.0), 1.0);\n"
    "  else if (r < 1.5) o = vec4(vec3(c / 255.0), 1.0);\n"
    "  else o = vec4(min(abs(g - c), 4.0) * 64.0 / 255.0, 0.0, 0.0, 1.0);\n"
    "}\n";

static const char *WGSL_MAIN =
    "struct U { u_n0: vec4f, u_n1: vec4f, u_n2: vec4f, u_n3: vec4f, u_base: vec4f, u_rect: vec4f };\n"
    "@group(0) @binding(0) var<uniform> u: U;\n"
    "@group(0) @binding(1) var u_cpu: texture_2d<f32>;\n"
    "@fragment fn fs(@builtin(position) pos: vec4f, @location(0) uv: vec2f) -> @location(0) vec4f {\n"
    "  let a = floor(cw_frag_coord(pos) * u.u_rect.z);\n"
    "  if (u.u_rect.x < 0.0) {\n"
    "    let nb = cw_noise_unpack(u.u_n0, u.u_n1, u.u_n2, u.u_n3);\n"
    "    let fc = cw_frag_coord(pos);\n"
    "    var vb: f32;\n"
    "    if (u.u_base.w > 2.5) { vb = cw_noise3(vec3f(fc, u.u_base.z), nb); } else { vb = cw_noise2(fc, nb); }\n"
    "    return vec4f(vec3f(vb * 0.5 + 0.5), 1.0);\n"
    "  }\n"
    "  let ax = a.x;\n"
    "  let ay = u.u_rect.w - 1.0 - a.y;\n"
    "  let col = floor(ax / 64.0);\n"
    "  let band = floor(ay / 192.0);\n"
    "  if (col != u.u_rect.x || band != u.u_rect.y) { discard; }\n"
    "  let ly = ay - band * 192.0;\n"
    "  let r = floor(ly / 64.0);\n"
    "  let ty = ly - r * 64.0;\n"
    "  let tx = ax - col * 64.0;\n"
    "  let n = cw_noise_unpack(u.u_n0, u.u_n1, u.u_n2, u.u_n3);\n"
    "  var v: f32;\n"
    "  if (u.u_base.w > 2.5) { v = cw_noise3(vec3f(tx + u.u_base.x, ty + u.u_base.y, u.u_base.z), n); }\n"
    "  else { v = cw_noise2(vec2f(tx + u.u_base.x, ty + u.u_base.y), n); }\n"
    "  let g = floor(clamp(v * 0.5 + 0.5, 0.0, 1.0) * 255.0 + 0.5);\n"
    "  let c = floor(textureLoad(u_cpu, vec2i(i32(col * 64.0 + tx), i32(band * 64.0 + ty)), 0).r * 255.0 + 0.5);\n"
    "  if (r < 0.5) { return vec4f(vec3f(g / 255.0), 1.0); }\n"
    "  if (r < 1.5) { return vec4f(vec3f(c / 255.0), 1.0); }\n"
    "  return vec4f(min(abs(g - c), 4.0) * 64.0 / 255.0, 0.0, 0.0, 1.0);\n"
    "}\n";

/* #region gpu */
static unsigned make_program(void) {
  bool wg = cw_gpu_backend() == CW_GPU_WEBGPU;
  const char *lib = wg ? cw_noise_wgsl() : cw_noise_glsl();
  const char *main_src = wg ? WGSL_MAIN : GLSL_MAIN;
  size_t n = strlen(lib) + strlen(main_src) + 2;
  char *src = cw_alloc(n, CW_MEM_GAME);
  snprintf(src, n, "%s\n%s", lib, main_src);
  unsigned p = wg ? cw_pass_program_wgsl("noise_demo", src) : cw_pass_program("noise_demo", src);
  cw_free(src);
  return p;
}
/* #endregion */

static void noise_exec(cw_pass_ctx *ctx, void *user) {
  (void)user;
  if (!prog) prog = make_program();
  if (!prog) return;
  /* this frame's CPU texels (the previous texture goes: one upload a frame) */
  cw_pass_texture_free(cpu_tex);
  cpu_tex = cw_pass_texture_new(CW_W, P * BANDS, CW_RT_R8, cpu_px);
  cw_pass_use(prog);
  cw_pass_bind_texture(0, cpu_tex);
  cw_pass_uniform1i(prog, "u_cpu", 0);
  /* one draw a panel: each keeps its own pixels (the rest discard) */
  for (int k = bench >= 0 ? bench : 0; k < (bench >= 0 ? bench + 1 : COLS * BANDS); k++) {
    float g[16], b[3];
    cw_noise_gpu_params(&cfgs[k].n, g);
    if (break_gpu) g[0] += 1; /* the gate control: the GPU draws another seed */
    base(k, b);
    cw_pass_uniform4f(prog, "u_n0", g[0], g[1], g[2], g[3]);
    cw_pass_uniform4f(prog, "u_n1", g[4], g[5], g[6], g[7]);
    cw_pass_uniform4f(prog, "u_n2", g[8], g[9], g[10], g[11]);
    cw_pass_uniform4f(prog, "u_n3", g[12], g[13], g[14], g[15]);
    cw_pass_uniform4f(prog, "u_base", b[0], b[1], b[2], (float)cfgs[k].dims);
    cw_pass_uniform4f(prog, "u_rect", bench >= 0 ? -1.0f : (float)(k % COLS), (float)(k / COLS), (float)CW_W / (float)ctx->width, (float)CW_H);
    cw_pass_fullscreen();
  }
}

static void init(void *u) {
  (void)u;
  break_gpu = getenv("NOISE_DEMO_BREAK") != NULL;
  if (getenv("NOISE_DEMO_BENCH")) bench = atoi(getenv("NOISE_DEMO_BENCH")) % (COLS * BANDS);
  setup();
  cw_debug_u32("noise.tick", &tick);
  cw_debug_u32("noise.hash", &hash32);
  cw_debug_f32("noise.cpu_ms", &cpu_ms);
  cw_debug_f32("noise.gpu_ms", &gpu_ms);
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  if (!cw_pipeline_has_pass(p, "noise_demo"))
    cw_pipeline_add_pass(p, &(cw_pass_desc){ .name = "noise_demo", .phase = CW_PHASE_OVERLAY, .order = 100,
                                             .writes = CW_RES_OUTPUT, .execute = noise_exec, .webgpu = true });
  fill_cpu();
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(cw_pipeline_of(cw_scene_app_tree()), &st);
  for (int i = 0; i < n; i++)
    if (!strcmp(st[i].name, "noise_demo")) gpu_ms = st[i].gpu_ms_avg;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base_desc = {
    .title = "noise_demo", .width = CW_W, .height = CW_H, .debug = true, .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base_desc);
}
