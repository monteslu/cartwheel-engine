/* water_ripples: splashes, wakes and rain on Water3D's interactive ripple
 * layer (cartwheel/water.h). Two crates are dropped from above (each makes
 * a splash and, through the Splash pool, a burst of spray), a boat circles
 * in the middle (its Buoyancy3D leaves a wake and foam trail), one splash is
 * made by hand with cw_water_splash, and rain falls when WATER_RAIN is set.
 *
 * Environment (the tests' controls and variants):
 *   WATER_RIPPLES_OFF=1   no ripple grid (ripple_cells 0): the baseline
 *   WATER_RAIN=<drops/s>  rain over the ripple area
 *   WATER_DEBUG=<n>       the water's debug_view (2: the refraction depth as colour)
 *
 * Debug state: tick, rip.hash (cw_water_ripple_hash, the same bits on every
 * host), rip.h (the ripple height, mm, at three fixed points), boat.pos
 * (x, z, mm), bodies.hash, water.gpu_ms. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/water.h"

static uint32_t tick, rip_hash, bodies_hash;
static float rip_h[3], boat_pos[2], frame_ms;

static cw_node node(const char *path) {
  char p[96];
  snprintf(p, sizeof p, "Root/%s", path);
  return cw_node_get(cw_tree_root(cw_scene_app_tree()), p);
}

static void init(void *u) {
  (void)u;
  cw_debug_u32("tick", &tick);
  cw_debug_u32("rip.hash", &rip_hash);
  cw_debug_u32("bodies.hash", &bodies_hash);
  cw_debug_f32_array("rip.h", rip_h, 3);
  cw_debug_f32_array("boat.pos", boat_pos, 2);
  cw_debug_f32("water.gpu_ms", &frame_ms);
  cw_node w = node("Water");
  if (getenv("WATER_RIPPLES_OFF")) cw_node_set(w, "ripple_cells", cw_val_int(0));
  if (getenv("WATER_DEBUG")) cw_node_set(w, "debug_view", cw_val_int(atoi(getenv("WATER_DEBUG"))));
  if (getenv("WATER_RAIN")) cw_node_set(w, "rain", cw_val_float((float)atof(getenv("WATER_RAIN"))));
}

static void update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  cw_node water = node("Water");
  /* the boat is driven round a circle (the water does the rest) */
  cw_body3d boat = cw_collision_object3d_body(node("Boat"));
  if (cw_body3d_valid(boat)) {
    cw_xform3 x = cw_body3d_transform(boat);
    float a = cw_atan2(x.p.z + 2.0f, x.p.x);
    t3_vec3 v = cw_body3d_velocity(boat);
    const float speed = 4.5f;
    cw_body3d_set_velocity(boat, (t3_vec3){ -cw_sin(a) * speed, v.y, cw_cos(a) * speed });
    boat_pos[0] = x.p.x, boat_pos[1] = x.p.z;
  }
  /* a splash by hand, then the crates are let go by gravity alone */
  if (tick == 90) cw_water_splash(water, 6, 2, 1.4f, 0.45f, 1.0f);
  uint32_t h = CW_HASH32_INIT;
  static const char *const bodies[] = { "Boat", "Crate1", "Crate2" };
  for (int i = 0; i < 3; i++) {
    cw_body3d b = cw_collision_object3d_body(node(bodies[i]));
    if (!cw_body3d_valid(b)) continue;
    cw_xform3 x = cw_body3d_transform(b);
    h = cw_hash32(&x, sizeof x, h);
  }
  bodies_hash = h;
  rip_hash = cw_water_ripple_hash(water);
  static const float px[3] = { -4, 6, 0 }, pz[3] = { -3, 2, -2 };
  for (int i = 0; i < 3; i++) rip_h[i] = cw_water_ripple_at(water, px[i], pz[i]) * 1000.0f;
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  frame_ms = cw_pipeline_gpu_ms(cw_pipeline_of(cw_scene_app_tree()));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = {
    .title = "water_ripples", .width = 960, .height = 540, .debug = true, .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base);
}
