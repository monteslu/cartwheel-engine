/* water_open_sea: a sea much larger than its detail. The Water3D's grid is VIEW: rings around the camera out to the
 * horizon, dense under it. A wave mask holds the swell down in a sheltered bay (the west half of the map) while the chop
 * carries on, and a small, sharp ripple grid follows a boat as it crosses from the open sea into the bay
 * (cw_water_ripple_follow).
 *
 * Debug state (identical on every host): water.tick, boat.x, rip.hash, rip.center (the grid's centre, x and z), swell.a
 * (the swell's height over the open sea and in the bay, from cw_water_height_at), plus the probes the water-surface gate
 * reads: probe.uv, probe.h (the CPU's height), probe.h0 (the height with the mask ignored: a surface that drew no mask
 * would match this instead). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/water.h"

#define PX 8
#define PZ 6
#define NPROBE (PX * PZ)
#define MASK_N 64

static uint32_t tick, rip_hash;
static float boat_x = 120, rip_center[2], swell[2];
static float probe_uv[NPROBE * 2], probe_h[NPROBE], probe_h0[NPROBE];
static cw_node water, boat, camera;

static void init(void *u) {
  (void)u;
  cw_node root = cw_node_get(cw_tree_root(cw_scene_app_tree()), "Root");
  water = cw_node_get(root, "Water"), boat = cw_node_get(root, "Boat"), camera = cw_node_get(root, "Camera");
  /* #region mask */
  /* open sea to the east, a bay to the west: a smooth step across x */
  static float mask[MASK_N * MASK_N];
  for (int j = 0; j < MASK_N; j++)
    for (int i = 0; i < MASK_N; i++) {
      float x = ((float)i + 0.5f) / MASK_N * 600 - 300;
      float t = (x + 60) / 120;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      mask[j * MASK_N + i] = 0.1f + 0.9f * t * t * (3 - 2 * t);
    }
  cw_water_set_wave_mask(water, mask, MASK_N, MASK_N);
  if (getenv("WATER_OPEN_SEA_X")) boat_x = (float)atof(getenv("WATER_OPEN_SEA_X"));
  if (getenv("WATER_OPEN_SEA_HEIGHT")) { /* the height as colour, as the gate does through debug commands */
    cw_node_set(water, "debug_view", cw_val_enum("HEIGHT"));
    cw_node_set(cw_node_get(root, "Post"), "tonemap", cw_val_enum("NONE"));
  }
  /* #endregion */
  cw_debug_u32("water.tick", &tick);
  cw_debug_f32("boat.x", &boat_x);
  cw_debug_u32("rip.hash", &rip_hash);
  cw_debug_f32_array("rip.center", rip_center, 2);
  cw_debug_f32_array("swell.a", swell, 2);
  cw_debug_f32_array("probe.uv", probe_uv, NPROBE * 2);
  cw_debug_f32_array("probe.h", probe_h, NPROBE);
  cw_debug_f32_array("probe.h0", probe_h0, NPROBE);
}

static void fixed(void *u, double dt) {
  (void)u;
  tick++;
  float v = 12;
  boat_x -= v * (float)dt;
  if (boat_x < -160) boat_x = 120;
  /* #region follow */
  if (!getenv("WATER_OPEN_SEA_OFF")) cw_water_ripple_follow(water, boat_x, 0);
  cw_water_wake(water, boat_x, 0, -v, 0, 1.1f, 0.05f, 0.9f);
  /* #endregion */
  rip_hash = cw_water_ripple_hash(water);
  cw_value c;
  if (cw_node_get_prop(water, "ripple_center", &c)) rip_center[0] = c.v[0], rip_center[1] = c.v[1];
  double t = cw_water_time(water);
  float hi = 0, lo = 0;
  for (int k = 0; k < 16; k++) {
    float he = fabsf(cw_water_height_at(water, 240, (float)k * 9, t)), hw = fabsf(cw_water_height_at(water, -240, (float)k * 9, t));
    if (he > hi) hi = he;
    if (hw > lo) lo = hw;
  }
  swell[0] = hi, swell[1] = lo;
}

static void render(void *u, double alpha) {
  (void)u;
  cw_tree *t = cw_scene_app_tree();
  t3_camera *cam = cw_camera3d_t3(camera);
  double dt = cw_fixed_dt();
  float bx = boat_x + 12 * (float)((1 - alpha) * dt);
  cw_node3d_set_position(boat, (t3_vec3){ bx, cw_water_height_at(water, bx, 0, cw_water_time(water)) + 0.1f, 0 });
  cw_node3d_set_position(camera, (t3_vec3){ bx + 2, 25, 11 });
  /* the underwater gate: a low camera in the bay, where the mask holds the swell far under what it is at sea */
  { const char *e = getenv("WATER_OPEN_SEA_LOW");
    if (e) {
      cw_node3d_set_position(camera, (t3_vec3){ -240, (float)atof(e), 0 });
      cw_node3d_look_at(camera, (t3_vec3){ -200, (float)atof(e), 0 }, (t3_vec3){ 0, 1, 0 });
    } }
  if (!cam) return;
  double time = ((double)cw_tree_tick(t) - 1.0 + alpha) * dt;
  if (time < 0) time = 0;
  cw_waves w;
  cw_water_get_waves(water, &w);
  t3_mat4 vp;
  t3_mat4_multiply(&vp, &cam->projection_matrix, &cam->matrix_world_inverse);
  for (int j = 0; j < PZ; j++)
    for (int i = 0; i < PX; i++) {
      int k = j * PX + i;
      float x = bx - 10.0f + 20.0f * (float)i / (PX - 1) + 0.37f * (float)j, z = -14.0f + 3.0f * (float)j;
      float h = cw_water_height_at(water, x, z, time);
      t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ x, h, z }, &vp);
      probe_h[k] = h;
      probe_h0[k] = cw_waves_height(&w, x, z, time);
      probe_uv[k * 2] = p.x * 0.5f + 0.5f;
      probe_uv[k * 2 + 1] = 0.5f - p.y * 0.5f;
    }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc, (void)argv;
  static cw_app_desc base = { .title = "water_open_sea", .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
