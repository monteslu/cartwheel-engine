/* fog_shafts: volumetric fog. A dim room filled with thin haze: the sun
 * falls through a cross-barred window (light shafts with the bars' shadows),
 * a spot light draws its cone over a pillar (the pillar's shadow cuts it),
 * and a FogVolume3D of noisy mist lies on the floor at the back. The run is
 * a schedule:
 *   seg 0   frames  1-40  volumetric fog (the HeightFog's volumetric mode)
 *   seg 1   frames 41-60  the same fog analytic (volumetric off: no shafts)
 *   seg 2   frames 61-80  volumetric again, the spot light swung toward the
 *                         window (the grid follows a moving light)
 *
 * FOG_SHAFTS_HOT=1 (environment): the spot light flares absurdly bright (100000) for frames 5 to 7
 * and goes back to its own strength (FOG_SHAFTS_HOT=nan: instead its position is not a number for those frames,
 * one bad frame of input); the fog must look as if it never had (no light that strong may leave the grid
 * overflowed or poisoned for good: the temporal history would carry it forever). The test compares
 * frame 120 with and without the flare.
 *
 * Debug state: fog.seg, vfog.active (the grid was built and applied last
 * frame), vfog.grid_x / _y / _z, vfog.lights, vfog.shadowed, vfog.volumes.
 *
 * fog_shafts_perf (FOG_SHAFTS_PERF): the volumetric segment
 * only, the camera slowly turning; publishes the passes' GPU ms (averaged)
 * as perf.grid_ms, perf.apply_ms, perf.scene_ms (CW_GPU_TIMERS=1; -1 where
 * the host has no timers) for the preset CW_QUALITY picks. fog_shafts_perf_off
 * (FOG_SHAFTS_OFF too): the same with the fog analytic and the mist hidden
 * (no grid at all), the A/B baseline.
 *
 * fog_shafts_split (FOG_SHAFTS_SPLIT): four-way split screen, each view its
 * own camera (the room's camera, one from the window's side, one low by the
 * mist, one looking down from the ceiling): every view gets a grid of its
 * own, sized to its share of the screen (vfog.grid_x is a quarter view's),
 * lit and reprojected for its own camera. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/environment.h"
#include "cartwheel/math.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t tick, seg = -1, active, gx, gy, gz, lights, shadowed, volumes;
static float grid_ms = -1, apply_ms = -1, scene_ms = -1;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }
static int hot; /* 1 a flare, 2 a NaN position */
static float spot_energy;
static t3_vec3 spot_pos;

static void apply(int s) {
#ifdef FOG_SHAFTS_OFF
  s = 1;
  cw_node_set(node("Room/Mist"), "visible", cw_val_bool(false)); /* no volume either: no grid at all */
#endif
  cw_node_set(node("Room/Fog"), "volumetric", cw_val_bool(s != 1));
  if (s == 2) cw_node3d_look_at(node("Room/Spot"), (t3_vec3){ -1, 0, -6 }, (t3_vec3){ 0, 0, -1 });
}

static void init(void *u) {
  (void)u;
  cw_node3d_look_at(node("Room/Sun"), (t3_vec3){ -1, -2, -9 }, (t3_vec3){ 0, 1, 0 });
  cw_node3d_look_at(node("Room/Spot"), (t3_vec3){ 3, 0, -6 }, (t3_vec3){ 0, 0, -1 });
  cw_node3d_look_at(node("Room/Camera"), (t3_vec3){ -4, 2, -5 }, (t3_vec3){ 0, 1, 0 });
#ifdef FOG_SHAFTS_SPLIT
  {
    static const t3_vec3 at[3][2] = { { { -6, 2.5f, 2 }, { 2, 1, -6 } }, { { 3, 0.6f, -2 }, { -3, 0.8f, -8 } },
                                       { { 0, 5.5f, -3 }, { -1, 0, -5 } } };
    cw_node cams[4] = { node("Room/Camera") };
    for (int i = 0; i < 3; i++) {
      cams[i + 1] = cw_node_new("Camera3D", "SplitCam");
      cw_node_add_child(node("Room"), cams[i + 1]);
      cw_node_set(cams[i + 1], "fov", cw_val_float(70));
      cw_node3d_set_position(cams[i + 1], at[i][0]);
      cw_node3d_look_at(cams[i + 1], at[i][1], (t3_vec3){ 0, 1, 0 });
    }
    cw_pipeline_split(cw_pipeline_of(cw_scene_app_tree()), cams, 4);
  }
#endif
  hot = getenv("FOG_SHAFTS_HOT") ? (strcmp(getenv("FOG_SHAFTS_HOT"), "nan") == 0 ? 2 : 1) : 0;
  spot_pos = cw_node3d_position(node("Room/Spot"));
  cw_value sv;
  spot_energy = cw_node_get_prop(node("Room/Spot"), "energy", &sv) ? (float)sv.f : 60.0f;
  cw_debug_i32("fog.seg", &seg);
  cw_debug_i32("vfog.active", &active);
  cw_debug_i32("vfog.grid_x", &gx);
  cw_debug_i32("vfog.grid_y", &gy);
  cw_debug_i32("vfog.grid_z", &gz);
  cw_debug_i32("vfog.lights", &lights);
  cw_debug_i32("vfog.shadowed", &shadowed);
  cw_debug_i32("vfog.volumes", &volumes);
#ifdef FOG_SHAFTS_PERF
  cw_debug_f32("perf.grid_ms", &grid_ms);
  cw_debug_f32("perf.apply_ms", &apply_ms);
  cw_debug_f32("perf.scene_ms", &scene_ms);
#endif
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  bool flare = tick >= 5 && tick <= 7;
  if (hot == 1) cw_node_set(node("Room/Spot"), "energy", cw_val_float(flare ? 100000.0f : spot_energy));
  if (hot == 2) cw_node3d_set_position(node("Room/Spot"), flare ? (t3_vec3){ NAN, spot_pos.y, spot_pos.z } : spot_pos);
#ifdef FOG_SHAFTS_PERF
  int s = 0;
  float a = (float)tick * 0.002f;
  cw_node3d_look_at(node("Room/Camera"), (t3_vec3){ -4 + 3 * cw_sin(a), 2, -5 }, (t3_vec3){ 0, 1, 0 });
#else
  int s = tick <= 40 ? 0 : tick <= 60 ? 1 : 2;
#endif
  if (s != seg) {
    seg = s;
    apply(s);
  }
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_volumetric_fog_state st;
  cw_volumetric_fog_get_state(cw_scene_app_tree(), &st);
  active = st.active, gx = st.grid[0], gy = st.grid[1], gz = st.grid[2];
  lights = st.lights, shadowed = st.shadowed_lights, volumes = st.volumes;
  const cw_pass_stat *ps;
  int n = cw_pipeline_pass_stats(cw_pipeline_of(cw_scene_app_tree()), &ps);
  for (int i = 0; i < n; i++) {
    if (!strcmp(ps[i].name, "volumetric_fog.grid")) grid_ms = ps[i].gpu_ms_avg;
    if (!strcmp(ps[i].name, "volumetric_fog")) apply_ms = ps[i].gpu_ms_avg;
    if (!strcmp(ps[i].name, "scene")) scene_ms = ps[i].gpu_ms_avg;
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "fog_shafts", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
