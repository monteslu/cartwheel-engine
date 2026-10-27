/* water_shore: Water3D meeting a sloping beach and a flat sea floor: the baked
 * floor depth map gives the water its shallow-to-deep colour, foam where it
 * meets the shore and on the crests (cartwheel/water.h). A boat and two
 * crates float on the waves. Debug state: tick, bodies.hash, floor.depth
 * (cw_water_floor_depth_at at four fixed points: out in the open,
 * mid slope, near the shore, on land). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/physics_nodes.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/water.h"

static uint32_t tick, bodies_hash;
static float floor_depth[4];
static float frame_ms; /* the pipeline's GPU ms per frame (0 where the host cannot time it) */

static cw_node node(const char *path) {
  char p[96];
  snprintf(p, sizeof p, "Root/%s", path);
  return cw_node_get(cw_tree_root(cw_scene_app_tree()), p);
}

static void init(void *u) {
  (void)u;
  cw_debug_u32("water.tick", &tick);
  cw_debug_u32("bodies.hash", &bodies_hash);
  cw_debug_f32_array("floor.depth", floor_depth, 4);
  cw_debug_f32("water.gpu_ms", &frame_ms);
}

/* WATER_SHORE_OFF=1: the shore and foam features off (the cost's baseline) */
static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  static bool off_done;
  if (!off_done && getenv("WATER_SHORE_OFF")) {
    cw_node w = node("Water");
    cw_node_set(w, "floor_map_size", cw_val_int(0));
    cw_node_set(w, "shore_foam", cw_val_float(0));
    cw_node_set(w, "crest_foam", cw_val_float(0));
    off_done = true;
  }
  frame_ms = cw_pipeline_gpu_ms(cw_pipeline_of(cw_scene_app_tree()));
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
  cw_node water = node("Water");
  static const float zs[4] = { 16, 4, -10, -20 };
  for (int i = 0; i < 4; i++) {
    float d = cw_water_floor_depth_at(water, 0, zs[i]);
    floor_depth[i] = d > 1000 ? 1000 : d;
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = {
    .title = "water_shore", .width = 960, .height = 540, .debug = true, .init = init, .update = update, .render = render,
  };
  return cw_scene_app("main.cwscene", &base);
}
