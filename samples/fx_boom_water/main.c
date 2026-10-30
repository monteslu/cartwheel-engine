/* fx_boom_water: the explosion and water measurement scene. A pier, a boat
 * and two crates on Gerstner water, a shoreline, a rock, and a looping hero
 * explosion over the water, an Explosion3D of the whole kit (fireball and flames
 * from flipbooks, smoke, sparks, debris with a smoke trail sub-emitter, heat haze,
 * two shockwave rings, a pooled flash light) that fires again every 4.5 seconds. It is the scene the perf gate (tests/boom-perf.mjs) times
 * at every quality preset, with one view and with four (split screen).
 *
 * Environment (a test run has no other way to say these):
 *   BOOM_PRESET=low|medium|high|ultra   the quality preset (default high)
 *   BOOM_VIEWS=1|2|4                    split screen views (default 1)
 *   BOOM_FX=all|nodistort|none          all = everything; nodistort = no haze
 *                                       or ring; none = no explosion at all
 *                                       (the water-only baseline)
 *
 * Debug state: boom.tick, boom.gpu_total (the pipeline's GPU ms of the frame,
 * averaged), boom.pass_ms (the GPU ms average of the passes named in
 * boom.pass_names order: see PASS below), boom.alive (the peak). Timings are reported,
 * never compared. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/explosions.h"
#include "cartwheel/particles.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/quality.h"
#include "cartwheel/scene_file.h"

static const char *const PASS[] = { "scene", "particles", "distort_sources", "distort", "bloom", "tonemap", "ssao", "aa", "height_fog" };
enum { NPASS = sizeof PASS / sizeof PASS[0] };
static float pass_ms[NPASS], gpu_total;
static uint32_t tick;
static int32_t alive;

static cw_node node(const char *path) {
  char p[96];
  snprintf(p, sizeof p, "Root/%s", path);
  return cw_node_get(cw_tree_root(cw_scene_app_tree()), p);
}

static void init(void *u) {
  (void)u;
  cw_debug_u32("boom.tick", &tick);
  cw_debug_f32("boom.gpu_total", &gpu_total);
  cw_debug_f32_array("boom.pass_ms", pass_ms, NPASS);
  cw_debug_i32("boom.alive", &alive);
  const char *p = getenv("BOOM_PRESET");
  if (p) {
    for (int q = 0; q < CW_QUALITY_COUNT; q++)
      if (!strcmp(p, cw_quality_name(q)) || (p[0] && (p[0] | 32) == (cw_quality_name(q)[0] | 32))) cw_quality_set((cw_quality)q, true);
  }
  const char *fx = getenv("BOOM_FX");
  bool none = fx && !strcmp(fx, "none"), nodist = fx && !strcmp(fx, "nodistort");
  if (none) {
    cw_node_set(node("Boom"), "autostart", cw_val_bool(false));
    cw_node_set(node("Boom"), "loop", cw_val_bool(false));
  }
  if (nodist) {
    cw_node_set(node("Boom/Haze"), "distort_strength", cw_val_float(0));
    cw_node_set(node("Boom/Wave"), "strength", cw_val_float(0));
    cw_node_set(node("Boom/WaveAir"), "strength", cw_val_float(0));
  }
  const char *v = getenv("BOOM_VIEWS");
  int nv = v ? atoi(v) : 1;
  if (nv > 1) {
    static const char *const cams[] = { "Camera", "Cam1", "Cam2", "Cam3" };
    cw_node cn[4];
    for (int i = 0; i < 4; i++) cn[i] = node(cams[i]);
    cw_pipeline_split(cw_pipeline_of(cw_scene_app_tree()), cn, nv > 4 ? 4 : nv);
  }
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
}

static void render(void *u, double a) {
  (void)u;
  (void)a;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  const cw_pass_stat *ps;
  int n = cw_pipeline_pass_stats(p, &ps);
  for (int k = 0; k < NPASS; k++) {
    pass_ms[k] = 0;
    for (int i = 0; i < n; i++)
      if (!strcmp(ps[i].name, PASS[k]) && ps[i].gpu_ms_avg > 0) pass_ms[k] += ps[i].gpu_ms_avg;
  }
  gpu_total = cw_pipeline_gpu_ms(p);
  cw_particles_stats st;
  cw_particles_get_stats(cw_scene_app_tree(), &st);
  if (st.alive > alive) alive = st.alive;   /* the peak: a one-shot burst has mostly burnt out by the time a run ends */
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fx_boom_water", .width = 1920, .height = 1080, .render_scale = 1, .debug = true,
                              .init = init, .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
