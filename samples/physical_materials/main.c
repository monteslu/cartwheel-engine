/* physical_materials: three.js r186's MeshPhysicalMaterial lobes on
 * MeshInstance3D under a ProceduralSky (the background and the image-based
 * light). Left to right: plain red paint (Standard), the same paint with a
 * clearcoat (car paint), velvet (sheen), a beetle shell (iridescence) and
 * brushed metal (anisotropy). The camera slowly orbits so the lobes' view
 * dependence shows.
 *
 * Debug state: mat.tick, mat.physical (how many of the five draw with a
 * MeshPhysicalMaterial: 4). */
#include <math.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"

static int32_t tick, physical;

static void init(void *u) {
  (void)u;
  cw_debug_i32("mat.tick", &tick);
  cw_debug_i32("mat.physical", &physical);
}

static void fixed_update(void *u, double dt) {
  (void)u;
  (void)dt;
  tick++;
  cw_node root = cw_tree_root(cw_scene_app_tree());
  cw_node cam = cw_node_get(root, "Stage/Camera");
  float a = (float)tick * 0.004f;
  cw_node3d_snapshot(cam);
  cw_node3d_set_position(cam, (t3_vec3){ 9 * sinf(a), 2.6f, 9 * cosf(a) });
  cw_node3d_set_rotation(cam, (t3_vec3){ -0.16f, a, 0 });
  static const char *const names[] = { "Plain", "CarPaint", "Velvet", "Beetle", "Brushed" };
  physical = 0;
  for (int i = 0; i < 5; i++) {
    t3_object *o = cw_node3d_object(cw_node_get(root, cw_frame_printf("Stage/%s", names[i])));
    for (int c = 0; o && c < o->child_count; c++) {
      t3_object *k = o->children[c];
      if (k->type == T3_MESH && ((t3_mesh *)k)->material_count &&
          ((t3_mesh *)k)->materials[0]->type == T3_MESH_PHYSICAL_MATERIAL)
        physical++;
    }
  }
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "physical_materials", .width = 960, .height = 540, .debug = true, .init = init,
                              .fixed_update = fixed_update };
  return cw_scene_app("main.cwscene", &base);
}
