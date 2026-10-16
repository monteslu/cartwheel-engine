/* gi_probe_check: a reflection probe against known answers. A closed box
 * whose six walls glow in six colours (-x red, +x green, -z blue, +z yellow,
 * +y white, -y cyan), a probe in the middle sized to the box with box
 * projection, a mirror ball at the probe and a small mirror plate off to the
 * side. The walls are on layer 2 and the probe sees only layer 2 (cull_mask),
 * so the ball and plate are not in their own capture.
 *
 * A mirror reflects what a straight line from its surface reaches: the
 * ball's left edge the red wall, its right edge the green one, its top the
 * white ceiling, its bottom the cyan floor, its middle the yellow wall behind
 * the camera; the plate (seen from above its plane) the white ceiling, with
 * box projection; tests/gi-probe.mjs reads those pixels. GI_BOX=0 turns box
 * projection off (the plate then reads the capture point's view, which the
 * test can tell apart only where the walls differ) and GI_PROBE=0 the probe's
 * intensity (the must-fail control: every reflection is black).
 *
 * The tree runs clustered lighting with one lamp, so its materials draw with
 * the gi part composed after the cluster part (xcluster_gi,
 * cartwheel/shading.h).
 *
 * Debug state: gi.captures, gi.bound. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/gi.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t captures, bound;
static bool set;

static void layers(t3_object *o, uint32_t m) {
  if (!o) return;
  t3_object_set_layers(o, m);
  for (int i = 0; i < o->child_count; i++) layers(o->children[i], m);
}

static cw_node node(const char *p) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), p); }

static void init(void *u) {
  (void)u;
  cw_debug_i32("gi.captures", &captures);
  cw_debug_i32("gi.bound", &bound);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  if (set) return;
  set = true;
  const char *e = getenv("GI_BOX");
  if (e && !strcmp(e, "0")) cw_node_set(node("Box/Probe"), "box_projection", cw_val_bool(false));
  e = getenv("GI_PROBE");
  if (e && !strcmp(e, "0")) cw_node_set(node("Box/Probe"), "intensity", cw_val_float(0));
}

static void render(void *u, double alpha) {
  (void)u;
  (void)alpha;
  /* the walls on layer 2 (their meshes exist once they have drawn), then a
   * capture that sees them */
  static bool walls_set;
  if (!walls_set) {
    walls_set = true;
    static const char *walls[] = { "Box/NegX", "Box/PosX", "Box/NegZ", "Box/PosZ", "Box/PosY", "Box/NegY" };
    for (int i = 0; i < 6; i++) layers(cw_node3d_object(node(walls[i])), 1 | 2);
    cw_reflection_probe_update(node("Box/Probe"));
  }
  cw_gi_stats s;
  cw_gi_get_stats(cw_scene_app_tree(), &s);
  captures = s.captures_total;
  bound = s.probes_bound_max;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "gi_probe_check", .width = 320, .height = 240, .debug = true, .init = init,
                              .fixed_update = fixed_update, .render = render };
  return cw_scene_app("main.cwscene", &base);
}
