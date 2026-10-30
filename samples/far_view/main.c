/* far_view: a reversed depth buffer (cw_app_desc.reversed_depth) at the distance it is for.
 * Two glowing walls a metre apart 100 km out, the near one green, the far one red and
 * higher up, with a far plane of 200 km. The far wall draws last (render_order), so wherever
 * depth rounds the two together it paints over the near one: forward depth shows red bands,
 * a reversed depth buffer on WebGPU shows only the far wall's top edge above solid green.
 * FAR_VIEW_FORWARD=1 is the must-fail control (tests/reversed-depth.mjs); a GL run has
 * forward depth, as a GL cart has no way not to. */
#include <stdlib.h>

#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static void last(t3_object *o) {
  if (!o) return;
  o->render_order = 1;
  for (int i = 0; i < o->child_count; i++) last(o->children[i]);
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  last(cw_node3d_object(cw_node_get(cw_tree_root(cw_scene_app_tree()), "FarWall")));
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "far_view", .width = 640, .height = 360, .debug = true, .render = render };
  base.reversed_depth = !getenv("FAR_VIEW_FORWARD");
  return cw_scene_app("main.cwscene", &base);
}
