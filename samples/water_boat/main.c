/* water_boat: a boat and two crates bobbing on Gerstner waves beside a
 * pier, in the afternoon sun. Water3D draws the surface (planar reflection
 * of the pier and the sky, refraction of the posts and the seabed, scrolling
 * ripples); each floating body has a Buoyancy3D child that pushes it up
 * where its corners are under the water (cartwheel/water.h).
 *
 * Debug state (identical on every host): water.tick, bodies.hash (every
 * body's transform bits, each tick), boat.y / boat.tilt (x 1000, rounded),
 * boat.submerged, plus the surface probes the water-surface gate reads:
 * probe.uv (2 per probe: where it lands in the frame, 0..1 from the top
 * left) and probe.h (the CPU's height there, cw_water_height_at at the
 * drawn time). With the Water3D's debug_view HEIGHT the frame shows the
 * GPU's height at every pixel. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/water.h"

#define PX 8
#define PZ 6
#define NPROBE (PX * PZ)

static uint32_t tick, bodies_hash;
static int32_t boat_y, boat_tilt;
static float boat_sub;
static float probe_uv[NPROBE * 2], probe_h[NPROBE];
static float water_ms[3]; /* the water pass's GPU ms, reported */

static cw_node node(const char *path) {
  char p[96];
  snprintf(p, sizeof p, "Root/%s", path);
  return cw_node_get(cw_tree_root(cw_scene_app_tree()), p);
}

static void init(void *u) {
  (void)u;
  cw_debug_u32("water.tick", &tick);
  cw_debug_u32("bodies.hash", &bodies_hash);
  cw_debug_i32("boat.y", &boat_y);
  cw_debug_i32("boat.tilt", &boat_tilt);
  cw_debug_f32("boat.submerged", &boat_sub);
  cw_debug_f32_array("probe.uv", probe_uv, NPROBE * 2);
  cw_debug_f32_array("probe.h", probe_h, NPROBE);
  cw_debug_f32_array("water.gpu_ms", water_ms, 3);
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  static const char *const bodies[] = { "Boat", "Crate1", "Crate2" };
  uint32_t h = CW_HASH32_INIT;
  for (int i = 0; i < 3; i++) {
    cw_body3d b = cw_collision_object3d_body(node(bodies[i]));
    if (!cw_body3d_valid(b)) continue;
    cw_xform3 x = cw_body3d_transform(b);
    h = cw_hash32(&x, sizeof x, h);
  }
  bodies_hash = h;
  cw_body3d boat = cw_collision_object3d_body(node("Boat"));
  if (cw_body3d_valid(boat)) {
    cw_xform3 x = cw_body3d_transform(boat);
    boat_y = (int32_t)lroundf(x.p.y * 1000);
    /* tilt: how far the boat's up is from the world's (1 - up.y), x 1000 */
    float upy = 1 - 2 * (x.q.x * x.q.x + x.q.z * x.q.z);
    boat_tilt = (int32_t)lroundf((1 - upy) * 1000);
  }
  cw_value v;
  if (cw_node_get_prop(node("Boat/Float"), "submerged", &v)) boat_sub = (float)v.f;
}

/* after the 3D draw: where the probes land in the frame and the CPU's
 * height there, at the time the water was drawn */
static void render(void *u, double alpha) {
  (void)u;
  cw_tree *t = cw_scene_app_tree();
  cw_node water = node("Water"), camn = node("Camera");
  t3_camera *cam = cw_camera3d_t3(camn);
  if (!cw_node_valid(water) || !cam) return;
  double time = ((double)cw_tree_tick(t) - 1.0 + alpha) * cw_fixed_dt();
  if (time < 0) time = 0;
  t3_mat4 vp;
  t3_mat4_multiply(&vp, &cam->projection_matrix, &cam->matrix_world_inverse);
  for (int j = 0; j < PZ; j++)
    for (int i = 0; i < PX; i++) {
      int k = j * PX + i;
      /* a grid over the open water in front of the camera, clear of the boat and the pier */
      float x = -10.0f + 20.0f * (float)i / (PX - 1) + 0.37f * (float)j;
      float z = -14.0f + 3.0f * (float)j;
      float h = cw_water_height_at(water, x, z, time);
      t3_vec3 p = t3_vec3_apply_mat4((t3_vec3){ x, h, z }, &vp);
      probe_h[k] = h;
      probe_uv[k * 2] = p.x * 0.5f + 0.5f;
      probe_uv[k * 2 + 1] = 0.5f - p.y * 0.5f;
    }
  const cw_pass_stat *st;
  int n = cw_pipeline_pass_stats(cw_pipeline_of(t), &st);
  for (int i = 0; i < n; i++) {
    if (!strcmp(st[i].name, "water")) water_ms[0] = st[i].gpu_ms_avg;
    if (!strcmp(st[i].name, "scene")) water_ms[1] = st[i].gpu_ms_avg;
  }
  water_ms[2] = cw_pipeline_gpu_ms(cw_pipeline_of(t));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = {
    .title = "water_boat", .width = 960, .height = 540, .debug = true, .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base);
}
