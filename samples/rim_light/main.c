/* rim_light: a coloured glow along silhouettes that fades in with distance
 * (cartwheel/rim_light.h).
 *
 * Two lines of figures (a capsule body and a sphere head each) walk away from
 * the camera down a dim street at 3, 8, 15, 25 and 40 m: the left line has a
 * red rim, the right line a blue one, the middle figure a white rim that
 * pulses (a "protected" shimmer: its strength set every tick). Close to the
 * camera the rim is faint (`close` 0.15 of it, below `near` 6 m), by 20 m it
 * is at full strength, so the far figures read as red or blue outlines
 * against the dark, where without the rim they are grey shapes.
 *
 * MeshInstance3D primitives with the same look share one material in a tree,
 * so each line has its own grey (a hair apart) and so its own materials.
 *
 * RIM_OFF=1 gives no figure a rim (the gate's control: the state and the
 * picture differ). RIM_REMOVE=1 adds the rims and removes them again at
 * tick 30 (remove works: the count goes back to 0).
 *
 * Debug fields: rim.materials (cw_rim_light_count), rim.tick, rim.pulse
 * (the pulsing figure's strength now), rim.remove_ok (apply, get and remove
 * on a spare material leave the count and its references as they were). */
#include <stdlib.h>

#include "cartwheel/debug.h"
#include "cartwheel/math.h"
#include "cartwheel/platform.h"
#include "cartwheel/render3d.h"
#include "cartwheel/rim_light.h"
#include "cartwheel/scene_file.h"

#define ROWS 5
static int32_t materials, tick32, remove_ok;
static float pulse;
static bool off, remove_late;
static cw_node figs[2][ROWS], shimmer;

static cw_node node(const char *path) { return cw_node_get(cw_tree_root(cw_scene_app_tree()), path); }

static cw_node figure(cw_node parent, float x, float z, t3_color c) {
  cw_node f = cw_node_new("Node3D", "Figure");
  cw_node3d_set_position(f, (t3_vec3){ x, 0, z });
  cw_node body = cw_node_new("MeshInstance3D", "Body");
  cw_node_set(body, "primitive", cw_val_enum("CYLINDER"));
  cw_node_set(body, "size", cw_val_vec3(0.5f, 1.3f, 0.5f));
  cw_node_set(body, "color", cw_val_color(c.r, c.g, c.b, 1));
  cw_node_set(body, "roughness", cw_val_float(0.8f));
  cw_node_set(body, "cast_shadow", cw_val_bool(true));
  cw_node3d_set_position(body, (t3_vec3){ 0, 0.75f, 0 });
  cw_node_add_child(f, body);
  cw_node head = cw_node_new("MeshInstance3D", "Head");
  cw_node_set(head, "primitive", cw_val_enum("SPHERE"));
  cw_node_set(head, "size", cw_val_vec3(0.36f, 0.36f, 0.36f));
  cw_node_set(head, "color", cw_val_color(c.r, c.g, c.b, 1));
  cw_node_set(head, "roughness", cw_val_float(0.6f));
  cw_node_set(head, "cast_shadow", cw_val_bool(true));
  cw_node3d_set_position(head, (t3_vec3){ 0, 1.6f, 0 });
  cw_node_add_child(f, head);
  cw_node_add_child(parent, f);
  return f;
}

static void rim_on(cw_node fig, const cw_rim_light_desc *d) {
  for (int k = 0; k < 2; k++) {
    t3_object *o = cw_node3d_object(cw_node_get(fig, k ? "Head" : "Body"));
    if (o) cw_rim_light_apply_tree(o, d);
  }
}
static void rim_off(cw_node fig) {
  for (int k = 0; k < 2; k++) {
    t3_object *o = cw_node3d_object(cw_node_get(fig, k ? "Head" : "Body"));
    if (o) cw_rim_light_remove_tree(o);
  }
}

static void init(void *u) {
  (void)u;
  off = getenv("RIM_OFF") != NULL;
  remove_late = getenv("RIM_REMOVE") != NULL;
  cw_debug_i32("rim.materials", &materials);
  cw_debug_i32("rim.tick", &tick32);
  cw_debug_f32("rim.pulse", &pulse);
  cw_debug_i32("rim.remove_ok", &remove_ok);
  { /* apply, read back and remove on a material of our own: the count goes up by one and back, and the material is let go */
    t3_material *m = t3_mesh_standard_material_new(0xffffff);
    int before = cw_rim_light_count();
    cw_rim_light_desc got;
    bool on = cw_rim_light_apply(m, NULL) && cw_rim_light_count() == before + 1 && cw_rim_light_get(m, &got) && got.power == 3.0f;
    cw_rim_light_remove(m);
    remove_ok = on && cw_rim_light_count() == before && !cw_rim_light_get(m, NULL) && m->_refs == 1;
    t3_release(m);
  }
  cw_node world = node("World");
  static const float Z[ROWS] = { -3, -8, -15, -25, -40 };
  for (int r = 0; r < ROWS; r++)
    for (int s = 0; s < 2; s++) figs[s][r] = figure(world, s ? 1.6f : -1.6f, Z[r], (t3_color){ 0.32f, 0.33f + 0.004f * (float)s, 0.35f });
  shimmer = figure(world, 0, -11, (t3_color){ 0.32f, 0.342f, 0.35f });
}

static void fixed(void *u, double dt) {
  (void)u;
  (void)dt;
  tick32++;
  if (off) return;
  if (tick32 == 2) { /* the meshes exist once the tree has run: give them their rims */
    cw_rim_light_desc red = cw_rim_light_default(), blue = red;
    blue.color = (t3_color){ 0.25f, 0.55f, 1.0f };
    for (int r = 0; r < ROWS; r++) rim_on(figs[0][r], &red), rim_on(figs[1][r], &blue);
  }
  if (tick32 >= 2 && !(remove_late && tick32 >= 30)) {
    cw_rim_light_desc w = cw_rim_light_default();
    w.color = (t3_color){ 0.6f, 0.85f, 1.0f };
    w.close = 1.0f, w.power = 1.6f;
    pulse = w.strength = 1.2f + 2.4f * (0.5f + 0.5f * cw_sin((float)tick32 * 0.45f)); /* applied again each tick: a shimmer */
    rim_on(shimmer, &w);
  }
  if (remove_late && tick32 == 30) {
    for (int r = 0; r < ROWS; r++) rim_off(figs[0][r]), rim_off(figs[1][r]);
    rim_off(shimmer);
  }
  materials = cw_rim_light_count();
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "rim_light", .width = 960, .height = 540, .debug = true, .init = init, .fixed_update = fixed };
  return cw_scene_app("main.cwscene", &base);
}
