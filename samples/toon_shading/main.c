/* toon_shading: toon (cel) shaded meshes beside physically based ones
 * (cartwheel/toon.h, MeshInstance3D `shading = TOON`).
 *
 * Front row, left to right: a sphere with three.js's two soft-edged tones
 * (toon_bands 0), one with a ramp of three tones, one with five, and a dark
 * cylinder with an emissive glow (the glow ignores the light). Behind them a
 * physically based sphere of the second one's colour, for comparison. Two
 * trees from one glTF are toon shaded by the same property (every material
 * of the file, shared between the two instances), a third stays as the file
 * has it. The ground is toon too. Everything is lit by a cascaded sun with
 * shadows, a clustered orange omni light and a clustered blue spot light:
 * each light is banded on its own.
 *
 * At tick 60 the five-tone sphere changes to eight tones and back at tick
 * 120 (a property like any other).
 *
 * TOON_OFF=1 sets every mesh back to STANDARD at tick 2 (the gate's control:
 * the state and the picture differ).
 *
 * Debug fields: toon.materials (distinct toon materials drawn), toon.meshes
 * (meshes drawing one), toon.shared (1 when the two toon trees draw with the
 * same materials), toon.tick. */
#include <string.h>

#include "cartwheel/debug.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/toon.h"

static int32_t materials, meshes, shared, tick32;
static bool off;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

typedef struct { t3_material *seen[64]; int n, meshes; } count_ctx;
static void count_visit(t3_object *o, void *u) {
  count_ctx *c = u;
  if (o->type != T3_MESH && o->type != T3_SKINNED_MESH) return;
  t3_mesh *m = (t3_mesh *)o;
  bool any = false;
  for (int k = 0; k < m->material_count; k++) {
    t3_material *mt = m->materials[k];
    if (!mt || mt->type != T3_MESH_TOON_MATERIAL) continue;
    any = true;
    bool dup = false;
    for (int j = 0; j < c->n; j++) dup |= c->seen[j] == mt;
    if (!dup && c->n < 64) c->seen[c->n++] = mt;
  }
  c->meshes += any;
}

static void first_material(t3_object *o, void *u) {
  t3_material **out = u;
  if (*out || (o->type != T3_MESH && o->type != T3_SKINNED_MESH)) return;
  t3_mesh *m = (t3_mesh *)o;
  if (m->material_count) *out = m->materials[0];
}

static void set_all(cw_node n, const char *shading) {
  if (cw_node_is(n, "MeshInstance3D")) cw_node_set(n, "shading", cw_val_enum(shading));
  for (int i = 0; i < cw_node_child_count(n); i++) set_all(cw_node_child(n, i), shading);
}

static void init(void *u) {
  (void)u;
  cw_debug_i32("toon.materials", &materials);
  cw_debug_i32("toon.meshes", &meshes);
  cw_debug_i32("toon.shared", &shared);
  cw_debug_i32("toon.tick", &tick32);
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick32++;
  if (tick32 == 2) { /* the gate's switch, from the first frame every host can set it (cw_test_env, cartwheel/debug.h) */
    off = cw_test_env("TOON_OFF") != NULL;
    if (off) set_all(node("World"), "STANDARD");
  }
  if (!off && tick32 == 60) cw_node_set(node("World/FiveTone"), "toon_bands", cw_val_int(8));
  if (!off && tick32 == 120) cw_node_set(node("World/FiveTone"), "toon_bands", cw_val_int(5));
  count_ctx c = { { 0 }, 0, 0 };
  t3_object_traverse(cw_node3d_object(node("World")), count_visit, &c);
  materials = c.n;
  meshes = c.meshes;
  t3_material *a = NULL, *b = NULL;
  t3_object_traverse(cw_node3d_object(node("World/TreeToon")), first_material, &a);
  t3_object_traverse(cw_node3d_object(node("World/TreeToon2")), first_material, &b);
  shared = a && a == b && a->type == T3_MESH_TOON_MATERIAL;
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "toon_shading", .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
