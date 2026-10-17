/* gi_shadow: a reflection probe that hides an object from its capture must not
 * take the object's shadow with it. A crate casts a shadow from a shadowed sun
 * onto a floor; the probe (captured every second frame) has cull_mask 2, which
 * the crate (layer 1 only) does not share, so every capture draws the scene
 * without the crate. The frame is the same on a capture frame and on the one
 * after (tests/gi-shadow.mjs compares them).
 *
 * Debug state: gi.captures (probe captures so far). */
#include "cartwheel/debug.h"
#include "cartwheel/gi.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t captures;
static bool layered;

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("gi.captures", &captures);
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  if (!layered) {
    /* the floor is seen by the probe's mask (2) and the camera's (1); the crate only by the camera's */
    t3_object *fl = cw_node3d_object(node("Scene/Floor"));
    if (fl) {
      t3_object_set_layers(fl, 1 | 2);
      layered = true;
    }
  }
  cw_gi_stats s;
  cw_gi_get_stats(cw_scene_app_tree(), &s);
  captures = s.captures_total;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "gi_shadow", .width = 320, .height = 240, .debug = true, .init = init, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
