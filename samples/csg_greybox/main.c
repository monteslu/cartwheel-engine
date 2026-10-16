/* csg_greybox: a level blocked out with CSG shapes (cartwheel/csg.h). A
 * room is a box with a smaller box subtracted (open at the top so the camera
 * sees in), a door and a round window are subtracted from its walls, a
 * pillar is a unioned cylinder, and an ornament is a box intersected with a
 * sphere. The cwcsg tool baked them into level.cwcsg (the root's
 * bake_file); the game only loads that file. A ball drops into the room and
 * lands on the baked floor through a TRIMESH collision shape that reads the
 * baked mesh. On the right, "Sketch" has no bake file: it draws the preview
 * (the shapes as they are, the subtracted cylinder as a red ghost).
 *
 * Debug state: csg.tick, csg.baked, csg.stale, csg.triangles, csg.file_lo /
 * csg.file_hi (FNV-1a 64 of the bake file's bytes), csg.mesh_lo / csg.mesh_hi
 * (of the geometry as loaded), csg.sketch_baked, ball.y. */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/scene_file.h"

static int32_t tick, baked, stale, triangles, sketch_baked;
static uint32_t file_lo, file_hi, mesh_lo, mesh_hi;
static float ball_y;

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("csg.tick", &tick);
  cw_debug_i32("csg.baked", &baked);
  cw_debug_i32("csg.stale", &stale);
  cw_debug_i32("csg.triangles", &triangles);
  cw_debug_u32("csg.file_lo", &file_lo);
  cw_debug_u32("csg.file_hi", &file_hi);
  cw_debug_u32("csg.mesh_lo", &mesh_lo);
  cw_debug_u32("csg.mesh_hi", &mesh_hi);
  cw_debug_i32("csg.sketch_baked", &sketch_baked);
  cw_debug_f32("ball.y", &ball_y);
}

static void hex64(cw_node n, const char *prop, uint32_t *lo, uint32_t *hi) {
  cw_value v;
  if (cw_node_get_prop(n, prop, &v) && v.s && v.s[0]) {
    unsigned long long h = strtoull(v.s, NULL, 16);
    *lo = (uint32_t)h;
    *hi = (uint32_t)(h >> 32);
  }
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  ball_y = cw_node3d_global_position(node("World/Ball")).y;
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  cw_node level = node("World/Level");
  cw_value v;
  if (cw_node_get_prop(level, "baked", &v)) baked = v.b;
  if (cw_node_get_prop(level, "stale", &v)) stale = v.b;
  if (cw_node_get_prop(level, "triangles", &v)) triangles = (int32_t)v.i;
  hex64(level, "file_hash", &file_lo, &file_hi);
  hex64(level, "mesh_hash", &mesh_lo, &mesh_hi);
  if (cw_node_get_prop(node("World/Sketch"), "baked", &v)) sketch_baked = v.b;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "csg_greybox", .width = 640, .height = 360, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
