/* gi_room: reflections and bounce light (cartwheel/gi.h). A room open at the
 * front: a red left wall, a green right wall, a polished dark floor running
 * out onto a porch, a chrome ball in the middle and a gold one on the porch,
 * a glowing cube circling the plinth. RoomProbe (box projected to the room's
 * walls, captured every other frame) makes the chrome and the floor reflect
 * the room as it is, the circling cube included; PorchProbe (captured once,
 * not box projected) covers the porch, and the two blend across the opening.
 * Bounce, a 5 x 3 x 5 LightProbeGrid3D baked at load with one bounce, lights
 * the room with what its walls throw back: the ceiling turns red by the red
 * wall and green by the green one. The PostProcess turns on screen-space
 * reflections (the floor mirrors the balls and the plinth standing on it) and
 * screen-space bounce light.
 *
 * GI_PROBES=0 turns both probes' intensity to 0 and GI_GRID=0 the grid's
 * (the gates' controls); GI_NONE=1 removes the probes and the grid and turns SSR
 * and SSGI off (the timing report's baseline).
 *
 * Debug state: gi.tick, gi.probes, gi.captures (since the start), gi.bound
 * (the most probes one draw had bound last frame), gi.materials (materials
 * carrying the gi shading part); gi.ms_capture (GPU ms of the
 * "gi" pass: probe captures, grid updates and the G-buffer), gi.ms_ssr and
 * gi.ms_ssgi (those passes), and gi.ms_frame
 * (every pass), 16-frame averages, reported and never compared (natively,
 * with the timers on: CW_GPU_TIMERS=1). gi_room_perf is the same room at
 * 1280x720 for the timing report (tests/gi-perf.mjs). */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/math.h"

#include "cartwheel/debug.h"
#include "cartwheel/gi.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t tick, probes, captures, bound, materials;
static float ms_capture = -1, ms_ssr = -1, ms_ssgi = -1, ms_frame = -1;

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("gi.tick", &tick);
  cw_debug_i32("gi.probes", &probes);
  cw_debug_i32("gi.captures", &captures);
  cw_debug_i32("gi.bound", &bound);
  cw_debug_i32("gi.materials", &materials);
  cw_debug_f32("gi.ms_capture", &ms_capture);
  cw_debug_f32("gi.ms_ssr", &ms_ssr);
  cw_debug_f32("gi.ms_ssgi", &ms_ssgi);
  cw_debug_f32("gi.ms_frame", &ms_frame);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  if (tick == 1) {
    const char *e = getenv("GI_PROBES");
    if (e && !strcmp(e, "0")) {
      cw_node_set(node("Room/RoomProbe"), "intensity", cw_val_float(0));
      cw_node_set(node("Room/PorchProbe"), "intensity", cw_val_float(0));
    }
    e = getenv("GI_NONE");
    if (e && !strcmp(e, "1")) {
      cw_node_free(node("Room/RoomProbe"));
      cw_node_free(node("Room/PorchProbe"));
      cw_node_free(node("Room/Bounce"));
      cw_node_set(node("Room/Post"), "ssr", cw_val_bool(false));
      cw_node_set(node("Room/Post"), "ssgi", cw_val_bool(false));
    }
    e = getenv("GI_GRID");
    if (e && !strcmp(e, "0")) cw_node_set(node("Room/Bounce"), "intensity", cw_val_float(0));
  }
  /* render-only motion: the angle comes from the tick count (cw_ trig) */
  float a = (float)tick * 0.03f;
  cw_node3d_set_position(node("Room/Spinner"), (t3_vec3){ -2 + 1.6f * cw_cos(a), 0.5f, -2 + 1.6f * cw_sin(a) });
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_gi_stats s;
  cw_gi_get_stats(cw_scene_app_tree(), &s);
  probes = s.reflection_probes;
  captures = s.captures_total;
  bound = s.probes_bound_max;
  materials = s.materials;
  cw_pipeline *p = cw_pipeline_of(cw_scene_app_tree());
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(p, &st);
  float total = 0;
  bool timed = false;
  for (int i = 0; i < n; i++) {
    if (!strcmp(st[i].name, "gi")) ms_capture = st[i].gpu_ms_avg;
    if (!strcmp(st[i].name, "ssr")) ms_ssr = st[i].ran ? st[i].gpu_ms_avg : 0;
    if (!strcmp(st[i].name, "ssgi")) ms_ssgi = st[i].ran ? st[i].gpu_ms_avg : 0;
    if (st[i].ran && st[i].gpu_ms_avg >= 0) total += st[i].gpu_ms_avg, timed = true;
  }
  ms_frame = timed ? total : -1;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
#ifdef GI_PERF
  static cw_app_desc base = { .title = "gi_room_perf", .width = 1280, .height = 720, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
#else
  static cw_app_desc base = { .title = "gi_room", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
#endif
  return cw_scene_app("main.cwscene", &base);
}
